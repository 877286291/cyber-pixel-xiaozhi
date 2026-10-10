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
#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "cyber_audio.h"
#include "cyber_net.h"
#include "cyber_ota.h"
#include "cyber_theme.h"
#include "cyber_ui.h"
#include "cyber_ui_model.h"
#include "cyber_ws.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "cyber";

// 界面推进周期：40ms ≈ 25fps。240x320 像素界面再快也没有观感收益，只会多刷 SPI；
// 再慢则眨眼与声波条会明显发顿。
#define CYBER_TICK_MS      40
// 电量采样间隔（tick 数）= 10s。CW2017 走 I2C，单次读取不到 1ms，
// 但 SOC 本身缓变，没必要每帧都问一次。
#define CYBER_BATT_TICKS   (10000 / CYBER_TICK_MS)
#define CYBER_INPUT_DEPTH  8
// 等首个 OTA【成功】结论的上限。超过它就放行语音，避免服务端异常时把语音一起锁死。
#define CYBER_VOICE_WAIT_OTA_MS  30000u

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
    // 对话通道。上一拍的会话状态用于识别【迁移】——同一段回复里会反复出现
    // 相同的 state，只有看到边沿才知道"刚才开始说话"或"这一轮结束了"。
    cyber_ws_state_t ws_last;
    cyber_ws_state_t ws_logged;         // 仅用于"状态变化时才打日志"
    bool talk_enabled;                  // 已下发的启用状态，避免每拍重复投递请求
    char ws_subtitle[CYBER_WS_SUBTITLE_CAP];  // 上一拍的服务器字幕，用于去重
    // 音频。上一拍的阶段用于只在【迁移】时下发格式与清队列，避免每拍重复写标志。
    cyber_audio_phase_t audio_last;
    // 上行统计的上一拍值，用于按秒聚合出"实际上行通不通"，而不是逐帧打日志。
    uint32_t audio_tx_last;
    uint32_t audio_tick;
    // 开机以来的毫秒数。用于给"等首个 OTA 成功结论"设一个上限，避免服务端异常时
    // 把语音功能一起锁死。刻意不复用 model.screen_ms：那个值是"当前这一屏停留了
    // 多久"，切屏就会清零，不能当开机时长用。
    uint32_t up_ms;
    // 开机时 NVS 里是否已有对话凭证。有的话就不必等 OTA 检查——见 tick 第 6 步：
    // 早建连不只是"快"，更是把 TLS 握手放在内存最宽裕的时刻。
    bool have_cached_ws;
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

// 首个 OTA 检查是否已经有结论。IDLE/CHECKING 都还算"不知道"：
// 前者是还没联网或还没开始，后者是请求在路上。
static bool ota_resolved(const cyber_ota_snapshot_t *ota)
{
    switch (ota->state) {
    case CYBER_OTA_STATE_NEED_ACTIVATION:
    case CYBER_OTA_STATE_READY:
    case CYBER_OTA_STATE_TEST_ONLY:
    case CYBER_OTA_STATE_ERROR:
        return true;
    default:
        return false;
    }
}

// cyber_ota_state_t → cyber_boot_link_t。只描述"本次启动的链路走到哪一步"，
// 刻意不看 ota->paired：进度条要反映这次真实完成了什么，NVS 里的旧绑定结论
// 会让一台曾绑定的设备一上电就把进度顶满，等于对一个正在等的用户撒谎。
static cyber_boot_link_t map_boot_link(const cyber_ota_snapshot_t *ota, bool net)
{
    switch (ota->state) {
    case CYBER_OTA_STATE_CHECKING:
        return CYBER_BOOT_LINK_CHECKING;
    case CYBER_OTA_STATE_NEED_ACTIVATION:
    case CYBER_OTA_STATE_READY:
    case CYBER_OTA_STATE_TEST_ONLY:
        return CYBER_BOOT_LINK_DONE;
    case CYBER_OTA_STATE_ERROR:
        return CYBER_BOOT_LINK_OFFLINE;
    case CYBER_OTA_STATE_IDLE:
    default:
        // 还没联网 / 联网了但客户端还没开始查，这两件事在进度条上是不同档位。
        return net ? CYBER_BOOT_LINK_NET : CYBER_BOOT_LINK_IDLE;
    }
}

// Wi-Fi 事件任务与 LVGL 任务之间唯一的共享量。
// 事件回调只做一次布尔写入，模型读取后立刻转成模型字段；单字节对齐写是原子的，
// 且这里不存在"读-改-写"复合操作，所以不需要加锁。
// 反过来，事件回调【绝不能】直接调 cyber_model_set_net()：写它的任务不是 LVGL 任务，
// 那会破坏上面"模型只被 LVGL 任务触碰"的前提。
static volatile bool s_net_connected;

// 激活客户端是否真的起来了。只有 app_main 写、只有 LVGL 任务读，
// 与 s_net_connected 同一条规矩（单字节对齐写，无需加锁）。
// 它决定开机页要不要等：没有客户端就没有"首个结果"可等。
static volatile bool s_ota_up;

// 启动流程是否已经走完（app_main 末尾置位）。
//
// 为什么需要它：建屏与建 LVGL 定时器发生在 app_main 中段，而 NVS / Wi-Fi / 激活
// 客户端都在其后。定时器一建好就开始跑，于是头几拍 tick 必然早于启动收尾——
// 此刻 s_ota_up 天然还是 false。若把"客户端没起来"直接当成"没什么可等"来放行开机页，
// 开机动画会在 CYBER_BOOT_MS 就结束，落到 NVS 里的旧配对结论上；一台刚在控制台
// 解绑、本地却仍记着 paired=1 的设备，就会先闪一帧机甲脸再跳激活页。
// 这个标志把"还没轮到启动"与"启动完了但客户端没起来"分开。
static volatile bool s_boot_ready;

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

// 执行模型给出的动作。背光直接落 LEDC；语音动作转成对话通道的请求。
//
// 这里只【投递】请求：cyber_ws 的对外接口都是非阻塞的（只写队列），真正的
// DNS/TLS/发送在它自己的 worker 里完成。LVGL 任务一旦阻塞，界面立刻卡住。
static void apply_action(app_ctx_t *ctx, cyber_action_t act)
{
    switch (act) {
    case CYBER_ACT_BACKLIGHT:
        bsp_display_backlight(ctx->model.backlight_percent);
        break;
    case CYBER_ACT_VOLUME:
        // 音量落到 codec：bsp_audio_set_volume 在非播放期只会缓存（s_opened 为
        // false 时只更新内部值），播放开始时会恢复，所以这里每拍直接调用是安全的。
        bsp_audio_set_volume(ctx->model.volume_user);
        break;
    case CYBER_ACT_START_LISTEN:
        cyber_ws_request_listen(true);
        break;
    case CYBER_ACT_STOP_LISTEN:
        cyber_ws_request_listen(false);
        break;
    case CYBER_ACT_ABORT:
        cyber_ws_request_abort();
        break;
    case CYBER_ACT_NONE:
    default:
        break;
    }
}

// 会话状态 → 音频阶段。半双工：聆听即采集，回复即播放，其余一律停下。
//
// 映射放在这里而不是 cyber_audio 里：cyber_audio 只管"怎么采怎么放"，它不该知道
// 会话状态机长什么样；反过来模型层也拿不到 Opus/I2S 的语义。这里是唯一同时看得见
// 两边的位置（与 map_pair 同一个理由）。
//
// CONNECTING/READY 都归 IDLE：握手期间还没有下行格式，也没有可采集的对象。READY
// 是"这一轮说完了"，此时必须停播——否则播放器会继续吃上一轮残留的帧。
static cyber_audio_phase_t audio_phase_for(cyber_ws_state_t st)
{
    switch (st) {
    case CYBER_WS_LISTENING: return CYBER_AUDIO_CAPTURE;
    case CYBER_WS_SPEAKING:  return CYBER_AUDIO_PLAYBACK;
    default:                 return CYBER_AUDIO_IDLE;
    }
}

static void tick_cb(lv_timer_t *timer)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) return;

    ctx->up_ms += CYBER_TICK_MS;

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
    // 开机页进度条的阶段：由上面的 OTA 快照换算，不含旧绑定结论（见 map_boot_link）。
    cyber_model_set_boot_link(&ctx->model, map_boot_link(&ota, net));

    // 放开开机页。三种情况都该放：
    //   1) OTA 有结论了 —— 这是常态路径，也是这次改动的目的：开机动画一直演到
    //      这一刻，用户不会先看到一帧机甲脸再被激活页顶掉；
    //   2) 启动流程已走完、但激活客户端根本没起来 —— 没有"首个结果"可等；
    //   3) 启动流程已走完、连 Wi-Fi 都没上来，且本机没绑定过 —— 再等下去只是
    //      让开机页空转，早点把 OFFLINE 摆出来。
    // 2) 3) 都必须先确认 s_boot_ready：那之前 s_ota_up 天然还是 false，
    // 据此放行会让开机页在建屏后 2.6s 就提前结束（见 s_boot_ready 的注释）。
    // 3) 额外要求 !ota.paired：绑定过的设备在没网时，本地旧结论会把界面判成
    // "已就绪"而落到主界面——那正是"先闪机甲脸"的另一种触发方式，所以它宁可
    // 多等一会儿，交给模型里的兜底超时。
    // 模型内部另有一道兜底超时（CYBER_BOOT_HOLD_MAX_MS），这里不重复实现。
    if (ctx->model.boot_gate == CYBER_BOOT_GATE_HOLD && s_boot_ready &&
        (ota_resolved(&ota) || !s_ota_up ||
         (!net && !ota.paired && ctx->model.screen_ms >= CYBER_BOOT_NET_GRACE_MS))) {
        cyber_model_set_boot_gate(&ctx->model, CYBER_BOOT_GATE_AUTO);
        ESP_LOGI(TAG, "开机页放行：ota=%d net=%d paired=%d t=%ums",
                 (int)ota.state, (int)net, (int)ota.paired,
                 (unsigned)ctx->model.screen_ms);
    }

    if (ota.state != ctx->ota_last_state) {
        ctx->ota_last_state = ota.state;
        ESP_LOGI(TAG, "ota -> state=%d http=%d code=%s%s paired=%d",
                 (int)ota.state, ota.last_http_status,
                 ota.code[0] ? ota.code : "-",
                 ota.test_credentials ? " (临时账号)" : "",
                 (int)ota.paired);
    }

    // 6) 驱动对话通道。
    //    通道只在"已绑定 + 已联网"时开启，并且要避免 WebSocket 的 TLS 握手与
    //    OTA 检查在同一段内存里撞车。判定顺序是的取舍如下。
    //
    //    【有缓存凭证】就直接建连，不等 OTA 检查。这不只是为了快——更是因为握手
    //    需要一大块【连续】内存才能做完证书链校验（实测 leaf←GeoTrust G2←
    //    DigiCert Global Root G2，RSA4096，单证书约 2 KB）。而一次 OTA 检查会
    //    分配再释放 ~40 KB 的 TLS 缓冲，把堆切碎：实测握手可见的 largest 从
    //    43008（检查前）掉到 22528（检查后），恰好吃不下证书链，症状却是
    //    "找不到可信根证书"这种看起来与内存无关的报错。所以顺序反过来：先建连，
    //    OTA 检查自然被 defer 挡在后面，堆不会被切碎。
    //
    //    缓存凭证若已失效（服务端轮换 token），表现是建连失败 → 状态回到 IDLE →
    //    defer 落下 → OTA 检查才真正执行并刷新凭证 → 下一轮建连用上新凭证。
    //    也就是说 OTA 检查从"开机必经"变成了"建连失败时的修复路径"，这正好是它
    //    该扮演的角色，也让"不需要 OTA"的设备在正常运行时不产生任何 OTA 流量。
    //
    //    【无缓存凭证】时才退回原来的顺序：等 OTA 检查给出凭证。首次激活本来就要
    //    等用户在控制台完成绑定，这段等待不可省，此时也还没有语音会话要保护。
    //
    //    失败结论（ERROR）不算"已就绪"：它常常只是开机时网络还没稳，而一旦建连
    //    把这个重试挡掉，那个"稍后会成功"就永远不会发生了。ota_give_up 是兜底，
    //    避免服务端异常时把语音一起锁死。
    const bool ota_settled = ota_resolved(&ota) &&
                             ota.state != CYBER_OTA_STATE_ERROR;
    const bool ota_give_up = ctx->up_ms >= CYBER_VOICE_WAIT_OTA_MS;
    const bool ready_source = ota_settled || ota_give_up || !s_ota_up ||
                              ctx->have_cached_ws;
    const bool talk_ok = net && ready_source &&
                         !cyber_pair_blocks_ui(ctx->model.pair) &&
                         ctx->model.pair != CYBER_PAIR_OFF;
    if (talk_ok != ctx->talk_enabled) {
        cyber_ws_set_enabled(talk_ok);
        ctx->talk_enabled = talk_ok;
        ESP_LOGI(TAG, "对话通道 %s（ota_settled=%d 缓存凭证=%d up=%ums）",
                 talk_ok ? "开启" : "关闭", (int)ota_settled,
                 (int)ctx->have_cached_ws, (unsigned)ctx->up_ms);
    }
    // 联网时语音流程交给服务端驱动；离线时模型退回本地占位流程。
    cyber_model_set_remote_voice(&ctx->model, talk_ok);

    cyber_ws_snapshot_t ws;
    cyber_ws_get_snapshot(&ws);
    const cyber_ws_state_t prev = ctx->ws_last;
    const cyber_ws_state_t cur = ws.state;

    // 只认【迁移】，不认电平：同一段回复期间 state 会一直是 SPEAKING，
    // 每拍都灌一次会把 voice_ms 反复清零，界面上也什么都看不出变化。
    // 判定终点用的也是边沿：SPEAKING→READY 才是"这一轮说完了"，
    // 而 LISTENING→READY（用户主动停止收音）之后还要等服务端回复。
    if (cur != prev) {
        if (cur == CYBER_WS_SPEAKING) {
            cyber_model_voice_event(&ctx->model, CYBER_VOICE_SPEAKING,
                                    CYBER_EMO_HAPPY, ws.subtitle);
        } else if (cur == CYBER_WS_ERROR) {
            cyber_model_voice_event(&ctx->model, CYBER_VOICE_ERROR,
                                    CYBER_EMO_GLITCH, NULL);
        } else if (cur == CYBER_WS_READY && prev == CYBER_WS_SPEAKING) {
            cyber_model_voice_event(&ctx->model, CYBER_VOICE_IDLE,
                                    CYBER_EMO_NEUTRAL, NULL);
        } else if (cur == CYBER_WS_IDLE && ctx->model.screen == CYBER_SCREEN_VOICE) {
            // 链路断开：语音流程已经无法继续，收尾回主界面。
            cyber_model_voice_event(&ctx->model, CYBER_VOICE_IDLE,
                                    CYBER_EMO_NEUTRAL, NULL);
        }
        ctx->ws_last = cur;
    }

    // 同一段回复里服务端会逐句下发字幕：只更新文本，不重切状态。
    if (ctx->model.voice == CYBER_VOICE_SPEAKING &&
        strcmp(ws.subtitle, ctx->ws_subtitle) != 0) {
        cyber_model_set_subtitle(&ctx->model, ws.subtitle);
    }
    memcpy(ctx->ws_subtitle, ws.subtitle, sizeof(ctx->ws_subtitle));

    // 6b) 音频阶段。放在会话迁移之后：这一拍刚判定的 state 就是音频该处的阶段，
    //     中间不再插一次快照。
    //
    //     下行格式每拍都下发一次，而不是只在握手那一拍：set_downlink 只是拿一次
    //     无争用的互斥锁 + 拷 8 字节，代价可以忽略；换成"只在迁移时下发"则要在
    //     ctx 里再存一份 handshakes，多一处状态就多一处可能不同步的地方。
    //     顺序上先格式后阶段：worker 进入 PLAYBACK 时会读它，若反过来，就会有一拍
    //     用默认的 16 kHz 去解 24 kHz 的流（听起来是慢放的怪声）。
    cyber_audio_set_downlink(&ws.downlink);
    const cyber_audio_phase_t aim = audio_phase_for(ws.state);
    if (aim != ctx->audio_last) {
        ctx->audio_last = aim;
        cyber_audio_set_phase(aim);
        ESP_LOGI(TAG, "音频 -> %s（下行 %u Hz）",
                 aim == CYBER_AUDIO_CAPTURE ? "采集" :
                 aim == CYBER_AUDIO_PLAYBACK ? "播放" : "停止",
                 (unsigned)ws.downlink.rate);
    }

    // 会话占用期间挂起后台 OTA 检查：一次 check 要开一条 TLS，实测与 WebSocket
    // 会话叠加时堆最低点只剩 32716 B，对话中途触发会威胁音频缓冲。
    //
    // CONNECTING 也必须算进去：预建连一启动就占内存，若这时放行检查，两者会正面对撞
    // （实测把检查挤成 state=5）。含 READY 是因为两轮对话之间 TLS 仍然挂着，占用并
    // 没有释放。
    cyber_ota_set_defer(ws.state == CYBER_WS_CONNECTING ||
                        ws.state == CYBER_WS_READY ||
                        ws.state == CYBER_WS_LISTENING ||
                        ws.state == CYBER_WS_SPEAKING);

    // 上行每 60 ms 一帧，逐帧打日志会淹没串口。5 秒聚合一次：这一行能直接回答
    // "采集到底有没有真的发出去"，而这个问题的症状（服务端一直不回复）与
    // "麦克风没声音"看起来完全一样。
    if (++ctx->audio_tick >= (5000 / CYBER_TICK_MS)) {
        ctx->audio_tick = 0;
        cyber_audio_stats_t as;
        cyber_audio_get_stats(&as);
        const uint32_t sent = as.tx_frames - ctx->audio_tx_last;
        ctx->audio_tx_last = as.tx_frames;
        if (as.tx_frames != 0u || as.rx_frames != 0u || ctx->audio_last != CYBER_AUDIO_IDLE) {
            ESP_LOGI(TAG, "音频：上行 %u 帧/5s（累计 %u 丢 %u 编码错 %u 分配失败 %u），"
                          "下行 %u 帧（解错 %u 写错 %u 读错 %u），"
                          "抖动丢 满%u 超长%u，栈余量 %u B",
                     (unsigned)sent, (unsigned)as.tx_frames, (unsigned)as.tx_dropped,
                     (unsigned)as.encode_errors, (unsigned)as.alloc_failures,
                     (unsigned)as.rx_frames,
                     (unsigned)as.decode_errors, (unsigned)as.write_errors,
                     (unsigned)as.read_errors, (unsigned)as.queue_dropped_full,
                     (unsigned)as.queue_dropped_oversize,
                     (unsigned)as.stack_high_water);
        }
    }

    if (ws.state != ctx->ws_logged) {
        ctx->ws_logged = ws.state;
        ESP_LOGI(TAG, "会话 -> %s session=%s err=%d 握手=%u",
                 cyber_ws_state_label(ws.state),
                 ws.session_id[0] ? ws.session_id : "-",
                 ws.last_error, (unsigned)ws.handshakes);
    }

    // 7) 落到界面（内部只改真正变化的部分）。
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
    // 开机动画一直演到首个 OTA 结果：否则 NVS 里的旧"已绑定"会让界面先跳主界面，
    // 几秒后再被激活页顶掉，看起来像设备出错。若后面激活客户端没起来，
    // 会在 tick 里立刻把这道闸放开（见 s_ota_up）。
    cyber_model_set_boot_gate(&ctx.model, CYBER_BOOT_GATE_HOLD);

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
        } else {
            // 这行必须晚于 cyber_model_set_boot_gate(HOLD)：tick 一旦看到它，
            // 就会允许开机页按 OTA 结果放行。
            s_ota_up = true;
        }

        // NVS 里是否已有对话凭证。用专门的"存在性查询"而不是 get_ws_config：
        // 后者需要足够大的缓冲，用小缓冲会因 NVS 长度校验失败而把"有"误判成"无"。
        // 有缓存时的行为差异见 tick 第 6 步：可以直接建连，从而把 TLS 握手放在
        // 内存最宽裕的时刻。
        ctx.have_cached_ws = cyber_ota_has_ws_config();
        ESP_LOGI(TAG, "缓存对话凭证：%s", ctx.have_cached_ws ? "有" : "无");
    } else {
        ESP_LOGW(TAG, "NVS 不可用，跳过激活客户端");
    }

    // 对话通道：只建队列与 worker，不建连。是否允许连接由 tick 按"已绑定 + 已联网"
    // 下发（见 tick 第 6 步）——在这里就建连会在还没有凭证时白跑一次握手。
    if (cyber_ws_init() != ESP_OK) {
        ESP_LOGE(TAG, "对话通道初始化失败；语音入口将不可用");
    }
    cyber_net_log_heap("ws ready");

    // 音频通路：接在对话通道之后。
    //   1) 音频要在收到二进制帧时有地方可去，所以接收点必须早于允许建连（tick 里
    //      才下发 talk_ok，因此这里的顺序天然满足）；
    //   2) 上行接收点接的是 cyber_ws_send_audio —— 契约上它只能被音频 worker 调用，
    //      而 worker 是这里 xTaskCreate 出来的，接线本身就是这两个模块唯一的耦合点，
    //      放在一起是为了让"谁调谁"一眼可见。
    // 失败不阻塞启动：文本通路（字幕、状态机）不依赖音频，仍可工作。
    if (cyber_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频通路初始化失败；对话将只有字幕没有声音");
    } else {
        cyber_audio_set_tx_sink(cyber_ws_send_audio);
        cyber_ws_set_audio_sink(cyber_audio_push_opus);
    }
    cyber_net_log_heap("audio ready");

    // 启动收尾：从这一刻起，tick 才能把"客户端没起来/没联网"当作放行开机页的依据。
    // 必须在 s_ota_up 设置之后写，否则会短暂出现"启动已完成但客户端状态未知"的窗口。
    s_boot_ready = true;

    ESP_LOGI(TAG, "就绪：屏幕 %dx%d，界面 %d fps，背光 %u%%，联网 %s",
             BSP_LCD_W, BSP_LCD_H, 1000 / CYBER_TICK_MS,
             (unsigned)ctx.model.backlight_percent,
             cyber_net_is_connected() ? "UP" : "DOWN");
}
