// SPDX-License-Identifier: GPL-3.0-only
//
// CH32V006 protocol adapted from Waveshare's 07_lvgl_demo Board_IO.cpp
// (Apache-2.0, vendor repo @ 04e6134). The expander runs Waveshare-specific
// firmware - don't swap in a generic IO-expander library. Registers are
// written one byte wide, matching the vendor IDF driver; 01_exio's 16-bit
// form also gets ACKed but isn't the one proven to drive LCD reset/amp.

#include "board.h"
#include "config.h"
#include <Wire.h>

static const uint8_t CH32_ADDR       = 0x24;
static const uint8_t CH32_REG_MODE   = 0x02;   // direction, bit = 1 -> output
static const uint8_t CH32_REG_OUTPUT = 0x03;
static const uint8_t CH32_REG_PWM    = 0x05;   // backlight, 0-255

static uint8_t s_output = 0;   // shadow of the output register

static bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(CH32_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool setOutputs(uint8_t value) {
    if (!writeReg(CH32_REG_OUTPUT, value)) return false;
    s_output = value;
    return true;
}

bool boardInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    const uint8_t outputs = (1U << EXIO_TP_RST) | (1U << EXIO_LCD_RST) | (1U << EXIO_PA_EN);
    const uint8_t resets  = (1U << EXIO_TP_RST) | (1U << EXIO_LCD_RST);

    // Same sequence as the vendor init, except PA_EN is left low: the amp
    // comes on later, once the codec is configured and streaming silence.
    bool ok = writeReg(CH32_REG_MODE, 0xFF) &&
              setOutputs(0x00) &&
              writeReg(CH32_REG_PWM, 0x00) &&
              writeReg(CH32_REG_MODE, outputs) &&
              setOutputs(resets);
    delay(50);
    ok = ok && setOutputs(0x00);   // assert both resets
    delay(50);
    ok = ok && setOutputs(resets); // release
    delay(120);                    // ST7796 needs 120ms after reset release
    return ok;
}

bool boardSetBacklight(uint8_t percent) {
    if (percent > 100) percent = 100;
    return writeReg(CH32_REG_PWM, (uint8_t)(percent * 255U / 100U));
}

bool boardSetAmp(bool on) {
    uint8_t next = on ? (s_output | (1U << EXIO_PA_EN)) : (s_output & ~(1U << EXIO_PA_EN));
    return setOutputs(next);
}
