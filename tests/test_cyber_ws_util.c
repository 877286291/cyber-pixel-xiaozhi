// tests/test_cyber_ws_util.c
// cyber_ws_util 的宿主单测：控制消息构造、会话状态机、帧数学。
// 这些逻辑刻意不依赖 ESP-IDF / LVGL / cJSON，所以能在 PC 上直接跑。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_ws_util.h"

static void test_build_hello(void)
{
    char buf[CYBER_WS_JSON_CAP];
    const size_t n = cyber_ws_build_hello(buf, sizeof(buf));

    // 逐字节钉住：字段名与顺序是服务端契约的一部分，漂移必须在这里被发现。
    assert(n > 0);
    assert(n == strlen(buf));
    assert(strcmp(buf,
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}") == 0);

    // 缓冲不足必须失败，绝不能返回一条被截断的 JSON。
    char small[8];
    assert(cyber_ws_build_hello(small, sizeof(small)) == 0);
    assert(cyber_ws_build_hello(NULL, sizeof(buf)) == 0);
    assert(cyber_ws_build_hello(buf, 0) == 0);
}

static void test_build_listen(void)
{
    char buf[CYBER_WS_JSON_CAP];

    assert(cyber_ws_build_listen(buf, sizeof(buf), "abc", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_MANUAL) == strlen(buf));
    assert(strcmp(buf,
        "{\"session_id\":\"abc\",\"type\":\"listen\",\"state\":\"start\","
        "\"mode\":\"manual\"}") == 0);

    // 握手尚未完成时没有 session_id：整段省略，而不是发一个空串。
    assert(cyber_ws_build_listen(buf, sizeof(buf), NULL, CYBER_WS_LISTEN_STOP,
                                 CYBER_WS_MODE_AUTO) == strlen(buf));
    assert(strcmp(buf, "{\"type\":\"listen\",\"state\":\"stop\",\"mode\":\"auto\"}") == 0);
    assert(cyber_ws_build_listen(buf, sizeof(buf), "", CYBER_WS_LISTEN_DETECT,
                                 CYBER_WS_MODE_REALTIME) == strlen(buf));
    assert(strcmp(buf, "{\"type\":\"listen\",\"state\":\"detect\",\"mode\":\"realtime\"}") == 0);

    // session_id 含 JSON 需要转义的字符：拒绝而不是转义（见实现注释）。
    assert(cyber_ws_build_listen(buf, sizeof(buf), "a\"b", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_AUTO) == 0);
    assert(cyber_ws_build_listen(buf, sizeof(buf), "a\\b", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_AUTO) == 0);
    assert(cyber_ws_build_listen(buf, sizeof(buf), "a\nb", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_AUTO) == 0);

    // 越界枚举一律拒绝，不能拼出一个服务端看不懂的状态名。
    assert(cyber_ws_build_listen(buf, sizeof(buf), "abc", (cyber_ws_listen_t)99,
                                 CYBER_WS_MODE_AUTO) == 0);
    assert(cyber_ws_build_listen(buf, sizeof(buf), "abc", CYBER_WS_LISTEN_START,
                                 (cyber_ws_mode_t)99) == 0);
    assert(cyber_ws_build_listen(NULL, sizeof(buf), "abc", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_AUTO) == 0);

    char small[8];
    assert(cyber_ws_build_listen(small, sizeof(small), "abc", CYBER_WS_LISTEN_START,
                                 CYBER_WS_MODE_AUTO) == 0);
}

static void test_build_abort(void)
{
    char buf[CYBER_WS_JSON_CAP];

    assert(cyber_ws_build_abort(buf, sizeof(buf), "abc", "wake_word_detected") == strlen(buf));
    assert(strcmp(buf,
        "{\"session_id\":\"abc\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}") == 0);

    assert(cyber_ws_build_abort(buf, sizeof(buf), NULL, "wake_word_detected") == strlen(buf));
    assert(strcmp(buf, "{\"type\":\"abort\",\"reason\":\"wake_word_detected\"}") == 0);

    // reason 是必填且必须可安全嵌入：NULL 或含引号一律拒绝。
    assert(cyber_ws_build_abort(buf, sizeof(buf), "abc", NULL) == 0);
    assert(cyber_ws_build_abort(buf, sizeof(buf), "abc", "bad\"reason") == 0);
    assert(cyber_ws_build_abort(NULL, sizeof(buf), "abc", "x") == 0);
}

static void test_session_defaults_and_enable(void)
{
    cyber_ws_session_t s;
    cyber_ws_session_init(&s);

    // 默认未启用：未绑定/未联网时不该尝试建连。
    assert(s.state == CYBER_WS_OFF);
    assert(!s.enabled);
    assert(s.session_id[0] == '\0');
    assert(!cyber_ws_wants_mic(&s));
    assert(!cyber_ws_wants_playback(&s));

    // 未启用时任何事件都不能把状态推出 OFF，否则界面会显示"可连接"。
    for (int e = 0; e < CYBER_WS_EV_COUNT; e++) {
        assert(cyber_ws_session_step(&s, (cyber_ws_event_t)e, "x") == CYBER_WS_OFF);
    }
    assert(s.session_id[0] == '\0');

    cyber_ws_session_set_enabled(&s, true);
    assert(s.state == CYBER_WS_IDLE);

    cyber_ws_session_set_enabled(&s, false);
    assert(s.state == CYBER_WS_OFF);

    // 空指针与重复启用的边界。
    cyber_ws_session_init(NULL);
    cyber_ws_session_set_enabled(NULL, true);
    assert(cyber_ws_session_step(NULL, CYBER_WS_EV_CONNECT, NULL) == CYBER_WS_OFF);
    assert(!cyber_ws_wants_mic(NULL));
    assert(!cyber_ws_wants_playback(NULL));
}

static void test_session_happy_path(void)
{
    cyber_ws_session_t s;
    cyber_ws_session_init(&s);
    cyber_ws_session_set_enabled(&s, true);

    assert(cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL) == CYBER_WS_CONNECTING);
    // 建连成功但还没握手：仍停在 CONNECTING，不能提前当成就绪。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_CONNECTED, NULL) == CYBER_WS_CONNECTING);
    assert(s.session_id[0] == '\0');

    // 服务端 hello：转 READY 并记下 session_id。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "sess-1") == CYBER_WS_READY);
    assert(strcmp(s.session_id, "sess-1") == 0);

    // 开始收音：只有这一态才该打开麦克风。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_LISTEN_START, NULL) == CYBER_WS_LISTENING);
    assert(cyber_ws_wants_mic(&s));
    assert(!cyber_ws_wants_playback(&s));

    // 服务端开始说话：停止收音并转到播放。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_TTS_START, NULL) == CYBER_WS_SPEAKING);
    assert(!cyber_ws_wants_mic(&s));
    assert(cyber_ws_wants_playback(&s));
    // SPEAKING 期间 session_id 必须保留：后续 abort 要带上它。
    assert(strcmp(s.session_id, "sess-1") == 0);

    // 本段播完回到 READY（手动模式不自动接着听）。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_TTS_STOP, NULL) == CYBER_WS_READY);
    assert(!cyber_ws_wants_playback(&s));
    assert(strcmp(s.session_id, "sess-1") == 0);
}

static void test_session_barge_in_and_abort(void)
{
    cyber_ws_session_t s;
    cyber_ws_session_init(&s);
    cyber_ws_session_set_enabled(&s, true);
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "sess-2");

    // 播放中按下说话键：直接从 SPEAKING 进入 LISTENING（打断后立刻说）。
    cyber_ws_session_step(&s, CYBER_WS_EV_TTS_START, NULL);
    assert(s.state == CYBER_WS_SPEAKING);
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_LISTEN_START, NULL) == CYBER_WS_LISTENING);
    assert(cyber_ws_wants_mic(&s));
    assert(!cyber_ws_wants_playback(&s));

    // 收音中打断 → 回 READY，且仍在同一会话里。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_ABORT, NULL) == CYBER_WS_READY);
    assert(strcmp(s.session_id, "sess-2") == 0);
}

static void test_session_disconnect_and_error(void)
{
    cyber_ws_session_t s;
    cyber_ws_session_init(&s);
    cyber_ws_session_set_enabled(&s, true);

    // 建连失败 → ERROR；此时不该还留着会话 id。
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_FAIL, NULL) == CYBER_WS_ERROR);
    assert(s.session_id[0] == '\0');
    // ERROR 里可以重新发起连接。
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL) == CYBER_WS_CONNECTING);

    // 掉线 → IDLE，并清空 session_id：下一轮不该带上一轮的 id。
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "stale");
    assert(strcmp(s.session_id, "stale") == 0);
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_DISCONNECTED, NULL) == CYBER_WS_IDLE);
    assert(s.session_id[0] == '\0');

    // 播放中掉线也要回到 IDLE，而不是留在 SPEAKING 里"假装在放"。
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "s2");
    cyber_ws_session_step(&s, CYBER_WS_EV_LISTEN_START, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_TTS_START, NULL);
    assert(cyber_ws_session_step(&s, CYBER_WS_EV_DISCONNECTED, NULL) == CYBER_WS_IDLE);

    // 禁用会把会话彻底关掉并清 id。
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "s3");
    cyber_ws_session_set_enabled(&s, false);
    assert(s.state == CYBER_WS_OFF);
    assert(s.session_id[0] == '\0');
}

static void test_session_rejects_bad_id(void)
{
    cyber_ws_session_t s;
    cyber_ws_session_init(&s);
    cyber_ws_session_set_enabled(&s, true);
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);

    // 非法 id 不写进上下文：宁可没有 id，也不留半个可能被回填进请求的值。
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, "bad\"id");
    assert(s.session_id[0] == '\0');
    assert(s.state == CYBER_WS_READY);

    // 空/缺失 id 同样合法（服务端不强制下发）。
    cyber_ws_session_step(&s, CYBER_WS_EV_DISCONNECTED, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_CONNECT, NULL);
    cyber_ws_session_step(&s, CYBER_WS_EV_HELLO, NULL);
    assert(s.session_id[0] == '\0');
}

static void test_state_labels_and_math(void)
{
    assert(strcmp(cyber_ws_state_label(CYBER_WS_OFF), "OFF") == 0);
    assert(strcmp(cyber_ws_state_label(CYBER_WS_LISTENING), "LISTEN") == 0);
    assert(strcmp(cyber_ws_state_label(CYBER_WS_SPEAKING), "SPEAK") == 0);
    assert(strcmp(cyber_ws_state_label(CYBER_WS_ERROR), "ERROR") == 0);
    assert(strcmp(cyber_ws_state_label((cyber_ws_state_t)99), "?") == 0);

    // 帧长必须自洽：960 采样、60 ms、16 kHz、16 bit 单声道 = 1920 字节。
    assert(CYBER_WS_FRAME_SAMPLES == 960);
    assert(CYBER_WS_FRAME_PCM_BYTES == 1920);
    assert(cyber_ws_frame_ms() == 60);
    assert(cyber_ws_pcm_bytes(CYBER_WS_FRAME_SAMPLES) == CYBER_WS_FRAME_PCM_BYTES);
    assert(cyber_ws_pcm_bytes(0) == 0);
}

int main(void)
{
    test_build_hello();
    test_build_listen();
    test_build_abort();
    test_session_defaults_and_enable();
    test_session_happy_path();
    test_session_barge_in_and_abort();
    test_session_disconnect_and_error();
    test_session_rejects_bad_id();
    test_state_labels_and_math();

    printf("test_cyber_ws_util: PASS\n");
    return 0;
}
