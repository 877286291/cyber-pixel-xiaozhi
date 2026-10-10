// tests/test_cyber_net_util.c
// 联网纯逻辑的宿主单测：凭证校验、鉴权推导、重连退避。
// 这些判断在设备上只能靠"改一行、烧一次"来试，放在宿主上几分钟就能覆盖完边界。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_net_util.h"

static void test_ssid_valid(void)
{
    // NULL 与空串都不合法：没有 SSID 就无从连接。
    assert(!cyber_ssid_valid(NULL, 0));
    assert(!cyber_ssid_valid("", 0));

    assert(cyber_ssid_valid("A", 1));
    assert(cyber_ssid_valid("home-wifi", 9));

    // 边界：32 字节是 802.11 的上限，33 必须被拒。
    char buf[CYBER_SSID_MAX_BYTES + 2];
    memset(buf, 'x', sizeof(buf));
    assert(cyber_ssid_valid(buf, CYBER_SSID_MAX_BYTES));
    assert(!cyber_ssid_valid(buf, CYBER_SSID_MAX_BYTES + 1));

    // 中文 SSID 是合法字节串，不能因为非 ASCII 就被拒。
    const char *utf8 = "\xe5\xae\xb6\xe7\x94\xa8";   // "家用"
    assert(cyber_ssid_valid(utf8, strlen(utf8)));
}

static void test_pass_valid(void)
{
    // 开放网络：空口令是合法的，NULL 也按空处理（免得调用方漏判指针）。
    assert(cyber_pass_valid(NULL, 0));
    assert(cyber_pass_valid("", 0));

    // 1..7 字节是 WPA2 不接受的区间，必须拦掉。
    for (size_t len = 1; len < CYBER_PASS_MIN_BYTES; len++) {
        char buf[CYBER_PASS_MIN_BYTES];
        memset(buf, 'p', sizeof(buf));
        assert(!cyber_pass_valid(buf, len));
    }

    // 8 与 63 是合法端点；64（裸 PSK 长度）刻意不支持。
    char buf[CYBER_PASS_MAX_BYTES + 2];
    memset(buf, 'p', sizeof(buf));
    assert(cyber_pass_valid(buf, CYBER_PASS_MIN_BYTES));
    assert(cyber_pass_valid(buf, CYBER_PASS_MAX_BYTES));
    assert(!cyber_pass_valid(buf, CYBER_PASS_MAX_BYTES + 1));
}

static void test_auth_from_pass(void)
{
    // 无口令 → 开放网络；有口令 → WPA2。
    assert(cyber_wifi_auth_from_pass(NULL, 0) == CYBER_WIFI_AUTH_OPEN);
    assert(cyber_wifi_auth_from_pass("", 0) == CYBER_WIFI_AUTH_OPEN);
    assert(cyber_wifi_auth_from_pass("12345678", 8) == CYBER_WIFI_AUTH_WPA2_PSK);
}

static void test_retry_delay(void)
{
    // 第 0 次不需要等待。
    assert(cyber_net_retry_delay_ms(0) == 0);

    // 退避序列：1s → 2s → 4s → 8s，然后被上限截断在 15s。
    assert(cyber_net_retry_delay_ms(1) == 1000);
    assert(cyber_net_retry_delay_ms(2) == 2000);
    assert(cyber_net_retry_delay_ms(3) == 4000);
    assert(cyber_net_retry_delay_ms(4) == 8000);
    assert(cyber_net_retry_delay_ms(5) == CYBER_NET_RETRY_MAX_MS);
    assert(cyber_net_retry_delay_ms(6) == CYBER_NET_RETRY_MAX_MS);

    // attempt 取到 uint8_t 上限也不能移位溢出（这是最容易被写错的地方）。
    assert(cyber_net_retry_delay_ms(255) == CYBER_NET_RETRY_MAX_MS);

    // 全序列单调不减且不超上限：退避一旦回退就会变成"疯狂重连"。
    uint32_t prev = 0;
    for (int attempt = 0; attempt <= 255; attempt++) {
        const uint32_t d = cyber_net_retry_delay_ms((uint8_t)attempt);
        assert(d >= prev);
        assert(d <= CYBER_NET_RETRY_MAX_MS);
        prev = d;
    }

    // 上限存在的意义：路由器重启后应尽快恢复，而不是像纯指数退避那样等到分钟级。
    assert(CYBER_NET_RETRY_MAX_MS <= 30000u);
}

static void test_retry_bump_saturation(void)
{
    // 从 0 开始逐次递增到饱和点，之后必须【保持不变】而不是回绕。
    uint8_t a = 0;
    for (uint8_t i = 1; i <= CYBER_NET_RETRY_SATURATE; i++) {
        a = cyber_net_retry_bump(a);
        assert(a == i);
    }
    assert(cyber_net_retry_bump(a) == CYBER_NET_RETRY_SATURATE);
    assert(cyber_net_retry_bump(CYBER_NET_RETRY_SATURATE) == CYBER_NET_RETRY_SATURATE);

    // 关键回归：uint8_t 上限附近绝不能回绕到 0。
    // 回绕会让 delay 变成 0 → 退避定时器不再启动 → 重连永久静默停止。
    assert(cyber_net_retry_bump(200) == CYBER_NET_RETRY_SATURATE);
    assert(cyber_net_retry_bump(255) == CYBER_NET_RETRY_SATURATE);

    // 不变式：递增之后 delay 必 > 0，无论从哪个值出发。
    // 这条就是"Wi-Fi 断开后一定能自动重连"的形式化表述。
    for (int start = 0; start <= 255; start++) {
        const uint8_t v = cyber_net_retry_bump((uint8_t)start);
        assert(cyber_net_retry_delay_ms(v) > 0u);
    }

    // 反复递增（模拟长时间连不上）也要一直保持可重试。
    uint8_t v = 0;
    for (int i = 0; i < 1000; i++) {
        v = cyber_net_retry_bump(v);
        assert(cyber_net_retry_delay_ms(v) > 0u);
    }
    assert(v == CYBER_NET_RETRY_SATURATE);
}

static void test_classify_reason(void)
{
    // 常见原因各自归类（数值 = wifi_err_reason_t），不认识的归 OTHER（不硬套）。
    assert(cyber_net_classify_reason(201) == CYBER_NET_ERR_AP_NOT_FOUND);  // NO_AP_FOUND
    assert(cyber_net_classify_reason(202) == CYBER_NET_ERR_AUTH_FAIL);     // AUTH_FAIL
    assert(cyber_net_classify_reason(15) == CYBER_NET_ERR_AUTH_FAIL);      // 4WAY_HANDSHAKE_TIMEOUT
    assert(cyber_net_classify_reason(2) == CYBER_NET_ERR_AUTH_FAIL);       // AUTH_EXPIRE
    assert(cyber_net_classify_reason(4) == CYBER_NET_ERR_AUTH_FAIL);       // ASSOC_EXPIRE
    assert(cyber_net_classify_reason(200) == CYBER_NET_ERR_TIMEOUT);       // BEACON_TIMEOUT
    assert(cyber_net_classify_reason(203) == CYBER_NET_ERR_TIMEOUT);       // ASSOC_FAIL
    assert(cyber_net_classify_reason(204) == CYBER_NET_ERR_TIMEOUT);       // HANDSHAKE_TIMEOUT
    assert(cyber_net_classify_reason(205) == CYBER_NET_ERR_TIMEOUT);       // CONNECTION_FAIL

    // 0（还没有原因）与不认识的码都不能落进上面几类：错误页宁可说"连接失败"，
    // 也不要给出一条会被用户照着排查、方向却错了的提示。
    assert(cyber_net_classify_reason(0) == CYBER_NET_ERR_OTHER);
    assert(cyber_net_classify_reason(1) == CYBER_NET_ERR_OTHER);
    assert(cyber_net_classify_reason(210) == CYBER_NET_ERR_OTHER);
    assert(cyber_net_classify_reason(255) == CYBER_NET_ERR_OTHER);

    // 结果恒在合法枚举范围内（越界会被 UI 当成"无错误"而漏掉整页提示）。
    for (int r = 0; r <= 255; r++) {
        const cyber_net_err_t e = cyber_net_classify_reason((uint8_t)r);
        assert(e > CYBER_NET_ERR_NONE && e < CYBER_NET_ERR_COUNT);
    }
}

int main(void)
{
    test_ssid_valid();
    test_pass_valid();
    test_auth_from_pass();
    test_retry_delay();
    test_retry_bump_saturation();
    test_classify_reason();

    printf("test_cyber_net_util: PASS\n");
    return 0;
}
