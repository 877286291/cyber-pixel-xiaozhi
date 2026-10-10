// main/cyber_prov.c —— 见 cyber_prov.h 的分工、线程与内存约束。
//
// 结构对齐 FoloToy/ai-passport demo/blufi-provisioning 的 demo_blufi.c（那是官方验证
// 过的 BLUFI 流程），差异只在两处：
//   1. 不自建 Wi-Fi 栈——扫描与连接借用 cyber_net（Wi-Fi 与凭证的唯一归属）；
//   2. 不向界面推事件——界面轮询 cyber_prov_get_status()（避免跨任务碰 LVGL）。
#include "cyber_prov.h"

#include <string.h>

#include "cyber_net.h"
#include "cyber_prov_security.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "cyber_prov";

// 手机侧列表能一次展示的 AP 数，与 cyber_net 的扫描上限一致。
#define PROV_AP_LIST_COUNT 16

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
// 这些标志只被 NimBLE/BTC 任务与 BT 控制任务写；界面只读。单个 bool 的对齐写在本
// 平台上原子，且每个标志只有"置位/清零"这一种写，不存在读-改-写，故无需加锁。
static volatile bool s_started;        // BLE 栈已起
static volatile bool s_ble_connected;  // 手机已连上
static volatile bool s_committed;      // 已提交凭证、开始连接热点
static volatile bool s_failed;         // 启动或协商失败

// 控件与栈句柄（只在 BT 控制任务里用，不需要 volatile）。
static bool s_host_initialized;
static bool s_host_running;
static bool s_gatt_initialized;
static bool s_btc_initialized;
static bool s_profile_initialized;
static SemaphoreHandle_t s_host_stopped;

// 累积中的凭证。只在 BTC 任务里读写。
static cyber_prov_creds_t s_creds;

// ---------------------------------------------------------------------------
// 上报
// ---------------------------------------------------------------------------
static void send_wifi_report(esp_blufi_sta_conn_state_t state)
{
    esp_blufi_extra_info_t info = { 0 };
    if (s_creds.has_ssid) {
        info.sta_ssid = (uint8_t *)s_creds.ssid;
        info.sta_ssid_len = (int)s_creds.ssid_len;
    }
    // mode 固定 STA：本设备只做 station，不上报 AP 模式。
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, state, 0, &info);
}

// 扫描结果回调（运行在 Wi-Fi 事件任务）。records 是 wifi_ap_record_t 数组。
static void on_scan_done(const void *records, unsigned count, void *ctx)
{
    (void)ctx;
    if (!s_ble_connected) return;
    if (records == NULL) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    const wifi_ap_record_t *recs = (const wifi_ap_record_t *)records;
    static esp_blufi_ap_record_t list[PROV_AP_LIST_COUNT];
    const unsigned n = (count > PROV_AP_LIST_COUNT) ? PROV_AP_LIST_COUNT : count;
    for (unsigned i = 0; i < n; i++) {
        list[i].rssi = recs[i].rssi;
        memcpy(list[i].ssid, recs[i].ssid, sizeof(list[i].ssid));
    }
    esp_blufi_send_wifi_list((uint16_t)n, list);
}

// ---------------------------------------------------------------------------
// BLUFI 回调（BTC 任务上下文）
// ---------------------------------------------------------------------------
static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        // 初始化完成后开始广播，手机才能扫到。
        ESP_LOGI(TAG, "BLUFI 初始化完成，开始广播（free=%u largest=%u）",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_blufi_adv_start_with_name(CYBER_PROV_DEVICE_NAME);
        s_failed = false;
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "手机已连接（free=%u largest=%u）",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_ble_connected = true;
        esp_blufi_adv_stop();   // 单连接：连上即停广播，省电也避免第二个手机插进来
        if (cyber_prov_security_init() != 0) {
            ESP_LOGE(TAG, "安全上下文分配失败");
            s_failed = true;
        }
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        cyber_prov_security_deinit();
        // 断开后重新广播：用户可能只是中途走开，回来还能继续配。
        // 已提交凭证的情况下也让用户能再进来改网络。
        s_committed = false;
        esp_blufi_adv_start_with_name(CYBER_PROV_DEVICE_NAME);
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        // 只支持 STA，忽略手机下发的模式切换（本设备没有 AP 模式）。
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (!cyber_prov_accept_ssid(&s_creds, (const char *)param->sta_ssid.ssid,
                                    param->sta_ssid.ssid_len)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (!cyber_prov_accept_pass(&s_creds, (const char *)param->sta_passwd.passwd,
                                    param->sta_passwd.passwd_len)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;

    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        // 不锁定 BSSID：同一 SSID 可能有多个 AP（mesh/扩展器），锁死反而连不上。
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
        ESP_LOGI(TAG, "收到连接请求（SSID \"%s\" len=%u pass=%s，free=%u largest=%u）",
                 s_creds.has_ssid ? s_creds.ssid : "(缺)",
                 s_creds.ssid_len, s_creds.has_pass ? "有" : "无",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (!cyber_prov_creds_valid(&s_creds)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        const esp_err_t err = cyber_net_prov_commit(s_creds.ssid,
                                                    s_creds.has_pass ? s_creds.pass : "");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "提交凭证失败: %s", esp_err_to_name(err));
            esp_blufi_send_error_info(ESP_BLUFI_STA_CONN_FAIL);
            break;
        }
        s_committed = true;
        send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        break;
    }

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        // 手机要求断开热点。清掉连接意图并退出配网模式，避免又自动重连回去。
        s_committed = false;
        cyber_net_prov_end();
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (cyber_net_is_connected()) {
            send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        } else if (s_committed) {
            send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        } else {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        // 借用 cyber_net 的 Wi-Fi 栈扫描（它是唯一执行者）。
        if (cyber_net_prov_scan(on_scan_done, NULL) != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGW(TAG, "BLUFI 上报错误: %d", (int)param->report_error.state);
        break;

    default:
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = cyber_prov_negotiate,
    .encrypt_func = cyber_prov_encrypt,
    .decrypt_func = cyber_prov_decrypt,
    .checksum_func = cyber_prov_checksum,
};

// ---------------------------------------------------------------------------
// NimBLE 生命周期
// ---------------------------------------------------------------------------
static void blufi_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_failed = true;
}

static void blufi_sync(void)
{
    ESP_LOGI(TAG, "NimBLE 同步完成，初始化 BLUFI profile（free=%u largest=%u）",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const int rc = esp_blufi_profile_init();
    if (rc == 0) {
        s_profile_initialized = true;
    } else {
        ESP_LOGE(TAG, "BLUFI profile 初始化失败: %d", rc);
        s_failed = true;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped != NULL) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static esp_err_t host_start(void)
{
    esp_err_t err = esp_blufi_register_callbacks(&s_callbacks);
    if (err != ESP_OK) return err;

    err = nimble_port_init();
    if (err != ESP_OK) return err;
    s_host_initialized = true;
    ESP_LOGI(TAG, "[heap] nimble_port_init 后 free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_host_stopped = xSemaphoreCreateBinary();
    if (s_host_stopped == NULL) return ESP_ERR_NO_MEM;

    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    int rc = esp_blufi_gatt_svr_init();
    if (rc != 0) return ESP_FAIL;
    s_gatt_initialized = true;
    ESP_LOGI(TAG, "[heap] gatt_svr_init 后 free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    rc = ble_svc_gap_device_name_set(CYBER_PROV_DEVICE_NAME);
    if (rc != 0) return ESP_FAIL;

    // esp_blufi_btc_init() 内部对 btc_init() 的结果直接 assert，失败即 panic。
    // 而 btc_init() 不返回具体原因（只有 NOMEM 一种），所以在这里先记下堆状态：
    // 崩溃后能从上一行看出到底是"没内存"还是别的。
    ESP_LOGI(TAG, "[heap] btc_init 前 free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_blufi_btc_init();
    s_btc_initialized = true;
    ESP_LOGI(TAG, "[heap] btc_init 后 free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    err = esp_nimble_enable(host_task);
    if (err == ESP_OK) s_host_running = true;
    return err;
}

// 完整拆除 BLE 栈，把 RAM 还给系统。顺序是"后建的先拆"，漏拆任何一步都会让下次
// 启动失败（与 cyber_net 的失败回滚同一条规矩）。
static void host_stop(void)
{
    s_ble_connected = false;
    cyber_prov_security_deinit();

    if (s_host_initialized) {
        if (s_profile_initialized) {
            esp_blufi_adv_stop();
        }
        if (s_gatt_initialized) {
            esp_blufi_gatt_svr_deinit();
            s_gatt_initialized = false;
        }
        bool host_stopped = !s_host_running;
        if (s_host_running) {
            const int rc = nimble_port_stop();
            if (rc == 0) {
                // 等 host 任务真正退出再 deinit，否则会在正在跑的栈上做释放。
                xSemaphoreTake(s_host_stopped, portMAX_DELAY);
                host_stopped = true;
            } else {
                ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
            }
        }
        if (host_stopped) nimble_port_deinit();
        s_host_running = false;

        if (s_profile_initialized) {
            esp_blufi_profile_deinit();
            s_profile_initialized = false;
        }
        if (s_btc_initialized) {
            esp_blufi_btc_deinit();
            s_btc_initialized = false;
        }
        s_host_initialized = false;
    }

    if (s_host_stopped != NULL) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_started = false;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool cyber_prov_is_active(void)
{
    return s_started;
}

esp_err_t cyber_prov_start(void)
{
    if (s_started) return ESP_OK;

    // 清掉上一轮的残留：凭证与状态都要从头开始，否则上次没配完的 SSID 会
    // 被当成这一次的输入。
    memset(&s_creds, 0, sizeof(s_creds));
    s_ble_connected = false;
    s_committed = false;
    s_failed = false;

    // 让 cyber_net 进入配网模式：Wi-Fi 起起来（扫描要用）但抑制自动重连。
    // 失败不致命——没有 Wi-Fi 时 BLE 侧仍能收凭证，只是列表扫描不可用。
    const esp_err_t net_err = cyber_net_prov_begin();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "配网模式下 Wi-Fi 未就绪（%s），AP 列表将不可用",
                 esp_err_to_name(net_err));
    }

    // 起 BLE 前先记堆。BLE 启动失败最常见的原因是【连续】内存不足
    // （ble_buf_alloc 需要整块，碎片化的堆即使 free 够也会失败），
    // 没有这行就只能看到一句无信息量的 ESP_FAIL。
    ESP_LOGI(TAG, "启动 BLE 前：free=%u internal=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    const esp_err_t err = host_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败: %s（largest=%u）", esp_err_to_name(err),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        host_stop();
        cyber_net_prov_end();
        s_failed = true;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "配网已启动，广播名 %s", CYBER_PROV_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t cyber_prov_stop(void)
{
    if (!s_started && !s_host_initialized) {
        // 仍是幂等的：确保配网模式被退出。
        cyber_net_prov_end();
        return ESP_OK;
    }

    host_stop();
    // 退出配网模式：未提交凭证时会把 Wi-Fi 停下来（见 cyber_net_prov_end）。
    cyber_net_prov_end();
    s_committed = false;
    ESP_LOGI(TAG, "配网已停止");
    return ESP_OK;
}

void cyber_prov_get_status(cyber_prov_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->started = s_started;
    out->ble_connected = s_ble_connected;
    out->failed = s_failed;

    // Wi-Fi 侧事实直接问 cyber_net，不在这里复制一份——两处各存一份必然漂移，
    // 而"配网页说连上了、状态栏说 OFFLINE"正是那种最难解释的不一致。
    const bool online = cyber_net_is_connected();
    out->wifi_connected = online;
    out->wifi_connecting = s_committed && !online;
}
