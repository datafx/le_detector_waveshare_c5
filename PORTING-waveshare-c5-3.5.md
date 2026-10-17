# Porting brief: le_detector C5 build → Waveshare ESP32-C5-Touch-LCD-3.5-EN

## Goal

Port the existing ESP32-C5 build of le_detector to the Waveshare ESP32-C5-Touch-LCD-3.5-EN **as-is**, with one functional change: alerts play through the board's **onboard speaker** (ES8311 codec + NS4150B amp) instead of a GPIO piezo buzzer.

**In scope:** detection logic unchanged, display output, SD watchlists/logging, audio alerts.
**Out of scope for this pass:** GPS, touch input, external buzzer, daughterboard, new features.

The GitHub repo (`datafx/le_detector_c5`) is the source of truth. Read the current C5 code before changing anything. The previous C5 target was a Seeed XIAO ESP32-C5 driving a 2.8" ILI9341 SPI display (240×320, resistive touch, microSD), with a GPIO buzzer and a flashing screen border as the visual alert.

---

## Target hardware

| Item | Detail |
|---|---|
| Board | Waveshare ESP32-C5-Touch-LCD-3.5-EN (SKU 35416, no battery, no camera) |
| Module | ESP32-C5-WROOM-1U-N32R8 (32 MB flash, 8 MB PSRAM), IPEX antenna connector |
| Display | 3.5" IPS 320×480, **ST7796**, SPI |
| Touch | FT6336 capacitive, I2C (not used this pass) |
| Audio | ES8311 codec → NS4150B amp → onboard speaker (verified working with factory firmware) |
| IO expander | CH32V006, I2C, factory-programmed. Controls LCD reset, touch reset, backlight, amp enable |
| PMIC | AXP2101 |
| Storage | microSD, shares SPI bus with LCD |

Vendor resources (Apache-2.0):
- Repo: https://github.com/waveshareteam/ESP32-C5-Touch-LCD-3.5
- Schematic: `schematic/ESP32-C5-Touch-LCD-3.5.pdf` in the repo
- Wiki: https://docs.waveshare.com/ESP32-C5-Touch-LCD-3.5

---

## Toolchain

- Arduino IDE with **Arduino-ESP32 core 3.3.10** (the version Waveshare's examples target)
- Board: **ESP32C5 Dev Module**
- Tools menu: copy flash size, PSRAM and partition settings from Waveshare's Arduino wiki page. PSRAM must be enabled and flash set to 32 MB. Wrong settings build cleanly and then fail at runtime.
- Bundled libraries from `example/Arduino-v3.3.10/libraries/` (lvgl 8.4, SensorLib, XPowersLib) as needed.

---

## Pin map (from schematic)

### ESP32-C5 GPIO

| Function | GPIO | Notes |
|---|---|---|
| I2C SDA | 27 | Shared: CH32, AXP2101, ES8311, FT6336, QMI8658, PCF85063, SHTC3 |
| I2C SCL | 26 | Shared |
| SPI SCK | 6 | Shared LCD + SD |
| SPI MOSI | 7 | Shared LCD + SD |
| SPI MISO | 2 | Shared LCD + SD |
| LCD CS | 8 | |
| LCD DC | 5 | |
| SD CS | 9 | Via 0Ω selector R57 (should be strapped to SD) |
| Touch INT | 3 | Not used this pass |
| I2S SCLK (BCLK) | 23 | |
| I2S LRCK (WS) | 10 | |
| I2S data **out** to codec (ES8311 DSDIN) | **25** | ESP32 TX → codec DAC |
| I2S data **in** from codec (ES8311 ASDOUT) | **24** | Codec ADC → ESP32 RX (mic, unused) |
| BOOT button | 28 | Usable as an input after boot |
| USB D− / D+ | 13 / 14 | Native USB Serial/JTAG |
| UART0 TX / RX | 11 / 12 | Routed to header via R56/R59. Not needed this pass |

No dedicated MCLK GPIO was identified on the schematic. The ES8311 is probably clocked from SCLK. **Confirm the clock configuration from Waveshare's ESP-IDF factory code before writing the codec init.**

### I2C addresses

| Device | Address |
|---|---|
| CH32V006 IO expander | 0x24 |
| AXP2101 PMIC | 0x34 |
| FT6336 touch | 0x38 |
| ES8311 codec | 0x18 typical (CE pulled low). Confirm with an I2C scan |
| QMI8658 IMU | 0x6B |

### CH32V006 expander outputs (EXIO)

| EXIO | Signal |
|---|---|
| EXIO0 | LCD_RST |
| EXIO1 | TP_RST (touch reset) |
| EXIO5 | **PA_CTRL (amp enable)** |
| EXIO_PWM | LCD_BL (backlight) |
| EXIO2 / 3 / 4 | QMI_INT1 / AXP_IRQ / RTC_INT |
| EXIO7 | CAM_PWDN (no camera on -EN) |

The command protocol is Waveshare-specific firmware. **Do not use a generic expander library.** Reuse the code from the `01_exio` (Arduino) / `07_exio` (ESP-IDF) examples.

---

## Step 1: Verify hardware before porting

Flash and run these Waveshare Arduino examples in order. Do not start the port until all three pass.

1. **`01_exio`**: CH32 responds at 0x24. Backlight, LCD reset and amp enable all depend on it.
2. **`03_SD_Card`**: SD read/write works. This also confirms GPIO9 is the SD CS.
3. **`07_lvgl_demo`**: known-good ST7796 init and backlight on this board.

Note: flashing replaces the factory demo. It can be restored from `Firmware/ESP32-C5-Touch-LCD-3.5-20260911.bin` in the vendor repo.

---

## Step 2: Port

### 2.1 Remove stale pin definitions
Audit every GPIO define from the XIAO build (buzzer, display, SD, anything else). Several old numbers are now LCD, I2S or I2C lines on this board. Remove or remap all of them. A leftover `pinMode()` on a shared line will silently break a peripheral. **Remove the buzzer GPIO code entirely.**

### 2.2 Init order (mandatory)
1. `Wire.begin(27, 26)`, the I2C bus
2. CH32 expander init (from `01_exio`)
3. Drive EXIO0 to release LCD reset; set EXIO_PWM to turn on the backlight
4. SPI bus + ST7796 init
5. SD init (same SPI bus, CS 9)
6. ES8311 init over I2C, I2S driver, then EXIO5 high (amp on)
7. Radio / detection logic

If the display is initialized before the CH32, the result is a blank screen with no error.

### 2.3 Display
- Resolution changes from 240×320 to **320×480**. Rescale the UI layout and the flashing-border alert. Do not just letterbox.
- Pick a display library that is known to work on the C5 with the ST7796. The lowest-risk option is to take the panel init from `07_lvgl_demo`. LVGL on top is optional. If using TFT_eSPI, it needs C5-specific patches.
- Keep full-screen redraws to a minimum. The SPI bus is shared with SD and the panel has twice the pixels of the old one.

### 2.4 SD card
- Load the OUI/SSID CSV watchlists into **PSRAM at boot**. Do not read SD during scanning.
- Batch log writes and never write in the middle of a display redraw, since both share the bus.

### 2.5 Touch
Do not port the old XPT2046 resistive-touch code. Touch is deferred. If anything references touch, stub it out.

---

## Step 3: Audio

### 3.1 Bring-up
Waveshare has **no Arduino audio example**. The only reference is the ESP-IDF `01_factory` example, which is verified working on this exact board. Port from it:

1. **ES8311 init:** lift the register sequence from Waveshare's factory code, including the clock-source configuration (see the MCLK note above). Espressif's `es8311` component is a secondary reference.
2. **I2S output:** use the `ESP_I2S` library from arduino-esp32 3.x. BCLK 23, WS 10, **DOUT 25**. DIN 24 is optional (mic is unused). Use a sample rate and format that match the codec init.
3. **Amp enable:** set EXIO5 high via the CH32.
4. **Volume:** set the ES8311 DAC volume explicitly. Do not inherit the factory demo's level.

### 3.2 Behaviour
- **Turn the amp on once at boot and leave it on.** Output silence (zeros) between alerts. Toggling PA_CTRL per alert adds latency and makes an audible pop.
- **Generate tones in code** (sine or square into an I2S buffer). No sample files, no SD dependency for audio.
- **Keep the existing alert semantics.** Map the current confidence-graded alert scheme onto tone patterns. Prefer encoding tiers by **cadence** (single chirp, double chirp, continuous warble) over large pitch changes, so every tier is equally audible.
- Audio must not block the scan loop. Run tone generation in its own FreeRTOS task, or feed I2S from a non-blocking writer.

---

## Constraints

- **Do not disable AXP2101 rails** unless the load on that rail is understood. The PMIC powers several board rails.
- **Attach the IPEX antenna** before any radio testing. Range results without it are meaningless.
- **Do not reflash or modify the CH32V006.** Its firmware is factory-programmed, and its source is not in the vendor repo.
- **Pin the Waveshare example version** used. The CH32 protocol could change between production batches.
- Leave the detection logic, OUI/SSID lists, alert decay window and confidence grading **behaviorally unchanged.** This is a port, not a redesign.

---

## Acceptance criteria

- [ ] Boots from USB power with no button press; display comes up
- [ ] Watchlists load from SD at boot
- [ ] Scanning behavior matches the previous C5 build
- [ ] Each alert tier plays a distinct tone pattern through the onboard speaker
- [ ] No audio pop between alerts; no scan-loop stalls during alerts
- [ ] Border-flash visual alert works at 320×480
- [ ] SD logging works without display corruption
- [ ] No references to the old buzzer GPIO or XPT2046 remain

---

## Open items (verify on the bench)

- Meter 0Ω selectors **R55–R59** to confirm how the -EN is strapped (camera vs UART/GPIO/SD-CS). R57 matters now for SD; the others matter later for GPS.
- Confirm the ES8311 I2C address and clock source (scan + factory code).
- Speaker connector pitch: the schematic says PH1.25, the product page says MX1.25. This only matters if the speaker is ever replaced.

## Future (not this pass)

- GPS on UART0 (GPIO11/12 → header pins 26/28) via a header daughterboard
- Daughterboard: 12 V→5 V buck feeding VBUS (header pin 1), GPS, and an unpopulated piezo footprint as an audio fallback
- Capacitive touch via FT6336
