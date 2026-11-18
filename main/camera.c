// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// camera.c
//
// Camera setup and the capture task. Each frame is copied out of the driver's buffer into a
// reference-counted PSRAM frame and the driver buffer is returned at once, so slow consumers
// never hold up capture. Consumers block in frame_get() and are woken by task notification.
// If no frame arrives for 10 s the camera is reinitialized, and the board restarts if that fails.

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espicu.h"

#define MAX_WAITERS 12
#define STALL_US    10000000

static const char *TAG = "camera";

static portMUX_TYPE  lock = portMUX_INITIALIZER_UNLOCKED;
static frame_t      *latest;
static TaskHandle_t  waiters[MAX_WAITERS];
static volatile float fps;
static uint8_t       base_gain; // sensor defaults, restored when night mode is turned off
static int           base_aec2;

static esp_err_t camera_init(void);

void frame_put(frame_t *f) {
    if (!f)
        return;
    taskENTER_CRITICAL(&lock);
    int left = --f->refs;
    taskEXIT_CRITICAL(&lock);
    if (!left) {
        heap_caps_free(f->buf);
        free(f);
    }
}

frame_t *frame_get(uint32_t after_seq, uint32_t wait_ms) {
    TaskHandle_t me       = xTaskGetCurrentTaskHandle();
    int64_t      deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    for (;;) {
        int slot = -1;
        taskENTER_CRITICAL(&lock);
        frame_t *f = latest;
        if (f && f->seq != after_seq) {
            f->refs++;
            taskEXIT_CRITICAL(&lock);
            return f;
        }
        for (int i = 0; i < MAX_WAITERS; i++) {
            if (!waiters[i]) {
                waiters[i] = me;
                slot       = i;
                break;
            }
        }
        taskEXIT_CRITICAL(&lock);

        int64_t left = deadline - esp_timer_get_time();
        if (left > 0)
            ulTaskNotifyTake(pdTRUE, slot >= 0 ? pdMS_TO_TICKS(left / 1000 + 1) : pdMS_TO_TICKS(5));
        if (slot >= 0) {
            taskENTER_CRITICAL(&lock);
            if (waiters[slot] == me)
                waiters[slot] = NULL;
            taskEXIT_CRITICAL(&lock);
        }
        if (left <= 0)
            return NULL;
    }
}

static void capture_task(void *arg) {
    uint32_t seq = 0, n = 0;
    int64_t  t0  = esp_timer_get_time(), last_ok = t0;
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "frame capture failed");
            if (esp_timer_get_time() - last_ok > STALL_US) {
                ESP_LOGE(TAG, "no frames for 10 s, reinitializing the camera");
                esp_camera_deinit();
                vTaskDelay(pdMS_TO_TICKS(200));
                if (camera_init() != ESP_OK) {
                    ESP_LOGE(TAG, "camera reinit failed, restarting");
                    esp_restart();
                }
                last_ok = esp_timer_get_time();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        last_ok = esp_timer_get_time();
        frame_t *f = calloc(1, sizeof(frame_t));
        uint8_t *b = f ? heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM) : NULL;
        if (!b) {
            free(f);
            esp_camera_fb_return(fb);
            ESP_LOGW(TAG, "out of PSRAM for frame");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        memcpy(b, fb->buf, fb->len);
        f->buf  = b;
        f->len  = fb->len;
        f->us   = esp_timer_get_time();
        f->seq  = ++seq;
        f->refs = 1; // held by `latest`
        esp_camera_fb_return(fb);

        TaskHandle_t wake[MAX_WAITERS];
        taskENTER_CRITICAL(&lock);
        frame_t *old = latest;
        latest       = f;
        memcpy(wake, waiters, sizeof(wake));
        taskEXIT_CRITICAL(&lock);
        for (int i = 0; i < MAX_WAITERS; i++)
            if (wake[i])
                xTaskNotifyGive(wake[i]);
        frame_put(old);

        if (++n == 30) {
            int64_t now = esp_timer_get_time();
            fps         = 30e6f / (now - t0);
            t0          = now;
            n           = 0;
        }
    }
}

void camera_apply(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
        return;
    s->set_framesize(s, (framesize_t)cfg.framesize);
    s->set_quality(s, cfg.quality);
    // The OV3660 is mounted flipped relative to the OV2640; Espressif's CameraWebServer flips it back.
    bool ov3660 = s->id.PID == OV3660_PID;
    s->set_vflip(s, ov3660 ? !cfg.vflip : cfg.vflip);
    s->set_hmirror(s, cfg.hmirror);
    // Image controls are offsets from the tuned defaults: the OV3660 looks best at brightness +1, saturation -2.
#define LVL(v) ((v) < -2 ? -2 : (v) > 2 ? 2 : (v))
    if (s->set_brightness)
        s->set_brightness(s, LVL(cfg.brightness + (ov3660 ? 1 : 0)));
    if (s->set_contrast)
        s->set_contrast(s, LVL(cfg.contrast));
    if (s->set_saturation)
        s->set_saturation(s, LVL(cfg.saturation + (ov3660 ? -2 : 0)));
    if (s->set_ae_level)
        s->set_ae_level(s, LVL(cfg.ae_level));
    if (s->set_whitebal && s->set_wb_mode) {
        s->set_whitebal(s, 1);
        s->set_wb_mode(s, cfg.wb_mode > 4 ? 0 : cfg.wb_mode);
    }
    if (s->set_gainceiling)
        s->set_gainceiling(s, cfg.night ? GAINCEILING_128X : (gainceiling_t)base_gain);
    if (s->set_aec2)
        s->set_aec2(s, cfg.night ? 1 : base_aec2);
}

static const char *camera_name(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
        return "none";
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    return info ? info->name : "unknown";
}

float camera_fps(void) {
    return fps;
}

static esp_err_t camera_init(void) {
    // Pins for the XIAO ESP32-S3 Sense (CAMERA_MODEL_XIAO_ESP32S3 in Espressif's camera_pins.h).
    camera_config_t c = {
        .pin_pwdn     = -1,
        .pin_reset    = -1,
        .pin_xclk     = 10,
        .pin_sccb_sda = 40,
        .pin_sccb_scl = 39,
        .pin_d7       = 48,
        .pin_d6       = 11,
        .pin_d5       = 12,
        .pin_d4       = 14,
        .pin_d3       = 16,
        .pin_d2       = 18,
        .pin_d1       = 17,
        .pin_d0       = 15,
        .pin_vsync    = 38,
        .pin_href     = 47,
        .pin_pclk     = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        // JPEG buffers are sized from the init resolution and can never grow, so init at the
        // largest size offered and step down (esp32-camera issue #825).
        .frame_size   = FRAMESIZE_UXGA,
        .jpeg_quality = 10,
        .fb_count     = 3,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    base_gain   = s->status.gainceiling;
    base_aec2   = s->status.aec2;
    camera_apply();
    ESP_LOGI(TAG, "%s ready", camera_name());
    return ESP_OK;
}

esp_err_t camera_start(void) {
    esp_err_t err = camera_init();
    if (err == ESP_OK)
        xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 6, NULL, 1);
    return err;
}
