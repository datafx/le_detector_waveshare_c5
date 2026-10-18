// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// microSD card on the SPI bus shared with the display (SD CS = GPIO9).
// Optional: with no card every feature falls back to built-in defaults and
// the detector runs exactly as without SD.
//
// File rules: the firmware never creates or edits the user's files in
// /ledet (config.txt, oui.txt, ssid.txt). The only file it writes there is
// errors.txt, its own report of bad lines in those files.

#define SD_MOUNT "/sd"
#define SD_DIR   SD_MOUNT "/ledet"

// Mount the card. Needs the display up first (the display owns the SPI bus
// init). Never formats. Returns false with no card or no readable FAT32.
bool sdInit();
bool sdMounted();

// Record a bad line in one of the user's files, e.g.
// sdNoteError("config.txt", 12, "unknown key 'foo'"). Kept in RAM until
// sdFinishErrors().
void sdNoteError(const char* file, int line, const char* fmt, ...);
uint16_t sdErrorCount();

// Write /ledet/errors.txt if anything was noted, or delete a stale one from
// an earlier boot if not. Call once, after every user file has been read.
void sdFinishErrors();
