// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// storage.c
//
// SD card in SDMMC 1-bit mode (CLK 7, CMD 9, D0 8, as in s60sc/ESP32-CAM_MJPEG2SD for this board),
// usage stats, retention by age, oldest-first cleanup when space runs low, and the aligned reader.
// A failed write or status check unmounts the card once nothing has it open, and mounting is retried
// every 30 s, so a reseated or replaced card comes back without a reboot.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "espicu.h"
#include "sdmmc_cmd.h"

#define LOW_SPACE      (1024ULL * 1024 * 1024)
#define DELETE_PER_RUN 50
#define READ_BUF       8192
#define READERS        5 // concurrent readers: one per web job (4) plus one for the API
#define CHECK_MS       5000

static const char *TAG = "storage";

static volatile bool     mounted, bad;
static sdmmc_card_t     *card;
static int               pool_n;         // buffers actually reserved
static uint8_t          *pool[READERS]; // DMA read buffers reserved at boot, before Wi-Fi fragments internal RAM
static bool              pool_used[READERS];
static SemaphoreHandle_t pool_sem;
static portMUX_TYPE      pool_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint64_t total_bytes, used_bytes;

bool time_synced(void) {
    return time(NULL) > 1700000000;
}

bool storage_ok(void) {
    return mounted;
}

void storage_fail(void) {
    bad = true;
}

void storage_usage(uint64_t *total, uint64_t *used) {
    *total = total_bytes;
    *used  = used_bytes;
}

static void refresh_usage(void) {
    uint64_t total, free_bytes;
    if (esp_vfs_fat_info(SD_MOUNT, &total, &free_bytes) == ESP_OK) {
        total_bytes = total;
        used_bytes  = total - free_bytes;
    }
}

bool sd_reader_open(sd_reader_t *r, const char *path) {
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    if (!mounted || !pool_sem || !xSemaphoreTake(pool_sem, pdMS_TO_TICKS(3000))) {
        ESP_LOGW(TAG, "no free reader for %s", path);
        return false;
    }
    taskENTER_CRITICAL(&pool_lock);
    for (int i = 0; i < pool_n && !r->buf; i++) {
        if (!pool_used[i]) {
            pool_used[i] = true;
            r->buf       = pool[i];
        }
    }
    taskEXIT_CRITICAL(&pool_lock);
    r->fd = open(path, O_RDONLY);
    if (r->fd < 0) {
        ESP_LOGW(TAG, "open %s failed (errno %d)", path, errno);
        sd_reader_close(r);
        return false;
    }
    return true;
}

bool sd_reader_read(sd_reader_t *r, uint32_t pos, void *dst, uint32_t len) {
    uint8_t *out = dst;
    while (len) {
        if (pos < r->start || pos >= r->start + r->len) {
            // Refill from a sector-aligned offset so FatFs reads whole sectors straight into the DMA buffer.
            r->start = pos & ~511u;
            r->len   = 0;
            if (lseek(r->fd, r->start, SEEK_SET) < 0)
                return false;
            ssize_t n = read(r->fd, r->buf, READ_BUF);
            if (n <= 0)
                return false;
            r->len = n;
            if (pos >= r->start + r->len)
                return false;
        }
        uint32_t n = r->start + r->len - pos;
        if (n > len)
            n = len;
        memcpy(out, r->buf + (pos - r->start), n);
        out += n;
        pos += n;
        len -= n;
    }
    return true;
}

void sd_reader_close(sd_reader_t *r) {
    if (r->fd >= 0)
        close(r->fd);
    if (r->buf) {
        taskENTER_CRITICAL(&pool_lock);
        for (int i = 0; i < pool_n; i++)
            if (pool[i] == r->buf)
                pool_used[i] = false;
        taskEXIT_CRITICAL(&pool_lock);
        xSemaphoreGive(pool_sem);
    }
    r->fd  = -1;
    r->buf = NULL;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Sorted entry names of a directory (files or subdirectories); free with sd_list_free().
int sd_list(const char *path, bool dirs, char ***out) {
    DIR *d = opendir(path);
    int  n = 0, cap = 0;
    *out   = NULL;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if ((e->d_type == DT_DIR) != dirs || e->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap        = cap ? cap * 2 : 32;
            char **tmp = realloc(*out, cap * sizeof(char *));
            if (!tmp)
                break;
            *out = tmp;
        }
        (*out)[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(*out, n, sizeof(char *), cmp_names);
    return n;
}

void sd_list_free(char **names, int n) {
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static const char *ROOTS[] = {REC_DIR, SNAP_DIR}; // clips and screenshots, both in YYYYMMDD folders

static void remove_day(const char *root, const char *day) {
    char dir[64], path[128], **files;
    snprintf(dir, sizeof(dir), "%s/%s", root, day);
    int n = sd_list(dir, false, &files);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        unlink(path);
    }
    sd_list_free(files, n);
    rmdir(dir);
    ESP_LOGI(TAG, "retention removed %s", dir);
}

static void retention(void) {
    if (!cfg.retention_days || !time_synced())
        return;
    time_t    cutoff = time(NULL) - (time_t)cfg.retention_days * 86400;
    struct tm t;
    localtime_r(&cutoff, &t);
    char day[16];
    strftime(day, sizeof(day), "%Y%m%d", &t);
    for (int r = 0; r < 2; r++) {
        char **days;
        int    n = sd_list(ROOTS[r], true, &days);
        for (int i = 0; i < n; i++) {
            // Files from before the clock synced land in 1970 folders; leave those to the low-space cleanup.
            if (strlen(days[i]) == 8 && strcmp(days[i], "2020") >= 0 && strcmp(days[i], day) < 0)
                remove_day(ROOTS[r], days[i]);
        }
        sd_list_free(days, n);
    }
}

static void free_space(void) {
    for (int removed = 0; total_bytes && total_bytes - used_bytes < LOW_SPACE && removed < DELETE_PER_RUN; removed++) {
        // The oldest day folder across clips and screenshots goes first.
        char dir[64] = "", path[128], **files;
        for (int r = 0; r < 2; r++) {
            char **days;
            int    nd = sd_list(ROOTS[r], true, &days);
            if (nd && (!dir[0] || strcmp(days[0], strrchr(dir, '/') + 1) < 0))
                snprintf(dir, sizeof(dir), "%s/%s", ROOTS[r], days[0]);
            sd_list_free(days, nd);
        }
        if (!dir[0])
            return;
        int nf = sd_list(dir, false, &files);
        if (!nf) {
            rmdir(dir);
        } else {
            snprintf(path, sizeof(path), "%s/%s", dir, files[0]);
            // A clip that is recording, playing or downloading is left for a later pass.
            if (recorder_busy_with(path) || unlink(path)) {
                sd_list_free(files, nf);
                return;
            }
            ESP_LOGW(TAG, "low space, removed %s", path);
        }
        sd_list_free(files, nf);
        refresh_usage();
    }
}

static esp_err_t mount(void) {
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 12,
        .allocation_unit_size   = 0,
    };
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width               = 1;
    slot.clk                 = GPIO_NUM_7;
    slot.cmd                 = GPIO_NUM_9;
    slot.d0                  = GPIO_NUM_8;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    // GPIO21 is the card's DAT3/CS line; hold it high so the card stays in SD (not SPI) mode.
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_21, GPIO_PULLUP_ONLY);

    sdmmc_host_t  host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz  = SDMMC_FREQ_HIGHSPEED;
    esp_err_t err      = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "40 MHz mount failed (%s), retrying at 20 MHz", esp_err_to_name(err));
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        err               = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mcfg, &card);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, card);
    mkdir(REC_DIR, 0775);
    mkdir(SNAP_DIR, 0775);
    bad     = false;
    mounted = true;
    refresh_usage();
    return ESP_OK;
}

// Unmounts once the recorder and every reader have let go, so no open file outlives the filesystem.
static bool unmount(void) {
    mounted = false;
    char cur[96];
    recorder_path(cur, sizeof(cur));
    if (cur[0] || uxSemaphoreGetCount(pool_sem) != pool_n)
        return false;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
    card        = NULL;
    total_bytes = used_bytes = 0;
    ESP_LOGE(TAG, "SD card unmounted, retrying every 30 s");
    return true;
}

static void storage_task(void *arg) {
    int64_t last_retention = -1;
    int     ticks = 0;
    for (;; vTaskDelay(pdMS_TO_TICKS(CHECK_MS)), ticks++) {
        if (card && (!mounted || bad || sdmmc_get_status(card) != ESP_OK)) {
            if (mounted)
                ESP_LOGE(TAG, "SD card failed, unmounting");
            unmount();
            continue;
        }
        if (!card) {
            if (ticks % (30000 / CHECK_MS) == 0)
                mount();
            continue;
        }
        if (ticks % (60000 / CHECK_MS))
            continue;
        refresh_usage();
        free_space();
        int64_t now = time(NULL);
        if (time_synced() && (last_retention < 0 || now - last_retention >= 600)) {
            retention();
            last_retention = now;
        }
    }
}

esp_err_t storage_start(void) {
    // Read buffers are reserved even without a card, so one inserted later still has them.
    for (int i = 0; i < READERS; i++)
        if ((pool[pool_n] = heap_caps_malloc(READ_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)))
            pool_n++;
    if (pool_n < READERS)
        ESP_LOGW(TAG, "only %d of %d read buffers reserved", pool_n, READERS);
    pool_sem      = xSemaphoreCreateCounting(pool_n ? pool_n : 1, pool_n);
    esp_err_t err = mount();
    xTaskCreatePinnedToCoreWithCaps(storage_task, "storage", 6144, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM);
    return err;
}
