// main/cyber_ws_util.h
// 小智对话会话的【纯逻辑】：控制消息构造 + 会话状态机 + 帧数学。
//
// 不依赖 ESP-IDF / LVGL / cJSON，可直接跑宿主单测（与 cyber_ota_util / cyber_net_util
// 同一定位）。传输（WebSocket + JSON 解析）在 cyber_ws.c，音频（Opus + I2S）在
// cyber_audio.c —— 两者都只做"必须依赖硬件与网络"的事。
//
// 为什么把状态机放在这里而不是 cyber_ws.c：它是整个对话体验的骨架（什么时候采集、
// 什么时候播放、掉线回到哪），而它又完全不需要网络就能推演。放在传输层里就只能靠
// 上机才能验证，掉线/打断这类分支尤其难复现。
//
// 规格见 docs/development/engineering/xiaozhi-conversation.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 协议常量
// ---------------------------------------------------------------------------
// 二进制协议版本。v1 = 裸 Opus 帧：WebSocket 的文本/二进制帧本身已经区分了类型，
// 不再需要 v2/v3 那层头部。本应用不需要服务端 AEC 的时间戳，因此不引入 v2。
#define CYBER_WS_PROTO_VERSION      1

// 音频参数。上行 16 kHz 单声道、60 ms 一帧，与 hello 里申报的一致。
// 60 ms 而非 20 ms：同码率下帧数少三倍，每帧的 Opus 头开销摊薄，
// 且给 LVGL 刷新留出更整的 CPU 时间片。
#define CYBER_WS_SAMPLE_RATE        16000u
#define CYBER_WS_CHANNELS           1u
#define CYBER_WS_FRAME_MS           60u
// 一帧的采样数 = 16000/1000 * 60 = 960。Opus 的合法帧长之一。
#define CYBER_WS_FRAME_SAMPLES      ((CYBER_WS_SAMPLE_RATE / 1000u) * CYBER_WS_FRAME_MS)

// 单帧 PCM 的字节数（16 bit 单声道）。
#define CYBER_WS_FRAME_PCM_BYTES    (CYBER_WS_FRAME_SAMPLES * 2u * CYBER_WS_CHANNELS)

// 控制消息 JSON 缓冲上限。hello 约 230 字节；留足余量，且刻意给小——
// 这些缓冲会随会话常驻，没有理由按"可能很长"来分配。
#define CYBER_WS_JSON_CAP           320
// 服务端下发的 session_id，回填到后续消息里。定长存储，不做动态分配。
#define CYBER_WS_SESSION_CAP        64

// ---------------------------------------------------------------------------
// 上行控制消息
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_WS_LISTEN_START = 0,  // 开始上行麦克风
    CYBER_WS_LISTEN_STOP,       // 结束上行
    CYBER_WS_LISTEN_DETECT,     // 本地唤醒词命中（本应用暂无唤醒词，保留给后续）
    CYBER_WS_LISTEN_COUNT,
} cyber_ws_listen_t;

// listen 的会话模式。手动：按下才听，松开就停；自动：一次开始后由服务端决定何时收尾。
typedef enum {
    CYBER_WS_MODE_MANUAL = 0,
    CYBER_WS_MODE_AUTO,
    CYBER_WS_MODE_REALTIME,
    CYBER_WS_MODE_COUNT,
} cyber_ws_mode_t;

// 构造握手消息。写入 out（含结尾 '\0'），返回写入字节数；cap 不足返回 0。
size_t cyber_ws_build_hello(char *out, size_t cap);

// 构造 listen 消息。session_id 可为 NULL/空（握手尚未完成时服务端不需要它）。
// session_id 含 JSON 需要转义的字符时返回 0 —— 它来自服务端，出现引号只可能是
// 协议异常，把这个 bug 静默转义会变成一条看起来正常却语义错误的请求。
size_t cyber_ws_build_listen(char *out, size_t cap, const char *session_id,
                             cyber_ws_listen_t st, cyber_ws_mode_t mode);

// 构造 abort 消息（打断当前回复）。reason 同上，非法时返回 0。
size_t cyber_ws_build_abort(char *out, size_t cap, const char *session_id,
                            const char *reason);

// ---------------------------------------------------------------------------
// 会话状态机
// ---------------------------------------------------------------------------
// 传输层把收到的消息解析成一个事件，喂给这里；本地动作（发起连接、按下说话键）
// 也走同一套事件，让所有状态迁移集中在一处、可被宿主单测逐条推演。
typedef enum {
    CYBER_WS_EV_NONE = 0,
    // --- 本地动作 ---
    CYBER_WS_EV_CONNECT,        // 请求建立连接
    CYBER_WS_EV_LISTEN_START,   // 开始上行麦克风
    CYBER_WS_EV_LISTEN_STOP,    // 结束上行
    CYBER_WS_EV_ABORT,          // 本地要求打断（用户按下打断键）
    // --- 远端消息 / 链路 ---
    CYBER_WS_EV_CONNECTED,      // WS 已建连，等 hello
    CYBER_WS_EV_HELLO,          // 服务端 hello：会话就绪
    CYBER_WS_EV_STT,            // 用户语音的识别结果
    CYBER_WS_EV_TTS_START,      // 服务端即将下发 TTS 音频
    CYBER_WS_EV_TTS_STOP,       // 本段 TTS 结束
    CYBER_WS_EV_SENTENCE,       // 一句话开始（用于字幕）
    CYBER_WS_EV_DISCONNECTED,   // 链路断开
    CYBER_WS_EV_FAIL,           // 建连或握手失败
    CYBER_WS_EV_COUNT,
} cyber_ws_event_t;

// 会话状态。
//
// 与 cyber_voice_state_t（界面用的语音态）刻意分开：这里描述"链路能做什么"，
// 那里描述"屏幕上该画什么"。两者由 main.c 映射，与 cyber_ota→cyber_pair 同一条规矩。
typedef enum {
    CYBER_WS_OFF = 0,     // 未启用（未绑定或未联网）：连接无从谈起
    CYBER_WS_IDLE,        // 已启用，未连接
    CYBER_WS_CONNECTING,  // 建连 + 等 hello
    CYBER_WS_READY,       // 握手完成，可以开始下一轮
    CYBER_WS_LISTENING,   // 正在上行麦克风
    CYBER_WS_SPEAKING,    // 正在播放服务端回复
    CYBER_WS_ERROR,       // 协商失败，等待下次重试
    CYBER_WS_STATE_COUNT,
} cyber_ws_state_t;

// 会话上下文。状态之外只保留"回填进后续消息"和"决定开关设备"所需的字段。
typedef struct {
    cyber_ws_state_t state;
    char session_id[CYBER_WS_SESSION_CAP];
    bool enabled;   // 由 main.c 按"已绑定 + 已联网"设置
} cyber_ws_session_t;

// 初始化：默认未启用（未绑定/未联网时不该尝试建连）。
void cyber_ws_session_init(cyber_ws_session_t *s);

// 设置是否允许连接。禁用会立即回到 OFF 并清空 session_id——
// 留着过期会话 id 会让重连后的第一条消息带上服务端不认的 id。
void cyber_ws_session_set_enabled(cyber_ws_session_t *s, bool enabled);

// 推进状态机。session_id 非空时会记录（仅 HELLO 事件携带）。返回新状态。
cyber_ws_state_t cyber_ws_session_step(cyber_ws_session_t *s, cyber_ws_event_t ev,
                                       const char *session_id);

// 该不该打开麦克风上行。只有 LISTENING 为真。
bool cyber_ws_wants_mic(const cyber_ws_session_t *s);

// 该不该把收到的音频送去播放。只有 SPEAKING 为真。
// 非 SPEEKING 时收到的二进制帧必须丢弃：那多半是上一轮的尾包或服务端多推，
// 照着播会和正在收音的麦克风抢同一个 I2S 通道。
bool cyber_ws_wants_playback(const cyber_ws_session_t *s);

// 会话状态的短标签与强调色（供日志与界面消费）。
const char *cyber_ws_state_label(cyber_ws_state_t st);

// ---------------------------------------------------------------------------
// 帧数学
// ---------------------------------------------------------------------------
// 一帧 PCM 覆盖的时长（毫秒）。上行节拍按它走。
uint32_t cyber_ws_frame_ms(void);

// 由采样数换算字节数（16 bit 单声道）。用于校验 Opus 解出的 PCM 是否整帧。
size_t cyber_ws_pcm_bytes(size_t samples);
