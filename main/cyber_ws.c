// main/cyber_ws.c
// 见 cyber_ws.h 的分工与线程约束。
//
// 并发模型（这是本文件最容易写错的地方，先说清楚）：
//   - esp_websocket_client 的事件回调跑在【事件循环任务】里，不是本模块的 worker；
//   - 因此回调只做三件事：把收到的帧解析成事件、在锁内推进会话状态、往请求队列投
//     一条"该发什么"的请求。它【不】直接调用任何 esp_websocket_client_send_*。
//   - 所有发送、建连、销毁都在 worker 任务里做，客户端句柄因此只有一个 owner。
// 这样既不必在事件循环里做可能阻塞的发送（事件循环同时也在跑 Wi-Fi 事件），
// 也不用为句柄再引一层锁。
#include "cyber_ws.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cyber_ota.h"
#include "esp_netif.h"
#include "netdb.h"
#include "arpa/inet.h"
#include <sys/socket.h>

static const char *TAG = "cyber_ws";

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------
#define CYBER_WS_TASK_STACK      4096
#define CYBER_WS_TASK_PRIO       5
// WebSocket 接收缓冲。服务端文本消息最长的是带字幕的 tts 句，几百字节足够；
// 这个值同时决定单次 DATA 事件的最大载荷，越大越省一次拼帧。
#define CYBER_WS_RX_BUF          2048
// 组帧缓冲：一次完整消息的上限。超出即判为异常并丢弃，绝不让它无限增长。
#define CYBER_WS_IN_CAP          1024
#define CYBER_WS_IN_QDEPTH       2
// 请求队列深度。按键是人工节奏，8 条足够吸收抖动。
#define CYBER_WS_REQ_QDEPTH      8
// 上行音频队列深度。音频 worker 每 60 ms 产出一帧，这里只吸收网络抖动：4 帧
// = 240 ms。再深只会让"已经说过的话"迟到更久，对实时对话是负收益。
#define CYBER_WS_AUDIO_QDEPTH    4
// 上行音频发送的等待上限。音频 worker 按 60 ms 的节奏产出，等超过半帧就不值得了：
// 与其把麦克风采集堵住，不如丢掉这一帧并计数。
#define CYBER_WS_AUDIO_SEND_TICKS pdMS_TO_TICKS(40)
// 发送超时。握手/控制消息都很小，正常毫秒级完成；这里只用于兜底，避免 worker 卡死。
#define CYBER_WS_SEND_TICKS      pdMS_TO_TICKS(3000)
// 握手超时：建连后这么久没等到服务端 hello 就判失败。服务端 hello 是会话契约的
// 起点，等不到它就没有任何一条消息可以发。
#define CYBER_WS_HELLO_TIMEOUT_US (12 * 1000 * 1000)
// worker 的轮询节拍：既用于握手超时检查，也是上行音频的发送节奏上限。
// 20 ms 而不是 50 ms：上行每 60 ms 一帧，节拍太粗会让每帧都晚发最多一个节拍，
// 而服务端是按到达时间做识别与 AEC 的。20 ms 的唤醒开销可以忽略。
#define CYBER_WS_TICK_MS         20
// 建连前等待后台 OTA 检查让出 TLS 的上限。实测一次检查约 5~6 秒，给一倍余量。
#define CYBER_WS_TLS_WAIT_MS     12000u
// 预建连的续期间隔。实测一次完整握手要 4.9 秒，而用户按下按键的那一刻已经在说话，
// 所以链路必须【提前】建好，按键路径上不能有握手。链路一旦掉了就要自动重建，否则
// 这个"即时"只维持到第一次掉线为止。
//
// 5 秒不是为了"重试得快"，而是为了不在网络抖动时变成对 TLS 的紧凑重试：每次失败
// 都要走一遍完整握手，比按键延迟本身更耗电、也更占内存。
#define CYBER_WS_RECONNECT_MS    5000u
// "健康到寿"的判定阈值：一条会话活过这么久，才算服务端按空闲超时正常收场，而不是
// 建连异常。服务端的 READY 空闲超时实测在 66~70 秒，取 10 秒远低于正常寿命，又能
// 挡住"连上就断"的死循环。
#define CYBER_WS_HEALTHY_US      (10LL * 1000 * 1000)
// 健康到寿后立刻重连。此时退避没有意义：服务端只是按空闲超时收走了连接，等那 5 秒
// 纯粹是在扩大"按下没反应"的窗口。
#define CYBER_WS_RECONNECT_PROMPT_MS  0u

// ---------------------------------------------------------------------------
// 请求（LVGL 任务 / 事件回调 → worker）
// ---------------------------------------------------------------------------
typedef enum {
    REQ_ENABLE = 0,
    REQ_DISABLE,
    REQ_LISTEN_START,
    REQ_LISTEN_STOP,
    REQ_ABORT,
    REQ_SEND_HELLO,   // 事件回调在建连成功时投递
    REQ_TEARDOWN,     // 事件回调在掉线/出错时投递
} ws_req_t;

// ---------------------------------------------------------------------------
// 上行音频帧（音频 worker → 本模块 worker）
// ---------------------------------------------------------------------------
// 定长，整帧拷进队列：发送时必须已经拥有整帧数据，否则音频 worker 早就在拼下一帧了。
typedef struct {
    uint16_t len;
    uint8_t  data[CYBER_WS_MAX_OPUS_FRAME];
} ws_audio_tx_t;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static QueueHandle_t s_req_q;
static QueueHandle_t s_audio_q;    // 上行 Opus 帧
static SemaphoreHandle_t s_lock;   // 保护 s_sess 与 s_pub，两者总是一起更新
static cyber_ws_snapshot_t s_pub;

// 入站音频帧的接收点。只在 init 之前设置，此后只读。
static cyber_ws_audio_sink_t s_audio_sink;

// 以下几项只有 worker 访问，因此无需加锁。
static cyber_ws_session_t s_sess;
static esp_websocket_client_handle_t s_client;
static int64_t s_connect_us;       // 上次开始建连的时刻，用于握手超时
// 距下次允许预建连的剩余毫秒。掉线后置位，由 housekeeping 递减。
static uint32_t s_reconnect_wait_ms;
// 事件回调测出的本次收场该等多久，由 worker 在拆链后取用。事件回调跑在 WebSocket
// 自己的任务里，不能直接改 s_reconnect_wait_ms（那是 worker 的状态）。
static uint32_t s_drop_reconnect_ms;

// 事件回调查阅、worker 修改。单字节对齐读写即原子，且无读-改-写。
static volatile bool s_want_listen;
// 事件回调查阅、worker 修改：当前是否已收到 hello（决定能不能发 listen）。
static volatile bool s_handshaken;

// 组帧缓冲，只在事件回调（单任务）里使用。
static char s_rx[CYBER_WS_IN_CAP];
static size_t s_rx_len;
static bool s_rx_binary;
static bool s_rx_overflow;

// ---------------------------------------------------------------------------
// 快照发布
// ---------------------------------------------------------------------------
// 推进状态机并同步快照。返回新状态，供调用方决定后续动作。
// 会话与快照在同一个锁内一起更新：读快照的界面不会看到"状态已变但 session_id 还是旧的"。
static cyber_ws_state_t step_publish(cyber_ws_event_t ev, const char *sid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const cyber_ws_state_t st = cyber_ws_session_step(&s_sess, ev, sid);
    s_pub.state = st;
    memcpy(s_pub.session_id, s_sess.session_id, sizeof(s_pub.session_id));
    xSemaphoreGive(s_lock);
    return st;
}

static void publish_error(int err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pub.last_error = err;
    xSemaphoreGive(s_lock);
}

static void publish_subtitle(const char *text)
{
    // 先在栈上定长裁剪，再进临界区：临界区里只做一次 memcpy，不跑 snprintf。
    char tmp[CYBER_WS_SUBTITLE_CAP];
    if (text == NULL) {
        tmp[0] = '\0';
    } else {
        size_t n = strlen(text);
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, text, n);
        tmp[n] = '\0';
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_pub.subtitle, tmp, sizeof(tmp));
    xSemaphoreGive(s_lock);
}

static void publish_handshake(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pub.handshakes++;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------------
// 入站 JSON
// ---------------------------------------------------------------------------
// 把 cJSON 字符串字段拷进定长缓冲；字段缺失/类型不符时置空，绝不留下上次的残值。
static void json_str(const cJSON *o, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        size_t n = strlen(it->valuestring);
        if (n >= cap) n = cap - 1;
        memcpy(out, it->valuestring, n);
        out[n] = '\0';
    }
}

// 定义在后面的上行发送区，此处前向声明：收到 tts start 时要立刻丢弃排队中的上行帧。
static void flush_uplink_audio(void);

static void handle_json(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "下行 JSON 解析失败（%u 字节）", (unsigned)len);
        return;
    }

    char type[24];
    json_str(root, "type", type, sizeof(type));

    if (strcmp(type, "hello") == 0) {
        // 服务端可能在 hello 里声明它实际采用的音频参数。上行是我们申报的
        // 16k/60ms，下行由服务端决定——实测 api.tenclass.net 用 24000 Hz，
        // 上游文档也说明下行可能用 24000 以换取更好的音乐效果。
        //
        // 这层校验不能省：采样率或帧长对不上，症状只会是变调或静音，那时候再
        // 回头查协议代价极高。宁可让这次握手失败，也不要"将就着播"。
        const cJSON *ap = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        char fmt[16];
        fmt[0] = '\0';
        int rate = (int)CYBER_WS_SAMPLE_RATE;
        int ch = (int)CYBER_WS_CHANNELS;
        int frame_ms = (int)CYBER_WS_FRAME_MS;
        if (cJSON_IsObject(ap)) {
            json_str(ap, "format", fmt, sizeof(fmt));
            const cJSON *it = cJSON_GetObjectItemCaseSensitive(ap, "sample_rate");
            if (cJSON_IsNumber(it)) rate = it->valueint;
            it = cJSON_GetObjectItemCaseSensitive(ap, "channels");
            if (cJSON_IsNumber(it)) ch = it->valueint;
            it = cJSON_GetObjectItemCaseSensitive(ap, "frame_duration");
            if (cJSON_IsNumber(it)) frame_ms = it->valueint;
        }

        cyber_audio_format_t down;
        const bool down_ok =
            (fmt[0] == '\0' || strcmp(fmt, "opus") == 0) && ch == (int)CYBER_WS_CHANNELS &&
            cyber_audio_format_from_downlink(&down, (uint32_t)rate, (uint32_t)frame_ms);
        if (!down_ok) {
            ESP_LOGE(TAG, "服务端音频参数本机无法处理：format=%s rate=%d channels=%d frame=%dms",
                     fmt[0] ? fmt : "(未声明)", rate, ch, frame_ms);
            ESP_LOGE(TAG, "本机支持 opus / 16000 或 24000 Hz / 1 声道 / 60ms；判为握手失败");
            step_publish(CYBER_WS_EV_FAIL, NULL);
            publish_error(-1);
            ws_req_t tr = REQ_TEARDOWN;
            xQueueSend(s_req_q, &tr, 0);
            cJSON_Delete(root);
            return;
        }
        ESP_LOGI(TAG, "音频参数：format=%s 上行=%uHz 下行=%uHz channels=%d frame=%dms",
                 fmt[0] ? fmt : "opus", (unsigned)CYBER_WS_SAMPLE_RATE,
                 (unsigned)down.rate, ch, frame_ms);

        // 先写下行格式再推进状态：读快照的一方看到 READY 时，必须已经能拿到正确的
        // 播放格式，否则它会先按默认值配好 codec 再收到一次格式变更。
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pub.downlink = down;
        xSemaphoreGive(s_lock);

        char sid[CYBER_WS_SESSION_CAP];
        json_str(root, "session_id", sid, sizeof(sid));
        const cyber_ws_state_t st = step_publish(CYBER_WS_EV_HELLO, sid);
        s_handshaken = (st == CYBER_WS_READY);
        publish_handshake();
        ESP_LOGI(TAG, "握手完成：state=%s session=%s",
                 cyber_ws_state_label(st), sid[0] ? sid : "(无)");
        // 若用户在握手期间就按下了说话键，这里补发 listen。
        if (s_handshaken && s_want_listen) {
            ws_req_t r = REQ_LISTEN_START;
            xQueueSend(s_req_q, &r, 0);
        } else {
            // 这条日志是刻意留的：若不补发而设备停在 READY 上，看日志的人必须能
            // 立刻分辨是"用户已撤回收音"还是"补发逻辑坏了"。否则只能靠猜，
            // 而猜错的代价是去查一段根本没问题的握手代码。
            ESP_LOGI(TAG, "hello 后不补发 listen：收音意愿=%d", (int)s_want_listen);
        }
    } else if (strcmp(type, "stt") == 0) {
        char text[CYBER_WS_SUBTITLE_CAP];
        json_str(root, "text", text, sizeof(text));
        step_publish(CYBER_WS_EV_STT, NULL);
        publish_subtitle(text);
        ESP_LOGI(TAG, "识别：%s", text);
    } else if (strcmp(type, "llm") == 0) {
        // 情绪字段留给后续的表情联动；本增量只记录。
        char emotion[24];
        json_str(root, "emotion", emotion, sizeof(emotion));
        ESP_LOGD(TAG, "llm emotion=%s", emotion);
    } else if (strcmp(type, "tts") == 0) {
        char state[20];
        json_str(root, "state", state, sizeof(state));
        if (strcmp(state, "start") == 0) {
            // 标准流转 Listening --TTS Start--> Speaking 的语义是"立即停止录音"：
            // 先丢掉排队中还没发出的麦克风帧，再转状态。否则这些帧会在接下来的
            // 排空循环里被补发，等于服务端已经开始回话、设备还在继续送话。
            flush_uplink_audio();
            step_publish(CYBER_WS_EV_TTS_START, NULL);
        } else if (strcmp(state, "stop") == 0) {
            step_publish(CYBER_WS_EV_TTS_STOP, NULL);
        } else if (strcmp(state, "sentence_start") == 0) {
            char text[CYBER_WS_SUBTITLE_CAP];
            json_str(root, "text", text, sizeof(text));
            step_publish(CYBER_WS_EV_SENTENCE, NULL);
            publish_subtitle(text);
            ESP_LOGI(TAG, "回复：%s", text);
        } else {
            ESP_LOGD(TAG, "tts state=%s（未处理）", state);
        }
    } else if (strcmp(type, "system") == 0) {
        char cmd[24];
        json_str(root, "command", cmd, sizeof(cmd));
        ESP_LOGW(TAG, "服务端指令：%s（本固件未实现，忽略）", cmd);
    } else if (strcmp(type, "alert") == 0) {
        char msg[CYBER_WS_SUBTITLE_CAP];
        json_str(root, "message", msg, sizeof(msg));
        publish_subtitle(msg);
        ESP_LOGW(TAG, "服务端告警：%s", msg);
    } else {
        // mcp 等本次未实现的类型：记一条调试日志即可，不要判成协议错误——
        // 服务端多发消息不该让设备掉线。
        ESP_LOGD(TAG, "未处理的 type=%s", type);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// 组帧
// ---------------------------------------------------------------------------
static void rx_reset(void)
{
    s_rx_len = 0;
    s_rx_binary = false;
    s_rx_overflow = false;
}

// 处理一个 DATA 事件。esp_websocket_client 会在载荷超过接收缓冲时把一条消息
// 拆成多次事件（payload_offset 递增），因此必须按 offset 拼接。
static void handle_data(const esp_websocket_event_data_t *e)
{
    if (e->payload_offset == 0) {
        rx_reset();
        s_rx_binary = (e->op_code == 0x02);
        // 单条消息就超过组帧上限：直接标溢出，后续分片全部丢弃，
        // 免得一个畸形长度把缓冲区写穿。
        if (e->payload_len > (int)sizeof(s_rx)) s_rx_overflow = true;
    }

    if (s_rx_overflow) {
        // 仍需判断是否收尾，收尾后复位即可。
        if (e->payload_offset + e->data_len >= e->payload_len) rx_reset();
        return;
    }

    if (e->data_len > 0 && e->data_ptr != NULL) {
        if (s_rx_len + (size_t)e->data_len <= sizeof(s_rx)) {
            memcpy(s_rx + s_rx_len, e->data_ptr, (size_t)e->data_len);
            s_rx_len += (size_t)e->data_len;
        } else {
            s_rx_overflow = true;
        }
    }

    const bool complete = (e->payload_offset + e->data_len) >= e->payload_len;
    if (!complete) return;

    if (!s_rx_overflow) {
        if (s_rx_binary) {
            // 二进制即 Opus 帧，交给音频层。接收点必须在调用内把数据拷走：
            // s_rx 是组帧缓冲，下一个 DATA 事件就会覆盖它。
            if (s_audio_sink != NULL) {
                s_audio_sink(s_rx, s_rx_len);
            } else {
                // 静默丢弃会让"对方在说话但没声音"变成一个查不出来的现象。
                ESP_LOGD(TAG, "收到 Opus 帧 %u 字节（未注册音频接收点，丢弃）",
                         (unsigned)s_rx_len);
            }
        } else if (s_rx_len == 0) {
            // 空文本帧：实测 api.tenclass.net 每 ~10s 发一帧，是服务端的保活探测。
            // 官方协议里没有它的定义（文档原文："空消息没有标准业务定义"），
            // 也没有要求设备回应，所以这里什么都不做。判成解析失败会让日志
            // 每 10 秒刷一条 WARN，把真正的异常淹掉。
            ESP_LOGD(TAG, "服务端保活（空帧）");
        } else {
            handle_json(s_rx, s_rx_len);
        }
    } else {
        ESP_LOGW(TAG, "下行消息超过 %u 字节上限，已丢弃", (unsigned)sizeof(s_rx));
    }
    rx_reset();
}

// ---------------------------------------------------------------------------
// 事件回调（跑在事件循环任务里）
// ---------------------------------------------------------------------------
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *e = (const esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        // 只表示链路通了，会话尚未就绪：仍停在 CONNECTING，等 hello。
        step_publish(CYBER_WS_EV_CONNECTED, NULL);
        ESP_LOGI(TAG, "链路已建立，等待服务端 hello");
        ws_req_t r = REQ_SEND_HELLO;
        xQueueSend(s_req_q, &r, 0);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (e != NULL) handle_data(e);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {
        s_handshaken = false;
        step_publish(CYBER_WS_EV_DISCONNECTED, NULL);
        ESP_LOGW(TAG, "链路断开");
        // 区分"服务端按空闲超时正常收场"和"建连/握手异常"：前者立刻重连，后者退避。
        const int64_t lived_us = (s_connect_us != 0)
                                 ? (esp_timer_get_time() - s_connect_us) : 0;
        s_drop_reconnect_ms = (lived_us > CYBER_WS_HEALTHY_US)
                              ? CYBER_WS_RECONNECT_PROMPT_MS
                              : CYBER_WS_RECONNECT_MS;
        ws_req_t r = REQ_TEARDOWN;
        xQueueSend(s_req_q, &r, 0);
        break;
    }
    case WEBSOCKET_EVENT_ERROR: {
        s_handshaken = false;
        // 错误字符串在 e->data_ptr 里（可能是错误消息而非业务数据）。
        step_publish(CYBER_WS_EV_FAIL, NULL);
        publish_error(e != NULL ? e->data_len : -1);
        ESP_LOGE(TAG, "WebSocket 错误：%.*s", e != NULL ? e->data_len : 0,
                 (e != NULL && e->data_ptr != NULL) ? e->data_ptr : "");
        // 出错一律退避：连不上时的紧凑重试每次都要走完整握手，代价比那几秒等待大。
        s_drop_reconnect_ms = CYBER_WS_RECONNECT_MS;
        ws_req_t r = REQ_TEARDOWN;
        xQueueSend(s_req_q, &r, 0);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 发送（只在 worker 里调用）
// ---------------------------------------------------------------------------
static bool send_text(const char *json, size_t len)
{
    if (s_client == NULL || !s_handshaken) return false;
    const int n = esp_websocket_client_send_text(s_client, json, (int)len, CYBER_WS_SEND_TICKS);
    if (n < 0) {
        ESP_LOGW(TAG, "发送失败：%s", json);
        return false;
    }
    return true;
}

static void send_hello(void)
{
    char json[CYBER_WS_JSON_CAP];
    const size_t n = cyber_ws_build_hello(json, sizeof(json));
    if (n == 0) {
        ESP_LOGE(TAG, "hello 构造失败（缓冲 %u 不足）", (unsigned)sizeof(json));
        return;
    }
    if (s_client == NULL) return;
    // hello 是握手的第一条消息，此刻 s_handshaken 还是 false，因此不能走 send_text。
    const int sent = esp_websocket_client_send_text(s_client, json, (int)n, CYBER_WS_SEND_TICKS);
    if (sent < 0) {
        ESP_LOGE(TAG, "hello 发送失败");
        return;
    }
    ESP_LOGI(TAG, "hello 已发送");
}

// 发送一个二进制帧（Opus）。只在 worker 里调用。
static bool send_binary(const void *data, size_t len)
{
    if (s_client == NULL || !s_handshaken) return false;
    // esp_websocket_client 的 bin 接口吃 const char *，数据本身是字节流，
    // 这里只需要一次指针类型转换，不做任何编码。
    const int n = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len,
                                                CYBER_WS_SEND_TICKS);
    if (n < 0) {
        ESP_LOGW(TAG, "音频发送失败（%u 字节）", (unsigned)len);
        return false;
    }
    return true;
}

// 把上行队列里已排队的帧尽快发出去。发送失败时把剩余帧丢掉：它们已经陈旧，
// 补发只会让服务端收到时间错位的语音，不如让接收端出现一次短暂的静音。
static void drain_audio_tx(void)
{
    ws_audio_tx_t item;
    while (xQueueReceive(s_audio_q, &item, 0) == pdTRUE) {
        if (!send_binary(item.data, item.len)) {
            while (xQueueReceive(s_audio_q, &item, 0) == pdTRUE) { /* 丢弃 */ }
            break;
        }
    }
}

// 丢弃排队中的上行音频。这些帧一旦落后于会话节奏就必须丢，补发只会让服务端收到
// 时间错位的语音。两个调用时机都会让它们过期：
//   - TTS_START：服务端已判定本轮结束并开始回话（标准流转 Listening --TTS Start-->
//     Speaking 的语义就是"停止录音"），此时补发等于把用户的话塞进回复之后；
//   - 拆链：这些帧属于已经结束的会话。
static void flush_uplink_audio(void)
{
    if (s_audio_q != NULL) xQueueReset(s_audio_q);
}

static void send_listen(cyber_ws_listen_t mode)
{
    char json[CYBER_WS_JSON_CAP];
    char sid[CYBER_WS_SESSION_CAP];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(sid, s_sess.session_id, sizeof(sid));
    xSemaphoreGive(s_lock);

    const size_t n = cyber_ws_build_listen(json, sizeof(json), sid, mode, CYBER_WS_MODE_MANUAL);
    if (n > 0) send_text(json, n);
}

static void send_abort(void)
{
    char json[CYBER_WS_JSON_CAP];
    char sid[CYBER_WS_SESSION_CAP];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(sid, s_sess.session_id, sizeof(sid));
    xSemaphoreGive(s_lock);

    const size_t n = cyber_ws_build_abort(json, sizeof(json), sid, "user_interrupt");
    if (n > 0) send_text(json, n);
}

// ---------------------------------------------------------------------------
// 建连 / 拆链（只在 worker 里调用）
// ---------------------------------------------------------------------------
static void teardown(void)
{
    if (s_client == NULL) return;
    // 先 close 再 destroy：close 会走一次 WS CLOSE 握手，让服务端立刻回收会话，
    // 而不是等它自己的超时。
    esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_handshaken = false;
    rx_reset();
    // 丢弃排队中的上行音频：这些帧属于刚结束的会话，带到下一次建连只会让服务端
    // 收到一段时间错位的语音。
    flush_uplink_audio();
    ESP_LOGI(TAG, "链路已拆解，free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // 任何拆链都给下一次预建连留出间隔。没有这一句，一次网络抖动会变成对 TLS 的
    // 紧凑重试：每次失败都要走完整个握手，代价比那 5 秒的等待大得多。
    s_reconnect_wait_ms = CYBER_WS_RECONNECT_MS;
}

static esp_err_t start_connection(void)
{
    if (s_client != NULL) return ESP_OK;

    // 静态缓冲：esp_websocket_client 会 strdup headers，但 uri 等字段的生命周期
    // 不如依赖库文档明确，用静态存储避免悬空指针这类难查的问题。
    static char url[192];
    static char token[160];
    static char mac[24];
    static char cid[48];
    static char headers[384];

    if (cyber_ota_get_ws_config(url, sizeof(url), token, sizeof(token)) != ESP_OK) {
        ESP_LOGW(TAG, "尚无对话端点（未完成激活）");
        return ESP_ERR_NOT_FOUND;
    }

    if (cyber_ota_get_device_mac(mac, sizeof(mac)) != ESP_OK ||
        cyber_ota_get_client_id(cid, sizeof(cid)) != ESP_OK) {
        ESP_LOGW(TAG, "缺少 Device-Id / Client-Id");
        return ESP_FAIL;
    }

    const int hn = snprintf(headers, sizeof(headers),
        "Authorization: Bearer %s\r\n"
        "Protocol-Version: %d\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n",
        token, CYBER_WS_PROTO_VERSION, mac, cid);
    if (hn < 0 || (size_t)hn >= sizeof(headers)) {
        ESP_LOGE(TAG, "握手头过长");
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_websocket_client_config_t cfg = {
        .uri = url,
        .headers = headers,
        // 证书校验走 esp_crt_bundle，与 OTA 通道一致。api.tenclass.net 的链是
        // DigiCert 交叉签名（GeoTrust 中间证书由交叉背书的根签发），默认单锚验证
        // 会命中错误信任路径而报 PK verify 0x4290 / CERT_VERIFY_FAILED。交叉签名
        // 验证由 sdkconfig.defaults 的
        // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y（仅 IDF 6.x 提供）
        // 统一开启，此处不再单独挂 CA 链。
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = CYBER_WS_RX_BUF,
        .task_stack = CYBER_WS_TASK_STACK,
        .network_timeout_ms = 10000,
        // 自己管生命周期：自动重连会在我们主动挂断后仍然去连，白白耗电。
        .disable_auto_reconnect = true,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "websocket 初始化失败");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);

    const esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket 启动失败: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_connect_us = esp_timer_get_time();
    ESP_LOGI(TAG, "开始建连：%s free=%u largest=%u", url,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 请求处理（worker）
// ---------------------------------------------------------------------------
// 建连前等后台检查让出 TLS。
//
// 实测：一次 OTA 检查（约 5~6 秒，每 60 秒一轮）与 WebSocket 握手叠加时，堆最低
// 点只剩 108 字节，libopus 的编码器分配随之失败，整轮对话没有声音。两条 TLS 在
// 这块 8 MB flash / 无 PSRAM 的板子上不可能共存，所以让建连方等，而不是让它们
// 抢——抢的结果是两条都残缺。
//
// 在本 worker 任务里阻塞是可以接受的：它此刻还没有会话，上行队列必然是空的，
// 没有人在等它。等待上限刻意给足一次检查的时间，超时就照常建连（宁可少一层
// 保护，也不要让按键彻底没反应）。
static void wait_for_tls_room(void)
{
    if (!cyber_ota_busy()) return;

    ESP_LOGI(TAG, "后台检查占用 TLS，等它让出内存再建连");
    const uint32_t step_ms = 50;
    for (uint32_t waited = 0; waited < CYBER_WS_TLS_WAIT_MS; waited += step_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        if (!cyber_ota_busy()) {
            ESP_LOGI(TAG, "后台检查已结束，继续建连（让出 %ums）", (unsigned)waited);
            return;
        }
    }
    ESP_LOGW(TAG, "等待后台检查超时 %ums，仍按原样建连", (unsigned)CYBER_WS_TLS_WAIT_MS);
}

static void on_listen_start(void)
{
    s_want_listen = true;

    if (s_sess.state == CYBER_WS_IDLE || s_sess.state == CYBER_WS_ERROR) {
        wait_for_tls_room();
        if (start_connection() != ESP_OK) {
            step_publish(CYBER_WS_EV_FAIL, NULL);
            publish_error(-1);
            return;
        }
        step_publish(CYBER_WS_EV_CONNECT, NULL);
        // 状态已是 CONNECTING：等 hello 到达后由回调补发 listen（见 handle_json）。
        return;
    }

    if (!s_handshaken) {
        // 正在握手：记下意愿，hello 到达后补发。
        ESP_LOGI(TAG, "握手进行中，已记下收音请求");
        return;
    }

    if (s_sess.state == CYBER_WS_SPEAKING) {
        // 播放中按下说话键 = 打断并立刻改说。
        send_abort();
    }
    if (s_sess.state == CYBER_WS_READY || s_sess.state == CYBER_WS_SPEAKING) {
        send_listen(CYBER_WS_LISTEN_START);
        step_publish(CYBER_WS_EV_LISTEN_START, NULL);
    }
}

static void on_listen_stop(void)
{
    const cyber_ws_state_t st = s_sess.state;
    s_want_listen = false;

    if (st == CYBER_WS_LISTENING) {
        send_listen(CYBER_WS_LISTEN_STOP);
        step_publish(CYBER_WS_EV_LISTEN_STOP, NULL);
        // 已经通知服务端停止上行，排队中还留着的麦克风帧就作废了：再发出去
        // 会出现在 listen:stop 之后，服务端只会把它们当作噪声丢给识别。
        flush_uplink_audio();
        return;
    }

    if (st == CYBER_WS_CONNECTING) {
        // 收音意愿在握手期间被撤回：这条链路存在的唯一理由就消失了。留着它不会
        // 自己收场——实测它会一路走到 READY 然后干等，服务端要 ~68 秒才超时断开，
        // 这期间 TLS 会话一直占着堆（此时最大连续块常常只剩 32 KB）。直接拆掉，
        // 代价只是下次按键要重新握手 4.9 秒，比挂着一个没人要的连接划算。
        ESP_LOGI(TAG, "握手期间撤回收音，直接拆链");
        teardown();
        // 用户刚按过键，链路要尽快回到"按下即可用"，不要吃那 5 秒退避。
        s_reconnect_wait_ms = CYBER_WS_RECONNECT_PROMPT_MS;
        step_publish(CYBER_WS_EV_DISCONNECTED, NULL);
        return;
    }

    // READY 上的停止是常态（一轮结束后按键收回，或用户重复按）：连接刻意留着
    // 复用，不在这里拆。这条日志是为了区分"真的没事发生"与"请求根本没收到"。
    ESP_LOGI(TAG, "停止收音：state=%s 握手=%d（无需动作）",
             cyber_ws_state_label(st), (int)s_handshaken);
}

static void on_abort(void)
{
    s_want_listen = false;
    if (s_sess.state == CYBER_WS_LISTENING || s_sess.state == CYBER_WS_SPEAKING) {
        send_abort();
        // 打断即停：正在收音时，排队中的麦克风帧同样作废（理由同 TTS_START）；
        // 正在播放时，丢弃由音频层按相位切到 STOP/PLAYBACK 完成，这里不越权。
        flush_uplink_audio();
        step_publish(CYBER_WS_EV_ABORT, NULL);
    }
}

static void handle_req(ws_req_t r)
{
    switch (r) {
    case REQ_ENABLE:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cyber_ws_session_set_enabled(&s_sess, true);
        s_pub.state = s_sess.state;
        xSemaphoreGive(s_lock);
        // 预建连，而不是等第一次按键。
        //
        // 实测一次完整握手（DNS + TLS + WS upgrade + hello 往返）要 4.9 秒，而用户
        // 按下按键的那一刻已经在说话了。懒建连等于把开头近 5 秒的语音整段丢掉——
        // 对"按下就说"的交互这是硬伤，不是可接受的启动延迟。
        //
        // 提前建好之后，按键路径上只剩一条 listen:start（实测 <20 ms）。代价是这条
        // 链路要常驻（约 40 KB），因此会话期间后台 OTA 复查会被挂起，见
        // cyber_ota_set_defer。
        s_reconnect_wait_ms = 0;
        wait_for_tls_room();
        if (start_connection() == ESP_OK) {
            step_publish(CYBER_WS_EV_CONNECT, NULL);
            ESP_LOGI(TAG, "对话功能已启用：预建连中（建好后按键即可收音）");
        } else {
            ESP_LOGE(TAG, "对话功能已启用，但预建连失败；将由 housekeeping 重试");
            s_reconnect_wait_ms = CYBER_WS_RECONNECT_MS;
        }
        break;

    case REQ_DISABLE:
        s_want_listen = false;
        teardown();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cyber_ws_session_set_enabled(&s_sess, false);
        s_pub.state = s_sess.state;
        memcpy(s_pub.session_id, s_sess.session_id, sizeof(s_pub.session_id));
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "对话功能已停用");
        break;

    case REQ_LISTEN_START: on_listen_start(); break;
    case REQ_LISTEN_STOP:  on_listen_stop();  break;
    case REQ_ABORT:        on_abort();        break;

    case REQ_SEND_HELLO:
        send_hello();
        break;

    case REQ_TEARDOWN:
        teardown();
        // 覆盖 teardown 的默认退避：健康到寿（服务端空闲超时）的收场不需要等，
        // 那 5 秒纯粹是在扩大"按下没反应"的窗口。
        s_reconnect_wait_ms = s_drop_reconnect_ms;
        break;

    default: break;
    }
}

// 握手超时：连上了但迟迟等不到 hello，说明这条链路已经不可用，
// 不主动收场会让状态永远停在 CONNECTING，界面上就是一个转不完的圈。
static void housekeeping(void)
{
    // 1) 握手超时：连上了但迟迟等不到 hello，说明这条链路已经不可用，
    //    不主动收场会让状态永远停在 CONNECTING，界面上就是一个转不完的圈。
    if (s_sess.state == CYBER_WS_CONNECTING) {
        if (s_connect_us != 0 &&
            (esp_timer_get_time() - s_connect_us) >= CYBER_WS_HELLO_TIMEOUT_US) {
            ESP_LOGW(TAG, "握手超时，判定失败");
            s_want_listen = false;
            teardown();
            step_publish(CYBER_WS_EV_FAIL, NULL);
            publish_error(-1);
        }
        return;
    }

    // 2) 预建连的续期。让"按键即收音"在掉线之后也能自动恢复，而不是悄悄退回
    //    到按键付 4.9 秒握手的旧行为——那种退化用户只会感觉"有时候又变慢了"，
    //    极难归因。
    //
    //    三个前置条件都是必要的：已启用（禁用后不得偷偷建连）、状态 IDLE（不干扰
    //    正在进行的握手或会话）、没有客户端（真正需要重建）。
    if (!s_sess.enabled || s_sess.state != CYBER_WS_IDLE || s_client != NULL) return;

    if (s_reconnect_wait_ms > 0) {
        s_reconnect_wait_ms = (s_reconnect_wait_ms > CYBER_WS_TICK_MS)
                              ? (s_reconnect_wait_ms - CYBER_WS_TICK_MS) : 0;
        return;
    }

    wait_for_tls_room();
    if (start_connection() == ESP_OK) {
        step_publish(CYBER_WS_EV_CONNECT, NULL);
        ESP_LOGI(TAG, "预建连完成：按键将即时收音");
    } else {
        s_reconnect_wait_ms = CYBER_WS_RECONNECT_MS;
    }
}

static void ws_worker(void *arg)
{
    (void)arg;
    for (;;) {
        // 先发音频：它的节奏是硬的（每 60 ms 一帧），控制消息是由人工按键驱动的，
        // 迟一个节拍 20 ms 没有任何影响。
        drain_audio_tx();

        ws_req_t r;
        // 用短超时轮询而非无限等待：同一循环顺带跑握手超时检查。
        while (xQueueReceive(s_req_q, &r, 0) == pdTRUE) {
            handle_req(r);
        }
        housekeeping();
        vTaskDelay(pdMS_TO_TICKS(CYBER_WS_TICK_MS));
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t cyber_ws_init(void)
{
    static bool inited;
    if (inited) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_req_q = xQueueCreate(CYBER_WS_REQ_QDEPTH, sizeof(ws_req_t));
    s_audio_q = xQueueCreate(CYBER_WS_AUDIO_QDEPTH, sizeof(ws_audio_tx_t));
    if (s_req_q == NULL || s_audio_q == NULL) {
        if (s_audio_q != NULL) vQueueDelete(s_audio_q);
        if (s_req_q != NULL) vQueueDelete(s_req_q);
        s_audio_q = NULL;
        s_req_q = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    cyber_ws_session_init(&s_sess);
    memset(&s_pub, 0, sizeof(s_pub));
    s_pub.state = CYBER_WS_OFF;

    if (xTaskCreate(ws_worker, "cyber_ws", CYBER_WS_TASK_STACK, NULL,
                    CYBER_WS_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_audio_q);
        vQueueDelete(s_req_q);
        s_audio_q = NULL;
        s_req_q = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    inited = true;
    return ESP_OK;
}

void cyber_ws_set_enabled(bool enabled)
{
    if (s_req_q == NULL) return;
    const ws_req_t r = enabled ? REQ_ENABLE : REQ_DISABLE;
    xQueueSend(s_req_q, &r, 0);
}

void cyber_ws_request_listen(bool start)
{
    if (s_req_q == NULL) return;
    const ws_req_t r = start ? REQ_LISTEN_START : REQ_LISTEN_STOP;
    xQueueSend(s_req_q, &r, 0);
}

void cyber_ws_request_abort(void)
{
    if (s_req_q == NULL) return;
    const ws_req_t r = REQ_ABORT;
    xQueueSend(s_req_q, &r, 0);
}

void cyber_ws_set_audio_sink(cyber_ws_audio_sink_t sink)
{
    s_audio_sink = sink;
}

bool cyber_ws_send_audio(const void *opus, size_t len)
{
    if (s_audio_q == NULL || opus == NULL || len == 0u) return false;
    if (len > CYBER_WS_MAX_OPUS_FRAME) return false;
    // 握手完成前发音频没有意义：服务端还没建立会话，收到的帧会无处归属。
    if (!s_handshaken) return false;
    // 只有正在收音时才发。这条闸门拦的是"状态已切走、音频 worker 还在产出"的
    // 那几帧窗口：此刻服务端多半已经开始回话，补送麦克风帧只会污染识别。
    if (!cyber_ws_wants_mic(&s_sess)) return false;

    ws_audio_tx_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, opus, len);
    return xQueueSend(s_audio_q, &item, CYBER_WS_AUDIO_SEND_TICKS) == pdTRUE;
}

void cyber_ws_get_snapshot(cyber_ws_snapshot_t *out)
{
    if (out == NULL || s_lock == NULL) {
        if (out != NULL) memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_pub;
    xSemaphoreGive(s_lock);
}

uint32_t cyber_ws_last_free_heap(void)
{
    return (uint32_t)esp_get_free_heap_size();
}
