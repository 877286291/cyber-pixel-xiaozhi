// main/cyber_net_util.c
// 联网纯逻辑的实现。全文件不含 esp_* / lv_* 调用，可直接编译进宿主单测。
#include "cyber_net_util.h"

bool cyber_ssid_valid(const char *ssid, size_t len)
{
    // 长度是唯一的硬约束：不检查字符集（见头文件说明）。
    if (ssid == NULL) return false;
    return len >= 1u && len <= CYBER_SSID_MAX_BYTES;
}

bool cyber_pass_valid(const char *pass, size_t len)
{
    // 开放网络：口令必须为空。指针为 NULL 时也按"空"处理，
    // 这样调用方不必先判断指针，避免漏掉 NULL 分支。
    if (pass == NULL || len == 0u) return true;
    return len >= CYBER_PASS_MIN_BYTES && len <= CYBER_PASS_MAX_BYTES;
}

cyber_wifi_auth_t cyber_wifi_auth_from_pass(const char *pass, size_t len)
{
    if (pass == NULL || len == 0u) return CYBER_WIFI_AUTH_OPEN;
    return CYBER_WIFI_AUTH_WPA2_PSK;
}

uint32_t cyber_net_retry_delay_ms(uint8_t attempt)
{
    if (attempt == 0u) return 0u;

    // 1 << 0..4，即 1s/2s/4s/8s/16s；再往上必须先截断移位量，
    // 否则 attempt 较大时移位会溢出成未定义行为。
    uint32_t shift = (uint32_t)(attempt - 1u);
    if (shift > 4u) shift = 4u;

    const uint32_t delay = 1000u << shift;
    return (delay > CYBER_NET_RETRY_MAX_MS) ? CYBER_NET_RETRY_MAX_MS : delay;
}

uint8_t cyber_net_retry_bump(uint8_t attempt)
{
    // 饱和而不是回绕：回绕会让 delay 变成 0，退避定时器不再启动，
    // 重连静默停止（见头文件说明）。
    return (attempt < CYBER_NET_RETRY_SATURATE) ? (uint8_t)(attempt + 1u)
                                               : (uint8_t)CYBER_NET_RETRY_SATURATE;
}

cyber_net_err_t cyber_net_classify_reason(uint8_t reason)
{
    // 数值 = wifi_err_reason_t。注释里保留符号名，方便对照 IDF 头文件。
    switch (reason) {
    case 201:   // WIFI_REASON_NO_AP_FOUND
        return CYBER_NET_ERR_AP_NOT_FOUND;

    case 202:   // WIFI_REASON_AUTH_FAIL
    case 15:    // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 2:     // WIFI_REASON_AUTH_EXPIRE
    case 4:     // WIFI_REASON_ASSOC_EXPIRE
        return CYBER_NET_ERR_AUTH_FAIL;

    case 200:   // WIFI_REASON_BEACON_TIMEOUT
    case 203:   // WIFI_REASON_ASSOC_FAIL
    case 204:   // WIFI_REASON_HANDSHAKE_TIMEOUT
    case 205:   // WIFI_REASON_CONNECTION_FAIL
        return CYBER_NET_ERR_TIMEOUT;

    default:
        // 不认识的原因不硬套成上面几种：错误页宁可显示"连接失败"，
        // 也不要给出一个会被用户照着排查却方向错误的提示。
        return CYBER_NET_ERR_OTHER;
    }
}
