# LE Gear Detector — Waveshare ESP32-C5-Touch-LCD-3.5

Passive 5GHz/2.4GHz WiFi + BLE scanner for law-enforcement equipment, on a
single off-the-shelf board. A watchlist match flashes a red screen border
and chirps the onboard speaker, both faster as the signal gets stronger.
It only listens and never transmits.

This is a port of [`datafx/le_detector_c5`](https://github.com/datafx/le_detector_c5)
(Seeed XIAO ESP32-C5 + separate display + buzzer on a carrier PCB) to the
Waveshare board, which has the display, speaker and USB-C built in. No
wiring, soldering or carrier board needed. Detection logic and watchlists
are identical to the XIAO build.

## Hardware

| Part | Notes | Link |
|---|---|---|
| Waveshare ESP32-C5-Touch-LCD-3.5 | ESP32-C5-WROOM-1U module (dual-band WiFi 6, BLE 5, IPEX antenna socket), 3.5" 480×320 display, onboard speaker, USB-C. Built and tested on the -EN version without battery or camera (SKU 35416); neither is used | [Waveshare](https://www.waveshare.com/esp32-c5-touch-lcd-3.5.htm) |
| Right-angle IPEX (U.FL) → **RP-SMA** pigtail | Connects the board's antenna socket to the external antenna. Must be **RP-SMA**, not plain SMA, to mate with the Alfa | search "U.FL to RP-SMA right angle pigtail" |
| Alfa APA-M25 dual-band antenna | 8 dBi @2.4GHz / 10 dBi @5GHz, RP-SMA | [Alfa Network](https://alfa-network.eu/apa-m25) |
| USB-C cable | Power + programming | |

**Attach the antenna before powering on.** Range is meaningless without it.

## Assembly: fitting the antenna connector

The board's antenna socket is inside the case, so the pigtail's RP-SMA end
has to be mounted through the back. Unplug USB first.

1. **Open the case.** Peel off the rubber feet on the back, then remove the
   four screws underneath them.
2. **Drill a 6.5mm hole in the center of the battery bay.** On the version
   without a battery that bay is empty, so the connector and nut have room
   there. Take the board out of the back shell before drilling so no
   swarf ends up on it, and clean the hole afterwards.
3. **Install the pigtail.** Push the RP-SMA bulkhead through the hole from
   the inside and tighten its nut on the outside. Snap the right-angle IPEX
   (U.FL) end straight down onto the module's antenna socket. Press it on
   squarely; it's small and easy to damage if pushed at an angle.
4. **Route the cable** so it lies flat and isn't pinched by the case, then
   close the case, refit the four screws and stick the feet back on.
5. **Screw the Alfa antenna onto the RP-SMA connector.**

**Don't use the camera hole for the connector.** It looks like a ready-made
opening, but there isn't enough clearance behind it: the case won't close
with the connector mounted there.

## Build & flash

PlatformIO doesn't support the ESP32-C5 yet, so this builds with the
Arduino IDE or `arduino-cli`.

### Arduino IDE

1. **Install the ESP32 board package.** File → Preferences → Additional
   Boards Manager URLs, add:

       https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

   Then Tools → Board → Boards Manager, search "esp32", install **esp32 by
   Espressif Systems** version **3.3.11** (the version this is built and
   tested against).

2. **Select the board and settings** under Tools:

   | Setting | Value |
   |---|---|
   | Board | **ESP32C5 Dev Module** |
   | Flash Size | **16MB** (the board has 32MB; 16MB is the largest option and works fine) |
   | Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
   | PSRAM | **Enabled** |
   | USB CDC On Boot | **Enabled** |

   These matter: the firmware is too big for the default partition, the
   display buffer lives in PSRAM, and without USB CDC there's no serial
   output over USB.

3. **Install `Adafruit GFX Library`** via Library Manager (accept its
   `Adafruit BusIO` dependency). BLE, I2S and the display driver come with
   the esp32 core, so nothing else is needed.

4. **Open** `firmware/le_detector_waveshare_c5/le_detector_waveshare_c5.ino`,
   select the board's port, and upload.

### arduino-cli

    arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
    arduino-cli core update-index
    arduino-cli core install esp32:esp32@3.3.11
    arduino-cli lib install "Adafruit GFX Library"

    FQBN=esp32:esp32:esp32c5:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
    arduino-cli compile --fqbn $FQBN firmware/le_detector_waveshare_c5
    arduino-cli upload  --fqbn $FQBN -p /dev/ttyACM0 firmware/le_detector_waveshare_c5

Flashing replaces Waveshare's factory demo. To restore it, use the image in
the `Firmware/` folder of
[Waveshare's repo](https://github.com/waveshareteam/ESP32-C5-Touch-LCD-3.5).

### If the board doesn't show up, or upload fails

The C5 uses its own native USB, so no CH340/CP210x driver is needed. Things
that went wrong during bring-up:

- **Linux: no `/dev/ttyACM0`.** Check `lsmod | grep cdc_acm`; if it's
  missing, `sudo modprobe cdc_acm`. Your user also needs access to the port:
  group `dialout` on Debian/Ubuntu, `uucp` on Arch.
- **"Invalid head of packet" or "possible serial noise or corruption".**
  Usually a USB hub. Plug directly into the computer first.
- **No serial monitor output.** `arduino-cli monitor` can print nothing even
  when the board is running. `cat /dev/ttyACM0` works. Normal operation
  prints nothing anyway; the display is the status output.

## What you'll see

A boot screen with watchlist sizes for 5 seconds, then the main screen:

- **Header:** `ALL CLEAR`, or a red `** ALERT **`, plus which radio phase
  is listening right now (5GHz WiFi / 2.4GHz WiFi / BLE).
- **On a match:** the vendor name, what matched and on which band (e.g.
  "via 5GHz WiFi OUI" or "SSID beacon (2.4GHz): PSP-MVR"), and a signal
  strength bar. The border flashes red with a speaker chirp on each flash,
  faster as the signal gets stronger.
- The alert holds for 14 seconds after the last sighting, then clears.

### Touchscreen

The whole screen is one button, so there's nothing to aim at while driving:

- **Tap** during an alert to mute the speaker. The border keeps flashing and
  the header changes to `ALERT MUTED`. Tap again to unmute. The mute only
  lasts for that alert: once it clears, the next alert chirps again. Taps
  while nothing is alerting do nothing, so you can't mute ahead of time.
- **Press and hold** (about 1 second) to switch between day (full) and night
  (2%) brightness. It changes while your finger is still down. Every boot
  starts at full brightness.

To check the alert path without real gear, broadcast a WiFi network named
`LEDET-TEST` from a phone or laptop. It's a built-in test entry. A phone
hotspot works best: some laptop WiFi cards beacon irregularly in hotspot
mode, so a 2.4GHz laptop hotspot may alert once and then only now and then.

## Watchlist

**62 OUI entries** across law-enforcement radio, body/dash cam, vehicle
router/MDT, radar/lidar, ALPR and lightbar equipment, plus **6 SSID
patterns** (5 real + the `LEDET-TEST` test entry). The full list,
provenance and false-positive reasoning are comments in the source files:

- [`oui_table.cpp`](firmware/le_detector_waveshare_c5/oui_table.cpp): MAC
  OUI watchlist
- [`ssid_table.cpp`](firmware/le_detector_waveshare_c5/ssid_table.cpp):
  SSID watchlist (beacons, probe requests, probe responses)

Disabled by default after field testing showed more false positives than
real hits (commented out, not deleted): **CradlePoint** (via the
`EXCLUDE_VENDOR_CRADLEPOINT` toggle at the top of `oui_table.cpp`),
**Peplink**, **Pepwave**, **Vantiva**, **PRO-VISION**.

To try a local addition, add it to `user_oui_table.cpp`. Watchlist changes
meant to be kept go into
[`le_detector_c5`](https://github.com/datafx/le_detector_c5) first, which
has the watchlist unit test (`test/test_oui.cpp`), then get copied here.

### SSID watchlist

| Pattern | Label | Category |
|---|---|---|
| `PSP-MVR`    | PSP in-car video          | Body/car cam |
| `SP-MVR`     | PSP paired virtual AP     | Other |
| `PSP-TEST`   | PSP facility              | Other |
| `PSPWLAN`    | PSP facility              | Other |
| `PSP_UC`     | PSP facility voice        | Other |
| `LEDET-TEST` | TEST/DEMO — not real gear | Other |

All PSP rows are prefix matches on purpose: a looser `CONTAINS "PSP"` rule
matched unrelated civilian networks (a pet store, a retailer, a private
residence) in real survey data. `SP-MVR` and `PSP-MVR` are separate rows
for the same reason.

Confirmed via WiGLE survey data at three Pennsylvania State Police barracks
in three troops, about 280km apart, all on matching Cisco hardware.
`PSP-MVR`, `SP-MVR` and `PSP-TEST` were seen at all three, with matching
BSSID ranges, so this is one statewide deployment and not a coincidence.
`PSPWLAN` and `PSP_UC` were seen at one site only.

## How detection works

The ESP32-C5 has one radio: it can listen on 5GHz WiFi, 2.4GHz WiFi or BLE,
but only one at a time. So the firmware time-slices, weighted towards 5GHz
based on real drive data:

- **5GHz** sweeps continuously (9 non-DFS channels, about 2.7s per sweep).
  It's the only long-range, continuous signal and the main detection path.
- **BLE** gets a 400ms window every 3rd sweep. BLE is short-range, useful
  for a stopped or slow-moving encounter.
- **2.4GHz** gets a quick check of channels 1/6/11 every 8th sweep (about
  every 25s). On the road it mostly catches fixed facility APs.

Switching radios costs about 103ms into BLE and 9ms back (measured in
[`le_detector_c5`](https://github.com/datafx/le_detector_c5/tree/main/firmware/05_radio_switch_timing),
and the same on this board). The every-N-sweeps values in `config.h` are
starting points to tune, not a measured optimum.

## Known limits

- **One band at a time.** While one phase is listening, the other two are
  deaf.
- **2.4GHz-only targets alert on and off.** 2.4GHz is checked about every
  25s, longer than the 14s alert hold, so the alert can clear between
  sightings. 5GHz targets are re-seen every sweep and stay alerted.
- **MAC randomisation.** Only public BLE addresses and non-randomised WiFi
  MACs have a real vendor OUI, so randomised addresses are skipped. SSID
  matching isn't affected.
- **PSP SSID rows mean barracks proximity**, not confirmed mobile gear.
  They're fixed building APs; a road hit would need a cruiser probing for
  them, which hasn't been confirmed in the field.
- **Directed probe requests are fading.** Modern phones mostly send
  wildcard probes with no SSID, so probe-based SSID matching mainly catches
  older or embedded devices. Beacon matching is unaffected.
- **No SD card or GPS yet.** The board's microSD slot isn't used; the
  watchlists are compiled in and nothing is logged.

## Tuning

Everything is in `firmware/le_detector_waveshare_c5/config.h`: band and
channel lists, sweep cadence, the RSSI range that maps to slow/fast
flashing (`RSSI_WEAK`/`RSSI_STRONG`), flash rates, alert hold time, speaker
pitch and volume, day/night backlight levels (`LCD_BACKLIGHT_PCT`,
`LCD_BACKLIGHT_NIGHT_PCT`), long-press time (`TOUCH_LONG_PRESS_MS`), and `BUZZER_ENABLED` to silence the speaker for bench
testing.

## License

GPLv3 (see `LICENSE`), matching upstream
[`le_detector_c5`](https://github.com/datafx/le_detector_c5) and
[`le_detector`](https://github.com/datafx/le_detector).

Exceptions, each marked in its file header:
- [`detector.cpp`](firmware/le_detector_waveshare_c5/detector.cpp) has
  portions adapted from [nyanBOX](https://github.com/jbohack/nyanBOX)
  under MIT.
- The ST7796 init sequence, IO-expander protocol and touch register use
  (`display.cpp`, `board.cpp`, `touch.cpp`) are adapted from
  [Waveshare's examples](https://github.com/waveshareteam/ESP32-C5-Touch-LCD-3.5),
  and the ES8311 register sequence (`audio.cpp`) from Espressif's
  esp_codec_dev. Both are Apache-2.0.
