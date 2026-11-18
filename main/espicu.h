// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// espicu.h
//
// Types and cross-module functions shared by every part of the firmware.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"
#include "esp_err.h"

#define MASK_COLS  16
#define MASK_ROWS  12
#define MASK_CELLS (MASK_COLS * MASK_ROWS)
#define MASK_BYTES ((MASK_CELLS + 7) / 8)

#define SD_MOUNT "/sd"
#define REC_DIR  SD_MOUNT "/rec"
#define SNAP_DIR SD_MOUNT "/snap"

#define AUDIO_RATE 16000 // PDM mic, 16-bit mono

typedef struct {
    uint8_t  framesize;      // framesize_t
    uint8_t  quality;        // JPEG quality 4-63, lower is better
    bool     vflip;
    bool     hmirror;
    uint16_t rotate;         // display rotation in the UI: 0, 90, 180, 270
    uint8_t  record_fps;
    bool     motion_record;
    uint8_t  sensitivity;    // 1-100
    uint8_t  min_cells;      // active zone cells needed to count as motion
    uint8_t  pre_seconds;    // footage kept from before motion started
    uint8_t  post_seconds;   // keep recording this long after motion stops
    uint8_t  clip_minutes;   // split clips at this length
    uint16_t retention_days; // 0 = keep forever
    bool     notify_motion;   // ntfy push with a snapshot when motion starts
    uint16_t notify_cooldown; // minimum seconds between pushes
    char     tz[48];         // POSIX TZ string
    uint8_t  mask[MASK_BYTES];
    // Fields added later go below this line only: older saved blocks load as a prefix (see settings.c).
    uint8_t  tx_power;       // Wi-Fi max TX power, dBm
    int8_t   brightness;     // -2..2, image controls
    int8_t   contrast;
    int8_t   saturation;
    int8_t   ae_level;
    uint8_t  wb_mode;        // 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home
    bool     night;          // higher gain ceiling for low light
    bool     audio;          // record the mic into clips
    uint8_t  mic_gain;       // digital gain, 1-16
} settings_t;

extern settings_t cfg;
void settings_load(void);
void settings_save(void);

// A captured JPEG shared by reference between the capture task and every consumer.
typedef struct {
    uint8_t     *buf;
    size_t       len;
    uint32_t     seq;
    int64_t      us;   // capture time, esp_timer clock
    volatile int refs;
} frame_t;

frame_t    *frame_get(uint32_t after_seq, uint32_t wait_ms); // newest frame newer than after_seq, or NULL on timeout
void        frame_put(frame_t *f);
esp_err_t   camera_start(void);
void        camera_apply(void);
float       camera_fps(void);

void                     motion_start(void);
void                     motion_reset(void);
extern volatile bool     motion_active;
extern volatile uint16_t motion_score;
extern uint8_t           motion_cells[MASK_BYTES];

void                    recorder_start(void);
extern volatile bool    rec_manual;
extern volatile bool    rec_suppress; // set by Stop: ignore motion until it ends
extern volatile bool    rec_open;
extern volatile int64_t rec_started_us;
extern volatile uint32_t rec_dropped;
bool                    recorder_busy_with(const char *path);
void                    recorder_path(char *out, size_t n);

esp_err_t storage_start(void);
bool      storage_ok(void);
void      storage_fail(void); // a write failed: check the card and remount it
void      storage_usage(uint64_t *total, uint64_t *used);
bool      time_synced(void);
int       sd_list(const char *path, bool dirs, char ***out); // sorted names, free with sd_list_free()
void      sd_list_free(char **names, int n);

// Sector-aligned reads through an internal-RAM DMA buffer (SDMMC cannot DMA into PSRAM).
typedef struct {
    int      fd;
    uint8_t *buf;
    uint32_t start; // file offset of buf[0]
    uint32_t len;   // valid bytes in buf
} sd_reader_t;

bool sd_reader_open(sd_reader_t *r, const char *path);
bool sd_reader_read(sd_reader_t *r, uint32_t pos, void *dst, uint32_t len);
void sd_reader_close(sd_reader_t *r);

void     audio_start(void);
uint64_t audio_now(int64_t us);                               // mic sample index at an esp_timer time, 0 when the mic is off
uint32_t audio_read(uint64_t from, int16_t *dst, uint32_t n); // samples copied, 0 once they have left the ring

esp_err_t web_start(void);
float     chip_temp(void);
void      wifi_apply_power(void);
void      notify_start(void);
