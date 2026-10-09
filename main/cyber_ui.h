// main/cyber_ui.h
// 界面编排层：把 cyber_ui_model 的状态落到实际 LVGL 控件上。
//
// 分工：
//   cyber_ui_model.c —— 状态机与动画数值（纯逻辑，宿主可测）
//   cyber_ui.c       —— 建屏、增量刷新、页面切换（LVGL）
//   cyber_face.c     —— 像素机甲脸自绘控件
//   cyber_vis.c      —— 网格背景 / 语音可视化 / 抖动层
// 这样"配色好看不好看"改 cyber_theme.h，"状态对不对"改 cyber_ui_model.c，
// 各层都能单独验证。
#pragma once

#include "lvgl.h"

#include "cyber_ui_model.h"

typedef struct cyber_ui cyber_ui_t;

// 建屏并装配三个页面。需持有 bsp_lvgl_lock()；内存不足返回 NULL。
cyber_ui_t *cyber_ui_create(void);

// 释放。需持有 bsp_lvgl_lock()。
void cyber_ui_delete(cyber_ui_t *ui);

// 把模型状态刷到界面。需持有 bsp_lvgl_lock()。
// 内部保存"上次已应用值"，只改真正变化的部分：LVGL 每次 set style 都会 invalidate，
// 无脑全量刷新会让静止画面也持续重绘，既费 CPU 也费电。
void cyber_ui_apply(cyber_ui_t *ui, const cyber_ui_model_t *m);
