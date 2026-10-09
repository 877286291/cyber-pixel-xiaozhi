# Generated font sources live under assets/ so the whole font set — upstream TTFs,
# OFL license texts, regeneration script, and converter output — stays in one
# place (see assets/README.md). They are compiled here because the main component
# is what references the glyph symbols via LV_FONT_DECLARE.
set(CYBER_FONT_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/generated")

# Code layout, split by responsibility and by testability:
#   cyber_ui_model  pure state machine and animation math (no LVGL, no ESP-IDF)
#   cyber_face      the mech pixel face widget
#   cyber_vis       background grid, voice visualizer, boot glitch overlay
#   cyber_ui        screen orchestrator (boot, home, voice, activation)
#   cyber_net_util  pure credential/auth/backoff rules (no ESP-IDF, host tested)
#   cyber_net       Wi-Fi STA lifecycle: connect, retry, report state
#   cyber_ota_util  pure activation rules: code check, poll pacing, body builder
#   cyber_ota       activation client: HTTPS check, JSON parse, NVS persistence
# The upstream demo pages stay absent: this application replaces that menu rather
# than reusing it. Networking is present now, but the audio path is still unwired,
# so the codec/Opus dependencies are intentionally not registered yet.
# 注意：开发期凭证头（cyber_wifi_dev.h，可选且被 gitignore）刻意【不】在这里做
# CMake 依赖声明：组件 CMakeLists.txt 会被 ESP-IDF 当脚本再 include 一次以提取依赖，
# 那一趟里 set_source_files_properties / set_property 都不可用（会报 "not scriptable"）。
# 依赖由编译器的 .d 依赖文件自然覆盖：只要它存在时被 include 过一次，
# 之后改它就一定会重编。唯一要留意的是"构建目录配置完之后该文件才第一次出现"
# 那一次（详见 cyber_wifi_dev.h.example 的说明）。
idf_component_register(
    SRCS "main.c"
         "cyber_ui_model.c"
         "cyber_face.c"
         "cyber_vis.c"
         "cyber_ui.c"
         "cyber_net_util.c"
         "cyber_net.c"
         "cyber_ota_util.c"
         "cyber_ota.c"
         "${CYBER_FONT_DIR}/cyber_font_sm.c"
         "${CYBER_FONT_DIR}/cyber_font_md.c"
         "${CYBER_FONT_DIR}/cyber_font_title.c"
         "${CYBER_FONT_DIR}/cyber_font_title_lg.c"
         "${CYBER_FONT_DIR}/cyber_font_code.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event esp_timer
             esp_http_client esp-tls mbedtls json spi_flash
)

# lv_font_conv emits either "lvgl.h" or "lvgl/lvgl.h" depending on this macro.
# ESP-IDF puts lvgl's own header directory on the include path under the plain
# name, so select the simple form; without it the generated fonts cannot be
# found from this component.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
