// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// main.c
//
// Tiny home security camera firmware for the Seeed XIAO ESP32-S3 Sense.

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "espicu.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static temperature_sensor_handle_t tsens;
static esp_timer_handle_t          reconnect;

// ESP32-S3 die temperature in Celsius; the camera module has no sensor of its own.
float chip_temp(void) {
    float c = 0;
    if (tsens)
        temperature_sensor_get_celsius(tsens, &c);
    return c;
}

// Measured with the stock flat antenna: uploads ran at 20-34 KB/s at 19-20 dBm and 525-667 KB/s at
// 8-18 dBm. The cap is a setting (default 15 dBm) so other antennas can be tuned from the UI.
void wifi_apply_power(void) {
    int dbm = cfg.tx_power < 8 ? 8 : cfg.tx_power > 20 ? 20 : cfg.tx_power;
    esp_wifi_set_max_tx_power(dbm * 4);
}

static void do_reconnect(void *arg) {
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 20 MHz beats 40 MHz on the crowded 2.4 GHz band.
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
        wifi_apply_power();
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "wifi disconnected (reason %d), reconnecting", e->reason);
        // Retry from a timer, not by sleeping here, so the event loop keeps running.
        if (!esp_timer_is_active(reconnect))
            esp_timer_start_once(reconnect, 1000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "online: http://" IPSTR "/ (http://%s.local/)", IP2STR(&e->ip_info.ip), HOSTNAME);
    }
}

static void wifi_start(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, HOSTNAME);
    const esp_timer_create_args_t ta = {.callback = do_reconnect, .name = "reconnect"};
    esp_timer_create(&ta, &reconnect);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; // the SSID is hidden, so probe every channel
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    // Modem sleep delays ACKs to beacon intervals, which throttles streaming.
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    mdns_init();
    mdns_hostname_set(HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
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
