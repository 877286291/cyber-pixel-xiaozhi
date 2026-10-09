// tests/test_cyber_ota_util.c
// 小智激活纯逻辑的宿主单测：激活码校验、轮询节奏、结果判定、测试凭证识别、请求体构造。
// 这些都只能靠"改一行、烧一次、看日志"来验证，放在宿主上可以把边界一次覆盖完。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_ota_util.h"

static void test_code_valid(void)
{
    // NULL 与空串不合法。
    assert(!cyber_ota_code_valid(NULL));
    assert(!cyber_ota_code_valid(""));

    // 长度不足 / 超长都必须被拒。
    assert(!cyber_ota_code_valid("1"));
    assert(!cyber_ota_code_valid("12345"));
    assert(!cyber_ota_code_valid("1234567"));
    assert(!cyber_ota_code_valid("12345678"));

    // 非数字字符一律不合法。
    assert(!cyber_ota_code_valid("12345a"));
    assert(!cyber_ota_code_valid("abcdef"));
    assert(!cyber_ota_code_valid(" 12345"));
    assert(!cyber_ota_code_valid("123 456"));
    assert(!cyber_ota_code_valid("+12345"));
    assert(!cyber_ota_code_valid("-12345"));
    // 全角数字不是 ASCII 数字。
    assert(!cyber_ota_code_valid("\xef\xbc\x91\xef\xbc\x92\xef\xbc\x93\xef\xbc\x94\xef\xbc\x95\xef\xbc\x96"));

    // 端点值合法。
    assert(cyber_ota_code_valid("000000"));
    assert(cyber_ota_code_valid("999999"));
    assert(cyber_ota_code_valid("663840"));
}

static void test_poll_delay(void)
{
    // 第 0 次不需要等待。
    assert(cyber_ota_poll_delay_ms(0) == 0);

    // 序列：2s → 4s → 6s → 8s，之后被上限截断。
    assert(cyber_ota_poll_delay_ms(1) == 2000);
    assert(cyber_ota_poll_delay_ms(2) == 4000);
    assert(cyber_ota_poll_delay_ms(3) == 6000);
    assert(cyber_ota_poll_delay_ms(4) == CYBER_OTA_POLL_MAX_MS);
    assert(cyber_ota_poll_delay_ms(5) == CYBER_OTA_POLL_MAX_MS);
    assert(cyber_ota_poll_delay_ms(255) == CYBER_OTA_POLL_MAX_MS);

    // 全序列单调不减且不超上限。乘法不会溢出：attempt 最大 255，255*2000 远小于 UINT32_MAX。
    uint32_t prev = 0;
    for (int attempt = 0; attempt <= 255; attempt++) {
        const uint32_t d = cyber_ota_poll_delay_ms((uint8_t)attempt);
        assert(d >= prev);
        assert(d <= CYBER_OTA_POLL_MAX_MS);
        prev = d;
    }

    // 轮询不能比用户耐心还慢：上限必须是个"人等在屏幕前"能接受的量级。
    assert(CYBER_OTA_POLL_MAX_MS <= 15000u);
}

static void test_poll_bump_saturation(void)
{
    // 饱和点是 4，且此时 delay 恰为上限（两者不脱节）。
    uint8_t a = 0;
    a = cyber_ota_poll_bump(a); assert(a == 1);
    a = cyber_ota_poll_bump(a); assert(a == 2);
    a = cyber_ota_poll_bump(a); assert(a == 3);
    a = cyber_ota_poll_bump(a); assert(a == 4);
    assert(cyber_ota_poll_delay_ms(a) == CYBER_OTA_POLL_MAX_MS);
    assert(cyber_ota_poll_bump(a) == 4);
    assert(cyber_ota_poll_bump(4) == 4);

    // 关键回归：uint8_t 上限附近绝不能回绕到 0。
    // 回绕会让 delay 变成 0 → 轮询静默停止 → 用户输完码设备永远不知道。
    assert(cyber_ota_poll_bump(200) == 4);
    assert(cyber_ota_poll_bump(255) == 4);

    // 不变式：递增之后 delay 必 > 0。这就是"绑定完成后一定能就绪"的形式化表述。
    for (int start = 0; start <= 255; start++) {
        assert(cyber_ota_poll_delay_ms(cyber_ota_poll_bump((uint8_t)start)) > 0u);
    }
    uint8_t v = 0;
    for (int i = 0; i < 1000; i++) {
        v = cyber_ota_poll_bump(v);
        assert(cyber_ota_poll_delay_ms(v) > 0u);
    }
    assert(v == 4);
}

static void test_decide(void)
{
    // 实测回归：未绑定设备的响应【同时】含 activation.code 与 token="test-token"
    // 的临时 websocket。这种组合必须判为"需要激活"。
    // 早期版本因"有 websocket 就判 READY"，会让设备自信地报就绪却一直挂在临时
    // 账号上、激活码永远不显示——这个错误是真机上直接暴露出来的。
    assert(cyber_ota_decide(true, true, true) == CYBER_OTA_NEED_ACTIVATION);
    assert(cyber_ota_decide(true, false, true) == CYBER_OTA_NEED_ACTIVATION);
    // 有激活码时，凭证真假都不影响：激活码的存在就是"未绑定"的权威信号。
    assert(cyber_ota_decide(true, true, false) == CYBER_OTA_NEED_ACTIVATION);
    assert(cyber_ota_decide(true, false, false) == CYBER_OTA_NEED_ACTIVATION);

    // 无激活码 + 非临时凭证 → 真正就绪。
    assert(cyber_ota_decide(false, true, false) == CYBER_OTA_READY);

    // 无激活码 + 临时凭证 → TEST_ONLY：仍然算"已绑定"，只是服务端只给临时账号
    // （#1785，实测于本设备）。它【不是】阻塞态——判成不可用会让一台已经绑好的
    // 设备永远停在配对页，因为服务端再也不会给真凭证。由 cyber_pair_blocks_ui
    // 保证它进主界面，故障由日志呈现。
    assert(cyber_ota_decide(false, true, true) == CYBER_OTA_TEST_ONLY);

    // 什么都没有 → 无可用配置，而不是当成成功。
    assert(cyber_ota_decide(false, false, false) == CYBER_OTA_NO_CONFIG);
    assert(cyber_ota_decide(false, false, true) == CYBER_OTA_NO_CONFIG);

    // 不变式：只有 NEED_ACTIVATION 才表示"未绑定"。服务端只对未绑定的设备下发
    // activation，所以"不再要求激活"就是"已绑定"——不存在既不要求激活、又确实
    // 未绑定的响应。
    for (int code = 0; code <= 1; code++) {
        for (int ws = 0; ws <= 1; ws++) {
            for (int test = 0; test <= 1; test++) {
                const cyber_ota_outcome_t o =
                    cyber_ota_decide(code != 0, ws != 0, test != 0);
                assert((o == CYBER_OTA_NEED_ACTIVATION) == (code != 0));
                if (!code && !ws) assert(o == CYBER_OTA_NO_CONFIG);
            }
        }
    }
}

static void test_test_credentials(void)
{
    assert(cyber_ota_is_test_token("test-token"));
    assert(!cyber_ota_is_test_token("real-token"));
    assert(!cyber_ota_is_test_token(""));
    assert(!cyber_ota_is_test_token(NULL));
    // 大小写敏感：协议给的是小写字面量。
    assert(!cyber_ota_is_test_token("TEST-TOKEN"));

    assert(cyber_ota_is_test_client_id("GID_test@@@aa_bb@@@uuid"));
    assert(cyber_ota_is_test_client_id("GID_test"));
    assert(!cyber_ota_is_test_client_id("GID_real@@@aa_bb"));
    assert(!cyber_ota_is_test_client_id(""));
    assert(!cyber_ota_is_test_client_id(NULL));
}

static void test_build_body(void)
{
    const cyber_ota_body_t b = {
        .mac = "aa:bb:cc:dd:ee:ff",
        .board_type = "cyber-pixel-xiaozhi",
        .app_name = "cyber-pixel-xiaozhi",
        .app_version = "0.1.0",
        .idf_version = "v5.5.3",
        .flash_size = 8388608,
        .min_free_heap = 150000,
    };

    // 形状必须与已在真机上验证过的探测请求完全一致。
    const char *expected =
        "{\"flash_size\":8388608,\"minimum_free_heap_size\":150000,"
        "\"mac_address\":\"aa:bb:cc:dd:ee:ff\",\"chip_model_name\":\"esp32c3\","
        "\"chip_info\":{\"model\":5,\"cores\":1,\"revision\":3,\"features\":0},"
        "\"application\":{\"name\":\"cyber-pixel-xiaozhi\",\"version\":\"0.1.0\","
        "\"idf_version\":\"v5.5.3\"},"
        "\"partition_table\":[],\"ota\":{\"label\":\"factory\"},"
        "\"board\":{\"type\":\"cyber-pixel-xiaozhi\",\"mac\":\"aa:bb:cc:dd:ee:ff\"}}";

    char buf[512];
    const size_t n = cyber_ota_build_body(buf, sizeof(buf), &b);
    assert(n == strlen(expected));
    assert(strcmp(buf, expected) == 0);

    // Device-Id 与请求体 MAC 必须同源，否则服务端回 "Invalid OTA request"。
    // 这里断言 body 里出现的 MAC 与入参一致（且出现两次）。
    const char *first = strstr(buf, "\"aa:bb:cc:dd:ee:ff\"");
    assert(first != NULL);
    assert(strstr(first + 1, "aa:bb:cc:dd:ee:ff") != NULL);

    // 截断必须返回 0（宁可失败也不发出半个 JSON）。
    assert(cyber_ota_build_body(buf, n, &b) == 0);
    assert(cyber_ota_build_body(buf, n + 1, &b) == n);   // 刚好放下时才成功
    assert(cyber_ota_build_body(buf, 8, &b) == 0);
    assert(cyber_ota_build_body(buf, 1, &b) == 0);
    assert(cyber_ota_build_body(buf, 0, &b) == 0);

    // 参数非法一律返回 0。
    assert(cyber_ota_build_body(NULL, sizeof(buf), &b) == 0);
    assert(cyber_ota_build_body(buf, sizeof(buf), NULL) == 0);

    // 需要 JSON 转义的字符会被拒绝，而不是被静默转义。
    cyber_ota_body_t bad = b;
    bad.app_name = "say \"hi\"";
    assert(cyber_ota_build_body(buf, sizeof(buf), &bad) == 0);
    bad = b;
    bad.mac = "aa\\bb";
    assert(cyber_ota_build_body(buf, sizeof(buf), &bad) == 0);
    bad = b;
    bad.board_type = "line\nbreak";
    assert(cyber_ota_build_body(buf, sizeof(buf), &bad) == 0);
    bad = b;
    bad.app_version = NULL;
    assert(cyber_ota_build_body(buf, sizeof(buf), &bad) == 0);
}

int main(void)
{
    test_code_valid();
    test_poll_delay();
    test_poll_bump_saturation();
    test_decide();
    test_test_credentials();
    test_build_body();

    printf("test_cyber_ota_util: PASS\n");
    return 0;
}
