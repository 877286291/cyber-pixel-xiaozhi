// main/cyber_prov.h
// 蓝牙配网（BLUFI over NimBLE）：手机把 Wi-Fi 凭证经 BLE 下发给设备，设备存进 NVS
// 并直接开始连接。
//
// 与 cyber_net 的分工（重要）：
//   cyber_prov 只负责【BLE 与 BLUFI 协议】——广播、连接、安全协商、收凭证；
//   Wi-Fi 与凭证持久化全部交给 cyber_net（它是 Wi-Fi 驱动与 NVS 凭证的唯一归属）。
//   配网期间通过 cyber_net_prov_* 借用 Wi-Fi 栈做扫描与提交，不另起一套。
//
// 线程与内存：
//   - 所有回调都跑在 NimBLE/BTC 任务上，绝不允许碰 LVGL 或阻塞；
//   - 界面通过【轮询】cyber_prov_get_status() 获取状态（与联网状态同一条规矩），
//     因此本模块不需要向界面推事件，也就没有跨任务回调的时序问题。
//
// BLE 只在配网期间存活：cyber_prov_stop() 会把 NimBLE 完全拆掉并把 RAM 还给系统。
// 这是无 PSRAM 的 C3 上必须的取舍——常驻 BLE 会与 Wi-Fi 共存抢内存，而本应用同时
// 还要跑 Opus 编解码与 LVGL。
//
// 参考实现：FoloToy/ai-passport 的 demo/blufi-provisioning 分支（demo_blufi.c）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cyber_prov_util.h"
#include "esp_err.h"

// 设备蓝牙广播名（CYBER_PROV_DEVICE_NAME）定义在 cyber_prov_util.h：界面也要显示它，
// 而界面层不依赖 ESP-IDF。微信小程序只列出以 "BLUFI" 开头的设备，改名字就扫不到。

// 启动配网：起 NimBLE + BLUFI 广播，并让 cyber_net 进入配网模式（可扫描、不重连）。
// 幂等：已在配网时返回 ESP_OK。
esp_err_t cyber_prov_start(void);

// 停止配网：拆掉 NimBLE 并把 RAM 还给系统，同时退出 cyber_net 的配网模式。
// 未提交凭证且设备仍无可用凭证时，会一并把 Wi-Fi 停下来。幂等。
esp_err_t cyber_prov_stop(void);

// 配网是否处于活动状态（BLE 栈是否已起）。界面据此显示配网页。
bool cyber_prov_is_active(void);

// 读取状态快照，供界面轮询。out 必须非 NULL。未启动时全部为 false。
void cyber_prov_get_status(cyber_prov_status_t *out);
