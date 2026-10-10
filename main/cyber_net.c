// main/cyber_net.c
// Wi-Fi STA 联网层的实现。所有 esp_* 调用集中在这里；判断逻辑在 cyber_net_util.c。
#include "cyber_net.h"

#include <stdio.h>
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
    bool have_creds;           // 上次 start 时是否解析到可用凭证（错误页据此区分"没配置"）
    uint8_t last_reason;       // 最近一次断开/失败的 Wi-Fi reason 码（错误页据此给出原因）
    char ip[16];               // 已获取的 IPv4 字符串；未连接时为 ""
} cyber_net_ctx_t;

static cyber_net_ctx_t s_net;

// ---------------------------------------------------------------------------
// 配网模式
// ---------------------------------------------------------------------------
// 配网期间设备本来就没有可用凭证，若仍走常规路径，"连接失败→退避→重试"会刷屏，
// 且可能与扫描互相干扰。这个标志只影响两条自动行为：STA_START 的自动连接、
// 以及 STA_DISCONNECTED 的退避重连——扫描与手动连接不受影响。
static bool s_prov_mode;

// 扫描结果回调只能在事件循环任务里调用，故只存一个，不排队。
static cyber_net_scan_cb_t s_scan_cb;
static void *s_scan_ctx;

// 扫描结果的定长缓冲。刻意用静态数组而不是每次 malloc：扫描是串行的，且这段代码
// 跑在事件循环任务上，能不在那里做堆分配就不做。上限与配网页列出的 AP 数一致。
#define CYBER_NET_SCAN_MAX 16
static wifi_ap_record_t s_scan_recs[CYBER_NET_SCAN_MAX];

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
        // 栈已就绪，可以发起连接。配网模式下不自动连：此时没有可用凭证，
        // 连了只会立刻失败并触发一轮无意义的退避。
        if (!s_prov_mode) {
            (void)esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "已关联 AP，等待 DHCP");
        break;

    case WIFI_EVENT_SCAN_DONE: {
        // 扫描结果只在此刻有效：取到缓冲后立刻回调，回调返回即失效。
        // 无论有没有回调都要把驱动侧的记录取走，否则下一次扫描会返回
        // "still scanning" 或拿到陈旧结果。
        uint16_t n = CYBER_NET_SCAN_MAX;
        const esp_err_t err = esp_wifi_scan_get_ap_records(&n, s_scan_recs);
        const cyber_net_scan_cb_t cb = s_scan_cb;
        void *ctx = s_scan_ctx;
        s_scan_cb = NULL;
        s_scan_ctx = NULL;
        if (cb != NULL) {
            if (err == ESP_OK) {
                cb(s_scan_recs, (unsigned)n, ctx);
            } else {
                // 取记录失败：以 count=0 回调，让上层回一个"扫描失败"而不是干等。
                ESP_LOGW(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
                cb(NULL, 0u, ctx);
            }
        }
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)data;
        // 只打 reason 码不打密码：reason 是排查"密码错/信号弱"的关键信息。
        s_net.last_reason = (d != NULL) ? d->reason : 0u;
        s_net.ip[0] = '\0';   // 断开后 IP 不再代表现状
        ESP_LOGW(TAG, "断开 (reason=%d)", (d != NULL) ? (int)d->reason : -1);
        notify(false);
        // 配网模式下不重连：此刻的断开是预期内的（还没配好网），
        // 按退避重试只会把日志刷满，并可能在扫描时抢走射频。
        if (!s_prov_mode) {
            schedule_retry();
        }
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
        // 存下点分十进制串：配网页要把它显示出来，作为"确实配成功了"的证据。
        snprintf(s_net.ip, sizeof(s_net.ip), IPSTR, IP2STR(&ev->ip_info.ip));
    }
    s_net.attempts = 0;   // 连上了就清空退避，下次断开从最短间隔重试
    s_net.last_reason = 0;   // 连上了，上一次失败的原因不再代表现状
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

// 用当前凭证（开发覆盖头优先，其次 NVS）配置 STA。不启动、不连接。
// 抽出来是为了让"常规启动"与"配网提交"共用同一套配置逻辑：两处各写一份必然漂移，
// 而漂移的表现是"配网存进去的密码，常规路径连不上"，最难查。
static esp_err_t apply_sta_config(void)
{
    cyber_creds_t creds;
    if (!resolve_creds(&creds)) {
        // 没有凭证不是故障：设备照常开机，界面显示 OFFLINE，
        // 等配网功能补上凭证再 start。
        s_net.have_creds = false;
        ESP_LOGW(TAG, "无可用 Wi-Fi 凭证，保持未连接（界面显示 OFFLINE）");
        return ESP_ERR_NOT_FOUND;
    }
    s_net.have_creds = true;

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

    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;

    // SSID 可以打，密码绝对不打（仓库规范明令禁止记录 Wi-Fi 口令）。
    ESP_LOGI(TAG, "连接 SSID \"%s\"（%s）", creds.ssid,
             (auth == CYBER_WIFI_AUTH_OPEN) ? "开放网络" : "WPA/WPA2");
    // 明文口令清零：它已经进了驱动配置结构，这里没有必要继续留在栈上。
    memset(&creds, 0, sizeof(creds));
    return ESP_OK;
}

esp_err_t cyber_net_start(void)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;
    if (s_net.started) return ESP_ERR_INVALID_STATE;

    const esp_err_t err = apply_sta_config();
    if (err != ESP_OK) return err;

    const esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) return start_err;

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

void cyber_net_get_status(cyber_net_status_t *out)
{
    if (out == NULL) return;
    out->connected = s_net.connected;
    out->have_creds = s_net.have_creds;
    out->last_reason = s_net.last_reason;
    out->attempts = s_net.attempts;
    memcpy(out->ip, s_net.ip, sizeof(out->ip));
}

esp_err_t cyber_net_kick(void)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;

    // 还没启动过（通常是开机时没凭证）：再试一次 start，好让刚写入的新凭证生效。
    if (!s_net.started) {
        ESP_LOGI(TAG, "手动重试：重新解析凭证并启动");
        return cyber_net_start();
    }

    // 已启动：清空退避 + 撤掉待发的重试，立刻发起一次连接。
    // 不清退避的话，用户按了之后仍要等最多 15s 才会轮到下一个退避点，
    // 观感上就是"按了没反应"。
    s_net.attempts = 0;
    (void)esp_timer_stop(s_net.retry);
    ESP_LOGI(TAG, "手动重试：立刻重连");
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // 同步失败只会在这一个点出现（驱动状态未就绪）；排一次退避兜底，
        // 与 retry_cb 同一套逻辑，避免重连静默停死。
        ESP_LOGW(TAG, "手动重连失败: %s，转入退避", esp_err_to_name(err));
        schedule_retry();
    }
    return err;
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

// ---------------------------------------------------------------------------
// 配网
// ---------------------------------------------------------------------------
// 把配网收到的凭证写进 NVS（namespace "cyber"，与读取侧同一套键）。
//
// 为什么不写进 esp_wifi 自己的 flash 存储：本模块刻意用 WIFI_STORAGE_RAM，
// 让"凭证的唯一事实来源"就是这里。两处各存一份必然出现"改了 A 不生效"的经典困惑。
static esp_err_t save_creds_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CYBER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, CYBER_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        if (pass != NULL && pass[0] != '\0') {
            err = nvs_set_str(h, CYBER_NVS_KEY_PASS, pass);
        } else {
            // 开放网络：把可能存在的旧口令删掉，否则上一次的密码会被配到开放网络上，
            // 表现为"明明是不加密的网络却连不上"。
            const esp_err_t pass_err = nvs_erase_key(h, CYBER_NVS_KEY_PASS);
            if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) err = pass_err;
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    // 安全：只打 SSID，绝不打口令（仓库规范明令禁止记录 Wi-Fi 口令）。
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配网凭证已写入 NVS（SSID \"%s\"）", ssid);
    } else {
        ESP_LOGE(TAG, "配网凭证写入 NVS 失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cyber_net_prov_begin(void)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;

    // 先置位再动栈：esp_wifi_start() 会同步触发 STA_START，事件回调要靠它决定不自动连。
    s_prov_mode = true;
    s_net.attempts = 0;
    (void)esp_timer_stop(s_net.retry);

    if (!s_net.started) {
        // 没有凭证时也要把 Wi-Fi 起起来——扫描需要它。此时不配置 STA、不连接
        // （s_prov_mode 已让 STA_START 不再自动连）。
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            s_prov_mode = false;
            return err;
        }
        s_net.started = true;
    }
    ESP_LOGI(TAG, "进入配网模式");
    return ESP_OK;
}

esp_err_t cyber_net_prov_scan(cyber_net_scan_cb_t cb, void *ctx)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;
    if (!s_prov_mode) return ESP_ERR_INVALID_STATE;

    s_scan_cb = cb;
    s_scan_ctx = ctx;

    const wifi_scan_config_t cfg = { 0 };
    // 非阻塞：结果经 WIFI_EVENT_SCAN_DONE → 回调。阻塞在这里会卡住调用方
    // （配网的调用方是事件循环任务）。
    const esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        s_scan_cb = NULL;
        s_scan_ctx = NULL;
        ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cyber_net_prov_commit(const char *ssid, const char *pass)
{
    if (!s_net.inited) return ESP_ERR_INVALID_STATE;
    if (ssid == NULL) return ESP_ERR_INVALID_ARG;

    // 复用 cyber_net_util 的长度约束，不在这里重写一遍。
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = (pass != NULL) ? strlen(pass) : 0u;
    if (!cyber_ssid_valid(ssid, ssid_len)) return ESP_ERR_INVALID_ARG;
    if (!cyber_pass_valid(pass, pass_len)) return ESP_ERR_INVALID_ARG;

    const esp_err_t save_err = save_creds_to_nvs(ssid, pass);
    if (save_err != ESP_OK) return save_err;

    // 提交即退出配网模式：随后的自动连接与退避重连必须恢复正常语义。
    s_prov_mode = false;
    s_scan_cb = NULL;
    s_scan_ctx = NULL;

    esp_err_t err = apply_sta_config();
    if (err != ESP_OK) return err;

    if (!s_net.started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_net.started = true;
    } else {
        // 配网期间 Wi-Fi 已为扫描而启动：直接发起连接。
        err = esp_wifi_connect();
    }

    s_net.attempts = 0;
    s_net.last_reason = 0;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配网完成，开始连接热点");
    } else {
        // 同步失败不致命：退避重连会继续尝试。
        ESP_LOGW(TAG, "配网后立即连接失败: %s，转入退避", esp_err_to_name(err));
        schedule_retry();
    }
    return err;
}

void cyber_net_prov_end(void)
{
    s_scan_cb = NULL;
    s_scan_ctx = NULL;

    // 有凭证说明这次配网已提交（或本来就配好了）：无需停栈，连接要靠它。
    if (!s_net.started || s_net.have_creds) {
        s_prov_mode = false;
        return;
    }

    // 仍无可用凭证：把 Wi-Fi 停下来，避免配网取消后一直空转。
    // 先保持 s_prov_mode=true 再 stop——否则 stop 触发的断开事件会顺手排一次重连。
    (void)esp_timer_stop(s_net.retry);
    (void)esp_wifi_stop();
    s_net.started = false;
    s_prov_mode = false;
    ESP_LOGI(TAG, "退出配网模式（未提交凭证，Wi-Fi 已停止）");
}
