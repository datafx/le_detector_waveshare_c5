// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from legacy/src/ui.cpp (128x64 monochrome OLED via u8g2) to the
// ILI9341 colour display. Layout mirrors the original almost exactly -
// header state bar, auto-sized vendor name, match-detail line, RSSI bar (or
// a "no matched gear" watchlist summary) - just in colour, on a bigger
// screen, with one real change: the original signalled an alert via a
// physical LED; this design has none (decision: "No status LED"), so the
// flashing screen border (synced to the buzzer via alertOutputOn(), per
// decision #3) is the alert indicator instead. The header's ALL CLEAR / **
// ALERT ** text is otherwise unchanged from the original, non-flashing.
//
// Waveshare port: same layout rescaled from 320x240 to the ST7796's 480x320
// (text size 2 -> 3 keeps the same ~24 characters per line), drawn into a
// PSRAM canvas (display.h). Drawing is RAM-only now, so each section
// flushes exactly the rectangle it redrew.

#include "ui.h"
#include "alert.h"
#include "config.h"
#include "display.h"
#include "oui_table.h"
#include "ssid_table.h"

static Display tft(LCD_WIDTH, LCD_HEIGHT);

static const uint16_t COLOR_BG      = 0x0000;   // black
static const uint16_t COLOR_TEXT    = 0xFFFF;   // white
static const uint16_t COLOR_ALERT   = 0xF800;   // red
static const uint8_t  BORDER_THICKNESS = 9;
static const uint8_t  MARGIN = BORDER_THICKNESS + 9;

static uint16_t screenW() { return tft.width(); }
static uint16_t screenH() { return tft.height(); }

void uiInit() {
    if (!tft.begin()) Serial.println("ERROR: display init failed");
    tft.fillScreen(COLOR_BG);
    tft.flushAll();
    // A line that overflows the screen width would otherwise wrap onto the
    // next line and collide with whatever gets drawn at that fixed Y
    // position next - a silent overlap bug, not just a clipped line. Clip
    // instead: worst case one line loses its tail, which is far more
    // readable than two lines rendered on top of each other.
    tft.setTextWrap(false);
}

void uiBootScreen() {
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_TEXT);

    tft.setTextSize(4);
    tft.setCursor(MARGIN, MARGIN);
    tft.println("LE GEAR DETECTOR");
    uint16_t y = MARGIN + 38;
    tft.drawFastHLine(MARGIN, y, screenW() - 2 * MARGIN, COLOR_TEXT);
    y += 16;

    char line[48];
    tft.setTextSize(3);
    const uint16_t LINE_H = 32;

    snprintf(line, sizeof(line), "OUI entries: %u", (unsigned)ouiTableSize());
    tft.setCursor(MARGIN, y);
    tft.println(line);
    y += LINE_H;

    snprintf(line, sizeof(line), "SSID entries: %u", (unsigned)ssidTableSize());
    tft.setCursor(MARGIN, y);
    tft.println(line);
    y += LINE_H;

    snprintf(line, sizeof(line), "BLE scan: %s", BLE_SCAN_PASSIVE ? "passive" : "ACTIVE");
    tft.setCursor(MARGIN, y);
    tft.println(line);
    y += LINE_H;

    // Split across two lines, not one - the combined "WiFi 5GHz: N ch,
    // 2.4GHz: N ch (token)" line runs ~37 characters against a ~24-character
    // line (444px usable / 18px per char at size 3), and would clip.
    snprintf(line, sizeof(line), "WiFi 5GHz: %u ch", (unsigned)WIFI_5G_CHANNEL_COUNT);
    tft.setCursor(MARGIN, y);
    tft.println(line);
    y += LINE_H;

    snprintf(line, sizeof(line), "WiFi 2.4GHz: %u ch (tok)", (unsigned)WIFI_2G_CHANNEL_COUNT);
    tft.setCursor(MARGIN, y);
    tft.println(line);
    y += LINE_H;

    tft.setCursor(MARGIN, y);
    tft.println("Starting...");
    tft.flushAll();

    delay(5000);
    tft.fillScreen(COLOR_BG);
    tft.flushAll();
}

static void drawBorder(bool on) {
    uint16_t color = on ? COLOR_ALERT : COLOR_BG;
    for (uint8_t i = 0; i < BORDER_THICKNESS; i++) {
        tft.drawRect(i, i, screenW() - 2 * i, screenH() - 2 * i, color);
    }
    // Four thin strips, not the bounding box - that would be the whole screen.
    const int16_t t = BORDER_THICKNESS;
    tft.flush(0, 0, screenW(), t);
    tft.flush(0, screenH() - t, screenW(), t);
    tft.flush(0, t, t, screenH() - 2 * t);
    tft.flush(screenW() - t, t, t, screenH() - 2 * t);
}

// Largest text size (Adafruit_GFX's integer font-size multiplier) that keeps
// `text` within maxWidth, down to 1 as a guaranteed-fit fallback. Mirrors
// the original's "biggest font that fits, else drop a tier" vendor-name
// sizing - same idea, GFX's coarser size steps instead of u8g2's font list.
static uint8_t fitTextSize(const char* text, uint8_t maxSize, uint16_t maxWidth) {
    for (uint8_t size = maxSize; size >= 1; size--) {
        tft.setTextSize(size);
        int16_t x1, y1;
        uint16_t w, h;
        tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        if (w <= maxWidth) return size;
    }
    return 1;
}

static const uint8_t HEADER_H = 44;

// Header and content are redrawn independently, each only when its own
// inputs change. Phase label ticks over every few seconds (much more often
// than the match state changes) - redrawing the whole screen for that alone
// was the visible "flicker on phase change" this splits apart.
static bool      s_headerEverRendered = false;
static char      s_lastPhase[16]      = "";
static AlertState s_lastAlertState    = ALERT_CLEAR;

static bool            s_contentEverRendered = false;
static uint8_t          s_lastCount    = 0xFF;
static int16_t          s_lastRssi     = -127;
static const TrackedDevice* s_lastBest = nullptr;
static const int16_t    RSSI_REDRAW_DEADBAND = 3;

static bool s_borderEverRendered = false;
static bool s_lastBorderOn       = false;

static void drawHeader(bool alerting, const char* phaseLabel) {
    const int16_t hx = MARGIN - 4, hy = MARGIN - 4, hw = screenW() - 2 * (MARGIN - 4);
    tft.fillRect(hx, hy, hw, HEADER_H + 1, COLOR_BG);   // +1 clears the underline

    if (alerting) {
        tft.fillRect(hx, hy, hw, HEADER_H, COLOR_ALERT);
        tft.setTextColor(COLOR_TEXT);
        tft.setTextSize(3);
        tft.setCursor(MARGIN + 4, MARGIN + 6);
        tft.print("** ALERT **");
    } else {
        tft.setTextColor(COLOR_TEXT);
        tft.setTextSize(3);
        tft.setCursor(MARGIN + 4, MARGIN + 6);
        tft.print("ALL CLEAR");
        tft.drawFastHLine(hx, hy + HEADER_H, hw, COLOR_TEXT);
    }
    // Phase indicator sits on the header bar at a fixed x, same reasoning as
    // the original: a shorter/longer label (e.g. "5GHz WiFi" vs "BLE")
    // shouldn't shift other header text around. 210px fits the longest
    // label ("2.4GHz WiFi", 11 chars x 18px) with margin.
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(3);
    tft.setCursor(screenW() - 210, MARGIN + 6);
    tft.print(phaseLabel);

    tft.flush(hx, hy, hw, HEADER_H + 1);
}

static void drawContent(const DetectorStatus& st) {
    const uint16_t contentTop = MARGIN - 4 + HEADER_H + 14;
    const uint16_t contentW   = screenW() - 2 * (MARGIN - 4) - 8;
    char line[48];

    const int16_t cx = MARGIN - 4, cy = contentTop - 6;
    const int16_t cw = screenW() - 2 * (MARGIN - 4);
    const int16_t ch = screenH() - (contentTop - 6) - (MARGIN - 4);
    tft.fillRect(cx, cy, cw, ch, COLOR_BG);

    if (st.best) {
        // --- vendor name, as large as fits ---
        uint8_t size = fitTextSize(st.best->vendor, 7, contentW);
        tft.setTextSize(size);
        tft.setTextColor(COLOR_TEXT);
        tft.setCursor(MARGIN, contentTop + 14);
        tft.print(st.best->vendor);

        // --- secondary line: what actually matched, and which radio/band -
        // useful for troubleshooting, not just "it's an OUI hit" ---
        tft.setTextSize(3);
        tft.setCursor(MARGIN, contentTop + 94);
        const char* band = (st.best->band == BAND_5G) ? "5GHz" :
                            (st.best->band == BAND_2G) ? "2.4GHz" : "";
        if (st.best->ssid[0] != '\0') {
            const char* how = (st.best->source == SRC_PROBE) ? "probe" : "beacon";
            snprintf(line, sizeof(line), "SSID %s (%s): %s", how, band, st.best->ssid);
        } else if (st.best->source == SRC_BLE) {
            snprintf(line, sizeof(line), "via BLE");
        } else {
            snprintf(line, sizeof(line), "via %s WiFi OUI", band);
        }
        tft.print(line);

        // --- signal strength bar ---
        int16_t r = st.bestRssi;
        if (r < RSSI_WEAK)   r = RSSI_WEAK;
        if (r > RSSI_STRONG) r = RSSI_STRONG;
        int barMaxW = contentW - 116;   // leaves room for "-100dB" at size 3
        int barW = (int)(((int32_t)r - RSSI_WEAK) * barMaxW /
                         ((int32_t)RSSI_STRONG - RSSI_WEAK));

        int barY = contentTop + 130;
        tft.drawRect(MARGIN, barY, barMaxW, 26, COLOR_TEXT);
        if (barW > 0) tft.fillRect(MARGIN + 1, barY + 1, barW, 24, COLOR_TEXT);

        tft.setTextSize(3);
        tft.setCursor(MARGIN + barMaxW + 8, barY + 1);
        snprintf(line, sizeof(line), "%ddB", (int)st.bestRssi);
        tft.print(line);
    } else {
        tft.setTextColor(COLOR_TEXT);
        tft.setTextSize(3);

        tft.setCursor(MARGIN, contentTop + 14);
        tft.print("No matched gear");

        tft.setCursor(MARGIN, contentTop + 54);
        snprintf(line, sizeof(line), "Watchlist: %u OUI", (unsigned)ouiTableSize());
        tft.print(line);

        tft.setCursor(MARGIN, contentTop + 86);
        snprintf(line, sizeof(line), "%u SSID pattern%s", (unsigned)ssidTableSize(),
                 ssidTableSize() == 1 ? "" : "s");
        tft.print(line);
    }

    tft.flush(cx, cy, cw, ch);
}

void uiRender(const DetectorStatus& st, const char* phaseLabel) {
    // Upstream redrew the border every call; with a RAM canvas that would
    // push four strips over SPI for nothing, so only on a state change.
    bool borderOn = alertOutputOn();
    if (!s_borderEverRendered || borderOn != s_lastBorderOn) {
        s_borderEverRendered = true;
        s_lastBorderOn       = borderOn;
        drawBorder(borderOn);
    }

    AlertState alertNow = alertState();
    bool headerChanged = !s_headerEverRendered ||
                          (alertNow != s_lastAlertState) ||
                          (strcmp(phaseLabel, s_lastPhase) != 0);
    if (headerChanged) {
        s_headerEverRendered = true;
        s_lastAlertState     = alertNow;
        strncpy(s_lastPhase, phaseLabel, sizeof(s_lastPhase) - 1);
        s_lastPhase[sizeof(s_lastPhase) - 1] = '\0';
        drawHeader(alertNow == ALERT_ACTIVE, phaseLabel);
    }

    bool rssiMoved = abs((int)st.bestRssi - (int)s_lastRssi) >= RSSI_REDRAW_DEADBAND;
    bool contentChanged = !s_contentEverRendered ||
                          (st.activeCount != s_lastCount) ||
                          (st.best != s_lastBest) ||
                          rssiMoved;
    if (contentChanged) {
        s_contentEverRendered = true;
        s_lastCount = st.activeCount;
        s_lastRssi  = st.bestRssi;
        s_lastBest  = st.best;
        drawContent(st);
    }
}
