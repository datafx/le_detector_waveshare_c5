// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

// ST7796 behind a full-frame Adafruit_GFX canvas held in PSRAM. Drawing calls
// only touch RAM; nothing reaches the panel until flush() pushes a rectangle.
// Keeps upstream ui.cpp's Adafruit_GFX drawing code intact while using the
// vendor-proven esp_lcd panel init instead of a third-party ST7796 driver.
class Display : public GFXcanvas16 {
public:
    Display(uint16_t w, uint16_t h);

    // Panel init. boardInit() must already have released LCD reset.
    bool begin();

    // Push one rectangle of the canvas to the panel (clipped to the screen).
    // Blocks until the transfer is done.
    void flush(int16_t x, int16_t y, int16_t w, int16_t h);
    void flushAll() { flush(0, 0, width(), height()); }

    // Rotate the image 180 degrees (unit mounted upside down). Takes effect
    // on the next flush; doesn't touch the canvas.
    void setFlipped(bool flipped);
};
