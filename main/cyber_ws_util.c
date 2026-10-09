// main/cyber_ws_util.c
// 见 cyber_ws_util.h 的定位说明。本文件只有纯计算：无 malloc、无 IO、无全局状态。
#include "cyber_ws_util.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// JSON 构造
// ---------------------------------------------------------------------------
// 只在需要转义时拒绝，不做静默转义：这些值要么是固件常量，要么是服务端下发的
// session_id。后者出现引号/反斜杠只可能是协议异常，把它转义进请求里只会让一个
// bug 变成一条"看起来正常、语义却不对"的消息（与 cyber_ota_build_body 同一取舍）。
static bool token_ok(const char *s)
{
    if (s == NULL) return false;
    for (; *s != '\0'; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\' || c < 0x20u) return false;
    }
    return true;
}

// 空串与 NULL 都视为"暂无 session_id"，是合法输入（握手尚未完成时确实没有）。
static bool session_ok(const char *s)
{
    return (s == NULL) || (*s == '\0') || token_ok(s);
}

// snprintf 的收尾检查集中在这里：n < 0（编码错误）与截断都必须当成失败，
// 否则调用方会把一条被砍掉尾巴的 JSON 发出去，服务端只会报语法错误。
static size_t finish(int n, size_t cap)
{
    if (n < 0 || (size_t)n >= cap) return 0u;
    return (size_t)n;
}

size_t cyber_ws_build_hello(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) return 0u;
    const int n = snprintf(out, cap,
        "{\"type\":\"hello\",\"version\":%u,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%u,"
        "\"channels\":%u,\"frame_duration\":%u}}",
        (unsigned)CYBER_WS_PROTO_VERSION, (unsigned)CYBER_WS_SAMPLE_RATE,
        (unsigned)CYBER_WS_CHANNELS, (unsigned)CYBER_WS_FRAME_MS);
    return finish(n, cap);
}

static const char *listen_state_name(cyber_ws_listen_t st)
{
    switch (st) {
    case CYBER_WS_LISTEN_START:  return "start";
    case CYBER_WS_LISTEN_STOP:   return "stop";
    case CYBER_WS_LISTEN_DETECT: return "detect";
    default:                     return NULL;
    }
}

static const char *mode_name(cyber_ws_mode_t mode)
{
    switch (mode) {
    case CYBER_WS_MODE_MANUAL:   return "manual";
    case CYBER_WS_MODE_AUTO:     return "auto";
    case CYBER_WS_MODE_REALTIME: return "realtime";
    default:                     return NULL;
    }
}

size_t cyber_ws_build_listen(char *out, size_t cap, const char *session_id,
                             cyber_ws_listen_t st, cyber_ws_mode_t mode)
{
    if (out == NULL || cap == 0u) return 0u;
    const char *state = listen_state_name(st);
    const char *mode_s = mode_name(mode);
    if (state == NULL || mode_s == NULL) return 0u;
    if (!session_ok(session_id)) return 0u;

    // 没有 session_id 时整段省略，而不是发一个空串：服务端把空串当有效 id
    // 与"没有 id"是两件事，前者会让它去找一个不存在的会话。
    const int n = (session_id == NULL || *session_id == '\0')
        ? snprintf(out, cap,
                   "{\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"%s\"}",
                   state, mode_s)
        : snprintf(out, cap,
                   "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\","
                   "\"mode\":\"%s\"}",
                   session_id, state, mode_s);
    return finish(n, cap);
}

size_t cyber_ws_build_abort(char *out, size_t cap, const char *session_id,
                            const char *reason)
{
    if (out == NULL || cap == 0u) return 0u;
    if (!session_ok(session_id) || !token_ok(reason)) return 0u;

    const int n = (session_id == NULL || *session_id == '\0')
        ? snprintf(out, cap, "{\"type\":\"abort\",\"reason\":\"%s\"}", reason)
        : snprintf(out, cap,
                   "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"%s\"}",
                   session_id, reason);
    return finish(n, cap);
}

// ---------------------------------------------------------------------------
// 会话状态机
// ---------------------------------------------------------------------------
void cyber_ws_session_init(cyber_ws_session_t *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    // 默认未启用：未绑定或未联网时不该尝试建连，否则只会得到一串必然失败的握手。
    s->state = CYBER_WS_OFF;
    s->enabled = false;
}

void cyber_ws_session_set_enabled(cyber_ws_session_t *s, bool enabled)
{
    if (s == NULL) return;
    if (s->enabled == enabled) return;
    s->enabled = enabled;
    // 无论开关方向都清 session_id：禁用后它是过期数据，重新启用后它是上一个
    // 会话的残留。两种情况都会让重连后的第一条消息带上服务端不认的 id。
    s->session_id[0] = '\0';
    s->state = enabled ? CYBER_WS_IDLE : CYBER_WS_OFF;
}

// 记录服务端下发的 session_id。空/非法一律清空，绝不把半个 id 留在上下文里。
static void set_session_id(cyber_ws_session_t *s, const char *session_id)
{
    if (session_id != NULL && *session_id != '\0' && token_ok(session_id)) {
        snprintf(s->session_id, sizeof(s->session_id), "%s", session_id);
    } else {
        s->session_id[0] = '\0';
    }
}

cyber_ws_state_t cyber_ws_session_step(cyber_ws_session_t *s, cyber_ws_event_t ev,
                                       const char *session_id)
{
    if (s == NULL) return CYBER_WS_OFF;
    // 未启用时任何事件都不得把状态推出 OFF：OFF 是"这个功能现在不该存在"，
    // 让一次迟到的 DISCONNECTED 之类的事件把它改成 IDLE，界面就会显示"可连接"。
    if (!s->enabled) { s->state = CYBER_WS_OFF; return s->state; }

    // 被禁用后迟到的 HELLO 不该被记录（此时 session_id 已清），
    // 因此只在会话真正推进时才收取 id。
    switch (s->state) {
    case CYBER_WS_IDLE:
        if (ev == CYBER_WS_EV_CONNECT) s->state = CYBER_WS_CONNECTING;
        break;

    case CYBER_WS_CONNECTING:
        switch (ev) {
        // CONNECTED 只表示 TCP/TLS/WS 建好了，会话还没就绪——还要等 hello。
        // 在这里就转 READY 会让第一条 listen 发在一个服务端还没准备好的会话上。
        case CYBER_WS_EV_CONNECTED:    s->state = CYBER_WS_CONNECTING; break;
        case CYBER_WS_EV_HELLO:
            set_session_id(s, session_id);
            s->state = CYBER_WS_READY;
            break;
        case CYBER_WS_EV_FAIL:         s->state = CYBER_WS_ERROR; break;
        case CYBER_WS_EV_DISCONNECTED: s->state = CYBER_WS_IDLE;  break;
        default: break;
        }
        break;

    case CYBER_WS_READY:
        switch (ev) {
        case CYBER_WS_EV_LISTEN_START: s->state = CYBER_WS_LISTENING; break;
        // 服务端可以在没有我方收音的情况下直接推 TTS（多段回复、服务端主动播报）。
        // 只认 LISTENING→SPEAKING 会把这些音频判成"不该播"而整段丢掉。
        case CYBER_WS_EV_TTS_START:    s->state = CYBER_WS_SPEAKING; break;
        case CYBER_WS_EV_CONNECT:      s->state = CYBER_WS_CONNECTING; break;
        case CYBER_WS_EV_DISCONNECTED: s->state = CYBER_WS_IDLE; break;
        case CYBER_WS_EV_FAIL:         s->state = CYBER_WS_ERROR; break;
        default: break;
        }
        break;

    case CYBER_WS_LISTENING:
        switch (ev) {
        case CYBER_WS_EV_LISTEN_STOP:  s->state = CYBER_WS_READY; break;
        // 服务端开始说话：必须停止上行并转到播放。继续收音会把自己的麦克风
        // 灌进扬声器（无 AEC 时即啸叫），也会和播放抢同一个 I2S 通道。
        case CYBER_WS_EV_TTS_START:    s->state = CYBER_WS_SPEAKING; break;
        case CYBER_WS_EV_ABORT:        s->state = CYBER_WS_READY; break;
        case CYBER_WS_EV_DISCONNECTED: s->state = CYBER_WS_IDLE; break;
        case CYBER_WS_EV_FAIL:         s->state = CYBER_WS_ERROR; break;
        default: break;
        }
        break;

    case CYBER_WS_SPEAKING:
        switch (ev) {
        // 手动模式：一段回复播完就回到 READY 等下一次按键，不自动接着听。
        case CYBER_WS_EV_TTS_STOP:     s->state = CYBER_WS_READY; break;
        case CYBER_WS_EV_ABORT:        s->state = CYBER_WS_READY; break;
        // 打断后立刻重新说：允许从播放直接进入收音（barge-in）。
        case CYBER_WS_EV_LISTEN_START: s->state = CYBER_WS_LISTENING; break;
        case CYBER_WS_EV_DISCONNECTED: s->state = CYBER_WS_IDLE; break;
        case CYBER_WS_EV_FAIL:         s->state = CYBER_WS_ERROR; break;
        default: break;
        }
        break;

    case CYBER_WS_ERROR:
        switch (ev) {
        case CYBER_WS_EV_CONNECT:      s->state = CYBER_WS_CONNECTING; break;
        case CYBER_WS_EV_DISCONNECTED: s->state = CYBER_WS_IDLE; break;
        default: break;
        }
        break;

    case CYBER_WS_OFF:
    default:
        s->state = CYBER_WS_OFF;
        break;
    }

    // 会话一旦结束，会话 id 就失去意义；留着它会让下一轮的第一条消息带上
    // 服务端已经释放的 id。READY/LISTENING/SPEAKING 之间切换时不清。
    if (s->state == CYBER_WS_IDLE || s->state == CYBER_WS_ERROR) {
        s->session_id[0] = '\0';
    }
    return s->state;
}

bool cyber_ws_wants_mic(const cyber_ws_session_t *s)
{
    return s != NULL && s->state == CYBER_WS_LISTENING;
}

bool cyber_ws_wants_playback(const cyber_ws_session_t *s)
{
    // 只有确认在说话时才播放。收音期间服务端可能仍在推上一段的尾包，
    // 照播会与麦克风抢 I2S，也会把用户自己的声音从喇叭放出来。
    return s != NULL && s->state == CYBER_WS_SPEAKING;
}

const char *cyber_ws_state_label(cyber_ws_state_t st)
{
    switch (st) {
    case CYBER_WS_OFF:        return "OFF";
    case CYBER_WS_IDLE:       return "IDLE";
    case CYBER_WS_CONNECTING: return "LINKING";
    case CYBER_WS_READY:      return "READY";
    case CYBER_WS_LISTENING:  return "LISTEN";
    case CYBER_WS_SPEAKING:   return "SPEAK";
    case CYBER_WS_ERROR:      return "ERROR";
    default:                  return "?";
    }
}

// ---------------------------------------------------------------------------
// 帧数学
// ---------------------------------------------------------------------------
uint32_t cyber_ws_frame_ms(void)
{
    return CYBER_WS_FRAME_MS;
}

size_t cyber_ws_pcm_bytes(size_t samples)
{
    // 16 bit 单声道：每采样 2 字节，再乘声道数。
    return samples * 2u * (size_t)CYBER_WS_CHANNELS;
}
