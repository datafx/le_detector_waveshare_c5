// SPDX-License-Identifier: GPL-3.0-only
//
// Register use follows Waveshare's 07_lvgl_demo Touch_FT6336.cpp (Apache-2.0,
// vendor repo @ 04e6134): TD_STATUS at 0x02, low nibble = touch point count.

#include "touch.h"
#include "config.h"
#include <Wire.h>

static const uint8_t FT6336_ADDR      = 0x38;
static const uint8_t FT6336_TD_STATUS = 0x02;
static const uint8_t FT6336_MAX_POINTS = 2;

static bool     s_ok         = false;
static uint32_t s_lastPoll   = 0;
static uint8_t  s_downReads  = 0;      // consecutive "touched" reads
static bool     s_pressed    = false;  // debounced state
static bool     s_longFired  = false;
static uint32_t s_pressStart = 0;

// A failed read counts as "not touched", so a bus glitch can end a press
// early but never invent one.
static bool readTouched() {
    Wire.beginTransmission(FT6336_ADDR);
    Wire.write(FT6336_TD_STATUS);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(FT6336_ADDR, (uint8_t)1) != 1) return false;
    uint8_t points = Wire.read() & 0x0F;
    return points >= 1 && points <= FT6336_MAX_POINTS;   // 0x0F = invalid
}

bool touchInit() {
    Wire.beginTransmission(FT6336_ADDR);
    s_ok = (Wire.endTransmission() == 0);
    return s_ok;
}

TouchEvent touchPoll() {
    if (!s_ok) return TOUCH_NONE;

    uint32_t now = millis();
    if (now - s_lastPoll < TOUCH_POLL_MS) return TOUCH_NONE;
    s_lastPoll = now;

    bool touched = readTouched();

    if (!s_pressed) {
        // Two consecutive touched reads before a press counts - one stray
        // report (EMI, a raindrop of condensation) shouldn't toggle mute.
        s_downReads = touched ? s_downReads + 1 : 0;
        if (s_downReads >= 2) {
            s_pressed    = true;
            s_longFired  = false;
            s_pressStart = now;
        }
        return TOUCH_NONE;
    }

    if (touched) {
        if (!s_longFired && now - s_pressStart >= TOUCH_LONG_PRESS_MS) {
            s_longFired = true;
            return TOUCH_LONG;
        }
        return TOUCH_NONE;
    }

    // Released. A long press already fired its event while held.
    s_pressed   = false;
    s_downReads = 0;
    return s_longFired ? TOUCH_NONE : TOUCH_TAP;
}
