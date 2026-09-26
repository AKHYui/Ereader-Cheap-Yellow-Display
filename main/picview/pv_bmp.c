#include "pv_bmp.h"
#include "pv_config.h"
#include "pv_disp.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = PV_TAG;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool pv_bmp_probe(const char *path)
{
    if (!path || !path[0]) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t h[2] = { 0, 0 };
    const size_t n = fread(h, 1, 2, f);
    fclose(f);

    return (n == 2 && h[0] == 'B' && h[1] == 'M');
}

pv_img_err_t pv_bmp_show(const char *path)
{
    if (!path || !path[0]) return PV_IMG_ERR_OPEN;

    const int64_t t0 = esp_timer_get_time();

    FILE *f = fopen(path, "rb");
    if (!f) return PV_IMG_ERR_OPEN;

    uint8_t fh[14];
    if (fread(fh, 1, 14, f) != 14 || fh[0] != 'B' || fh[1] != 'M') {
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }
    const uint32_t data_off = le32(fh + 10);

    uint8_t ih[40];
    if (fread(ih, 1, 40, f) != 40) {
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }
    const uint32_t hdr_size = le32(ih + 0);
    if (hdr_size < 40) {
        ESP_LOGE(TAG, "BMP 信息头 %u 字节，不支持（只认 40 字节起的 BITMAPINFOHEADER）",
                 (unsigned)hdr_size);
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }

    const int32_t  Wraw   = (int32_t)le32(ih + 4);
    const int32_t  Hraw   = (int32_t)le32(ih + 8);
    const uint16_t planes = le16(ih + 12);
    const uint16_t bpp    = le16(ih + 14);
    const uint32_t compr  = le32(ih + 16);

    if (Wraw <= 0 || Hraw == 0 || planes != 1) {
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }
    if (compr != 0) {
        ESP_LOGE(TAG, "BMP 压缩方式 %u 不支持（只支持 BI_RGB 未压缩）",
                 (unsigned)compr);
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }
    if (bpp != 24 && bpp != 32) {
        ESP_LOGE(TAG, "BMP %u 位不支持（只支持 24 / 32 位；带调色板的 1/4/8 位不认）",
                 (unsigned)bpp);
        fclose(f);
        return PV_IMG_ERR_FORMAT;
    }

    const int      W         = (int)Wraw;
    const int      H         = (Hraw < 0) ? (int)(-Hraw) : (int)Hraw;
    const bool     bottom_up = (Hraw > 0);
    const int      px_bytes  = (int)(bpp / 8);
    const uint32_t row_bytes = (((uint32_t)W * (uint32_t)px_bytes) + 3u) & ~3u;

    int tw, th;
    if (W <= PV_SCR_W && H <= PV_IMG_H) {
        tw = W;
        th = H;
    } else if ((int64_t)W * PV_IMG_H > (int64_t)H * PV_SCR_W) {
        tw = PV_SCR_W;
        th = (int)((int64_t)H * PV_SCR_W / W);
    } else {
        th = PV_IMG_H;
        tw = (int)((int64_t)W * PV_IMG_H / H);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    uint8_t  *raw  = (uint8_t *)malloc(row_bytes);
    uint16_t *line = (uint16_t *)malloc((size_t)tw * sizeof(uint16_t));
    if (!raw || !line) {
        free(raw);
        free(line);
        fclose(f);
        return PV_IMG_ERR_NOMEM;
    }

    pv_disp_img_begin(tw, th, tw, th);

    int done = 0;
    for (int ty = 0; ty < th; ty++) {
        const int      sy = (int)((int64_t)ty * H / th);
        const uint32_t fr = bottom_up ? (uint32_t)(H - 1 - sy) : (uint32_t)sy;
        const long     off = (long)data_off + (long)fr * (long)row_bytes;

        if (fseek(f, off, SEEK_SET) != 0) break;
        if (fread(raw, 1, row_bytes, f) != row_bytes) break;

        for (int dx = 0; dx < tw; dx++) {
            const int      sx = (int)((int64_t)dx * W / tw);
            const uint8_t *p  = raw + (size_t)sx * (size_t)px_bytes;

            line[dx] = (uint16_t)(((p[2] & 0xF8) << 8) |
                                  ((p[1] & 0xFC) << 3) |
                                   (p[0] >> 3));
        }
        pv_disp_img_block(0, ty, tw, 1, line, tw);
        done++;
    }

    pv_disp_img_end();
    free(raw);
    free(line);
    fclose(f);

    if (done == 0) {
        ESP_LOGE(TAG, "BMP 一行都没读出来（文件被截断？）");
        return PV_IMG_ERR_DECODE;
    }

#if PV_STATS
    ESP_LOGI(TAG, "BMP %dx%d (%u 位) -> %dx%d  %d/%d 行  用时 %lldms",
             W, H, (unsigned)bpp, tw, th, done, th,
             (long long)((esp_timer_get_time() - t0) / 1000));
#endif
    return PV_IMG_OK;
}
