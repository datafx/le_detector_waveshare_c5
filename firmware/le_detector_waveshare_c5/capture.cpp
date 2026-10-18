// SPDX-License-Identifier: GPL-3.0-only

#include "capture.h"
#include "config.h"
#include "detector.h"
#include "sdcard.h"
#include <BLEAdvertisedDevice.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/ringbuf.h"

static const char*    CAPTURE_DIR = SD_DIR "/captures";
static const size_t   RING_BYTES  = 64 * 1024;   // PSRAM; ~0.2-1s of a busy area
static const uint16_t MAX_FRAME   = 2500;        // longest 802.11 MPDU we keep
static const uint32_t SYNC_MS     = 1000;        // a power cut loses at most ~1s
static const uint32_t MESSAGE_MS  = 5000;        // "Saved ..." stays up this long

static const uint16_t COLOR_RED    = 0xF800;
static const uint16_t COLOR_GREEN  = 0x07E0;
static const uint16_t COLOR_YELLOW = 0xFFE0;

enum State : uint8_t { IDLE, RECORDING, STOPPING };

// Ring items start with a tag byte saying which file they belong to.
static const uint8_t TAG_WIFI = 'W';
static const uint8_t TAG_BLE  = 'B';

static volatile State    s_state = IDLE;
static volatile uint32_t s_frames = 0, s_adverts = 0, s_dropped = 0;
static volatile bool     s_writeError = false;
static RingbufHandle_t   s_ring = nullptr;
static TaskHandle_t      s_writer = nullptr;
static FILE*             s_pcap = nullptr;
static FILE*             s_ble  = nullptr;
static uint32_t          s_startMs = 0;
static uint16_t          s_boot = 0;     // 0 = not worked out yet
static uint16_t          s_index = 0;
static char              s_name[16];

static char     s_msg[32];
static uint16_t s_msgColor = 0;
static uint32_t s_msgUntil = 0;

static void message(uint16_t color, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
    s_msgColor = color;
    s_msgUntil = millis() + MESSAGE_MS;   // set last: the UI checks this first
}

// ---------------------------------------------------------------------------
// Taps - run in the WiFi / BLE host tasks. Copy and return, never block.
// ---------------------------------------------------------------------------

static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }   // RISC-V is little-endian,
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }   // as pcap/radiotap want

static uint16_t channelMhz(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return 2407 + 5 * ch;
    if (ch == 14)            return 2484;
    return 5000 + 5 * ch;
}

// Radiotap: flags (FCS included), channel, antenna signal dBm. 15 bytes.
static const uint8_t RT_LEN = 15;

static void wifiTap(const void* buff) {
    if (s_state != RECORDING) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buff;
    uint16_t orig = p->rx_ctrl.sig_len;   // includes the 4-byte FCS
    uint16_t len  = orig > MAX_FRAME ? MAX_FRAME : orig;
    uint8_t  ch   = p->rx_ctrl.channel ? p->rx_ctrl.channel : detectorChannel();

    void* item;
    if (xRingbufferSendAcquire(s_ring, &item, 1 + 16 + RT_LEN + len, 0) != pdTRUE) {
        s_dropped++;
        return;
    }
    uint8_t* d = (uint8_t*)item;
    d[0] = TAG_WIFI;

    int64_t us = esp_timer_get_time();
    uint8_t* rec = d + 1;                  // pcap record header
    put32(rec + 0, (uint32_t)(us / 1000000));
    put32(rec + 4, (uint32_t)(us % 1000000));
    put32(rec + 8, RT_LEN + len);
    put32(rec + 12, RT_LEN + orig);

    uint8_t* rt = rec + 16;                // radiotap header
    rt[0] = 0;                             // version
    rt[1] = 0;
    put16(rt + 2, RT_LEN);
    put32(rt + 4, (1u << 1) | (1u << 3) | (1u << 5));   // flags, channel, dBm signal
    rt[8] = 0x10;                          // flags: frame includes FCS
    rt[9] = 0;                             // pad - channel is 2-byte aligned
    put16(rt + 10, channelMhz(ch));
    put16(rt + 12, ch <= 14 ? 0x00A0 : 0x0140);   // 2GHz+CCK / 5GHz+OFDM
    rt[14] = (uint8_t)(int8_t)p->rx_ctrl.rssi;

    memcpy(rt + RT_LEN, p->payload, len);
    xRingbufferSendComplete(s_ring, item);
    s_frames++;
}

static void appendHex(String& out, const uint8_t* data, size_t n) {
    static const char* HEX_DIGITS = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out += HEX_DIGITS[data[i] >> 4];
        out += HEX_DIGITS[data[i] & 0x0F];
    }
}

static void appendCsvText(String& out, const String& s) {
    out += '"';
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"') out += "\"\"";
        else if ((uint8_t)c >= 0x20) out += c;   // drop control characters
    }
    out += '"';
}

static void bleTap(BLEAdvertisedDevice& adv) {
    if (s_state != RECORDING) return;

    String line;
    line.reserve(160);
    char head[64];
    snprintf(head, sizeof(head), "%.3f,%s,%u,%d,", esp_timer_get_time() / 1e6,
             adv.getAddress().toString().c_str(), adv.getAddressType(), adv.getRSSI());
    line += head;
    appendCsvText(line, adv.haveName() ? adv.getName() : String());
    line += ',';
    if (adv.haveManufacturerData()) {
        String m = adv.getManufacturerData();
        appendHex(line, (const uint8_t*)m.c_str(), m.length());
    }
    line += ',';
    String uuids;
    for (int i = 0; i < adv.getServiceUUIDCount(); i++) {
        if (i) uuids += ' ';
        uuids += adv.getServiceUUID(i).toString();
    }
    appendCsvText(line, uuids);
    line += ',';
    appendHex(line, adv.getPayload(), adv.getPayloadLength());
    line += '\n';

    void* item;
    if (xRingbufferSendAcquire(s_ring, &item, 1 + line.length(), 0) != pdTRUE) {
        s_dropped++;
        return;
    }
    ((uint8_t*)item)[0] = TAG_BLE;
    memcpy((uint8_t*)item + 1, line.c_str(), line.length());
    xRingbufferSendComplete(s_ring, item);
    s_adverts++;
}

// ---------------------------------------------------------------------------
// Writer task - the only thing that touches the card during a capture.
// ---------------------------------------------------------------------------

static void syncFiles() {
    if (fflush(s_pcap) != 0 || fflush(s_ble) != 0) s_writeError = true;
    fsync(fileno(s_pcap));
    fsync(fileno(s_ble));
}

static void writerTask(void*) {
    for (;;) {
        // Sleep until a capture starts.
        while (s_state == IDLE) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t lastSync = millis();
        uint32_t emptySince = 0;
        for (;;) {
            size_t size;
            uint8_t* item = (uint8_t*)xRingbufferReceive(s_ring, &size, pdMS_TO_TICKS(50));
            if (item) {
                FILE* f = (item[0] == TAG_WIFI) ? s_pcap : s_ble;
                if (!s_writeError && fwrite(item + 1, 1, size - 1, f) != size - 1) s_writeError = true;
                vRingbufferReturnItem(s_ring, item);
                emptySince = 0;
            } else if (s_state == STOPPING) {
                // Taps are off; a frame already inside a tap can still land
                // for a moment. 200ms of empty ring = really done.
                if (!emptySince) emptySince = millis();
                if (millis() - emptySince > 200) break;
            }
            if (millis() - lastSync >= SYNC_MS) {
                syncFiles();
                lastSync = millis();
            }
        }

        syncFiles();
        fclose(s_pcap);
        fclose(s_ble);
        s_pcap = s_ble = nullptr;

        if (s_writeError) message(COLOR_RED, "SD error - %s", s_name);
        else if (s_dropped) message(COLOR_GREEN, "Saved %s (%lu drop)", s_name, (unsigned long)s_dropped);
        else message(COLOR_GREEN, "Saved %s", s_name);
        s_state = IDLE;
    }
}

// ---------------------------------------------------------------------------
// Start / stop (main loop)
// ---------------------------------------------------------------------------

// Boot number = one past the highest NNNN already in the captures folder.
static uint16_t nextBootNumber() {
    uint16_t highest = 0;
    DIR* d = opendir(CAPTURE_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            unsigned n;
            if (sscanf(e->d_name, "%4u_", &n) == 1 && n > highest) highest = (uint16_t)n;
        }
        closedir(d);
    }
    return highest >= 9999 ? 1 : highest + 1;
}

static bool startCapture() {
    if (!sdMounted()) {
        message(COLOR_YELLOW, "No SD card");
        return false;
    }
    mkdir(SD_DIR, 0777);        // the press is the user's go-ahead
    mkdir(CAPTURE_DIR, 0777);
    if (!s_boot) s_boot = nextBootNumber();
    s_index++;
    snprintf(s_name, sizeof(s_name), "%04u_%u", (unsigned)s_boot, (unsigned)s_index);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.pcap", CAPTURE_DIR, s_name);
    s_pcap = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/%s_ble.csv", CAPTURE_DIR, s_name);
    s_ble = fopen(path, "w");
    if (!s_pcap || !s_ble) {
        if (s_pcap) fclose(s_pcap);
        if (s_ble) fclose(s_ble);
        s_pcap = s_ble = nullptr;
        message(COLOR_RED, "SD error");
        return false;
    }

    // pcap global header: magic, v2.4, tz 0, sigfigs 0, snaplen, radiotap.
    uint8_t gh[24] = {};
    put32(gh + 0, 0xA1B2C3D4);
    put16(gh + 4, 2);
    put16(gh + 6, 4);
    put32(gh + 16, 65535);
    put32(gh + 20, 127);        // LINKTYPE_IEEE802_11_RADIOTAP
    fwrite(gh, 1, sizeof(gh), s_pcap);
    fputs("time_s,address,address_type,rssi,name,manufacturer_data,service_uuids,raw_payload\n", s_ble);

    // Ring buffer and writer are created on first use and then kept: a tap
    // already running in the WiFi task when a capture stops could otherwise
    // touch a freed buffer. Idle they cost 64KB of PSRAM and no CPU.
    if (!s_ring) s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (!s_writer) xTaskCreate(writerTask, "capture", 4096, nullptr, 1, &s_writer);
    if (!s_ring || !s_writer) {
        fclose(s_pcap);
        fclose(s_ble);
        s_pcap = s_ble = nullptr;
        message(COLOR_RED, "Capture failed");
        return false;
    }

    s_frames = s_adverts = s_dropped = 0;
    s_writeError = false;
    s_msgUntil = 0;
    s_startMs = millis();
    s_state = RECORDING;
    detectorWifiTap = wifiTap;
    detectorBleTap  = bleTap;
    xTaskNotifyGive(s_writer);
    return true;
}

static void stopCapture() {
    detectorWifiTap = nullptr;
    detectorBleTap  = nullptr;
    s_state = STOPPING;   // writer drains the ring, closes the files, -> IDLE
}

void captureButton() {
    if (s_state == RECORDING) stopCapture();
    else if (s_state == IDLE) startCapture();
    // STOPPING: still saving - ignore the press.
}

void captureUpdate() {
    if (s_state != RECORDING) return;
    if (s_writeError || millis() - s_startMs >= CAPTURE_MS) stopCapture();
}

bool captureStatus(char* text, size_t n, uint16_t* color, bool* recording) {
    *recording = false;
    switch (s_state) {
        case RECORDING: {
            uint32_t elapsed = millis() - s_startMs;
            uint32_t left = elapsed >= CAPTURE_MS ? 0 : (CAPTURE_MS - elapsed + 999) / 1000;
            snprintf(text, n, "REC %lu:%02lu  %lu", (unsigned long)(left / 60),
                     (unsigned long)(left % 60), (unsigned long)(s_frames + s_adverts));
            *color = COLOR_RED;
            *recording = true;
            return true;
        }
        case STOPPING:
            snprintf(text, n, "Saving...");
            *color = COLOR_YELLOW;
            return true;
        case IDLE:
            if (s_msgUntil && (int32_t)(millis() - s_msgUntil) < 0) {
                snprintf(text, n, "%s", s_msg);
                *color = s_msgColor;
                return true;
            }
            return false;
    }
    return false;
}
