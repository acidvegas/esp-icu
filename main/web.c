// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// web.c
//
// HTTP server: UI page, live MJPEG stream, snapshots, settings, clip browsing and playback.
// Live view, playback and downloads are long-lived responses, so each runs as an async request in
// its own task and the API stays responsive.

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "espicu.h"

#define MAX_JOBS 4

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

typedef struct {
    void (*fn)(void *);
    httpd_req_t *req;
    char         path[96];
    uint32_t     start; // playback: first frame
    float        speed; // playback: rate multiplier
} job_t;

typedef struct {
    char      path[96];
    uint32_t *idx;  // pairs of (file offset of JPEG data, length)
    uint32_t  n;
    uint32_t *aidx; // pairs of (file offset of PCM data, length) for the audio stream
    uint32_t  an;
    uint64_t  asamples;
    float     fps;
    uint16_t  w, h;
} clip_t;

static char          auth[160];
static atomic_int    jobs;   // queued + running
static QueueHandle_t job_q;
static clip_t     cache; // last clip opened from the server task

static const framesize_t SIZES[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XGA, FRAMESIZE_HD, FRAMESIZE_SXGA, FRAMESIZE_UXGA};

static esp_err_t fail(httpd_req_t *r, const char *status, const char *msg) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, msg);
}

static bool authorized(httpd_req_t *r) {
    char   buf[192];
    size_t want = strlen(auth);
    if (httpd_req_get_hdr_value_str(r, "Authorization", buf, sizeof(buf)) == ESP_OK && strlen(buf) == want) {
        uint8_t diff = 0;
        for (size_t i = 0; i < want; i++)
            diff |= buf[i] ^ auth[i];
        if (!diff)
            return true;
    }
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"ESP-ICU\"");
    fail(r, "401 Unauthorized", "unauthorized");
    return false;
}

// State-changing requests need a custom header, which a cross-site form or image cannot send.
static bool mutating(httpd_req_t *r) {
    if (!authorized(r))
        return false;
    if (httpd_req_get_hdr_value_len(r, "X-Req"))
        return true;
    fail(r, "403 Forbidden", "missing X-Req header");
    return false;
}

static esp_err_t send_json(httpd_req_t *r, const char *s) {
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, s);
}

static bool param(httpd_req_t *r, const char *key, char *out, size_t n) {
    char q[192];
    return httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, key, out, n) == ESP_OK;
}

static bool valid_day(const char *d) {
    return strlen(d) == 8 && strspn(d, "0123456789") == 8;
}

enum { BAD, CLIP, SNAP };

// Builds the path of a clip (REC_DIR/<d>/<f>.avi) or screenshot (SNAP_DIR/<d>/<f>.jpg) from the d (YYYYMMDD)
// and f query params, rejecting anything else. Returns CLIP, SNAP or BAD.
static int file_path(httpd_req_t *r, char *path, size_t n) {
    char d[12], f[32];
    if (!param(r, "d", d, sizeof(d)) || !param(r, "f", f, sizeof(f)) || !valid_day(d))
        return BAD;
    size_t len  = strlen(f);
    int    kind = len < 5 || strstr(f, "..") ? BAD : !strcmp(f + len - 4, ".avi") ? CLIP : !strcmp(f + len - 4, ".jpg") ? SNAP : BAD;
    for (char *c = f; kind && *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '.')
            kind = BAD;
    if (kind)
        snprintf(path, n, "%s/%s/%s", kind == CLIP ? REC_DIR : SNAP_DIR, d, f);
    return kind;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[b[i] >> 4];
        out[i * 2 + 1] = H[b[i] & 15];
    }
    out[n * 2] = 0;
}

static void urldecode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++      = strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void clip_free(clip_t *c) {
    heap_caps_free(c->idx);
    heap_caps_free(c->aidx);
    memset(c, 0, sizeof(*c));
}

static bool list_add(uint32_t **idx, uint32_t *n, uint32_t *cap, uint32_t pos, uint32_t len) {
    if (*n * 2 + 2 > *cap) {
        uint32_t  ncap = *cap ? *cap * 2 : 8192;
        uint32_t *p    = heap_caps_realloc(*idx, ncap * 4, MALLOC_CAP_SPIRAM);
        if (!p)
            return false;
        *idx = p;
        *cap = ncap;
    }
    (*idx)[*n * 2]     = pos;
    (*idx)[*n * 2 + 1] = len;
    (*n)++;
    return true;
}

// Files a movi chunk under video (00dc) or audio (01wb). False for any other chunk.
static bool clip_add(clip_t *c, uint32_t *cap, uint32_t *acap, const uint8_t *id, uint32_t pos, uint32_t len) {
    if (!memcmp(id, "00dc", 4))
        return list_add(&c->idx, &c->n, cap, pos, len);
    if (!memcmp(id, "01wb", 4)) {
        c->asamples += len / 2;
        return list_add(&c->aidx, &c->an, acap, pos, len);
    }
    return false;
}

// Loads a clip's frame (and audio) index from its idx1 chunk, or by walking the chunks when a power cut
// left no index. The movi list is found by walking the top-level chunks, so clips with and without audio load.
static bool clip_load(clip_t *c, sd_reader_t *rd, const char *path) {
    if (!strcmp(c->path, path) && c->n)
        return true;
    clip_free(c);
    struct stat st;
    uint8_t     h[72];
    if (stat(path, &st) || !sd_reader_read(rd, 0, h, sizeof(h)) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4))
        return false;
    uint32_t size = st.st_size, us = rd32(h + 32), frames = rd32(h + 48), cap = 0, acap = 0;
    c->w   = rd32(h + 64);
    c->h   = rd32(h + 68);
    c->fps = us ? 1e6f / us : cfg.record_fps;

    uint8_t  e[16 * 64];
    uint32_t mpos = 12, mfour = 0, movi = 0; // movi: list size, 0 when a power cut left it unpatched
    while (mpos + 12 <= size && sd_reader_read(rd, mpos, e, 12)) {
        uint32_t len = rd32(e + 4);
        if (!memcmp(e, "LIST", 4) && !memcmp(e + 8, "movi", 4)) {
            mfour = mpos + 8;
            movi  = len;
            break;
        }
        uint32_t next = mpos + 8 + len + (len & 1);
        if (next <= mpos)
            break; // corrupt size wrapped around
        mpos = next;
    }
    if (!mfour)
        return false;

    uint32_t ipos = mfour + movi;
    if (frames && movi && ipos + 8 <= size && sd_reader_read(rd, ipos, e, 8) && !memcmp(e, "idx1", 4)) {
        uint32_t total = rd32(e + 4) / 16;
        for (uint32_t i = 0; i < total;) {
            uint32_t k = total - i < 64 ? total - i : 64;
            if (!sd_reader_read(rd, ipos + 8 + i * 16, e, k * 16))
                break;
            for (uint32_t j = 0; j < k; j++)
                clip_add(c, &cap, &acap, e + j * 16, rd32(e + j * 16 + 8) + mfour + 8, rd32(e + j * 16 + 12));
            i += k;
        }
    }
    if (!c->n) {
        uint32_t pos = mfour + 4;
        c->an = 0;
        c->asamples = 0;
        while (pos + 8 <= size && sd_reader_read(rd, pos, e, 8)) {
            uint32_t len = rd32(e + 4);
            if (!len || pos + 8 + len > size || !clip_add(c, &cap, &acap, e, pos + 8, len))
                break;
            pos += 8 + len + (len & 1);
        }
    }
    strlcpy(c->path, path, sizeof(c->path));
    return c->n > 0;
}

// Long-lived responses run on a fixed pool of workers with PSRAM stacks, fed from a queue.
static esp_err_t start_job(httpd_req_t *r, void (*fn)(void *), const char *path, uint32_t start, float speed) {
    if (atomic_fetch_add(&jobs, 1) >= MAX_JOBS) {
        atomic_fetch_sub(&jobs, 1);
        return fail(r, "503 Service Unavailable", "too many streams");
    }
    job_t *j = calloc(1, sizeof(job_t));
    if (!j || httpd_req_async_handler_begin(r, &j->req) != ESP_OK) {
        free(j);
        atomic_fetch_sub(&jobs, 1);
        return fail(r, "500 Internal Server Error", "async failed");
    }
    j->fn = fn;
    strlcpy(j->path, path, sizeof(j->path));
    j->start = start;
    j->speed = speed;
    int one  = 1;
    setsockopt(httpd_req_to_sockfd(j->req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    xQueueSend(job_q, &j, portMAX_DELAY);
    return ESP_OK;
}

static void job_done(job_t *j) {
    httpd_req_async_handler_complete(j->req);
    free(j);
    atomic_fetch_sub(&jobs, 1);
}

static void worker_task(void *arg) {
    job_t *j;
    for (;;)
        if (xQueueReceive(job_q, &j, portMAX_DELAY))
            j->fn(j);
}

static esp_err_t send_part(httpd_req_t *r, const uint8_t *jpg, size_t len) {
    char part[96];
    int  n = snprintf(part, sizeof(part), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    esp_err_t err = httpd_resp_send_chunk(r, part, n);
    return err == ESP_OK ? httpd_resp_send_chunk(r, (const char *)jpg, len) : err;
}

static void stream_task(void *arg) {
    job_t       *j = arg;
    httpd_req_t *r = j->req;
    httpd_resp_set_type(r, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    uint32_t seq = 0;
    for (;;) {
        frame_t *f = frame_get(seq, 5000);
        if (!f)
            break; // camera stalled: end the stream so the slot frees and the page reconnects
        seq           = f->seq;
        esp_err_t err = send_part(r, f->buf, f->len);
        frame_put(f);
        if (err != ESP_OK)
            break;
    }
    job_done(j);
}

// Streams a clip as MJPEG paced by the wall clock: each tick sends the frame due at that moment and
// skips frames if the link falls behind, so playback always runs at true speed.
static void play_task(void *arg) {
    job_t       *j = arg;
    httpd_req_t *r = j->req;
    clip_t       c = {0};
    sd_reader_t  rd;
    uint8_t     *buf = NULL;
    uint32_t     cap = 0;
    if (!sd_reader_open(&rd, j->path) || !clip_load(&c, &rd, j->path)) {
        fail(r, "404 Not Found", "no frames");
        goto done;
    }
    httpd_resp_set_type(r, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    double  rate = c.fps * j->speed / 1e6; // frames per microsecond
    int64_t t0   = esp_timer_get_time();
    int64_t sent = -1;
    for (;;) {
        int64_t i = j->start + (int64_t)((esp_timer_get_time() - t0) * rate);
        if (i >= c.n)
            break;
        if (i == sent) {
            int64_t wait = t0 + (int64_t)((sent + 1 - j->start) / rate) - esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(wait > 1000 ? wait / 1000 : 1));
            continue;
        }
        uint32_t pos = c.idx[i * 2], len = c.idx[i * 2 + 1];
        if (len > cap) {
            heap_caps_free(buf);
            buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
            cap = buf ? len : 0;
        }
        if (!buf || !sd_reader_read(&rd, pos, buf, len) || send_part(r, buf, len) != ESP_OK)
            break;
        sent = i;
    }
    httpd_resp_send_chunk(r, NULL, 0);
done:
    sd_reader_close(&rd);
    heap_caps_free(buf);
    clip_free(&c);
    job_done(j);
}

// A clip's audio from video frame `start` on, as a WAV the browser plays alongside the MJPEG playback.
static void wav_task(void *arg) {
    job_t       *j = arg;
    httpd_req_t *r = j->req;
    clip_t       c = {0};
    sd_reader_t  rd;
    uint8_t     *buf = NULL;
    if (!sd_reader_open(&rd, j->path) || !clip_load(&c, &rd, j->path) || !c.an) {
        fail(r, "404 Not Found", "no audio");
        goto done;
    }
    uint64_t skip = (uint64_t)(j->start / c.fps * AUDIO_RATE);
    if (skip > c.asamples)
        skip = c.asamples;
    uint32_t data = (c.asamples - skip) * 2;
    uint8_t  w[44];
    memcpy(w, "RIFF", 4);
    put32le(w + 4, 36 + data);
    memcpy(w + 8, "WAVEfmt ", 8);
    put32le(w + 16, 16);
    w[20] = 1, w[21] = 0, w[22] = 1, w[23] = 0; // PCM, mono
    put32le(w + 24, AUDIO_RATE);
    put32le(w + 28, AUDIO_RATE * 2);
    w[32] = 2, w[33] = 0, w[34] = 16, w[35] = 0;
    memcpy(w + 36, "data", 4);
    put32le(w + 40, data);
    httpd_resp_set_type(r, "audio/wav");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf || httpd_resp_send_chunk(r, (const char *)w, sizeof(w)) != ESP_OK)
        goto end;
    for (uint32_t i = 0; i < c.an; i++) {
        uint32_t pos = c.aidx[i * 2], len = c.aidx[i * 2 + 1];
        if (skip >= len / 2) {
            skip -= len / 2;
            continue;
        }
        pos += skip * 2;
        len -= skip * 2;
        skip = 0;
        while (len) {
            uint32_t n = len < 16384 ? len : 16384;
            if (!sd_reader_read(&rd, pos, buf, n) || httpd_resp_send_chunk(r, (const char *)buf, n) != ESP_OK)
                goto end;
            pos += n;
            len -= n;
        }
    }
end:
    httpd_resp_send_chunk(r, NULL, 0);
done:
    sd_reader_close(&rd);
    heap_caps_free(buf);
    clip_free(&c);
    job_done(j);
}

static void download_task(void *arg) {
    job_t       *j = arg;
    httpd_req_t *r = j->req;
    sd_reader_t  rd;
    struct stat  st;
    if (stat(j->path, &st) || !sd_reader_open(&rd, j->path)) {
        fail(r, "404 Not Found", "not found");
        job_done(j);
        return;
    }
    // /sd/rec/20261012/101500_m.avi -> 20261012_101500_m.avi
    char        cd[96];
    const char *name = strrchr(j->path, '/');
    snprintf(cd, sizeof(cd), "attachment; filename=\"%.8s_%s\"", name - 8, name + 1);
    httpd_resp_set_type(r, strstr(name, ".jpg") ? "image/jpeg" : "video/x-msvideo");
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    uint8_t *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    for (uint32_t pos = 0; buf && pos < (uint32_t)st.st_size;) {
        uint32_t n = st.st_size - pos < 16384 ? st.st_size - pos : 16384;
        if (!sd_reader_read(&rd, pos, buf, n) || httpd_resp_send_chunk(r, (const char *)buf, n) != ESP_OK)
            break;
        pos += n;
    }
    httpd_resp_send_chunk(r, NULL, 0);
    heap_caps_free(buf);
    sd_reader_close(&rd);
    job_done(j);
}

static esp_err_t index_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1); // EMBED_TXTFILES appends a NUL
}

static esp_err_t stream_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    return start_job(r, stream_task, "", 0, 1);
}

static esp_err_t snapshot_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    frame_t *f = frame_get(0, 1000);
    if (!f)
        return fail(r, "503 Service Unavailable", "no frame yet");
    char      cd[64];
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(cd, sizeof(cd), "attachment; filename=\"espicu_%Y%m%d_%H%M%S.jpg\"", &t);
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    esp_err_t err = httpd_resp_send(r, (const char *)f->buf, f->len);
    frame_put(f);
    return err;
}

// Saves the current frame to SNAP_DIR/YYYYMMDD/HHMMSS.jpg.
static esp_err_t snapshot_post(httpd_req_t *r) {
    if (!mutating(r))
        return ESP_OK;
    if (!storage_ok())
        return fail(r, "503 Service Unavailable", "no SD card");
    frame_t *f = frame_get(0, 1000);
    if (!f)
        return fail(r, "503 Service Unavailable", "no frame yet");
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char dir[48], path[96];
    strftime(dir, sizeof(dir), SNAP_DIR "/%Y%m%d", &t);
    mkdir(SNAP_DIR, 0775);
    mkdir(dir, 0775);
    int len = snprintf(path, sizeof(path), "%s/%02d%02d%02d", dir, t.tm_hour, t.tm_min, t.tm_sec);
    strcpy(path + len, ".jpg");
    struct stat st;
    for (int i = 2; stat(path, &st) == 0 && i < 100; i++)
        snprintf(path + len, sizeof(path) - len, "_%d.jpg", i);
    FILE *fp = fopen(path, "wb");
    bool  ok = fp && fwrite(f->buf, 1, f->len, fp) == f->len;
    if (fp)
        fclose(fp);
    frame_put(f);
    if (!ok)
        return fail(r, "500 Internal Server Error", "write failed");
    char s[96];
    snprintf(s, sizeof(s), "{\"d\":\"%.8s\",\"f\":\"%s\"}", dir + strlen(SNAP_DIR) + 1, strrchr(path, '/') + 1);
    return send_json(r, s);
}

// A saved screenshot, shown inline in the Storage tab.
static esp_err_t image_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char        path[96];
    struct stat st;
    if (file_path(r, path, sizeof(path)) != SNAP || stat(path, &st))
        return fail(r, "404 Not Found", "not found");
    sd_reader_t rd  = {.fd = -1};
    uint8_t    *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    esp_err_t   err;
    if (!buf || !sd_reader_open(&rd, path) || !sd_reader_read(&rd, 0, buf, st.st_size)) {
        err = fail(r, "500 Internal Server Error", "read failed");
    } else {
        httpd_resp_set_type(r, "image/jpeg");
        httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");
        err = httpd_resp_send(r, (const char *)buf, st.st_size);
    }
    sd_reader_close(&rd);
    heap_caps_free(buf);
    return err;
}

static esp_err_t status_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    uint64_t total, used;
    storage_usage(&total, &used);
    char cells[MASK_BYTES * 2 + 1], s[512];
    hex(motion_cells, MASK_BYTES, cells);
    snprintf(s, sizeof(s),
             "{\"rssi\":%d,\"fps\":%.0f,\"temp\":%.1f,\"rec\":%s,\"manual\":%s,\"rec_s\":%lld,"
             "\"motion\":%s,\"score\":%u,\"cells\":\"%s\",\"sd\":%s,\"sd_total\":%llu,\"sd_used\":%llu}",
             ap.rssi, camera_fps(), chip_temp(), rec_open ? "true" : "false", rec_manual ? "true" : "false",
             rec_open ? (esp_timer_get_time() - rec_started_us) / 1000000 : 0LL, motion_active ? "true" : "false", motion_score, cells,
             storage_ok() ? "true" : "false", total, used);
    return send_json(r, s);
}

static void settings_json(char *s, size_t n) {
    char mask[MASK_BYTES * 2 + 1];
    hex(cfg.mask, MASK_BYTES, mask);
    int len = snprintf(s, n,
                       "{\"framesize\":%u,\"quality\":%u,\"vflip\":%d,\"hmirror\":%d,\"rotate\":%u,\"record_fps\":%u,\"motion_record\":%d,"
                       "\"sensitivity\":%u,\"min_cells\":%u,\"pre_seconds\":%u,\"post_seconds\":%u,\"clip_minutes\":%u,\"retention_days\":%u,"
                       "\"notify_motion\":%d,\"notify_cooldown\":%u,\"ntfy\":%s,\"tz\":\"%s\",\"mask\":\"%s\",\"cols\":%d,\"rows\":%d,"
                       "\"tx_power\":%u,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"ae_level\":%d,\"wb_mode\":%u,\"night\":%d,"
                       "\"audio\":%d,\"mic_gain\":%u,\"sizes\":[",
                       cfg.framesize, cfg.quality, cfg.vflip, cfg.hmirror, cfg.rotate, cfg.record_fps, cfg.motion_record, cfg.sensitivity,
                       cfg.min_cells, cfg.pre_seconds, cfg.post_seconds, cfg.clip_minutes, cfg.retention_days, cfg.notify_motion,
                       cfg.notify_cooldown, NTFY_TOKEN[0] ? "true" : "false", cfg.tz, mask, MASK_COLS,
                       MASK_ROWS, cfg.tx_power, cfg.brightness, cfg.contrast, cfg.saturation, cfg.ae_level, cfg.wb_mode, cfg.night,
                       cfg.audio, cfg.mic_gain);
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++)
        len += snprintf(s + len, n - len, "%s[%d,\"%ux%u\"]", i ? "," : "", SIZES[i], resolution[SIZES[i]].width, resolution[SIZES[i]].height);
    snprintf(s + len, n - len, "]}");
}

static esp_err_t settings_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char s[1536];
    settings_json(s, sizeof(s));
    return send_json(r, s);
}

static void num(const char *body, const char *key, int lo, int hi, void *dst, size_t size) {
    char v[16];
    if (httpd_query_key_value(body, key, v, sizeof(v)) != ESP_OK)
        return;
    int x = atoi(v);
    x     = x < lo ? lo : x > hi ? hi : x; // int8 fields take negatives through the uint8 store below
    if (size == 1)
        *(uint8_t *)dst = x;
    else
        *(uint16_t *)dst = x;
}

#define NUM(key, lo, hi) num(body, #key, lo, hi, &cfg.key, sizeof(cfg.key))

static esp_err_t settings_post(httpd_req_t *r) {
    if (!mutating(r))
        return ESP_OK;
    char body[512];
    if (r->content_len >= sizeof(body))
        return fail(r, "413 Payload Too Large", "too large");
    int len = 0;
    while (len < (int)r->content_len) {
        int n = httpd_req_recv(r, body + len, r->content_len - len);
        if (n <= 0)
            return fail(r, "400 Bad Request", "read failed");
        len += n;
    }
    body[len] = 0;

    settings_t old = cfg;
    char       v[64];
    if (httpd_query_key_value(body, "framesize", v, sizeof(v)) == ESP_OK)
        for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++)
            if (atoi(v) == SIZES[i])
                cfg.framesize = SIZES[i];
    NUM(quality, 4, 63);
    NUM(vflip, 0, 1);
    NUM(hmirror, 0, 1);
    NUM(rotate, 0, 270);
    cfg.rotate -= cfg.rotate % 90;
    NUM(record_fps, 1, 30);
    NUM(motion_record, 0, 1);
    NUM(sensitivity, 1, 100);
    NUM(min_cells, 1, MASK_CELLS);
    NUM(pre_seconds, 0, 10);
    NUM(post_seconds, 1, 120);
    NUM(clip_minutes, 1, 60);
    NUM(retention_days, 0, 3650);
    NUM(notify_motion, 0, 1);
    NUM(notify_cooldown, 0, 3600);
    NUM(tx_power, 8, 20);
    NUM(brightness, -2, 2);
    NUM(contrast, -2, 2);
    NUM(saturation, -2, 2);
    NUM(ae_level, -2, 2);
    NUM(wb_mode, 0, 4);
    NUM(night, 0, 1);
    NUM(audio, 0, 1);
    NUM(mic_gain, 1, 16);
    if (httpd_query_key_value(body, "tz", v, sizeof(v)) == ESP_OK) {
        urldecode(v);
        if (*v && strlen(v) < sizeof(cfg.tz) && !strpbrk(v, "\"\\"))
            strcpy(cfg.tz, v);
    }
    if (httpd_query_key_value(body, "mask", v, sizeof(v)) == ESP_OK && strlen(v) == MASK_BYTES * 2) {
        for (int i = 0; i < MASK_BYTES; i++) {
            char h[3]   = {v[i * 2], v[i * 2 + 1], 0};
            cfg.mask[i] = strtol(h, NULL, 16);
        }
    }

    if (cfg.framesize != old.framesize || cfg.quality != old.quality || cfg.vflip != old.vflip || cfg.hmirror != old.hmirror ||
        cfg.brightness != old.brightness || cfg.contrast != old.contrast || cfg.saturation != old.saturation ||
        cfg.ae_level != old.ae_level || cfg.wb_mode != old.wb_mode || cfg.night != old.night)
        camera_apply();
    if (cfg.tx_power != old.tx_power)
        wifi_apply_power();
    if (cfg.framesize != old.framesize || cfg.vflip != old.vflip || cfg.hmirror != old.hmirror)
        motion_reset();
    if (strcmp(cfg.tz, old.tz)) {
        setenv("TZ", cfg.tz, 1);
        tzset();
    }
    settings_save();
    char s[1536];
    settings_json(s, sizeof(s));
    return send_json(r, s);
}

static esp_err_t record_post(httpd_req_t *r) {
    if (!mutating(r))
        return ESP_OK;
    char v[4];
    if (param(r, "on", v, sizeof(v))) {
        rec_manual   = v[0] == '1';
        rec_suppress = !rec_manual;
    }
    return send_json(r, "{\"ok\":true}");
}

static esp_err_t days_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    // Days that have clips, screenshots or both, newest first.
    char **a, **b;
    int    na = sd_list(REC_DIR, true, &a), nb = sd_list(SNAP_DIR, true, &b);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, "[");
    for (int i = na - 1, k = nb - 1, first = 1; i >= 0 || k >= 0;) {
        int         c   = i < 0 ? -1 : k < 0 ? 1 : strcmp(a[i], b[k]);
        const char *day = c >= 0 ? a[i] : b[k];
        if (c >= 0)
            i--;
        if (c <= 0)
            k--;
        if (!valid_day(day))
            continue;
        char item[16];
        snprintf(item, sizeof(item), "%s\"%s\"", first ? "" : ",", day);
        httpd_resp_sendstr_chunk(r, item);
        first = 0;
    }
    sd_list_free(a, na);
    sd_list_free(b, nb);
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

// Clips and screenshots of one day, newest first.
static esp_err_t files_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char d[12];
    if (!param(r, "d", d, sizeof(d)) || !valid_day(d))
        return fail(r, "400 Bad Request", "bad day");
    char rdir[48], sdir[48], path[96], cur[96], **a, **b;
    snprintf(rdir, sizeof(rdir), REC_DIR "/%s", d);
    snprintf(sdir, sizeof(sdir), SNAP_DIR "/%s", d);
    recorder_path(cur, sizeof(cur));
    int na = sd_list(rdir, false, &a), nb = sd_list(sdir, false, &b);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, "[");
    for (int i = na - 1, k = nb - 1, first = 1; i >= 0 || k >= 0;) {
        bool        clip = k < 0 || (i >= 0 && strcmp(a[i], b[k]) >= 0);
        const char *name = clip ? a[i--] : b[k--];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", clip ? rdir : sdir, name);
        if (stat(path, &st))
            continue;
        char item[128];
        snprintf(item, sizeof(item), "%s{\"f\":\"%s\",\"size\":%ld,\"rec\":%s}", first ? "" : ",", name, (long)st.st_size, strcmp(path, cur) ? "false" : "true");
        httpd_resp_sendstr_chunk(r, item);
        first = 0;
    }
    sd_list_free(a, na);
    sd_list_free(b, nb);
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t clip_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char path[96];
    if (file_path(r, path, sizeof(path)) != CLIP)
        return fail(r, "400 Bad Request", "bad clip");
    if (recorder_busy_with(path))
        return fail(r, "409 Conflict", "clip is still recording");
    sd_reader_t rd;
    bool        ok = sd_reader_open(&rd, path) && clip_load(&cache, &rd, path);
    sd_reader_close(&rd);
    if (!ok)
        return fail(r, "404 Not Found", "no frames");
    char s[128];
    snprintf(s, sizeof(s), "{\"frames\":%lu,\"fps\":%.2f,\"w\":%u,\"h\":%u,\"audio\":%s}", (unsigned long)cache.n, cache.fps, cache.w, cache.h,
             cache.an ? "true" : "false");
    return send_json(r, s);
}

// One frame of a clip, shown while paused or scrubbing.
static esp_err_t frame_get_handler(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char path[96], v[12];
    if (file_path(r, path, sizeof(path)) != CLIP || !param(r, "n", v, sizeof(v)))
        return fail(r, "400 Bad Request", "bad request");
    uint32_t    n = atoi(v);
    sd_reader_t rd;
    uint8_t    *buf = NULL;
    esp_err_t   err;
    if (!sd_reader_open(&rd, path) || !clip_load(&cache, &rd, path) || n >= cache.n) {
        err = fail(r, "404 Not Found", "no frame");
    } else if (!(buf = heap_caps_malloc(cache.idx[n * 2 + 1], MALLOC_CAP_SPIRAM)) || !sd_reader_read(&rd, cache.idx[n * 2], buf, cache.idx[n * 2 + 1])) {
        err = fail(r, "500 Internal Server Error", "read failed");
    } else {
        httpd_resp_set_type(r, "image/jpeg");
        httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");
        err = httpd_resp_send(r, (const char *)buf, cache.idx[n * 2 + 1]);
    }
    heap_caps_free(buf);
    sd_reader_close(&rd);
    return err;
}

static esp_err_t play_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char path[96], v[12];
    if (file_path(r, path, sizeof(path)) != CLIP)
        return fail(r, "400 Bad Request", "bad clip");
    if (recorder_busy_with(path))
        return fail(r, "409 Conflict", "clip is still recording");
    uint32_t start = param(r, "n", v, sizeof(v)) ? atoi(v) : 0;
    float    speed = param(r, "speed", v, sizeof(v)) ? atof(v) : 1;
    speed          = !(speed >= 0.25f) ? 0.25f : speed > 16 ? 16 : speed; // !(>=) also catches NaN
    return start_job(r, play_task, path, start, speed);
}

static esp_err_t audio_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char path[96], v[12];
    if (file_path(r, path, sizeof(path)) != CLIP)
        return fail(r, "400 Bad Request", "bad clip");
    if (recorder_busy_with(path))
        return fail(r, "409 Conflict", "clip is still recording");
    return start_job(r, wav_task, path, param(r, "n", v, sizeof(v)) ? atoi(v) : 0, 1);
}

static esp_err_t download_get(httpd_req_t *r) {
    if (!authorized(r))
        return ESP_OK;
    char path[96];
    if (!file_path(r, path, sizeof(path)))
        return fail(r, "400 Bad Request", "bad file");
    return start_job(r, download_task, path, 0, 1);
}

static esp_err_t delete_post(httpd_req_t *r) {
    if (!mutating(r))
        return ESP_OK;
    char path[96];
    if (!file_path(r, path, sizeof(path)))
        return fail(r, "400 Bad Request", "bad file");
    if (recorder_busy_with(path))
        return fail(r, "409 Conflict", "clip is still recording");
    if (!strcmp(cache.path, path))
        clip_free(&cache);
    if (unlink(path))
        return errno == EACCES ? fail(r, "409 Conflict", "file is in use, close playback or download first") : fail(r, "404 Not Found", "not found");
    return send_json(r, "{\"ok\":true}");
}

esp_err_t web_start(void) {
    char   creds[96];
    size_t olen;
    int    n = snprintf(creds, sizeof(creds), "%s:%s", WEB_USER, WEB_PASS);
    strcpy(auth, "Basic ");
    mbedtls_base64_encode((unsigned char *)auth + 6, sizeof(auth) - 6, &olen, (const unsigned char *)creds, n);
    auth[6 + olen] = 0;

    httpd_config_t c   = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 20;
    c.max_open_sockets = 12;
    c.lru_purge_enable = true;
    c.stack_size       = 8192;
    c.core_id          = 0;
    job_q = xQueueCreate(MAX_JOBS, sizeof(job_t *));
    for (int i = 0; i < MAX_JOBS; i++)
        xTaskCreatePinnedToCoreWithCaps(worker_task, "job", 6144, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    httpd_handle_t srv;
    esp_err_t      err = httpd_start(&srv, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_get},
        {.uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/record", .method = HTTP_POST, .handler = record_post},
        {.uri = "/api/days", .method = HTTP_GET, .handler = days_get},
        {.uri = "/api/files", .method = HTTP_GET, .handler = files_get},
        {.uri = "/api/snapshot", .method = HTTP_POST, .handler = snapshot_post},
        {.uri = "/api/image", .method = HTTP_GET, .handler = image_get},
        {.uri = "/api/clip", .method = HTTP_GET, .handler = clip_get},
        {.uri = "/api/frame", .method = HTTP_GET, .handler = frame_get_handler},
        {.uri = "/api/play", .method = HTTP_GET, .handler = play_get},
        {.uri = "/api/audio", .method = HTTP_GET, .handler = audio_get},
        {.uri = "/api/download", .method = HTTP_GET, .handler = download_get},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = delete_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(srv, &uris[i]);
    return ESP_OK;
}
