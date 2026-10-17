// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 1 bench check: scan the shared I2C bus (SDA 27 / SCL 26) and name
// what answers, then read the ES8311's chip ID registers (0xFD/0xFE, expect
// 0x83/0x11) to confirm the codec address. Read-only - writes nothing to
// any device, so the CH32 and AXP2101 are left exactly as boot left them.

#include <Arduino.h>
#include <Wire.h>

static const uint8_t I2C_SDA = 27;
static const uint8_t I2C_SCL = 26;

static const char* deviceName(uint8_t addr) {
    switch (addr) {
        case 0x18: return "ES8311 codec (expected)";
        case 0x24: return "CH32V006 IO expander";
        case 0x34: return "AXP2101 PMIC";
        case 0x38: return "FT6336 touch";
        case 0x51: return "PCF85063 RTC";
        case 0x6B: return "QMI8658 IMU";
        case 0x70: return "SHTC3 temp/humidity";
    }
    return "unknown";
}

static bool readReg(uint8_t addr, uint8_t reg, uint8_t* value) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
    *value = Wire.read();
    return true;
}

static void scan() {
    Serial.println("I2C scan (SDA 27, SCL 26):");
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X  %s\n", addr, deviceName(addr));
            found++;
        }
    }
    Serial.printf("%u device(s) found\n", found);

    // ES8311 can strap to 0x18 or 0x19 (CE pin) - check both.
    for (uint8_t addr = 0x18; addr <= 0x19; addr++) {
        uint8_t id1, id2;
        if (readReg(addr, 0xFD, &id1) && readReg(addr, 0xFE, &id2)) {
            Serial.printf("ES8311 ID at 0x%02X: %02X %02X %s\n", addr, id1, id2,
                          (id1 == 0x83 && id2 == 0x11) ? "(match)" : "(UNEXPECTED)");
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);   // let USB CDC enumerate before the first print
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
}

void loop() {
    scan();
    Serial.println();
    delay(5000);
}
