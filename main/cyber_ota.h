// main/cyber_ota.h
// 小智服务激活客户端：向官方 OTA 接口做检查，拿到激活码或对话配置。
//
// 与 UI 的边界（和 cyber_net 同一条规矩）：
//   worker 任务运行在【自己的】任务上下文里，绝不碰 lv_* 或界面模型。
//   它只把一个快照写进受临界区保护的结构，由 LVGL 任务按自己的节奏读走。
//   这里比 cyber_net 复杂一点：要传递的不再是一个 bool，而是一组字段（含激活码字符串），
//   所以用一个 portMUX 临界区保护整份快照；临界区内只做定长拷贝，不会长时间关中断。
//
// 为什么没有 start/stop：见 cyber_ota_set_net 的注释。激活是"跟着联网状态走"的
// 长期后台行为，用一对会阻塞的启停函数去管它，反而会引出"从 LVGL 任务里等待退出"
// 这种会冻界面的用法。
//
// 依赖：调用前须已 nvs_flash_init()。Wi-Fi 未连接时 worker 只是空转等待。
//
// 规格见 docs/development/engineering/xiaozhi-activation.md。
#pragma once

#include <stdbool.h>

#include "cyber_ota_util.h"
#include "esp_err.h"

// 激活阶段对外可见的状态。
typedef enum {
    CYBER_OTA_STATE_IDLE = 0,        // 尚未开始（例如还没联网）
    CYBER_OTA_STATE_CHECKING,        // 正在请求
    CYBER_OTA_STATE_NEED_ACTIVATION, // 已有激活码，等待用户在控制台绑定
    CYBER_OTA_STATE_READY,           // 已拿到对话端点，可以开对话通道
    CYBER_OTA_STATE_TEST_ONLY,       // 只有临时测试凭证，绑定未生效（见规格文档）
    CYBER_OTA_STATE_ERROR,           // 网络/TLS/协议失败
} cyber_ota_state_t;

// 状态快照。整份在临界区里读写，因此可以按值传递。
typedef struct {
    cyber_ota_state_t state;
    char code[CYBER_OTA_CODE_CAP]; // NEED_ACTIVATION 时有效；否则为 ""
    bool test_credentials;         // 当前是否仍是服务端的临时测试账号
    int last_http_status;          // 便于排查；0 表示还没拿到响应
    // 本机是否【曾经】绑定成功。它跨重启保留，且一旦为真就不再变假。
    //
    // 为什么需要它：state 在开机后到首次检查返回前一直是 IDLE，而"未绑定"和
    // "已绑定但此刻没网"在这个状态下长得一模一样。界面靠 state 判断要不要显示
    // 配对屏，只看 state 就会把一台已绑定的设备在断网时打回配对屏。paired 把
    // "绑定过"这件事从一次查询的结果变成设备的持久属性，两者才能分开。
    bool paired;
} cyber_ota_snapshot_t;

// 启动 worker（幂等），并立即发布 IDLE。需在 nvs_flash_init() 之后调用。
esp_err_t cyber_ota_init(void);

// 上报联网状态。由 LVGL 任务在每个 tick 调用，代价是一次比较 + 一次赋值。
//
// 刻意不做成"阻塞式启停"：本函数可能被 LVGL 任务调用，一旦阻塞就会冻住界面。
// worker 自己轮询这个标志：联网就干活，掉线就回到 IDLE 等待。
// 掉线时自动回到 IDLE 这一点很重要——否则界面会一直显示一个已经失效的激活码。
void cyber_ota_notify_net(bool connected);

// 读取当前快照。可从任意任务调用（含 LVGL 任务），临界区内完成，不阻塞。
void cyber_ota_get_snapshot(cyber_ota_snapshot_t *out);

// 读取对话端点配置（握手用）。由本模块持有 NVS 键，避免调用方重复定义字符串键。
// 任一字段缺失时写入空串。返回 ESP_OK 表示至少 url 非空。
// 可从任意任务调用。
esp_err_t cyber_ota_get_ws_config(char *url, size_t url_cap,
                                  char *token, size_t token_cap);

// 本机 STA MAC（"aa:bb:cc:dd:ee:ff"）。Device-Id 与 OTA 请求体必须同源，
// 因此由本模块统一提供，避免两处各自取一次而产生漂移。
esp_err_t cyber_ota_get_device_mac(char *out, size_t cap);

// Client-Id（NVS 中持久化）。对话握手的 Client-Id 头必须与 OTA 请求一致，
// 服务端靠它认同一台设备；重新生成会丢掉绑定。
esp_err_t cyber_ota_get_client_id(char *out, size_t cap);
