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
// 发送超时。握手/控制消息都很小，正常毫秒级完成；这里只用于兜底，避免 worker 卡死。
#define CYBER_WS_SEND_TICKS      pdMS_TO_TICKS(3000)
// 握手超时：建连后这么久没等到服务端 hello 就判失败。服务端 hello 是会话契约的
// 起点，等不到它就没有任何一条消息可以发。
#define CYBER_WS_HELLO_TIMEOUT_US (12 * 1000 * 1000)
// worker 的轮询节拍：同时用作握手超时的检查周期。
#define CYBER_WS_TICK_MS         50

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
// 状态
// ---------------------------------------------------------------------------
static QueueHandle_t s_req_q;
static SemaphoreHandle_t s_lock;   // 保护 s_sess 与 s_pub，两者总是一起更新
static cyber_ws_snapshot_t s_pub;

// 以下几项只有 worker 访问，因此无需加锁。
static cyber_ws_session_t s_sess;
static esp_websocket_client_handle_t s_client;
static int64_t s_connect_us;       // 上次开始建连的时刻，用于握手超时

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
        // 服务端可能带上 audio_params；本项目固定 16k/单声道/60ms，
        // 若服务端要求不一致就是协商失败，而不是可以"将就"的情况。
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
            // 二进制即 Opus 帧。本增量还没有解码器，因此明确丢弃并记录——
            // 静默丢弃会让"对方在说话但没声音"变成一个查不出来的现象。
            ESP_LOGD(TAG, "收到 Opus 帧 %u 字节（解码器尚未接线，丢弃）", (unsigned)s_rx_len);
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
    ESP_LOGI(TAG, "链路已拆解，free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
static void on_listen_start(void)
{
    s_want_listen = true;

    if (s_sess.state == CYBER_WS_IDLE || s_sess.state == CYBER_WS_ERROR) {
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
    s_want_listen = false;
    if (s_sess.state == CYBER_WS_LISTENING) {
        send_listen(CYBER_WS_LISTEN_STOP);
        step_publish(CYBER_WS_EV_LISTEN_STOP, NULL);
    }
}

static void on_abort(void)
{
    s_want_listen = false;
    if (s_sess.state == CYBER_WS_LISTENING || s_sess.state == CYBER_WS_SPEAKING) {
        send_abort();
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
        ESP_LOGI(TAG, "对话功能已启用（等待说话请求）");
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
        break;

    default: break;
    }
}

// 握手超时：连上了但迟迟等不到 hello，说明这条链路已经不可用，
// 不主动收场会让状态永远停在 CONNECTING，界面上就是一个转不完的圈。
static void housekeeping(void)
{
    if (s_sess.state != CYBER_WS_CONNECTING) return;
    if (s_connect_us == 0) return;
    if ((esp_timer_get_time() - s_connect_us) < CYBER_WS_HELLO_TIMEOUT_US) return;

    ESP_LOGW(TAG, "握手超时，判定失败");
    s_want_listen = false;
    teardown();
    step_publish(CYBER_WS_EV_FAIL, NULL);
    publish_error(-1);
}

static void ws_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ws_req_t r;
        // 用短超时轮询而非无限等待：同一循环顺带跑握手超时检查。
        if (xQueueReceive(s_req_q, &r, pdMS_TO_TICKS(CYBER_WS_TICK_MS)) == pdTRUE) {
            handle_req(r);
        }
        housekeeping();
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
    if (s_req_q == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    cyber_ws_session_init(&s_sess);
    memset(&s_pub, 0, sizeof(s_pub));
    s_pub.state = CYBER_WS_OFF;

    if (xTaskCreate(ws_worker, "cyber_ws", CYBER_WS_TASK_STACK, NULL,
                    CYBER_WS_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_req_q);
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
