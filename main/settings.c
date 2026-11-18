// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// settings.c
//
// Runtime settings persisted to NVS.

#include <string.h>
#include "espicu.h"
#include "nvs.h"

settings_t cfg;

void settings_load(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.framesize      = FRAMESIZE_VGA;
    cfg.quality        = 12;
    cfg.record_fps     = 20;
    cfg.motion_record  = false;
    cfg.sensitivity    = 60;
    cfg.min_cells      = 2;
    cfg.pre_seconds    = 3;
    cfg.post_seconds   = 10;
    cfg.clip_minutes   = 5;
    cfg.retention_days = 14;
    cfg.notify_cooldown = 60;
    strcpy(cfg.tz, "UTC0");
    memset(cfg.mask, 0xFF, sizeof(cfg.mask));
    cfg.tx_power       = 15;
    cfg.audio          = true;
    cfg.mic_gain       = 4;

    nvs_handle_t h;
    if (nvs_open("espicu", NVS_READONLY, &h) != ESP_OK)
        return;
    settings_t saved;
    size_t     len = sizeof(saved);
    // New fields are only ever appended, so a shorter block from older firmware loads as a prefix
    // and the newer fields keep their defaults.
    if (nvs_get_blob(h, "cfg", &saved, &len) == ESP_OK && len <= sizeof(saved))
        memcpy(&cfg, &saved, len);
    nvs_close(h);
}

void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open("espicu", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_blob(h, "cfg", &cfg, sizeof(cfg));
    nvs_commit(h);
    nvs_close(h);
}
