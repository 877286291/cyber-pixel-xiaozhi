// main/cyber_net.h
// Wi-Fi STA 联网层：连路由器、维持连接、把上下线状态抛给应用。
//
// 与 UI 的边界（重要）：
//   本模块的回调运行在 ESP-IDF 的 Wi-Fi/IP 事件任务上下文里，【不是】LVGL 任务。
//   因此回调内绝对不允许碰 lv_* 或界面模型。回调只应记录一个标志/入队，
//   由 LVGL 任务在自己的节奏里读取并落到模型上（见 main.c 的 tick_cb）。
//
// 依赖：调用 cyber_net_init() 之前必须先初始化 NVS（nvs_flash_init()），
// 因为 Wi-Fi 驱动与凭证读取都要用它。
//
// 凭证来源优先级：
//   1. 编译期的开发覆盖头 cyber_wifi_dev.h（见 .gitignore；不进版本库）
//   2. NVS namespace "cyber" 的 ssid / pass 键
//   两者都没有时保持未连接，返回 ESP_ERR_NOT_FOUND，应用照常运行（界面显示 OFFLINE）。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 上下线通知。connected 为 true 表示已关联 AP 且拿到 IP。
typedef void (*cyber_net_state_cb_t)(bool connected, void *ctx);

// 建立 Wi-Fi 栈但不连接。幂等：重复调用返回 ESP_ERR_INVALID_STATE。
esp_err_t cyber_net_init(cyber_net_state_cb_t cb, void *ctx);

// 读取凭证并开始连接。无可用凭证返回 ESP_ERR_NOT_FOUND（不是故障）。
// 失败会自动按退避序列重连，无需调用方介入。
esp_err_t cyber_net_start(void);

// 停止并释放 Wi-Fi 栈。会注销事件处理，之后可再次 init。
esp_err_t cyber_net_stop(void);

// 当前是否已关联且拿到 IP。
bool cyber_net_is_connected(void);

// 清除 NVS 里的凭证。给后续"配网/换网"功能用；会同时断开当前连接。
// 刻意显式提供：仓库的配网规范要求凭证必须有明确的清除途径。
esp_err_t cyber_net_clear_credentials(void);

// 内存埋点（临时的预算测量工具）。
// 打印系统堆/内部堆/最大连续块的当前值，用于核算 Wi-Fi + 后续 Opus 的 RAM 预算。
// 无 PSRAM 的 C3 上，"最大连续块"比"总空闲"更关键：TLS 与 Opus 都要连续内存。
void cyber_net_log_heap(const char *stage);
