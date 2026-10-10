// main/cyber_net_util.h
// 联网相关的【纯逻辑】：凭证合法性、鉴权推导、重连退避。
//
// 为什么不放在 cyber_net.c 里：这些都是"给个输入就该有个确定答案"的判断，
// 与 ESP-IDF 无关，可以脱离硬件在宿主上验证。联网代码本身要 flash 才能试，
// 把这类判断剥出来能省下大量"改一行、烧一次"的往返（见 AGENTS.md 关于
// 把可测逻辑与 ESP-IDF 隔离的要求）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 凭证约束：ST7789 面板本身无关，这里是 802.11 + WPA2-PSK 的硬限制。
// ---------------------------------------------------------------------------
#define CYBER_SSID_MAX_BYTES  32   // SSID 上限 32 字节（按字节算,不是字符）
#define CYBER_PASS_MIN_BYTES   8   // WPA2-PSK 口令下限
#define CYBER_PASS_MAX_BYTES  63   // WPA2-PSK 口令上限（64 位十六进制裸 PSK 不支持）

// SSID 合法性：长度在 1..32 字节之间。
// 不做字符集检查：SSID 是任意字节串，中文 SSID 合法，过滤只会误伤。
bool cyber_ssid_valid(const char *ssid, size_t len);

// 口令合法性：0 字节表示开放网络；否则必须 8..63 字节。
bool cyber_pass_valid(const char *pass, size_t len);

// 由口令长度推导鉴权方式。
// 刻意不支持"64 位十六进制裸 PSK"：它需要额外区分长度 64 与十六进制字符集，
// 而家用路由几乎不会用到，加进来只会多一条永远测不到的路径。
typedef enum {
    CYBER_WIFI_AUTH_OPEN = 0,
    CYBER_WIFI_AUTH_WPA2_PSK,
} cyber_wifi_auth_t;

// pass 为空(len==0)返回 OPEN；否则返回 WPA2_PSK。
// 非法口令由调用方先用 cyber_pass_valid() 拦掉，本函数不做校验。
cyber_wifi_auth_t cyber_wifi_auth_from_pass(const char *pass, size_t len);

// ---------------------------------------------------------------------------
// 重连退避
// ---------------------------------------------------------------------------
#define CYBER_NET_RETRY_MAX_MS  15000u  // 退避上限，避免长时间连不上时无限拉长

// 重连尝试计数的饱和上限。计数器到此不再增长。
//
// 这不是"顺手加的保险"，而是修掉一个会让重连【永久失效】的缺陷：
// 若计数用 uint8_t 且无饱和，连续失败 256 次后会回绕到 0，而 delay(0)==0
// 意味着退避定时器不再被启动 —— 重连从此彻底停止，且日志上看不出任何异常。
// 按 15s 上限算，256 次约 1 小时：设备整夜开着而路由器是关的，就会踩到。
#define CYBER_NET_RETRY_SATURATE 5u

// 第 attempt 次重连前的等待毫秒数（attempt 从 1 开始计）。
// attempt==0 返回 0（无需等待）。
// 退避序列：1s, 2s, 4s, 8s，之后稳定在 CYBER_NET_RETRY_MAX_MS。
// 上限的意义：路由器重启时最长 15s 就能重连，不会像纯指数退避那样等到几分钟。
uint32_t cyber_net_retry_delay_ms(uint8_t attempt);

// 递增尝试计数（饱和到 CYBER_NET_RETRY_SATURATE，绝不回绕）。
// 不变式：cyber_net_retry_bump() 之后 delay 必 > 0，即"重连永远不会停止"。
uint8_t cyber_net_retry_bump(uint8_t attempt);

// ---------------------------------------------------------------------------
// 连接失败的分类
// 把 Wi-Fi 驱动的 reason 码归一成少数几种"用户看得懂的原因"，供错误页给出
// 准确提示，而不是笼统的 OFFLINE。分类是纯函数，可在宿主上覆盖各 reason。
//
// reason 数值来自 ESP-IDF 的 wifi_err_reason_t，是稳定 ABI，可直接按整数匹配；
// 这里只列本项目会遇到的常见值，其余归入 OTHER（不猜、不硬套）。
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_NET_ERR_NONE = 0,      // 无错误（已连上，或仍在正常连接窗口内）
    CYBER_NET_ERR_NO_CREDS,      // 未配置 Wi-Fi 凭证（不是驱动 reason，由上层判定）
    CYBER_NET_ERR_AP_NOT_FOUND,  // 找不到热点：SSID 不存在或信号太弱
    CYBER_NET_ERR_AUTH_FAIL,     // 口令错误或鉴权失败
    CYBER_NET_ERR_TIMEOUT,       // 关联/握手超时，或 AP 无响应
    CYBER_NET_ERR_SERVER,        // 已联网但服务端不可达（不是驱动 reason，由上层判定）
    CYBER_NET_ERR_OTHER,         // 其余原因
    CYBER_NET_ERR_COUNT,
} cyber_net_err_t;

// 由 Wi-Fi 断开的 reason 码分类（见上表）。reason==0（尚无原因）归入 OTHER。
cyber_net_err_t cyber_net_classify_reason(uint8_t reason);
