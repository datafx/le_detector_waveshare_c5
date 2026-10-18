// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>
#include "oui_table.h"

// SRC_PROBE is its own value (not lumped into SRC_WIFI) because a probe
// request SSID is weaker evidence than a beacon/probe-response SSID or an
// OUI hit: it's a client leaking a network it previously joined, not proof
// that network's AP is nearby now. Beacon/probe-response SSID matches stay
// tagged SRC_WIFI - same evidentiary weight as an OUI hit on those frames.
enum Source : uint8_t { SRC_BLE = 0, SRC_WIFI, SRC_PROBE };

// Which radio band a match came from - meaningless for SRC_BLE (BAND_NONE),
// since BLE only ever runs on 2.4GHz and isn't ambiguous the way WiFi is.
enum WifiBand : uint8_t { BAND_NONE = 0, BAND_5G, BAND_2G };

struct TrackedDevice {
    uint8_t      mac[6];
    int16_t      rssi;        // EMA-smoothed
    uint32_t     firstSeen;
    uint32_t     lastSeen;
    const char*  vendor;
    GearCategory category;
    Source       source;
    WifiBand     band;        // BAND_NONE when source == SRC_BLE
    char         ssid[33];    // NUL-terminated; empty when match wasn't SSID-based
    bool         used;
};

void detectorInit();

// Phase control - only one radio stack is up at a time. WiFi 5GHz and 2.4GHz
// are separate start functions (band-mode switch happens on entry) but share
// one stop path; BLE is fully independent.
void detectorStartWifi5GPhase();
void detectorStartWifi2GPhase();
void detectorStopWifiPhase();
void detectorStartBlePhase();
void detectorStopBlePhase();

void detectorHopChannel();      // call from loop during either WiFi phase
void detectorExpire();          // drop entries older than DEVICE_TTL_MS

// Summary of the current threat picture, for the UI and the alert engine.
struct DetectorStatus {
    uint8_t              activeCount;   // matched devices currently in the table
    int16_t              bestRssi;      // strongest matched device
    const TrackedDevice* best;          // nullptr when nothing is matched
    uint32_t             lastHitMs;     // millis() of the most recent match
};

DetectorStatus detectorStatus();

// Waveshare port: research-capture taps. capture.cpp points these at its
// own functions while a capture runs and clears them afterwards; null means
// no capture and costs one pointer check per frame/advert. They see every
// management frame / advert before any filtering, and must only copy -
// they run in the WiFi and BLE host tasks.
class BLEAdvertisedDevice;
typedef void (*DetectorWifiTap)(const void* promiscuousPkt);   // wifi_promiscuous_pkt_t*
typedef void (*DetectorBleTap)(BLEAdvertisedDevice& advert);
extern DetectorWifiTap volatile detectorWifiTap;
extern DetectorBleTap  volatile detectorBleTap;

uint8_t  detectorChannel();
uint16_t detectorSeenTotal();   // every frame/advert we looked at, matched or not
