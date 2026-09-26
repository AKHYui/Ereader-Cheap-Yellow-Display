#include "sd_card.h"
#include "board_pins.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

#ifndef SD_DIRS_SELFTEST
#define SD_DIRS_SELFTEST 0
#endif

#define SD_DEBUG_SKIP_MOUNT 0

#if SD_DIRS_SELFTEST

#define SD_SELFTEST_BAK  SD_MOUNT_POINT "/images__selftest"

static void sd_dirs_selftest(void)
{
    struct stat st;

    if (stat(SD_SELFTEST_BAK, &st) == 0) {
        ESP_LOGW(TAG, "自检备份目录已存在（上次没清干净？），本次跳过");
        return;
    }

    if (rename(SD_DIR_IMAGES, SD_SELFTEST_BAK) != 0) {
        ESP_LOGW(TAG, "改名 %s -> %s 失败，跳过自检", SD_DIR_IMAGES, SD_SELFTEST_BAK);
        return;
    }
    ESP_LOGW(TAG, "① 已把 %s 改名为 %s（内容原样保留，一个新文件都没碰）",
             SD_DIR_IMAGES, SD_SELFTEST_BAK);

    sd_card_ensure_dirs();

    const bool ok = (stat(SD_DIR_IMAGES, &st) == 0 && S_ISDIR(st.st_mode));
    ESP_LOGW(TAG, "② 自检结论：%s", ok
             ? "ensure_dirs 在目录缺失时把它建回来了 ✓ 功能成立"
             : "!! images 没有被建出来 ✗ 功能失效，必须查");

    if (ok) {
        if (rmdir(SD_DIR_IMAGES) != 0) {
            ESP_LOGW(TAG, "新建的 images 删不掉（不该发生），请手工检查");
        } else {
            ESP_LOGW(TAG, "③ 已清掉自检新建的空目录");
        }
    }

    if (rename(SD_SELFTEST_BAK, SD_DIR_IMAGES) != 0) {
        ESP_LOGE(TAG, "!! 恢复失败：原目录还在 %s，请手工改回 %s",
                 SD_SELFTEST_BAK, SD_DIR_IMAGES);
    } else {
        ESP_LOGW(TAG, "④ 原 images 目录已恢复");
    }
}
#endif

esp_err_t sd_card_mount(void)
{
    if (s_mounted) return ESP_OK;

#if SD_DEBUG_SKIP_MOUNT
    static bool s_skipped_once = false;
    if (!s_skipped_once) {
        s_skipped_once = true;
        ESP_LOGW(TAG, "SD_DEBUG_SKIP_MOUNT=1，跳过首次挂载（模拟开机时无卡）");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "SD_DEBUG_SKIP_MOUNT=1，后续挂载按正常流程走");
#endif

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
    esp_err_t r = spi_bus_initialize(SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(r));
        return r;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    host.max_freq_khz = 10000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    r = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                &mount_cfg, &s_card);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (无卡也能进系统)", esp_err_to_name(r));
        spi_bus_free(SD_SPI_HOST);
        return r;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);

    sd_card_ensure_dirs();
#if SD_DIRS_SELFTEST
    sd_dirs_selftest();
#endif
    return ESP_OK;
}

void sd_card_ensure_dirs(void)
{
    if (!s_mounted) return;

    static const char *const dirs[] = { SD_DIR_IMAGES, SD_DIR_NOVELS };
    int made = 0;

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0) {

            if (!S_ISDIR(st.st_mode)) {
                ESP_LOGW(TAG, "%s 存在但不是目录（被同名文件占了）", dirs[i]);
            }
            continue;
        }

        if (mkdir(dirs[i], 0775) == 0) {
            made++;
            ESP_LOGI(TAG, "已创建目录 %s", dirs[i]);
        } else if (errno != EEXIST) {

            ESP_LOGW(TAG, "建目录失败 %s (errno=%d) —— 写保护或坏卡？",
                     dirs[i], errno);
        }
    }

    ESP_LOGI(TAG, "目录结构就绪（本次新建 %d 个）", made);
}

void sd_card_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    spi_bus_free(SD_SPI_HOST);
    s_mounted = false;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

uint64_t sd_card_total_bytes(void)
{
    if (!s_mounted) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}
