// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// motion.c
//
// Motion detection on a scaled-down luma image against an adaptive background.
//
// Each pass decodes the newest frame at 1/8 scale (1/4 for QVGA), removes the global brightness
// shift (auto exposure) and compares every pixel with a running-average background. Changed
// pixels are counted per zone cell (16x12 grid); a cell is active when a quarter of its pixels
// changed, and motion needs min_cells active cells inside the enabled zones on two consecutive
// passes. A frame where most of the image changed is a lighting change and rebases the background.

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "espicu.h"

#define PASS_MS 200

volatile bool     motion_active = false;
volatile uint16_t motion_score  = 0;
uint8_t           motion_cells[MASK_BYTES];

static volatile bool reset_req = true;

void motion_reset(void) {
    reset_req = true;
}

static void motion_task(void *arg) {
    uint8_t  *rgb = NULL, *luma = NULL;
    uint16_t *bg = NULL;
    size_t    px = 0;
    uint32_t  seq = 0;
    uint8_t   hits = 0;
    int64_t   last_hit = 0;
    uint16_t  changed[MASK_CELLS], total[MASK_CELLS];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PASS_MS));
        frame_t *f = frame_get(seq, 1000);
        if (!f)
            continue;
        seq = f->seq;

        esp_jpeg_image_cfg_t jc = {
            .indata      = f->buf,
            .indata_size = f->len,
            .out_format  = JPEG_IMAGE_FORMAT_RGB888,
            .out_scale   = cfg.framesize <= FRAMESIZE_QVGA ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8,
        };
        esp_jpeg_image_output_t img;
        if (esp_jpeg_get_image_info(&jc, &img) != ESP_OK) {
            frame_put(f);
            continue;
        }
        // get_image_info reports full-size dimensions; the decoder outputs them divided by the scale.
        int    div = jc.out_scale == JPEG_IMAGE_SCALE_1_8 ? 8 : 4;
        int    w = img.width / div, h = img.height / div;
        size_t n = (size_t)w * h;
        if (n != px) {
            heap_caps_free(rgb);
            heap_caps_free(luma);
            heap_caps_free(bg);
            rgb       = heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM);
            luma      = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            bg        = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
            px        = (rgb && luma && bg) ? n : 0;
            reset_req = true;
        }
        jc.outbuf      = rgb;
        jc.outbuf_size = n * 3;
        bool ok = px && esp_jpeg_decode(&jc, &img) == ESP_OK && img.width == w && img.height == h;
        frame_put(f);
        if (!ok)
            continue;

        int32_t sum_l = 0, sum_b = 0;
        for (size_t i = 0; i < n; i++) {
            luma[i] = (rgb[i * 3] * 77 + rgb[i * 3 + 1] * 150 + rgb[i * 3 + 2] * 29) >> 8;
            sum_l += luma[i];
            sum_b += bg[i] >> 4;
        }
        // The background is reset for the first seconds after boot while auto exposure settles.
        if (reset_req || esp_timer_get_time() < 5000000) {
            for (size_t i = 0; i < n; i++)
                bg[i] = luma[i] << 4;
            reset_req     = false;
            hits          = 0;
            motion_score  = 0;
            motion_active = false;
            memset(motion_cells, 0, sizeof(motion_cells));
            continue;
        }

        int    offset = (sum_l - sum_b) / (int32_t)n;
        int    thresh = 50 - (cfg.sensitivity * 42) / 100;
        size_t changed_all = 0;
        memset(changed, 0, sizeof(changed));
        memset(total, 0, sizeof(total));
        for (int y = 0; y < h; y++) {
            int row = (y * MASK_ROWS / h) * MASK_COLS;
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                int    c = row + x * MASK_COLS / w;
                total[c]++;
                if (abs(luma[i] - (bg[i] >> 4) - offset) > thresh) {
                    changed[c]++;
                    changed_all++;
                }
            }
        }

        uint8_t cells[MASK_BYTES] = {0};
        int     score = 0;
        for (int c = 0; c < MASK_CELLS; c++) {
            if (total[c] && changed[c] * 4 >= total[c]) {
                cells[c / 8] |= 1 << (c % 8);
                if (cfg.mask[c / 8] & (1 << (c % 8)))
                    score++;
            }
        }

        if (changed_all * 3 > n * 2) {
            for (size_t i = 0; i < n; i++)
                bg[i] = luma[i] << 4;
            score = 0;
        } else {
            // ~1.6s time constant at 5 passes/s: anything that left the scene fades from the
            // background within a few seconds instead of lingering as a ghost.
            for (size_t i = 0; i < n; i++)
                bg[i] += ((luma[i] << 4) - (int)bg[i]) >> 3;
        }

        memcpy(motion_cells, cells, sizeof(cells));
        motion_score = score;
        hits         = score >= cfg.min_cells ? hits + 1 : 0;
        if (hits >= 2) {
            last_hit      = esp_timer_get_time();
            motion_active = true;
        } else if (motion_active && esp_timer_get_time() - last_hit > cfg.post_seconds * 1000000LL) {
            motion_active = false;
        }
    }
}

void motion_start(void) {
    xTaskCreatePinnedToCoreWithCaps(motion_task, "motion", 6144, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}
