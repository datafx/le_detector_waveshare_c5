#!/usr/bin/env python3
"""Check that the example SD files match the firmware's built-in data.

- config.txt promises that uncommenting a line gives stock behaviour, so
  every value in it must equal the default in config.h.
- oui.txt / ssid.txt are generated from the built-in tables in
  oui_table.cpp / ssid_table.cpp (disabled rows included as # lines), so
  copying them to the card behaves exactly like the built-in lists.

Run after changing a default, a watchlist row, or an example file. Exits
non-zero on any mismatch. Developer tool only - nothing here runs on the
device.

    python3 tools/check_sd_examples.py          # check
    python3 tools/check_sd_examples.py --write  # regenerate oui.txt/ssid.txt

When watchlist rows change, also bump WATCHLIST_VERSION in watchlist.h
before regenerating, so older card copies get flagged on the boot screen.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_H = ROOT / "firmware/le_detector_waveshare_c5/config.h"
SETTINGS_CPP = ROOT / "firmware/le_detector_waveshare_c5/settings.cpp"
FW = ROOT / "firmware/le_detector_waveshare_c5"
EXAMPLE = ROOT / "sd_card/ledet/config.txt"
OUI_EXAMPLE = ROOT / "sd_card/ledet/oui.txt"
SSID_EXAMPLE = ROOT / "sd_card/ledet/ssid.txt"

# config.txt key -> (config.h name, function turning the config.h default
# into the value a user writes in config.txt).
KEYS = {
    "speaker":             ("BUZZER_ENABLED",          lambda v: "on" if v else "off"),
    "volume_db":           ("AUDIO_DAC_VOLUME",        lambda v: str((v - 0xBF) // 2)),
    "pitch_hz":            ("AUDIO_TONE_HZ",           str),
    "chirp_ms":            ("BUZZER_MAX_ON_MS",        str),
    "day_brightness":      ("LCD_BACKLIGHT_PCT",       str),
    "night_brightness":    ("LCD_BACKLIGHT_NIGHT_PCT", str),
    "flip_screen":         ("LCD_FLIP",                lambda v: "on" if v else "off"),
    "boot_screen_seconds": ("BOOT_SCREEN_MS",          lambda v: str(v // 1000)),
    "rssi_weak":           ("RSSI_WEAK",               str),
    "rssi_strong":         ("RSSI_STRONG",             str),
    "min_rssi":            ("MIN_ALERT_RSSI",          lambda v: "off" if v == -128 else str(v)),
    "alert_hold":          ("ALERT_HOLD_MS",           lambda v: str(v // 1000)),
    "flash_slow":          ("FLASH_PERIOD_SLOW",       str),
    "flash_fast":          ("FLASH_PERIOD_FAST",       str),
    "capture_minutes":     ("CAPTURE_MS",              lambda v: str(v // 60000)),
    "wifi5g_channels":     ("WIFI_5G_CHANNELS",        None),
    "wifi5g_hop_ms":       ("WIFI_5G_HOP_MS",          str),
    "wifi2g_channels":     ("WIFI_2G_CHANNELS",        None),
    "wifi2g_hop_ms":       ("WIFI_2G_HOP_MS",          str),
    "wifi2g_every":        ("WIFI_2G_EVERY_N_SWEEPS",  str),
    "ble_window_ms":       ("BLE_PHASE_MS",            str),
    "ble_every":           ("BLE_EVERY_N_SWEEPS",      str),
}


def c_value(text):
    text = text.strip()
    if text in ("true", "false"):
        return text == "true"
    if text == "MIN_ALERT_RSSI_OFF":
        return -128
    return int(text, 0)


def firmware_defaults():
    src = CONFIG_H.read_text()
    out = {}
    for name, value in re.findall(r"^TUNABLE\(\s*\w+\s*,\s*(\w+)\s*,\s*([^)]+)\)", src, re.M):
        out[name] = c_value(value)
    for name, items in re.findall(r"^#define (\w+)_DEFAULT_LIST (.+)$", src, re.M):
        out[name] = ", ".join(i.strip() for i in items.split(","))
    return out


def example_values():
    out = {}
    for line in EXAMPLE.read_text().splitlines():
        m = re.match(r"^#\s*([a-z0-9_]+)\s*=\s*(.+?)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def watchlist_version():
    m = re.search(r'#define WATCHLIST_VERSION "([0-9-]+)"', (FW / "watchlist.h").read_text())
    return m.group(1)


def oui_rows():
    """(enabled, prefix text, label) for every row in OUI_TABLE, in order."""
    src = (FW / "oui_table.cpp").read_text()
    excluded = set(re.findall(r"^#define (EXCLUDE_VENDOR_\w+)", src, re.M))
    body = src[src.index("OUI_TABLE[] = {"):]
    body = body[:body.index("};")]
    rows, block = [], None
    for line in body.splitlines():
        m = re.match(r"#ifndef (EXCLUDE_VENDOR_\w+)", line.strip())
        if m:
            block = m.group(1)
            continue
        if line.strip() == "#endif":
            block = None
            continue
        m = re.match(r'\s*(//)?\s*\{ \{ ([^}]+) \}, (\d+), "([^"]+)"', line)
        if not m:
            continue
        b = [int(x, 16) for x in m.group(2).split(",")]
        digits = {24: 6, 28: 7, 36: 9}[int(m.group(3))]
        hexs = "".join(f"{x:02X}" for x in b)[:digits]
        prefix = ":".join(hexs[i:i + 2] for i in range(0, len(hexs), 2))
        enabled = not m.group(1) and block not in excluded
        rows.append((enabled, prefix, m.group(4)))
    return rows


def ssid_rows():
    """(enabled, pattern text, label) for every row in SSID_TABLE, in order."""
    src = (FW / "ssid_table.cpp").read_text()
    body = src[src.index("SSID_TABLE[] = {"):]
    body = body[:body.index("};")]
    rows = []
    for m in re.finditer(r'^\s*(//)?\s*\{ "([^"]+)",\s*(SSID_MATCH_\w+),\s*"([^"]+)",\s*\w+,\s*(nullptr|"[^"]*")\s*\}',
                         body, re.M):
        pat, mode, contains = m.group(2), m.group(3), m.group(5).strip('"')
        text = {"SSID_MATCH_PREFIX": f"{pat}*",
                "SSID_MATCH_CONTAINS": f"*{pat}*",
                "SSID_MATCH_PREFIX_AND_CONTAINS": f"{pat}*{contains}*",
                "SSID_MATCH_EXACT": pat}[mode]
        rows.append((not m.group(1), text, m.group(4)))
    return rows


OUI_HEADER = """\
# le_detector OUI watchlist - version: {version}
#
# One entry per line:  prefix, label
#   prefix = the first 6, 7 or 9 hex digits of a MAC address (colons
#            optional), e.g. 00:25:DF. Longer prefixes are the IEEE MA-M
#            (/28) and MA-S (/36) blocks.
#   label  = what the screen shows on a match.
# Lines starting with # are ignored. Entries shipped as "# ..." are in the
# list but disabled by default (too many false positives) - remove the # to
# turn one on, add a # to turn one off.
#
# Copying this file to the card's /ledet folder REPLACES the firmware's
# built-in OUI list. As shipped it matches the built-in list exactly. Delete
# the file (or leave no active lines) to go back to the built-in list.
# "version" above tells the firmware how old this copy is: newer firmware
# with a newer built-in list shows "(old)" on the boot screen.

"""

SSID_HEADER = """\
# le_detector SSID watchlist - version: {version}
#
# One entry per line:  pattern, label
#   ABC*      network name starts with ABC
#   *ABC*     network name contains ABC
#   ABC*DEF*  starts with ABC, and DEF appears somewhere after that
#   ABC       network name is exactly ABC
# Matching ignores upper/lower case. A pattern can't contain a comma.
# Keep patterns specific: a short one like *PSP* matched pet stores and
# homes in real survey data.
# Lines starting with # are ignored; "# ..." entries are examples or
# disabled by default.
#
# Copying this file to the card's /ledet folder REPLACES the firmware's
# built-in SSID list. As shipped it matches the built-in list exactly.
# Delete the file (or leave no active lines) to go back to the built-in list.

"""


def render(header, rows):
    out = header.format(version=watchlist_version())
    for enabled, key, label in rows:
        out += ("" if enabled else "# ") + f"{key}, {label}\n"
    return out


def check_watchlists(write):
    problems = []
    for path, text in ((OUI_EXAMPLE, render(OUI_HEADER, oui_rows())),
                       (SSID_EXAMPLE, render(SSID_HEADER, ssid_rows()))):
        if write:
            path.write_text(text)
            print(f"wrote {path.relative_to(ROOT)}")
        elif not path.exists() or path.read_text() != text:
            problems.append(f"{path.relative_to(ROOT)}: out of date with the built-in table "
                            "(run with --write)")
    return problems


def main():
    if "--write" in sys.argv:
        check_watchlists(write=True)
    defaults = firmware_defaults()
    example = example_values()
    firmware_keys = set(re.findall(r'^\s*\{ "([a-z0-9_]+)",', SETTINGS_CPP.read_text(), re.M))
    problems = []

    for key in sorted(firmware_keys - set(KEYS)):
        problems.append(f"{key}: in settings.cpp but not in this script's KEYS table")
    for key in sorted(set(KEYS) - firmware_keys):
        problems.append(f"{key}: in this script but not in settings.cpp")

    for key, (name, conv) in KEYS.items():
        if key not in example:
            problems.append(f"{key}: missing from {EXAMPLE.relative_to(ROOT)}")
            continue
        want = defaults[name] if conv is None else conv(defaults[name])
        if example[key] != want:
            problems.append(f"{key}: example says '{example[key]}', firmware default is '{want}'")
    for key in sorted(set(example) - set(KEYS)):
        problems.append(f"{key}: in the example file but not a known setting")

    problems += check_watchlists(write=False)

    if problems:
        print("SD example files are out of sync:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"OK - {len(KEYS)} config.txt settings match config.h defaults; "
          f"oui.txt ({sum(r[0] for r in oui_rows())} active) and "
          f"ssid.txt ({sum(r[0] for r in ssid_rows())} active) match the built-in tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
