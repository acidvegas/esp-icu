// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// recorder.c
//
// Records MJPEG AVI clips to /sd/rec/YYYYMMDD/HHMMSS_<m|r>.avi on manual record or motion, with the mic
// as a second PCM stream when audio is on. Audio is interleaved after each frame up to that frame's time.
//
// The sampler task picks frames at record_fps, keeps a pre-roll of frame references while idle,
// and decides when clips open and close. It hands frame references to the writer task through a
// queue, so a slow SD write never stalls sampling. The writer stages data in an internal-RAM DMA
// buffer and writes whole 16 KB blocks: SDMMC on the S3 cannot DMA from PSRAM and otherwise
// falls back to one 512-byte sector per transaction.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "espicu.h"

#define PRE_MAX       300                  // 10 s at 30 fps
#define PRE_RESERVE   (2 * 1024 * 1024)    // pre-roll never takes PSRAM below this
#define QUEUE_LEN     (PRE_MAX + 64) // room for a full pre-roll, so starting a clip never blocks the sampler
#define STAGE_SIZE    16384
#define SPLIT_BYTES   (1024UL * 1024 * 1024) // start a new clip at 1 GB, well under FAT32's 4 GB file limit
#define AUDIO_CHUNK   8000                 // max samples per audio chunk
#define IDX_AUDIO     0x80000000u          // idx length flag: audio chunk

// AVI header sizes: video only, or video plus a 16 kHz mono PCM stream.
#define HDR_V  224
#define HDR_AV 326

static const char *TAG = "recorder";

volatile bool     rec_manual     = false;
volatile bool     rec_suppress   = false;
volatile bool     rec_open       = false;
volatile int64_t  rec_started_us = 0;
volatile uint32_t rec_dropped    = 0;

typedef enum { MSG_OPEN, MSG_FRAME, MSG_CLOSE } msg_type_t;

typedef struct {
    msg_type_t type;
    frame_t   *frame;
    char       kind;      // MSG_OPEN: 'm' motion or 'r' manual
    uint8_t    framesize; // MSG_OPEN
} msg_t;

static QueueHandle_t queue;
static portMUX_TYPE  path_lock = portMUX_INITIALIZER_UNLOCKED;
static char          cur_path[96]; // file the writer has open, "" when idle

// Writer state, only touched by the writer task.
static int       fd = -1;
static uint8_t  *stage;
static uint32_t  stage_len, file_pos, count, entries, cap, *idx, hdr;
static int64_t   first_us, last_us, synced_us;
static bool      failed, with_audio;
static char      cur_kind;
static uint8_t   cur_fs;
static uint64_t  a_pos, a_count;
static int16_t  *abuf;

bool recorder_busy_with(const char *path) {
    taskENTER_CRITICAL(&path_lock);
    bool busy = cur_path[0] && !strcmp(cur_path, path);
    taskEXIT_CRITICAL(&path_lock);
    return busy;
}

void recorder_path(char *out, size_t n) {
    taskENTER_CRITICAL(&path_lock);
    strlcpy(out, cur_path, n);
    taskEXIT_CRITICAL(&path_lock);
}

static void set_path(const char *p) {
    taskENTER_CRITICAL(&path_lock);
    strlcpy(cur_path, p, sizeof(cur_path));
    taskEXIT_CRITICAL(&path_lock);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}

static bool flush_stage(void) {
    if (!stage_len)
        return true;
    ssize_t n = write(fd, stage, stage_len);
    if (n != (ssize_t)stage_len) {
        ESP_LOGE(TAG, "write failed: %d (errno %d)", (int)n, errno);
        storage_fail();
        return false;
    }
    stage_len = 0;
    return true;
}

// Appends bytes to the file through the staging buffer; the file offset only advances in whole blocks.
static bool emit(const void *data, uint32_t len) {
    const uint8_t *p = data;
    file_pos += len;
    while (len) {
        uint32_t n = STAGE_SIZE - stage_len;
        if (n > len)
            n = len;
        memcpy(stage + stage_len, p, n);
        stage_len += n;
        p += n;
        len -= n;
        if (stage_len == STAGE_SIZE && !flush_stage())
            return false;
    }
    return true;
}

// Returns the header size. The movi LIST header is its last 12 bytes.
static uint32_t avi_header(uint8_t *h, uint16_t w, uint16_t ht, bool audio) {
    memset(h, 0, HDR_AV);
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "AVI LIST", 8);
    put32(h + 16, audio ? 294 : 192);
    memcpy(h + 20, "hdrlavih", 8);
    put32(h + 28, 56);
    put32(h + 44, 0x10); // AVIF_HASINDEX
    put32(h + 56, audio ? 2 : 1);
    put32(h + 64, w);
    put32(h + 68, ht);
    memcpy(h + 88, "LIST", 4);
    put32(h + 92, 116);
    memcpy(h + 96, "strlstrh", 8);
    put32(h + 104, 56);
    memcpy(h + 108, "vidsMJPG", 8);
    put32(h + 128, 1000);
    put16(h + 160, w);
    put16(h + 162, ht);
    memcpy(h + 164, "strf", 4);
    put32(h + 168, 40);
    put32(h + 172, 40);
    put32(h + 176, w);
    put32(h + 180, ht);
    put16(h + 184, 1);
    put16(h + 186, 24);
    memcpy(h + 188, "MJPG", 4);
    put32(h + 192, (uint32_t)w * ht * 3);
    uint32_t p = 212;
    if (audio) {
        memcpy(h + 212, "LIST", 4);
        put32(h + 216, 94);
        memcpy(h + 220, "strlstrh", 8);
        put32(h + 228, 56);
        memcpy(h + 232, "auds", 4);
        put32(h + 252, 2);              // dwScale = block align
        put32(h + 256, AUDIO_RATE * 2); // dwRate = bytes per second
        put32(h + 268, AUDIO_RATE / 5); // dwSuggestedBufferSize
        put32(h + 276, 2);              // dwSampleSize
        memcpy(h + 288, "strf", 4);
        put32(h + 292, 18);
        put16(h + 296, 1); // WAVE_FORMAT_PCM
        put16(h + 298, 1);
        put32(h + 300, AUDIO_RATE);
        put32(h + 304, AUDIO_RATE * 2);
        put16(h + 308, 2);
        put16(h + 310, 16);
        p = 314;
    }
    memcpy(h + p, "LIST", 4);
    memcpy(h + p + 8, "movi", 4);
    return p + 12;
}

static bool idx_add(uint32_t pos, uint32_t len) {
    if (entries * 2 + 2 > cap) {
        uint32_t  ncap = cap ? cap * 2 : 8192;
        uint32_t *p    = heap_caps_realloc(idx, ncap * 4, MALLOC_CAP_SPIRAM);
        if (!p)
            return false;
        idx = p;
        cap = ncap;
    }
    idx[entries * 2]     = pos - (hdr - 4); // offsets count from the "movi" fourcc
    idx[entries * 2 + 1] = len;
    entries++;
    return true;
}

// Writes the mic samples captured up to `us` as 01wb chunks.
static bool clip_audio(int64_t us) {
    uint64_t target = audio_now(us);
    if (!a_pos) {
        a_pos = target; // the clip's audio starts at its first frame
        return true;
    }
    while (a_pos < target) {
        uint32_t want = target - a_pos < AUDIO_CHUNK ? target - a_pos : AUDIO_CHUNK;
        uint32_t got  = audio_read(a_pos, abuf, want);
        if (!got) {
            a_pos = target; // the writer fell too far behind and the ring moved on
            break;
        }
        uint8_t h[8];
        memcpy(h, "01wb", 4);
        put32(h + 4, got * 2);
        uint32_t pos = file_pos;
        if (!emit(h, 8) || !emit(abuf, got * 2) || !idx_add(pos, got * 2 | IDX_AUDIO))
            return false;
        a_pos += got;
        a_count += got;
    }
    return true;
}

static void clip_open(char kind, uint8_t framesize) {
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char dir[48], path[96];
    strftime(dir, sizeof(dir), REC_DIR "/%Y%m%d", &t);
    mkdir(dir, 0775);
    int len = snprintf(path, sizeof(path), "%s/%02d%02d%02d_%c", dir, t.tm_hour, t.tm_min, t.tm_sec, kind);
    strcpy(path + len, ".avi");
    struct stat st;
    for (int i = 2; stat(path, &st) == 0 && i < 100; i++)
        snprintf(path + len, sizeof(path) - len, "%d.avi", i);

    // The path is published before the file opens so storage never unmounts under an open clip.
    set_path(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed (errno %d)", path, errno);
        set_path("");
        failed = true;
        storage_fail();
        return;
    }
    cur_kind  = kind;
    cur_fs    = framesize;
    stage_len = file_pos = count = entries = 0;
    first_us = last_us = 0;
    a_pos = a_count    = 0;
    with_audio         = cfg.audio && abuf && audio_now(esp_timer_get_time());
    synced_us          = esp_timer_get_time();
    failed             = false;
    uint8_t h[HDR_AV];
    hdr = avi_header(h, resolution[framesize].width, resolution[framesize].height, with_audio);
    if (!emit(h, hdr))
        failed = true;
    ESP_LOGI(TAG, "recording %s%s", path, with_audio ? " with audio" : "");
}

static void clip_frame(frame_t *f) {
    if (fd < 0 || failed)
        return;
    uint32_t pos = file_pos;
    uint8_t  h[8];
    memcpy(h, "00dc", 4);
    put32(h + 4, f->len);
    static const uint8_t pad = 0;
    if (!emit(h, 8) || !emit(f->buf, f->len) || ((f->len & 1) && !emit(&pad, 1)) || !idx_add(pos, f->len) ||
        (with_audio && !clip_audio(f->us))) {
        failed = true;
        return;
    }
    count++;
    if (!first_us)
        first_us = f->us;
    last_us = f->us;
    // fsync regularly so a power cut loses seconds of footage, not the clip's directory entry.
    if (esp_timer_get_time() - synced_us > 5000000) {
        fsync(fd);
        synced_us = esp_timer_get_time();
    }
}

static void clip_close(void);

static void clip_split(void) {
    if (fd >= 0 && !failed && file_pos >= SPLIT_BYTES) {
        clip_close();
        clip_open(cur_kind, cur_fs);
    }
}

static void clip_close(void) {
    if (fd < 0)
        return;
    float fps = (count > 1 && last_us > first_us) ? (count - 1) * 1e6f / (last_us - first_us) : cfg.record_fps;
    if (!failed) {
        uint32_t movi_end = file_pos;
        uint8_t  e[16];
        memcpy(e, "idx1", 4);
        put32(e + 4, entries * 16);
        bool ok = emit(e, 8);
        for (uint32_t i = 0; ok && i < entries; i++) {
            memcpy(e, idx[i * 2 + 1] & IDX_AUDIO ? "01wb" : "00dc", 4);
            put32(e + 4, 0x10); // AVIIF_KEYFRAME
            put32(e + 8, idx[i * 2]);
            put32(e + 12, idx[i * 2 + 1] & ~IDX_AUDIO);
            ok = emit(e, 16);
        }
        ok = ok && flush_stage();
        struct {
            uint32_t off, val;
        } patch[] = {
            {4, file_pos - 8},
            {32, (uint32_t)(1e6f / fps)},
            {48, count},
            {132, (uint32_t)(fps * 1000 + 0.5f)},
            {140, count},
            {hdr - 8, movi_end - (hdr - 4)},
            {264, (uint32_t)a_count}, // audio strh dwLength, only written when the clip has audio
        };
        for (size_t i = 0; ok && i < sizeof(patch) / sizeof(patch[0]) - !with_audio; i++) {
            uint8_t v[4];
            put32(v, patch[i].val);
            ok = lseek(fd, patch[i].off, SEEK_SET) >= 0 && write(fd, v, 4) == 4;
        }
        failed = !ok;
    }
    close(fd);
    fd = -1;
    char path[96];
    recorder_path(path, sizeof(path));
    set_path("");
    ESP_LOGI(TAG, "closed %s: %lu frames, %.1fs, %.2f fps, %.1f MB%s", path, (unsigned long)count, (last_us - first_us) / 1e6f, fps, file_pos / 1048576.0f, failed ? " (WRITE FAILED)" : "");
}

static void writer_task(void *arg) {
    msg_t m;
    for (;;) {
        xQueueReceive(queue, &m, portMAX_DELAY);
        switch (m.type) {
        case MSG_OPEN:
            clip_close();
            clip_open(m.kind, m.framesize);
            break;
        case MSG_FRAME:
            clip_frame(m.frame);
            frame_put(m.frame);
            clip_split();
            break;
        case MSG_CLOSE:
            clip_close();
            break;
        }
    }
}

static void send_frame(frame_t *f) {
    msg_t m = {.type = MSG_FRAME, .frame = f};
    if (xQueueSend(queue, &m, 0) != pdTRUE) {
        frame_put(f);
        rec_dropped++;
    }
}

static void sampler_task(void *arg) {
    static frame_t *pre[PRE_MAX];
    int      pre_head = 0, pre_count = 0;
    uint32_t seq = 0;
    int64_t  next_us = 0;
    uint8_t  framesize = cfg.framesize;

    for (;;) {
        frame_t *f = frame_get(seq, 1000);
        if (!motion_active)
            rec_suppress = false;
        bool    want = rec_manual || (cfg.motion_record && motion_active && !rec_suppress);
        int64_t now  = esp_timer_get_time();

        if (rec_open && (!want || !storage_ok() || framesize != cfg.framesize || now - rec_started_us >= cfg.clip_minutes * 60000000LL)) {
            msg_t m = {.type = MSG_CLOSE};
            xQueueSend(queue, &m, portMAX_DELAY);
            rec_open = false;
        }
        if (framesize != cfg.framesize) {
            framesize = cfg.framesize;
            while (pre_count) {
                frame_put(pre[(pre_head - pre_count + PRE_MAX) % PRE_MAX]);
                pre_count--;
            }
        }
        if (want && !rec_open && storage_ok()) {
            msg_t m = {.type = MSG_OPEN, .kind = rec_manual ? 'r' : 'm', .framesize = framesize};
            xQueueSend(queue, &m, portMAX_DELAY);
            rec_open       = true;
            rec_started_us = now;
            // Pre-roll frames go first, oldest to newest; the queue takes over their references.
            while (pre_count) {
                frame_t *p = pre[(pre_head - pre_count + PRE_MAX) % PRE_MAX];
                pre_count--;
                msg_t fm = {.type = MSG_FRAME, .frame = p};
                xQueueSend(queue, &fm, portMAX_DELAY);
            }
        }
        if (!f)
            continue;
        seq = f->seq;

        // Decimate the camera's frame rate to record_fps using capture timestamps.
        int64_t interval = 1000000 / (cfg.record_fps ? cfg.record_fps : 1);
        if (f->us < next_us) {
            frame_put(f);
            continue;
        }
        next_us = (next_us && f->us - next_us < interval) ? next_us + interval : f->us + interval;

        if (rec_open) {
            send_frame(f);
            continue;
        }
        // Idle: keep the newest pre_seconds of frames, as long as PSRAM stays above the reserve.
        int keep = cfg.pre_seconds * cfg.record_fps;
        if (keep > PRE_MAX)
            keep = PRE_MAX;
        while (pre_count && (pre_count >= keep || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < PRE_RESERVE)) {
            frame_put(pre[(pre_head - pre_count + PRE_MAX) % PRE_MAX]);
            pre_count--;
        }
        if (!keep) {
            frame_put(f);
            continue;
        }
        pre[pre_head] = f;
        pre_head      = (pre_head + 1) % PRE_MAX;
        pre_count++;
    }
}

void recorder_start(void) {
    stage = heap_caps_malloc(STAGE_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!stage) {
        ESP_LOGE(TAG, "no internal RAM for the write buffer, recording disabled");
        return;
    }
    abuf  = heap_caps_malloc(AUDIO_CHUNK * 2, MALLOC_CAP_SPIRAM);
    queue = xQueueCreateWithCaps(QUEUE_LEN, sizeof(msg_t), MALLOC_CAP_SPIRAM);
    // Neither task touches flash, so their stacks can live in PSRAM.
    xTaskCreatePinnedToCoreWithCaps(writer_task, "writer", 6144, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(sampler_task, "sampler", 4096, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
}
