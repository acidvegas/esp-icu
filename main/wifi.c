// ESP-ICU - Developed by acidvegas in C (https://github.com/acidvegas)
// wifi.c
//
// Wi-Fi station with up to 10 saved networks, plus an access point toggled by the BOOT button.
// One task owns the radio: it joins the strongest saved network in range, and serves scans and joins
// asked for by the web UI between attempts. Saved networks that a scan cannot see are still tried,
// since hidden networks never show up by name.

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "espicu.h"
#include "nvs.h"

#define MAX_NETS  10
#define MAX_SCAN  20
#define BOOT_GPIO GPIO_NUM_0

#define B_IP        BIT0 // station has an address
#define B_DISC      BIT1 // station lost its network
#define B_BTN       BIT2 // BOOT pressed: toggle the access point
#define B_SCAN      BIT3 // web UI wants a scan
#define B_SCAN_DONE BIT4
#define B_JOIN      BIT5 // web UI wants to join join_ssid
#define B_REQ       (B_BTN | B_SCAN | B_JOIN)

static const char *TAG = "wifi";

typedef struct {
    char ssid[33];
    char pass[65];
} net_t;

static EventGroupHandle_t ev;
static SemaphoreHandle_t  lock; // guards everything below
static esp_netif_t       *sta, *ap;
static net_t              nets[MAX_NETS]; // most recently connected first
static int                nnets;
static wifi_ap_record_t   recs[MAX_SCAN];
static uint16_t           nrecs;
static net_t              join, last; // last: the network joined most recently, wifi task only
static char               cur[33], msg[96];
static volatile bool      ap_on;

// Measured with the stock flat antenna: uploads ran at 20-34 KB/s at 19-20 dBm and 525-667 KB/s at
// 8-18 dBm. The cap is a setting (default 15 dBm) so other antennas can be tuned from the UI.
void wifi_apply_power(void) {
    int dbm = cfg.tx_power < 8 ? 8 : cfg.tx_power > 20 ? 20 : cfg.tx_power;
    esp_wifi_set_max_tx_power(dbm * 4);
}

static void set_msg(const char *fmt, const char *ssid) {
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(msg, sizeof(msg), fmt, ssid);
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", msg);
}

static void nets_save(void) {
    nvs_handle_t h;
    if (nvs_open("espicu", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_blob(h, "nets", nets, nnets * sizeof(net_t));
    nvs_commit(h);
    nvs_close(h);
}

// The config.h network seeds the list until one has been saved.
static void nets_load(void) {
    nvs_handle_t h;
    size_t       len = sizeof(nets);
    if (nvs_open("espicu", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, "nets", nets, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            nnets = len / sizeof(net_t);
            return;
        }
    }
    if (WIFI_SSID[0]) {
        strlcpy(nets[0].ssid, WIFI_SSID, sizeof(nets[0].ssid));
        strlcpy(nets[0].pass, WIFI_PASS, sizeof(nets[0].pass));
        nnets = 1;
    }
}

static int nets_find(const char *ssid) {
    for (int i = 0; i < nnets; i++)
        if (!strcmp(nets[i].ssid, ssid))
            return i;
    return -1;
}

// Moves (or adds) a network to the front of the list, dropping the oldest past MAX_NETS.
void wifi_add(const char *ssid, const char *pass) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int i = nets_find(ssid);
    if (!i && !strcmp(nets[0].pass, pass)) { // already first: skip the flash write
        xSemaphoreGive(lock);
        return;
    }
    if (i < 0)
        i = nnets < MAX_NETS ? nnets++ : MAX_NETS - 1;
    memmove(&nets[1], &nets[0], i * sizeof(net_t));
    strlcpy(nets[0].ssid, ssid, sizeof(nets[0].ssid));
    strlcpy(nets[0].pass, pass, sizeof(nets[0].pass));
    nets_save();
    xSemaphoreGive(lock);
}

void wifi_forget(const char *ssid) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int i = nets_find(ssid);
    if (i >= 0) {
        memmove(&nets[i], &nets[i + 1], (--nnets - i) * sizeof(net_t));
        nets_save();
    }
    xSemaphoreGive(lock);
}

void wifi_join(const char *ssid, const char *pass) {
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(join.ssid, ssid, sizeof(join.ssid));
    strlcpy(join.pass, pass, sizeof(join.pass));
    xSemaphoreGive(lock);
    xEventGroupSetBits(ev, B_JOIN);
}

bool wifi_ap_on(void) {
    return ap_on;
}

static void do_scan(void) {
    wifi_scan_config_t sc = {0};
    uint16_t           n  = MAX_SCAN;
    esp_err_t          err = esp_wifi_scan_start(&sc, true); // seconds long, so not under the lock
    xSemaphoreTake(lock, portMAX_DELAY);
    nrecs = err == ESP_OK && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK ? n : 0;
    xSemaphoreGive(lock);
}

// One connection attempt. False if it failed or BOOT was pressed while waiting.
static bool try_join(const net_t *n) {
    esp_wifi_disconnect();
    xEventGroupClearBits(ev, B_IP | B_DISC);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, n->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, n->pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; // hidden networks need every channel probed
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    set_msg("joining %s", n->ssid);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(ev, B_IP | B_DISC | B_BTN, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (b & B_IP) {
        xSemaphoreTake(lock, portMAX_DELAY);
        strlcpy(cur, n->ssid, sizeof(cur));
        xSemaphoreGive(lock);
        last = *n;
        wifi_add(n->ssid, n->pass);
        set_msg("connected to %s", n->ssid);
        return true;
    }
    esp_wifi_disconnect();
    set_msg("could not join %s", n->ssid);
    return false;
}

// Tries the saved networks, strongest in range first, then the ones the scan did not see.
static void auto_connect(void) {
    net_t cand[MAX_NETS];
    int   rssi[MAX_NETS], n;
    do_scan();
    xSemaphoreTake(lock, portMAX_DELAY);
    n = nnets;
    for (int i = 0; i < n; i++) {
        cand[i] = nets[i];
        rssi[i] = -1000;
        for (int k = 0; k < nrecs; k++)
            if (!strcmp((char *)recs[k].ssid, cand[i].ssid) && recs[k].rssi > rssi[i])
                rssi[i] = recs[k].rssi;
    }
    xSemaphoreGive(lock);
    for (int i = 1; i < n; i++) // insertion sort keeps list order among equals
        for (int k = i; k > 0 && rssi[k] > rssi[k - 1]; k--) {
            net_t t = cand[k];
            int   r = rssi[k];
            cand[k] = cand[k - 1], rssi[k] = rssi[k - 1];
            cand[k - 1] = t, rssi[k - 1] = r;
        }
    for (int i = 0; i < n; i++) {
        if (xEventGroupGetBits(ev) & B_REQ)
            return;
        if (try_join(&cand[i]))
            return;
    }
    if (!n)
        set_msg("no saved networks, press BOOT for the access point%s", "");
    xEventGroupWaitBits(ev, B_REQ, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
}

static void ap_set(bool on) {
    ap_on = on;
    if (on) {
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifi_config_t wc = {.ap = {.ssid = AP_SSID, .ssid_len = strlen(AP_SSID), .password = AP_PASS, .channel = 1,
                                   .authmode = WIFI_AUTH_WPA2_PSK, .max_connection = 4}};
        esp_wifi_set_config(WIFI_IF_AP, &wc);
        set_msg("access point %s is on, http://192.168.4.1/", AP_SSID);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        set_msg("access point off%s", "");
    }
}

static void wifi_task(void *arg) {
    for (;;) {
        EventBits_t b = xEventGroupGetBits(ev);
        if (b & B_BTN) {
            xEventGroupClearBits(ev, B_BTN);
            ap_set(!ap_on);
        } else if (b & B_SCAN) {
            xEventGroupClearBits(ev, B_SCAN);
            do_scan();
            xEventGroupSetBits(ev, B_SCAN_DONE);
        } else if (b & B_JOIN) {
            xEventGroupClearBits(ev, B_JOIN);
            net_t n;
            xSemaphoreTake(lock, portMAX_DELAY);
            n     = join;
            int i = nets_find(n.ssid);
            if (!n.pass[0] && i >= 0) // joining a saved network from the list
                strlcpy(n.pass, nets[i].pass, sizeof(n.pass));
            xSemaphoreGive(lock);
            if (try_join(&n) && ap_on) {
                // Leave time for the page on the access point to show where the camera went.
                set_msg("connected to %s, access point off in 10 seconds", n.ssid);
                vTaskDelay(pdMS_TO_TICKS(10000));
                ap_set(false);
            }
        } else if (!ap_on && !(b & B_IP)) {
            auto_connect();
        } else {
            if (!(xEventGroupWaitBits(ev, B_REQ | B_DISC, pdFALSE, pdFALSE, portMAX_DELAY) & B_DISC))
                continue;
            xEventGroupClearBits(ev, B_DISC);
            // A dropped network usually comes back (router reboot, brief interference), so retry it
            // for 30 seconds before the full scan. Skipped if it was forgotten while connected.
            xSemaphoreTake(lock, portMAX_DELAY);
            bool saved = nets_find(last.ssid) >= 0;
            xSemaphoreGive(lock);
            int64_t end = esp_timer_get_time() + 30000000;
            while (saved && !ap_on && esp_timer_get_time() < end && !(xEventGroupGetBits(ev) & B_REQ) && !try_join(&last))
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// BOOT is active low with the board's pull-up; it is only a strapping pin at reset.
static void button_task(void *arg) {
    gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY);
    for (int held = 0;; vTaskDelay(pdMS_TO_TICKS(20))) {
        if (gpio_get_level(BOOT_GPIO)) {
            held = 0;
        } else if (++held == 3) { // 60 ms debounce, once per press
            xEventGroupSetBits(ev, B_BTN);
        }
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 20 MHz beats 40 MHz on the crowded 2.4 GHz band.
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
        wifi_apply_power();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        xSemaphoreTake(lock, portMAX_DELAY);
        cur[0] = 0;
        xSemaphoreGive(lock);
        xEventGroupClearBits(ev, B_IP);
        if (e->reason != WIFI_REASON_ASSOC_LEAVE) { // ASSOC_LEAVE is our own esp_wifi_disconnect()
            ESP_LOGW(TAG, "disconnected (reason %d)", e->reason);
            xEventGroupSetBits(ev, B_DISC);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "online: http://" IPSTR "/ (http://%s.local/)", IP2STR(&e->ip_info.ip), HOSTNAME);
        xEventGroupSetBits(ev, B_IP);
    }
}

// Appends str as a JSON string. Callers size the buffer for the worst case (6 bytes per input byte).
static size_t json_str(char *s, size_t len, const char *str) {
    s[len++] = '"';
    for (; *str; str++) {
        unsigned char c = *str;
        if (c == '"' || c == '\\')
            s[len++] = '\\', s[len++] = c;
        else if (c < 0x20)
            len += sprintf(s + len, "\\u%04x", c);
        else
            s[len++] = c;
    }
    s[len++] = '"';
    s[len]   = 0;
    return len;
}

// {"ap":..,"ssid":..,"ip":..,"msg":..,"saved":[..]}. Needs about 3 KB.
void wifi_json(char *s) {
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(sta, &ip);
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t len = sprintf(s, "{\"ap\":%s,\"ssid\":", ap_on ? "true" : "false");
    len        = json_str(s, len, cur);
    len += sprintf(s + len, ",\"ip\":\"" IPSTR "\",\"msg\":", IP2STR(&ip.ip));
    len = json_str(s, len, msg);
    len += sprintf(s + len, ",\"saved\":[");
    for (int i = 0; i < nnets; i++) {
        if (i)
            s[len++] = ',';
        len = json_str(s, len, nets[i].ssid);
    }
    strcpy(s + len, "]}");
    xSemaphoreGive(lock);
}

// Asks the Wi-Fi task for a scan and lists what it found, one entry per name, strongest first.
// Needs about 6 KB. Blocks until the scan is done (a few seconds, longer if a join is in progress).
void wifi_scan_json(char *s) {
    xEventGroupClearBits(ev, B_SCAN_DONE);
    xEventGroupSetBits(ev, B_SCAN);
    xEventGroupWaitBits(ev, B_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(25000));
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t len = 0;
    s[len++]   = '[';
    for (int i = 0; i < nrecs; i++) {
        const char *ssid = (char *)recs[i].ssid;
        bool        dup  = !*ssid;
        for (int k = 0; k < i && !dup; k++)
            dup = !strcmp(ssid, (char *)recs[k].ssid); // records come sorted by signal
        if (dup)
            continue;
        if (len > 1)
            s[len++] = ',';
        len += sprintf(s + len, "{\"ssid\":");
        len = json_str(s, len, ssid);
        len += sprintf(s + len, ",\"rssi\":%d,\"open\":%s,\"saved\":%s}", recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false",
                       nets_find(ssid) >= 0 ? "true" : "false");
    }
    strcpy(s + len, "]");
    xSemaphoreGive(lock);
}

void wifi_start(void) {
    ev   = xEventGroupCreate();
    lock = xSemaphoreCreateMutex();
    nets_load();
    esp_netif_init();
    esp_event_loop_create_default();
    sta = esp_netif_create_default_wifi_sta();
    ap  = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, HOSTNAME);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    // Modem sleep delays ACKs to beacon intervals, which throttles streaming.
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    mdns_init();
    mdns_hostname_set(HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    xTaskCreate(wifi_task, "wifi", 5120, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);
}
