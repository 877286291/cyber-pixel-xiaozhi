// main/cyber_ws.h
// 小智对话通道的【传输层】：WebSocket 建连、鉴权头、JSON 收发、事件分发。
//
// 分工：
//   cyber_ws_util —— 纯逻辑（消息怎么拼、状态怎么走），可宿主单测；
//   cyber_ws      —— 只有"必须联网才能做"的事：握手、收发、把收到的帧解析成事件。
//
// 线程与调用约束（与 cyber_ota 同一套规矩）：
//   - 所有对外函数都可由 LVGL 任务调用，且都不阻塞：它们只往队列里投一条请求，
//     真正的建连/发送由本模块自己的 worker 完成。LVGL 任务里做 DNS/TLS 会让界面卡住。
//   - 快照在临界区内整体拷贝，读到的是一致的一组字段，不会看到"半次"状态迁移。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyber_audio_util.h"
#include "cyber_ws_util.h"
#include "esp_err.h"

// 字幕缓冲上限：STT 识别结果与 TTS 当前句共用。够放一两句中文，
// 且刻意定长——它是随会话常驻的字段，不做动态分配。
#define CYBER_WS_SUBTITLE_CAP 128

// 会话快照。由 worker 发布，供 LVGL 任务读取。
typedef struct {
    cyber_ws_state_t state;                    // 链路现在能做什么
    char session_id[CYBER_WS_SESSION_CAP];     // 服务端下发的会话 id
    char subtitle[CYBER_WS_SUBTITLE_CAP];      // 最近一句文本（STT 或 TTS sentence）
    int last_error;                            // 最近一次失败原因（esp_err_t 或 -1）
    uint32_t handshakes;                       // 累计成功握手次数
    // 服务端声明的下行音频格式，握手完成时写入。播放端必须按它配置 codec：
    // 实测服务端用的是 24000 Hz，而上行是 16000 Hz，两个方向并不相同。
    cyber_audio_format_t downlink;
} cyber_ws_snapshot_t;

// 入站音频帧的接收点。由 cyber_ws 的【事件回调任务】调用，参数指向内部组帧缓冲，
// 只在本次调用内有效，实现必须在调用内把数据拷走。
// 契约：必须非阻塞。事件循环任务同时还在跑 Wi-Fi 事件，堵住它会连锁影响整机。
typedef void (*cyber_ws_audio_sink_t)(const void *data, size_t len);

// 创建 worker 与请求队列。幂等；应在 app_main 中调用一次。
// 注意：此时不建连——未绑定或未联网时连接只会必然失败。
esp_err_t cyber_ws_init(void);

// 标记"现在允许连接"（已绑定且已联网时为 true）。非阻塞。
// 置为 false 会立即回到 OFF 并清空会话 id。
void cyber_ws_set_enabled(bool enabled);

// 按下/松开说话键。start=true 会按需建连，并在握手完成后开始上行。非阻塞。
void cyber_ws_request_listen(bool start);

// 打断当前回复。非阻塞。
void cyber_ws_request_abort(void);

// 注册入站音频帧的接收点。传 NULL 取消注册。应在 cyber_ws_init() 之后、允许建连
// 之前调用；接收点只会被事件回调调用，见 cyber_ws_audio_sink_t 的契约。
void cyber_ws_set_audio_sink(cyber_ws_audio_sink_t sink);

// 发送一个 Opus 帧（二进制）。返回 false 表示这一帧没发出去（未握手、超长或上行
// 队列满），调用方据此计数。
//
// 线程约束：【不要从 LVGL 任务调用】。它最多会阻塞 CYBER_WS_AUDIO_SEND_TICKS，
// 因为上行队列只有几帧深，而 LVGL 任务一旦阻塞界面就停帧。音频 worker 是唯一的
// 预期调用方，它本来就在按 60 ms 的节奏产出帧。
bool cyber_ws_send_audio(const void *opus, size_t len);

// 读取快照。任意任务可调用，临界区内完成，不阻塞。
void cyber_ws_get_snapshot(cyber_ws_snapshot_t *out);

// 最近一次实测的堆余量（字节），供日志与界面诊断使用。
// 返回 0 表示尚未采样。
uint32_t cyber_ws_last_free_heap(void);
