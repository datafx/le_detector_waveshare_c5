// SPDX-License-Identifier: GPL-3.0-only

#include "settings.h"
#include "config.h"
#include "sdcard.h"
#include <ctype.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Storage for the TUNABLE settings declared in config.h, starting at their
// defaults. Nothing reads these before setup(), so static init is enough.
// ---------------------------------------------------------------------------
static const uint8_t k5gDefault[] = { WIFI_5G_CHANNELS_DEFAULT_LIST };
static const uint8_t k2gDefault[] = { WIFI_2G_CHANNELS_DEFAULT_LIST };
uint8_t WIFI_5G_CHANNELS[MAX_SCAN_CHANNELS] = { WIFI_5G_CHANNELS_DEFAULT_LIST };
uint8_t WIFI_5G_CHANNEL_COUNT = sizeof(k5gDefault);
uint8_t WIFI_2G_CHANNELS[MAX_SCAN_CHANNELS] = { WIFI_2G_CHANNELS_DEFAULT_LIST };
uint8_t WIFI_2G_CHANNEL_COUNT = sizeof(k2gDefault);

#define DEFINE_TUNABLE(type, name) type name = name##_DEFAULT;
DEFINE_TUNABLE(uint32_t, WIFI_5G_PHASE_MS)
DEFINE_TUNABLE(uint32_t, WIFI_5G_HOP_MS)
DEFINE_TUNABLE(uint32_t, WIFI_2G_PHASE_MS)
DEFINE_TUNABLE(uint32_t, WIFI_2G_HOP_MS)
DEFINE_TUNABLE(uint32_t, BLE_PHASE_MS)
DEFINE_TUNABLE(uint8_t,  BLE_EVERY_N_SWEEPS)
DEFINE_TUNABLE(uint8_t,  WIFI_2G_EVERY_N_SWEEPS)
DEFINE_TUNABLE(uint32_t, ALERT_HOLD_MS)
DEFINE_TUNABLE(int8_t,   RSSI_WEAK)
DEFINE_TUNABLE(int8_t,   RSSI_STRONG)
DEFINE_TUNABLE(uint32_t, FLASH_PERIOD_SLOW)
DEFINE_TUNABLE(uint32_t, FLASH_PERIOD_FAST)
DEFINE_TUNABLE(int16_t,  MIN_ALERT_RSSI)
DEFINE_TUNABLE(uint32_t, BUZZER_MAX_ON_MS)
DEFINE_TUNABLE(bool,     BUZZER_ENABLED)
DEFINE_TUNABLE(uint32_t, AUDIO_TONE_HZ)
DEFINE_TUNABLE(uint8_t,  AUDIO_DAC_VOLUME)
DEFINE_TUNABLE(bool,     LCD_FLIP)
DEFINE_TUNABLE(uint8_t,  LCD_BACKLIGHT_PCT)
DEFINE_TUNABLE(uint8_t,  LCD_BACKLIGHT_NIGHT_PCT)
DEFINE_TUNABLE(uint32_t, BOOT_SCREEN_MS)
DEFINE_TUNABLE(uint32_t, CAPTURE_MS)

// ---------------------------------------------------------------------------
// Keys. Each numeric key has an allowed range; anything outside it is a bad
// line, so a typo can make the device behave differently but never leaves it
// with an invalid schedule. Units are the ones a user thinks in (seconds,
// dB); the setter converts.
// ---------------------------------------------------------------------------
enum KeyKind : uint8_t { KIND_INT, KIND_BOOL, KIND_MIN_RSSI, KIND_5G_LIST, KIND_2G_LIST };

struct Key {
    const char* name;
    KeyKind     kind;
    int32_t     min, max;            // KIND_INT / KIND_MIN_RSSI
    void      (*set)(int32_t value); // KIND_INT / KIND_BOOL / KIND_MIN_RSSI
};

static const Key KEYS[] = {
    // Speaker
    { "speaker",             KIND_BOOL,   0, 1,      [](int32_t v) { BUZZER_ENABLED = v; } },
    { "volume_db",           KIND_INT,  -60, 32,     [](int32_t v) { AUDIO_DAC_VOLUME = (uint8_t)(0xBF + 2 * v); } },
    { "pitch_hz",            KIND_INT,  200, 8000,   [](int32_t v) { AUDIO_TONE_HZ = v; } },
    { "chirp_ms",            KIND_INT,   10, 1000,   [](int32_t v) { BUZZER_MAX_ON_MS = v; } },
    // Display
    { "day_brightness",      KIND_INT,    1, 100,    [](int32_t v) { LCD_BACKLIGHT_PCT = v; } },
    { "night_brightness",    KIND_INT,    1, 100,    [](int32_t v) { LCD_BACKLIGHT_NIGHT_PCT = v; } },
    { "flip_screen",         KIND_BOOL,   0, 1,      [](int32_t v) { LCD_FLIP = v; } },
    { "boot_screen_seconds", KIND_INT,    0, 30,     [](int32_t v) { BOOT_SCREEN_MS = v * 1000; } },
    // Alerts
    { "rssi_weak",           KIND_INT, -120, 0,      [](int32_t v) { RSSI_WEAK = v; } },
    { "rssi_strong",         KIND_INT, -120, 0,      [](int32_t v) { RSSI_STRONG = v; } },
    { "min_rssi",            KIND_MIN_RSSI, -120, 0, [](int32_t v) { MIN_ALERT_RSSI = v; } },
    { "alert_hold",          KIND_INT,    1, 600,    [](int32_t v) { ALERT_HOLD_MS = v * 1000; } },
    { "flash_slow",          KIND_INT,   50, 5000,   [](int32_t v) { FLASH_PERIOD_SLOW = v; } },
    { "flash_fast",          KIND_INT,   50, 5000,   [](int32_t v) { FLASH_PERIOD_FAST = v; } },
    // Research capture
    { "capture_minutes",     KIND_INT,    1, 30,     [](int32_t v) { CAPTURE_MS = v * 60000; } },
    // Scan schedule
    { "wifi5g_channels",     KIND_5G_LIST, 0, 0,     nullptr },
    { "wifi5g_hop_ms",       KIND_INT,   50, 2000,   [](int32_t v) { WIFI_5G_HOP_MS = v; } },
    { "wifi2g_channels",     KIND_2G_LIST, 0, 0,     nullptr },
    { "wifi2g_hop_ms",       KIND_INT,   50, 2000,   [](int32_t v) { WIFI_2G_HOP_MS = v; } },
    { "wifi2g_every",        KIND_INT,    0, 100,    [](int32_t v) { WIFI_2G_EVERY_N_SWEEPS = v; } },
    { "ble_window_ms",       KIND_INT,  100, 5000,   [](int32_t v) { BLE_PHASE_MS = v; } },
    { "ble_every",           KIND_INT,    0, 100,    [](int32_t v) { BLE_EVERY_N_SWEEPS = v; } },
};

static const char* CONFIG_FILE = SD_DIR "/config.txt";
static const char* FILE_LABEL  = "config.txt";

// 20MHz-wide channels the C5 can listen on. DFS channels (52-144) are
// receive-only here, which is all a passive scanner does.
static bool valid5g(long ch) {
    if (ch >= 36 && ch <= 64)   return ch % 4 == 0;
    if (ch >= 100 && ch <= 144) return ch % 4 == 0;
    if (ch >= 149 && ch <= 165) return (ch - 149) % 4 == 0;
    return false;
}
static bool valid2g(long ch) { return ch >= 1 && ch <= 13; }

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static bool parseInt(const char* s, long* out) {
    errno = 0;
    char* end;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *trim(end) != '\0') return false;
    *out = v;
    return true;
}

static bool parseBool(const char* s, long* out) {
    if (!strcasecmp(s, "on")  || !strcasecmp(s, "yes") || !strcasecmp(s, "true")  || !strcmp(s, "1")) { *out = 1; return true; }
    if (!strcasecmp(s, "off") || !strcasecmp(s, "no")  || !strcasecmp(s, "false") || !strcmp(s, "0")) { *out = 0; return true; }
    return false;
}

// "36, 40, 44" -> list. Rejects the whole line on any bad, duplicate or
// out-of-band channel rather than keeping part of it.
static bool parseChannels(char* s, bool (*valid)(long), uint8_t* out, uint8_t* count, int line, const char* key) {
    uint8_t tmp[MAX_SCAN_CHANNELS];
    uint8_t n = 0;
    for (char* tok = strtok(s, ","); tok; tok = strtok(nullptr, ",")) {
        long ch;
        if (!parseInt(trim(tok), &ch) || !valid(ch)) {
            sdNoteError(FILE_LABEL, line, "%s: '%s' is not a valid channel", key, trim(tok));
            return false;
        }
        for (uint8_t i = 0; i < n; i++) {
            if (tmp[i] == ch) {
                sdNoteError(FILE_LABEL, line, "%s: channel %ld listed twice", key, ch);
                return false;
            }
        }
        if (n == MAX_SCAN_CHANNELS) {
            sdNoteError(FILE_LABEL, line, "%s: more than %u channels", key, (unsigned)MAX_SCAN_CHANNELS);
            return false;
        }
        tmp[n++] = (uint8_t)ch;
    }
    if (n == 0) {
        sdNoteError(FILE_LABEL, line, "%s: no channels", key);
        return false;
    }
    memcpy(out, tmp, n);
    *count = n;
    return true;
}

static bool applyLine(char* key, char* value, int line) {
    for (const Key& k : KEYS) {
        if (strcasecmp(key, k.name) != 0) continue;
        long v;
        switch (k.kind) {
            case KIND_BOOL:
                if (!parseBool(value, &v)) {
                    sdNoteError(FILE_LABEL, line, "%s: '%s' should be on or off", k.name, value);
                    return false;
                }
                k.set(v);
                return true;
            case KIND_MIN_RSSI:
                if (!strcasecmp(value, "off")) {
                    MIN_ALERT_RSSI = MIN_ALERT_RSSI_OFF;
                    return true;
                }
                // fall through: a number
            case KIND_INT:
                if (!parseInt(value, &v) || v < k.min || v > k.max) {
                    sdNoteError(FILE_LABEL, line, "%s: '%s' is not a number from %ld to %ld%s",
                                k.name, value, (long)k.min, (long)k.max,
                                k.kind == KIND_MIN_RSSI ? " (or off)" : "");
                    return false;
                }
                k.set(v);
                return true;
            case KIND_5G_LIST:
                return parseChannels(value, valid5g, WIFI_5G_CHANNELS, &WIFI_5G_CHANNEL_COUNT, line, k.name);
            case KIND_2G_LIST:
                return parseChannels(value, valid2g, WIFI_2G_CHANNELS, &WIFI_2G_CHANNEL_COUNT, line, k.name);
        }
    }
    sdNoteError(FILE_LABEL, line, "unknown setting '%s'", key);
    return false;
}

SettingsResult settingsLoad() {
    SettingsResult r = { false, 0 };
    if (!sdMounted()) return r;

    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) return r;
    r.fileFound = true;

    char buf[256];
    int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        char* hash = strchr(buf, '#');
        if (hash) *hash = '\0';
        char* s = trim(buf);
        if (*s == '\0') continue;

        char* eq = strchr(s, '=');
        if (!eq) {
            sdNoteError(FILE_LABEL, line, "expected 'setting = value'");
            continue;
        }
        *eq = '\0';
        char* key   = trim(s);
        char* value = trim(eq + 1);
        if (*key == '\0' || *value == '\0') {
            sdNoteError(FILE_LABEL, line, "expected 'setting = value'");
            continue;
        }
        if (applyLine(key, value, line)) r.applied++;
    }
    fclose(f);

    // Pairs that only make sense one way round. A bad pair goes back to
    // stock entirely rather than half-applying.
    if (RSSI_WEAK >= RSSI_STRONG) {
        sdNoteError(FILE_LABEL, 0, "rssi_weak (%d) must be below rssi_strong (%d) - both reset to default",
                    RSSI_WEAK, RSSI_STRONG);
        RSSI_WEAK   = RSSI_WEAK_DEFAULT;
        RSSI_STRONG = RSSI_STRONG_DEFAULT;
    }
    if (FLASH_PERIOD_FAST > FLASH_PERIOD_SLOW) {
        sdNoteError(FILE_LABEL, 0, "flash_fast (%lu) must not be above flash_slow (%lu) - both reset to default",
                    (unsigned long)FLASH_PERIOD_FAST, (unsigned long)FLASH_PERIOD_SLOW);
        FLASH_PERIOD_FAST = FLASH_PERIOD_FAST_DEFAULT;
        FLASH_PERIOD_SLOW = FLASH_PERIOD_SLOW_DEFAULT;
    }

    // Each band's phase is always exactly one sweep of its channel list.
    WIFI_5G_PHASE_MS = WIFI_5G_CHANNEL_COUNT * WIFI_5G_HOP_MS;
    WIFI_2G_PHASE_MS = WIFI_2G_CHANNEL_COUNT * WIFI_2G_HOP_MS;
    return r;
}
