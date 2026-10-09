// tests/test_cyber_ui_model.c
// cyber_ui_model 的宿主单测：状态机、动画/进度数学、面部几何。
// 这些逻辑刻意不依赖 LVGL 与 ESP-IDF，所以可以在 PC 上直接跑（见 tools/validate.sh）。
// 渲染层（cyber_face.c / cyber_ui.c）不在宿主覆盖范围内，它依赖真实的 LVGL 绘制管线。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cyber_theme.h"
#include "cyber_ui_model.h"

// 按 tick 周期推进 total_ms，返回最后一次非 NONE 的动作。
static cyber_action_t advance(cyber_ui_model_t *m, uint32_t total_ms, uint32_t step_ms)
{
    cyber_action_t last = CYBER_ACT_NONE;
    for (uint32_t t = 0; t < total_ms; t += step_ms) {
        const cyber_action_t a = cyber_model_tick(m, step_ms);
        if (a != CYBER_ACT_NONE) last = a;
    }
    return last;
}

// 把模型从开机动画推到主界面。
static void boot_to_home(cyber_ui_model_t *m)
{
    advance(m, CYBER_BOOT_MS, 40);
    assert(m->screen == CYBER_SCREEN_HOME);
}

static void test_init_defaults(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);

    assert(m.screen == CYBER_SCREEN_BOOT);
    assert(m.emotion == CYBER_EMO_NEUTRAL);
    assert(m.voice == CYBER_VOICE_IDLE);
    assert(m.idle == CYBER_IDLE_ACTIVE);
    assert(m.backlight_user == 100);
    assert(m.backlight_percent == 100);
    // 未读到电量必须保持 -1，渲染层据此画 "--" 而不是编一个数字。
    assert(m.battery_soc == -1);
    assert(m.net_connected == false);
    assert(m.subtitle[0] == '\0');

    // 空指针与零步长都必须安全。
    cyber_model_init(NULL);
    assert(cyber_model_tick(NULL, 40) == CYBER_ACT_NONE);
    assert(cyber_model_tick(&m, 0) == CYBER_ACT_NONE);
    assert(m.uptime_ms == 0);
    assert(cyber_model_button(NULL, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
}

static void test_boot_progress_math(void)
{
    assert(cyber_boot_progress(0, CYBER_BOOT_MS) == 0);
    assert(cyber_boot_progress(CYBER_BOOT_MS / 2, CYBER_BOOT_MS) == 50);
    assert(cyber_boot_progress(CYBER_BOOT_MS, CYBER_BOOT_MS) == 100);
    // 超时与零时长都必须饱和到 100，不能让进度的除数变成 0。
    assert(cyber_boot_progress(CYBER_BOOT_MS * 2, CYBER_BOOT_MS) == 100);
    assert(cyber_boot_progress(1234, 0) == 100);
    // 乘 100 必须在 64 位里做，否则这两个大值会溢出回绕成小数字。
    assert(cyber_boot_progress(2000000000u, 3000000000u) == 66);
}

static void test_boot_log_count(void)
{
    assert(cyber_boot_log_count(0, CYBER_BOOT_MS, CYBER_BOOT_LOG_LINES) == 0);
    // 自检行在前 70% 进度内出齐，之后只走进度条。
    assert(cyber_boot_log_count((CYBER_BOOT_MS * 7u) / 10u, CYBER_BOOT_MS,
                                CYBER_BOOT_LOG_LINES) == CYBER_BOOT_LOG_LINES);
    assert(cyber_boot_log_count(CYBER_BOOT_MS, CYBER_BOOT_MS,
                                CYBER_BOOT_LOG_LINES) == CYBER_BOOT_LOG_LINES);
    // 行数为 0 时不能除零。
    assert(cyber_boot_log_count(100, CYBER_BOOT_MS, 0) == 0);
    // 行数随时间单调不减。
    uint8_t prev = 0;
    for (uint32_t t = 0; t <= CYBER_BOOT_MS; t += 50) {
        const uint8_t n = cyber_boot_log_count(t, CYBER_BOOT_MS, CYBER_BOOT_LOG_LINES);
        assert(n >= prev);
        assert(n <= CYBER_BOOT_LOG_LINES);
        prev = n;
    }
}

static void test_boot_glitch_window(void)
{
    assert(cyber_boot_glitch(0, CYBER_BOOT_MS));
    assert(cyber_boot_glitch(CYBER_BOOT_GLITCH_MS - 1, CYBER_BOOT_MS));
    assert(!cyber_boot_glitch(CYBER_BOOT_GLITCH_MS, CYBER_BOOT_MS));

    // 70% 处的第二次抖动是一个窄窗口，不能一直亮着。
    const uint32_t mark = (CYBER_BOOT_MS * 7u) / 10u;
    assert(cyber_boot_glitch(mark, CYBER_BOOT_MS));
    assert(cyber_boot_glitch(mark + 119, CYBER_BOOT_MS));
    assert(!cyber_boot_glitch(mark + 120, CYBER_BOOT_MS));
}

static void test_blink_window(void)
{
    assert(cyber_face_blink(0));
    assert(cyber_face_blink(CYBER_BLINK_MS - 1));
    assert(!cyber_face_blink(CYBER_BLINK_MS));
    assert(!cyber_face_blink(CYBER_BLINK_PERIOD_MS - 1));
    assert(cyber_face_blink(CYBER_BLINK_PERIOD_MS));
}

static void test_wave_level(void)
{
    assert(cyber_wave_level(0, 0, 0) == 0);

    // 同一输入必须给出同一输出：动画由时间决定，不能有隐藏状态。
    for (uint32_t t = 0; t < CYBER_WAVE_PERIOD_MS; t += 37) {
        for (uint8_t i = 0; i < 12; i++) {
            assert(cyber_wave_level(t, i, 12) == cyber_wave_level(t, i, 12));
        }
    }

    // 整个周期内必须有起伏，否则 VU 表会画成一条直线。
    uint8_t lo = 255, hi = 0;
    for (uint32_t t = 0; t < CYBER_WAVE_PERIOD_MS; t++) {
        const uint8_t v = cyber_wave_level(t, 0, 12);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    assert(hi > lo);

    // 相邻 bar 相位错开，同一时刻不应完全相同（否则是整体齐平升降）。
    assert(cyber_wave_level(0, 0, 12) != cyber_wave_level(0, 1, 12));
}

static void test_pulse(void)
{
    // 周期短到没有"半个周期"可分时退化为常亮，同时避免除零。
    assert(cyber_pulse(0, 0) == 255);
    assert(cyber_pulse(1234, 1) == 255);

    // 起点最暗、半周期最亮。
    assert(cyber_pulse(0, 1000) == 0);
    assert(cyber_pulse(500, 1000) == 255);

    // 整个周期内必须走到两端，否则"呼吸"看不出来。
    uint8_t lo = 255, hi = 0;
    for (uint32_t t = 0; t < 1000; t++) {
        const uint8_t v = cyber_pulse(t, 1000);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    assert(lo == 0 && hi == 255);

    // 整周期后回到同一亮度，否则重绘时会看到"跳变"。
    for (uint32_t t = 0; t < 500; t += 7) {
        assert(cyber_pulse(t, 1000) == cyber_pulse(t + 1000, 1000));
    }
}

static void test_hud_chase(void)
{
    // 退化输入返回 0，不越界。
    assert(cyber_hud_chase(0, 0, 0) == 0);
    assert(cyber_hud_chase(5000, -3, 0) == 0);
    assert(cyber_hud_chase(0, 1, 0) == 0);

    enum { TRACK = 11 };

    // 结果恒在 [0, track_len)：负相位（拖尾）也不能逃出范围。
    for (uint32_t t = 0; t < 20 * CYBER_HUD_STEP_MS; t += 17) {
        for (int phase = -3; phase <= 3; phase++) {
            const int pos = cyber_hud_chase(t, TRACK, phase);
            assert(pos >= 0 && pos < TRACK);
        }
    }

    // 每过一个步长推进一格，走满一圈后回绕（而不是到顶停住）。
    assert(cyber_hud_chase(0, TRACK, 0) == 0);
    assert(cyber_hud_chase(CYBER_HUD_STEP_MS, TRACK, 0) == 1);
    assert(cyber_hud_chase((uint32_t)TRACK * CYBER_HUD_STEP_MS, TRACK, 0) == 0);

    // 拖尾灯恒比头灯落后一格（带取模回绕），三盏灯才连成一段。
    for (uint32_t t = 0; t < 5 * CYBER_HUD_STEP_MS; t += 37) {
        const int head = cyber_hud_chase(t, TRACK, 0);
        assert(cyber_hud_chase(t, TRACK, -1) == (head + TRACK - 1) % TRACK);
    }
}

static void test_visor_scan_index(void)
{
    // 退化输入不越界。
    assert(cyber_visor_scan_index(0, 0) == 0);
    assert(cyber_visor_scan_index(0, -5) == 0);
    assert(cyber_visor_scan_index(9999, 1) == 0);

    enum { SPAN = 11 };

    // 恒在 [0, span)。
    for (uint32_t t = 0; t < 3 * CYBER_SCAN_PERIOD_MS; t += 11) {
        const int i = cyber_visor_scan_index(t, SPAN);
        assert(i >= 0 && i < SPAN);
    }

    // 起点在一端、半周期时在另一端：扫描线必须真的走完全程，
    // 否则会"卡"在局部反复抖。
    assert(cyber_visor_scan_index(0, SPAN) == 0);
    assert(cyber_visor_scan_index(CYBER_SCAN_PERIOD_MS / 2u, SPAN) == SPAN - 1);

    // 整周期后回到原位。
    for (uint32_t t = 0; t < 500; t += 23) {
        assert(cyber_visor_scan_index(t, SPAN) ==
               cyber_visor_scan_index(t + CYBER_SCAN_PERIOD_MS, SPAN));
    }
}

static void test_speak_duration(void)
{
    // 极短文本也要有下限，否则字幕会一闪而过。
    assert(cyber_speak_duration_ms(0) == CYBER_SPEAK_MIN_MS);
    assert(cyber_speak_duration_ms(1) == CYBER_SPEAK_MIN_MS);
    assert(cyber_speak_duration_ms(20) == 20 * CYBER_SPEAK_MS_PER_CHAR);
    // 超长文本必须封顶，不能把"说话"态挂住几十秒。
    assert(cyber_speak_duration_ms(10000) == CYBER_SPEAK_MAX_MS);
}

static void test_face_half_width(void)
{
    // 第 0..4 行留给天线，不是头部实体。
    for (int row = 0; row < CYBER_FACE_HEAD_TOP; row++) {
        assert(cyber_face_half_width(row) == 0);
    }
    assert(cyber_face_half_width(-1) == 0);
    assert(cyber_face_half_width(CYBER_FACE_GRID_H) == 0);

    // 圆角阶梯：向内收进的格数逐行递减。
    assert(cyber_face_half_width(CYBER_FACE_HEAD_TOP + 0) == CYBER_FACE_HALF_MAX - 8);
    assert(cyber_face_half_width(CYBER_FACE_HEAD_TOP + 1) == CYBER_FACE_HALF_MAX - 4);
    assert(cyber_face_half_width(CYBER_FACE_HEAD_TOP + 2) == CYBER_FACE_HALF_MAX - 2);
    assert(cyber_face_half_width(CYBER_FACE_HEAD_TOP + 3) == CYBER_FACE_HALF_MAX - 1);
    assert(cyber_face_half_width(CYBER_FACE_HEAD_TOP + CYBER_FACE_ROUND) == CYBER_FACE_HALF_MAX);

    // 上下轮廓必须镜像，否则头部看起来是歪的。
    for (int row = CYBER_FACE_HEAD_TOP; row < CYBER_FACE_GRID_H; row++) {
        const int mirror = (CYBER_FACE_GRID_H - 1) - (row - CYBER_FACE_HEAD_TOP);
        assert(cyber_face_half_width(row) == cyber_face_half_width(mirror));
    }

    // 从顶端到中部单调外扩，不能出现"锯齿回勾"。
    for (int row = CYBER_FACE_HEAD_TOP; row < CYBER_FACE_GRID_H / 2; row++) {
        assert(cyber_face_half_width(row) <= cyber_face_half_width(row + 1));
    }
}

static void test_face_head_bands(void)
{
    enum { MAX_BANDS = 12 };
    cyber_face_band_t bands[MAX_BANDS];

    const int n = cyber_face_head_bands(bands, MAX_BANDS);
    assert(n > 0 && n <= MAX_BANDS);

    // 横带必须无缝、有序地覆盖整个头部，且每带半宽与逐行查询一致。
    int expect_row = CYBER_FACE_HEAD_TOP;
    for (int i = 0; i < n; i++) {
        assert(bands[i].row0 == expect_row);
        assert(bands[i].row1 >= bands[i].row0);
        assert(bands[i].half == cyber_face_half_width(bands[i].row0));
        // 带内每行同宽：合并的前提，否则渲染会画出错误轮廓。
        for (int row = bands[i].row0; row <= bands[i].row1; row++) {
            assert(cyber_face_half_width(row) == bands[i].half);
        }
        expect_row = bands[i].row1 + 1;
    }
    assert(expect_row == CYBER_FACE_GRID_H);

    // 中部大段等宽区必须被合并成一个矩形，而不是逐行输出 30 多个带。
    assert(n < CYBER_FACE_GRID_H - CYBER_FACE_HEAD_TOP);

    // 容量不足时截断而不是越界写入。
    for (int cap = 1; cap <= n; cap++) {
        assert(cyber_face_head_bands(bands, cap) == cap);
    }
    assert(cyber_face_head_bands(NULL, MAX_BANDS) == 0);
    assert(cyber_face_head_bands(bands, 0) == 0);
}

static void test_boot_to_home(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);

    // 开机动画期间按键不应生效。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.screen == CYBER_SCREEN_BOOT);

    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_HOME);
    assert(m.uptime_ms >= CYBER_BOOT_MS);
}

static void test_home_navigation(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    // 短按上/下在主界面轮转表情，便于逐个人工检查 7 种表情。
    assert(cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.emotion == CYBER_EMO_HAPPY);
    assert(cyber_model_button(&m, CYBER_BTN_UP, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.emotion == CYBER_EMO_NEUTRAL);
    // 向上越过 0 必须回绕到最后一个，而不是变成负数。
    assert(cyber_model_button(&m, CYBER_BTN_UP, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.emotion == CYBER_EMO_COUNT - 1);
    assert(cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.emotion == CYBER_EMO_NEUTRAL);

    // 双击语义未定义，必须被忽略。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_DOUBLE) == CYBER_ACT_NONE);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 单击 OK 进入语音流程。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
    assert(m.screen == CYBER_SCREEN_VOICE);
    assert(m.voice == CYBER_VOICE_LISTENING);
}

static void test_voice_flow_offline(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);

    // 聆听超时后自动收尾，避免没有语音输入时卡死在聆听态。
    cyber_action_t last = advance(&m, CYBER_LISTEN_TIMEOUT_MS, 40);
    assert(last == CYBER_ACT_STOP_LISTEN);
    assert(m.voice == CYBER_VOICE_THINKING);
    assert(m.screen == CYBER_SCREEN_VOICE);

    // 思考结束后进入占位回复：字幕与情绪必须来自 mock 表。
    advance(&m, CYBER_THINK_TIMEOUT_MS, 40);
    assert(m.voice == CYBER_VOICE_SPEAKING);
    assert(m.subtitle[0] != '\0');
    assert(cyber_model_visible_emotion(&m) == m.voice_emotion);

    // 按字数估算的朗读时长结束后自动回主界面。
    const uint32_t speak_ms = cyber_speak_duration_ms(strlen(m.subtitle));
    advance(&m, speak_ms, 40);
    assert(m.voice == CYBER_VOICE_IDLE);
    assert(m.screen == CYBER_SCREEN_HOME);
    assert(m.subtitle[0] == '\0');
}

static void test_voice_button_interrupts(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    // 聆听中单击 = "我说完了"，转入思考。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_STOP_LISTEN);
    assert(m.voice == CYBER_VOICE_THINKING);

    // 思考中单击 = 打断本轮。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_ABORT);
    assert(m.voice == CYBER_VOICE_IDLE);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 长按 OK 在语音页随时退出（阶段 2 同时下发服务器 abort）。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_LONG) == CYBER_ACT_ABORT);
    assert(m.screen == CYBER_SCREEN_HOME);
    assert(m.voice == CYBER_VOICE_IDLE);
}

static void test_voice_event_interface(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    // 阶段 2 的接口：把下行事件灌进来即可，不需要动渲染层。
    cyber_model_voice_event(&m, CYBER_VOICE_SPEAKING, CYBER_EMO_ALERT, "NEON LINK");
    assert(m.screen == CYBER_SCREEN_VOICE);
    assert(m.voice == CYBER_VOICE_SPEAKING);
    assert(m.voice_emotion == CYBER_EMO_ALERT);
    assert(strcmp(m.subtitle, "NEON LINK") == 0);
    assert(cyber_model_visible_emotion(&m) == CYBER_EMO_ALERT);

    // 非说话态必须清空字幕，避免残字留在下一态的画面上。
    cyber_model_voice_event(&m, CYBER_VOICE_THINKING, CYBER_EMO_NEUTRAL, NULL);
    assert(m.subtitle[0] == '\0');

    // 错误态固定用故障表情。
    cyber_model_voice_event(&m, CYBER_VOICE_ERROR, CYBER_EMO_NEUTRAL, NULL);
    assert(cyber_model_visible_emotion(&m) == CYBER_EMO_GLITCH);

    // 超长字幕必须截断且始终以 NUL 结尾。
    char huge[CYBER_SUBTITLE_MAX + 64];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    cyber_model_voice_event(&m, CYBER_VOICE_SPEAKING, CYBER_EMO_HAPPY, huge);
    assert(strlen(m.subtitle) == CYBER_SUBTITLE_MAX);
}

static void test_error_state_recovers(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    cyber_model_voice_event(&m, CYBER_VOICE_ERROR, CYBER_EMO_NEUTRAL, NULL);
    advance(&m, CYBER_ERROR_MS, 40);
    assert(m.screen == CYBER_SCREEN_HOME);
    assert(m.voice == CYBER_VOICE_IDLE);
}

static void test_backlight_ladder(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    // 长按上/下调一档。
    assert(cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG) == CYBER_ACT_BACKLIGHT);
    assert(m.backlight_user == 80);
    assert(m.backlight_percent == 80);
    assert(cyber_model_button(&m, CYBER_BTN_UP, CYBER_BTN_EV_LONG) == CYBER_ACT_BACKLIGHT);
    assert(m.backlight_user == 100);
    // 上限封顶。
    assert(cyber_model_button(&m, CYBER_BTN_UP, CYBER_BTN_EV_LONG) == CYBER_ACT_BACKLIGHT);
    assert(m.backlight_user == 100);

    // 下限留一档：调到 0 会让用户以为按键失效，真正全灭走省电路径。
    for (int i = 0; i < 10; i++) {
        cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);
    }
    assert(m.backlight_user == CYBER_BL_STEP_PERCENT);
    assert(m.backlight_percent == CYBER_BL_STEP_PERCENT);
}

static void test_idle_dim_and_off(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    // 先调低用户亮度，验证降亮取"用户档位与暗档的较小值"，不会反而变亮。
    cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);   // 80
    cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);   // 60
    cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);   // 40
    cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);   // 20
    assert(m.backlight_user == 20);

    assert(advance(&m, CYBER_IDLE_DIM_MS, 40) == CYBER_ACT_BACKLIGHT);
    assert(m.idle == CYBER_IDLE_DIM);
    assert(m.backlight_percent == 20);           // 用户档位低于暗档
    assert(cyber_model_visible_emotion(&m) == CYBER_EMO_SLEEPY);

    assert(advance(&m, CYBER_IDLE_OFF_MS - CYBER_IDLE_DIM_MS, 40) == CYBER_ACT_BACKLIGHT);
    assert(m.idle == CYBER_IDLE_OFF);
    assert(m.backlight_percent == 0);

    // 关屏后第一次按键只负责点亮：否则用户在黑屏上随手一按可能正好开始收音。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_BACKLIGHT);
    assert(m.screen == CYBER_SCREEN_HOME);
    assert(m.idle == CYBER_IDLE_ACTIVE);
    assert(m.backlight_percent == m.backlight_user);

    // 点亮之后按键恢复正常语义。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
}

static void test_idle_ok_long_powers_off(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);
    assert(m.idle == CYBER_IDLE_ACTIVE);

    // 长按 OK 立即省电：走的是与空闲超时同一条降亮路径。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_LONG) == CYBER_ACT_NONE);
    assert(advance(&m, CYBER_BOOT_MS, 40) == CYBER_ACT_BACKLIGHT);
    assert(m.idle == CYBER_IDLE_OFF);
    assert(m.backlight_percent == 0);
}

static void test_idle_recovery_keeps_user_level(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    boot_to_home(&m);

    cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG);   // 用户档位 80
    advance(&m, CYBER_IDLE_OFF_MS, 40);
    assert(m.idle == CYBER_IDLE_OFF);

    // 恢复活动后回到用户自己调的档位，而不是强行拉满。
    assert(cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_CLICK) == CYBER_ACT_BACKLIGHT);
    assert(m.idle == CYBER_IDLE_ACTIVE);
    assert(m.backlight_percent == 80);
}

static void test_battery_and_link_queries(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);

    // 越界一律当"读不到"，让 UI 走降级分支而不是画出 105%。
    cyber_model_set_battery(&m, 57);
    assert(m.battery_soc == 57);
    cyber_model_set_battery(&m, 0);
    assert(m.battery_soc == 0);
    cyber_model_set_battery(&m, 100);
    assert(m.battery_soc == 100);
    cyber_model_set_battery(&m, -1);
    assert(m.battery_soc == -1);
    cyber_model_set_battery(&m, 101);
    assert(m.battery_soc == -1);
    cyber_model_set_battery(&m, -5);
    assert(m.battery_soc == -1);
    cyber_model_set_battery(NULL, 50);

    assert(strcmp(cyber_model_link_label(&m), "OFFLINE") == 0);
    cyber_model_set_net(&m, true);
    assert(strcmp(cyber_model_link_label(&m), "AI.LINK") == 0);
    cyber_model_set_net(&m, false);
    assert(strcmp(cyber_model_link_label(&m), "OFFLINE") == 0);
    assert(strcmp(cyber_model_link_label(NULL), "OFFLINE") == 0);
}

static void test_lookup_tables_cover_enum(void)
{
    // 查表长度与枚举同步由 cyber_ui_model.c 的 _Static_assert 保证；
    // 这里只验证越界访问会回落到默认值而不是读到表外。
    assert(cyber_emotion_color(CYBER_EMO_COUNT) == CYBER_CYAN);
    assert(strcmp(cyber_emotion_label(CYBER_EMO_COUNT), "NEUTRAL") == 0);
    assert(strcmp(cyber_voice_label(CYBER_VOICE_IDLE), "STANDBY") == 0);
    assert(strcmp(cyber_voice_label(99), "STANDBY") == 0);
    assert(cyber_voice_color(99) == CYBER_TEXT_DIM);

    // 每个表情都要有自己的名字，否则界面上会出现两个相同状态。
    for (int i = 0; i < CYBER_EMO_COUNT; i++) {
        assert(cyber_emotion_label((cyber_emotion_t)i)[0] != '\0');
    }
}

static void test_face_grid_matches_render_size(void)
{
    // 面部渲染尺寸必须能被网格整除，否则像素会被半格采样糊掉。
    assert(CYBER_FACE_HOME_PX % CYBER_FACE_GRID_W == 0);
    assert(CYBER_FACE_VOICE_PX % CYBER_FACE_GRID_W == 0);
    assert(CYBER_FACE_HALF_MAX <= CYBER_FACE_GRID_W / 2);
    assert(CYBER_FACE_CENTER == CYBER_FACE_GRID_W / 2);

    // 头部最宽处不得超出屏幕可用宽度。
    const int px_per_cell = CYBER_FACE_HOME_PX / CYBER_FACE_GRID_W;
    assert(2 * CYBER_FACE_HALF_MAX * px_per_cell <= CYBER_SCREEN_W);

    // 状态栏、提示条与面部区域不能重叠。
    assert(CYBER_STATUS_H + CYBER_STATUS_RULE <= CYBER_FACE_HEAD_TOP * px_per_cell +
           CYBER_FACE_HOME_PX);
    assert(CYBER_HINT_TOP > CYBER_STATUS_H);
}

// ---------------------------------------------------------------------------
// 配对（激活）流程
// ---------------------------------------------------------------------------

// "未启用"、"已就绪"与"临时账号"都该放行到主界面；只有真正还没绑定的路径留下。
// TEST_ONLY 放行是有意的：服务端不再要求激活即表示绑定已生效，把它钉在配对页
// 会让一台已经绑好的设备永久不可用（见 cyber_pair_blocks_ui 的注释）。
static void test_pair_blocks_ui_table(void)
{
    assert(!cyber_pair_blocks_ui(CYBER_PAIR_OFF));
    assert(!cyber_pair_blocks_ui(CYBER_PAIR_READY));
    assert(!cyber_pair_blocks_ui(CYBER_PAIR_TEST_ONLY));
    assert(cyber_pair_blocks_ui(CYBER_PAIR_LINKING));
    assert(cyber_pair_blocks_ui(CYBER_PAIR_WAITING));
    assert(cyber_pair_blocks_ui(CYBER_PAIR_OFFLINE));
    assert(cyber_pair_blocks_ui(CYBER_PAIR_ERROR));

    // 除 OFF 外每个状态都要有文字，否则用户分不清"等绑定"和"连不上"。
    for (int i = CYBER_PAIR_OFF + 1; i < CYBER_PAIR_COUNT; i++) {
        const char *label = cyber_pair_label((cyber_pair_state_t)i);
        assert(label != NULL && label[0] != '\0');
    }
    // 越界回落到"查询失败"，绝不读表外。
    assert(strcmp(cyber_pair_label(CYBER_PAIR_COUNT),
                  cyber_pair_label(CYBER_PAIR_ERROR)) == 0);
    assert(cyber_pair_color(CYBER_PAIR_COUNT) == cyber_pair_color(CYBER_PAIR_ERROR));
    assert(cyber_pair_color(999) == cyber_pair_color(CYBER_PAIR_ERROR));
}

static void test_pair_setter(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);

    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "654321");
    assert(m.pair == CYBER_PAIR_WAITING);
    assert(strcmp(m.pair_code, "654321") == 0);

    // NULL 码必须清空：否则绑定生效后上一次的码还留在模型里，用户会以为它仍然有效。
    cyber_model_set_pair(&m, CYBER_PAIR_READY, NULL);
    assert(m.pair_code[0] == '\0');

    // 越界状态回落成"查询失败"，而不是拿去当下标查标签表。
    cyber_model_set_pair(&m, (cyber_pair_state_t)CYBER_PAIR_COUNT, "1");
    assert(m.pair == CYBER_PAIR_ERROR);
    cyber_model_set_pair(&m, (cyber_pair_state_t)999, NULL);
    assert(m.pair == CYBER_PAIR_ERROR);

    // 超长码必须截断且始终以 NUL 结尾。
    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "1234567890");
    assert(strlen(m.pair_code) == CYBER_PAIR_CODE_LEN);

    cyber_model_set_pair(NULL, CYBER_PAIR_WAITING, "123456");
}

static void test_pair_split_code(void)
{
    char r1[CYBER_PAIR_CODE_ROW + 1];
    char r2[CYBER_PAIR_CODE_ROW + 1];

    // 均分两行，顺序必须与输入一致：错位会让用户输到一个不存在的码。
    cyber_pair_split_code("123456", r1, sizeof(r1), r2, sizeof(r2));
    assert(strcmp(r1, "123") == 0);
    assert(strcmp(r2, "456") == 0);

    // 还没拿到码时用 '-' 补齐，两行恒等宽，数字面板不会因码长短而左右跳动。
    cyber_pair_split_code(NULL, r1, sizeof(r1), r2, sizeof(r2));
    assert(strcmp(r1, "---") == 0);
    assert(strcmp(r2, "---") == 0);

    // 半截码：先填第一行，第二行保持占位。
    cyber_pair_split_code("12", r1, sizeof(r1), r2, sizeof(r2));
    assert(strcmp(r1, "12-") == 0);
    assert(strcmp(r2, "---") == 0);

    // 只有一个字符。这条用例的关键在于读 code[1..5] 必须先知道长度，
    // 直接按下标去判断"够不够长"会读到字符串之外。
    cyber_pair_split_code("9", r1, sizeof(r1), r2, sizeof(r2));
    assert(strcmp(r1, "9--") == 0);
    assert(strcmp(r2, "---") == 0);

    // 容量不足时输出空串，绝不写出一个没有结尾的缓冲。
    char tiny[2] = { 'X', 'X' };
    cyber_pair_split_code("123456", tiny, sizeof(tiny), NULL, 0);
    assert(tiny[0] == '\0');

    // 空指针不得崩溃。
    cyber_pair_split_code("123456", NULL, 0, NULL, 0);
}

static void test_activation_gate_on_boot(void)
{
    cyber_ui_model_t m;

    // 未启用配对流程（宿主单测与阶段 1 的默认值）：行为与接入激活前完全一致。
    cyber_model_init(&m);
    assert(m.pair == CYBER_PAIR_OFF);
    assert(m.pair_code[0] == '\0');
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 已绑定：开机动画结束后也直接进主界面，不该再看到配对页。
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_READY, NULL);
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 未绑定：开机动画结束后直接进激活页，不经过主界面。
    // 把配对码藏在一个"看起来正常"的主界面后面，用户只会以为设备坏了。
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "123456");
    assert(m.screen == CYBER_SCREEN_BOOT);
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);
    assert(strcmp(m.pair_code, "123456") == 0);

    // 连服务端都连不上时同样停在激活页，并且不会被误判成"已就绪"。
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_OFFLINE, NULL);
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);
}

static void test_activation_blocks_interaction(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "123456");
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 绑定完成前语音通道根本不可用，短按一律无效。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);
    assert(m.voice == CYBER_VOICE_IDLE);

    // 上/下也不该"顺手"把表情改掉：那会让人以为设备响应了操作。
    assert(cyber_model_button(&m, CYBER_BTN_UP, CYBER_BTN_EV_CLICK) == CYBER_ACT_NONE);
    assert(m.emotion == CYBER_EMO_NEUTRAL);
    assert(cyber_model_button(&m, CYBER_BTN_DOWN, CYBER_BTN_EV_LONG) == CYBER_ACT_NONE);
    assert(m.backlight_user == 100);

    // 一条下行语音事件也不能绕过激活页，把用户带到一个没有音频通道的页面上。
    cyber_model_voice_event(&m, CYBER_VOICE_SPEAKING, CYBER_EMO_HAPPY, "HI");
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 但等待期间必须还能关屏，否则用户只能干等。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_LONG) == CYBER_ACT_NONE);
    assert(advance(&m, 200, 40) == CYBER_ACT_BACKLIGHT);
    assert(m.idle == CYBER_IDLE_OFF);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);
}

static void test_activation_releases_to_home(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "123456");
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 绑定生效后自动放行：用户在控制台输完码，屏幕上不该再需要任何操作。
    cyber_model_set_pair(&m, CYBER_PAIR_READY, NULL);
    cyber_model_tick(&m, 40);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 放行之后语音流程恢复正常，证明激活期间被压住只是"屏幕不放行"，
    // 而不是把状态机改坏了。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
    assert(m.screen == CYBER_SCREEN_VOICE);
}

static void test_activation_returning_from_offline(void)
{
    cyber_ui_model_t m;
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_OFFLINE, NULL);
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 恢复联网 → 正在查询；仍然留在激活页，状态行换成 LINKING。
    cyber_model_set_pair(&m, CYBER_PAIR_LINKING, NULL);
    cyber_model_tick(&m, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 查询失败也不能放行。
    cyber_model_set_pair(&m, CYBER_PAIR_ERROR, NULL);
    cyber_model_tick(&m, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    // 只有真正就绪才放行。
    cyber_model_set_pair(&m, CYBER_PAIR_READY, NULL);
    cyber_model_tick(&m, 40);
    assert(m.screen == CYBER_SCREEN_HOME);
}

// 回归（本机实测症状）：设备在控制台已经绑定成功，但服务端始终只返回临时凭证
// （#1785）。固件曾因为把"只有临时凭证"判成不可用，让这台设备永远停在配对页。
// TEST_ONLY 现在表示"可用，只是走临时账号"，必须放行到主界面。
static void test_test_only_reaches_home(void)
{
    cyber_ui_model_t m;

    // 开机即已知是临时账号：直接进主界面，不再闪一次配对页。
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_TEST_ONLY, NULL);
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_HOME);

    // 从配对页等待中途收到 TEST_ONLY：立刻放行，不必等下一次开机。
    cyber_model_init(&m);
    cyber_model_set_pair(&m, CYBER_PAIR_WAITING, "663840");
    advance(&m, CYBER_BOOT_MS, 40);
    assert(m.screen == CYBER_SCREEN_ACTIVATION);

    cyber_model_set_pair(&m, CYBER_PAIR_TEST_ONLY, NULL);
    cyber_model_tick(&m, 40);
    assert(m.screen == CYBER_SCREEN_HOME);
    // 放行后语音流程必须可用，否则"进主界面"只是把设备换了个地方卡住。
    assert(cyber_model_button(&m, CYBER_BTN_OK, CYBER_BTN_EV_CLICK) == CYBER_ACT_START_LISTEN);
}

int main(void)
{
    test_init_defaults();
    test_boot_progress_math();
    test_boot_log_count();
    test_boot_glitch_window();
    test_blink_window();
    test_wave_level();
    test_pulse();
    test_hud_chase();
    test_visor_scan_index();
    test_speak_duration();
    test_face_half_width();
    test_face_head_bands();
    test_boot_to_home();
    test_home_navigation();
    test_voice_flow_offline();
    test_voice_button_interrupts();
    test_voice_event_interface();
    test_error_state_recovers();
    test_backlight_ladder();
    test_idle_dim_and_off();
    test_idle_ok_long_powers_off();
    test_idle_recovery_keeps_user_level();
    test_battery_and_link_queries();
    test_lookup_tables_cover_enum();
    test_face_grid_matches_render_size();
    test_pair_blocks_ui_table();
    test_pair_setter();
    test_pair_split_code();
    test_activation_gate_on_boot();
    test_activation_blocks_interaction();
    test_activation_releases_to_home();
    test_activation_returning_from_offline();
    test_test_only_reaches_home();

    printf("test_cyber_ui_model: PASS\n");
    return 0;
}
