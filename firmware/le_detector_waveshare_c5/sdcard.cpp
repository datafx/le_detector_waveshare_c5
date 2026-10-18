// SPDX-License-Identifier: GPL-3.0-only
//
// IDF sdspi rather than the Arduino SD library: the display already drives
// SPI2 through esp_lcd, and sdspi joins that same bus as a second device
// (the SPI master driver arbitrates). The Arduino SD library would bring its
// own SPIClass and fight the display for the bus.

#include "sdcard.h"
#include "config.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const gpio_num_t PIN_SD_CS = GPIO_NUM_9;
static const char*      ERRORS_FILE = SD_DIR "/errors.txt";
static const uint16_t   MAX_ERRORS  = 50;   // more than this is just noise

static sdmmc_card_t* s_card = nullptr;
static String        s_errors;
static uint16_t      s_errorCount = 0;

bool sdInit() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // never touch the card's format
    mcfg.max_files = 5;
    mcfg.allocation_unit_size = 0;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        return false;
    }
    return true;
}

bool sdMounted() { return s_card != nullptr; }

void sdNoteError(const char* file, int line, const char* fmt, ...) {
    s_errorCount++;
    if (s_errorCount > MAX_ERRORS) return;

    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char entry[200];
    if (line > 0) snprintf(entry, sizeof(entry), "%s line %d: %s\n", file, line, msg);
    else          snprintf(entry, sizeof(entry), "%s: %s\n", file, msg);
    s_errors += entry;
    Serial.print(entry);
}

uint16_t sdErrorCount() { return s_errorCount; }

void sdFinishErrors() {
    if (!s_card) return;

    // Errors can only come from files in /ledet, so if the folder isn't
    // there, there's nothing to report and nothing gets created.
    struct stat st;
    if (stat(SD_DIR, &st) != 0) return;

    if (s_errorCount == 0) {
        unlink(ERRORS_FILE);   // stale report from an earlier boot
        return;
    }

    FILE* f = fopen(ERRORS_FILE, "w");
    if (!f) return;
    fputs("# Lines le_detector skipped at boot. Each one was ignored (a bad\n"
          "# setting keeps its default). Fix them and reboot; this file is\n"
          "# rewritten (or deleted when clean) on every boot.\n", f);
    fputs(s_errors.c_str(), f);
    if (s_errorCount > MAX_ERRORS) {
        fprintf(f, "... and %u more\n", (unsigned)(s_errorCount - MAX_ERRORS));
    }
    fclose(f);
}
