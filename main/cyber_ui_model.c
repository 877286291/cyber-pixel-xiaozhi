// main/cyber_ui_model.c
// 界面模型的实现。全文件不含 lv_* / esp_* 调用，可直接编译进宿主单测。
#include "cyber_ui_model.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 常量表
// ---------------------------------------------------------------------------
// 面部圆角阶梯：inset[d] = 距上下边缘第 d 行向内收进的格数。
// 数值递减保证轮廓单调外扩，避免出现"锯齿回勾"的畸形轮廓。
static const uint8_t FACE_CORNER_INSET[CYBER_FACE_ROUND] = { 8, 4, 2, 1 };

// 阶段 1 占位回复：确定性短句，让「聆听 → 思考 → 说话 → 回落」整条 UI 链路
// 能完全离线跑通，便于在没有服务器时验证动画与布局。
// 阶段 2 由小智下行 LLM/TTS 文本替换；cyber_model_voice_event 已按同一状态机设计，
// 到时只需把下行事件灌进来，渲染层不用改。
typedef struct {
    cyber_emotion_t emo;
    const char *text;
} cyber_mock_reply_t;

static const cyber_mock_reply_t MOCK_REPLIES[] = {
    { CYBER_EMO_HAPPY,   "HELLO! I AM YOUR PIXEL ASSISTANT." },
    { CYBER_EMO_CURIOUS, "SYSTEM ONLINE. ASK ME ANYTHING." },
    { CYBER_EMO_FOCUS,   "SCANNING... NO THREATS DETECTED." },
    { CYBER_EMO_ALERT,   "NEON LINK STABLE AT FULL POWER." },
};
#define MOCK_REPLY_COUNT (sizeof(MOCK_REPLIES) / sizeof(MOCK_REPLIES[0]))

// 表情 → 主色 + 短名。数组顺序必须与 cyber_emotion_t 一致。
static const uint32_t EMOTION_COLOR[CYBER_EMO_COUNT] = {
    CYBER_CYAN, CYBER_LIME, CYBER_AMBER, CYBER_CYAN,
    CYBER_MAGENTA, CYBER_PURPLE, CYBER_RED,
};
static const char *const EMOTION_NAME[CYBER_EMO_COUNT] = {
    "NEUTRAL", "HAPPY", "CURIOUS", "FOCUS", "ALERT", "SLEEPY", "GLITCH",
};

// 语音状态 → 标签 + 强调色。数组顺序必须与 cyber_voice_state_t 一致。
static const char *const VOICE_LABEL[] = {
    "STANDBY", "LISTENING", "THINKING", "SPEAKING", "ERROR",
};
static const uint32_t VOICE_COLOR[] = {
    CYBER_TEXT_DIM, CYBER_AMBER, CYBER_MAGENTA, CYBER_CYAN, CYBER_RED,
};
#define VOICE_STATE_COUNT (sizeof(VOICE_LABEL) / sizeof(VOICE_LABEL[0]))

// 配对状态 → 状态行文字 + 强调色。数组顺序必须与 cyber_pair_state_t 一致。
// OFF 一项永远不显示（该状态下界面根本不在激活页），留空串占位以保持下标对齐。
static const char *const PAIR_LABEL[] = {
    "", "LINKING", "WAIT FOR BIND", "READY", "TEST ACCOUNT", "NO LINK", "LINK ERROR",
};
static const uint32_t PAIR_COLOR[] = {
    CYBER_TEXT_DIM, CYBER_CYAN, CYBER_AMBER, CYBER_LIME,
    CYBER_MAGENTA, CYBER_TEXT_DIM, CYBER_RED,
};

// 静态断言：查表长度必须覆盖枚举，改枚举忘了改表时编译即失败。
_Static_assert(sizeof(EMOTION_COLOR) / sizeof(EMOTION_COLOR[0]) == CYBER_EMO_COUNT,
               "EMOTION_COLOR 与 cyber_emotion_t 不同步");
_Static_assert(sizeof(EMOTION_NAME) / sizeof(EMOTION_NAME[0]) == CYBER_EMO_COUNT,
               "EMOTION_NAME 与 cyber_emotion_t 不同步");
_Static_assert(sizeof(PAIR_LABEL) / sizeof(PAIR_LABEL[0]) == CYBER_PAIR_COUNT,
               "PAIR_LABEL 与 cyber_pair_state_t 不同步");
_Static_assert(sizeof(PAIR_COLOR) / sizeof(PAIR_COLOR[0]) == CYBER_PAIR_COUNT,
               "PAIR_COLOR 与 cyber_pair_state_t 不同步");

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
static void copy_text(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// 切屏：重置该屏的停留计时，供超时/动画使用。
static void enter_screen(cyber_ui_model_t *m, cyber_screen_t s)
{
    if (m->screen == s) return;
    m->screen = s;
    m->screen_ms = 0;
}

// 切语音状态。离开"说话"态时清掉字幕，避免残字留在下一态的画面上。
static void enter_voice(cyber_ui_model_t *m, cyber_voice_state_t v)
{
    if (v != CYBER_VOICE_SPEAKING) m->subtitle[0] = '\0';
    m->voice = v;
    m->voice_ms = 0;
}

// 按空闲档位推出实际背光。降亮/关屏只是"用户偏好亮度"的临时覆盖，
// 恢复活动后回到用户自己调的档位，而不是强行拉满。
static void apply_idle_backlight(cyber_ui_model_t *m)
{
    switch (m->idle) {
    case CYBER_IDLE_DIM:
        m->backlight_percent = (m->backlight_user < CYBER_BL_DIM_PERCENT)
                             ? m->backlight_user : CYBER_BL_DIM_PERCENT;
        break;
    case CYBER_IDLE_OFF:
        m->backlight_percent = 0;
        break;
    case CYBER_IDLE_ACTIVE:
    default:
        m->backlight_percent = m->backlight_user;
        break;
    }
}

static cyber_action_t adjust_backlight(cyber_ui_model_t *m, int steps)
{
    int level = (int)m->backlight_user + steps * CYBER_BL_STEP_PERCENT;
    // 下限留一档：调到 0 会让用户以为"按键失效"，真正全灭走省电路径。
    if (level < CYBER_BL_STEP_PERCENT) level = CYBER_BL_STEP_PERCENT;
    if (level > 100) level = 100;
    m->backlight_user = (uint8_t)level;
    m->idle = CYBER_IDLE_ACTIVE;
    apply_idle_backlight(m);
    return CYBER_ACT_BACKLIGHT;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void cyber_model_init(cyber_ui_model_t *m)
{
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    m->screen = CYBER_SCREEN_BOOT;
    m->emotion = CYBER_EMO_NEUTRAL;
    m->voice_emotion = CYBER_EMO_HAPPY;
    m->voice = CYBER_VOICE_IDLE;
    m->idle = CYBER_IDLE_ACTIVE;
    m->backlight_user = 100;
    m->backlight_percent = 100;
    m->battery_soc = -1;      // 未知：渲染层画"--"而不是编一个数字
    m->net_connected = false; // 阶段 1 未联网
    // 配对流程默认关闭：这样阶段 1 的演示流程与宿主单测的行为与接入激活前完全一致。
    // 它只由 main.c 显式打开（见 cyber_model_set_pair）。
    m->pair = CYBER_PAIR_OFF;
    m->pair_code[0] = '\0';
    // 开机页默认到点就走：宿主单测与阶段 1 的行为完全不变。
    // 只有真的会去查 OTA 的设备（main.c）才把它改成 HOLD。
    m->boot_gate = CYBER_BOOT_GATE_AUTO;
    m->boot_link = CYBER_BOOT_LINK_IDLE;
    m->boot_pct = 0;
}

// ---------------------------------------------------------------------------
// 时间推进
// ---------------------------------------------------------------------------
cyber_action_t cyber_model_tick(cyber_ui_model_t *m, uint32_t dt_ms)
{
    if (m == NULL || dt_ms == 0u) return CYBER_ACT_NONE;

    m->uptime_ms += dt_ms;
    m->screen_ms += dt_ms;
    m->voice_ms += dt_ms;
    m->anim_ms += dt_ms;
    m->idle_ms += dt_ms;

    // 空闲降亮以"无操作"为准，但激活页例外：这一屏的用途就是让用户盯着 6 位码
    // 到别的设备上输入，读码期间本来就不会碰设备，按无操作息屏会把用户正要看的
    // 码关掉。所以激活页把计时按住（归零），除非用户主动关屏——长按 OK 会把
    // idle_ms 直接顶到 OFF_MS，这里必须先认它、再按住，否则主动关屏会被下一拍取消。
    const bool reading_code = (m->screen == CYBER_SCREEN_ACTIVATION);
    cyber_idle_t want = CYBER_IDLE_ACTIVE;
    if (reading_code && m->idle_ms < CYBER_IDLE_OFF_MS) {
        m->idle_ms = 0;
    } else if (m->idle_ms >= CYBER_IDLE_OFF_MS) {
        want = CYBER_IDLE_OFF;
    } else if (m->idle_ms >= CYBER_IDLE_DIM_MS) {
        want = CYBER_IDLE_DIM;
    }
    if (want != m->idle) {
        m->idle = want;
        apply_idle_backlight(m);
        return CYBER_ACT_BACKLIGHT;
    }

    if (m->screen == CYBER_SCREEN_BOOT) {
        // 进度条：AUTO 档位沿用原来的纯时间进度；HOLD 档位改按真实链路阶段抬升，
        // 且只增不减（阶段可能因丢网回退，进度条不能跟着往回缩）。
        if (m->boot_gate == CYBER_BOOT_GATE_HOLD) {
            const uint8_t pct = cyber_boot_phase_pct(m->screen_ms, CYBER_BOOT_MS,
                                                     m->boot_link);
            if (pct > m->boot_pct) m->boot_pct = pct;
        } else {
            m->boot_pct = cyber_boot_progress(m->screen_ms, CYBER_BOOT_MS);
        }
        // 开机动画本身很短，播完自动进下一屏；期间不响应按键（见 cyber_model_button）。
        // 未绑定就走激活页：把配对码藏在一个"看起来正常"的主界面后面，
        // 用户只会以为设备坏了。
        //
        // HOLD 档位下多等一会儿：首个 OTA 结果出来前，"是否已绑定"只有 NVS 里的
        // 旧结论，据此进主界面会在几秒后被激活页顶掉。兜底超时保证没网或服务端
        // 不响应时不会永远停在开机页。
        if (m->boot_gate == CYBER_BOOT_GATE_HOLD &&
            m->screen_ms >= CYBER_BOOT_HOLD_MAX_MS) {
            m->boot_gate = CYBER_BOOT_GATE_AUTO;
        }
        if (m->screen_ms >= CYBER_BOOT_MS && m->boot_gate == CYBER_BOOT_GATE_AUTO) {
            enter_screen(m, cyber_pair_blocks_ui(m->pair) ? CYBER_SCREEN_ACTIVATION
                                                          : CYBER_SCREEN_HOME);
        }
        return CYBER_ACT_NONE;
    }

    // 主界面与激活页之间的迁移必须是双向的："绑定已生效"和"服务端现在要求
    // 激活"都可能在任意时刻到来，哪边来就该切哪边。反方向不是可有可无的：
    // 用户在控制台解绑后，服务端会重新下发激活码，设备必须回到激活页等用户
    // 重输码。若只实现"激活页→主界面"，设备会永远停留在它最后知道的那个
    // "已绑定"状态上，用户看到一个看起来正常的画面，实际需要激活。
    if (m->screen == CYBER_SCREEN_ACTIVATION) {
        // 绑定一旦生效就自动放行到主界面：用户在控制台输完码，屏幕上不该再需要任何操作。
        if (!cyber_pair_blocks_ui(m->pair)) enter_screen(m, CYBER_SCREEN_HOME);
        return CYBER_ACT_NONE;
    }
    if (m->screen == CYBER_SCREEN_HOME && m->voice == CYBER_VOICE_IDLE) {
        if (cyber_pair_blocks_ui(m->pair)) enter_screen(m, CYBER_SCREEN_ACTIVATION);
        return CYBER_ACT_NONE;
    }

    if (m->screen != CYBER_SCREEN_VOICE) return CYBER_ACT_NONE;

    switch (m->voice) {
    case CYBER_VOICE_LISTENING:
        // 阶段 1 用固定上限兜底，避免没有语音输入时卡死在聆听态。
        // 阶段 2 由服务器的 stt/tts 事件结束本态，这个超时仍作为断线兜底。
        if (m->voice_ms >= CYBER_LISTEN_TIMEOUT_MS) {
            enter_voice(m, CYBER_VOICE_THINKING);
            return CYBER_ACT_STOP_LISTEN;
        }
        break;
    case CYBER_VOICE_THINKING:
        if (m->voice_ms >= CYBER_THINK_TIMEOUT_MS) {
            const cyber_mock_reply_t *r = &MOCK_REPLIES[m->mock_index % MOCK_REPLY_COUNT];
            m->mock_index = (uint8_t)((m->mock_index + 1u) % MOCK_REPLY_COUNT);
            cyber_model_voice_event(m, CYBER_VOICE_SPEAKING, r->emo, r->text);
        }
        break;
    case CYBER_VOICE_SPEAKING:
        if (m->voice_ms >= cyber_speak_duration_ms(strlen(m->subtitle))) {
            enter_voice(m, CYBER_VOICE_IDLE);
            enter_screen(m, CYBER_SCREEN_HOME);
        }
        break;
    case CYBER_VOICE_ERROR:
        if (m->voice_ms >= CYBER_ERROR_MS) {
            enter_voice(m, CYBER_VOICE_IDLE);
            enter_screen(m, CYBER_SCREEN_HOME);
        }
        break;
    default:
        break;
    }
    return CYBER_ACT_NONE;
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
static cyber_action_t home_button(cyber_ui_model_t *m, cyber_btn_t btn,
                                  cyber_btn_ev_t ev)
{
    if (ev == CYBER_BTN_EV_LONG) {
        if (btn == CYBER_BTN_UP) return adjust_backlight(m, +1);
        if (btn == CYBER_BTN_DOWN) return adjust_backlight(m, -1);
        if (btn == CYBER_BTN_OK) {
            // 立即省电：把空闲计时直接推到关屏阈值，复用 tick 里的统一降亮路径，
            // 避免"手动关屏"和"空闲关屏"两套逻辑各自维护背光状态。
            m->idle_ms = CYBER_IDLE_OFF_MS;
        }
        return CYBER_ACT_NONE;
    }
    if (ev != CYBER_BTN_EV_CLICK) return CYBER_ACT_NONE;

    switch (btn) {
    case CYBER_BTN_UP:
        // 上/下短按在主界面手动切表情，便于不接服务器时逐个检查 7 种表情。
        m->emotion = (cyber_emotion_t)((m->emotion + CYBER_EMO_COUNT - 1u) % CYBER_EMO_COUNT);
        return CYBER_ACT_NONE;
    case CYBER_BTN_DOWN:
        m->emotion = (cyber_emotion_t)((m->emotion + 1u) % CYBER_EMO_COUNT);
        return CYBER_ACT_NONE;
    case CYBER_BTN_OK:
        enter_voice(m, CYBER_VOICE_LISTENING);
        enter_screen(m, CYBER_SCREEN_VOICE);
        return CYBER_ACT_START_LISTEN;
    default:
        return CYBER_ACT_NONE;
    }
}

static cyber_action_t voice_button(cyber_ui_model_t *m, cyber_btn_t btn,
                                   cyber_btn_ev_t ev)
{
    // 长按 OK 随时退出语音流程回主界面；阶段 2 在此同时下发服务器 abort。
    if (ev == CYBER_BTN_EV_LONG && btn == CYBER_BTN_OK) {
        enter_voice(m, CYBER_VOICE_IDLE);
        enter_screen(m, CYBER_SCREEN_HOME);
        return CYBER_ACT_ABORT;
    }
    if (ev != CYBER_BTN_EV_CLICK) return CYBER_ACT_NONE;

    switch (m->voice) {
    case CYBER_VOICE_LISTENING:
        // 提前收尾，模拟"用户说完了"。
        enter_voice(m, CYBER_VOICE_THINKING);
        return CYBER_ACT_STOP_LISTEN;
    case CYBER_VOICE_THINKING:
    case CYBER_VOICE_SPEAKING:
        // 打断本轮。
        enter_voice(m, CYBER_VOICE_IDLE);
        enter_screen(m, CYBER_SCREEN_HOME);
        return CYBER_ACT_ABORT;
    case CYBER_VOICE_ERROR:
        enter_voice(m, CYBER_VOICE_IDLE);
        enter_screen(m, CYBER_SCREEN_HOME);
        return CYBER_ACT_NONE;
    default:
        enter_voice(m, CYBER_VOICE_LISTENING);
        return CYBER_ACT_START_LISTEN;
    }
}

// 激活页。绑定完成前语音通道根本不可用，所以短按一律无效——做任何事都只会
// 让用户以为操作生效了。长按 OK 仍保留"立即省电"，与主界面同一套语义，
// 用户在等待时也能把屏幕关掉。
static cyber_action_t activation_button(cyber_ui_model_t *m, cyber_btn_t btn,
                                        cyber_btn_ev_t ev)
{
    if (ev == CYBER_BTN_EV_LONG && btn == CYBER_BTN_OK) {
        m->idle_ms = CYBER_IDLE_OFF_MS;
    }
    return CYBER_ACT_NONE;
}

cyber_action_t cyber_model_button(cyber_ui_model_t *m, cyber_btn_t btn,
                                  cyber_btn_ev_t ev)
{
    if (m == NULL || btn >= CYBER_BTN_COUNT) return CYBER_ACT_NONE;

    // 背光全灭时，第一次按键只负责点亮，不触发语义动作 —— 否则用户在黑屏上
    // 随手一按可能正好命中"开始聆听"，观感像设备自己动了。
    const bool was_off = (m->idle == CYBER_IDLE_OFF);
    m->idle_ms = 0;
    if (m->idle != CYBER_IDLE_ACTIVE) {
        m->idle = CYBER_IDLE_ACTIVE;
        apply_idle_backlight(m);
        if (was_off) return CYBER_ACT_BACKLIGHT;
    }

    switch (m->screen) {
    case CYBER_SCREEN_HOME:
        return home_button(m, btn, ev);
    case CYBER_SCREEN_VOICE:
        return voice_button(m, btn, ev);
    case CYBER_SCREEN_ACTIVATION:
        return activation_button(m, btn, ev);
    case CYBER_SCREEN_BOOT:
    default:
        return CYBER_ACT_NONE;   // 开机动画很短，不做可打断。
    }
}

// ---------------------------------------------------------------------------
// 外部状态注入
// ---------------------------------------------------------------------------
void cyber_model_set_battery(cyber_ui_model_t *m, int soc)
{
    if (m == NULL) return;
    // 只接受合法区间；越界一律当作"读不到"，让 UI 走降级分支。
    m->battery_soc = (soc < 0 || soc > 100) ? -1 : soc;
}

void cyber_model_set_net(cyber_ui_model_t *m, bool connected)
{
    if (m == NULL) return;
    m->net_connected = connected;
}

void cyber_model_set_pair(cyber_ui_model_t *m, cyber_pair_state_t state,
                          const char *code)
{
    if (m == NULL) return;
    // 越界值一律当作查询失败：宁可显示一个需要重试的状态，也不要因为数组下标
    // 越界去查 PAIR_LABEL/PAIR_COLOR，或者把界面留在激活页却说不清为什么。
    m->pair = (state < CYBER_PAIR_COUNT) ? state : CYBER_PAIR_ERROR;
    copy_text(m->pair_code, sizeof(m->pair_code), code);
}

void cyber_model_set_boot_gate(cyber_ui_model_t *m, cyber_boot_gate_t gate)
{
    if (m == NULL) return;
    // 只认两个合法值；越界一律当作放行，免得一个坏参数把界面永远钉在开机页。
    m->boot_gate = (gate == CYBER_BOOT_GATE_HOLD) ? CYBER_BOOT_GATE_HOLD
                                                  : CYBER_BOOT_GATE_AUTO;
}

void cyber_model_set_boot_link(cyber_ui_model_t *m, cyber_boot_link_t link)
{
    if (m == NULL) return;
    // 越界一律当"还没联网"：宁可少画一点进度，也不要越界取标签。
    m->boot_link = (link < CYBER_BOOT_LINK_COUNT) ? link : CYBER_BOOT_LINK_IDLE;
}

uint8_t cyber_boot_meter(const cyber_ui_model_t *m)
{
    return (m == NULL) ? 0u : m->boot_pct;
}

const char *cyber_boot_link_label(cyber_boot_link_t link)
{
    switch (link) {
    case CYBER_BOOT_LINK_NET:      return "WAITING FOR SERVICE";
    case CYBER_BOOT_LINK_CHECKING: return "CHECKING ACTIVATION";
    case CYBER_BOOT_LINK_DONE:     return "LINK READY";
    case CYBER_BOOT_LINK_OFFLINE:  return "NO SERVER LINK";
    case CYBER_BOOT_LINK_IDLE:
    default:                       return "WAITING FOR NETWORK";
    }
}

bool cyber_boot_waiting(const cyber_ui_model_t *m)
{
    if (m == NULL) return false;
    return m->screen == CYBER_SCREEN_BOOT &&
           m->boot_gate == CYBER_BOOT_GATE_HOLD &&
           m->screen_ms >= CYBER_BOOT_MS;
}

void cyber_model_voice_event(cyber_ui_model_t *m, cyber_voice_state_t state,
                             cyber_emotion_t emo, const char *text)
{
    if (m == NULL) return;

    if (state == CYBER_VOICE_SPEAKING) {
        if (emo < CYBER_EMO_COUNT) m->voice_emotion = emo;
        copy_text(m->subtitle, sizeof(m->subtitle), text);
    } else {
        m->subtitle[0] = '\0';
        if (state == CYBER_VOICE_ERROR) m->voice_emotion = CYBER_EMO_GLITCH;
    }

    enter_voice(m, state);
    // 绑定完成前不给语音界面：否则一条下行事件就能绕过激活页，把用户带到一个
    // 注定没有音频通道的页面上。状态本身照样记录，绑定一旦生效即可正常显示。
    if (!cyber_pair_blocks_ui(m->pair)) enter_screen(m, CYBER_SCREEN_VOICE);
}

// ---------------------------------------------------------------------------
// 派生查询
// ---------------------------------------------------------------------------
cyber_emotion_t cyber_model_visible_emotion(const cyber_ui_model_t *m)
{
    if (m == NULL) return CYBER_EMO_NEUTRAL;

    if (m->screen == CYBER_SCREEN_VOICE) {
        switch (m->voice) {
        case CYBER_VOICE_LISTENING: return CYBER_EMO_CURIOUS;
        case CYBER_VOICE_THINKING:  return CYBER_EMO_FOCUS;
        case CYBER_VOICE_SPEAKING:  return m->voice_emotion;
        case CYBER_VOICE_ERROR:     return CYBER_EMO_GLITCH;
        default: break;
        }
    }
    // 省电档位统一睡眼，给"该充电了"一个视觉提示。
    if (m->idle != CYBER_IDLE_ACTIVE) return CYBER_EMO_SLEEPY;
    return m->emotion;
}

uint32_t cyber_emotion_color(cyber_emotion_t e)
{
    if (e >= CYBER_EMO_COUNT) return CYBER_CYAN;
    return EMOTION_COLOR[e];
}

const char *cyber_emotion_label(cyber_emotion_t e)
{
    if (e >= CYBER_EMO_COUNT) return EMOTION_NAME[CYBER_EMO_NEUTRAL];
    return EMOTION_NAME[e];
}

const char *cyber_model_link_label(const cyber_ui_model_t *m)
{
    if (m == NULL || !m->net_connected) return "OFFLINE";
    return "AI.LINK";
}

const char *cyber_voice_label(cyber_voice_state_t s)
{
    if (s >= VOICE_STATE_COUNT) return VOICE_LABEL[CYBER_VOICE_IDLE];
    return VOICE_LABEL[s];
}

uint32_t cyber_voice_color(cyber_voice_state_t s)
{
    if (s >= VOICE_STATE_COUNT) return VOICE_COLOR[CYBER_VOICE_IDLE];
    return VOICE_COLOR[s];
}

bool cyber_pair_blocks_ui(cyber_pair_state_t s)
{
    switch (s) {
    case CYBER_PAIR_LINKING:
    case CYBER_PAIR_WAITING:
    case CYBER_PAIR_OFFLINE:
    case CYBER_PAIR_ERROR:
        return true;
    // TEST_ONLY 不阻塞：服务端不再要求激活、又给了可用端点，说明绑定已经生效，
    // 只是它仍返回临时凭证（#1785，实测于本设备）。把设备永远钉在配对页比让用户
    // 先用上（临时账号本身可用）更糟；这一事实由日志与快照记录，供排查。
    case CYBER_PAIR_TEST_ONLY:
    case CYBER_PAIR_OFF:
    case CYBER_PAIR_READY:
    case CYBER_PAIR_COUNT:
    default:
        return false;
    }
}

const char *cyber_pair_label(cyber_pair_state_t s)
{
    if (s >= CYBER_PAIR_COUNT) return PAIR_LABEL[CYBER_PAIR_ERROR];
    return PAIR_LABEL[s];
}

uint32_t cyber_pair_color(cyber_pair_state_t s)
{
    if (s >= CYBER_PAIR_COUNT) return PAIR_COLOR[CYBER_PAIR_ERROR];
    return PAIR_COLOR[s];
}

// ---------------------------------------------------------------------------
// 动画与进度
// ---------------------------------------------------------------------------
uint8_t cyber_boot_progress(uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0u || elapsed_ms >= duration_ms) return 100u;
    // 先扩到 64 位再乘，避免 elapsed 较大时 32 位溢出。
    return (uint8_t)(((uint64_t)elapsed_ms * 100u) / duration_ms);
}

uint8_t cyber_boot_phase_pct(uint32_t elapsed_ms, uint32_t duration_ms,
                             cyber_boot_link_t link)
{
    // 自检动画只走到 SELFTEST，不再自己冲到 100：余下的进度必须由真实阶段挣来。
    // 否则等待期间进度已满，只能靠来回动证明"没死"，那正是要消除的反复横跳。
    uint32_t pct = (uint32_t)cyber_boot_progress(elapsed_ms, duration_ms) *
                   CYBER_BOOT_PCT_SELFTEST / 100u;
    if (pct > CYBER_BOOT_PCT_SELFTEST) pct = CYBER_BOOT_PCT_SELFTEST;

    uint32_t floor = 0u;
    switch (link) {
    case CYBER_BOOT_LINK_NET:      floor = CYBER_BOOT_PCT_NET;   break;
    case CYBER_BOOT_LINK_CHECKING: floor = CYBER_BOOT_PCT_CHECK; break;
    case CYBER_BOOT_LINK_DONE:     floor = CYBER_BOOT_PCT_DONE;  break;
    // OFFLINE 不给下限：链路是"没打通"，不该伪装成接近完成。进度靠调用方
    // （tick 里的 max 锁存）保持只增不减，超时后即为开机页最后一帧。
    case CYBER_BOOT_LINK_OFFLINE:
    case CYBER_BOOT_LINK_IDLE:
    default:                       break;
    }
    return (uint8_t)((pct > floor) ? pct : floor);
}

uint8_t cyber_boot_log_count(uint32_t elapsed_ms, uint32_t duration_ms, uint8_t total)
{
    if (total == 0u) return 0u;
    const uint32_t p = cyber_boot_progress(elapsed_ms, duration_ms);
    // 自检行在前 70% 进度内出齐，剩下 30% 留给进度条走完，观感上"先自检、后启动"。
    uint32_t remapped = (p * 100u) / 70u;
    if (remapped > 100u) remapped = 100u;
    const uint32_t n = (remapped * total) / 100u;
    return (uint8_t)((n > total) ? total : n);
}

bool cyber_boot_glitch(uint32_t elapsed_ms, uint32_t duration_ms)
{
    // 开头一段模拟信号不稳；中途在 70% 处再抖一次，呼应"启动完成前的最后一次重同步"。
    if (elapsed_ms < CYBER_BOOT_GLITCH_MS) return true;
    const uint32_t mark = (duration_ms * 7u) / 10u;
    return (elapsed_ms >= mark) && (elapsed_ms < mark + 120u);
}

bool cyber_face_blink(uint32_t anim_ms)
{
    return (anim_ms % CYBER_BLINK_PERIOD_MS) < CYBER_BLINK_MS;
}

uint8_t cyber_wave_level(uint32_t anim_ms, uint8_t index, uint8_t count)
{
    if (count == 0u) return 0u;

    const uint32_t half = CYBER_WAVE_PERIOD_MS / 2u;   // 恒 > 0，见 cyber_theme.h
    const uint32_t step = CYBER_WAVE_PERIOD_MS / count;

    // 三角波给"说话起伏"，每个 bar 相位错开 step，形成从左到右的流动感。
    const uint32_t phase = (anim_ms + (uint32_t)index * step) % CYBER_WAVE_PERIOD_MS;
    const uint32_t tri = (phase < half) ? phase : (CYBER_WAVE_PERIOD_MS - phase);

    // 叠一层慢速分量，免得所有 bar 一起整齐升降、看起来像机械扫描。
    const uint32_t slow = ((anim_ms / 3u) + (uint32_t)index * 53u) % 256u;

    uint32_t value = (tri * 200u) / half + slow / 4u;
    if (value > 255u) value = 255u;
    return (uint8_t)value;
}

uint32_t cyber_speak_duration_ms(size_t text_len)
{
    uint64_t d = (uint64_t)text_len * CYBER_SPEAK_MS_PER_CHAR;
    if (d < CYBER_SPEAK_MIN_MS) d = CYBER_SPEAK_MIN_MS;
    if (d > CYBER_SPEAK_MAX_MS) d = CYBER_SPEAK_MAX_MS;
    return (uint32_t)d;
}

void cyber_pair_split_code(const char *code, char *row1, size_t cap1,
                           char *row2, size_t cap2)
{
    // 占位符用 '-'：cyber_font_code 的字符范围里特意保留了它，好让"还没拿到码"
    // 和"码就是 000000"在屏幕上长得不一样。
    const char FILL = '-';
    // 先取长度再按下标取字符：直接读 code[3] 去判断"够不够长"会在短字符串上越界。
    const size_t len = (code != NULL) ? strlen(code) : 0u;
    char *const rows[2] = { row1, row2 };
    const size_t caps[2] = { cap1, cap2 };

    for (int r = 0; r < 2; r++) {
        char *out = rows[r];
        if (out == NULL || caps[r] == 0u) continue;
        if (caps[r] <= (size_t)CYBER_PAIR_CODE_ROW) {
            // 容量不够时输出空串，绝不写出一个没有结尾的缓冲。
            out[0] = '\0';
            continue;
        }
        for (int i = 0; i < CYBER_PAIR_CODE_ROW; i++) {
            const size_t src = (size_t)(r * CYBER_PAIR_CODE_ROW + i);
            out[i] = (src < len) ? code[src] : FILL;
        }
        out[CYBER_PAIR_CODE_ROW] = '\0';
    }
}

uint8_t cyber_pulse(uint32_t anim_ms, uint32_t period_ms)
{
    // 周期短到没有"半个周期"可分时，呼吸退化为常亮。
    if (period_ms < 2u) return 255u;

    const uint32_t half = period_ms / 2u;
    const uint32_t phase = anim_ms % period_ms;
    const uint32_t tri = (phase < half) ? phase : (period_ms - phase);
    if (tri >= half) return 255u;
    return (uint8_t)((tri * 255u) / half);
}

int cyber_hud_chase(uint32_t anim_ms, int track_len, int phase)
{
    if (track_len <= 0) return 0;

    // 先按步长取模再叠 phase：phase 为负时用 +track_len 规整，
    // 这样拖尾灯和头灯永远在同一套坐标上，不会因为取模方式不同而错位。
    int pos = (int)((anim_ms / CYBER_HUD_STEP_MS) % (uint32_t)track_len);
    pos += phase % track_len;
    pos %= track_len;
    if (pos < 0) pos += track_len;
    return pos;
}

int cyber_visor_scan_index(uint32_t anim_ms, int span)
{
    if (span <= 0) return 0;
    if (span == 1) return 0;

    const uint32_t half = CYBER_SCAN_PERIOD_MS / 2u;   // 恒 > 0，见 cyber_theme.h
    const uint32_t phase = anim_ms % CYBER_SCAN_PERIOD_MS;
    const uint32_t tri = (phase < half) ? phase : (CYBER_SCAN_PERIOD_MS - phase);

    // 把 0..half 的三角波映射到 0..span-1，两端都能取到，扫描才覆盖满行程。
    int idx = (int)(((uint64_t)tri * (uint32_t)(span - 1)) / half);
    if (idx < 0) idx = 0;
    if (idx >= span) idx = span - 1;
    return idx;
}

// ---------------------------------------------------------------------------
// 面部几何
// ---------------------------------------------------------------------------
int cyber_face_half_width(int row)
{
    if (row < CYBER_FACE_HEAD_TOP || row >= CYBER_FACE_GRID_H) return 0;

    const int d_top = row - CYBER_FACE_HEAD_TOP;
    const int d_bot = (CYBER_FACE_GRID_H - 1) - row;
    const int d = (d_top < d_bot) ? d_top : d_bot;
    if (d >= CYBER_FACE_ROUND) return CYBER_FACE_HALF_MAX;
    return CYBER_FACE_HALF_MAX - (int)FACE_CORNER_INSET[d];
}

int cyber_face_head_bands(cyber_face_band_t *out, int max_bands)
{
    if (out == NULL || max_bands <= 0) return 0;

    int count = 0;
    int row = CYBER_FACE_HEAD_TOP;
    while (row < CYBER_FACE_GRID_H && count < max_bands) {
        const int half = cyber_face_half_width(row);
        int last = row;
        // 同半宽的连续行合并：中部大段直线区因此只占一个矩形。
        while ((last + 1) < CYBER_FACE_GRID_H &&
               cyber_face_half_width(last + 1) == half) {
            last++;
        }
        out[count].row0 = row;
        out[count].row1 = last;
        out[count].half = half;
        count++;
        row = last + 1;
    }
    return count;
}
