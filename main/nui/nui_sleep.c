#include "nui_sleep.h"

#include "net_wifi.h"
#include "nui_ui.h"
#include "nui_whale.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"
#include "rd_font.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = PV_TAG;

#define SLP_NS      "ui"
#define SLP_K_TO    "slpt"
#define SLP_K_STYLE "slps"

static const int         TO_SEC[5] = { 60, 300, 600, 1800, 0 };
static const char *const TO_TXT[5] = { "1分钟", "5分钟", "10分钟", "30分钟", "从不" };
static const char *const ST_TXT[NUI_SLP_STYLE_N] = { "时间", "鲸鱼娘" };

#define SLP_DEF_TO     2
#define SLP_DEF_STYLE  NUI_SLP_CLOCK

static int      s_to;
static int      s_style;

static int64_t  s_awake_ms;

static int      s_test_ms;
static int      s_test_to;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void cfg_load(void)
{
    s_to       = (int)TO_SEC[SLP_DEF_TO];
    s_style    = SLP_DEF_STYLE;

    nvs_handle_t h;
    if (nvs_open(SLP_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v = 0;
    if (nvs_get_u8(h, SLP_K_TO, &v) == ESP_OK && v < 5)        s_to    = (int)TO_SEC[v];
    if (nvs_get_u8(h, SLP_K_STYLE, &v) == ESP_OK && v < NUI_SLP_STYLE_N) s_style = (int)v;
    nvs_close(h);
}

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(SLP_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打不开 NVS，屏保设置没存上");
        return;
    }

    uint8_t ti = 0;
    for (int i = 0; i < 5; i++) if (TO_SEC[i] == s_to) ti = (uint8_t)i;
    nvs_set_u8(h, SLP_K_TO, ti);
    nvs_set_u8(h, SLP_K_STYLE, (uint8_t)s_style);
    nvs_commit(h);
    nvs_close(h);
}

void nui_sleep_init(void)
{
    cfg_load();
    s_awake_ms = now_ms();
    ESP_LOGI(TAG, "屏保：%s / %s（%d 秒）", nui_sleep_timeout_text(),
             nui_sleep_style_text(), s_to);
}

int  nui_sleep_timeout(void) { return s_to; }
int  nui_sleep_style(void)   { return s_style; }

const char *nui_sleep_timeout_text(void)
{
    for (int i = 0; i < 5; i++) if (TO_SEC[i] == s_to) return TO_TXT[i];
    return TO_TXT[4];
}

const char *nui_sleep_style_text(void)
{
    return (s_style >= 0 && s_style < NUI_SLP_STYLE_N) ? ST_TXT[s_style] : ST_TXT[0];
}

void nui_sleep_next_timeout(void)
{
    int idx = 0;
    for (int i = 0; i < 5; i++) if (TO_SEC[i] == s_to) idx = i;
    idx = (idx + 1) % 5;
    s_to = TO_SEC[idx];
    cfg_save();
    ESP_LOGI(TAG, "屏保激活时间 -> %s", nui_sleep_timeout_text());
}

void nui_sleep_next_style(void)
{
    s_style = (s_style + 1) % NUI_SLP_STYLE_N;
    cfg_save();
    ESP_LOGI(TAG, "屏保样式 -> %s", nui_sleep_style_text());
}

void nui_sleep_keep_awake(void)
{
    s_awake_ms = now_ms();
}

static bool s_autoback;

static void drain_touch(void)
{

    pv_touch_ignore_until_release();
}

void nui_sleep_test_autoback(int on) { s_autoback = (on != 0); }
bool nui_sleep_test_should_back(void) { return s_autoback; }

bool nui_sleep_poll(void)
{
    const int to = (s_test_to > 0) ? s_test_to : s_to;
    if (to <= 0) return false;

    const int64_t now = now_ms();
    int64_t last = pv_touch_last_ms();
    if (s_awake_ms > last) last = s_awake_ms;

    if (now - last < (int64_t)to * 1000) return false;

    ESP_LOGI(TAG, "空闲 %d 秒（阈值 %d 秒）—— 进屏保", (int)((now - last) / 1000), to);

    pv_touch_ignore_until_release();

    nui_sleep_run_once();

    drain_touch();

    nui_sleep_keep_awake();
    return true;
}

static void clear_black(void)
{
    pv_disp_page_begin(NUI_BG);
    for (int sy = 0; sy < PV_SCR_H; sy++) {
        (void)pv_disp_page_row(sy);
    }
    pv_disp_page_end();
}

#define DIG_H 7
static const uint8_t DIG[12][DIG_H] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    { 0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00 },
};

static int dig_of(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == ':') return 11;
    return -1;
}

static int dig_w(const char *s, int sc)
{
    const int n = (int)strlen(s);
    return n > 0 ? n * 6 * sc - sc : 0;
}

static void dig_row(uint16_t *row, int sy, const char *s, int x, int y, int sc, uint16_t c)
{
    for (int i = 0; s[i]; i++) {
        const int gi = dig_of(s[i]);
        if (gi < 0) continue;
        const int gx = x + i * 6 * sc;
        for (int r = 0; r < DIG_H; r++) {
            const int py = y + r * sc;
            if (sy < py || sy >= py + sc) continue;
            const uint8_t bits = DIG[gi][r];
            if (!bits) continue;
            for (int k = 0; k < 5; k++) {
                if (!(bits & (1u << (4 - k)))) continue;
                const int px = gx + k * sc;
                nui_hspan(row, px, px + sc, c);
            }
        }
    }
}

#define CLOCK_NTP_SERVER   "ntp.aliyun.com"
#define CLOCK_CONN_MS      10000
#define CLOCK_SNTP_MS       8000
#define CLOCK_MIN_FREE     20000
#define CLOCK_TZ_OFF_S     (8 * 3600)
#define CLOCK_DS_SCALE     3
#define CLOCK_TS_SCALE     5
#define CLOCK_DATE_Y       124
#define CLOCK_TIME_Y       162

static bool s_clock_wifi;

static bool touched(void);

static void clock_draw_status(const char *msg)
{
    pv_disp_page_begin(NUI_BG);
    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;
        nui_hint_row(row, sy, 150, msg, NUI_DIM, NUI_BG);
    }
    pv_disp_page_end();
}

void nui_sleep_test_autoexit(int ms) { s_test_ms = ms; }
void nui_sleep_test_timeout(int s)   { s_test_to = s; }

static void clock_draw(void)
{
    const time_t utc = time(NULL) + CLOCK_TZ_OFF_S;
    struct tm tm;
    gmtime_r(&utc, &tm);

    char d[40], t[40];
    snprintf(d, sizeof(d), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(t, sizeof(t), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

    const int dw = dig_w(d, CLOCK_DS_SCALE);
    const int tw = dig_w(t, CLOCK_TS_SCALE);

    pv_disp_page_begin(NUI_BG);
    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        dig_row(row, sy, d, (PV_SCR_W - dw) / 2, CLOCK_DATE_Y, CLOCK_DS_SCALE, 0x9DBF);
        dig_row(row, sy, t, (PV_SCR_W - tw) / 2, CLOCK_TIME_Y, CLOCK_TS_SCALE, 0xFFFF);
    }
    pv_disp_page_end();
}

static bool clock_prepare(void)
{

    const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeb < CLOCK_MIN_FREE) {
        ESP_LOGW(TAG, "空闲堆只有 %u < %d —— 不起网络，改用鲸鱼娘",
                 (unsigned)freeb, CLOCK_MIN_FREE);
        return false;
    }

    if (!net_wifi_connected()) {
        char ssid[NET_WIFI_SSID_LEN];
        char pass[NET_WIFI_PASS_LEN];
        if (!net_wifi_load_saved(ssid, sizeof(ssid), pass, sizeof(pass))) {
            ESP_LOGW(TAG, "没有保存过 WiFi —— 取不到网络时间");
            return false;
        }
        clock_draw_status("正在连接网络");
        if (net_wifi_start() != ESP_OK) return false;

        if (net_wifi_connect(ssid, pass, false) != ESP_OK) {
            ESP_LOGW(TAG, "发起连接失败");
            return false;
        }
        s_clock_wifi = true;

        net_wifi_mem("屏保起 WiFi 后");
        for (int i = 0; i < CLOCK_CONN_MS / 100; i++) {
            if (net_wifi_connected()) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!net_wifi_connected()) {
        ESP_LOGW(TAG, "连不上（%s）—— 拿不到网络时间", net_wifi_state_text());
        return false;
    }
    ESP_LOGI(TAG, "网络就绪 %s，开始对时", net_wifi_ip());

    clock_draw_status("正在读取网络时间");
    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CLOCK_NTP_SERVER);
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 起不来");
        return false;
    }
    const esp_err_t r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CLOCK_SNTP_MS));
    esp_netif_sntp_deinit();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "对时失败：%s", esp_err_to_name(r));
        return false;
    }
    ESP_LOGI(TAG, "对时成功，UTC=%lld", (long long)time(NULL));
    return true;
}

static void clock_release(void)
{
    if (s_clock_wifi) {
        s_clock_wifi = false;

        net_wifi_shutdown();
        net_wifi_mem("屏保退出、WiFi 还回去（立刻）");

        vTaskDelay(pdMS_TO_TICKS(600));
        net_wifi_mem("屏保退出、WiFi 还回去（600ms 后）");
    }
}

static void clock_loop(void)
{
    time_t        last = 0;
    const int64_t t0   = now_ms();

    while (!touched()) {
        if (s_test_ms > 0 && now_ms() - t0 > s_test_ms) {
            ESP_LOGW(TAG, "自检：到点自动退出屏保");
            return;
        }
        const time_t t = time(NULL);
        if (t != last) {
            last = t;
            clock_draw();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#define WH_BUBBLES   5
#define WH_TICK_MS    60

static bool touched(void)
{
    pv_touch_evt_t ev;
    return pv_touch_take(&ev);
}

static void bubble_row(uint16_t *row, int sy, int cx, int cy, int r)
{
    if (sy < cy - r || sy > cy + r) return;
    const int dy = sy - cy;
    const int half = r * r - dy * dy;
    if (half <= 0) return;
    int w = r;
    while (w > 0 && w * w > half) w--;
    nui_hspan(row, cx - w, cx + w, nui_whale_cyan());
    if (sy == cy - r / 2) nui_hspan(row, cx - r / 2, cx - r / 2 + 2, 0xFFFF);
}

static void whale_draw(int off, int frame, int64_t el)
{
    const int s  = NUI_WHALE_SCALE;
    const int px = NUI_WHALE_N * s;
    const int ox = (PV_SCR_W - px) / 2;
    const int oy = (PV_SCR_H - px) / 2 + off;

    pv_disp_page_begin(NUI_BG);
    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_whale_row(row, sy, ox, oy, frame);

        for (int i = 0; i < WH_BUBBLES; i++) {
            const int period = 2600 + i * 700;
            const int ph     = (int)((el + i * 900) % period);
            const int y      = 300 - ph * 285 / period;
            const int x      = 14 + ((i * 61) % 212);
            const int r      = (i % 3) ? 3 : 2;
            bubble_row(row, sy, x, y, r);
        }
    }
    pv_disp_page_end();
}

static void whale_loop(void)
{
    static const int bob[4] = { 0, -1, 0, 1 };
    const int64_t t0 = now_ms();

    for (;;) {
        if (touched()) return;

        const int64_t el = now_ms() - t0;
        if (s_test_ms > 0 && el > s_test_ms) {
            ESP_LOGW(TAG, "自检：到点自动退出屏保");
            return;
        }
        const int off    = bob[(el / 500) % 4] * NUI_WHALE_SCALE;
        const int frame  = ((el % 3000) < 160) ? 1 : 0;

        whale_draw(off, frame, el);
        vTaskDelay(pdMS_TO_TICKS(WH_TICK_MS));
    }
}

void nui_sleep_run_once(void)
{
    int style = s_style;

    if (style == NUI_SLP_CLOCK) {

        clear_black();
        if (!clock_prepare()) {
            ESP_LOGW(TAG, "拿不到网络时间 —— 按规则强制改用鲸鱼娘");
            style = NUI_SLP_WHALE;
        }
    }

    if (style == NUI_SLP_CLOCK) {
        ESP_LOGI(TAG, "屏保 1（时间）开始");
        clock_loop();
    } else {
        ESP_LOGI(TAG, "屏保 2（鲸鱼娘）开始");
        whale_loop();
    }

    clock_release();
    ESP_LOGI(TAG, "屏保结束");
}
