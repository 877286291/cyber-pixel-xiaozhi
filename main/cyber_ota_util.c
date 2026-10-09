// main/cyber_ota_util.c
// 小智服务激活的纯逻辑实现。规格见 docs/development/engineering/xiaozhi-activation.md。
#include "cyber_ota_util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool cyber_ota_code_valid(const char *code)
{
    if (code == NULL) return false;

    for (size_t i = 0; i < CYBER_OTA_CODE_LEN; i++) {
        const char c = code[i];
        if (c < '0' || c > '9') return false;
    }

    // 必须【恰好】6 位：要求第 7 个字节就是结尾。
    // 否则 "1234567" 的前 6 位都是数字，会被误判为合法。
    return code[CYBER_OTA_CODE_LEN] == '\0';
}

uint32_t cyber_ota_poll_delay_ms(uint8_t attempt)
{
    if (attempt == 0u) return 0u;

    // 2s 起，每次 +2s，8s 封顶。饱和点取 4 使 4*2s 恰好等于上限，
    // 计数一旦饱和，delay 也同时到位，两者不会脱节。
    const uint32_t ms = (uint32_t)attempt * CYBER_OTA_POLL_MIN_MS;
    return (ms > CYBER_OTA_POLL_MAX_MS) ? CYBER_OTA_POLL_MAX_MS : ms;
}

uint8_t cyber_ota_poll_bump(uint8_t attempt)
{
    // 饱和而不是回绕。回绕会让 delay 变成 0，轮询静默停止，
    // 而且日志上完全看不出来（与 cyber_net 的重连计数是同一类缺陷）。
    return (attempt < 4u) ? (uint8_t)(attempt + 1u) : (uint8_t)4u;
}

cyber_ota_outcome_t cyber_ota_decide(bool has_activation_code, bool has_websocket,
                                     bool is_test_credentials)
{
    // 激活码优先：它的存在就是"尚未绑定"的权威信号。
    // 实测未绑定设备会同时拿到 code 与临时 websocket，所以必须先判 code，
    // 否则会把"挂临时账号"误报成就绪。
    if (has_activation_code) return CYBER_OTA_NEED_ACTIVATION;

    // 服务端不再要求激活，又给了可用端点 → 已绑定。
    // 不再区分"临时凭证"是否阻塞：绑定生效后服务端仍可能只给临时凭证
    // （#1785，实测于本设备），把这种情况判为不可用会让设备永远停在配对页。
    if (has_websocket) {
        return is_test_credentials ? CYBER_OTA_TEST_ONLY : CYBER_OTA_READY;
    }

    return CYBER_OTA_NO_CONFIG;
}

bool cyber_ota_is_test_token(const char *token)
{
    return (token != NULL) && (strcmp(token, "test-token") == 0);
}

bool cyber_ota_is_test_client_id(const char *client_id)
{
    return (client_id != NULL) && (strstr(client_id, "GID_test") != NULL);
}

// JSON 字符串值里是否可直接内联。出现引号/反斜杠/控制字符就拒绝。
static bool json_field_safe(const char *s)
{
    if (s == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p != 0u; p++) {
        if (*p == '"' || *p == '\\' || *p < 0x20u) return false;
    }
    return true;
}

size_t cyber_ota_build_body(char *out, size_t cap, const cyber_ota_body_t *b)
{
    if (out == NULL || cap == 0u || b == NULL) return 0u;

    // 不转义而是拒绝：这些字段全部来自固件常量与 MAC，出现需要转义的字符
    // 只可能是调用方传错了东西。静默转义会把一个 bug 变成一份语义错误的请求。
    if (!json_field_safe(b->mac) || !json_field_safe(b->board_type) ||
        !json_field_safe(b->app_name) || !json_field_safe(b->app_version) ||
        !json_field_safe(b->idf_version)) {
        return 0u;
    }

    // 字段集合与顺序刻意与已在真机上验证过的探测请求一致（见规格文档的
    // "Verified against the live service"），改动这里的形状等于改动已验证的事实。
    const int n = snprintf(
        out, cap,
        "{\"flash_size\":%" PRIu32 ",\"minimum_free_heap_size\":%" PRIu32 ","
        "\"mac_address\":\"%s\",\"chip_model_name\":\"esp32c3\","
        "\"chip_info\":{\"model\":5,\"cores\":1,\"revision\":3,\"features\":0},"
        "\"application\":{\"name\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\"},"
        "\"partition_table\":[],\"ota\":{\"label\":\"factory\"},"
        "\"board\":{\"type\":\"%s\",\"mac\":\"%s\"}}",
        (uint32_t)b->flash_size, (uint32_t)b->min_free_heap, b->mac,
        b->app_name, b->app_version, b->idf_version, b->board_type, b->mac);

    // 截断时宁可返回 0 也不发出半个 JSON：残缺请求会得到一个难以解释的服务端错误。
    if (n < 0 || (size_t)n >= cap) return 0u;
    return (size_t)n;
}
