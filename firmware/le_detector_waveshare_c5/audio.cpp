// SPDX-License-Identifier: GPL-3.0-only
//
// ES8311 register sequence adapted from Espressif esp_codec_dev
// device/es8311/es8311.c (Apache-2.0, esp-adf @ e05f54d, component 1.6.0),
// flattened for the exact configuration Waveshare's factory BSP uses on this
// board: codec is I2S slave, no MCLK pin (use_mclk = false - the codec
// derives its internal clock as BCLK x 8), Philips I2S, 16-bit, 22050 Hz.
// The register writes are in the same order as es8311_open() ->
// es8311_set_fs() -> es8311_enable() -> es8311_set_vol().
//
// I2S uses the IDF i2s_std driver directly rather than the core's ESP_I2S
// wrapper: ESP_I2S hard-codes 6 x 240-frame DMA buffers (~65ms queued at
// 22050Hz), which would start every 70ms chirp about a chirp-length late.

#include "audio.h"
#include "board.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s_std.h>

static const uint8_t PIN_I2S_BCLK = 23;
static const uint8_t PIN_I2S_WS   = 10;
static const uint8_t PIN_I2S_DOUT = 25;   // ESP32 -> ES8311 DSDIN

static const uint8_t  ES8311_ADDR   = 0x18;   // confirmed by I2C scan + chip ID
static const uint32_t SAMPLE_RATE   = 22050;  // vendor default; 5.6448MHz internal MCLK
static const uint32_t DMA_FRAMES    = 128;    // per DMA buffer, = one task block
static const uint32_t DMA_BUFFERS   = 4;      // 512 frames queued, ~23ms
static const uint32_t FADE_SAMPLES  = SAMPLE_RATE * 2 / 1000;   // 2ms edges

static i2s_chan_handle_t s_tx      = nullptr;
static volatile bool     s_toneOn  = false;
static int16_t           s_sine[256];

// ---------------------------------------------------------------------------
// ES8311 over I2C
// ---------------------------------------------------------------------------

static bool s_codecOk = true;

static void wr(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(value);
    if (Wire.endTransmission() != 0) s_codecOk = false;
}

static uint8_t rd(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(ES8311_ADDR, (uint8_t)1) != 1) {
        s_codecOk = false;
        return 0;
    }
    return Wire.read();
}

static bool codecInit() {
    s_codecOk = true;

    // --- es8311_open(): slave, BCLK as clock source, no inversion ---
    if (rd(0x0D) != 0xFA) wr(0x0D, 0xFA);
    wr(0x44, 0x08);   // I2C noise immunity - written twice, per Espressif:
    wr(0x44, 0x08);   // the first write after power-up occasionally fails
    wr(0x01, 0x30);
    wr(0x02, 0x00);
    wr(0x03, 0x10);
    wr(0x16, 0x24);
    wr(0x04, 0x10);
    wr(0x05, 0x00);
    wr(0x0B, 0x00);
    wr(0x0C, 0x00);
    wr(0x10, 0x1F);
    wr(0x11, 0x7F);
    wr(0x00, 0x80);
    wr(0x00, rd(0x00) & 0xBF);          // slave mode
    wr(0x01, 0xBF);                     // internal MCLK from BCLK, not inverted
    wr(0x06, rd(0x06) & ~0x20);         // SCLK not inverted
    wr(0x13, 0x10);
    wr(0x1B, 0x0A);
    wr(0x1C, 0x6A);
    wr(0x44, 0x58);                     // internal reference (ADCL + DACR)

    // --- es8311_set_fs(): 16-bit, Philips, 22050Hz ---
    wr(0x09, rd(0x09) | 0x0C);          // 16-bit DAC port
    wr(0x0A, rd(0x0A) | 0x0C);
    wr(0x09, rd(0x09) & 0xFC);          // I2S normal format
    wr(0x0A, rd(0x0A) & 0xFC);
    // coeff row {mclk 5644800, 22050}: pre_div 1, adc/dac div 1, single
    // speed, lrck 0x00FF, bclk_div 4, osr 0x10. With use_mclk = false the
    // pre-multiplier is forced to x8 (BCLK 705.6kHz x 8 = 5.6448MHz).
    wr(0x02, (rd(0x02) & 0x07) | (3 << 3));
    wr(0x05, 0x00);
    wr(0x03, (rd(0x03) & 0x80) | 0x10);
    wr(0x04, (rd(0x04) & 0x80) | 0x10);
    wr(0x07, (rd(0x07) & 0xC0) | 0x00);
    wr(0x08, 0xFF);
    wr(0x06, (rd(0x06) & 0xE0) | (4 - 1));

    // --- es8311_enable() -> es8311_start(): DAC path up ---
    wr(0x00, 0x80);                     // slave
    wr(0x01, 0xBF);
    wr(0x09, rd(0x09) & 0xBF);          // DAC serial port on
    wr(0x0A, rd(0x0A) & 0xBF);
    wr(0x17, 0xBF);
    wr(0x0E, 0x02);
    wr(0x12, 0x00);                     // DAC on
    wr(0x14, 0x1A);
    wr(0x14, rd(0x14) & ~0x40);         // analog mic, not PDM
    wr(0x0D, 0x01);
    wr(0x15, 0x40);
    wr(0x37, 0x08);
    wr(0x45, 0x00);
    wr(0x31, rd(0x31) & 0x9F);          // unmute

    // --- volume: set explicitly, never inherited from the factory demo ---
    wr(0x32, AUDIO_DAC_VOLUME);

    return s_codecOk;
}

// ---------------------------------------------------------------------------
// I2S + tone task
// ---------------------------------------------------------------------------

static bool i2sInit() {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num  = DMA_BUFFERS;
    chan.dma_frame_num = DMA_FRAMES;
    chan.auto_clear    = true;   // underrun plays silence, not a stale buffer
    if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_WS,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return false;
    return i2s_channel_enable(s_tx) == ESP_OK;
}

// Runs forever. i2s_channel_write() blocks until a DMA buffer frees up, which
// is what paces the loop - one 128-frame block per ~5.8ms.
static void audioTask(void*) {
    static int16_t block[DMA_FRAMES];
    const uint32_t phaseInc = (uint32_t)(((uint64_t)AUDIO_TONE_HZ << 32) / SAMPLE_RATE);
    uint32_t phase = 0;
    uint32_t gain  = 0;   // 0..FADE_SAMPLES, linear fade envelope

    for (;;) {
        const bool on = s_toneOn;
        for (uint32_t i = 0; i < DMA_FRAMES; i++) {
            if (on  && gain < FADE_SAMPLES) gain++;
            if (!on && gain > 0)            gain--;
            if (gain == 0) {
                block[i] = 0;
                phase = 0;   // every chirp starts at the same zero crossing
            } else {
                int32_t s = s_sine[phase >> 24];
                block[i] = (int16_t)(s * (int32_t)gain / (int32_t)FADE_SAMPLES);
                phase += phaseInc;
            }
        }
        size_t written;
        i2s_channel_write(s_tx, block, sizeof(block), &written, portMAX_DELAY);
    }
}

bool audioInit() {
    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int16_t)(AUDIO_TONE_AMPLITUDE * sinf(2.0f * PI * i / 256.0f));
    }

    // BCLK has to be running before the codec is configured - with no MCLK
    // pin, it's the codec's only clock source.
    if (!i2sInit()) {
        Serial.println("ERROR: I2S init failed");
        return false;
    }
    xTaskCreate(audioTask, "audio", 4096, nullptr, 5, nullptr);

    if (!codecInit()) {
        Serial.println("ERROR: ES8311 init failed");
        return false;
    }

    // Amp on last, once the codec is already outputting silence, and left on
    // for good - toggling it per alert would add latency and pop.
    delay(50);
    if (!boardSetAmp(true)) {
        Serial.println("ERROR: amp enable failed");
        return false;
    }
    return true;
}

void audioSetTone(bool on) {
    s_toneOn = on;
}
