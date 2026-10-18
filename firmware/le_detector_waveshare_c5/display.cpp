// SPDX-License-Identifier: GPL-3.0-only
//
// ST7796 bus setup and init sequence adapted from Waveshare's 07_lvgl_demo
// lcd_begin() (Apache-2.0, vendor repo @ 04e6134), which is the vendor IDF
// BSP sequence - bench-verified on this board 2026-10-16. Only MADCTL
// differs (landscape instead of portrait, see config.h).

#include "display.h"
#include "config.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

// The frame lives in PSRAM, which the SPI DMA can't read directly, and the
// panel wants big-endian RGB565 while the canvas stores native little-endian.
// So each flush is copied + byte-swapped through this internal DMA buffer,
// a band of rows at a time.
static const size_t BOUNCE_PIXELS = 480 * 16;

static esp_lcd_panel_io_handle_t s_io     = nullptr;
static uint16_t*                 s_bounce = nullptr;
static SemaphoreHandle_t         s_done   = nullptr;

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t* data = nullptr, size_t len = 0) {
    esp_lcd_panel_io_tx_param(s_io, c, data, len);
}

Display::Display(uint16_t w, uint16_t h) : GFXcanvas16(w, h, false) {}

bool Display::begin() {
    // GFXcanvas16 would malloc() this itself, but a 300KB frame has to be
    // forced into PSRAM - internal RAM is what WiFi and BLE run on. Done
    // here, not in the constructor: a global's constructor runs before the
    // Arduino core has brought PSRAM up.
    buffer = (uint16_t*)heap_caps_calloc((size_t)WIDTH * HEIGHT, sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return false;

    s_bounce = (uint16_t*)heap_caps_malloc(BOUNCE_PIXELS * sizeof(uint16_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_done = xSemaphoreCreateBinary();
    if (!s_bounce || !s_done) return false;

    spi_bus_config_t bus = {};
    bus.sclk_io_num     = PIN_SPI_SCK;
    bus.mosi_io_num     = PIN_SPI_MOSI;
    bus.miso_io_num     = PIN_SPI_MISO;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = BOUNCE_PIXELS * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    esp_lcd_panel_io_spi_config_t io = {};
    io.dc_gpio_num         = PIN_LCD_DC;
    io.cs_gpio_num         = PIN_LCD_CS;
    io.pclk_hz             = LCD_SPI_HZ;
    io.spi_mode            = 0;
    io.trans_queue_depth   = 10;
    io.on_color_trans_done = onColorDone;
    io.lcd_cmd_bits        = 8;
    io.lcd_param_bits      = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io) != ESP_OK) {
        return false;
    }

    cmd(0x01); delay(20);    // software reset
    cmd(0x11); delay(100);   // sleep out
    const uint8_t madctl = LCD_MADCTL;
    const uint8_t colmod = 0x55;   // RGB565
    cmd(0x36, &madctl, 1);
    cmd(0x3A, &colmod, 1);

    static const uint8_t f0_c3[] = {0xC3};
    static const uint8_t f0_96[] = {0x96};
    static const uint8_t b4[]    = {0x01};
    static const uint8_t b7[]    = {0xC6};
    static const uint8_t e8[]    = {0x40,0x8A,0x00,0x00,0x29,0x19,0xA5,0x33};
    static const uint8_t c1[]    = {0x06};
    static const uint8_t c2[]    = {0xA7};
    static const uint8_t c5[]    = {0x18};
    static const uint8_t e0[]    = {0xF0,0x09,0x0B,0x06,0x04,0x15,0x2F,0x54,0x42,0x3C,0x17,0x14,0x18,0x1B};
    static const uint8_t e1[]    = {0xF0,0x09,0x0B,0x06,0x04,0x03,0x2D,0x43,0x42,0x3B,0x16,0x14,0x17,0x1B};
    static const uint8_t f0_3c[] = {0x3C};
    static const uint8_t f0_69[] = {0x69};
    cmd(0xF0, f0_c3, sizeof(f0_c3));
    cmd(0xF0, f0_96, sizeof(f0_96));
    cmd(0xB4, b4, sizeof(b4));
    cmd(0xB7, b7, sizeof(b7));
    cmd(0xE8, e8, sizeof(e8));
    cmd(0xC1, c1, sizeof(c1));
    cmd(0xC2, c2, sizeof(c2));
    cmd(0xC5, c5, sizeof(c5));
    cmd(0xE0, e0, sizeof(e0));
    cmd(0xE1, e1, sizeof(e1));
    cmd(0xF0, f0_3c, sizeof(f0_3c));
    cmd(0xF0, f0_69, sizeof(f0_69));
    cmd(0x21);               // inversion on, same as vendor BSP
    cmd(0x29); delay(120);   // display on
    return true;
}

void Display::setFlipped(bool flipped) {
    if (!s_io) return;
    const uint8_t madctl = flipped ? LCD_MADCTL_FLIPPED : LCD_MADCTL;
    cmd(0x36, &madctl, 1);
}

void Display::flush(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!s_io) return;

    // Clip to the screen.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width())  w = width()  - x;
    if (y + h > height()) h = height() - y;
    if (w <= 0 || h <= 0) return;

    const int16_t bandRows = BOUNCE_PIXELS / w;
    const int16_t x2 = x + w - 1;

    for (int16_t row = y; row < y + h; row += bandRows) {
        int16_t rows = min<int16_t>(bandRows, y + h - row);

        uint16_t* dst = s_bounce;
        for (int16_t r = 0; r < rows; r++) {
            const uint16_t* src = buffer + (size_t)(row + r) * width() + x;
            for (int16_t c = 0; c < w; c++) *dst++ = __builtin_bswap16(src[c]);
        }

        const int16_t y1 = row, y2 = row + rows - 1;
        const uint8_t caset[] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x2 >> 8), (uint8_t)x2};
        const uint8_t raset[] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
        cmd(0x2A, caset, sizeof(caset));
        cmd(0x2B, raset, sizeof(raset));
        esp_lcd_panel_io_tx_color(s_io, 0x2C, s_bounce, (size_t)rows * w * sizeof(uint16_t));

        // The bounce buffer is reused next band - wait for the DMA to finish.
        xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    }
}
