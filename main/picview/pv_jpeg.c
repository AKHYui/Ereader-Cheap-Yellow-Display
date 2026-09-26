#include "pv_jpeg.h"
#include "pv_config.h"
#include "pv_disp.h"

#include "JPEGDEC.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = PV_TAG;

static JPEGIMAGE s_jpg;

static int s_blocks;
static int64_t s_t0;

static int jpeg_draw_cb(JPEGDRAW *pDraw)
{
    if (!pDraw || !pDraw->pPixels) return 1;

    pv_disp_img_block(pDraw->x, pDraw->y,
                      pDraw->iWidthUsed, pDraw->iHeight,
                      (const uint16_t *)pDraw->pPixels,
                      pDraw->iWidth);
    s_blocks++;
    return 1;
}

static void fit_target(int W, int H, int *tw, int *th)
{
    if (W <= PV_SCR_W && H <= PV_IMG_H) {
        *tw = W;
        *th = H;
        return;
    }
    if ((int64_t)W * PV_IMG_H > (int64_t)H * PV_SCR_W) {
        *tw = PV_SCR_W;
        *th = (int)((int64_t)H * PV_SCR_W / W);
    } else {
        *th = PV_IMG_H;
        *tw = (int)((int64_t)W * PV_IMG_H / H);
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

static int pick_scale(int W, int H, int tw, int th)
{
    static const int divs[4] = { 8, 4, 2, 1 };
    for (int i = 0; i < 4; i++) {
        const int d  = divs[i];
        const int sw = (W + d - 1) / d;
        const int sh = (H + d - 1) / d;
        if (sw >= tw && sh >= th) {
            return (d == 1) ? 0 : d;
        }
    }
    return 0;
}

static void decoded_size(int W, int H, int div, int *ow, int *oh)
{

    const int hs = (s_jpg.ucSubSample >> 4) & 0x0f;
    const int mcu_w = 8 * (hs > 1 ? 2 : 1);

    *ow = ((W + mcu_w - 1) / mcu_w) * (mcu_w / div);
    *oh = (H + div - 1) / div;

    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

static pv_img_err_t render_opened(void)
{

    if (s_jpg.ucMode == 0xc2) {
        ESP_LOGW(TAG, "渐进式 JPEG：只能解出约 1/8 分辨率的预览，会明显发虚"
                      " —— 要清晰改用 tools/pv_prep_photos.py 转 baseline");
    }

    const int W = JPEG_getWidth(&s_jpg);
    const int H = JPEG_getHeight(&s_jpg);
    if (W <= 0 || H <= 0) {
        ESP_LOGE(TAG, "读不出尺寸 lastError=%d", JPEG_getLastError(&s_jpg));
        JPEG_close(&s_jpg);
        return PV_IMG_ERR_DECODE;
    }

    int tw = 0, th = 0;
    fit_target(W, H, &tw, &th);

    const int opt = pick_scale(W, H, tw, th);
    const int div = (opt == 0) ? 1 : opt;

    int sw = 0, sh = 0;
    decoded_size(W, H, div, &sw, &sh);

    pv_disp_img_begin(sw, sh, tw, th);

    JPEG_setPixelType(&s_jpg, RGB565_LITTLE_ENDIAN);

    const int ok = JPEG_decode(&s_jpg, 0, 0, opt);

    pv_disp_img_end();
    JPEG_close(&s_jpg);

    if (!ok) {
        ESP_LOGE(TAG, "解码失败 lastError=%d (%dx%d)",
                 JPEG_getLastError(&s_jpg), W, H);
        return PV_IMG_ERR_DECODE;
    }

#if PV_STATS
    ESP_LOGI(TAG, "%dx%d -> %dx%d  1/%d  块=%d  用时 %lldms  堆余 %u",
             W, H, tw, th, div, s_blocks,
             (long long)((esp_timer_get_time() - s_t0) / 1000),
             (unsigned)esp_get_free_heap_size());
#endif
    return PV_IMG_OK;
}

pv_img_err_t pv_jpeg_show(const char *path)
{
    if (!path || !path[0]) return PV_IMG_ERR_OPEN;

    s_t0 = esp_timer_get_time();
    s_blocks = 0;

    if (!JPEG_openFile(&s_jpg, path, jpeg_draw_cb)) {
        ESP_LOGE(TAG, "JPEG 打不开: %s", path);
        return PV_IMG_ERR_OPEN;
    }
    return render_opened();
}

pv_img_err_t pv_jpeg_show_ram(const void *data, unsigned len)
{
    if (!data || len < 4) return PV_IMG_ERR_OPEN;

    s_t0 = esp_timer_get_time();
    s_blocks = 0;

    if (!JPEG_openRAM(&s_jpg, (uint8_t *)data, (int)len, jpeg_draw_cb)) {
        ESP_LOGE(TAG, "JPEG 内存流打不开（%u 字节）", len);
        return PV_IMG_ERR_OPEN;
    }
    return render_opened();
}

const char *pv_img_err_text(pv_img_err_t e)
{
    switch (e) {
    case PV_IMG_OK:          return "OK";
    case PV_IMG_ERR_OPEN:    return "打不开文件";
    case PV_IMG_ERR_FORMAT:  return "格式不支持";
    case PV_IMG_ERR_DECODE:  return "解码失败";
    case PV_IMG_ERR_NOMEM:   return "内存不足";
    default:                 return "未知错误";
    }
}
