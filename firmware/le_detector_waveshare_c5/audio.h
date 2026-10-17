// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

// Onboard speaker: ES8311 codec (I2C 0x18) -> NS4150B amp, fed over I2S by a
// FreeRTOS task that streams continuously - a generated tone while "on",
// silence otherwise - so the amp stays enabled from boot and never pops.
// Needs boardInit() first (I2C bus + CH32, which owns the amp enable).

// Returns false if the codec didn't respond; alerts then stay visual-only.
bool audioInit();

// Non-blocking; takes effect within one DMA buffer (~23ms), with a short
// fade at each edge to avoid clicks.
void audioSetTone(bool on);
