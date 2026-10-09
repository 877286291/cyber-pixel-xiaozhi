// main/cyber_face.h
// 像素机甲脸控件：一个自绘 lv_obj，用矩形拼出像素画。
//
// 为什么不用 lv_canvas：本板无 PSRAM，全屏 RGB565 画布要 150KB，脸区画布也要十几 KB。
// 自绘走 LVGL 的绘制层，只画"实心格子"，多余显存开销为零；并且整张脸只占一个 lv_obj，
// 省下 LVGL 内部堆（CONFIG_LV_MEM_SIZE_KILOBYTES 只有 24KB）。
//
// 所有 lv_* / 绘制调用都要求调用方已持有 bsp_lvgl_lock()。
#pragma once

#include "lvgl.h"

#include "cyber_ui_model.h"

// 创建脸控件。size_px 必须是 CYBER_FACE_GRID_W 的整数倍（内部据此推每格边长）。
lv_obj_t *cyber_face_create(lv_obj_t *parent, int size_px);

// 提交本帧的表情与动画相位。内部只在参数变化时 invalidate，避免每帧无谓重绘。
// speaking=true 时嘴角换成随 cyber_wave_level 起伏的声波条。
void cyber_face_render(lv_obj_t *face, cyber_emotion_t emo, uint32_t anim_ms,
                       bool speaking);
