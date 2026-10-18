// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// Research capture: the BOOT button records every WiFi management frame and
// BLE advert the scanner hears for CAPTURE_MS (config.txt capture_minutes),
// or until pressed again. Nothing is buffered before the press.
//
//   /ledet/captures/NNNN_K.pcap      WiFi, pcap + radiotap (Wireshark/Kismet)
//   /ledet/captures/NNNN_K_ble.csv   BLE adverts, decoded + raw payload
//
// NNNN = boot number (one drive's captures group together), K = capture
// number this boot. Timestamps are time since boot - there's no clock yet.
//
// Detection is untouched: the detector's callbacks only hand each frame to
// a non-blocking copy into a PSRAM ring buffer, and a low-priority task
// writes the card. If the card falls behind, frames are dropped and counted
// - scanning and alerts never wait on the SD card.

// BOOT button pressed: start a capture, or stop the running one.
void captureButton();

// Call every loop iteration: handles the time limit and finishing up.
void captureUpdate();

// What the bottom status strip should show right now ("REC 2:41  1234",
// "Saved 0042_1", "No SD card"...). Returns false when it should be blank.
// *recording is true while capturing (the UI adds a red dot).
bool captureStatus(char* text, size_t n, uint16_t* color, bool* recording);
