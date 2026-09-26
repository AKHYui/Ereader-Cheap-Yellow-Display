#include "pv_scan.h"
#include "pv_config.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = PV_TAG;

typedef struct {
    uint32_t off;
    bool     jpeg;
} pv_entry_t;

static char       s_pool[PV_NAME_POOL];
static size_t     s_pool_used;
static pv_entry_t s_entries[PV_MAX_FILES];
static int        s_count;

static bool ext_is(const char *name, const char *ext)
{
    const size_t ln = strlen(name);
    const size_t le = strlen(ext);
    if (ln <= le) return false;
    return strcasecmp(name + (ln - le), ext) == 0;
}

static int cmp_entry(const void *a, const void *b)
{

    const pv_entry_t *x = (const pv_entry_t *)a;
    const pv_entry_t *y = (const pv_entry_t *)b;
    return strcasecmp(&s_pool[x->off], &s_pool[y->off]);
}

int pv_scan_run(void)
{
    s_count     = 0;
    s_pool_used = 0;

    DIR *d = opendir(PV_DIR);
    if (!d) {
        ESP_LOGE(TAG, "打不开目录 %s（SD 卡没挂上？）", PV_DIR);
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        if (n[0] == '.') continue;

        bool jpeg;
        if (ext_is(n, ".jpg") || ext_is(n, ".jpeg")) {
            jpeg = true;
        } else if (ext_is(n, ".bmp")) {
            jpeg = false;
        } else {
            continue;
        }

        if (s_count >= PV_MAX_FILES) {
            ESP_LOGW(TAG, "图片数超过上限 %d，其余忽略", PV_MAX_FILES);
            break;
        }

        const size_t nl = strlen(n);
        if (s_pool_used + nl + 1 > sizeof(s_pool)) {

            ESP_LOGW(TAG, "名字池满（%u 字节），从「%s」起不再列出（已列 %d 张）",
                     (unsigned)sizeof(s_pool), n, s_count);
            break;
        }

        pv_entry_t *e = &s_entries[s_count];
        e->off  = (uint32_t)s_pool_used;
        e->jpeg = jpeg;
        memcpy(&s_pool[e->off], n, nl + 1);
        s_pool_used += nl + 1;
        s_count++;
    }
    closedir(d);

    if (s_count > 1) {
        qsort(s_entries, (size_t)s_count, sizeof(pv_entry_t), cmp_entry);
    }

    size_t maxlen = 0;
    for (int i = 0; i < s_count; i++) {
        const size_t l = strlen(&s_pool[s_entries[i].off]);
        if (l > maxlen) maxlen = l;
    }

    ESP_LOGI(TAG, "扫描 %s：找到 %d 张图（名字池 %u/%u 字节，最长名字 %u 字节）",
             PV_DIR, s_count, (unsigned)s_pool_used, (unsigned)sizeof(s_pool),
             (unsigned)maxlen);
    return s_count;
}

int pv_scan_count(void)
{
    return s_count;
}

const char *pv_scan_name(int idx)
{
    if (idx < 0 || idx >= s_count) return "?";
    return &s_pool[s_entries[idx].off];
}

void pv_scan_path(int idx, char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (idx < 0 || idx >= s_count) return;

    const int n = snprintf(out, outsz, "%s/%s", PV_DIR, pv_scan_name(idx));

    if (n < 0 || (size_t)n >= outsz) {
        ESP_LOGE(TAG, "路径缓冲不够（需要 %d 字节，只有 %u），置空避免用假路径",
                 n, (unsigned)outsz);
        out[0] = '\0';
    }
}

bool pv_scan_is_jpeg(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    return s_entries[idx].jpeg;
}

#if PV_SCAN_SELFTEST

static void probe_long_names(void)
{
    static const int LENS[] = { 63, 64, 100, 200 };
    #define NPROBE ((int)(sizeof(LENS) / sizeof(LENS[0])))
    static char probe[NPROBE][PV_PATH_MAX];

    int made = 0;
    for (int i = 0; i < NPROBE; i++) {
        probe[i][0] = 0;

        char nm[208];
        int  k = snprintf(nm, sizeof(nm), "zz_probe_%d_", i);
        while (k < LENS[i] - 4) nm[k++] = 'x';
        nm[k++] = '.'; nm[k++] = 'j'; nm[k++] = 'p'; nm[k++] = 'g';
        nm[k]    = 0;
        if (k != LENS[i]) continue;

        snprintf(probe[i], sizeof(probe[0]), "%s/%s", PV_DIR, nm);

        FILE *f = fopen(probe[i], "wb");
        if (!f) {
            ESP_LOGW(TAG, "探针 %d 字节：建文件失败（卡写保护？）", LENS[i]);
            probe[i][0] = 0;
            continue;
        }
        fputc('x', f);
        fclose(f);
        made++;
    }

    ESP_LOGW(TAG, "=== 长名字探针：建了 %d 个（%d/%d/%d/%d 字节）===",
             made, LENS[0], LENS[1], LENS[2], LENS[3]);
    if (!made) return;

    const int n = pv_scan_run();

    struct stat st;
    int pass = 0;
    for (int i = 0; i < NPROBE; i++) {
        if (!probe[i][0]) continue;

        const char *base = strrchr(probe[i], '/');
        base = base ? base + 1 : probe[i];
        const bool on_disk = (stat(probe[i], &st) == 0);

        int found = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(pv_scan_name(j), base) == 0) { found = j; break; }
        }
        const bool listed = (found >= 0);
        const bool full   = listed && ((int)strlen(pv_scan_name(found)) == LENS[i]);

        static char pp[PV_PATH_MAX];
        bool path_ok = false;
        if (listed) {
            pv_scan_path(found, pp, sizeof(pp));
            path_ok = (pp[0] != '\0' && stat(pp, &st) == 0);
        }

        const bool ok = on_disk && listed && full && path_ok;
        if (ok) pass++;

        ESP_LOGW(TAG, "  %3d 字节：卡上有=%d 列表里有=%d 名字完整=%d 路径可用=%d  %s",
                 LENS[i], on_disk, listed, full, path_ok, ok ? "✓ 通过" : "✗ 失败");
    }
    ESP_LOGW(TAG, "=== 长名字结论：%d/%d 通过 ===", pass, made);

    for (int i = 0; i < NPROBE; i++) {
        if (probe[i][0]) remove(probe[i]);
    }
    pv_scan_run();
}

void pv_scan_selftest(void)
{

    probe_long_names();

    const int n = pv_scan_run();
    ESP_LOGW(TAG, "=== 扫描自检：%s 下 %d 张 ===", PV_DIR, n);
    if (n <= 0) return;

    int mi = 0;
    for (int i = 1; i < n; i++) {
        if (strlen(pv_scan_name(i)) > strlen(pv_scan_name(mi))) mi = i;
    }

    static char p[PV_PATH_MAX];
    pv_scan_path(mi, p, sizeof(p));

    struct stat st;
    const bool exists = (p[0] != '\0' && stat(p, &st) == 0);

    ESP_LOGW(TAG, "卡上最长名字 [%d/%d]：%u 字节", mi + 1, n,
             (unsigned)strlen(pv_scan_name(mi)));
    ESP_LOGW(TAG, "  %s", pv_scan_name(mi));
    ESP_LOGW(TAG, "  拼出的路径 %u 字节，stat -> %s", (unsigned)strlen(p),
             exists ? "存在 ✓ 名字完整" : "不存在 ✗ 名字被截断了！");
    ESP_LOGW(TAG, "=== 扫描自检结束 ===");
}
#endif
