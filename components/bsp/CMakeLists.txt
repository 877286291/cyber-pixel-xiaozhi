idf_component_register(
    SRCS "src/bsp_i2c.c"
         "src/bsp_display.c"
         "src/bsp_display_lvgl.c"
         "src/bsp_display_rounding.c"
         "src/bsp_button.c"
         "src/bsp_audio.c"
         "src/bsp_es8311_sleep_check.c"
         "src/bsp_battery.c"
    INCLUDE_DIRS "include"
    # ESP-IDF 6.x 把旧的 `driver` 组件拆成 esp_driver_* 独立组件；头文件名不变
    # （driver/gpio.h、driver/spi_master.h、driver/ledc.h），只是归属组件改了。
    # 因此 REQUIRES 用具体的 esp_driver_* 组件替代整包 driver。
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_ledc
             esp_driver_i2c esp_driver_i2s esp_lcd esp_lvgl_port button esp_adc esp_codec_dev
    PRIV_REQUIRES esp_timer
)
