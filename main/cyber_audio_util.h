// main/cyber_audio_util.h
// 音频通路的【纯逻辑】：下行格式校验、帧算术、有界帧队列。
//
// 不依赖 ESP-IDF / LVGL / Opus，可直接跑宿主单测。真正碰硬件的部分（I2S、编解码
// 器、worker 任务）在 cyber_audio.c。
//
// 上行格式不在这里定义：它是【协议】的一部分（hello 里申报的就是它），所以那组
// 常量留在 cyber_ws_util.h。这里只补上下行与队列——下行采样率不是我们能选的，
// 只能按服务端 hello 的声明运行，见
// docs/development/engineering/xiaozhi-conversation.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 只为拿上行格式常量与单帧 Opus 上限。这两个值由【协议】定义，不是设备侧的选择，
// 所以它们住在 cyber_ws_util.h 而不是这里；在这里重复定义只会让两边漂移。
#include "cyber_ws_util.h"

// ---------------------------------------------------------------------------
// 下行格式
// ---------------------------------------------------------------------------
// 只支持协议实际会用到的两档，而不是"libopus 支持的都放进来"。原因很实际：
// 每帧 PCM 缓冲要在任务栈上开，允许 48000 Hz 就得开 5760 字节，而这个栈同时
// 还要跑 libopus 的编解码调用。收窄到两档，缓冲上界锁死在 2880 字节。
//
// 服务端声明了别的值时的正确处理是【让这次握手失败】，而不是"将就着播"：
// 采样率错了，症状只会是变调或静音，那时候再回头查协议代价极高。
#define CYBER_AUDIO_DOWN_RATE_A 16000u  // 服务端与设备同档时的可能取值
#define CYBER_AUDIO_DOWN_RATE_B 24000u  // 实测 api.tenclass.net 用这一档

// 唯一的合法帧长。服务端 hello 声明的 frame_duration 必须是它。
#define CYBER_AUDIO_FRAME_MS    60u
// 两档采样率下每帧 PCM 的上界：24000/1000*60*2 = 2880 字节。
#define CYBER_AUDIO_MAX_PCM_BYTES ((CYBER_AUDIO_DOWN_RATE_B / 1000u) * CYBER_AUDIO_FRAME_MS * 2u)

// 单帧 Opus 上限。与上行共用同一条红线，所以常量定义在 cyber_ws_util.h。
#define CYBER_AUDIO_MAX_FRAME_BYTES CYBER_WS_MAX_OPUS_FRAME

// 下行抖动缓冲的帧数：12 帧 = 720 ms。
//
// 服务端生成 TTS 快于实时播放，所以这个缓冲注定会被填满。满时的策略是丢【新】
// 帧：丢旧帧会把已经播出去的内容接续断掉，丢新帧只是截尾，行为可预测。丢弃
// 次数必须计数并上报，否则"说了半句就没声了"会变成一个查不出来的现象。
#define CYBER_AUDIO_QUEUE_FRAMES    12u

// 一次会话里下行格式的快照。
typedef struct {
    uint32_t rate;        // 采样率（Hz）
    uint16_t frame_ms;    // 帧长（ms）
    uint16_t samples;     // 每帧采样数 = rate / 1000 * frame_ms
    uint16_t pcm_bytes;   // 每帧 PCM 字节数 = samples * 2（16 bit 单声道）
} cyber_audio_format_t;

// 校验并写入一个下行格式。rate 不在支持档位、或 frame_ms 不是 60 时返回 false，
// 此时 out 保持原值不变。
bool cyber_audio_format_from_downlink(cyber_audio_format_t *out, uint32_t rate,
                                      uint32_t frame_ms);

// 按采样率与帧长算每帧采样数。参数非法（rate 不是 1000 的整数倍、frame_ms 不整除
// rate/1000）时返回 0。
uint32_t cyber_audio_samples_for(uint32_t rate, uint32_t frame_ms);

// ---------------------------------------------------------------------------
// 有界帧队列（SPSC）
// ---------------------------------------------------------------------------
// 生产者是 WebSocket 事件回调（默认事件循环任务），消费者是音频 worker。两者都
// 约定只碰自己那一端：
//   - push 只写 (head + count) % N 这一格，且队满时【不覆盖】未读数据；
//   - front/pop 只读 head 这一格。
// 因此读者拿到的指针在下次 pop 之前始终有效，无需加锁。索引是 8 位，单核上
// 读写不会撕裂。
//
// 注意：这个"无需加锁"的前提是【恰好一个生产者 + 恰好一个消费者】。再多一个
// 生产者就必须换成临界区。
typedef struct {
    uint8_t  data[CYBER_AUDIO_QUEUE_FRAMES][CYBER_AUDIO_MAX_FRAME_BYTES];
    uint16_t len[CYBER_AUDIO_QUEUE_FRAMES];
    volatile uint8_t head;    // 下一个要读的槽位
    volatile uint8_t count;   // 已排队的帧数
    uint32_t pushed;          // 累计入队成功的帧数
    uint32_t dropped_full;    // 因队满丢弃的帧数
    uint32_t dropped_oversize;// 因超过 MAX_FRAME_BYTES 丢弃的帧数
} cyber_audio_queue_t;

void cyber_audio_queue_reset(cyber_audio_queue_t *q);

// 把一帧拷进队列。len 为 0 或超过 MAX_FRAME_BYTES 时拒收并计入 dropped_oversize；
// 队满时拒收并计入 dropped_full。返回 true 表示这一帧已经进队列。
bool cyber_audio_queue_push(cyber_audio_queue_t *q, const void *data, size_t len);

// 看队首那一帧，不取出。空队列返回 NULL 并把 *len 置 0。
const uint8_t *cyber_audio_queue_front(const cyber_audio_queue_t *q, size_t *len);

// 取走队首那一帧。空队列时是空操作。
void cyber_audio_queue_pop(cyber_audio_queue_t *q);

// 丢弃全部已排队帧（打断、会话结束时用）。不清统计值。
void cyber_audio_queue_drain(cyber_audio_queue_t *q);

uint8_t cyber_audio_queue_count(const cyber_audio_queue_t *q);
