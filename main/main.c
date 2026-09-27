#include "board_pins.h"
#include "lcd_st7789.h"
#include "sd_card.h"

#include "pv_config.h"
#include "net_ap.h"
#include "net_menu.h"
#include "net_recv.h"
#include "net_remote.h"
#include "net_wifi.h"
#include "nui_menu.h"
#include "nui_sec.h"
#include "nui_setting.h"
#include "pv_app.h"
#include "pv_disp.h"
#include "pv_jpeg.h"
#include "pv_scan.h"
#include "pv_touch.h"
#include "rd_list.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#if NET_BOOT_SELFTEST && PV_BUILTIN_CARD
extern const uint8_t _binary_pv_card_1080p_jpg_start[];
extern const uint8_t _binary_pv_card_1080p_jpg_end[];

static void mem_check_with_wifi(void)
{
    const unsigned len = (unsigned)(_binary_pv_card_1080p_jpg_end -
                                    _binary_pv_card_1080p_jpg_start);
    ESP_LOGI(TAG, "WiFi 已占堆，验证 1080P 解码（%u 字节）", len);
    ESP_LOGI(TAG, "解码前：空闲堆 %u，最大可分配块 %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    pv_disp_img_clear();
    const pv_img_err_t e = pv_jpeg_show_ram(_binary_pv_card_1080p_jpg_start, len);

    ESP_LOGI(TAG, "解码结果：%s", pv_img_err_text(e));
    ESP_LOGI(TAG, "解码后：空闲堆 %u，最大可分配块 %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    pv_disp_img_clear();
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "=== 启动 ===");
    ESP_LOGI(TAG, "芯片: %s, 复位原因: %d", CONFIG_IDF_TARGET, (int)esp_reset_reason());
    ESP_LOGI(TAG, "空闲堆: %lu 字节", (unsigned long)esp_get_free_heap_size());

    esp_err_t nvs_r = nvs_flash_init();
    if (nvs_r == ESP_ERR_NVS_NO_FREE_PAGES || nvs_r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化，擦除后重试");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_r = nvs_flash_init();
    }
    if (nvs_r != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_r));
    }

    esp_lcd_panel_handle_t panel = lcd_st7789_init();
    lcd_cfg_load();

    const esp_err_t sd_r = sd_card_mount();
    ESP_LOGI(TAG, "SD 卡: %s", sd_r == ESP_OK ? "已挂载" : "未挂载");

    if (pv_disp_init(panel, lcd_st7789_io()) != ESP_OK) {
        ESP_LOGE(TAG, "显示层初始化失败，停在这里");
        return;
    }
    pv_touch_start();

    net_remote_init();

#if NET_BOOT_SELFTEST
    net_wifi_selftest();
#endif
#if PV_SCAN_SELFTEST
    pv_scan_selftest();
#endif
#if NET_BOOT_SELFTEST && PV_BUILTIN_CARD
    mem_check_with_wifi();
#endif
#if NET_RECV_SELFTEST
    net_recv_selftest();
#endif
#if NET_AP_SELFTEST
    net_ap_selftest();
#endif
#if RD_BOOT_SELFTEST
    rd_selftest();
#endif

    nui_sec_unlock();

    for (;;) {
        const nui_action_t a = nui_menu_run();

        if (a == NUI_ACT_READ) {
            rd_list_run();
        } else if (a == NUI_ACT_IMAGE) {
            pv_app_run();
        } else if (a == NUI_ACT_NET) {
            net_menu_run();
        } else if (a == NUI_ACT_SETTING) {
            nui_setting_run();
        } else if (a != NUI_ACT_NONE) {
            ESP_LOGW(TAG, "动作 %d 没有对应的处理程序", (int)a);
        }
    }
}
