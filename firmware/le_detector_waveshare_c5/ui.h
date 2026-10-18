// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "detector.h"

// Minimal working UI - border flash + status text. Colours, border
// thickness, and layout are cosmetic and get tuned later (decision #3);
// this is deliberately not the final design.
void uiInit();
void uiSetFlipped(bool flipped);
// sdLine: one short status line about the SD card / config (<= 24 chars).
// The OUI/SSID lines say where each watchlist came from (built-in or SD).
struct WatchlistResult;
void uiBootScreen(const char* sdLine, const WatchlistResult& wl);
void uiRender(const DetectorStatus& st, const char* phaseLabel);

// Bottom status strip (research capture state). Empty text clears it.
// Redraws only when something changed. `dot` adds a red recording dot.
void uiSetStrip(const char* text, uint16_t color, bool dot);
