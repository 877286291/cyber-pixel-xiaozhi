// main/cyber_prov_util.h
// 蓝牙配网的【纯逻辑】：界面该显示哪个配网阶段、阶段文案、以及"凭证是否可提交"。
//
// 为什么不放在 cyber_prov.c 里：配网界面要同时反映两路独立事实——BLE 侧（手机连上
// 没有）与 Wi-Fi 侧（凭证生效没有），而这两路的组合是纯粹的布尔逻辑，可以在宿主上
// 逐种组合验证。放在需要真机 BLE 才能跑的代码里，就只能"改一行、烧一次"。
// 见 AGENTS.md 关于把可测逻辑与 ESP-IDF 隔离的要求。
#pragma once

#include <stdbool.h>

#include "cyber_net_util.h"

// 设备蓝牙广播名。微信小程序"蓝牙配网-FoloToy AI PASSPORT"只列出以 "BLUFI"
// 开头的设备，故这个名字不是随手取的，改了手机就扫不到。
// 放在纯逻辑头里：界面要显示它（告诉用户该选哪个设备），而界面层不依赖 ESP-IDF。
#define CYBER_PROV_DEVICE_NAME "BLUFI_FoloPassport"

// ---------------------------------------------------------------------------
// 配网阶段
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_PROV_OFF = 0,        // 未在配网
    CYBER_PROV_STARTING,       // 正在起 BLE/Wi-Fi
    CYBER_PROV_ADVERTISING,    // 已广播，等手机连接
    CYBER_PROV_BLE_CONNECTED,  // 手机已连上，等它下发凭证
    CYBER_PROV_WIFI_CONNECTING,// 已收到凭证，正在连接热点
    CYBER_PROV_WIFI_CONNECTED, // 热点连上并拿到 IP
    CYBER_PROV_FAILED,         // 启动或协商失败
    CYBER_PROV_COUNT,
} cyber_prov_state_t;

// ---------------------------------------------------------------------------
// 配网状态快照：由 cyber_prov（BLE/Wi-Fi 回调）维护，界面只读消费。
//
// 刻意用"几个独立布尔量"而不是单个状态枚举：BLE 与 Wi-Fi 是【两路独立事实】，
// 用一个枚举表达不了"手机还连着、同时热点也已经连上"这种组合（配网成功时就处于
// 这个组合下）。展示用哪个阶段由下面的 cyber_prov_display_state() 推导。
// ---------------------------------------------------------------------------
typedef struct {
    bool started;          // 配网已启动（BLE 栈已起）
    bool ble_connected;    // 手机已通过 BLE 连上
    bool wifi_connecting;  // 正在连接热点
    bool wifi_connected;   // 热点已连上且拿到 IP
    bool failed;           // 启动或协商失败（failed 为真时其余组合不参与展示）
} cyber_prov_status_t;

// 由状态快照推导"当前该显示哪个配网阶段"。
//
// 推导规则（顺序即优先级，逐条都有理由）：
//   1. failed 优先：失败了就要说失败，不能因为"手机还连着"而显示成正常在配网。
//   2. 热点已连上优先于手机已连接：配网成功那一刻手机通常还连着，此时要显示
//      "已连上 Wi-Fi"（以及 IP），而不是停在"等下发凭证"——后者会让用户以为没成功。
//   3. 正在连热点优先于手机已连接：收到凭证后进入连接中，这一刻手机也还连着。
//   4. 未启动 → OFF，不显示任何配网文案。
//   5. 其余按 BLE 是否连接落到 ADVERTISING / BLE_CONNECTED。
cyber_prov_state_t cyber_prov_display_state(const cyber_prov_status_t *st);

// 阶段短标签（英文，像素字体渲染）。越界一律回落到 OFF 的文案。
const char *cyber_prov_state_label(cyber_prov_state_t s);

// 该阶段是否表示"设备正等待用户拿手机来做点什么"（界面据此给操作提示）。
bool cyber_prov_awaits_phone(cyber_prov_state_t s);

// ---------------------------------------------------------------------------
// 凭证提交
// ---------------------------------------------------------------------------
// 累积中的凭证（来自 BLUFI 的下行 BSSID/SSID/passwd，可能分多帧到达）。
typedef struct {
    char ssid[CYBER_SSID_MAX_BYTES + 1];
    unsigned ssid_len;
    char pass[CYBER_PASS_MAX_BYTES + 1];
    unsigned pass_len;
    bool has_ssid;
    bool has_pass;
} cyber_prov_creds_t;

// 收到 SSID 帧。ssid_len 为 0 或超长时不改动，返回 false。
// 语义：SSID 到达即重置整份凭证（新网络重新配），与参考实现一致——否则上一次
// 残留的口令会配到新 SSID 上，表现为"密码明明对却连不上"。
bool cyber_prov_accept_ssid(cyber_prov_creds_t *c, const char *ssid, unsigned len);

// 收到口令帧。空口令表示开放网络，是合法输入（has_pass 置位）。
bool cyber_prov_accept_pass(cyber_prov_creds_t *c, const char *pass, unsigned len);

// 提交前校验：必须有 SSID，且 SSID/口令都要通过 cyber_net_util 的长度约束。
// 复用 cyber_net_util 的校验而不是在这里重写一遍——两处各写一份必然漂移，
// 而"配网存进去的凭证，联网侧读出来判非法"正是最难查的那种不一致。
bool cyber_prov_creds_valid(const cyber_prov_creds_t *c);
