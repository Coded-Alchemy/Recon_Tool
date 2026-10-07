/*
   ThingPulse Color Kit Grande
   ESP32 Wi-Fi Analyzer

   DISPLAY
   ILI9488
   Resolution: 320 x 480 PORTRAIT

   SPI:
     SCLK = GPIO 5
     MOSI = GPIO 18
     MISO = GPIO 19
     CS   = GPIO 15
     DC   = GPIO 2
     RST  = GPIO 4
     BL   = GPIO 32

   TOUCH
   FT6236
   SDA = GPIO 23
   SCL = GPIO 22
   INT = GPIO 27

   Arduino IDE
   Board: ESP32 Wrover Module
   Library: LovyanGFX
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LovyanGFX.hpp>

// ============================================================
// HARDWARE
// ============================================================

#define LCD_SCLK   5
#define LCD_MOSI   18
#define LCD_MISO   19
#define LCD_CS     15
#define LCD_DC     2
#define LCD_RST    4
#define LCD_BL     32

#define TOUCH_SDA  23
#define TOUCH_SCL  22
#define TOUCH_INT  27

#define TOUCH_ADDR 0x38

// ============================================================
// DISPLAY
// ============================================================

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI _bus;

public:

    LGFX()
    {
        // SPI bus
        {
            auto cfg = _bus.config();

            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;

            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;

            cfg.pin_sclk = LCD_SCLK;
            cfg.pin_mosi = LCD_MOSI;
            cfg.pin_miso = LCD_MISO;
            cfg.pin_dc   = LCD_DC;

            _bus.config(cfg);

            _panel.setBus(&_bus);
        }

        // Display
        {
            auto cfg = _panel.config();

            cfg.pin_cs  = LCD_CS;
            cfg.pin_rst = LCD_RST;
            cfg.pin_busy = -1;

            cfg.memory_width  = 320;
            cfg.memory_height = 480;

            cfg.panel_width  = 320;
            cfg.panel_height = 480;

            cfg.offset_x = 0;
            cfg.offset_y = 0;

            cfg.offset_rotation = 0;

            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;

            cfg.readable = true;

            cfg.invert = false;

            // Color Kit Grande uses BGR ordering
            cfg.rgb_order = false;

            cfg.dlen_16bit = false;
            cfg.bus_shared = true;

            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

LGFX lcd;

// ============================================================
// COLORS
// ============================================================

#define BLACK       0x0000
#define WHITE       0xFFFF
#define RED         0xF800
#define GREEN       0x07E0
#define BLUE        0x001F
#define CYAN        0x07FF
#define YELLOW      0xFFE0
#define ORANGE      0xFD20
#define GRAY        0x8410
#define DARKGRAY    0x4208
#define LIGHTGRAY   0xC618

// ============================================================
// WIFI
// ============================================================

#define MAX_NETWORKS 50

struct NetworkInfo
{
    String ssid;
    String bssid;

    int32_t rssi;
    int32_t channel;

    wifi_auth_mode_t encryption;

    bool hidden;
};

NetworkInfo networks[MAX_NETWORKS];

int networkCount = 0;

int currentPage = 0;

const int NETWORKS_PER_PAGE = 5;

// ============================================================
// TOUCH
// ============================================================

bool readTouch(uint16_t &x, uint16_t &y)
{
    /*
       FT6236 register 0x02:

       Byte 0:
         Number of touches

       Bytes 1-2:
         X coordinate

       Bytes 3-4:
         Y coordinate
    */

    Wire.beginTransmission(TOUCH_ADDR);

    Wire.write(0x02);

    if (Wire.endTransmission(false) != 0)
    {
        return false;
    }

    Wire.requestFrom(TOUCH_ADDR, 5);

    if (Wire.available() < 5)
    {
        return false;
    }

    uint8_t touches = Wire.read();

    uint8_t xHigh = Wire.read();
    uint8_t xLow  = Wire.read();

    uint8_t yHigh = Wire.read();
    uint8_t yLow  = Wire.read();

    if (touches == 0)
    {
        return false;
    }

    x = ((xHigh & 0x0F) << 8) | xLow;
    y = ((yHigh & 0x0F) << 8) | yLow;

    /*
       The Color Kit Grande's touchscreen is physically
       oriented differently from the LCD.

       For the portrait orientation we use:
       
           screen X = raw Y
           screen Y = 319 - raw X

       If your particular panel revision behaves differently,
       the TOUCH_TRANSFORM settings below can be changed.
    */

    // Color Kit Grande portrait orientation.
    // FT6236 coordinates map directly to the display.
    if (x >= 320)
        x = 319;

    if (y >= 480)
        y = 479;

    return true;
}

// ============================================================
// SECURITY NAME
// ============================================================

String encryptionName(wifi_auth_mode_t auth)
{
    switch (auth)
    {
        case WIFI_AUTH_OPEN:
            return "OPEN";

        case WIFI_AUTH_WEP:
            return "WEP";

        case WIFI_AUTH_WPA_PSK:
            return "WPA";

        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";

        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";

        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "WPA2-ENT";

        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";

        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/3";

        default:
            return "UNKNOWN";
    }
}

// ============================================================
// SIGNAL
// ============================================================

int signalPercent(int rssi)
{
    int value = 2 * (rssi + 100);

    if (value < 0)
        value = 0;

    if (value > 100)
        value = 100;

    return value;
}

// ============================================================
// SIGNAL BARS
// ============================================================

void drawSignalBars(
    int x,
    int y,
    int rssi
)
{
    int percent = signalPercent(rssi);

    for (int i = 0; i < 4; i++)
    {
        int height = 5 + i * 5;

        uint16_t color;

        if (percent >= ((i + 1) * 25))
            color = GREEN;
        else
            color = DARKGRAY;

        lcd.fillRect(
            x + i * 7,
            y + (20 - height),
            5,
            height,
            color
        );
    }
}

// ============================================================
// BUTTON
// ============================================================

void drawButton(
    int x,
    int y,
    int width,
    int height,
    const char *label,
    uint16_t color
)
{
    lcd.fillRoundRect(
        x,
        y,
        width,
        height,
        8,
        color
    );

    lcd.drawRoundRect(
        x,
        y,
        width,
        height,
        8,
        WHITE
    );

    lcd.setTextColor(WHITE);

    lcd.setTextSize(2);

    int textWidth = strlen(label) * 12;

    lcd.setCursor(
        x + (width - textWidth) / 2,
        y + (height - 16) / 2
    );

    lcd.print(label);
}

// ============================================================
// HEADER
// ============================================================

void drawHeader()
{
    lcd.fillRect(
        0,
        0,
        320,
        45,
        BLUE
    );

    lcd.setTextColor(WHITE);

    lcd.setTextSize(2);

    lcd.setCursor(10, 13);

    lcd.print("WIFI ANALYZER");

    lcd.setTextSize(1);

    lcd.setCursor(235, 17);

    lcd.print(networkCount);

    lcd.print(" APs");
}

// ============================================================
// NETWORK ROW
// ============================================================

void drawNetwork(
    int index,
    int y
)
{
    NetworkInfo &net = networks[index];

    // Background
    lcd.fillRoundRect(
        5,
        y,
        310,
        62,
        5,
        DARKGRAY
    );

    lcd.drawRoundRect(
        5,
        y,
        310,
        62,
        5,
        GRAY
    );

    // Signal
    drawSignalBars(
        12,
        y + 22,
        net.rssi
    );

    // SSID
    lcd.setTextColor(WHITE);

    lcd.setTextSize(1);

    String name = net.ssid;

    if (name.length() == 0)
        name = "<HIDDEN SSID>";

    if (name.length() > 22)
        name = name.substring(0, 22);

    lcd.setCursor(
        48,
        y + 8
    );

    lcd.print(name);

    // RSSI
    lcd.setCursor(
        48,
        y + 27
    );

    lcd.print(net.rssi);

    lcd.print(" dBm");

    // Channel
    lcd.setCursor(
        105,
        y + 27
    );

    lcd.print("CH ");
    lcd.print(net.channel);

    // Security
    String security =
        encryptionName(net.encryption);

    if (net.encryption == WIFI_AUTH_OPEN)
        lcd.setTextColor(RED);
    else
        lcd.setTextColor(GREEN);

    lcd.setCursor(
        160,
        y + 27
    );

    lcd.print(security);

    // BSSID
    lcd.setTextColor(LIGHTGRAY);

    lcd.setCursor(
        48,
        y + 45
    );

    lcd.print(net.bssid);
}

// ============================================================
// MAIN SCREEN
// ============================================================

void drawScreen()
{
    lcd.fillScreen(BLACK);

    drawHeader();

    if (networkCount == 0)
    {
        lcd.setTextColor(WHITE);

        lcd.setTextSize(2);

        lcd.setCursor(
            75,
            105
        );

        lcd.print("No networks");

        lcd.setTextSize(1);

        lcd.setCursor(
            95,
            135
        );

        lcd.print("Touch below to scan");

        drawButton(
            40,
            170,
            240,
            60,
            "SCAN WIFI",
            BLUE
        );

        return;
    }

    int start =
        currentPage * NETWORKS_PER_PAGE;

    int end =
        min(
            start + NETWORKS_PER_PAGE,
            networkCount
        );

    int y = 52;

    for (int i = start; i < end; i++)
    {
        drawNetwork(
            i,
            y
        );

        y += 67;
    }

    // ========================================================
    // PAGE NAVIGATION
    // ========================================================

    int totalPages =
        (networkCount + NETWORKS_PER_PAGE - 1)
        / NETWORKS_PER_PAGE;

    drawButton(
        5,
        392,
        75,
        42,
        "< PREV",
        GRAY
    );

    drawButton(
        240,
        392,
        75,
        42,
        "NEXT >",
        GRAY
    );

    // Page indicator

    lcd.setTextColor(WHITE);

    lcd.setTextSize(1);

    String pageText =
        "PAGE " +
        String(currentPage + 1) +
        "/" +
        String(totalPages);

    lcd.setCursor(
        137,
        410
    );

    lcd.print(pageText);

    // ========================================================
    // RESCAN
    // ========================================================

    drawButton(
        70,
        445,
        180,
        30,
        "RESCAN",
        BLUE
    );
}

// ============================================================
// SORT BY SIGNAL
// ============================================================

void sortNetworks()
{
    for (int i = 0;
         i < networkCount - 1;
         i++)
    {
        for (int j = i + 1;
             j < networkCount;
             j++)
        {
            if (
                networks[j].rssi >
                networks[i].rssi
            )
            {
                NetworkInfo temp =
                    networks[i];

                networks[i] =
                    networks[j];

                networks[j] =
                    temp;
            }
        }
    }
}

// ============================================================
// WIFI SCANNER
// ============================================================

void scanWiFi()
{
    lcd.fillScreen(BLACK);

    lcd.setTextColor(WHITE);

    lcd.setTextSize(2);

    lcd.setCursor(
        75,
        190
    );

    lcd.print("SCANNING...");

    lcd.setTextSize(1);

    lcd.setCursor(
        100,
        220
    );

    lcd.print("Searching for APs");

    Serial.println();
    Serial.println(
        "================================"
    );

    Serial.println(
        "Wi-Fi scan starting..."
    );

    Serial.println(
        "================================"
    );

    WiFi.mode(WIFI_STA);

    WiFi.disconnect(
        false,
        false
    );

    delay(100);

    int found =
        WiFi.scanNetworks(
            false,
            true,
            false,
            300
        );

    networkCount = 0;

    if (found <= 0)
    {
        Serial.println(
            "No networks found."
        );

        WiFi.scanDelete();

        drawScreen();

        return;
    }

    for (
        int i = 0;
        i < found &&
        networkCount < MAX_NETWORKS;
        i++
    )
    {
        NetworkInfo &net =
            networks[networkCount];

        net.ssid =
            WiFi.SSID(i);

        net.rssi =
            WiFi.RSSI(i);

        net.channel =
            WiFi.channel(i);

        net.encryption =
            WiFi.encryptionType(i);

        net.bssid =
            WiFi.BSSIDstr(i);

        net.hidden =
            net.ssid.length() == 0;

        // Serial output

        Serial.print(
            networkCount
        );

        Serial.print(" | ");

        Serial.print(
            net.ssid
        );

        Serial.print(" | ");

        Serial.print(
            net.rssi
        );

        Serial.print(" dBm | CH ");

        Serial.print(
            net.channel
        );

        Serial.print(" | ");

        Serial.print(
            encryptionName(
                net.encryption
            )
        );

        Serial.print(" | ");

        Serial.println(
            net.bssid
        );

        networkCount++;
    }

    WiFi.scanDelete();

    sortNetworks();

    currentPage = 0;

    Serial.println();

    Serial.print(
        "Networks found: "
    );

    Serial.println(
        networkCount
    );

    drawScreen();
}

// ============================================================
// TOUCH
// ============================================================

void handleTouch(
    uint16_t x,
    uint16_t y
)
{
    Serial.print(
        "Touch: X="
    );

    Serial.print(x);

    Serial.print(
        " Y="
    );

    Serial.println(y);

    // ========================================================
    // NO NETWORKS
    // ========================================================

    if (networkCount == 0)
    {
        if (
            x >= 40 &&
            x <= 280 &&
            y >= 170 &&
            y <= 230
        )
        {
            scanWiFi();
        }

        return;
    }

    // ========================================================
    // PREVIOUS
    // ========================================================

    if (
        x >= 5 &&
        x <= 80 &&
        y >= 392 &&
        y <= 434
    )
    {
        if (currentPage > 0)
        {
            currentPage--;

            drawScreen();
        }

        return;
    }

    // ========================================================
    // NEXT
    // ========================================================

    if (
        x >= 240 &&
        x <= 315 &&
        y >= 392 &&
        y <= 434
    )
    {
        int totalPages =
            (networkCount +
             NETWORKS_PER_PAGE - 1)
            / NETWORKS_PER_PAGE;

        if (
            currentPage <
            totalPages - 1
        )
        {
            currentPage++;

            drawScreen();
        }

        return;
    }

    // ========================================================
    // RESCAN
    // ========================================================

    if (
        x >= 70 &&
        x <= 250 &&
        y >= 445 &&
        y <= 480
    )
    {
        scanWiFi();

        return;
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );

    delay(500);

    Serial.println();
    Serial.println(
        "Color Kit Grande"
    );

    Serial.println(
        "Wi-Fi Analyzer"
    );

    // Backlight

    pinMode(
        LCD_BL,
        OUTPUT
    );

    digitalWrite(
        LCD_BL,
        HIGH
    );

    // I2C

    Wire.begin(
        TOUCH_SDA,
        TOUCH_SCL
    );

    pinMode(
        TOUCH_INT,
        INPUT_PULLUP
    );

    // Display

    lcd.init();

    /*
       PORTRAIT

       320 wide
       480 high
    */

    lcd.setRotation(0);

    lcd.setBrightness(255);

    lcd.fillScreen(
        BLACK
    );

    lcd.setTextColor(
        WHITE
    );

    lcd.setTextSize(2);

    lcd.setCursor(
        45,
        180
    );

    lcd.print(
        "COLOR KIT GRANDE"
    );

    lcd.setCursor(
        70,
        215
    );

    lcd.print(
        "WIFI ANALYZER"
    );

    delay(1500);

    // Wi-Fi

    WiFi.mode(
        WIFI_STA
    );

    WiFi.disconnect(
        false,
        false
    );

    delay(100);

    drawScreen();

    Serial.println(
        "Ready."
    );

    Serial.println(
        "Touch SCAN WIFI."
    );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    static unsigned long lastTouch = 0;

    uint16_t x;
    uint16_t y;

    if (
        millis() - lastTouch > 300
    )
    {
        if (
            readTouch(x, y)
        )
        {
            lastTouch =
                millis();

            handleTouch(
                x,
                y
            );
        }
    }

    delay(5);
}