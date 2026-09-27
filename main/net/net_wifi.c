#include "net_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "net_wifi";

#define NVS_NS      "nuiwifi"
#define NVS_K_SSID  "ssid"
#define NVS_K_PASS  "pass"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool         s_tried;
static esp_err_t    s_start_rc = ESP_FAIL;
static esp_netif_t *s_netif;
static esp_netif_t *s_ap_netif;

static esp_event_handler_instance_t s_h_wifi;
static esp_event_handler_instance_t s_h_ip;

static net_wifi_state_t s_state = NET_WIFI_OFF;
static char             s_ssid[NET_WIFI_SSID_LEN];
static char             s_ip[16];
static char             s_try_ssid[NET_WIFI_SSID_LEN];

static bool             s_ap_on;
static char             s_ap_ssid[NET_WIFI_SSID_LEN];
static char             s_ap_ip[16];
static int              s_ap_clients;

static wifi_ap_record_t s_recs[NET_WIFI_MAX_AP];

static esp_err_t nvs_put(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_str(h, key, val);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

static bool nvs_get(const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = n;
    const esp_err_t r = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    if (r != ESP_OK) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool net_wifi_load_saved(char *ssid, size_t n, char *pass, size_t m)
{
    if (!ssid || !pass) return false;
    if (!nvs_get(NVS_K_SSID, ssid, n)) return false;
    if (!nvs_get(NVS_K_PASS, pass, m)) pass[0] = 0;
    return ssid[0] != 0;
}

bool net_wifi_is_saved(const char *ssid)
{
    if (!ssid || !ssid[0]) return false;
    char buf[NET_WIFI_SSID_LEN];
    return nvs_get(NVS_K_SSID, buf, sizeof(buf)) && strcmp(buf, ssid) == 0;
}

void net_wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_K_SSID);
    nvs_erase_key(h, NVS_K_PASS);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "已删除保存的 WiFi 凭据");
}

static void save_creds(const char *ssid, const char *pass)
{
    if (nvs_put(NVS_K_SSID, ssid) != ESP_OK) {
        ESP_LOGW(TAG, "凭据写 NVS 失败（SSID）");
        return;
    }
    if (nvs_put(NVS_K_PASS, pass ? pass : "") != ESP_OK) {
        ESP_LOGW(TAG, "凭据写 NVS 失败（密码）");
        return;
    }
    ESP_LOGI(TAG, "凭据已保存：%s", ssid);
}

static void set_state(net_wifi_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_state = st;
    portEXIT_CRITICAL(&s_mux);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 就绪");
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        portENTER_CRITICAL(&s_mux);
        s_ip[0] = 0;
        s_state = NET_WIFI_IDLE;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "断开（reason=%d）", d ? (int)d->reason : -1);
        return;
    }

    if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP 已启动");
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        portENTER_CRITICAL(&s_mux);
        s_ap_clients++;
        const int n = s_ap_clients;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "一台设备连上 AP（现在 %d 台）", n);
        return;
    }
    if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        portENTER_CRITICAL(&s_mux);
        if (s_ap_clients > 0) s_ap_clients--;
        const int n = s_ap_clients;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "一台设备离开 AP（剩 %d 台）", n);
        return;
    }
}

static void refresh_ap_ip(void)
{
    char tmp[16] = { 0 };

    if (s_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(tmp, sizeof(tmp), IPSTR, IP2STR(&ip.ip));
        }
    }
    if (!tmp[0]) {

        snprintf(tmp, sizeof(tmp), "192.168.4.1");
    }

    portENTER_CRITICAL(&s_mux);
    memcpy(s_ap_ip, tmp, sizeof(s_ap_ip));
    portEXIT_CRITICAL(&s_mux);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *e = data;
    char tmp[16];
    snprintf(tmp, sizeof(tmp), IPSTR, IP2STR(&e->ip_info.ip));

    portENTER_CRITICAL(&s_mux);
    memcpy(s_ip, tmp, sizeof(s_ip));
    memcpy(s_ssid, s_try_ssid, sizeof(s_ssid));
    s_state = NET_WIFI_CONNECTED;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "已连上 %s，IP %s", s_ssid, s_ip);
}

esp_err_t net_wifi_start(void)
{
    if (s_tried) return s_start_rc;
    s_tried = true;

    esp_err_t r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(r));
        s_start_rc = r;
        return r;
    }

    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(r));
        s_start_rc = r;
        return r;
    }

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            s_start_rc = ESP_FAIL;
            return s_start_rc;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init(&cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(r));
        s_start_rc = r;
        return r;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, &s_h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, &s_h_ip));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    r = esp_wifi_start();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(r));
        s_start_rc = r;
        return r;
    }

    set_state(NET_WIFI_IDLE);
    s_start_rc = ESP_OK;
    net_wifi_mem("wifi 启动后");
    return ESP_OK;
}

void net_wifi_shutdown(void)
{
    if (!s_tried || s_start_rc != ESP_OK) return;
    if (net_wifi_ap_on()) net_wifi_ap_stop();

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_h_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        s_h_wifi = NULL;
    }
    if (s_h_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        s_h_ip = NULL;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    portENTER_CRITICAL(&s_mux);
    s_ap_on      = false;
    s_ap_clients = 0;
    s_ap_ssid[0] = 0;
    s_ap_ip[0]   = 0;
    s_ssid[0]    = 0;
    s_ip[0]      = 0;
    portEXIT_CRITICAL(&s_mux);

    s_tried    = false;
    s_start_rc = ESP_FAIL;
    set_state(NET_WIFI_OFF);
    net_wifi_mem("wifi 已关闭");
}

bool net_wifi_started(void) { return s_tried && s_start_rc == ESP_OK; }

net_wifi_state_t net_wifi_state(void)
{
    portENTER_CRITICAL(&s_mux);
    const net_wifi_state_t st = s_state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

bool net_wifi_connected(void) { return net_wifi_state() == NET_WIFI_CONNECTED; }

int net_wifi_rssi(void)
{

    if (!s_tried || s_start_rc != ESP_OK) return 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

const char *net_wifi_state_text(void)
{
    switch (net_wifi_state()) {
    case NET_WIFI_OFF:       return "未启动";
    case NET_WIFI_SCANNING:  return "正在扫描";
    case NET_WIFI_CONNECTED: return "已连接";
    default:                 return "未连接";
    }
}

const char *net_wifi_ssid(void)
{
    static char out[NET_WIFI_SSID_LEN];
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_ssid, sizeof(out));
    portEXIT_CRITICAL(&s_mux);
    return out;
}

const char *net_wifi_ip(void)
{
    static char out[16];
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_ip, sizeof(out));
    portEXIT_CRITICAL(&s_mux);
    return out;
}

static void sort_by_rssi(net_ap_t *a, int n)
{

    for (int i = 1; i < n; i++) {
        const net_ap_t key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].rssi < key.rssi) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

int net_wifi_scan(net_ap_t *out, int max)
{
    if (!out || max <= 0) return -1;
    if (net_wifi_start() != ESP_OK) return -1;

    const net_wifi_state_t before = net_wifi_state();
    if (before != NET_WIFI_CONNECTED) set_state(NET_WIFI_SCANNING);

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;

    esp_err_t r = esp_wifi_scan_start(&sc, true);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(r));
        if (before != NET_WIFI_CONNECTED) set_state(NET_WIFI_IDLE);
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    uint16_t got = (num > NET_WIFI_MAX_AP) ? NET_WIFI_MAX_AP : num;
    if (got) {

        esp_wifi_scan_get_ap_records(&got, s_recs);
    }

    int cnt = 0;
    for (int i = 0; i < got && cnt < max; i++) {
        const uint8_t *raw = s_recs[i].ssid;
        int len = 0;

        while (len < 32 && raw[len]) len++;

        net_ap_t *e = &out[cnt];
        memset(e, 0, sizeof(*e));
        if (len == 0) {
            snprintf(e->ssid, sizeof(e->ssid), "(hidden)");
        } else {
            const int n = (len < (int)sizeof(e->ssid) - 1) ? len : (int)sizeof(e->ssid) - 1;
            memcpy(e->ssid, raw, (size_t)n);
            e->ssid[n] = 0;
        }
        e->rssi   = s_recs[i].rssi;
        e->secure = (s_recs[i].authmode != WIFI_AUTH_OPEN);
        cnt++;
    }

    sort_by_rssi(out, cnt);

    if (before == NET_WIFI_CONNECTED) set_state(NET_WIFI_CONNECTED);
    else                              set_state(NET_WIFI_IDLE);

    ESP_LOGI(TAG, "扫描完成：%d 个 AP（信道结果 %u 条）", cnt, (unsigned)num);
    return cnt;
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass, bool save)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (net_wifi_start() != ESP_OK) return ESP_FAIL;

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }

    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_disconnect();

    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "set_config 失败: %s", esp_err_to_name(r));
        return r;
    }

    portENTER_CRITICAL(&s_mux);
    strncpy(s_try_ssid, ssid, sizeof(s_try_ssid) - 1);
    s_try_ssid[sizeof(s_try_ssid) - 1] = 0;
    s_ip[0] = 0;
    portEXIT_CRITICAL(&s_mux);

    r = esp_wifi_connect();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "connect 失败: %s", esp_err_to_name(r));
        return r;
    }

    if (save) save_creds(ssid, pass);

    ESP_LOGI(TAG, "正在连接 %s%s", ssid, (pass && pass[0]) ? "" : "（开放网络）");
    return ESP_OK;
}

void net_wifi_disconnect(void)
{
    if (!net_wifi_started()) return;
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "已断开");
}

void net_wifi_ap_default_ssid(char *buf, size_t n)
{
    if (!buf || n < 4) return;

    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        snprintf(buf, n, "CYD-0000");
        return;
    }

    snprintf(buf, n, "CYD-%02X%02X", mac[4], mac[5]);
}

esp_err_t net_wifi_ap_start(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (net_wifi_start() != ESP_OK) return ESP_FAIL;

    portENTER_CRITICAL(&s_mux);
    const bool already = s_ap_on;
    portEXIT_CRITICAL(&s_mux);
    if (already) return ESP_OK;

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "创建 AP netif 失败");
            return ESP_FAIL;
        }
    }

    esp_err_t r = esp_wifi_set_mode(WIFI_MODE_APSTA);
    const bool  apsta = (r == ESP_OK);
    if (!apsta) {
        ESP_LOGW(TAG, "APSTA 起不来（%s），退回纯 AP 模式", esp_err_to_name(r));
        r = esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "设 AP 模式失败: %s", esp_err_to_name(r));
        return r;
    }

    const bool have_pass = (pass && strlen(pass) >= NET_WIFI_AP_PASS_MIN);

    wifi_config_t ac;
    memset(&ac, 0, sizeof(ac));
    strncpy((char *)ac.ap.ssid, ssid, sizeof(ac.ap.ssid) - 1);
    ac.ap.ssid_len = (uint8_t)strlen((char *)ac.ap.ssid);
    if (have_pass) {
        strncpy((char *)ac.ap.password, pass, sizeof(ac.ap.password) - 1);
    }
    ac.ap.channel        = 1;
    ac.ap.max_connection = 4;

    ac.ap.authmode       = have_pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    r = esp_wifi_set_config(WIFI_IF_AP, &ac);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "配 AP 失败: %s", esp_err_to_name(r));
        return r;
    }

    portENTER_CRITICAL(&s_mux);
    strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = 0;
    s_ap_on      = true;
    s_ap_clients = 0;
    portEXIT_CRITICAL(&s_mux);

    refresh_ap_ip();

    ESP_LOGI(TAG, "AP 已开启：%s / %s（%s，IP %s）",
             ssid, have_pass ? pass : "(开放)", apsta ? "APSTA" : "AP",
             net_wifi_ap_ip());
    net_wifi_mem("AP 开启后");
    return ESP_OK;
}

void net_wifi_ap_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool on = s_ap_on;
    portEXIT_CRITICAL(&s_mux);
    if (!on) return;

    if (net_wifi_started()) (void)esp_wifi_set_mode(WIFI_MODE_STA);

    portENTER_CRITICAL(&s_mux);
    s_ap_on      = false;
    s_ap_clients = 0;
    s_ap_ip[0]   = 0;
    s_ap_ssid[0] = 0;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "AP 已关闭（回到 STA 模式）");
}

bool net_wifi_ap_on(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool on = s_ap_on;
    portEXIT_CRITICAL(&s_mux);
    return on;
}

const char *net_wifi_ap_ssid(void)
{
    static char out[NET_WIFI_SSID_LEN];
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_ap_ssid, sizeof(out));
    portEXIT_CRITICAL(&s_mux);
    return out;
}

const char *net_wifi_ap_ip(void)
{
    static char out[16];
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_ap_ip, sizeof(out));
    portEXIT_CRITICAL(&s_mux);
    return out;
}

int net_wifi_ap_clients(void)
{
    portENTER_CRITICAL(&s_mux);
    const int n = s_ap_clients;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void net_wifi_mem(const char *where)
{
    ESP_LOGI(TAG, "[%s] 空闲堆 %u，最大可分配块 %u",
             where,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

#if NET_BOOT_SELFTEST
void net_wifi_selftest(void)
{
    ESP_LOGI(TAG, "=== WiFi 开机自检 ===");
    net_wifi_mem("自检前");

    if (net_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 启动失败，自检终止");
        return;
    }
    net_wifi_mem("wifi 启动后");

    static net_ap_t aps[NET_WIFI_MAX_AP];
    const int n = net_wifi_scan(aps, NET_WIFI_MAX_AP);

    if (n <= 0) {
        ESP_LOGW(TAG, "一个 AP 都没扫到（n=%d）—— 天线、还是周围没热点？", n);
    } else {
        ESP_LOGI(TAG, "扫到 %d 个 AP：", n);
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  %2d  %-24s  %4d dBm  %s",
                     i, aps[i].ssid, (int)aps[i].rssi,
                     aps[i].secure ? "加密" : "开放");
        }
    }
    net_wifi_mem("扫描后");
    ESP_LOGI(TAG, "=== WiFi 自检结束 ===");
}
#endif
