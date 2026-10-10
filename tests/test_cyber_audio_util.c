// tests/test_cyber_audio_util.c
// cyber_audio_util 的宿主单测：下行格式校验、帧算术、有界帧队列。
// 这些逻辑刻意不依赖 ESP-IDF / Opus，所以能在 PC 上直接跑。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_audio_util.h"

static void test_samples_for(void)
{
    // 两个受支持的档位，以及上界 24000。
    assert(cyber_audio_samples_for(16000u, 60u) == 960u);
    assert(cyber_audio_samples_for(24000u, 60u) == 1440u);
    assert(cyber_audio_samples_for(48000u, 60u) == 2880u);
    assert(cyber_audio_samples_for(8000u, 20u) == 160u);

    // 采样率不能被 1000 整除时，"每毫秒样本数"不是整数，必须拒绝而不是取整。
    assert(cyber_audio_samples_for(44100u, 60u) == 0u);
    assert(cyber_audio_samples_for(0u, 60u) == 0u);
    assert(cyber_audio_samples_for(16000u, 0u) == 0u);
}

static void test_format_from_downlink(void)
{
    cyber_audio_format_t f;

    // 实测值：服务端声明 24000 Hz / 60 ms。每帧 1440 样本、2880 字节。
    memset(&f, 0, sizeof(f));
    assert(cyber_audio_format_from_downlink(&f, 24000u, 60u));
    assert(f.rate == 24000u && f.frame_ms == 60u);
    assert(f.samples == 1440u && f.pcm_bytes == 2880u);

    memset(&f, 0, sizeof(f));
    assert(cyber_audio_format_from_downlink(&f, 16000u, 60u));
    assert(f.samples == 960u && f.pcm_bytes == 1920u);

    // 帧长不是 60 ms：本固件的每帧缓冲与发送节奏都是按 60 ms 定的，不能将就。
    assert(!cyber_audio_format_from_downlink(&f, 24000u, 20u));
    assert(!cyber_audio_format_from_downlink(&f, 24000u, 40u));
    assert(!cyber_audio_format_from_downlink(&f, 24000u, 0u));

    // 采样率不在支持档位。48000 是合法 Opus 采样率，但仍要拒绝：接受它就得把
    // 每帧 PCM 缓冲抬到 5760 字节，而那个缓冲和 libopus 调用共用同一个任务栈。
    assert(!cyber_audio_format_from_downlink(&f, 48000u, 60u));
    assert(!cyber_audio_format_from_downlink(&f, 8000u, 60u));
    assert(!cyber_audio_format_from_downlink(&f, 44100u, 60u));

    // 失败时不能改动 out：半新半旧的格式会让播放端按错误的采样率工作。
    cyber_audio_format_t keep;
    memset(&keep, 0, sizeof(keep));
    assert(cyber_audio_format_from_downlink(&keep, 24000u, 60u));
    const cyber_audio_format_t before = keep;
    assert(!cyber_audio_format_from_downlink(&keep, 48000u, 60u));
    assert(memcmp(&keep, &before, sizeof(keep)) == 0);

    assert(!cyber_audio_format_from_downlink(NULL, 24000u, 60u));
}

static void test_queue_fifo(void)
{
    cyber_audio_queue_t q;
    cyber_audio_queue_reset(&q);
    assert(cyber_audio_queue_count(&q) == 0u);

    size_t len = 123u;
    assert(cyber_audio_queue_front(&q, &len) == NULL);
    assert(len == 0u);

    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {9, 8};
    assert(cyber_audio_queue_push(&q, a, sizeof(a)));
    assert(cyber_audio_queue_push(&q, b, sizeof(b)));
    assert(cyber_audio_queue_count(&q) == 2u);
    assert(q.pushed == 2u);

    // front 不取出：连读两次必须看到同一帧。
    const uint8_t *p = cyber_audio_queue_front(&q, &len);
    assert(p != NULL && len == sizeof(a));
    assert(memcmp(p, a, sizeof(a)) == 0);
    p = cyber_audio_queue_front(&q, &len);
    assert(len == sizeof(a) && memcmp(p, a, sizeof(a)) == 0);

    cyber_audio_queue_pop(&q);
    p = cyber_audio_queue_front(&q, &len);
    assert(p != NULL && len == sizeof(b) && memcmp(p, b, sizeof(b)) == 0);

    cyber_audio_queue_pop(&q);
    assert(cyber_audio_queue_count(&q) == 0u);
    assert(cyber_audio_queue_front(&q, &len) == NULL);
    // 空队列上 pop 必须是空操作，不能把 head 推着走。
    cyber_audio_queue_pop(&q);
    assert(cyber_audio_queue_count(&q) == 0u);
}

static void test_queue_rejects(void)
{
    cyber_audio_queue_t q;
    cyber_audio_queue_reset(&q);

    // 零长度：没有可解的内容，收下只会让解码端拿到空包。
    assert(!cyber_audio_queue_push(&q, "x", 0u));
    assert(q.dropped_oversize == 1u && q.pushed == 0u);

    // 超长：必须整帧丢弃，而不是截断。截断的 Opus 解出来是杂音，比丢掉更难查。
    static uint8_t big[CYBER_AUDIO_MAX_FRAME_BYTES + 1u];
    memset(big, 0xAB, sizeof(big));
    assert(!cyber_audio_queue_push(&q, big, sizeof(big)));
    assert(q.dropped_oversize == 2u && q.pushed == 0u);
    assert(!cyber_audio_queue_push(&q, NULL, 4u));

    // 恰好到上界的一帧是合法的。
    assert(cyber_audio_queue_push(&q, big, CYBER_AUDIO_MAX_FRAME_BYTES));
    assert(q.pushed == 1u);
    cyber_audio_queue_reset(&q);

    // 队满：丢【新】帧，已排队的内容一帧不动。
    uint8_t frame[CYBER_AUDIO_MAX_FRAME_BYTES];
    for (unsigned i = 0u; i < CYBER_AUDIO_QUEUE_FRAMES; i++) {
        memset(frame, (int)i, sizeof(frame));
        assert(cyber_audio_queue_push(&q, frame, 8u));
    }
    assert(cyber_audio_queue_count(&q) == CYBER_AUDIO_QUEUE_FRAMES);
    memset(frame, 0xFF, sizeof(frame));
    assert(!cyber_audio_queue_push(&q, frame, 8u));
    assert(q.dropped_full == 1u);
    assert(cyber_audio_queue_count(&q) == CYBER_AUDIO_QUEUE_FRAMES);

    size_t len = 0u;
    const uint8_t *p = cyber_audio_queue_front(&q, &len);
    assert(p != NULL && len == 8u && p[0] == 0u);

    // drain 一次性清空，但不动累计统计。
    cyber_audio_queue_drain(&q);
    assert(cyber_audio_queue_count(&q) == 0u);
    assert(q.dropped_full == 1u && q.pushed == CYBER_AUDIO_QUEUE_FRAMES);
    assert(cyber_audio_queue_front(&q, &len) == NULL);
}

static void test_queue_wraps(void)
{
    cyber_audio_queue_t q;
    cyber_audio_queue_reset(&q);

    // 推满、读空，重复多轮：索引必须绕回并复用槽位，内容不能串。
    for (unsigned round = 0u; round < CYBER_AUDIO_QUEUE_FRAMES * 3u; round++) {
        uint8_t frame[4];
        const uint8_t value = (uint8_t)(round & 0xFFu);
        memset(frame, value, sizeof(frame));
        assert(cyber_audio_queue_push(&q, frame, sizeof(frame)));

        size_t len = 0u;
        const uint8_t *p = cyber_audio_queue_front(&q, &len);
        assert(p != NULL && len == sizeof(frame));
        assert(p[0] == value && p[3] == value);
        cyber_audio_queue_pop(&q);
        assert(cyber_audio_queue_count(&q) == 0u);
    }
    assert(q.pushed == CYBER_AUDIO_QUEUE_FRAMES * 3u);
    assert(q.dropped_full == 0u && q.dropped_oversize == 0u);
}

int main(void)
{
    test_samples_for();
    test_format_from_downlink();
    test_queue_fifo();
    test_queue_rejects();
    test_queue_wraps();
    puts("cyber_audio_util: PASS");
    return 0;
}
