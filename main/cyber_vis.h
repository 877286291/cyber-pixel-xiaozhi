// main/cyber_vis.h
// 三个纯绘制控件：静态霓虹网格背景、语音状态可视化、开机信号故障层。
//
// 都用 LVGL 的"自绘回调"实现而不是堆 lv_obj 小方块：本板无 PSRAM、LVGL 内部堆只有
// 24KB，几十个装饰性 lv_obj 会白白吃掉可观的内存，而绘制回调是零常驻开销。
//
// 所有函数都要求调用方已持有 bsp_lvgl_lock()。
#pragma once

#include "lvgl.h"

// 覆盖整屏的暗色网格 + 四角霓虹括号。内容静态，只在屏幕失效时重绘。
lv_obj_t *cyber_bg_create(lv_obj_t *parent, int w, int h);

// 语音状态可视化模式。
typedef enum {
    CYBER_VIS_NONE = 0,   // 隐藏（不占绘制开销）
    CYBER_VIS_WAVE,       // 聆听：以中线为中心的对称声波条
    CYBER_VIS_SCAN,       // 思考：左右往复的扫描线
    CYBER_VIS_SPECTRUM,   // 说话：自底部生长的频谱条
} cyber_vis_mode_t;

// 创建可视化控件（建议 176x34）。
lv_obj_t *cyber_vis_create(lv_obj_t *parent, int w, int h);

// 提交模式与相位；模式为 NONE 时自动隐藏，避免无谓重绘。
void cyber_vis_render(lv_obj_t *vis, cyber_vis_mode_t mode, uint32_t anim_ms);

// 开机/故障信号抖动层：整屏覆盖，仅在 active 时画几条错位色带。
lv_obj_t *cyber_glitch_create(lv_obj_t *parent, int w, int h);
void cyber_glitch_render(lv_obj_t *glitch, bool active, uint32_t anim_ms);
