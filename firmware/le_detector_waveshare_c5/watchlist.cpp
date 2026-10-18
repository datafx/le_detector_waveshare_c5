// SPDX-License-Identifier: GPL-3.0-only

#include "watchlist.h"
#include "oui_table.h"
#include "ssid_table.h"
#include "sdcard.h"
#include <ctype.h>
#include <esp_heap_caps.h>

static const char*    OUI_FILE  = SD_DIR "/oui.txt";
static const char*    SSID_FILE = SD_DIR "/ssid.txt";
static const size_t   MAX_LABEL = 48;     // longer is almost certainly a mistake
static const uint16_t MAX_ENTRIES = 4000; // ouiTableSize() is a uint16_t

// Loaded tables live in PSRAM for good - internal RAM is for WiFi/BLE.
static void* psRealloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }

static char* psStrdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (d) memcpy(d, s, n);
    return d;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

// Header comment "... version: 2026-10-17" -> true if older than ours.
// No version header at all = can't tell, so not flagged.
static void checkVersion(const char* line, bool* old) {
    const char* v = strstr(line, "version:");
    if (!v) return;
    v += strlen("version:");
    while (*v == ' ') v++;
    char ver[11] = {};
    strncpy(ver, v, 10);
    *old = strcmp(ver, WATCHLIST_VERSION) < 0;   // ISO dates sort as strings
}

// Splits "key, label" at the first comma. Labels may contain commas.
static bool splitLine(char* s, char** first, char** label, const char* file, int line) {
    char* comma = strchr(s, ',');
    if (!comma) {
        sdNoteError(file, line, "expected 'value, label'");
        return false;
    }
    *comma = '\0';
    *first = trim(s);
    *label = trim(comma + 1);
    if (**first == '\0' || **label == '\0') {
        sdNoteError(file, line, "expected 'value, label'");
        return false;
    }
    if (strlen(*label) > MAX_LABEL) {
        sdNoteError(file, line, "label longer than %u characters", (unsigned)MAX_LABEL);
        return false;
    }
    return true;
}

// Reads one watchlist file, calling parse() for every non-comment line.
// Returns false if the file isn't there.
template <typename F>
static bool readLines(const char* path, bool* old, F parse) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buf[256];
    int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        char* s = trim(buf);
        if (*s == '#') { checkVersion(s, old); continue; }
        if (*s == '\0') continue;
        parse(s, line);
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// oui.txt
// ---------------------------------------------------------------------------

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parsePrefix(const char* s, OuiEntry* e, const char* file, int line) {
    uint8_t nib[9];
    uint8_t n = 0;
    for (const char* p = s; *p; p++) {
        if (*p == ':' || *p == '-') continue;
        int v = hexVal(*p);
        if (v < 0 || n == 9) {
            sdNoteError(file, line, "'%s' is not a MAC prefix", s);
            return false;
        }
        nib[n++] = (uint8_t)v;
    }
    if (n != 6 && n != 7 && n != 9) {
        sdNoteError(file, line, "'%s' has %u hex digits - use 6, 7 or 9", s, (unsigned)n);
        return false;
    }
    memset(e->prefix, 0, sizeof(e->prefix));
    for (uint8_t i = 0; i < n; i++) {
        e->prefix[i / 2] |= (i % 2) ? nib[i] : (uint8_t)(nib[i] << 4);
    }
    e->bits = (n == 6) ? 24 : (n == 7) ? 28 : 36;
    // The detector skips multicast and randomized (locally administered)
    // MACs for OUI matching, so such a prefix could never alert.
    if (e->prefix[0] & 0x03) {
        sdNoteError(file, line, "'%s' is a multicast or locally administered prefix - it can never match", s);
        return false;
    }
    return true;
}

static int cmpOui(const void* a, const void* b) {
    return memcmp(((const OuiEntry*)a)->prefix, ((const OuiEntry*)b)->prefix, 5);
}

static bool loadOui(bool* old) {
    OuiEntry* table = nullptr;
    uint16_t  count = 0, cap = 0;
    bool full = false;

    bool found = readLines(OUI_FILE, old, [&](char* s, int line) {
        char *prefix, *label;
        if (!splitLine(s, &prefix, &label, "oui.txt", line)) return;
        OuiEntry e = {};
        if (!parsePrefix(prefix, &e, "oui.txt", line)) return;
        if (count == MAX_ENTRIES) {
            if (!full) sdNoteError("oui.txt", line, "more than %u entries - rest ignored", (unsigned)MAX_ENTRIES);
            full = true;
            return;
        }
        if (count == cap) {
            uint16_t ncap = cap ? cap * 2 : 64;
            if (ncap > MAX_ENTRIES) ncap = MAX_ENTRIES;
            OuiEntry* t = (OuiEntry*)psRealloc(table, ncap * sizeof(OuiEntry));
            if (!t) return;
            table = t;
            cap = ncap;
        }
        e.vendor      = psStrdup(label);
        e.category    = CAT_OTHER;     // not used by anything at runtime
        e.specificity = SPEC_BROAD;    // curation metadata only
        if (!e.vendor) return;
        table[count++] = e;
    });

    // Empty file = no file: keep the built-in table.
    if (!found || count == 0) return false;
    qsort(table, count, sizeof(OuiEntry), cmpOui);   // lookup is a binary search
    ouiSetTable(table, count);
    return true;
}

// ---------------------------------------------------------------------------
// ssid.txt
// ---------------------------------------------------------------------------

static bool parsePattern(char* s, SsidEntry* e, const char* file, int line) {
    size_t len = strlen(s);
    uint8_t stars = 0;
    for (const char* p = s; *p; p++) stars += (*p == '*');

    const char* pattern = nullptr;
    const char* contains = nullptr;
    SsidMatchMode mode;

    if (stars == 0) {                                          // ABC
        mode = SSID_MATCH_EXACT;
        pattern = s;
    } else if (stars == 1 && s[len - 1] == '*' && len > 1) {   // ABC*
        mode = SSID_MATCH_PREFIX;
        s[len - 1] = '\0';
        pattern = s;
    } else if (stars == 2 && s[0] == '*' && s[len - 1] == '*' && len > 2) {   // *ABC*
        mode = SSID_MATCH_CONTAINS;
        s[len - 1] = '\0';
        pattern = s + 1;
    } else if (stars == 2 && s[0] != '*' && s[len - 1] == '*') {   // ABC*DEF*
        char* mid = strchr(s, '*');
        if (mid[1] == '*') goto bad;                           // ABC** - no DEF
        mode = SSID_MATCH_PREFIX_AND_CONTAINS;
        *mid = '\0';
        s[len - 1] = '\0';
        pattern = s;
        contains = mid + 1;
    } else {
        goto bad;
    }

    if (strlen(pattern) > 32 || (contains && strlen(contains) > 32)) {
        sdNoteError(file, line, "pattern longer than an SSID can be (32)");
        return false;
    }
    e->pattern  = psStrdup(pattern);
    e->contains = contains ? psStrdup(contains) : nullptr;
    e->mode     = mode;
    return e->pattern && (!contains || e->contains);

bad:
    sdNoteError(file, line, "'%s': use ABC*, *ABC*, ABC*DEF* or plain ABC", s);
    return false;
}

static bool loadSsid(bool* old) {
    SsidEntry* table = nullptr;
    uint16_t   count = 0, cap = 0;
    bool full = false;

    bool found = readLines(SSID_FILE, old, [&](char* s, int line) {
        char *pattern, *label;
        if (!splitLine(s, &pattern, &label, "ssid.txt", line)) return;
        SsidEntry e = {};
        if (!parsePattern(pattern, &e, "ssid.txt", line)) return;
        if (count == MAX_ENTRIES) {
            if (!full) sdNoteError("ssid.txt", line, "more than %u entries - rest ignored", (unsigned)MAX_ENTRIES);
            full = true;
            return;
        }
        if (count == cap) {
            uint16_t ncap = cap ? cap * 2 : 16;
            if (ncap > MAX_ENTRIES) ncap = MAX_ENTRIES;
            SsidEntry* t = (SsidEntry*)psRealloc(table, ncap * sizeof(SsidEntry));
            if (!t) return;
            table = t;
            cap = ncap;
        }
        e.vendor   = psStrdup(label);
        e.category = CAT_OTHER;
        if (!e.vendor) return;
        table[count++] = e;
    });

    if (!found || count == 0) return false;
    ssidSetTable(table, count);   // linear scan - file order kept
    return true;
}

WatchlistResult watchlistLoad() {
    WatchlistResult r = {};
    if (!sdMounted()) return r;
    bool ouiOld = false, ssidOld = false;
    r.ouiFromSd  = loadOui(&ouiOld);
    r.ssidFromSd = loadSsid(&ssidOld);
    r.ouiOld  = r.ouiFromSd && ouiOld;
    r.ssidOld = r.ssidFromSd && ssidOld;
    return r;
}
