// main/cyber_layout.h
// 各屏元素的【实际绘制矩形】（px）。纯常量，不依赖 LVGL / ESP-IDF。
//
// 为什么单独拆一个文件：屏幕四角有 30px 圆角遮罩（BSP_LVGL_SCREEN_RADIUS），
// 而"哪块内容会落进被切掉的角里"是可以用纯几何在宿主上算出来的。
// 布局一旦散落在 cyber_ui.c 的 lv_* 调用里，就只能靠上机肉眼发现，
// 所以这里把它提出来，由 tests/test_cyber_ui_layout.c 用 BSP 的同一套
// 遮罩函数逐行校验。cyber_ui.c 必须只用这里的常量，不得再写字面量坐标。
#pragma once

#include "cyber_theme.h"

// 圆角遮罩半径。cyber_ui.c 里有 _Static_assert 保证它与 BSP 的取值一致
// （这里不能直接 include bsp_display.h，那会把 LVGL 依赖带进纯逻辑层）。
#define CYBER_SCREEN_RADIUS     30

// 直边安全区：y 落在 [CYBER_SAFE_TOP, CYBER_SAFE_BOTTOM] 内的行，
// 圆角遮罩不会削掉任何像素，整幅宽度都可用。
#define CYBER_SAFE_TOP          CYBER_SCREEN_RADIUS
#define CYBER_SAFE_BOTTOM       (CYBER_SCREEN_H - 1 - CYBER_SCREEN_RADIUS)

// 状态栏内容与屏幕右缘的距离。必须比裸看更宽：上排圆角在 y=6 处
// 已经把可见区域收到 x<=227，用 8px 边距会把电池正极凸点切掉。
#define CYBER_EDGE_PAD          14

// ============================================================================
// 顶部状态栏（0 .. CYBER_STATUS_H-1）
// ============================================================================
// 状态栏底板是满幅背景，故意让它被圆角切掉，观感才是"屏幕本身是圆的"。
#define CYBER_STATUSBAR_W       CYBER_SCREEN_W
#define CYBER_STATUSBAR_H       CYBER_STATUS_H

// 左侧链路状态（"OFFLINE" / "AI.LINK"）。
#define CYBER_LINK_X            CYBER_EDGE_PAD
#define CYBER_LINK_Y            7
#define CYBER_LINK_W            56
#define CYBER_LINK_H            9    // = cyber_font_sm 的 line_height

// 右侧电量：百分比读数 + 电池壳 + 正极凸点 + 电量填充条。
#define CYBER_BATT_BODY_W       22
#define CYBER_BATT_BODY_H       11
#define CYBER_BATT_BODY_X       (CYBER_SCREEN_W - CYBER_EDGE_PAD - CYBER_BATT_BODY_W)
#define CYBER_BATT_BODY_Y       6

#define CYBER_BATT_NUB_W        2
#define CYBER_BATT_NUB_H        5
#define CYBER_BATT_NUB_X        (CYBER_BATT_BODY_X + CYBER_BATT_BODY_W)
#define CYBER_BATT_NUB_Y        (CYBER_BATT_BODY_Y + 3)

#define CYBER_BATT_FILL_W       (CYBER_BATT_BODY_W - 4)
#define CYBER_BATT_FILL_H       (CYBER_BATT_BODY_H - 4)
#define CYBER_BATT_FILL_X       (CYBER_BATT_BODY_X + 2)
#define CYBER_BATT_FILL_Y       (CYBER_BATT_BODY_Y + 2)

#define CYBER_BATT_PCT_W        38
#define CYBER_BATT_PCT_H        9
#define CYBER_BATT_PCT_X        (CYBER_BATT_BODY_X - 4 - CYBER_BATT_PCT_W)
#define CYBER_BATT_PCT_Y        7

// 状态栏下沿的霓虹分隔线。两端各留一点：满幅会被上排圆角削掉，
// 一条"两头缺角"的线比短一点的线更扎眼。
#define CYBER_STATUS_RULE_X     4
#define CYBER_STATUS_RULE_W     (CYBER_SCREEN_W - 2 * CYBER_STATUS_RULE_X)
#define CYBER_STATUS_RULE_Y     (CYBER_STATUS_H - CYBER_STATUS_RULE)
#define CYBER_STATUS_RULE_H     CYBER_STATUS_RULE

// ============================================================================
// 底部提示条
// ============================================================================
#define CYBER_HINT_RULE_X       34
#define CYBER_HINT_RULE_W       172
#define CYBER_HINT_RULE_Y       CYBER_HINT_TOP
#define CYBER_HINT_RULE_H       1

#define CYBER_HINT_W            208
#define CYBER_HINT_H            9
#define CYBER_HINT_X            ((CYBER_SCREEN_W - CYBER_HINT_W) / 2)
#define CYBER_HINT_Y            (CYBER_HINT_TOP + 9)

// ============================================================================
// 开机页
// ============================================================================
#define CYBER_BOOT_TITLE_W      200
#define CYBER_BOOT_TITLE_X      ((CYBER_SCREEN_W - CYBER_BOOT_TITLE_W) / 2)
#define CYBER_BOOT_TITLE_Y      58
#define CYBER_BOOT_TITLE_H      24   // = cyber_font_title_lg 的 line_height
#define CYBER_BOOT_TITLE2_Y     86

#define CYBER_BOOT_SUB_W        224
#define CYBER_BOOT_SUB_X        ((CYBER_SCREEN_W - CYBER_BOOT_SUB_W) / 2)
#define CYBER_BOOT_SUB_Y        118
#define CYBER_BOOT_SUB_H        16   // = cyber_font_title 的 line_height

#define CYBER_BOOT_DIV_X        30
#define CYBER_BOOT_DIV_W        180
#define CYBER_BOOT_DIV_Y        148
#define CYBER_BOOT_DIV_H        2

#define CYBER_BOOT_BAR_X        30
#define CYBER_BOOT_BAR_W        180
#define CYBER_BOOT_BAR_Y        182
#define CYBER_BOOT_BAR_H        12
#define CYBER_BOOT_BAR_INNER_W  (CYBER_BOOT_BAR_W - 4)
#define CYBER_BOOT_BAR_INNER_H  (CYBER_BOOT_BAR_H - 4)

#define CYBER_BOOT_PCT_W        48
#define CYBER_BOOT_PCT_H        9
#define CYBER_BOOT_PCT_X        ((CYBER_SCREEN_W - CYBER_BOOT_PCT_W) / 2)
#define CYBER_BOOT_PCT_Y        200

#define CYBER_BOOT_LOG_X        34
#define CYBER_BOOT_LOG_W        172
#define CYBER_BOOT_LOG_H        9
#define CYBER_BOOT_LOG_Y        232
#define CYBER_BOOT_LOG_STEP     14

// ============================================================================
// 主界面
// ============================================================================
#define CYBER_HOME_TITLE_W      200
#define CYBER_HOME_TITLE_X      ((CYBER_SCREEN_W - CYBER_HOME_TITLE_W) / 2)
#define CYBER_HOME_TITLE_Y      30
#define CYBER_HOME_TITLE_H      16   // = cyber_font_title 的 line_height

#define CYBER_HOME_FACE_X       ((CYBER_SCREEN_W - CYBER_FACE_HOME_PX) / 2)
#define CYBER_HOME_FACE_Y       58

#define CYBER_HOME_MODE_W       200
#define CYBER_HOME_MODE_X       ((CYBER_SCREEN_W - CYBER_HOME_MODE_W) / 2)
#define CYBER_HOME_MODE_Y       (CYBER_HOME_FACE_Y + CYBER_FACE_HOME_PX + 12)
#define CYBER_HOME_MODE_H       18   // = cyber_font_md 的 line_height

// ============================================================================
// 语音页
// ============================================================================
#define CYBER_VOICE_LABEL_W     200
#define CYBER_VOICE_LABEL_X     ((CYBER_SCREEN_W - CYBER_VOICE_LABEL_W) / 2)
#define CYBER_VOICE_LABEL_Y     30
#define CYBER_VOICE_LABEL_H     16

#define CYBER_VOICE_FACE_X      ((CYBER_SCREEN_W - CYBER_FACE_VOICE_PX) / 2)
#define CYBER_VOICE_FACE_Y      54

#define CYBER_VOICE_VIS_W       176
#define CYBER_VOICE_VIS_H       34
#define CYBER_VOICE_VIS_X       ((CYBER_SCREEN_W - CYBER_VOICE_VIS_W) / 2)
#define CYBER_VOICE_VIS_Y       (CYBER_VOICE_FACE_Y + CYBER_FACE_VOICE_PX + 9)

#define CYBER_VOICE_SUB_W       208
#define CYBER_VOICE_SUB_X       ((CYBER_SCREEN_W - CYBER_VOICE_SUB_W) / 2)
#define CYBER_VOICE_SUB_Y       (CYBER_VOICE_VIS_Y + CYBER_VOICE_VIS_H + 8)
// 字幕按固定宽度自动换行，预留两行。
#define CYBER_VOICE_SUB_H       (2 * 18)

// ============================================================================
// 激活页（未绑定时的唯一界面）
// ============================================================================
// 整页落在直边安全区内：最上 32px、最下 196px，离 30px 圆角都有富余，
// 所以这里不必像状态栏那样为圆角额外内缩。
#define CYBER_PAIR_TITLE_W      200
#define CYBER_PAIR_TITLE_X      ((CYBER_SCREEN_W - CYBER_PAIR_TITLE_W) / 2)
#define CYBER_PAIR_TITLE_Y      32
#define CYBER_PAIR_TITLE_H      16   // = cyber_font_title 的 line_height

// 包住数字的面板。数字是整页的视觉重心，面板给它一圈边框把视线收住。
#define CYBER_PAIR_FRAME_X      18
#define CYBER_PAIR_FRAME_W      (CYBER_SCREEN_W - 2 * CYBER_PAIR_FRAME_X)   // 204
#define CYBER_PAIR_FRAME_Y      62
#define CYBER_PAIR_FRAME_H      132  // 62..194

// 面板上沿的霓虹强调线，压在边框内侧。
#define CYBER_PAIR_RULE_X       (CYBER_PAIR_FRAME_X + 1)
#define CYBER_PAIR_RULE_W       (CYBER_PAIR_FRAME_W - 2)
#define CYBER_PAIR_RULE_Y       (CYBER_PAIR_FRAME_Y + 1)
#define CYBER_PAIR_RULE_H       2

// 两行数字。行距居中推导：两行 + 行间距共 102px，面板内高 132px，上下各余 15px。
#define CYBER_PAIR_ROW_W        168  // 3 位 × 48px = 144px，余量留给字形两侧留白
#define CYBER_PAIR_ROW_X        ((CYBER_SCREEN_W - CYBER_PAIR_ROW_W) / 2)
#define CYBER_PAIR_ROW_H        48   // = cyber_font_code 的 line_height
#define CYBER_PAIR_ROW_GAP      6
#define CYBER_PAIR_ROW1_Y       (CYBER_PAIR_FRAME_Y + 15)                    // 77
#define CYBER_PAIR_ROW2_Y       (CYBER_PAIR_ROW1_Y + CYBER_PAIR_ROW_H + CYBER_PAIR_ROW_GAP)  // 131

// 数字下方的状态行（"LINKING" / "WAIT FOR BIND" / "LINK ERROR" ...）。
#define CYBER_PAIR_STATE_W      208
#define CYBER_PAIR_STATE_X      ((CYBER_SCREEN_W - CYBER_PAIR_STATE_W) / 2)
#define CYBER_PAIR_STATE_Y      206
#define CYBER_PAIR_STATE_H      9    // = cyber_font_sm 的 line_height

// 等待时的扫描条：一根暗轨 + 一个来回滑动的亮块。
// 用"一个移动的块"而不是一排跑马灯，是为了把常驻控件数压到 2 个——
// LVGL 内部堆只有 24KB，每个 lv_obj 连同样式都要从这里出。
#define CYBER_PAIR_SCAN_X       30
#define CYBER_PAIR_SCAN_W       180
#define CYBER_PAIR_SCAN_Y       226
#define CYBER_PAIR_SCAN_H       4
#define CYBER_PAIR_SCAN_HEAD_W  28
// 亮块左边缘的行程格数（含两端），供 cyber_visor_scan_index 使用。
#define CYBER_PAIR_SCAN_SPAN    (CYBER_PAIR_SCAN_W - CYBER_PAIR_SCAN_HEAD_W + 1)
