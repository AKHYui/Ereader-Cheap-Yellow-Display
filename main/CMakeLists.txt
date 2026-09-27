idf_component_register(
    SRCS
        "main.c"
        "drivers/lcd_st7789.c"
        "drivers/sd_card.c"
        "book/gbk_map.c"
        "book/gbk_text.c"
        "picview/pv_app.c"
        "picview/pv_bmp.c"
        "picview/pv_disp.c"
        "picview/pv_jpeg.c"
        "picview/pv_scan.c"
        "picview/pv_touch.c"
        "nui/nui_ascii.c"
        "nui/nui_glyphs.c"
        "nui/nui_ui.c"
        "nui/nui_led.c"
        "nui/nui_qr.c"
        "nui/nui_menu.c"
        "nui/nui_setting.c"
        "nui/nui_diag.c"
        "nui/nui_sec.c"
        "net/net_wifi.c"
        "net/net_httpd.c"
        "net/net_ap.c"
        "net/net_qr.c"
        "net/net_remote.c"
        "net/net_menu.c"
        "net/net_recv.c"
        "net/net_wlan.c"
        "reader/rd_font.c"
        "reader/rd_walk.c"
        "reader/rd_txt.c"
        "reader/rd_epub.c"
        "reader/rd_book.c"
        "reader/rd_bar.c"
        "reader/rd_list.c"
        "reader/rd_view.c"
    INCLUDE_DIRS
        "."
        "drivers"
        "book"
        "picview"
        "nui"
        "net"
        "reader"
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_rom
        fatfs
        sdmmc
        esp_timer
        esp_driver_gpio
        esp_driver_spi
        esp_hw_support
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
)

option(PV_BUILTIN_CARD "把 1080P 测试卡编进固件（配合 pv_config.h 同名宏）" ON)
if(PV_BUILTIN_CARD)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_LIST_DIR}/picview/testcard/pv_card_1080p.jpg" BINARY)
endif()

option(NET_RECV_WEBPAGE "把文件接收的上传网页编进固件" ON)
if(NET_RECV_WEBPAGE)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_LIST_DIR}/net/www/index.html" BINARY)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_LIST_DIR}/net/www/remote.html" BINARY)
endif()

option(RD_FONT_BLOB "把阅读页字库编进固件" ON)
if(RD_FONT_BLOB)
    foreach(_blob rd_font16.bin rd_font16_ext.bin)
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/reader/${_blob}")
            message(FATAL_ERROR
                "缺 main/reader/${_blob} —— 字库文件应随仓库一起提供")
        endif()
        target_add_binary_data(${COMPONENT_LIB}
            "${CMAKE_CURRENT_LIST_DIR}/reader/${_blob}" BINARY)
    endforeach()
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-deprecated-declarations
    -Wno-error=deprecated-declarations
)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
