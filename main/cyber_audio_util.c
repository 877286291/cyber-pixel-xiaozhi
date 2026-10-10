// main/cyber_audio_util.c —— 见 cyber_audio_util.h。
#include "cyber_audio_util.h"

#include <string.h>

uint32_t cyber_audio_samples_for(uint32_t rate, uint32_t frame_ms)
{
    if (rate == 0u || frame_ms == 0u) return 0u;
    // 采样率必须能被 1000 整除，否则"每毫秒的样本数"不是整数，每帧样本数会随
    // 帧漂移。调用方传进来的 rate 来自服务端声明，所以要挡住而不是假设。
    if (rate % 1000u != 0u) return 0u;
    return (rate / 1000u) * frame_ms;
}

bool cyber_audio_format_from_downlink(cyber_audio_format_t *out, uint32_t rate,
                                      uint32_t frame_ms)
{
    if (out == NULL) return false;
    if (frame_ms != CYBER_AUDIO_FRAME_MS) return false;
    if (rate != CYBER_AUDIO_DOWN_RATE_A && rate != CYBER_AUDIO_DOWN_RATE_B) return false;

    const uint32_t samples = cyber_audio_samples_for(rate, frame_ms);
    if (samples == 0u) return false;

    // 顺序有讲究：全部算完再写，失败时不留下半新半旧的格式。
    cyber_audio_format_t tmp;
    tmp.rate = rate;
    tmp.frame_ms = (uint16_t)frame_ms;
    tmp.samples = (uint16_t)samples;
    tmp.pcm_bytes = (uint16_t)(samples * 2u);
    if (tmp.pcm_bytes > CYBER_AUDIO_MAX_PCM_BYTES) return false;
    *out = tmp;
    return true;
}

void cyber_audio_queue_reset(cyber_audio_queue_t *q)
{
    if (q == NULL) return;
    memset(q, 0, sizeof(*q));
}

bool cyber_audio_queue_push(cyber_audio_queue_t *q, const void *data, size_t len)
{
    if (q == NULL || data == NULL) return false;
    if (len == 0u || len > CYBER_AUDIO_MAX_FRAME_BYTES) {
        q->dropped_oversize++;
        return false;
    }
    if (q->count >= CYBER_AUDIO_QUEUE_FRAMES) {
        q->dropped_full++;
        return false;
    }
    const uint8_t slot = (uint8_t)((q->head + q->count) % CYBER_AUDIO_QUEUE_FRAMES);
    memcpy(q->data[slot], data, len);
    q->len[slot] = (uint16_t)len;
    // 先写数据再放行：count 是给消费者看的"这格可以读了"的信号，必须最后更新。
    q->count = (uint8_t)(q->count + 1u);
    q->pushed++;
    return true;
}

const uint8_t *cyber_audio_queue_front(const cyber_audio_queue_t *q, size_t *len)
{
    if (len != NULL) *len = 0u;
    if (q == NULL || q->count == 0u) return NULL;
    if (len != NULL) *len = q->len[q->head];
    return q->data[q->head];
}

void cyber_audio_queue_pop(cyber_audio_queue_t *q)
{
    if (q == NULL || q->count == 0u) return;
    q->head = (uint8_t)((q->head + 1u) % CYBER_AUDIO_QUEUE_FRAMES);
    q->count = (uint8_t)(q->count - 1u);
}

void cyber_audio_queue_drain(cyber_audio_queue_t *q)
{
    if (q == NULL) return;
    q->head = 0u;
    q->count = 0u;
}

uint8_t cyber_audio_queue_count(const cyber_audio_queue_t *q)
{
    return (q == NULL) ? 0u : q->count;
}
