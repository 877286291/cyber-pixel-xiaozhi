// main/cyber_prov_util.c
// 蓝牙配网纯逻辑的实现。全文件不含 esp_* / lv_* / nimble 调用，可直接编译进宿主单测。
#include "cyber_prov_util.h"

#include <string.h>

// 阶段标签。数组顺序必须与 cyber_prov_state_t 一致。
static const char *const PROV_LABEL[CYBER_PROV_COUNT] = {
    "PROVISION OFF",
    "STARTING...",
    "READY TO PAIR",
    "PHONE LINKED",
    "LINKING WIFI",
    "WIFI ONLINE",
    "PAIR FAILED",
};

cyber_prov_state_t cyber_prov_display_state(const cyber_prov_status_t *st)
{
    if (st == NULL) return CYBER_PROV_OFF;

    // 1) 失败压过一切：设备真出问题时不能因为"手机还连着"而显示成在正常配网。
    if (st->failed) return CYBER_PROV_FAILED;

    // 4) 未启动：不显示任何配网文案（其余标志位无意义）。
    if (!st->started) return CYBER_PROV_OFF;

    // 2) 热点已连上优先于手机已连接：配网成功那一刻手机通常还连着，
    //    要显示"已连上 Wi-Fi"，否则用户会以为没成功。
    if (st->wifi_connected) return CYBER_PROV_WIFI_CONNECTED;

    // 3) 正在连热点也优先于手机已连接：收到凭证后手机仍连着。
    if (st->wifi_connecting) return CYBER_PROV_WIFI_CONNECTING;

    // 5) 其余按 BLE 侧落到"等手机"或"等下发凭证"。
    return st->ble_connected ? CYBER_PROV_BLE_CONNECTED : CYBER_PROV_ADVERTISING;
}

const char *cyber_prov_state_label(cyber_prov_state_t s)
{
    if (s >= CYBER_PROV_COUNT) return PROV_LABEL[CYBER_PROV_OFF];
    return PROV_LABEL[s];
}

bool cyber_prov_awaits_phone(cyber_prov_state_t s)
{
    // 只剩这两种阶段需要用户动手（去手机上打开小程序/输凭证）。
    return s == CYBER_PROV_ADVERTISING || s == CYBER_PROV_BLE_CONNECTED;
}

bool cyber_prov_accept_ssid(cyber_prov_creds_t *c, const char *ssid, unsigned len)
{
    if (c == NULL || ssid == NULL) return false;
    // 长度非法一律不采纳：宁可让用户重发，也不要存一份注定连不上的 SSID。
    if (len == 0u || len > CYBER_SSID_MAX_BYTES) return false;

    // 新 SSID 到达 = 这是一次新的配网：把整份凭证重置，
    // 否则上一轮残留的口令会配到新 SSID 上，表现为"密码对却连不上"。
    memset(c, 0, sizeof(*c));

    memcpy(c->ssid, ssid, len);
    c->ssid[len] = '\0';
    c->ssid_len = len;
    c->has_ssid = true;
    return true;
}

bool cyber_prov_accept_pass(cyber_prov_creds_t *c, const char *pass, unsigned len)
{
    if (c == NULL) return false;
    // 超长直接拒；空口令是合法输入（开放网络），要显式标记"口令已收到"，
    // 否则开放网络的连接请求会因为没有 has_pass 而被当成"凭证不全"。
    if (len > CYBER_PASS_MAX_BYTES) return false;
    if (len > 0u && pass == NULL) return false;

    memset(c->pass, 0, sizeof(c->pass));
    if (len > 0u) memcpy(c->pass, pass, len);
    c->pass[len] = '\0';
    c->pass_len = len;
    c->has_pass = true;
    return true;
}

bool cyber_prov_creds_valid(const cyber_prov_creds_t *c)
{
    if (c == NULL || !c->has_ssid) return false;
    // 口令没收到时按开放网络处理（与 cyber_net 读取 NVS 时"没有 pass 键 = 开放网络"一致）。
    const char *pass = c->has_pass ? c->pass : "";
    const unsigned pass_len = c->has_pass ? c->pass_len : 0u;
    return cyber_ssid_valid(c->ssid, c->ssid_len) &&
           cyber_pass_valid(pass, (size_t)pass_len);
}
