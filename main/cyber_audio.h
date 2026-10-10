// main/cyber_audio.h
// 音频通路：I2S 采集/播放 + Opus 编解码，跑在独立的 worker 任务里。
//
// 分工与线程约束：
//   - cyber_audio_util —— 下行格式校验、帧算术、有界帧队列（纯逻辑，可宿主单测）；
//   - 本模块            —— 只有"必须碰硬件"的事：codec/I2S、libopus、任务调度。
//
// 对外函数除 init 外都非阻塞，可被 LVGL 任务调用：它们只写一个标志或往队列里拷
// 一帧，真正的 I2S 读写与编解码全在 worker 里。音频 I/O 会阻塞（一帧 60 ms），
// 放在 LVGL 任务里界面立刻掉帧。
//
// 半双工：采集与播放不会同时进行。这既是产品选择（按键打断，而不是语音打断），
// 也是必要的取舍——没有 AEC 时边放边采只会把自己的声音当成用户在说话。
// 由它换来的好处是采集与播放可以各自用最合适的采样率：上行 16000 Hz（协议申报
// 值），下行按服务端声明（实测 24000 Hz）。两者不同，因此【不能在同一个 codec
// 配置下同时工作】，这也正是半双工在这里是硬约束而不只是偏好的原因。
//
// 规格见 docs/development/engineering/xiaozhi-conversation.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyber_audio_util.h"
#include "esp_err.h"

// 音频阶段。由会话状态驱动（映射见 main.c）：
//   IDLE      不采不放，排队中的播放帧会被丢弃
//   CAPTURE   按上行格式采集、编码、交给上行接收点
//   PLAYBACK  按服务端声明的下行格式解码并播放
typedef enum {
    CYBER_AUDIO_IDLE = 0,
    CYBER_AUDIO_CAPTURE,
    CYBER_AUDIO_PLAYBACK,
} cyber_audio_phase_t;

// 上行帧的接收点（编码完成的 Opus 帧交给传输层）。返回 false 表示这一帧没被接收
// （通常是上行队列满），本模块据此计数。
//
// 契约：实现可以短暂阻塞（传输层的上行队列只有几帧深）。它只会被音频 worker 调用，
// 所以绝不能把它接到 LVGL 任务上。
typedef bool (*cyber_audio_tx_fn)(const void *opus, size_t len);

// 初始化 codec/I2S、创建帧队列与 worker。幂等。
// 失败时返回具体错误，调用方应记录并放弃音频（文本通路不依赖它，仍可工作）。
esp_err_t cyber_audio_init(void);

// 除 init 外的所有接口都可以在【init 之前或 init 失败之后】被安全调用：它们会直接
// 返回（get_stats 返回全 0）。这不是可选的宽容度——tick_cb 从建屏那一刻就开始跳，
// 一定早于 init，而 xSemaphoreTake(NULL) 在这套 IDF 里会直接 assert 崩溃。
// 因此调用方不需要、也不应该用"是否已初始化"来决定要不要调它们。

// 注册上行帧接收点。传 NULL 取消注册。
void cyber_audio_set_tx_sink(cyber_audio_tx_fn fn);

// 切换阶段。非阻塞：只记下目标，格式切换与 I/O 开关由 worker 串行完成。
// BSP 要求格式切换串行化、且切换前必须停止所有 PCM 读写——把这件事交给唯一的
// I/O 执行者（worker）是唯一不会踩到它的做法。
void cyber_audio_set_phase(cyber_audio_phase_t phase);

// 设置服务端声明的下行格式。凭 ws 快照的 downlink 字段调用；传 NULL 表示回到
// 默认（与上行同档）。任意阶段调用都安全：worker 会在进入 PLAYBACK 之前按它
// 重新配置 codec 与解码器。
void cyber_audio_set_downlink(const cyber_audio_format_t *fmt);

// 投入一个入站 Opus 帧。由 cyber_ws 的音频接收点调用（事件循环任务）——
// 实现必须是流水作业：只做一次定长拷贝，队满时丢【新】帧并计数。
void cyber_audio_push_opus(const void *data, size_t len);

// 丢弃已排队的播放帧。打断、tts 结束、会话结束时调用。
void cyber_audio_flush(void);

// 诊断计数。用于日志与"声音不对"时的定位。
typedef struct {
    uint32_t tx_frames;           // 交给上行接收点并成功的帧数
    uint32_t tx_dropped;          // 上行接收点没收下的帧数（队列满）
    uint32_t encode_errors;       // 编码失败次数
    uint32_t alloc_failures;      // 编解码器【分配】失败次数（内存不足，与上面的算错分开）
    uint32_t rx_frames;           // 成功解码并写入 I2S 的帧数
    uint32_t decode_errors;       // 解码失败次数
    uint32_t write_errors;        // I2S 播放写入失败次数
    uint32_t read_errors;         // I2S 采集读取失败次数
    uint32_t queue_dropped_full;  // 抖动缓冲因队满丢弃的帧数
    uint32_t queue_dropped_oversize;
    uint32_t stack_high_water;    // worker 栈剩余最小值（字节），用于定容
} cyber_audio_stats_t;

void cyber_audio_get_stats(cyber_audio_stats_t *out);
