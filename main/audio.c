// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// audio.c
//
// The Sense board's PDM microphone (CLK GPIO42, DATA GPIO41) captured continuously at 16 kHz into a
// PSRAM ring. The recorder pulls samples by capture time, so clips get audio for their pre-roll too.

#include <string.h>
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "espicu.h"

#define RING  (AUDIO_RATE * 16) // 16 s: covers the longest pre-roll plus writer lag
#define BLOCK (AUDIO_RATE / 50) // 20 ms per read

static const char *TAG = "audio";

static portMUX_TYPE      lock = portMUX_INITIALIZER_UNLOCKED;
static i2s_chan_handle_t rx;
static int16_t          *ring;
static uint64_t          total;   // samples captured since boot
static int64_t           last_us; // capture time of sample `total`

uint64_t audio_now(int64_t us) {
    taskENTER_CRITICAL(&lock);
    uint64_t t = total;
    int64_t  l = last_us;
    taskEXIT_CRITICAL(&lock);
    if (!t)
        return 0;
    int64_t back = (l - us) * AUDIO_RATE / 1000000;
    back         = back < 0 ? 0 : back > (int64_t)t ? (int64_t)t : back;
    return t - back;
}

uint32_t audio_read(uint64_t from, int16_t *dst, uint32_t n) {
    taskENTER_CRITICAL(&lock);
    uint64_t t = total;
    taskEXIT_CRITICAL(&lock);
    // Keep one block of margin from the write head, which may be overwriting the oldest samples right now.
    if (from >= t || t - from > RING - BLOCK)
        return 0;
    if (n > t - from)
        n = t - from;
    uint32_t at    = from % RING;
    uint32_t first = n < RING - at ? n : RING - at;
    memcpy(dst, ring + at, first * 2);
    memcpy(dst + first, ring, (n - first) * 2);
    return n;
}

static void audio_task(void *arg) {
    int16_t buf[BLOCK];
    int32_t dc = 0; // running mean << 8, removes the mic's DC offset
    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY) != ESP_OK || !got)
            continue;
        uint32_t n    = got / 2;
        int      gain = cfg.mic_gain ? cfg.mic_gain : 1;
        uint64_t at   = total;
        for (uint32_t i = 0; i < n; i++) {
            dc += buf[i] - (dc >> 8);
            int32_t v = (buf[i] - (dc >> 8)) * gain;
            ring[(at + i) % RING] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&lock);
        total += n;
        last_us = now;
        taskEXIT_CRITICAL(&lock);
    }
}

void audio_start(void) {
    ring = heap_caps_malloc(RING * 2, MALLOC_CAP_SPIRAM);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_pdm_rx_config_t pc = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = GPIO_NUM_42, .din = GPIO_NUM_41},
    };
    if (!ring || i2s_new_channel(&cc, NULL, &rx) != ESP_OK || i2s_channel_init_pdm_rx_mode(rx, &pc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed, clips will have no audio");
        return;
    }
    xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "mic ready, %d Hz", AUDIO_RATE);
}
