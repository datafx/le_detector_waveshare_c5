// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// Board bring-up for the Waveshare ESP32-C5-Touch-LCD-3.5: shared I2C bus
// and the CH32V006 IO expander, which owns LCD reset, touch reset, the amp
// enable and the backlight. Must run before the display is touched - with
// LCD reset still held by the expander the panel stays blank, no error.

// CH32V006 expander pins (vendor BSP bsp_io_expander_init - note the
// porting brief had P0/P1 swapped).
static const uint8_t EXIO_TP_RST  = 0;
static const uint8_t EXIO_LCD_RST = 1;
static const uint8_t EXIO_PA_EN   = 5;   // NS4150B speaker amp enable

// I2C up, expander configured, LCD + touch reset pulsed and released. Amp
// stays off and backlight at 0 - callers turn those on when ready.
bool boardInit();

bool boardSetBacklight(uint8_t percent);
bool boardSetAmp(bool on);
