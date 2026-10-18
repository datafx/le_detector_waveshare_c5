// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// /ledet/config.txt: `key = value` lines overriding the TUNABLE settings in
// config.h. Missing file, missing key, or a commented-out key = the
// firmware default. A bad line (unknown key, bad or out-of-range value) is
// skipped, its default kept, and the line reported via sdNoteError(). The
// firmware never writes this file. Key names and ranges: settings.cpp.

struct SettingsResult {
    bool     fileFound;   // config.txt exists on the card
    uint16_t applied;     // valid key = value lines (0 = stock behaviour)
};

// Call once at boot, after sdInit() and before anything reads a setting.
// Safe without a card: everything stays at its default.
SettingsResult settingsLoad();
