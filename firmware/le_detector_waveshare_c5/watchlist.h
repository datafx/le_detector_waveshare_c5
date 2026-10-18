// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// SD card watchlists: /ledet/oui.txt and /ledet/ssid.txt. Each one, if
// present with at least one valid entry, REPLACES the matching built-in
// table. Missing, empty, comments-only or all-bad = built-in table. The two
// files are independent. The firmware never writes either file; examples
// ship in the repo (sd_card/ledet/), generated from the built-in tables by
// tools/check_sd_examples.py --write.
//
// oui.txt:   prefix, label      e.g.  00:25:DF, Axon
//            6, 7 or 9 hex digits = /24, /28, /36 (':' or '-' optional)
// ssid.txt:  pattern, label     e.g.  PSP-MVR*, PSP in-car video
//            ABC* starts with | *ABC* contains | ABC*DEF* starts with ABC,
//            then contains DEF | ABC (no star) exact. Case-insensitive.

// Version of the built-in tables. Bump whenever oui_table.cpp or
// ssid_table.cpp rows change, then regenerate the examples - a card file
// whose header carries an older version gets flagged on the boot screen.
#define WATCHLIST_VERSION "2026-10-17"

struct WatchlistResult {
    bool ouiFromSd;    // oui.txt replaced the built-in OUI table
    bool ssidFromSd;   // ssid.txt replaced the built-in SSID table
    bool ouiOld;       // ...and its version header is older than WATCHLIST_VERSION
    bool ssidOld;
};

// Call once at boot, after sdInit() and before scanning starts.
WatchlistResult watchlistLoad();
