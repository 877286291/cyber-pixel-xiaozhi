// main/main.c —— 赛博像素 AI 助手（cyber-pixel-xiaozhi）应用入口。
//
// 与上游 demo 入口的区别：这里没有硬件自检菜单，启动即进开机动画 → 主界面，
// 硬件自检页是上游 BSP 的示例外壳，本应用不复用（见 docs/development/ai-guide.md
// 的"派生应用必须重做界面"规则）。
//
// 任务与并发模型：
//   - 按键回调运行在 button 组件的共享 esp_timer 任务上，只做"入队"这一件有界的事；
//   - 模型与界面全部由 LVGL 定时器回调推进，因此它们只被 LVGL 任务触碰，
//     不存在跨任务竞态，也就不需要在模型上加锁；
//   - 需要操作 lv_* 的地方（建屏、建定时器）持有 bsp_lvgl_lock()；
//   - Wi-Fi 事件任务要通知"已联网"，它只写一个 bool，再由 LVGL 任务读走并写进模型。
//     这样跨任务共享量只有一个标志位，模型的写入方始终只有 LVGL 任务。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "cyber_net.h"
#include "cyber_ota.h"
#include "cyber_theme.h"
#include "cyber_ui.h"
#include "cyber_ui_model.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "cyber";

// 界面推进周期：40ms ≈ 25fps。240x320 像素界面再快也没有观感收益，只会多刷 SPI；
// 再慢则眨眼与声波条会明显发顿。
#define CYBER_TICK_MS      40
// 电量采样间隔（tick 数）= 10s。CW2017 走 I2C，单次读取不到 1ms，
// 但 SOC 本身缓变，没必要每帧都问一次。
#define CYBER_BATT_TICKS   (10000 / CYBER_TICK_MS)
#define CYBER_INPUT_DEPTH  8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef struct {
    cyber_ui_model_t model;
    cyber_ui_t *ui;
    lv_timer_t *tick;
    uint32_t batt_ticks;
    cyber_ota_state_t ota_last_state;   // 仅用于"状态变化时才打日志"
} app_ctx_t;

static QueueHandle_t s_btn_queue;

// cyber_ota_state_t → cyber_pair_state_t。放在这里而不是模型里，是因为这是唯一
// 同时看得见"HTTP 走到哪一步"和"界面该显示什么"的地方：模型不能 include
// cyber_ota.h（它要能脱离 ESP-IDF 单测），cyber_ota 也不该知道屏幕上有什么。
//
// 判定顺序是有讲究的：服务端【明确】要求重新绑定时，它压过一切，
// 包括"本机曾绑定成功"；其余情况下一旦绑定过就保持在已就绪，掉线或单次查询
// 失败都不该把一台绑好的设备打回配对屏。
static cyber_pair_state_t map_pair(const cyber_ota_snapshot_t *ota, bool net)
{
    switch (ota->state) {
    case CYBER_OTA_STATE_NEED_ACTIVATION:
        return CYBER_PAIR_WAITING;      // 服务端要求绑定 → 之前的绑定已失效
    case CYBER_OTA_STATE_READY:
        return CYBER_PAIR_READY;
    case CYBER_OTA_STATE_TEST_ONLY:
        return CYBER_PAIR_TEST_ONLY;
    case CYBER_OTA_STATE_CHECKING:
        return ota->paired ? CYBER_PAIR_READY : CYBER_PAIR_LINKING;
    case CYBER_OTA_STATE_ERROR:
        return ota->paired ? CYBER_PAIR_READY : CYBER_PAIR_ERROR;
    case CYBER_OTA_STATE_IDLE:
    default:
        if (ota->paired) return CYBER_PAIR_READY;
        // 开机后首个响应回来之前就是这个状态。没网时显示"连不上"而不是
        // "正在连接"：后者会让用户一直等一个不会到来的响应。
        return net ? CYBER_PAIR_LINKING : CYBER_PAIR_OFFLINE;
    }
}

// Wi-Fi 事件任务与 LVGL 任务之间唯一的共享量。
// 事件回调只做一次布尔写入，模型读取后立刻转成模型字段；单字节对齐写是原子的，
// 且这里不存在"读-改-写"复合操作，所以不需要加锁。
// 反过来，事件回调【绝不能】直接调 cyber_model_set_net()：写它的任务不是 LVGL 任务，
// 那会破坏上面"模型只被 LVGL 任务触碰"的前提。
static volatile bool s_net_connected;

// 联网状态回调：运行在 Wi-Fi/IP 事件任务上下文，禁止碰 LVGL、禁止阻塞。
static void on_net_state(bool connected, void *ctx)
{
    (void)ctx;
    s_net_connected = connected;
}

// 按键回调：运行在共享 esp_timer 任务，禁止阻塞、禁止碰 LVGL。
// 只把事件塞进队列并立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (s_btn_queue == NULL) return;
    const input_event_t in = { .btn = btn, .event = ev };
    (void)xQueueSend(s_btn_queue, &in, 0);
}

// BSP 事件 → 模型事件。返回 false 表示该事件与应用无关，直接丢弃。
static bool map_event(bsp_btn_t btn, bsp_btn_ev_t ev,
                      cyber_btn_t *out_btn, cyber_btn_ev_t *out_ev)
{
    switch (btn) {
    case BSP_BTN_UP:   *out_btn = CYBER_BTN_UP;   break;
    case BSP_BTN_DOWN: *out_btn = CYBER_BTN_DOWN; break;
    case BSP_BTN_OK:   *out_btn = CYBER_BTN_OK;   break;
    default:           return false;
    }

    switch (ev) {
    case BSP_BTN_CLICK:  *out_ev = CYBER_BTN_EV_CLICK;  break;
    case BSP_BTN_DOUBLE: *out_ev = CYBER_BTN_EV_DOUBLE; break;
    case BSP_BTN_LONG:   *out_ev = CYBER_BTN_EV_LONG;   break;
    // BSP_BTN_PRESS 是"刚按下"的即时事件，适合游戏类逐帧响应；
    // 本应用只用边沿语义（单击/长按），因此忽略它。
    default: return false;
    }
    return true;
}

// 执行模型给出的动作。背光直接落 LEDC；语音动作在阶段 1 是占位。
static void apply_action(app_ctx_t *ctx, cyber_action_t act)
{
    switch (act) {
    case CYBER_ACT_BACKLIGHT:
        bsp_display_backlight(ctx->model.backlight_percent);
        break;
    case CYBER_ACT_START_LISTEN:
    case CYBER_ACT_STOP_LISTEN:
    case CYBER_ACT_ABORT:
        // 阶段 1：本地占位流程不需要外部动作，这里只留下钩子。
        // 阶段 2 接入小智协议后在同样三个分支里收发音频/下发 abort，
        // 状态机与渲染层都不需要改动（见 cyber_ui_model.h 的 voice_event）。
        ESP_LOGI(TAG, "voice action %d", (int)act);
        break;
    case CYBER_ACT_NONE:
    default:
        break;
    }
}

static void tick_cb(lv_timer_t *timer)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) return;

    // 1) 先消化按键：模型只在 LVGL 任务里被修改。
    input_event_t in;
    while (s_btn_queue != NULL && xQueueReceive(s_btn_queue, &in, 0) == pdTRUE) {
        cyber_btn_t btn;
        cyber_btn_ev_t ev;
        if (!map_event(in.btn, in.event, &btn, &ev)) continue;
        apply_action(ctx, cyber_model_button(&ctx->model, btn, ev));
    }

    // 2) 推进时间：超时回落（聆听超时、说话结束）与空闲降亮都在这一步发生。
    apply_action(ctx, cyber_model_tick(&ctx->model, CYBER_TICK_MS));

    // 3) 低频刷新电量。读失败返回 -1，模型与界面都会降级为 "--"。
    if (++ctx->batt_ticks >= CYBER_BATT_TICKS) {
        ctx->batt_ticks = 0;
        cyber_model_set_battery(&ctx->model, bsp_battery_soc());
    }

    // 4) 应用联网状态。事件任务只写 s_net_connected，真正的模型写入在这里发生，
    //    以此维持"模型只被 LVGL 任务修改"的前提。
    const bool net = s_net_connected;
    if (net != ctx->model.net_connected) {
        cyber_model_set_net(&ctx->model, net);
    }

    // 5) 驱动激活客户端并把快照写进模型。
    //    notify 只是写一个标志；快照读取在临界区内完成，两者都不阻塞。
    //    模型写入仍然只发生在 LVGL 任务里，与第 4 步同一条规矩。
    cyber_ota_notify_net(net);
    cyber_ota_snapshot_t ota;
    cyber_ota_get_snapshot(&ota);
    cyber_model_set_pair(&ctx->model, map_pair(&ota, net), ota.code);
    if (ota.state != ctx->ota_last_state) {
        ctx->ota_last_state = ota.state;
        ESP_LOGI(TAG, "ota -> state=%d http=%d code=%s%s paired=%d",
                 (int)ota.state, ota.last_http_status,
                 ota.code[0] ? ota.code : "-",
                 ota.test_credentials ? " (临时账号)" : "",
                 (int)ota.paired);
    }

    // 6) 落到界面（内部只改真正变化的部分）。
    cyber_ui_apply(ctx->ui, &ctx->model);
}

void app_main(void)
{
    ESP_LOGI(TAG, "cyber-pixel-xiaozhi 启动");
    cyber_net_log_heap("boot");

    (void)bsp_i2c_init();
    (void)bsp_i2c_scan();

    // 屏幕是本应用唯一的输出通道。初始化失败就没有可用形态，
    // 不做"串口菜单"降级 —— 那会让入口复杂一倍，且不是产品该有的行为。
    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线 "
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    cyber_net_log_heap("display+lvgl");

    // 常驻单实例：界面模型不大，但要长期存活，放静态区比反复堆分配更省心。
    static app_ctx_t ctx;
    cyber_model_init(&ctx.model);

    // 电量计缺失不阻塞启动：界面显示 "--"，其余功能照常。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用，界面将显示 --");
    } else {
        cyber_model_set_battery(&ctx.model, bsp_battery_soc());
    }

    s_btn_queue = xQueueCreate(CYBER_INPUT_DEPTH, sizeof(input_event_t));
    if (s_btn_queue == NULL) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        // 按键失败仍继续：屏幕能显示，用户至少能确认设备活着。
        ESP_LOGE(TAG, "按键初始化失败：界面可见但无法交互");
    }

    // 建屏与建定时器都要碰 lv_*，统一持锁完成。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 加锁超时，放弃建屏");
        return;
    }
    ctx.ui = cyber_ui_create();
    if (ctx.ui != NULL) {
        ctx.tick = lv_timer_create(tick_cb, CYBER_TICK_MS, &ctx);
    }
    bsp_display_backlight(ctx.model.backlight_percent);
    bsp_lvgl_unlock();

    if (ctx.ui == NULL) {
        // 最常见原因：LVGL 内部堆不够（CONFIG_LV_MEM_SIZE_KILOBYTES）。
        ESP_LOGE(TAG, "界面创建失败：检查 CONFIG_LV_MEM_SIZE_KILOBYTES 是否够用");
        return;
    }
    if (ctx.tick == NULL) {
        ESP_LOGE(TAG, "界面推进定时器创建失败");
        return;
    }
    cyber_net_log_heap("ui ready");

    // --- 联网 ---
    // 刻意放在建屏之后：界面这时已经在跑，联网成败都不影响用户立刻看到画面。
    // 失败只降级成界面上的 OFFLINE，不阻塞启动。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 只有这两个特例才擦除重建（官方推荐的两条路径）；其余错误一概不擦，
        // 因为擦 NVS 会连带清掉用户凭证，不能让启动流程顺手做破坏性操作。
        ESP_LOGW(TAG, "NVS 需要重建（%s）", esp_err_to_name(nvs_err));
        nvs_err = (nvs_flash_erase() == ESP_OK) ? nvs_flash_init() : ESP_FAIL;
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 不可用，跳过联网: %s", esp_err_to_name(nvs_err));
    } else if (cyber_net_init(on_net_state, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 栈初始化失败，界面保持 OFFLINE");
    } else {
        // 没有凭证会返回 ESP_ERR_NOT_FOUND，那是预期情况（配网尚未实现），
        // 已在 cyber_net 内部打过 WARN，这里不重复报错。
        (void)cyber_net_start();
    }
    cyber_net_log_heap("net up");

    // 激活客户端：需要 NVS 与联网。放在联网之后而不是更早——它要读 NVS 里的
    // Client-Id，而没网络时它只会空转等待，早启动没有任何收益。
    if (nvs_err == ESP_OK) {
        if (cyber_ota_init() != ESP_OK) {
            ESP_LOGE(TAG, "激活客户端启动失败；界面照常运行");
        }
    } else {
        ESP_LOGW(TAG, "NVS 不可用，跳过激活客户端");
    }

    ESP_LOGI(TAG, "就绪：屏幕 %dx%d，界面 %d fps，背光 %u%%，联网 %s",
             BSP_LCD_W, BSP_LCD_H, 1000 / CYBER_TICK_MS,
             (unsigned)ctx.model.backlight_percent,
             cyber_net_is_connected() ? "UP" : "DOWN");
}
