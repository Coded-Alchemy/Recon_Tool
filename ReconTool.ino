/*
 * ============================================================
 * Wireless Recon Console  -  Phase 1
 * ============================================================
 * Sketch : Arduino IDE / arduino-cli (.ino)
 *   Board  : ESP32 Wrover Module, PSRAM Enabled, ESP32 core 2.0.17
 *   Library: LovyanGFX (lovyan03)
 * Target : ThingPulse ESP32 Color Kit Grande
 *          (ePulse Feather ESP32, ILI9488 320x480, FT6236 touch)
 *
 * Phase 1: passive Wi-Fi visibility, touch UI, network list and
 *          details, channel statistics, privacy / recording mode.
 * Future : BLE, Bluetooth Classic, RF analysis, storage/export.
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <algorithm>

// ============================================================
// PIN MAP  (display pins match ThingPulse's reference project)
// ============================================================

namespace Pins
{
    constexpr int TFT_SCLK = 5;
    constexpr int TFT_MOSI = 18;
    constexpr int TFT_MISO = 19;
    constexpr int TFT_CS   = 15;
    constexpr int TFT_DC   = 2;
    constexpr int TFT_RST  = 4;
    constexpr int TFT_BL   = 32;   // backlight, driven via digitalWrite in setup()

    // FT6236 capacitive touch (I2C). Verify against ThingPulse settings.h
    // if the boot log says "touch controller NOT found".
    constexpr int TOUCH_SDA = 23;
    constexpr int TOUCH_SCL = 22;
}

constexpr uint8_t TOUCH_ADDR = 0x38;

// Touch orientation tweaks - flip if taps land in the wrong place.
constexpr bool TOUCH_SWAP_XY  = false;
constexpr bool TOUCH_INVERT_X = false;
constexpr bool TOUCH_INVERT_Y = false;
constexpr bool TOUCH_DEBUG    = true;   // log raw press coordinates

// ============================================================
// DISPLAY
// ============================================================

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI       _bus;

public:
    LGFX()
    {
        {
            auto cfg = _bus.config();

            cfg.spi_host    = VSPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 20000000;   // 20 MHz - verified working with DisplayTest
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;

            cfg.pin_sclk = Pins::TFT_SCLK;
            cfg.pin_mosi = Pins::TFT_MOSI;
            cfg.pin_miso = Pins::TFT_MISO;
            cfg.pin_dc   = Pins::TFT_DC;

            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        {
            auto cfg = _panel.config();

            cfg.pin_cs   = Pins::TFT_CS;
            cfg.pin_rst  = Pins::TFT_RST;
            cfg.pin_busy = -1;

            cfg.memory_width  = 320;
            cfg.memory_height = 480;
            cfg.panel_width   = 320;
            cfg.panel_height  = 480;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;

            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;

            cfg.readable   = false;   // no need to read pixels back
            cfg.invert     = false;   // try true if colors look inverted
            cfg.rgb_order  = false;   // try true if red/blue are swapped
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;   // SPI is display-only; touch is I2C

            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

LGFX lcd;

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 480;

// ============================================================
// COLORS (RGB565)
// ============================================================

constexpr uint16_t COLOR_BG     = 0x0000;
constexpr uint16_t COLOR_PANEL  = 0x1082;
constexpr uint16_t COLOR_ACTIVE = 0x041F;
constexpr uint16_t COLOR_TEXT   = 0xFFFF;
constexpr uint16_t COLOR_DIM    = 0x8410;
constexpr uint16_t COLOR_GREEN  = 0x07E0;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_RED    = 0xF800;
constexpr uint16_t COLOR_CYAN   = 0x07FF;
constexpr uint16_t COLOR_ORANGE = 0xFD20;

// ============================================================
// LAYOUT
// ============================================================

constexpr int HEADER_H = 48;
constexpr int STATUS_Y = 48;
constexpr int LIST_TOP = 68;
constexpr int ROW_PITCH = 70;
constexpr int ROW_H = 64;
constexpr int ROWS_PER_PAGE = 5;
constexpr int NAV_Y = 430;
constexpr int NAV_H = 50;
constexpr int TAB_COUNT = 6;

constexpr int SWIPE_PX = 40;
constexpr uint32_t SCAN_INTERVAL_MS = 30000;
constexpr uint32_t SCAN_TIMEOUT_MS  = 15000;

// ============================================================
// DATA MODEL
// ============================================================

constexpr int MAX_WIFI_NETWORKS = 50;

struct NetworkInfo
{
    String ssid;
    String bssid;
    int32_t rssi = -100;
    int32_t channel = 0;
    wifi_auth_mode_t encryption = WIFI_AUTH_OPEN;
    bool hidden = false;
    bool duplicateSSID = false;
    uint32_t firstSeen = 0;
    uint32_t lastSeen = 0;
};

NetworkInfo networks[MAX_WIFI_NETWORKS];
int networkCount = 0;

// ============================================================
// UI STATE
// ============================================================

enum Screen
{
    SCR_WIFI,
    SCR_DETAILS,
    SCR_STATS,
    SCR_BLE,
    SCR_BT,
    SCR_RF,
    SCR_PRIVACY
};

struct Tab
{
    const char *label;
    Screen screen;
};

const Tab TABS[TAB_COUNT] = {
    {"WIFI", SCR_WIFI},
    {"CHAN", SCR_STATS},
    {"BLE",  SCR_BLE},
    {"BT",   SCR_BT},
    {"RF",   SCR_RF},
    {"PRIV", SCR_PRIVACY},
};

Screen currentScreen = SCR_WIFI;
String selectedBssid;       // selection survives rescans / re-sorting
int scrollOffset = 0;
bool needRedraw = true;

// ============================================================
// PRIVACY
//   OFF          : everything visible
//   IDENTIFIERS  : BSSIDs masked
//   RECORDING    : SSIDs + BSSIDs masked (safe for screen capture)
//   MAX          : SSIDs, BSSIDs and signal levels masked
// ============================================================

enum PrivacyMode
{
    PRIVACY_OFF,
    PRIVACY_IDENTIFIERS,
    PRIVACY_RECORDING,
    PRIVACY_MAX
};

PrivacyMode privacyMode = PRIVACY_OFF;

String shownSSID(int i)
{
    if (networks[i].hidden)
        return "<hidden>";

    if (privacyMode >= PRIVACY_RECORDING)
        return "WiFi-" + String(i + 1);

    return networks[i].ssid;
}

String shownBSSID(int i)
{
    if (privacyMode >= PRIVACY_IDENTIFIERS)
        return "AP-" + String(i + 1);

    return networks[i].bssid;
}

String shownRSSI(int i)
{
    if (privacyMode >= PRIVACY_MAX)
        return "--";

    return String(networks[i].rssi) + " dBm";
}

String fit(const String &s, size_t maxLen)
{
    if (s.length() <= maxLen)
        return s;

    return s.substring(0, maxLen - 2) + "..";
}

// ============================================================
// TOUCH (FT6236)
// ============================================================

bool readTouch(int &x, int &y)
{
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);

    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (size_t)5, true) != 5)
        return false;

    uint8_t touches = Wire.read() & 0x0F;
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();

    if (touches == 0 || touches > 2)
        return false;

    int rx = ((xh & 0x0F) << 8) | xl;
    int ry = ((yh & 0x0F) << 8) | yl;

    if (TOUCH_SWAP_XY)
        std::swap(rx, ry);
    if (TOUCH_INVERT_X)
        rx = SCREEN_W - 1 - rx;
    if (TOUCH_INVERT_Y)
        ry = SCREEN_H - 1 - ry;

    x = constrain(rx, 0, SCREEN_W - 1);
    y = constrain(ry, 0, SCREEN_H - 1);

    return true;
}

// ============================================================
// WIFI SCANNING (asynchronous - UI never blocks)
// ============================================================

bool scanRunning = false;
uint32_t scanStarted = 0;
uint32_t lastScanDone = 0;

void drawStatusLine();

void startScan()
{
    if (scanRunning)
        return;

    WiFi.scanDelete();
    // async, show hidden, active scan, 300 ms per channel
    WiFi.scanNetworks(true, true, false, 300);

    scanRunning = true;
    scanStarted = millis();

    if (currentScreen == SCR_WIFI)
        drawStatusLine();
}

void collectScan(int found)
{
    static NetworkInfo fresh[MAX_WIFI_NETWORKS];

    if (found > MAX_WIFI_NETWORKS)
        found = MAX_WIFI_NETWORKS;

    uint32_t now = millis();

    for (int i = 0; i < found; i++)
    {
        fresh[i].ssid          = WiFi.SSID(i);
        fresh[i].bssid         = WiFi.BSSIDstr(i);
        fresh[i].rssi          = WiFi.RSSI(i);
        fresh[i].channel       = WiFi.channel(i);
        fresh[i].encryption    = WiFi.encryptionType(i);
        fresh[i].hidden        = fresh[i].ssid.length() == 0;
        fresh[i].duplicateSSID = false;
        fresh[i].firstSeen     = now;
        fresh[i].lastSeen      = now;
    }

    std::sort(fresh, fresh + found,
              [](const NetworkInfo &a, const NetworkInfo &b)
              { return a.rssi > b.rssi; });

    // Duplicate SSIDs (hidden ones excluded)
    for (int i = 0; i < found; i++)
    {
        if (fresh[i].hidden)
            continue;

        for (int j = i + 1; j < found; j++)
        {
            if (fresh[i].ssid == fresh[j].ssid)
            {
                fresh[i].duplicateSSID = true;
                fresh[j].duplicateSSID = true;
            }
        }
    }

    // Keep firstSeen for APs we already knew about
    for (int i = 0; i < found; i++)
    {
        for (int j = 0; j < networkCount; j++)
        {
            if (networks[j].bssid == fresh[i].bssid)
            {
                fresh[i].firstSeen = networks[j].firstSeen;
                break;
            }
        }
    }

    for (int i = 0; i < found; i++)
        networks[i] = fresh[i];

    networkCount = found;

    int maxOffset = max(0, networkCount - ROWS_PER_PAGE);
    scrollOffset = constrain(scrollOffset, 0, maxOffset);
}

void pollScan()
{
    if (!scanRunning)
        return;

    int n = WiFi.scanComplete();

    if (n == WIFI_SCAN_RUNNING)
    {
        if (millis() - scanStarted > SCAN_TIMEOUT_MS)
        {
            Serial.println("Wi-Fi scan timed out, resetting.");
            WiFi.scanDelete();
            scanRunning = false;
            lastScanDone = millis();
        }
        return;
    }

    scanRunning = false;
    lastScanDone = millis();

    if (n < 0)
    {
        Serial.println("Wi-Fi scan failed.");
        needRedraw = true;
        return;
    }

    collectScan(n);
    WiFi.scanDelete();

    // Counts only - identifiers are never logged (Recording Mode safe)
    Serial.printf("Scan complete: %d networks\n", networkCount);

    needRedraw = true;
}

void maybeAutoScan()
{
    if (scanRunning)
        return;

    if (currentScreen != SCR_WIFI && currentScreen != SCR_STATS)
        return;

    if (millis() - lastScanDone > SCAN_INTERVAL_MS)
        startScan();
}

// ============================================================
// HELPERS
// ============================================================

String securityName(wifi_auth_mode_t mode)
{
    switch (mode)
    {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        default:                        return "OTHER";
    }
}

bool isWeakSecurity(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_OPEN || mode == WIFI_AUTH_WEP;
}

int findSelected()
{
    for (int i = 0; i < networkCount; i++)
        if (networks[i].bssid == selectedBssid)
            return i;

    return -1;
}

void setFont2()
{
    lcd.setFont(&lgfx::fonts::Font2);
    lcd.setTextSize(1);
}

// ============================================================
// DRAWING PRIMITIVES
// ============================================================

void drawSignalBars(int x, int y, int rssi)
{
    int level = 0;

    if (rssi >= -50)      level = 4;
    else if (rssi >= -60) level = 3;
    else if (rssi >= -70) level = 2;
    else if (rssi >= -80) level = 1;

    uint16_t on = level >= 3 ? COLOR_GREEN
                : level == 2 ? COLOR_YELLOW
                             : COLOR_ORANGE;

    for (int i = 0; i < 4; i++)
    {
        int h = 6 + i * 5;

        lcd.fillRect(x + i * 7, y + 25 - h, 5, h,
                     i < level ? on : COLOR_PANEL);
    }
}

void drawHeader(const char *title, bool showBack)
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, COLOR_PANEL);

    lcd.setFont(&lgfx::fonts::Font4);
    lcd.setTextSize(1);
    lcd.setTextColor(COLOR_TEXT, COLOR_PANEL);
    lcd.setCursor(8, 11);
    lcd.print(title);

    setFont2();

    if (showBack)
    {
        lcd.setTextColor(COLOR_CYAN, COLOR_PANEL);
        lcd.setCursor(246, 15);
        lcd.print("< BACK");
    }
    else if (privacyMode != PRIVACY_OFF)
    {
        lcd.setTextColor(COLOR_YELLOW, COLOR_PANEL);
        lcd.setCursor(250, 15);
        lcd.print("PRIVACY");
    }
}

void drawHeader(const char *title)
{
    drawHeader(title, false);
}

void drawBottomNav()
{
    lcd.fillRect(0, NAV_Y, SCREEN_W, NAV_H, COLOR_PANEL);

    Screen active = (currentScreen == SCR_DETAILS) ? SCR_WIFI : currentScreen;
    int tabW = SCREEN_W / TAB_COUNT;

    setFont2();

    for (int i = 0; i < TAB_COUNT; i++)
    {
        uint16_t bg = (TABS[i].screen == active) ? COLOR_ACTIVE : COLOR_PANEL;

        lcd.fillRect(i * tabW, NAV_Y, tabW, NAV_H, bg);
        lcd.setTextColor(COLOR_TEXT, bg);

        int tw = lcd.textWidth(TABS[i].label);
        lcd.setCursor(i * tabW + (tabW - tw) / 2, NAV_Y + 17);
        lcd.print(TABS[i].label);
    }
}

void drawStatusLine()
{
    lcd.fillRect(0, STATUS_Y, SCREEN_W, 18, COLOR_BG);

    setFont2();
    lcd.setTextColor(COLOR_DIM, COLOR_BG);
    lcd.setCursor(8, STATUS_Y + 1);
    lcd.print(String(networkCount) + " networks");

    if (scanRunning)
    {
        lcd.setTextColor(COLOR_YELLOW, COLOR_BG);
        lcd.print("   scanning...");
    }
    else
    {
        lcd.setTextColor(COLOR_DIM, COLOR_BG);
        lcd.print("   tap to rescan");
    }
}

void drawField(const char *label, const String &value, int y,
               uint16_t color)
{
    setFont2();

    lcd.setTextColor(COLOR_DIM, COLOR_BG);
    lcd.setCursor(10, y);
    lcd.print(label);

    lcd.setTextColor(color, COLOR_BG);
    lcd.setCursor(120, y);
    lcd.print(value);
}

void drawField(const char *label, const String &value, int y)
{
    drawField(label, value, y, COLOR_TEXT);
}

// ============================================================
// SCREENS
// ============================================================

void drawWiFiScreen()
{
    lcd.fillScreen(COLOR_BG);
    drawHeader("WIFI RECON");
    drawStatusLine();

    setFont2();

    for (int r = 0; r < ROWS_PER_PAGE; r++)
    {
        int i = scrollOffset + r;

        if (i >= networkCount)
            break;

        int y = LIST_TOP + r * ROW_PITCH;

        lcd.drawRect(5, y, 310, ROW_H, COLOR_PANEL);

        lcd.setTextColor(COLOR_TEXT, COLOR_BG);
        lcd.setCursor(12, y + 5);
        lcd.print(fit(shownSSID(i), 22));

        lcd.setTextColor(isWeakSecurity(networks[i].encryption)
                             ? COLOR_RED : COLOR_DIM,
                         COLOR_BG);
        lcd.setCursor(12, y + 24);
        lcd.print("CH " + String(networks[i].channel) + "  " +
                  securityName(networks[i].encryption));

        lcd.setTextColor(COLOR_DIM, COLOR_BG);
        lcd.setCursor(12, y + 43);
        lcd.print(shownBSSID(i));

        drawSignalBars(282, y + 8, networks[i].rssi);

        String rssi = shownRSSI(i);
        lcd.setCursor(310 - lcd.textWidth(rssi), y + 43);
        lcd.print(rssi);
    }

    // Scroll indicator
    if (networkCount > ROWS_PER_PAGE)
    {
        int trackH = ROWS_PER_PAGE * ROW_PITCH - 6;
        int thumbH = max(16, trackH * ROWS_PER_PAGE / networkCount);
        int maxOff = networkCount - ROWS_PER_PAGE;
        int thumbY = LIST_TOP + (trackH - thumbH) * scrollOffset / maxOff;

        lcd.fillRect(316, thumbY, 3, thumbH, COLOR_DIM);
    }

    drawBottomNav();
}

void drawDetailsScreen()
{
    lcd.fillScreen(COLOR_BG);
    drawHeader("NETWORK", true);

    int i = findSelected();

    if (i < 0)
    {
        setFont2();
        lcd.setTextColor(COLOR_DIM, COLOR_BG);
        lcd.setCursor(10, 70);
        lcd.print("Network no longer visible");
        drawBottomNav();
        return;
    }

    const NetworkInfo &n = networks[i];

    lcd.setFont(&lgfx::fonts::Font4);
    lcd.setTextSize(1);
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    lcd.setCursor(10, 62);
    lcd.print(fit(shownSSID(i), 20));

    int y = 110;

    drawField("BSSID", shownBSSID(i), y);               y += 28;
    drawField("RSSI", shownRSSI(i), y);                 y += 28;
    drawField("CHANNEL",
              String(n.channel) + (n.channel > 14 ? " (5 GHz)" : " (2.4 GHz)"),
              y);                                       y += 28;
    drawField("SECURITY", securityName(n.encryption), y,
              isWeakSecurity(n.encryption) ? COLOR_RED : COLOR_TEXT);
                                                        y += 28;
    drawField("SSID REUSE", n.duplicateSSID ? "DUPLICATE" : "UNIQUE", y,
              n.duplicateSSID ? COLOR_YELLOW : COLOR_TEXT);
                                                        y += 28;
    drawField("FIRST SEEN",
              String((millis() - n.firstSeen) / 1000) + " s ago", y);
                                                        y += 28;
    drawField("LAST SEEN",
              String((millis() - n.lastSeen) / 1000) + " s ago", y);
                                                        y += 40;

    setFont2();

    if (n.encryption == WIFI_AUTH_OPEN)
    {
        lcd.setTextColor(COLOR_RED, COLOR_BG);
        lcd.setCursor(10, y);
        lcd.print("WARNING: OPEN NETWORK");
    }
    else if (n.encryption == WIFI_AUTH_WEP)
    {
        lcd.setTextColor(COLOR_RED, COLOR_BG);
        lcd.setCursor(10, y);
        lcd.print("WARNING: WEP IS BROKEN");
    }

    drawBottomNav();
}

void drawStatsScreen()
{
    lcd.fillScreen(COLOR_BG);
    drawHeader("CHANNELS");

    constexpr int CH_COUNT = 13;
    int perChannel[CH_COUNT + 1] = {0};
    int open = 0, wep = 0, hidden = 0, dup = 0;

    for (int i = 0; i < networkCount; i++)
    {
        int ch = networks[i].channel;

        if (ch >= 1 && ch <= CH_COUNT)
            perChannel[ch]++;

        if (networks[i].encryption == WIFI_AUTH_OPEN) open++;
        if (networks[i].encryption == WIFI_AUTH_WEP)  wep++;
        if (networks[i].hidden)                       hidden++;
        if (networks[i].duplicateSSID)                dup++;
    }

    int busiest = 1;
    for (int ch = 1; ch <= CH_COUNT; ch++)
        if (perChannel[ch] > perChannel[busiest])
            busiest = ch;

    setFont2();
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);

    lcd.setCursor(10, 58);
    lcd.print("Networks " + String(networkCount) +
              "   Hidden " + String(hidden));

    lcd.setCursor(10, 76);
    lcd.print("Open " + String(open) + "  WEP " + String(wep) +
              "  Dup SSID " + String(dup));

    lcd.setCursor(10, 94);
    lcd.setTextColor(COLOR_ORANGE, COLOR_BG);
    lcd.print("Busiest: CH " + String(busiest) + " (" +
              String(perChannel[busiest]) + " APs)");

    lcd.setTextColor(COLOR_DIM, COLOR_BG);
    lcd.setCursor(10, 120);
    lcd.print("APs per 2.4 GHz channel");

    const int baseY = 390;
    const int maxH  = 230;
    const int barW  = 18;
    const int pitch = 23;
    const int x0    = 10;
    int maxCount    = max(1, perChannel[busiest]);

    lcd.drawFastHLine(x0 - 2, baseY, pitch * CH_COUNT + 2, COLOR_DIM);

    for (int ch = 1; ch <= CH_COUNT; ch++)
    {
        int x = x0 + (ch - 1) * pitch;
        int h = perChannel[ch] * maxH / maxCount;

        if (perChannel[ch] > 0)
        {
            lcd.fillRect(x, baseY - h, barW, h,
                         ch == busiest ? COLOR_ORANGE : COLOR_CYAN);

            lcd.setTextColor(COLOR_TEXT, COLOR_BG);
            lcd.setCursor(x + (perChannel[ch] > 9 ? 1 : 5), baseY - h - 18);
            lcd.print(perChannel[ch]);
        }

        lcd.setTextColor(COLOR_DIM, COLOR_BG);
        lcd.setCursor(x + (ch > 9 ? 1 : 5), baseY + 4);
        lcd.print(ch);
    }

    drawBottomNav();
}

void drawPlaceholder(const char *title, const char *message)
{
    lcd.fillScreen(COLOR_BG);
    drawHeader(title);

    lcd.setFont(&lgfx::fonts::Font4);
    lcd.setTextSize(1);
    lcd.setTextColor(COLOR_DIM, COLOR_BG);

    int tw = lcd.textWidth(message);
    lcd.setCursor((SCREEN_W - tw) / 2, 200);
    lcd.print(message);

    drawBottomNav();
}

void drawPrivacyScreen()
{
    lcd.fillScreen(COLOR_BG);
    drawHeader("PRIVACY");

    static const char *OPTIONS[] = {
        "PRIVACY OFF", "IDENTIFIERS", "RECORDING MODE", "MAX PRIVACY"};

    static const char *DESCRIPTIONS[] = {
        "Everything visible.",
        "BSSIDs are masked.",
        "SSIDs and BSSIDs masked. Safe to screen-record.",
        "SSIDs, BSSIDs and signal levels masked."};

    setFont2();
    lcd.setTextColor(COLOR_DIM, COLOR_BG);
    lcd.setCursor(10, 62);
    lcd.print("Hide sensitive identifiers on screen");

    for (int i = 0; i < 4; i++)
    {
        int y = 100 + i * 60;
        uint16_t bg = (privacyMode == i) ? COLOR_ACTIVE : COLOR_PANEL;

        lcd.fillRect(10, y, 300, 48, bg);

        lcd.setFont(&lgfx::fonts::Font4);
        lcd.setTextSize(1);
        lcd.setTextColor(COLOR_TEXT, bg);
        lcd.setCursor(24, y + 11);
        lcd.print(OPTIONS[i]);
    }

    setFont2();
    lcd.setTextColor(COLOR_YELLOW, COLOR_BG);
    lcd.setCursor(10, 350);
    lcd.print(DESCRIPTIONS[privacyMode]);

    drawBottomNav();
}

void drawScreen()
{
    switch (currentScreen)
    {
        case SCR_WIFI:    drawWiFiScreen();    break;
        case SCR_DETAILS: drawDetailsScreen(); break;
        case SCR_STATS:   drawStatsScreen();   break;
        case SCR_BLE:     drawPlaceholder("BLE RECON",   "Coming next"); break;
        case SCR_BT:      drawPlaceholder("BLUETOOTH",   "Coming next"); break;
        case SCR_RF:      drawPlaceholder("RF ANALYSIS", "Coming next"); break;
        case SCR_PRIVACY: drawPrivacyScreen(); break;
    }
}

// ============================================================
// INPUT HANDLING
// ============================================================

void goTo(int s)
{
    if (currentScreen == (Screen)s)
        return;

    currentScreen = (Screen)s;
    needRedraw = true;
}

void handleTap(int x, int y)
{
    // Bottom navigation is available everywhere
    if (y >= NAV_Y)
    {
        int tab = constrain(x / (SCREEN_W / TAB_COUNT), 0, TAB_COUNT - 1);
        goTo(TABS[tab].screen);
        return;
    }

    switch (currentScreen)
    {
        case SCR_WIFI:
        {
            if (y >= STATUS_Y && y < LIST_TOP)
            {
                startScan();
                return;
            }

            if (y >= LIST_TOP)
            {
                int r = (y - LIST_TOP) / ROW_PITCH;
                int within = (y - LIST_TOP) % ROW_PITCH;
                int i = scrollOffset + r;

                if (r < ROWS_PER_PAGE && within < ROW_H && i < networkCount)
                {
                    selectedBssid = networks[i].bssid;
                    goTo(SCR_DETAILS);
                }
            }
            break;
        }

        case SCR_DETAILS:
            if (y < HEADER_H && x > 230)
                goTo(SCR_WIFI);
            break;

        case SCR_PRIVACY:
        {
            if (y >= 100 && y < 340)
            {
                int i = (y - 100) / 60;
                int within = (y - 100) % 60;

                if (i < 4 && within < 48)
                {
                    privacyMode = (PrivacyMode)i;
                    needRedraw = true;
                }
            }
            break;
        }

        default:
            break;
    }
}

void handleSwipe(int dy)
{
    if (currentScreen != SCR_WIFI)
        return;

    int rows = max(1, abs(dy) / ROW_PITCH);

    scrollOffset += (dy < 0) ? rows : -rows;   // swipe up => scroll down

    int maxOffset = max(0, networkCount - ROWS_PER_PAGE);
    scrollOffset = constrain(scrollOffset, 0, maxOffset);

    needRedraw = true;
}

// Press / release state machine. Releases are confirmed after two
// consecutive empty reads so I2C glitches don't cause phantom taps.
void pollTouch()
{
    static bool down = false;
    static int startX = 0, startY = 0, lastX = 0, lastY = 0;
    static uint8_t misses = 0;

    int x, y;

    if (readTouch(x, y))
    {
        misses = 0;

        if (!down)
        {
            down = true;
            startX = x;
            startY = y;

            if (TOUCH_DEBUG)
                Serial.printf("Touch down: x=%d y=%d\n", x, y);
        }

        lastX = x;
        lastY = y;
        return;
    }

    if (down && ++misses >= 2)
    {
        down = false;
        misses = 0;

        int dx = lastX - startX;
        int dy = lastY - startY;

        if (abs(dy) > SWIPE_PX && abs(dy) > abs(dx))
            handleSwipe(dy);
        else
            handleTap(startX, startY);
    }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void splash(const char *line1, const char *line2)
{
    lcd.fillScreen(COLOR_BG);

    lcd.setFont(&lgfx::fonts::Font4);
    lcd.setTextSize(1);
    lcd.setTextColor(COLOR_GREEN, COLOR_BG);
    lcd.setCursor(20, 180);
    lcd.print("WIRELESS RECON");

    setFont2();
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    lcd.setCursor(20, 220);
    lcd.print(line1);

    if (line2)
    {
        lcd.setTextColor(COLOR_DIM, COLOR_BG);
        lcd.setCursor(20, 242);
        lcd.print(line2);
    }
}

void splash(const char *line1)
{
    splash(line1, nullptr);
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("================================");
    Serial.println(" Wireless Recon Console - Phase 1");
    Serial.println("================================");

    // Display
    // Backlight: plain GPIO, active-high (verified working in DisplayTest)
    pinMode(Pins::TFT_BL, OUTPUT);
    digitalWrite(Pins::TFT_BL, HIGH);

    lcd.init();
    lcd.setRotation(0);
    splash("Display OK", "Starting touch + Wi-Fi...");
    Serial.println("Display initialised.");

    // Touch
    Wire.begin(Pins::TOUCH_SDA, Pins::TOUCH_SCL);
    Wire.setClock(400000);

    Wire.beginTransmission(TOUCH_ADDR);
    bool touchFound = (Wire.endTransmission() == 0);

    Serial.println(touchFound
                       ? "Touch controller found at 0x38."
                       : "Touch controller NOT found - check I2C pins.");

    if (!touchFound)
        splash("Display OK", "Touch controller not found (I2C)");

    // Wi-Fi: passive scanning only, never associate
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    startScan();
    drawScreen();

    Serial.println("Ready.");
}

void loop()
{
    pollTouch();
    pollScan();
    maybeAutoScan();

    if (needRedraw)
    {
        needRedraw = false;
        drawScreen();
    }

    delay(10);
}
