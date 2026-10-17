// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// FT6336 touch controller (I2C 0x38). Only "is the screen being touched" is
// read - no coordinates - because the whole screen is one button: a tap
// anywhere and a long press anywhere are the only two gestures, so nothing
// needs aiming while driving. Needs boardInit() first (I2C bus, and the
// CH32 already pulsed the touch reset).

enum TouchEvent : uint8_t { TOUCH_NONE = 0, TOUCH_TAP, TOUCH_LONG };

// Returns false if the controller didn't answer; touchPoll() then always
// returns TOUCH_NONE and the detector runs exactly as without touch.
bool touchInit();

// Non-blocking; call every loop iteration (it throttles its own I2C reads).
// TOUCH_TAP fires on release of a press shorter than TOUCH_LONG_PRESS_MS;
// TOUCH_LONG fires once while still held, as soon as that time is reached.
TouchEvent touchPoll();
