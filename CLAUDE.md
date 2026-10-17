# CLAUDE.md — LE Detector, Waveshare ESP32-C5-Touch-LCD-3.5 port

Session start: **"Read CLAUDE.md and continue."**

Decisions get locked before code, one item at a time. Don't write code for
anything still listed under "Open decisions".

Source of truth for detection logic and watchlists: `datafx/le_detector_c5`
(git remote `upstream`; local checkout at `../le_detector_c5`). Original
porting brief: `PORTING-waveshare-c5-3.5.md`. Where the brief and the
upstream code disagree, **the code wins** (see "Brief corrections" below).

---

## Status (2026-10-17)

**Phases 0-4 complete - working unit.** Detector runs on the Waveshare
board with the rescaled UI and speaker alerts, all user-verified on the
physical unit 2026-10-16. Repo: public at
https://github.com/datafx/le_detector_waveshare_c5 (`origin`), `upstream` =
le_detector_c5. Phase 5 throughput check done (below).
Touch added 2026-10-17 (decisions 9-10): tap = mute/unmute, long press =
day/night backlight - user-verified on the unit.
Remaining: road test + RSSI_STRONG re-check with the IPEX antenna - user
will do these the week of 2026-10-19 - and trying the MagSafe mount.

### Phase 5 throughput check (2026-10-16)

Temporary probe (built with `-DTHROUGHPUT_PROBE`, since removed) reporting
5GHz frames/s and max main-loop gap every 5s. 5GHz chosen because the
2.4GHz test hotspot can't inflate it. Idle 60s vs LEDET-TEST alert 100s:

| | 5GHz frames/s | max loop gap |
|---|---|---|
| Idle | 2.7 - 7.7 | 5ms (no phase switch), 103-119ms (with switch) |
| Alerting | 3.3 - 7.3 | 83-97ms (no switch), 103-136ms (with switch) |

- **No scan throughput loss while alerting.** The capture runs in the WiFi
  task's promiscuous callback, not the main loop, so redraws don't block it.
- 103ms gaps = the WiFi->BLE switch, exactly upstream's measured cost.
- 83-97ms non-switch gaps while alerting = header + content + border
  redraw landing in one uiRender (big PSRAM fillRect + ~230KB flush).
  Comparable to upstream's ILI9341 redraws; user heard no chirp glitches.
  If it ever matters: flush the content box only around changed lines.
- **Upstream behavior found, not a port issue**: a 2.4GHz-only target can't
  hold the alert continuously. 2.4GHz is visited every 8 sweeps (~25s),
  longer than `ALERT_HOLD_MS` (14s), so the alert drops out between
  sightings (seen in the probe log). Identical code upstream. Worth
  knowing for the road test; 5GHz targets (the main use case) refresh every
  2.7s sweep and are unaffected.

### Phase 4 notes (2026-10-16)

- `audio.*`: ES8311 register sequence flattened from esp_codec_dev
  `es8311.c` (vendor copy in `vendor/esp_codec_dev_es8311/`, esp-adf
  @ `e05f54d`, component 1.6.0) for Waveshare's exact config (slave,
  use_mclk=false, Philips 16-bit, 22050Hz). Codec mode is DAC-only (mic
  unused); vendor used BOTH - the register writes come out identical.
- I2S via IDF `i2s_std` directly, **not** the core's ESP_I2S: ESP_I2S
  hard-codes 6x240-frame DMA (~65ms latency at 22050Hz, nearly a whole 70ms
  chirp). Ours: 4x128 frames (~23ms). BCLK started before codec config (it's
  the codec's only clock). TX-only channel; GPIO24 (DIN) left unclaimed.
- Tone task (prio 5) streams continuously: 256-entry sine LUT, 2ms linear
  fade at each edge, phase reset at silence. Amp (EXIO P5) enabled once
  after codec is streaming zeros, never toggled.
- `alertSetBuzzer()` -> `audioSetTone()`; nothing else in `alert.cpp`
  changed. Chirp is level-driven from the main loop exactly as upstream.
- Bench result (LEDET-TEST hotspot from this PC, `nmcli device wifi hotspot
  ifname wlan0 ...`, ch1; wlan0 auto-reconnects to its usual network after; wired
  ethernet keeps the session up. **Caveat (2026-10-17):** this PC's Intel
  iwlwifi beacons irregularly in AP mode - a temp per-channel probe saw 10-38
  other frames per 250ms ch1 dwell but only 0-2 from the laptop, so most
  2.4GHz visits miss it. Firmware fine; use a phone hotspot when a test
  needs repeated alerts): **user - "works perfectly. volume and
  pitch are exactly what I want. no clicks or audio glitches."** No pop at
  boot, no idle hiss, chirps in sync with the border.

### Phase 2/3 notes (2026-10-16)

- Firmware: `firmware/le_detector_waveshare_c5/`. Detection files copied
  verbatim from upstream `78eb8b2` (`detector.*`, `oui_table.*`,
  `ssid_table.*`, `user_oui_table.cpp`, main phase loop).
- New: `board.*` (I2C + CH32; PA_EN deliberately left low),
  `display.*` (Adafruit_GFX `GFXcanvas16` subclass, 300KB frame in PSRAM
  allocated in `begin()` - not the constructor, PSRAM isn't up during static
  init - flushed through a 15KB internal DMA bounce buffer with RGB565 byte
  swap, vendor ST7796 init, MADCTL 0x28 landscape).
- `ui.cpp`: upstream layout scaled 320x240 -> 480x320 (text 2->3, title
  3->4, vendor autosize max 5->7, border 6->9, header 32->44). Each draw
  section flushes only its own rect; border flushed as 4 strips and only
  when its on/off state changes.
- `alert.cpp`: buzzer GPIO removed (`alertSetBuzzer()` was a stub until
  Phase 4 wired it to the speaker). `BUZZER_ACTIVE_LOW` removed from config.
- Build: 1.28MB - 97% of the default 1.25MB app partition, so switched to
  `PartitionScheme=app3M_fat9M_16MB` (40%).
- **Fixed in both repos (2026-10-16)**: `E wifi:failed to post WiFi
  event=43 ret=259` on every channel hop (upstream's notes said only at band
  transitions - wrong). Cause: `detector.cpp` drives `esp_wifi_*` directly
  and never created the default event loop. Fix: one
  `esp_event_loop_create_default()` in `detectorInit()`, applied
  identically here and upstream (detector.cpp stays byte-identical across
  the two repos). Verified: 40s of serial on this board, zero lines.
  Upstream commit `bb16b1b`, compile-checked only (XIAO not connected).

### Phase 1 bench results (2026-10-16), all on core 3.3.11

The board on `/dev/ttyACM0` is the Waveshare (esptool: ESP32-C5 rev v1.2,
32MB flash). Factory demo has been overwritten (restore image in vendor
`Firmware/`).

| Check | Result |
|---|---|
| Vendor `01_exio`, `07_lvgl_demo` compile on 3.3.11 | PASS - decision 4 holds so far |
| USB serial with `CDCOnBoot=cdc` | PASS |
| `bench/01_i2c_scan` | PASS - 0x18 ES8311, 0x24 CH32, 0x34 AXP2101, 0x38 FT6336, 0x51 PCF85063, 0x6B QMI8658, 0x70 SHTC3. ES8311 chip ID `83 11` at 0x18 |
| `01_exio` | PASS - CH32 accepts all writes |
| `07_lvgl_demo` | PASS (user visual) - correct image, colors, orientation; backlight PWM works |
| `03_SD_Card` | skipped - SD deferred (decision 1) |

Bench gotchas:
- `arduino-cli monitor` prints nothing when run without a TTY. Read serial
  with `stty -F /dev/ttyACM0 115200 raw -echo; timeout N cat /dev/ttyACM0`.
- `arduino-cli upload` doesn't take `--libraries` (compile does); upload
  from the existing `--build-path` instead.
- 07_lvgl_demo needs the vendor libs:
  `--libraries vendor/ESP32-C5-Touch-LCD-3.5/example/Arduino-v3.3.10/libraries`.

---

## Decisions — settled (2026-10-16)

1. **Straight port first, no SD.** Goal is a working unit with behavior
   identical to the XIAO build. SD (CSV watchlists into PSRAM, per-run
   logging) is a separate future project after the port is done. Until then
   watchlists stay compiled in, exactly as upstream.
2. **Alert behavior copied exactly.** Single alert state, RSSI-scaled flash
   period (`FLASH_PERIOD_SLOW` 1000ms → `FLASH_PERIOD_FAST` 150ms across
   `RSSI_WEAK` -95 → `RSSI_STRONG` -65), speaker chirp replaces the buzzer
   pulse on the same phase, capped at `BUZZER_MAX_ON_MS` (70ms). No tiers,
   no cadence patterns (upstream locked decision: "no runtime confidence
   tiers - every match alerts identically").
3. **New repo** (this one), with `le_detector_c5` as `upstream` so watchlist
   and detector fixes can be cherry-picked across.
4. **Arduino-ESP32 core 3.3.11** (already installed), not the 3.3.10 the
   vendor examples target. Downgrade only if something actually breaks.
5. Carried from upstream unchanged: BLE scan stays passive; only
   non-randomised MACs matched; three-band 5GHz-dominant phase machine and
   all its timing constants; `ALERT_HOLD_MS` decay window; watchlist data.
6. **Display: PSRAM canvas + vendor esp_lcd init** (Adafruit_GFX
   `GFXcanvas16` subclass), not Arduino_GFX/LovyanGFX/LVGL. Chosen by
   Claude with user's go-ahead 2026-10-16.
7. **Speaker tone locked**: 2400Hz sine, amplitude 16384 (-6dBFS), ES8311
   DAC volume 0xBF (0dB). User-approved by ear 2026-10-16.
8. **2.4GHz alert drop-out accepted** (user, after road testing): 2.4GHz-only
   targets alerting on and off is not a problem. Focus stays on 5GHz. Revisit
   only if it becomes a concern.
9. **Touch = whole-screen button, no coordinates** (user, 2026-10-17). Tap
   toggles mute on the active alert (`alertToggleMute()`); the mute keeps
   upstream's `alertMute()` lifetime - no-op while clear, dropped when the
   alert clears, so the next alert is always audible. Header reads
   `ALERT MUTED` while muted. Taps need two consecutive 20ms "touched"
   reads (stray-report filter).
10. **Brightness = long press (800ms) toggles day 100% / night 2%**
    (Claude's pick, user left it open 2026-10-17). Not persisted: every
    boot starts at day level, since a dim screen after a daytime reboot is
    worse than a bright one after a night reboot. No light sensor on board,
    so no auto-dimming.

## Open decisions

- `RSSI_STRONG` re-check against the IPEX pigtail + Alfa (road-test data).
- Windshield mounting: MagSafe-style magnetic mount ordered to try first
  (2026-10-17). Check: ring clears the rear RP-SMA, mount is non-charging
  (a Qi coil heats the steel ring), adhesive holds in summer heat. Ball
  stud through the empty camera hole ruled out for now - not enough room
  inside the case behind it.

---

## Brief corrections (found by reading code, 2026-10-16)

- **No SD code exists upstream.** `PIN_SD_CS` is defined but never read; SD
  was deferred 2026-09-18. Not part of this port (decision 1).
- **No alert tiers exist upstream.** See decision 2.
- **No touch code exists upstream.** XPT2046 was never wired. Nothing to
  strip.
- **EXIO numbering in the brief is wrong.** Vendor BSP
  (`01_factory/.../esp32_c5_touch_lcd_3_5.c` `bsp_io_expander_init`) and
  `07_lvgl_demo/Board_IO.h` agree: **P0 = touch reset, P1 = LCD reset**,
  P5 = amp (PA) enable, PWM = backlight.
- **No MCLK.** BSP: `BSP_I2S_MCLK = GPIO_NUM_NC`, ES8311 config
  `use_mclk = false` → codec clocks from BCLK. Vendor default I2S format:
  Philips std, 16-bit, mono, 22050 Hz, ESP32 is master.
- **32MB flash isn't selectable** on core 3.3.11's `ESP32C5 Dev Module`
  (max 16MB). Using `FlashSize=16M` on 32MB hardware is safe; just leaves
  the top half unused.

---

## Hardware

Complete equipment list (also in README.md):

- Waveshare ESP32-C5-Touch-LCD-3.5-EN, SKU 35416 (ESP32-C5-WROOM-1U-N32R8,
  32MB flash, 8MB PSRAM, IPEX antenna socket; no battery, no camera)
- Right-angle IPEX (U.FL) -> **RP-SMA** pigtail - the Alfa is RP-SMA, so a
  plain-SMA pigtail won't mate
- Alfa APA-M25 dual-band antenna (same as upstream)
- USB-C cable

Assembly (user, 2026-10-17; in README): RP-SMA bulkhead goes through a
6.5mm hole drilled in the center of the empty battery bay (feet off, 4
screws). The camera hole has no clearance - case won't close.

**Attach the antenna before any radio testing.** Full board detail in the
brief and `vendor/.../schematic/`.

### ESP32-C5 GPIO

| Function | GPIO |
|---|---|
| I2C SDA / SCL | 27 / 26 (CH32 0x24, AXP2101 0x34, ES8311 0x18, FT6336 0x38 - see Phase 1 scan) |
| SPI SCK / MOSI / MISO | 6 / 7 / 2 (LCD + SD shared) |
| LCD CS / DC | 8 / 5 |
| SD CS | 9 (unused this pass) |
| I2S BCLK / WS / DOUT / DIN | 23 / 10 / 25 / 24 |
| BOOT button | 28 |
| Touch INT | 3 (unused) |

### Every upstream pin collides on this board

| Upstream (XIAO) | GPIO | Here |
|---|---|---|
| TFT_DC | 7 | SPI MOSI |
| TFT_CS | 25 | I2S DOUT |
| TFT_RST | 23 | I2S BCLK |
| SD_CS | 24 | I2S DIN |
| SCK / MISO / MOSI | 8 / 9 / 10 | LCD CS / SD CS / I2S WS |
| BUZZER | 1 | free |

`config.h` pin block gets replaced wholesale, not patched.

### CH32V006 expander (0x24) - Waveshare-specific protocol

Registers: `0x02` mode (bit=1 → output), `0x03` output, `0x04` input,
`0x05` PWM (backlight, 0-255; vendor caps at 97%), `0x06` ADC.
Note: `01_exio` writes 16-bit words; `07_lvgl_demo` writes single bytes to
the same registers, matching the IDF driver. Use the `07_lvgl_demo`
(`Board_IO.cpp`) form - it's the one proven to bring up LCD + amp.
Vendor init: mode=0xFF, out=0, pwm=0, mode=P0|P1|P5, then pulse P0|P1
high→low→high with P5 high.
**Never reflash the CH32.**

### ST7796 panel init (from `07_lvgl_demo`, = vendor IDF BSP)

SPI mode 0, vendor runs 60MHz pclk. Sequence: `01` swreset, `11` sleep out,
`36`=0x48, `3A`=0x55, `F0`=C3, `F0`=96, `B4`=01, `B7`=C6, `E8`=8 bytes,
`C1`=06, `C2`=A7, `C5`=18, `E0`/`E1` gamma, `F0`=3C, `F0`=69, `21`
inversion on, `29` display on. Exact bytes in
`vendor/.../07_lvgl_demo/07_lvgl_demo.ino` `lcd_begin()`. Native
orientation 320×480 portrait; we run landscape 480×320 (upstream used
`setRotation(1)`).

---

## Toolchain

```
FQBN: esp32:esp32:esp32c5:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
```

- `CDCOnBoot=cdc` because USB is the C5's native USB Serial/JTAG on
  GPIO13/14; without it `Serial` goes to UART0 (GPIO11/12), which isn't on
  the USB port. (Verified.)
- Build/flash with `arduino-cli`, same as upstream.
- Vendor repo: `vendor/ESP32-C5-Touch-LCD-3.5` (gitignored, reference only),
  **pinned at `04e6134cf3e37309b2bec9915189efeace1161bc`** (2026-09-30).
  ES8311 driver is Espressif `esp_codec_dev` (a managed component, not in
  the vendor repo and not shipped in the Arduino core). Its `es8311.c` is
  saved in `vendor/esp_codec_dev_es8311/` (gitignored, esp-adf @ `e05f54d`)
  and flattened into `audio.cpp`.
- Only library dependency: `Adafruit GFX Library` (+ BusIO). Everything
  else (BLE, i2s_std, esp_lcd) ships with the core.
- Factory firmware restore: `vendor/.../Firmware/ESP32-C5-Touch-LCD-3.5-20260911.bin`.

---

## Plan

**Phase 0 — Setup.** Repo, vendor clone, this file. *(done)*

**Phase 1 — Bench verification.** *(done - see Status)* `01_exio`, `07_lvgl_demo`, plus an I2C scan sketch (confirm 0x18
for ES8311). All must pass on 3.3.11 before porting - also our first test
of decision 4.

**Phase 2 — Board layer** *(done)* (`board.h/.cpp`): pins, `Wire.begin(27, 26)`,
CH32 driver from `Board_IO.cpp`, mandatory init order: I2C → CH32 → LCD
reset + backlight → SPI/display → audio (codec, I2S, amp on) → radio.

**Phase 3 — Display.** *(done)* Copy detection files verbatim from upstream
(`detector.*`, `oui_table.*`, `ssid_table.*`, `user_oui_table.cpp`, main
phase loop). Swap the display driver (decision 6), rescale `ui.cpp` to
480×320: border, header, vendor-name autosize, RSSI bar, boot screen. Keep
partial-redraw logic and `setTextWrap(false)`.

**Phase 4 — Audio** *(done)* (`audio.cpp`): ES8311 init, IDF `i2s_std` TX on 23/10/25
(not ESP_I2S - see Phase 4 notes),
explicit DAC volume, amp on once at boot and left on. FreeRTOS task streams
continuously: tone when "buzzer on", zeros otherwise, few-ms fade edges to
avoid clicks. `alert.cpp` changes only inside `alertSetBuzzer()`; all
buzzer GPIO code removed.

**Phase 5 — Integration.** Throughput idle vs alerting *(done - no loss)*.
End-to-end LEDET-TEST hotspot check *(done)*. **Still to do (user, week of
2026-10-19):** road test, and re-check `RSSI_STRONG` (-65, calibrated on the
XIAO's antenna) against the IPEX pigtail + Alfa in the field.

**Watchlist changes** go upstream first (it has `test/test_oui.cpp`), then
get copied here - `oui_table.*`, `ssid_table.*`, `detector.*` should stay
byte-identical across the two repos.

**Future (not this project phase):** SD config/logging (+ an on-screen
detection history), optional GPS.

**GPS purpose - settled by user 2026-10-17, don't re-ask:** (1) logging -
lat/lon on every hit in the SD log; (2) **lockout areas** - suppress alerts
inside user-defined zones to stop repeat false alarms from known fixed gear.
Requirements: optional (device works identically with no GPS attached -
auto-detected, no setting), self-contained module with built-in antenna,
plugs into the back header J3 with no custom PCB. GPS work is done with/after
SD, since both uses need storage. J3 pinout (schematic): 1 VBUS, 29/30 GND,
31/32 3V3, 25/27 I2C SCL/SDA (shared bus), 26/28 U0TXD/U0RXD (GPIO11/12 via
0R selectors R56/R59 - camera vs header, unverified on -EN; check with a
26<->28 loopback), 15/17 GPIO0/1 (via R55/R58, same caveat).

**GPS hardware - chosen option (2026-10-17, not yet bought):** I2C, not
UART - sidesteps the unverified R56/R59 straps and the ROM boot log on
UART0, and allows auto-detect by probing the bus at boot.
- SparkFun GPS Breakout - Chip Antenna, SAM-M10Q (Qwiic), GPS-21834, ~$52.
  u-blox M10, built-in chip antenna, backup battery, I2C addr 0x42 (no
  clash with the board's 0x18/0x24/0x34/0x38/0x51/0x6B/0x70).
- SparkFun Flexible Qwiic Cable - Breadboard Jumper (4-pin), PRT-17912.
  Male pins into J3: 25 SCL, 27 SDA, 29 GND, 31 3V3 (adjacent, one row).
- Budget alternative: Adafruit Mini GPS PA1010D (STEMMA QT, I2C 0x10,
  ~$30; weaker antenna). UART-only modules (Beitian BN-220 etc.) rejected.
- Breakout pull-ups parallel the board's; cut its jumper if bus issues.
  First step when it arrives: a bench sketch reporting address + fix. Touch mute/brightness done (decisions
9-10), so the BOOT-button mute is no longer needed.

### Acceptance (this pass)

- [x] Boots from USB with no button press; display comes up
- [ ] Scanning behavior matches the upstream C5 build (bench: alerts fire
      on LEDET-TEST; road test still to do)
- [x] Alert chirp through the speaker, synced to the border flash, rate
      scaling with RSSI exactly as upstream
- [x] No audio pop between chirps (user-verified); no scan throughput loss
      during alerts (measured, see Phase 5 notes)
- [x] Border-flash alert works at 480×320
- [x] No references to the buzzer GPIO remain
