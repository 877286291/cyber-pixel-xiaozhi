// main/cyber_ota.c
// 小智服务激活客户端的 ESP-IDF 实现。
//
// 职责：做一次 HTTPS 检查、解析响应、把配置落 NVS、把状态发布给 UI 任务。
// 请求体的构造与结果判定刻意放在 cyber_ota_util.c（纯逻辑、宿主可测），
// 本文件只处理"网络、解析、持久化"这三件必须依赖 ESP-IDF 的事。
//
// 规格见 docs/development/engineering/xiaozhi-activation.md。
#include "cyber_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "cyber_ota";

// 内存埋点：TLS 会话是本应用目前最大的单次内部内存消费者，
// 这里的数字是后续接入 Opus 时核算预算的基准。
static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %-16s free=%u min=%u largest=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// 身份常量。请求体与 User-Agent 共用同一处定义：
// 规格文档里最容易被踩的坑就是二者漂移，共用宏从源头消除这种可能。
#define CYBER_OTA_BOARD_TYPE   "cyber-pixel-xiaozhi"
#define CYBER_OTA_APP_VERSION  "0.1.0"

// 服务端默认端点；可用 NVS cyber/ota_url 覆盖（规格要求端点必须可运行时配置）。
#define CYBER_OTA_DEFAULT_URL  "https://api.tenclass.net/xiaozhi/ota/"
#define CYBER_OTA_NVS_NS       "cyber"
#define CYBER_OTA_KEY_URL      "ota_url"
#define CYBER_OTA_KEY_CLIENT   "client_id"
#define CYBER_OTA_KEY_WS_URL   "ws_url"
#define CYBER_OTA_KEY_WS_TOKEN "ws_token"
#define CYBER_OTA_KEY_PAIRED   "paired"

// 响应上限。实测响应约 600 字节；规格要求"按可接受响应上限确定解析容量，
// 并在解析前拒掉超大响应"，这样既不会因为容量不足而解析失败，也不会被
// 一个异常巨大的响应拖垮内存。4096 给足了协议增长余量。
#define CYBER_OTA_RESP_CAP     4096
#define CYBER_OTA_BODY_CAP      640
#define CYBER_OTA_HTTP_TIMEOUT_MS 10000

// worker 栈。TLS 握手是本任务里最深的调用链，8KB 是留给 mbedTLS 的余量。
#define CYBER_OTA_TASK_STACK   8192
#define CYBER_OTA_TASK_PRIO    4

// 连续失败上限：到这里就报 ERROR 并停下，等下一次联网事件重新驱动。
// 无限重试会让一块连不上服务端的设备在后台永久空转，白耗电。
#define CYBER_OTA_MAX_FAILS    5
// 已就绪后重新检查的间隔：token 可能轮换，但没必要频繁问。
#define CYBER_OTA_READY_RECHECK_MS (30u * 60u * 1000u)

// ---------------------------------------------------------------------------
// 发布通道：worker 写、任意任务读，一份小快照
// ---------------------------------------------------------------------------
static portMUX_TYPE s_pub_lock = portMUX_INITIALIZER_UNLOCKED;
static cyber_ota_snapshot_t s_pub;

// "曾经绑定成功"：跨重启保留，一旦为真不再变假。只在 worker 任务里写。
static bool s_paired;

static void publish(cyber_ota_snapshot_t *s)
{
    // paired 由发布通道统一补上，而不是让每个发布点各记一遍：漏记一处就会让
    // 一台已绑定的设备在某个瞬间被界面判成"未绑定"，那正是这个字段要避免的事。
    s->paired = s_paired;
    // 临界区里只做一次定长结构赋值（几十字节），不会长时间关中断。
    taskENTER_CRITICAL(&s_pub_lock);
    s_pub = *s;
    taskEXIT_CRITICAL(&s_pub_lock);
}

void cyber_ota_get_snapshot(cyber_ota_snapshot_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL(&s_pub_lock);
    *out = s_pub;
    taskEXIT_CRITICAL(&s_pub_lock);
}

// 联网标志：由 LVGL 任务写，worker 读。单字节对齐写是原子的，且无读-改-写，
// 因此不需要锁（与 main.c 里 s_net_connected 同一个理由）。
static volatile bool s_net_up;
static volatile bool s_net_seen;  // worker 是否已首次看到联网，用于触发一次检查

void cyber_ota_notify_net(bool connected)
{
    s_net_up = connected;
}

// ---------------------------------------------------------------------------
// NVS 小工具
// ---------------------------------------------------------------------------
static esp_err_t nvs_read_str(const char *key, char *out, size_t cap)
{
    if (cap == 0u) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CYBER_OTA_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = cap;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CYBER_OTA_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Client-Id：生成一次就持久化。重新生成会让服务端把它当成一台新设备，
// 从而丢掉此前的绑定，所以必须复用 NVS 里的值。
static bool load_or_create_client_id(char *out, size_t cap)
{
    if (nvs_read_str(CYBER_OTA_KEY_CLIENT, out, cap) == ESP_OK && out[0] != '\0') {
        return true;
    }

    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t r = esp_random();
        b[i + 0] = (uint8_t)(r);
        b[i + 1] = (uint8_t)(r >> 8);
        b[i + 2] = (uint8_t)(r >> 16);
        b[i + 3] = (uint8_t)(r >> 24);
    }
    b[6] = (uint8_t)((b[6] & 0x0Fu) | 0x40u);  // version 4
    b[8] = (uint8_t)((b[8] & 0x3Fu) | 0x80u);  // RFC 4122 variant

    const int n = snprintf(out, cap,
                           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    if (n < 0 || (size_t)n >= cap) return false;

    // 写失败不致命：本次仍能用这个 id 完成检查，只是下次开机要重新生成。
    const esp_err_t err = nvs_write_str(CYBER_OTA_KEY_CLIENT, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Client-Id 写入 NVS 失败: %s（本次仍继续）", esp_err_to_name(err));
    }
    return true;
}

static void get_endpoint(char *out, size_t cap)
{
    if (nvs_read_str(CYBER_OTA_KEY_URL, out, cap) == ESP_OK && out[0] != '\0') {
        return;
    }
    snprintf(out, cap, "%s", CYBER_OTA_DEFAULT_URL);
}

// STA MAC。必须与请求体里的 MAC 同源：不一致会被服务端回 "Invalid OTA request"。
static bool get_sta_mac(char *out, size_t cap)
{
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return false;
    const int n = snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return n > 0 && (size_t)n < cap;
}

// ---------------------------------------------------------------------------
// 响应解析 → 纯逻辑判定 → 持久化
// ---------------------------------------------------------------------------
typedef struct {
    bool has_code;
    char code[CYBER_OTA_CODE_CAP];
    bool has_ws;
    char ws_url[192];
    char ws_token[160];
    bool test_creds;
} parsed_t;

// 把 cJSON 字符串字段拷进定长缓冲；字段缺失或类型不符时保持原值不变。
static void copy_if_string(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        snprintf(out, cap, "%s", it->valuestring);
    }
}

static void parse_response(const char *json, parsed_t *p)
{
    memset(p, 0, sizeof(*p));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return;   // 交给调用方按"无可用配置"处理

    const cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (cJSON_IsObject(act)) {
        copy_if_string(act, "code", p->code, sizeof(p->code));
        // 只接受合法激活码：服务端给个畸形值就当作没有激活码，
        // 免得把一个注定输不对的字符串显示给用户。
        p->has_code = cyber_ota_code_valid(p->code);
        if (!p->has_code) p->code[0] = '\0';
    }

    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (cJSON_IsObject(ws)) {
        copy_if_string(ws, "url", p->ws_url, sizeof(p->ws_url));
        copy_if_string(ws, "token", p->ws_token, sizeof(p->ws_token));
        p->has_ws = (p->ws_url[0] != '\0') && (p->ws_token[0] != '\0');
        p->test_creds = cyber_ota_is_test_token(p->ws_token);
    }

    if (!p->test_creds) {
        const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
        if (cJSON_IsObject(mqtt)) {
            char cid[160] = {0};
            copy_if_string(mqtt, "client_id", cid, sizeof(cid));
            p->test_creds = cyber_ota_is_test_client_id(cid);
        }
    }

    cJSON_Delete(root);
}

// 响应体收集器。必须用事件回调接，不能事后调用 esp_http_client_read_response()：
// esp_http_client_perform() 内部已经把响应体读掉，事后再读只会拿到 0 字节，
// 症状正是"HTTP 200 却判为无可用配置"。
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} resp_ctx_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    resp_ctx_t *rc = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rc != NULL && evt->data != NULL) {
        // 有界累积：超过 cap 只置溢出标志，绝不越界写。
        // 这条边界同时覆盖了"长度未知的分块响应"，比只看 content-length 更稳。
        if (rc->len + (size_t)evt->data_len < rc->cap) {
            memcpy(rc->buf + rc->len, evt->data, (size_t)evt->data_len);
            rc->len += (size_t)evt->data_len;
            rc->buf[rc->len] = '\0';
        } else {
            rc->overflow = true;
        }
    }
    return ESP_OK;
}

// 记下"已绑定"。这个事实要跨重启保留，否则一台绑好的设备在下次开机、首次检查
// 返回之前，会与"从未绑过"完全无法区分：两者都是 IDLE/CHECKING，界面只能靠它
// 决定该显示配对页还是主界面。
static void mark_paired(void)
{
    if (s_paired) return;
    s_paired = true;
    if (nvs_write_str(CYBER_OTA_KEY_PAIRED, "1") != ESP_OK) {
        ESP_LOGW(TAG, "绑定标记写入 NVS 失败；本次仍按已绑定处理");
    }
}

// ---------------------------------------------------------------------------
// 一次完整的检查
// ---------------------------------------------------------------------------
static cyber_ota_snapshot_t do_check(void)
{
    cyber_ota_snapshot_t snap = { .state = CYBER_OTA_STATE_ERROR };

    char mac[24];
    char client_id[48];
    char url[224];
    if (!get_sta_mac(mac, sizeof(mac))) {
        ESP_LOGE(TAG, "取 STA MAC 失败");
        return snap;
    }
    if (!load_or_create_client_id(client_id, sizeof(client_id))) {
        ESP_LOGE(TAG, "Client-Id 不可用");
        return snap;
    }
    get_endpoint(url, sizeof(url));

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) flash_size = 0;

    const cyber_ota_body_t body_in = {
        .mac = mac,
        .board_type = CYBER_OTA_BOARD_TYPE,
        .app_name = CYBER_OTA_BOARD_TYPE,
        .app_version = CYBER_OTA_APP_VERSION,
        .idf_version = "v5.5.3",   // 与 sdkconfig.defaults 声明的 IDF 版本一致
        .flash_size = flash_size,
        .min_free_heap = esp_get_minimum_free_heap_size(),
    };
    char body[CYBER_OTA_BODY_CAP];
    const size_t body_len = cyber_ota_build_body(body, sizeof(body), &body_in);
    if (body_len == 0u) {
        ESP_LOGE(TAG, "请求体构造失败（字段非法或缓冲不足）");
        return snap;
    }

    // 响应缓冲与收集器必须在 cfg 之前声明：cfg 的 user_data 指向它。
    static char resp[CYBER_OTA_RESP_CAP];
    resp_ctx_t rc = { .buf = resp, .cap = sizeof(resp), .len = 0u, .overflow = false };

    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CYBER_OTA_HTTP_TIMEOUT_MS,
        // 缓冲刻意压小：TLS 已经吃掉一大块内部 RAM，响应实测仅几百字节，
        // 没必要为它留默认的 4KB。
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .event_handler = http_evt,
        .user_data = &rc,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        ESP_LOGE(TAG, "http client 创建失败");
        return snap;
    }
    log_heap("ota pre-check");

    esp_err_t err = ESP_OK;
    int status = 0;
    int content_len = -1;

    do {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_header(cli, "Activation-Version", "1");  // 本板无 eFuse 序列号
        esp_http_client_set_header(cli, "Device-Id", mac);
        esp_http_client_set_header(cli, "Client-Id", client_id);
        esp_http_client_set_header(cli, "User-Agent", CYBER_OTA_BOARD_TYPE "/" CYBER_OTA_APP_VERSION);
        esp_http_client_set_header(cli, "Accept-Language", "zh-CN");
        esp_http_client_set_post_field(cli, body, (int)body_len);

        err = esp_http_client_perform(cli);
        status = esp_http_client_get_status_code(cli);
        content_len = esp_http_client_get_content_length(cli);
        snap.last_http_status = status;

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "检查请求失败: %s", esp_err_to_name(err));
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "检查返回 HTTP %d", status);
            break;
        }
        // 响应体已在事件回调里有界累积。溢出即等价于"响应过大"，
        // 且这条边界对长度未知的分块响应同样成立（比只看 content-length 更稳）。
        if (rc.overflow) {
            ESP_LOGW(TAG, "响应超过 %u 字节上限，拒绝解析", (unsigned)CYBER_OTA_RESP_CAP);
            break;
        }
        if (rc.len == 0u) {
            ESP_LOGW(TAG, "响应体为空 (HTTP %d, content-length %d)", status, content_len);
            break;
        }

        parsed_t p;
        parse_response(rc.buf, &p);
        // 把判定输入原样打出来：规格已确认服务端并非每轮都带回 activation，
        // 排查"屏幕上是空框"时，第一个要看的就是这一轮到底有没有 code。
        ESP_LOGI(TAG, "响应解析：code=%s websocket=%s 临时凭证=%s 曾绑定=%s",
                 p.has_code ? p.code : "(无)",
                 p.has_ws ? "有" : "无",
                 p.test_creds ? "是" : "否",
                 s_paired ? "是" : "否");

        switch (cyber_ota_decide(p.has_code, p.has_ws, p.test_creds)) {
        case CYBER_OTA_NEED_ACTIVATION:
            // 未绑定：服务端同时下发临时凭证与激活码。此时显示激活码，
            // 由用户去控制台绑定；临时账号只用来让日志说清现状。
            snprintf(snap.code, sizeof(snap.code), "%s", p.code);
            snap.state = CYBER_OTA_STATE_NEED_ACTIVATION;
            snap.test_credentials = p.test_creds;
            ESP_LOGI(TAG, "需要激活：code=%s (临时账号=%s)",
                     snap.code, p.test_creds ? "是" : "否");
            break;

        case CYBER_OTA_READY:
            snap.state = CYBER_OTA_STATE_READY;
            snap.test_credentials = p.test_creds;
            // 只持久化真正拿到的字段；写失败不改变"已就绪"的判定。
            if (nvs_write_str(CYBER_OTA_KEY_WS_URL, p.ws_url) != ESP_OK ||
                nvs_write_str(CYBER_OTA_KEY_WS_TOKEN, p.ws_token) != ESP_OK) {
                ESP_LOGW(TAG, "对话配置写入 NVS 失败（本次仍按就绪处理）");
            }
            ESP_LOGI(TAG, "已就绪：ws=%s", p.ws_url);
            mark_paired();
            break;

        case CYBER_OTA_TEST_ONLY:
            // 服务端不再要求激活、又给了可用端点 → 绑定已经生效，只是它仍然只返回
            // 临时凭证（#1785，实测于本设备）。设备照常进主界面，但这条日志必须
            // 一眼可见：否则排查者只会看到"能用但走的是共享默认智能体"。
            snap.state = CYBER_OTA_STATE_TEST_ONLY;
            snap.test_credentials = true;
            if (nvs_write_str(CYBER_OTA_KEY_WS_URL, p.ws_url) != ESP_OK ||
                nvs_write_str(CYBER_OTA_KEY_WS_TOKEN, p.ws_token) != ESP_OK) {
                ESP_LOGW(TAG, "对话配置写入 NVS 失败（本次仍按就绪处理）");
            }
            ESP_LOGW(TAG, "服务端不再要求激活，判为已绑定；但返回的仍是临时测试凭证"
                          "（#1785），对话走的可能是共享默认智能体：ws=%s", p.ws_url);
            mark_paired();
            break;

        case CYBER_OTA_NO_CONFIG:
        default:
            ESP_LOGW(TAG, "响应里既无 activation 也无 websocket，判为无可用配置");
            break;
        }
    } while (0);

    esp_http_client_cleanup(cli);
    log_heap("ota post-check");
    return snap;
}

// ---------------------------------------------------------------------------
// worker：跟着联网状态走
// ---------------------------------------------------------------------------
static void ota_worker(void *arg)
{
    (void)arg;
    uint8_t poll_attempt = 0;
    int fails = 0;
    // 最近一次成功拿到的激活码。仅用于在一次网络抖动时把码留在屏幕上。
    char last_code[CYBER_OTA_CODE_CAP] = {0};

    for (;;) {
        if (!s_net_up) {
            // 掉线：清掉激活码，否则界面会一直显示一个已经失效的码。
            if (s_net_seen) {
                s_net_seen = false;
                last_code[0] = '\0';
                cyber_ota_snapshot_t idle = { .state = CYBER_OTA_STATE_IDLE };
                publish(&idle);
            }
            poll_attempt = 0;
            fails = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_net_seen) {
            s_net_seen = true;
            cyber_ota_snapshot_t checking = { .state = CYBER_OTA_STATE_CHECKING };
            publish(&checking);
        }

        cyber_ota_snapshot_t snap = do_check();

        if (snap.code[0] != '\0') {
            snprintf(last_code, sizeof(last_code), "%s", snap.code);
        } else if (snap.state == CYBER_OTA_STATE_NEED_ACTIVATION ||
                   snap.state == CYBER_OTA_STATE_ERROR) {
            // 抖动一次就把 6 位码从屏幕上抹掉，用户得重新等下一轮，而码本身在
            // 服务端并没有变（实测码以 MAC 为键、跨轮次恒定）。所以"还在等绑定"
            // 或"这一轮失败"时，都沿用上一轮已知的码。
            snprintf(snap.code, sizeof(snap.code), "%s", last_code);
        } else if (snap.state == CYBER_OTA_STATE_READY ||
                   snap.state == CYBER_OTA_STATE_TEST_ONLY) {
            // 已绑定：旧码作废。留着它会在日后解绑/重新激活时显示一个过期的码。
            last_code[0] = '\0';
        }

        if (snap.state == CYBER_OTA_STATE_READY) {
            publish(&snap);
            // 已就绪：低频复查（token 可能轮换），期间仍然对掉线保持敏感。
            const int steps = (int)(CYBER_OTA_READY_RECHECK_MS / 500u);
            for (int i = 0; i < steps && s_net_up; i++) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            poll_attempt = 0;
            fails = 0;
            continue;
        }

        if (snap.state == CYBER_OTA_STATE_TEST_ONLY) {
            // 绑定已生效，但服务端仍只给临时凭证（#1785）。设备可用，不需要用户
            // 操作；比 READY 勤一点复查，是为了在服务端换成真凭证、或用户在控制台
            // 解绑/换绑时尽快跟上。这【不是】网络失败，不计入失败次数。
            publish(&snap);
            fails = 0;
            for (int i = 0; i < 120 && s_net_up; i++) {   // 60s
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        if (snap.state == CYBER_OTA_STATE_NEED_ACTIVATION) {
            // 用户正在屏幕前输码：节奏要"勤"，但不要勤到白耗电。
            publish(&snap);
            fails = 0;
            poll_attempt = cyber_ota_poll_bump(poll_attempt);
            const uint32_t delay = cyber_ota_poll_delay_ms(poll_attempt);
            for (uint32_t waited = 0; waited < delay && s_net_up;
                 waited += 250u) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            continue;
        }

        // 失败：只把状态降级。激活码已在上面沿用上一轮的值，屏幕不会闪一下空。
        publish(&snap);
        if (++fails >= CYBER_OTA_MAX_FAILS) {
            ESP_LOGW(TAG, "连续失败 %d 次，暂停；等下一次联网事件", fails);
            while (s_net_up) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            fails = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t cyber_ota_init(void)
{
    static bool inited;
    if (inited) return ESP_OK;

    // 先把"曾绑定过"读回来，再发布首个快照：发布之后再补，界面已经拿着
    // 一份错误的 IDLE 做过一次切屏判断了。
    char paired[4] = {0};
    s_paired = (nvs_read_str(CYBER_OTA_KEY_PAIRED, paired, sizeof(paired)) == ESP_OK) &&
               (paired[0] != '\0');

    cyber_ota_snapshot_t idle = { .state = CYBER_OTA_STATE_IDLE };
    publish(&idle);

    if (xTaskCreate(ota_worker, "cyber_ota", CYBER_OTA_TASK_STACK, NULL,
                    CYBER_OTA_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker 创建失败");
        return ESP_FAIL;
    }
    inited = true;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 对话端点配置（供 cyber_ws 握手使用）
// ---------------------------------------------------------------------------
// 这些键由本模块写（激活响应里解析出来），因此也只有本模块知道它们的拼写。
// 让 cyber_ws 自己再写一遍 "ws_url"/"ws_token" 就是给键名漂移留了口子。
esp_err_t cyber_ota_get_ws_config(char *url, size_t url_cap,
                                  char *token, size_t token_cap)
{
    if (url == NULL || token == NULL || url_cap == 0u || token_cap == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    // 先清空：NVS 里没有值时调用方不会读到上一次循环遗留的旧凭证。
    url[0] = '\0';
    token[0] = '\0';
    nvs_read_str(CYBER_OTA_KEY_WS_URL, url, url_cap);
    nvs_read_str(CYBER_OTA_KEY_WS_TOKEN, token, token_cap);
    return (url[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t cyber_ota_get_device_mac(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    return get_sta_mac(out, cap) ? ESP_OK : ESP_FAIL;
}

esp_err_t cyber_ota_get_client_id(char *out, size_t cap)
{
    if (out == NULL || cap == 0u) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    // 复用同一个生成/持久化路径：握手头与 OTA 请求体必须是同一个 id，
    // 否则服务端会把同一台设备认成两台。
    return load_or_create_client_id(out, cap) ? ESP_OK : ESP_FAIL;
}
