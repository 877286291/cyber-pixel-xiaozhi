// main/cyber_net.c
// Wi-Fi STA 联网层的实现。所有 esp_* 调用集中在这里；判断逻辑在 cyber_net_util.c。
#include "cyber_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "cyber_net_util.h"

static const char *TAG = "cyber_net";

// ---------------------------------------------------------------------------
// 开发期凭证覆盖：文件不进版本库（见 .gitignore）。
// 用 __has_include 而不是让构建强制依赖它，缺失时就是"没有覆盖"。
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("cyber_wifi_dev.h")
#    include "cyber_wifi_dev.h"
#  endif
#endif

#ifndef CYBER_WIFI_DEV_SSID
#  define CYBER_WIFI_DEV_SSID ""
#endif
#ifndef CYBER_WIFI_DEV_PASS
#  define CYBER_WIFI_DEV_PASS ""
#endif

// NVS 凭证键。namespace 用 "cyber" 而不是 Wi-Fi 驱动自己的存储，
// 因为驱动侧用 WIFI_STORAGE_RAM，凭证的唯一事实来源是这里。
#define CYBER_NVS_NAMESPACE  "cyber"
#define CYBER_NVS_KEY_SSID   "ssid"
#define CYBER_NVS_KEY_PASS   "pass"

typedef struct {
    cyber_net_state_cb_t cb;
    void *ctx;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_evt;
    esp_event_handler_instance_t ip_evt;
    esp_timer_handle_t retry;
    uint8_t attempts;          // 连续失败次数，用于退避
    bool inited;
    bool started;
    bool connected;
} cyber_net_ctx_t;

static cyber_net_ctx_t s_net;

// ---------------------------------------------------------------------------
// 内存埋点
// ---------------------------------------------------------------------------
void cyber_net_log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %-20s free=%u internal=%u largest=%u",
             (stage != NULL) ? stage : "?",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// ---------------------------------------------------------------------------
// 凭证
// ---------------------------------------------------------------------------
typedef struct {
    char ssid[CYBER_SSID_MAX_BYTES + 1];
    char pass[CYBER_PASS_MAX_BYTES + 1];
    size_t ssid_len;
    size_t pass_len;
} cyber_creds_t;

// 从 NVS 读凭证。读到非法或缺失返回 false。
static bool load_creds_from_nvs(cyber_creds_t *out)
{
    nvs_handle_t h;
    if (nvs_open(CYBER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t ssid_len = sizeof(out->ssid);
    size_t pass_len = sizeof(out->pass);
    const esp_err_t ssid_err = nvs_get_str(h, CYBER_NVS_KEY_SSID, out->ssid, &ssid_len);
    const esp_err_t pass_err = nvs_get_str(h, CYBER_NVS_KEY_PASS, out->pass, &pass_len);
    nvs_close(h);

    if (ssid_err != ESP_OK) return false;

    // nvs_get_str 写回的 len 含结尾 '\0'，转成实际内容长度。
    out->ssid_len = (ssid_len > 0u) ? ssid_len - 1u : 0u;
    if (pass_err == ESP_OK) {
        out->pass_len = (pass_len > 0u) ? pass_len - 1u : 0u;
    } else {
        // 没有 pass 键 = 开放网络，是合法情况而不是错误。
        out->pass[0] = '\0';
        out->pass_len = 0u;
    }
    return cyber_ssid_valid(out->ssid, out->ssid_len) &&
           cyber_pass_valid(out->pass, out->pass_len);
}

// 解析最终使用的凭证：开发覆盖优先，其次 NVS。
static bool resolve_creds(cyber_creds_t *out)
{
    memset(out, 0, sizeof(*out));

    const size_t dev_ssid_len = strlen(CYBER_WIFI_DEV_SSID);
    if (dev_ssid_len > 0u) {
        const size_t dev_pass_len = strlen(CYBER_WIFI_DEV_PASS);
        if (!cyber_ssid_valid(CYBER_WIFI_DEV_SSID, dev_ssid_len) ||
            !cyber_pass_valid(CYBER_WIFI_DEV_PASS, dev_pass_len)) {
            ESP_LOGE(TAG, "开发覆盖头里的凭证不合法（SSID 需 1..%d 字节，口令需 %d..%d 字节）",
                     CYBER_SSID_MAX_BYTES, CYBER_PASS_MIN_BYTES, CYBER_PASS_MAX_BYTES);
            return false;
        }
        memcpy(out->ssid, CYBER_WIFI_DEV_SSID, dev_ssid_len + 1u);
        memcpy(out->pass, CYBER_WIFI_DEV_PASS, dev_pass_len + 1u);
        out->ssid_len = dev_ssid_len;
        out->pass_len = dev_pass_len;
        ESP_LOGI(TAG, "凭证来源：开发覆盖头");
        return true;
    }

    if (load_creds_from_nvs(out)) {
        ESP_LOGI(TAG, "凭证来源：NVS");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 连接与重连
// ---------------------------------------------------------------------------
static void notify(bool connected)
{
    if (s_net.connected == connected) return;
    s_net.connected = connected;
    if (s_net.cb != NULL) s_net.cb(connected, s_net.ctx);
}

// 定义在后面；retry_cb 需要它来做"同步失败也继续重试"的兜底。
static void schedule_retry(void);

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s_net.started) return;

    ESP_LOGI(TAG, "重连尝试 #%u", (unsigned)s_net.attempts);
    const esp_err_t err = esp_wifi_connect();
    // esp_wifi_connect() 有时会【同步返回错误而不产生任何事件】（例如驱动状态
    // 未就绪）。若只依赖 DISCONNECTED 事件来重试，这种情况下就再也不会有下一次
    // 尝试，连接静默停死。所以这里自己兜底再排一次。
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect 失败: %s，继续退避重试", esp_err_to_name(err));
        schedule_retry();
    }
}

static void schedule_retry(void)
{
    if (!s_net.started) return;
    s_net.attempts = cyber_net_retry_bump(s_net.attempts);

    const uint32_t delay_ms = cyber_net_retry_delay_ms(s_net.attempts);
    if (delay_ms == 0u) return;

    // 先停再启：esp_timer_start_once 对运行中的定时器会返回 INVALID_STATE，
    // 而短时间内连续断开（AP 抖动）是常见情况。
    (void)esp_timer_stop(s_net.retry);
    const esp_err_t err = esp_timer_start_once(s_net.retry, (uint64_t)delay_ms * 1000u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "退避定时器启动失败: %s", esp_err_to_name(err));
    }
}

static void wifi_evt_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        // 栈已就绪，可以发起连接。
        (void)esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "已关联 AP，等待 DHCP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)data;
        // 只打 reason 码不打密码：reason 是排查"密码错/信号弱"的关键信息。
        ESP_LOGW(TAG, "断开 (reason=%d)", (d != NULL) ? (int)d->reason : -1);
        notify(false);
        schedule_retry();
        break;
    }

    default:
        break;
    }
}

static void ip_evt_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    if (ev != NULL) {
        ESP_LOGI(TAG, "已获取 IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
    s_net.attempts = 0;   // 连上了就清空退避，下次断开从最短间隔重试
    // 顺带撤掉可能已排好的重试：不撤的话它到期会再调一次 esp_wifi_connect()，
    // 虽是无害的空操作，但会在日志里留下令人困惑的多余一行。
    (void)esp_timer_stop(s_net.retry);
    notify(true);
    cyber_net_log_heap("wifi connected");
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t cyber_net_init(cyber_net_state_cb_t cb, void *ctx)
{
    if (s_net.inited) return ESP_ERR_INVALID_STATE;

    memset(&s_net, 0, sizeof(s_net));
    s_net.cb = cb;
    s_net.ctx = ctx;

    // 默认事件循环是 Wi-Fi 栈的隐式前置条件，而漏建它的失败极其难读：
    // esp_netif_create_default_wifi_sta() 内部用 ESP_ERROR_CHECK 调
    // esp_wifi_set_default_wifi_sta_handlers()，后者在默认循环上注册处理器，
    // 没有循环就直接 abort，只留下 "ESP_ERR_INVALID_STATE ... set_default_wifi_sta_handlers"。
    // 注意 esp_netif_init() 自己【不】创建该循环（它只初始化 lwip/tcpip），
    // 所以这两步都必须在建 netif 之前显式做掉。
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    // 已存在是正常情况（其他子系统可能先建过），不当错误。
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "默认事件循环创建失败: %s", esp_err_to_name(err));
        return err;
    }

    s_net.netif = esp_netif_create_default_wifi_sta();
    if (s_net.netif == NULL) return ESP_FAIL;

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_evt_cb, NULL, &s_net.wifi_evt);
    if (err != ESP_OK) goto fail_wifi;

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &ip_evt_cb, NULL, &s_net.ip_evt);
    if (err != ESP_OK) goto fail_wifi_evt;

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_cb,
        .name = "cyber_net_retry",
    };
    err = esp_timer_create(&timer_args, &s_net.retry);
    if (err != ESP_OK) goto fail_ip_evt;

    // 凭证由本模块独占管理（见文件头的优先级说明），故驱动侧不落盘，
    // 避免出现"NVS 里有两份凭证、改了一份不生效"的经典困惑。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail_timer;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail_timer;

    s_net.inited = true;
    cyber_net_log_heap("wifi inited");
    return ESP_OK;

    // 失败按"后建先拆"回滚，保证可重试；任何一步漏拆都会让下次 init 失败。
fail_timer:
    esp_timer_delete(s_net.retry);
    s_net.retry = NULL;
fail_ip_evt:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_net.ip_evt);
fail_wifi_evt:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_net.wifi_evt);
fail_wifi:
    esp_wifi_deinit();
fail:
    esp_netif_destroy_default_wifi(s_net.netif);
    s_net.netif = NULL;
    ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t cyber_net_start(void)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;
    if (s_net.started) return ESP_ERR_INVALID_STATE;

    cyber_creds_t creds;
    if (!resolve_creds(&creds)) {
        // 没有凭证不是故障：设备照常开机，界面显示 OFFLINE，
        // 等后续配网功能补上凭证再 start。
        ESP_LOGW(TAG, "无可用 Wi-Fi 凭证，保持未连接（界面显示 OFFLINE）");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, creds.ssid, creds.ssid_len);
    memcpy(cfg.sta.password, creds.pass, creds.pass_len);

    const cyber_wifi_auth_t auth = cyber_wifi_auth_from_pass(creds.pass, creds.pass_len);
    // 阈值取 WPA_PSK 而不是 WPA2_PSK：混用 WPA/WPA2 的老路由也能连上，
    // 收紧到 WPA2 会把这些路由器直接判死，而它们并不少见。
    cfg.sta.threshold.authmode = (auth == CYBER_WIFI_AUTH_OPEN) ? WIFI_AUTH_OPEN
                                                                : WIFI_AUTH_WPA_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;

    // SSID 可以打，密码绝对不打（仓库规范明令禁止记录 Wi-Fi 口令）。
    ESP_LOGI(TAG, "连接 SSID \"%s\"（%s）", creds.ssid,
             (auth == CYBER_WIFI_AUTH_OPEN) ? "开放网络" : "WPA/WPA2");
    // 明文口令清零：它已经进了驱动配置结构，这里没有必要继续留在栈上。
    memset(&creds, 0, sizeof(creds));

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    s_net.started = true;
    s_net.attempts = 0;
    return ESP_OK;
}

esp_err_t cyber_net_stop(void)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;

    // 先摘回调再拆栈：否则拆的过程中触发的断开事件会去跑已经失效的路径。
    if (s_net.started) {
        (void)esp_timer_stop(s_net.retry);
        // 先置 false 再 stop：esp_wifi_stop() 会触发 WIFI_EVENT_STA_DISCONNECTED，
        // 若此时 started 还是 true，事件处理里会顺手排一次重连定时器，
        // 而我们紧接着就把它删了 —— 属于"自己给自己制造竞态"。
        s_net.started = false;
        (void)esp_wifi_stop();
    }
    if (s_net.retry != NULL) {
        esp_timer_delete(s_net.retry);
        s_net.retry = NULL;
    }
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_net.ip_evt);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_net.wifi_evt);
    esp_wifi_deinit();
    if (s_net.netif != NULL) {
        esp_netif_destroy_default_wifi(s_net.netif);
        s_net.netif = NULL;
    }

    s_net.inited = false;
    notify(false);
    return ESP_OK;
}

bool cyber_net_is_connected(void)
{
    return s_net.connected;
}

esp_err_t cyber_net_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CYBER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(h, CYBER_NVS_KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // 本来就没有，视为已清除
    if (err == ESP_OK) {
        const esp_err_t pass_err = nvs_erase_key(h, CYBER_NVS_KEY_PASS);
        if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) err = pass_err;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) ESP_LOGI(TAG, "已清除 NVS 中的 Wi-Fi 凭证");
    return err;
}
