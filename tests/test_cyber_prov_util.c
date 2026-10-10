// tests/test_cyber_prov_util.c
// 蓝牙配网纯逻辑的宿主单测：阶段推导、文案、凭证累积与校验。
// 这些判断在设备上要靠"手机连一次、输一次密码"才能试，放宿主上几毫秒就覆盖完。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_prov_util.h"

static void test_display_state_priority(void)
{
    // 未启动：无论其他标志如何都显示 OFF（其余标志此时无意义）。
    cyber_prov_status_t st = { 0 };
    assert(cyber_prov_display_state(&st) == CYBER_PROV_OFF);
    st.ble_connected = true;
    st.wifi_connected = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_OFF);

    // 已启动但手机没连：等手机。
    memset(&st, 0, sizeof(st));
    st.started = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_ADVERTISING);

    // 手机连上、还没收到可用凭证：等下发凭证。
    st.ble_connected = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_BLE_CONNECTED);

    // 收到凭证、正在连热点：手机还连着，但必须优先显示"连接中"。
    st.wifi_connecting = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_WIFI_CONNECTING);

    // 热点连上：手机仍连着，必须优先显示"已连上"（否则用户以为没成功）。
    st.wifi_connecting = false;
    st.wifi_connected = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_WIFI_CONNECTED);

    // 手机断开但热点已连上：仍显示"已连上"。
    st.ble_connected = false;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_WIFI_CONNECTED);

    // 失败压过一切，包括"热点已连上"和"手机还连着"。
    st.failed = true;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_FAILED);
    st.started = false;
    assert(cyber_prov_display_state(&st) == CYBER_PROV_FAILED);

    // 空指针不得越界。
    assert(cyber_prov_display_state(NULL) == CYBER_PROV_OFF);
}

static void test_display_state_full_truth_table(void)
{
    // 穷举所有布尔组合，钉住两条不变式：
    //   (1) 结果恒是合法枚举值；
    //   (2) failed 为真时恒为 FAILED，started 为假时恒为 OFF（除 failed）。
    for (int bits = 0; bits < 32; bits++) {
        cyber_prov_status_t st = {
            .started = (bits & 1) != 0,
            .ble_connected = (bits & 2) != 0,
            .wifi_connecting = (bits & 4) != 0,
            .wifi_connected = (bits & 8) != 0,
            .failed = (bits & 16) != 0,
        };
        const cyber_prov_state_t s = cyber_prov_display_state(&st);
        assert(s >= CYBER_PROV_OFF && s < CYBER_PROV_COUNT);
        if (st.failed) {
            assert(s == CYBER_PROV_FAILED);
        } else if (!st.started) {
            assert(s == CYBER_PROV_OFF);
        } else if (st.wifi_connected) {
            assert(s == CYBER_PROV_WIFI_CONNECTED);
        } else if (st.wifi_connecting) {
            assert(s == CYBER_PROV_WIFI_CONNECTING);
        }
    }
}

static void test_labels(void)
{
    // 每个阶段都要有非空文字，否则配网页会出现一行空白，用户不知道设备在干什么。
    for (int i = 0; i < CYBER_PROV_COUNT; i++) {
        const char *l = cyber_prov_state_label((cyber_prov_state_t)i);
        assert(l != NULL && l[0] != '\0');
    }
    // 越界回落，不读表外。
    assert(strcmp(cyber_prov_state_label(CYBER_PROV_COUNT),
                  cyber_prov_state_label(CYBER_PROV_OFF)) == 0);
    assert(strcmp(cyber_prov_state_label((cyber_prov_state_t)999),
                  cyber_prov_state_label(CYBER_PROV_OFF)) == 0);
}

static void test_awaits_phone(void)
{
    // 只有"等手机连接"和"等下发凭证"这两个阶段需要用户动手。
    assert(cyber_prov_awaits_phone(CYBER_PROV_ADVERTISING));
    assert(cyber_prov_awaits_phone(CYBER_PROV_BLE_CONNECTED));
    assert(!cyber_prov_awaits_phone(CYBER_PROV_OFF));
    assert(!cyber_prov_awaits_phone(CYBER_PROV_STARTING));
    assert(!cyber_prov_awaits_phone(CYBER_PROV_WIFI_CONNECTING));
    assert(!cyber_prov_awaits_phone(CYBER_PROV_WIFI_CONNECTED));
    assert(!cyber_prov_awaits_phone(CYBER_PROV_FAILED));
}

static void test_accept_ssid(void)
{
    cyber_prov_creds_t c = { 0 };

    assert(cyber_prov_accept_ssid(&c, "MyWiFi", 6));
    assert(c.has_ssid);
    assert(c.ssid_len == 6);
    assert(strcmp(c.ssid, "MyWiFi") == 0);
    assert(!c.has_pass);   // 新 SSID 到达即重置，口令回到"未收到"

    // 空 SSID 与超长 SSID 都不采纳，且不改动已有内容。
    assert(!cyber_prov_accept_ssid(&c, "", 0));
    assert(!cyber_prov_accept_ssid(&c, NULL, 3));
    assert(strcmp(c.ssid, "MyWiFi") == 0);

    char big[CYBER_SSID_MAX_BYTES + 2];
    memset(big, 'x', sizeof(big));
    assert(!cyber_prov_accept_ssid(&c, big, CYBER_SSID_MAX_BYTES + 1));
    assert(strcmp(c.ssid, "MyWiFi") == 0);

    // 边界：恰好 32 字节合法。
    assert(cyber_prov_accept_ssid(&c, big, CYBER_SSID_MAX_BYTES));
    assert(c.ssid_len == CYBER_SSID_MAX_BYTES);

    // 中文 SSID 是合法字节串（按字节计），不能被误拒。
    const char *utf8 = "\xe5\xae\xb6\xe7\x94\xa8";   // "家用"
    assert(cyber_prov_accept_ssid(&c, utf8, (unsigned)strlen(utf8)));
    assert(strcmp(c.ssid, "家用") == 0);

    // 关键：带对口令后再来一个新 SSID，旧口令必须被清掉。
    assert(cyber_prov_accept_pass(&c, "secret123", 9));
    assert(c.has_pass);
    assert(cyber_prov_accept_ssid(&c, "Another", 7));
    assert(!c.has_pass);
    assert(c.pass_len == 0);
    assert(c.pass[0] == '\0');
}

static void test_accept_pass(void)
{
    cyber_prov_creds_t c = { 0 };
    assert(cyber_prov_accept_ssid(&c, "Net", 3));

    assert(cyber_prov_accept_pass(&c, "12345678", 8));
    assert(c.has_pass);
    assert(c.pass_len == 8);
    assert(strcmp(c.pass, "12345678") == 0);

    // 空口令 = 开放网络：必须被接受并置位 has_pass，否则开放网络会被判成"凭证不全"。
    assert(cyber_prov_accept_pass(&c, "", 0));
    assert(c.has_pass);
    assert(c.pass_len == 0);

    // 超长拒收且不改动已有内容。
    char big[CYBER_PASS_MAX_BYTES + 2];
    memset(big, 'p', sizeof(big));
    assert(!cyber_prov_accept_pass(&c, big, CYBER_PASS_MAX_BYTES + 1));
    assert(c.pass_len == 0);

    // len>0 但指针为空是畸形输入，必须拒。
    assert(!cyber_prov_accept_pass(&c, NULL, 5));
    assert(c.pass_len == 0);
}

static void test_creds_valid(void)
{
    cyber_prov_creds_t c = { 0 };

    // 还没收到 SSID：不合法。
    assert(!cyber_prov_creds_valid(&c));
    assert(!cyber_prov_creds_valid(NULL));

    // 只有 SSID、没收到口令：按开放网络处理，合法。
    assert(cyber_prov_accept_ssid(&c, "OpenNet", 7));
    assert(cyber_prov_creds_valid(&c));

    // 收到空口令：仍是开放网络，合法。
    assert(cyber_prov_accept_pass(&c, "", 0));
    assert(cyber_prov_creds_valid(&c));

    // 口令长度落在 WPA2 不接受区间（1..7）时判非法。
    assert(cyber_prov_accept_pass(&c, "short", 5));
    assert(!cyber_prov_creds_valid(&c));

    // 8 字节合法。
    assert(cyber_prov_accept_pass(&c, "12345678", 8));
    assert(cyber_prov_creds_valid(&c));
}

int main(void)
{
    test_display_state_priority();
    test_display_state_full_truth_table();
    test_labels();
    test_awaits_phone();
    test_accept_ssid();
    test_accept_pass();
    test_creds_valid();

    printf("test_cyber_prov_util: PASS\n");
    return 0;
}
