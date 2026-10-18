// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Runtime-tunable settings
//
// TUNABLE(type, NAME, default) declares NAME as a global variable (defined
// in settings.cpp, starting at its default) instead of a compile-time
// constant, so /ledet/config.txt on the SD card can override it at boot
// (settings.cpp has the key names and allowed ranges). The names are the
// ones upstream uses, which keeps detector.cpp byte-identical with
// le_detector_c5. NAME_DEFAULT stays available as the stock value.
// Everything not declared with TUNABLE is fixed at build time.
// ---------------------------------------------------------------------------
#define TUNABLE(type, name, value) \
    extern type name;              \
    static const type name##_DEFAULT = value;

// ---------------------------------------------------------------------------
// Hardware pins - Waveshare ESP32-C5-Touch-LCD-3.5 (schematic + vendor BSP,
// confirmed on the bench 2026-10-16). Replaced wholesale from the XIAO
// build: every old pin number is a different peripheral on this board.
// ---------------------------------------------------------------------------
static const uint8_t PIN_I2C_SDA  = 27;  // shared: CH32, AXP2101, ES8311, FT6336, ...
static const uint8_t PIN_I2C_SCL  = 26;
static const uint32_t I2C_FREQ_HZ = 400000;

static const uint8_t PIN_SPI_SCK  = 6;   // shared LCD + SD
static const uint8_t PIN_SPI_MOSI = 7;
static const uint8_t PIN_SPI_MISO = 2;
static const uint8_t PIN_LCD_CS   = 8;
static const uint8_t PIN_LCD_DC   = 5;
// SD CS is GPIO9 - not used until SD bring-up (future project).

// LCD reset, touch reset, backlight and amp enable are not GPIOs here - they
// hang off the CH32V006 IO expander (I2C 0x24). See board.h.

// No PIN_LED - the screen border flashes instead (decision: "No status LED").
// BOOT button (active low): starts/stops a research capture (capture.h).
// Only read after boot - holding it at power-on still enters download mode.
static const uint8_t PIN_BOOT_BUTTON = 28;

// ---------------------------------------------------------------------------
// Radio scheduling - three-band phase machine
//
// The C5 has ONE radio behind a dual-band diplexer: it cannot listen to
// 2.4 and 5 GHz simultaneously, and still cannot do WiFi and BLE at once.
// So three phases, time-sliced, one radio stack up at a time.
//
// Reworked 2026-10-01 based on real drive data (Kismet, long highway drive)
// and a measured radio-switch cost on this board: WiFi->BLE = 103ms,
// BLE->WiFi = 9ms (perfectly consistent across 8 iterations, zero variance -
// see firmware/05_radio_switch_timing). Switching is cheap, so the duty
// cycle is driven by what's actually useful, not switch-cost avoidance:
//
// - 5 GHz is the only long-range, continuous signal (beacons every ~100ms,
//   seen at Axon Fleet Hub range up to ~0.5mi in the drive data) and is
//   therefore the dominant phase by a wide margin.
// - BLE is short-range (~10m) and most useful for a stopped/slow/adjacent
//   encounter, which is a multi-second situation, not a sub-second one - a
//   fast highway pass is barely catchable at any reasonable scan frequency
//   regardless, so BLE doesn't need to run every cycle. A short dip every
//   few 5GHz sweeps is enough.
// - 2.4 GHz contributes almost nothing on the road per the drive data
//   (PSP-MVR/Cradlepoint-type hits are a fixed-barracks-proximity signal,
//   not a mobile one) - kept only as a rare token check on the 3
//   non-overlapping channels (1/6/11), not a full 1-11 sweep.
// ---------------------------------------------------------------------------
// Channel lists are runtime arrays too (config.txt can replace them), up to
// MAX_SCAN_CHANNELS each. *_DEFAULT_LIST is the stock list.
static const uint8_t MAX_SCAN_CHANNELS = 24;

#define WIFI_5G_CHANNELS_DEFAULT_LIST 36, 40, 44, 48, 149, 153, 157, 161, 165
extern uint8_t WIFI_5G_CHANNELS[MAX_SCAN_CHANNELS];
extern uint8_t WIFI_5G_CHANNEL_COUNT;
// Phase length is always exactly one sweep (channel count x hop), recomputed
// whenever config.txt changes either - not a key of its own.
TUNABLE(uint32_t, WIFI_5G_PHASE_MS, 2700)   // one sweep of 9 channels at 300ms hop
TUNABLE(uint32_t, WIFI_5G_HOP_MS, 300)

// Non-overlapping US channels only - a token check for stationary facility
// APs, not a mobile detection path. See note above.
#define WIFI_2G_CHANNELS_DEFAULT_LIST 1, 6, 11
extern uint8_t WIFI_2G_CHANNELS[MAX_SCAN_CHANNELS];
extern uint8_t WIFI_2G_CHANNEL_COUNT;
TUNABLE(uint32_t, WIFI_2G_PHASE_MS, 750)    // one pass of 3 channels at 250ms hop
TUNABLE(uint32_t, WIFI_2G_HOP_MS, 250)

TUNABLE(uint32_t, BLE_PHASE_MS, 400)        // short dip, not a long window

// How many 5GHz sweeps happen between a BLE dip / a 2.4GHz token visit.
// Tune these empirically - the right split is still unknown, this is a
// starting point based on the reasoning above, not a measured optimum.
// 0 = never visit that band (e.g. 5GHz-only highway use).
TUNABLE(uint8_t, BLE_EVERY_N_SWEEPS, 3)
TUNABLE(uint8_t, WIFI_2G_EVERY_N_SWEEPS, 8)

// Passive = receive only, we never transmit. Set false only if you
// specifically want active BLE scanning (locked decision: stays passive).
static const bool BLE_SCAN_PASSIVE = true;

// ---------------------------------------------------------------------------
// Device tracking
// ---------------------------------------------------------------------------
static const uint8_t  MAX_TRACKED       = 48;    // fixed table, LRU eviction
static const uint32_t DEVICE_TTL_MS     = 10000; // drop entry if unseen this long
static const uint8_t  RSSI_EMA_NUM      = 1;     // EMA smoothing: new weight
static const uint8_t  RSSI_EMA_DEN      = 3;     //   value = (new*1 + old*2)/3

// ---------------------------------------------------------------------------
// Alert behaviour
//
// Screen border and buzzer are driven from a single phase so they are always
// in sync. Flash rate scales with the strongest active signal: weak = slow,
// strong = fast.
// ---------------------------------------------------------------------------
// Pure decay timer: resets to full duration on every qualifying receive
// (any matched device, not just the one that originally tripped the alert),
// never a latch. Three-phase mode needs enough slack to survive a full
// 5GHz->2.4GHz->BLE round trip plus missed-frame margin.
TUNABLE(uint32_t, ALERT_HOLD_MS, 14000)

TUNABLE(int8_t, RSSI_WEAK, -95)         // maps to the slowest flash
// -45 was an assumed ceiling, never measured. Field data: parked right next
// to real target vehicles tops out around -65 to -60 dBm (embedded, modest-
// power radios read through a vehicle body/window, not free-space range) -
// -45 was never reachable in practice, so the bar/flash rate never hit full
// scale. -65 is now "as strong as it gets" - full bar, fastest flash.
TUNABLE(int8_t, RSSI_STRONG, -65)       // maps to the fastest flash
TUNABLE(uint32_t, FLASH_PERIOD_SLOW, 1000)  // ms, full on+off cycle
TUNABLE(uint32_t, FLASH_PERIOD_FAST, 150)

// Matches weaker than this still show on screen (and get logged) but don't
// start or hold an alert - for ignoring distant gear. MIN_ALERT_RSSI_OFF
// (the default) disables the filter.
static const int16_t MIN_ALERT_RSSI_OFF = -128;
TUNABLE(int16_t, MIN_ALERT_RSSI, MIN_ALERT_RSSI_OFF)

// Buzzer follows the border flash exactly, but capped so a fast flash chirps
// instead of droning. Set to a large value to make it perfectly 1:1.
TUNABLE(uint32_t, BUZZER_MAX_ON_MS, 70)

// Set false to run silently (bench testing without the noise). "Buzzer" is
// now the onboard speaker (ES8311 + NS4150B) - name kept from upstream.
TUNABLE(bool, BUZZER_ENABLED, true)

// Speaker chirp, tuned on the bench by ear (decision 7). Loudness is
// AUDIO_DAC_VOLUME (ES8311 reg 0x32: 0xBF = 0dB, 0.5dB/step, 0xFF = +32dB)
// times AUDIO_TONE_AMPLITUDE (of 32767 full scale). config.txt sets pitch
// and volume (in dB, relative to 0xBF); amplitude stays fixed.
TUNABLE(uint32_t, AUDIO_TONE_HZ, 2400)      // near a piezo buzzer's pitch
static const int16_t  AUDIO_TONE_AMPLITUDE = 16384;  // -6dBFS
TUNABLE(uint8_t, AUDIO_DAC_VOLUME, 0xBF)    // 0dB

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static const uint32_t UI_REFRESH_MS = 200;

// ST7796 native is 320x480 portrait; we run landscape like upstream's
// setRotation(1). MADCTL 0x28 = MV|BGR (landscape); 0xE8 = MV|MX|MY|BGR is
// the same landscape rotated 180 degrees, for mounting the unit upside down.
static const uint8_t  LCD_MADCTL         = 0x28;
static const uint8_t  LCD_MADCTL_FLIPPED = 0xE8;
TUNABLE(bool, LCD_FLIP, false)
static const uint16_t LCD_WIDTH          = 480;
static const uint16_t LCD_HEIGHT         = 320;
static const uint32_t LCD_SPI_HZ         = 60000000;  // vendor-proven rate
TUNABLE(uint8_t, LCD_BACKLIGHT_PCT, 100)        // day; always the level at boot
TUNABLE(uint8_t, LCD_BACKLIGHT_NIGHT_PCT, 2)    // long press toggles day/night
TUNABLE(uint32_t, BOOT_SCREEN_MS, 5000)

// Research capture length (BOOT button; pressing again stops early).
TUNABLE(uint32_t, CAPTURE_MS, 180000)

// ---------------------------------------------------------------------------
// Touch - whole screen is one button. Tap = mute/unmute the current alert,
// long press = day/night backlight.
// ---------------------------------------------------------------------------
static const uint32_t TOUCH_POLL_MS       = 20;
static const uint32_t TOUCH_LONG_PRESS_MS = 800;
