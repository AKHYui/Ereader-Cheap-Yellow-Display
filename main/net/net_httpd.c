#include "net_httpd.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "net_wifi.h"

static const char *TAG = "net_httpd";

#define SD_ROOT      "/sdcard"
#define RECV_BUF     4096
#define MAX_DIRS     24
#define MAX_FILES    200
#define NAME_MAX_LEN 80

static httpd_handle_t s_srv;

static char s_buf[RECV_BUF];

static char s_cur[48];

static volatile int  s_percent = -1;
static volatile int  s_count;
static volatile bool s_busy;

void net_httpd_prog(net_httpd_prog_t *out)
{
    if (!out) return;
    out->percent = s_percent;
    out->files   = s_count;
    out->name[0] = 0;
    if (s_busy) {
        strncpy(out->name, s_cur, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = 0;
    }
}

static esp_err_t qs_get(httpd_req_t *r, const char *key, char *out, size_t n)
{
    const size_t qlen = httpd_req_get_url_query_len(r);
    if (qlen <= 0 || qlen > 256) return ESP_ERR_INVALID_ARG;

    char q[260];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return ESP_FAIL;

    char raw[192];
    if (httpd_query_key_value(q, key, raw, sizeof(raw)) != ESP_OK) return ESP_ERR_NOT_FOUND;

    size_t j = 0;
    for (size_t i = 0; raw[i] && j + 1 < n; i++) {
        if (raw[i] == '%' && isxdigit((unsigned char)raw[i + 1]) &&
            isxdigit((unsigned char)raw[i + 2])) {
            const char h[3] = { raw[i + 1], raw[i + 2], 0 };
            out[j++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (raw[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = raw[i];
        }
    }
    out[j] = 0;
    return j ? ESP_OK : ESP_FAIL;
}

static bool dir_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0 || n > 40 || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

static bool name_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0 || n > NAME_MAX_LEN || s[0] == '.') return false;
    if (strstr(s, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

static void ensure_dir(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) {
        if (mkdir(p, 0775) != 0) ESP_LOGW(TAG, "建目录失败: %s", p);
    }
}

static void json_escape(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            out[j++] = '?';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t h_root(httpd_req_t *r)
{
    const size_t n = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)index_html_start, n);
}

static esp_err_t h_dirs(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, "{\"dirs\":[");

    int n = 0;
    DIR *d = opendir(SD_ROOT);
    if (d) {
        struct dirent *e;
        char item[96];
        while ((e = readdir(d)) != NULL && n < MAX_DIRS) {
            if (e->d_name[0] == '.') continue;

            if (!dir_ok(e->d_name)) continue;

            char full[160];
            struct stat st;
            snprintf(full, sizeof(full), SD_ROOT "/%.*s", 40, e->d_name);
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            snprintf(item, sizeof(item), "%s\"%.*s\"", n ? "," : "", 40, e->d_name);
            httpd_resp_sendstr_chunk(r, item);
            n++;
        }
        closedir(d);
    }

    if (!n) httpd_resp_sendstr_chunk(r, "\"" NET_HTTPD_DEFAULT_DIR "\"");

    httpd_resp_sendstr_chunk(r, "]}");
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_list(httpd_req_t *r)
{
    char dir[48];
    if (qs_get(r, "dir", dir, sizeof(dir)) != ESP_OK || !dir_ok(dir)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad dir");
        return ESP_OK;
    }

    char path[96];
    snprintf(path, sizeof(path), SD_ROOT "/%s", dir);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    httpd_resp_sendstr_chunk(r, "{\"files\":[");

    int n = 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char nm[NAME_MAX_LEN + 1];
        char item[NAME_MAX_LEN * 2 + 64];
        char esc[NAME_MAX_LEN * 2 + 2];

        while ((e = readdir(d)) != NULL && n < MAX_FILES) {
            if (e->d_name[0] == '.') continue;

            snprintf(nm, sizeof(nm), "%.*s", NAME_MAX_LEN, e->d_name);

            char full[192];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", path, nm);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            json_escape(nm, esc, sizeof(esc));
            snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%d}",
                     n ? "," : "", esc, (int)st.st_size);
            httpd_resp_sendstr_chunk(r, item);
            n++;
        }
        closedir(d);
    }

    httpd_resp_sendstr_chunk(r, "]}");
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_upload(httpd_req_t *r)
{
    char dir[48], name[NAME_MAX_LEN + 1];
    if (qs_get(r, "dir", dir, sizeof(dir)) != ESP_OK || !dir_ok(dir)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad dir");
        return ESP_OK;
    }
    if (qs_get(r, "name", name, sizeof(name)) != ESP_OK || !name_ok(name)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    if (r->content_len <= 0) {

        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "need Content-Length");
        return ESP_OK;
    }

    char dpath[96], fpath[192];
    snprintf(dpath, sizeof(dpath), SD_ROOT "/%s", dir);
    snprintf(fpath, sizeof(fpath), "%s/%s", dpath, name);
    ensure_dir(dpath);

    FILE *f = fopen(fpath, "wb");
    if (!f) {

        char hex[3 * (NAME_MAX_LEN + 1)];
        size_t k = 0;
        for (size_t i = 0; name[i] && k + 3 < sizeof(hex); i++) {
            k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02X ", (unsigned char)name[i]);
        }
        ESP_LOGE(TAG, "打不开 %s（errno=%d，name 十六进制：%s）", fpath, errno, hex);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    const int total = (int)r->content_len;
    int left = total;

    strncpy(s_cur, name, sizeof(s_cur) - 1);
    s_cur[sizeof(s_cur) - 1] = 0;
    s_busy    = true;
    s_percent = 0;

    ESP_LOGI(TAG, "开始接收 /%s/%s（%d 字节）", dir, name, total);

    while (left > 0) {
        const int want = (left < RECV_BUF) ? left : RECV_BUF;
        const int got  = httpd_req_recv(r, s_buf, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            ESP_LOGW(TAG, "接收中断（还剩 %d 字节），删掉半截文件", left);
            fclose(f);
            unlink(fpath);
            s_busy = false; s_percent = -1; s_cur[0] = 0;
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }

        if (fwrite(s_buf, 1, (size_t)got, f) != (size_t)got) {
            ESP_LOGE(TAG, "写卡失败（卡满了？）");
            fclose(f);
            unlink(fpath);
            s_busy = false; s_percent = -1; s_cur[0] = 0;
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_OK;
        }

        left -= got;
        s_percent = (int)((int64_t)(total - left) * 100 / total);
    }

    fclose(f);

    s_busy = false;
    s_percent = -1;
    s_cur[0] = 0;
    s_count++;

    ESP_LOGI(TAG, "完成 /%s/%s，累计 %d 个", dir, name, s_count);
    httpd_resp_sendstr(r, "ok");
    return ESP_OK;
}

esp_err_t net_httpd_start(void)
{
    if (s_srv) return ESP_OK;

    if (!net_wifi_connected() && !net_wifi_ap_on()) {
        ESP_LOGW(TAG, "还没有可访问的地址（既没连 WiFi 也没开 AP），先把网络处理掉");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 6;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 5120;

    esp_err_t r = httpd_start(&s_srv, &cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(r));
        s_srv = NULL;
        return r;
    }

    static const httpd_uri_t uri_root =   { .uri = "/",          .method = HTTP_GET,  .handler = h_root };
    static const httpd_uri_t uri_index =  { .uri = "/index.html", .method = HTTP_GET, .handler = h_root };
    static const httpd_uri_t uri_dirs =   { .uri = "/api/dirs",  .method = HTTP_GET,  .handler = h_dirs };
    static const httpd_uri_t uri_list =   { .uri = "/api/list",  .method = HTTP_GET,  .handler = h_list };
    static const httpd_uri_t uri_upload = { .uri = "/upload",    .method = HTTP_POST, .handler = h_upload };

    httpd_register_uri_handler(s_srv, &uri_root);
    httpd_register_uri_handler(s_srv, &uri_index);
    httpd_register_uri_handler(s_srv, &uri_dirs);
    httpd_register_uri_handler(s_srv, &uri_list);
    httpd_register_uri_handler(s_srv, &uri_upload);

    ESP_LOGI(TAG, "文件接收服务已启动：http://%s/（%s）",
             net_wifi_connected() ? net_wifi_ip() : net_wifi_ap_ip(),
             net_wifi_connected() ? "STA" : "AP");
    net_wifi_mem("httpd 启动后");
    return ESP_OK;
}

bool net_httpd_running(void) { return s_srv != NULL; }
