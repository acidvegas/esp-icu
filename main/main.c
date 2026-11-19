// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// main.c
//
// Tiny home security camera firmware for the Seeed XIAO ESP32-S3 Sense.

#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espicu.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static temperature_sensor_handle_t tsens;

// ESP32-S3 die temperature in Celsius; the camera module has no sensor of its own.
float chip_temp(void) {
    float c = 0;
    if (tsens)
        temperature_sensor_get_celsius(tsens, &c);
    return c;
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&tc, &tsens) == ESP_OK)
        temperature_sensor_enable(tsens);
    setenv("TZ", cfg.tz, 1);
    tzset();

    // DMA buffers in internal RAM are reserved before Wi-Fi starts and fragments it.
    camera_start();
    storage_start();
    recorder_start();
    audio_start();
    motion_start();
    wifi_start();
    web_start();
    notify_start();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        uint64_t total, used;
        storage_usage(&total, &used);
        ESP_LOGI(TAG, "fps %.1f, temp %.1fC, rec %d, motion %d (score %u), dropped %lu, sd %llu/%llu MB, psram free %u KB, internal free %u KB (largest %u KB)",
                 camera_fps(), chip_temp(), rec_open, motion_active, motion_score, (unsigned long)rec_dropped, used >> 20, total >> 20,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10));
    }
}
