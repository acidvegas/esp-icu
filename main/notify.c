// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// notify.c
//
// Sends an ntfy push with a snapshot attached when motion starts, at most once per cooldown.
// The JPEG is the request body and the Filename header makes ntfy store it as an attachment.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espicu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36"

static const char *TAG = "notify";

static void push(frame_t *f) {
    char      when[16] = "", message[64], name[40];
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (time_synced())
        strftime(when, sizeof(when), " at %H:%M:%S", &t);
    snprintf(message, sizeof(message), "Motion detected%s", when);
    strftime(name, sizeof(name), "espicu_%Y%m%d_%H%M%S.jpg", &t);

    esp_http_client_config_t c = {
        .url               = NTFY_URL "/" NTFY_TOPIC,
        .method            = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = UA,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "Authorization", "Bearer " NTFY_TOKEN);
    esp_http_client_set_header(h, "Title", "ESP-ICU");
    esp_http_client_set_header(h, "Message", message);
    esp_http_client_set_header(h, "Filename", name);
    esp_http_client_set_header(h, "Tags", "rotating_light");
    esp_http_client_set_header(h, "Priority", "4");
    esp_http_client_set_header(h, "Click", "http://" HOSTNAME ".local/");
    esp_http_client_set_post_field(h, (const char *)f->buf, f->len);
    esp_err_t err    = esp_http_client_perform(h);
    int       status = esp_http_client_get_status_code(h);
    if (err != ESP_OK || status != 200)
        ESP_LOGW(TAG, "push failed: %s, HTTP %d", esp_err_to_name(err), status);
    else
        ESP_LOGI(TAG, "push sent (%u KB snapshot)", (unsigned)(f->len >> 10));
    esp_http_client_cleanup(h);
}

static void notify_task(void *arg) {
    bool    was    = false;
    int64_t last_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool now = motion_active;
        // Only the start of a motion event counts, and never more often than the cooldown.
        if (now && !was && cfg.notify_motion && (!last_us || esp_timer_get_time() - last_us >= cfg.notify_cooldown * 1000000LL)) {
            frame_t *f = frame_get(0, 1000);
            if (f) {
                last_us = esp_timer_get_time();
                push(f);
                frame_put(f);
            }
        }
        was = now;
    }
}

void notify_start(void) {
    if (!NTFY_TOKEN[0]) {
        ESP_LOGI(TAG, "NTFY_TOKEN is empty, motion notifications disabled");
        return;
    }
    // TLS needs a deep stack; it lives in PSRAM along with mbedTLS's buffers.
    xTaskCreatePinnedToCoreWithCaps(notify_task, "notify", 8192, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM);
}
