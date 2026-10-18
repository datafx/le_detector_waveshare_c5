// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "oui_table.h"   // GearCategory

// How a pattern is matched against a parsed SSID. Still plain byte
// comparisons (no regex/glob engine, no dynamic allocation) - matches the
// OUI table's "as simple as the job needs" philosophy. There's no literal
// wildcard character; each mode is a fixed shape instead.
//
// SSID_MATCH_PREFIX             - pattern must match the start of the SSID,
//                                 e.g. "IBR900-" matches "IBR900-1A2B3C".
// SSID_MATCH_CONTAINS           - pattern may appear anywhere in the SSID.
// SSID_MATCH_PREFIX_AND_CONTAINS - `pattern` must match the start of the
//                                 SSID AND `contains` must appear somewhere
//                                 in what's left after that prefix - the
//                                 two-literals-with-a-gap shape a glob would
//                                 write as "IBR*-mobile". Requires both
//                                 fields set on the row.
//
// SSID_MATCH_EXACT              - (Waveshare port) whole SSID equals
//                                 `pattern`. Only produced by SD card
//                                 watchlists (a pattern with no `*`).
enum SsidMatchMode : uint8_t {
    SSID_MATCH_PREFIX = 0,
    SSID_MATCH_CONTAINS,
    SSID_MATCH_PREFIX_AND_CONTAINS,
    SSID_MATCH_EXACT
};

struct SsidEntry {
    const char*   pattern;   // case-insensitive, plain bytes, no wildcards
    SsidMatchMode mode;
    const char*   vendor;
    GearCategory  category;
    // Only read for SSID_MATCH_PREFIX_AND_CONTAINS - the required substring
    // after `pattern`. Every row must still supply all five fields (no
    // default member initializer here - that would make this a
    // non-aggregate under -std=c++11, breaking every existing row's
    // 4-field initializer, including the off-target test build). Pass
    // nullptr explicitly for PREFIX/CONTAINS-only rows.
    const char*   contains;
};

// Case-insensitive lookup against the watchlist. `ssid` must be NUL-terminated
// (the caller already bounds-checks and copies the raw IE bytes into a fixed
// buffer before calling this). Returns nullptr when nothing matches, which
// includes an empty string - there's nothing to match against a wildcard
// probe or a hidden-SSID beacon.
const SsidEntry* ssidLookup(const char* ssid);

uint16_t ssidTableSize();   // entries in the active table

// Waveshare port: replace the built-in table with one loaded from the SD
// card. `table` must stay valid forever. Call before scanning starts.
void ssidSetTable(const SsidEntry* table, uint16_t count);
