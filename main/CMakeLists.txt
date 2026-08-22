idf_component_register(SRC_DIRS "." "drivers" "display" "lvgl" "lvgl/components" "lvgl/screens" "lvgl/images" "lvgl/fonts"
                    INCLUDE_DIRS "."
                                  "drivers"
                                  "display"
                                  "lvgl"
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio esp_driver_i2s esp_driver_spi esp_lcd mbedtls esp_timer esp_websocket_client cjson lvgl)

# 本组件混有 SquareLine 生成代码(ui.c/ui_*.c)和 C++ 驱动,
# 这些代码用 C99 省略初始化/生成器未初始化变量, 会被 IDF 默认的 -Werror 拦截。
# 保留警告输出, 但不把警告当错误。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers
    -Wno-error=uninitialized
    -Wno-error=maybe-uninitialized)
