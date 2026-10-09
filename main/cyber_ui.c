// main/cyber_ui.c
// 界面装配与增量刷新。
//
// 这里只负责"把控件放到 cyber_layout.h 声明的矩形里，并按模型改属性"，
// 不自己算坐标：坐标一旦散落在 lv_* 调用里，圆角遮罩安全区就没法在宿主上验证。
#include "cyber_ui.h"

#include <string.h>

#include "bsp_display.h"
#include "bsp_pins.h"
#include "cyber_face.h"
#include "cyber_layout.h"
#include "cyber_theme.h"
#include "cyber_vis.h"

// 字体符号由 assets/fonts/generated 下的 .c 提供（见 assets/README.md）。
LV_FONT_DECLARE(cyber_font_sm);
LV_FONT_DECLARE(cyber_font_md);
LV_FONT_DECLARE(cyber_font_title);
LV_FONT_DECLARE(cyber_font_title_lg);
// 仅含 0-9 与 '-' ':'：配对码专用，别拿它渲染正文（缺字会静默地什么都不画）。
LV_FONT_DECLARE(cyber_font_code);

// 布局常量是 BSP 硬件的副本（纯逻辑层不能 include LVGL 头），用静态断言钉住，
// 换屏幕或改圆角半径时编译就失败，而不是等到上机才发现内容被切掉。
_Static_assert(CYBER_SCREEN_W == BSP_LCD_W && CYBER_SCREEN_H == BSP_LCD_H,
               "cyber_theme.h 的屏幕尺寸与 bsp_pins.h 不一致");
_Static_assert(CYBER_SCREEN_RADIUS == BSP_LVGL_SCREEN_RADIUS,
               "cyber_layout.h 的圆角半径与 BSP 不一致");

// 开机自检行。这是给用户看的启动氛围，同时也大致对应真正初始化过的子系统；
// 阶段 1 没有音频与网络，所以末行说明它们处于待命而不是"已就绪"。
static const char *const BOOT_LOGS[CYBER_BOOT_LOG_LINES] = {
    "> BOOT SEQUENCE",
    "> LCD 240X320 OK",
    "> PIXEL CORE OK",
    "> LINK STANDBY",
};

// ---------------------------------------------------------------------------
struct cyber_ui {
    lv_obj_t *screen;
    lv_obj_t *bg;
    lv_obj_t *glitch;      // 置顶的抖动层

    lv_obj_t *boot_page;
    lv_obj_t *home_page;
    lv_obj_t *voice_page;
    lv_obj_t *pair_page;

    // 共用状态栏（电量固定右上角）
    lv_obj_t *link_label;
    lv_obj_t *batt_pct;
    lv_obj_t *batt_fill;

    lv_obj_t *hint;

    // 开机页
    lv_obj_t *boot_title;
    lv_obj_t *boot_title2;
    lv_obj_t *boot_sub;
    lv_obj_t *boot_bar_fill;
    lv_obj_t *boot_pct;
    lv_obj_t *boot_logs[CYBER_BOOT_LOG_LINES];

    // 主界面
    lv_obj_t *home_face;
    lv_obj_t *home_mode;

    // 语音页
    lv_obj_t *voice_label;
    lv_obj_t *voice_face;
    lv_obj_t *voice_vis;
    lv_obj_t *voice_sub;

    // 激活页
    lv_obj_t *pair_title;
    lv_obj_t *pair_frame;
    lv_obj_t *pair_rule;
    lv_obj_t *pair_row1;
    lv_obj_t *pair_row2;
    lv_obj_t *pair_state;
    lv_obj_t *pair_scan_track;
    lv_obj_t *pair_scan_head;

    // 上次已应用值：用于跳过无变化的重设
    struct {
        bool valid;
        cyber_screen_t screen;
        cyber_voice_state_t voice;
        cyber_emotion_t emotion;
        int battery_soc;
        bool net_connected;
        uint8_t boot_progress;
        uint8_t boot_logs;
        cyber_pair_state_t pair;
        char pair_code[CYBER_PAIR_CODE_LEN + 1];
        int pair_scan;
        char subtitle[CYBER_SUBTITLE_MAX + 1];
    } applied;
};

// ---------------------------------------------------------------------------
// 控件构造小工具
// ---------------------------------------------------------------------------
static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// 透明无样式的全屏容器，作为页面底板。
static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    if (page == NULL) return NULL;
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, CYBER_SCREEN_W, CYBER_SCREEN_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

// 实心矩形（无圆角，像素风）。
static lv_obj_t *make_rect(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) return NULL;
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

// 把标签放进 cyber_layout.h 声明的矩形。长文本一律 CLIP：
// 宁可截断也不能换行把高度撑开，否则会压到下面的元素上。
static void place_label(lv_obj_t *label, int x, int y, int w, int h, lv_text_align_t align)
{
    if (label == NULL) return;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
}

// 顶部/底部之间的横向霓虹渐变线：赛博朋克最省事也最有效的"氛围"元素。
static lv_obj_t *make_neon_rule(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *rule = make_rect(parent, x, y, w, h, CYBER_CYAN, LV_OPA_COVER);
    if (rule == NULL) return NULL;
    lv_obj_set_style_bg_grad_color(rule, lv_color_hex(CYBER_MAGENTA), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(rule, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    return rule;
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------
static void build_statusbar(cyber_ui_t *ui)
{
    // 满幅底板：让它被圆角切掉，观感才是"屏幕本身是圆的"。
    make_rect(ui->screen, 0, 0, CYBER_STATUSBAR_W, CYBER_STATUSBAR_H,
              CYBER_PANEL, LV_OPA_COVER);

    ui->link_label = make_label(ui->screen, &cyber_font_sm, CYBER_TEXT_DIM);
    place_label(ui->link_label, CYBER_LINK_X, CYBER_LINK_Y, CYBER_LINK_W,
                CYBER_LINK_H, LV_TEXT_ALIGN_LEFT);

    ui->batt_pct = make_label(ui->screen, &cyber_font_sm, CYBER_TEXT_DIM);
    place_label(ui->batt_pct, CYBER_BATT_PCT_X, CYBER_BATT_PCT_Y, CYBER_BATT_PCT_W,
                CYBER_BATT_PCT_H, LV_TEXT_ALIGN_RIGHT);

    // 电池外形：壳体 + 正极凸点 + 内部电量条。用控件拼而不是字体图标，
    // 这样不必为了一个图标往字库里塞 LV_SYMBOL_*，也就没有缺字风险。
    lv_obj_t *body = make_rect(ui->screen, CYBER_BATT_BODY_X, CYBER_BATT_BODY_Y,
                               CYBER_BATT_BODY_W, CYBER_BATT_BODY_H,
                               CYBER_BG, LV_OPA_COVER);
    if (body != NULL) {
        lv_obj_set_style_border_width(body, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(body, lv_color_hex(CYBER_TEXT_DIM), LV_PART_MAIN);
        lv_obj_set_style_border_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    }
    make_rect(ui->screen, CYBER_BATT_NUB_X, CYBER_BATT_NUB_Y, CYBER_BATT_NUB_W,
              CYBER_BATT_NUB_H, CYBER_TEXT_DIM, LV_OPA_COVER);

    ui->batt_fill = make_rect(ui->screen, CYBER_BATT_FILL_X, CYBER_BATT_FILL_Y, 0,
                              CYBER_BATT_FILL_H, CYBER_LIME, LV_OPA_COVER);

    make_neon_rule(ui->screen, CYBER_STATUS_RULE_X, CYBER_STATUS_RULE_Y,
                   CYBER_STATUS_RULE_W, CYBER_STATUS_RULE_H);
}

// 电量：读不到（-1）时显示 "--" 并藏掉填充条，绝不编造一个数字。
static void update_battery(cyber_ui_t *ui, int soc)
{
    if (ui->batt_pct == NULL) return;

    if (soc < 0) {
        lv_label_set_text(ui->batt_pct, "--");
        lv_obj_set_style_text_color(ui->batt_pct, lv_color_hex(CYBER_TEXT_DIM), LV_PART_MAIN);
        set_hidden(ui->batt_fill, true);
        return;
    }

    lv_label_set_text_fmt(ui->batt_pct, "%d%%", soc);
    lv_obj_set_style_text_color(ui->batt_pct, lv_color_hex(CYBER_TEXT), LV_PART_MAIN);

    if (ui->batt_fill == NULL) return;

    const uint32_t color = (soc >= 50) ? CYBER_LIME : (soc >= 20) ? CYBER_AMBER : CYBER_RED;
    lv_obj_set_style_bg_color(ui->batt_fill, lv_color_hex(color), LV_PART_MAIN);

    if (soc == 0) {
        set_hidden(ui->batt_fill, true);
        return;
    }
    set_hidden(ui->batt_fill, false);
    int w = CYBER_BATT_FILL_W * soc / 100;
    if (w < 1) w = 1;
    lv_obj_set_width(ui->batt_fill, w);
}

// ---------------------------------------------------------------------------
// 底部提示条
// ---------------------------------------------------------------------------
static void build_hint(cyber_ui_t *ui)
{
    make_rect(ui->screen, CYBER_HINT_RULE_X, CYBER_HINT_RULE_Y, CYBER_HINT_RULE_W,
              CYBER_HINT_RULE_H, CYBER_GRID, LV_OPA_COVER);

    ui->hint = make_label(ui->screen, &cyber_font_sm, CYBER_TEXT_DIM);
    place_label(ui->hint, CYBER_HINT_X, CYBER_HINT_Y, CYBER_HINT_W, CYBER_HINT_H,
                LV_TEXT_ALIGN_CENTER);
}

static const char *hint_for(const cyber_ui_model_t *m)
{
    switch (m->screen) {
    case CYBER_SCREEN_BOOT:
        // 等首个 OTA 结果时底部要写清"在等哪一步"：进度条会按真实阶段停留几秒，
        // 一句说明比写死 SYSTEM BOOT 更能让人确信设备在干活。文案上限受
        // CYBER_HINT_W 约束（sm 字体 7px/字符，208px ≈ 28 字符）。
        return cyber_boot_waiting(m) ? cyber_boot_link_label(m->boot_link)
                                     : "SYSTEM BOOT";
    case CYBER_SCREEN_ACTIVATION:
        // 有码时提示用户去哪儿输；没码时不做无意义的指引——这时真正的问题是链路。
        // 文案长度上限受 CYBER_HINT_W 约束（sm 字体 7px/字符，208px ≈ 28 字符）。
        return (m->pair_code[0] != '\0') ? "ENTER CODE AT XIAOZHI.ME"
                                         : "WAITING FOR SERVER";
    case CYBER_SCREEN_VOICE:
        // 文案长度上限受 CYBER_HINT_W 约束（sm 字体 7px/字符，208px ≈ 28 字符）。
        switch (m->voice) {
        case CYBER_VOICE_LISTENING: return "OK DONE  HOLD OK EXIT";
        case CYBER_VOICE_THINKING:  return "OK CANCEL  HOLD OK EXIT";
        case CYBER_VOICE_SPEAKING:  return "OK STOP  HOLD OK EXIT";
        case CYBER_VOICE_ERROR:     return "OK BACK";
        default:                    return "HOLD OK EXIT";
        }
    case CYBER_SCREEN_HOME:
    default:
        return "OK TALK  UP/DN FACE";
    }
}

// ---------------------------------------------------------------------------
// 开机页
// ---------------------------------------------------------------------------
static void build_boot_page(cyber_ui_t *ui)
{
    lv_obj_t *page = make_page(ui->screen);
    ui->boot_page = page;
    if (page == NULL) return;

    // 双色霓虹 logo：青 + 品红，是整套配色里最"赛博"的一处。
    ui->boot_title = make_label(page, &cyber_font_title_lg, CYBER_CYAN);
    lv_label_set_text(ui->boot_title, "CYBER");
    place_label(ui->boot_title, CYBER_BOOT_TITLE_X, CYBER_BOOT_TITLE_Y,
                CYBER_BOOT_TITLE_W, CYBER_BOOT_TITLE_H, LV_TEXT_ALIGN_CENTER);

    ui->boot_title2 = make_label(page, &cyber_font_title_lg, CYBER_MAGENTA);
    lv_label_set_text(ui->boot_title2, "PIXEL");
    place_label(ui->boot_title2, CYBER_BOOT_TITLE_X, CYBER_BOOT_TITLE2_Y,
                CYBER_BOOT_TITLE_W, CYBER_BOOT_TITLE_H, LV_TEXT_ALIGN_CENTER);

    ui->boot_sub = make_label(page, &cyber_font_title, CYBER_TEXT);
    lv_label_set_text(ui->boot_sub, "AI ASSISTANT");
    place_label(ui->boot_sub, CYBER_BOOT_SUB_X, CYBER_BOOT_SUB_Y,
                CYBER_BOOT_SUB_W, CYBER_BOOT_SUB_H, LV_TEXT_ALIGN_CENTER);

    make_neon_rule(page, CYBER_BOOT_DIV_X, CYBER_BOOT_DIV_Y, CYBER_BOOT_DIV_W,
                   CYBER_BOOT_DIV_H);

    // 进度条：底槽 + 填充。填充宽度按进度算，另配一个百分比读数。
    lv_obj_t *bar = make_rect(page, CYBER_BOOT_BAR_X, CYBER_BOOT_BAR_Y,
                              CYBER_BOOT_BAR_W, CYBER_BOOT_BAR_H,
                              CYBER_PANEL, LV_OPA_COVER);
    if (bar != NULL) {
        lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_hex(CYBER_GRID), LV_PART_MAIN);
        lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    }

    ui->boot_bar_fill = make_rect(page, CYBER_BOOT_BAR_X + 2, CYBER_BOOT_BAR_Y + 2, 0,
                                  CYBER_BOOT_BAR_INNER_H, CYBER_CYAN, LV_OPA_COVER);

    ui->boot_pct = make_label(page, &cyber_font_sm, CYBER_CYAN);
    lv_label_set_text(ui->boot_pct, "0%");
    place_label(ui->boot_pct, CYBER_BOOT_PCT_X, CYBER_BOOT_PCT_Y, CYBER_BOOT_PCT_W,
                CYBER_BOOT_PCT_H, LV_TEXT_ALIGN_CENTER);

    for (int i = 0; i < CYBER_BOOT_LOG_LINES; i++) {
        ui->boot_logs[i] = make_label(page, &cyber_font_sm, CYBER_TEXT_DIM);
        lv_label_set_text(ui->boot_logs[i], BOOT_LOGS[i]);
        place_label(ui->boot_logs[i], CYBER_BOOT_LOG_X,
                    CYBER_BOOT_LOG_Y + i * CYBER_BOOT_LOG_STEP,
                    CYBER_BOOT_LOG_W, CYBER_BOOT_LOG_H, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_flag(ui->boot_logs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 开机页只播一次。用完即删，连同 4 条日志一起释放 LVGL 内部堆；
// 删完必须把所有指针清空，否则后续 apply 会写到已释放对象上。
static void destroy_boot_page(cyber_ui_t *ui)
{
    if (ui->boot_page == NULL) return;
    lv_obj_delete(ui->boot_page);
    ui->boot_page = NULL;
    ui->boot_title = NULL;
    ui->boot_title2 = NULL;
    ui->boot_sub = NULL;
    ui->boot_bar_fill = NULL;
    ui->boot_pct = NULL;
    for (int i = 0; i < CYBER_BOOT_LOG_LINES; i++) ui->boot_logs[i] = NULL;
}

// ---------------------------------------------------------------------------
// 主界面
// ---------------------------------------------------------------------------
static void build_home_page(cyber_ui_t *ui)
{
    lv_obj_t *page = make_page(ui->screen);
    ui->home_page = page;
    if (page == NULL) return;

    lv_obj_t *title = make_label(page, &cyber_font_title, CYBER_TEXT);
    lv_label_set_text(title, "CYBER.PIXEL");
    place_label(title, CYBER_HOME_TITLE_X, CYBER_HOME_TITLE_Y, CYBER_HOME_TITLE_W,
                CYBER_HOME_TITLE_H, LV_TEXT_ALIGN_CENTER);

    ui->home_face = cyber_face_create(page, CYBER_FACE_HOME_PX);
    if (ui->home_face != NULL) {
        lv_obj_set_pos(ui->home_face, CYBER_HOME_FACE_X, CYBER_HOME_FACE_Y);
    }

    // 表情名：不接服务器时，这一行是逐个检查 7 种表情的依据。
    ui->home_mode = make_label(page, &cyber_font_md, CYBER_CYAN);
    lv_label_set_text(ui->home_mode, "MODE NEUTRAL");
    place_label(ui->home_mode, CYBER_HOME_MODE_X, CYBER_HOME_MODE_Y, CYBER_HOME_MODE_W,
                CYBER_HOME_MODE_H, LV_TEXT_ALIGN_CENTER);
}

// ---------------------------------------------------------------------------
// 语音页
// ---------------------------------------------------------------------------
static void build_voice_page(cyber_ui_t *ui)
{
    lv_obj_t *page = make_page(ui->screen);
    ui->voice_page = page;
    if (page == NULL) return;

    ui->voice_label = make_label(page, &cyber_font_title, CYBER_AMBER);
    lv_label_set_text(ui->voice_label, cyber_voice_label(CYBER_VOICE_LISTENING));
    place_label(ui->voice_label, CYBER_VOICE_LABEL_X, CYBER_VOICE_LABEL_Y,
                CYBER_VOICE_LABEL_W, CYBER_VOICE_LABEL_H, LV_TEXT_ALIGN_CENTER);

    ui->voice_face = cyber_face_create(page, CYBER_FACE_VOICE_PX);
    if (ui->voice_face != NULL) {
        lv_obj_set_pos(ui->voice_face, CYBER_VOICE_FACE_X, CYBER_VOICE_FACE_Y);
    }

    ui->voice_vis = cyber_vis_create(page, CYBER_VOICE_VIS_W, CYBER_VOICE_VIS_H);
    if (ui->voice_vis != NULL) {
        lv_obj_set_pos(ui->voice_vis, CYBER_VOICE_VIS_X, CYBER_VOICE_VIS_Y);
    }

    // 字幕是唯一允许换行的标签：给固定宽度 + 居中，最多两行（见 CYBER_VOICE_SUB_H）。
    ui->voice_sub = make_label(page, &cyber_font_md, CYBER_TEXT);
    lv_label_set_long_mode(ui->voice_sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(ui->voice_sub, CYBER_VOICE_SUB_X, CYBER_VOICE_SUB_Y);
    lv_obj_set_width(ui->voice_sub, CYBER_VOICE_SUB_W);
    lv_obj_set_style_text_align(ui->voice_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

// ---------------------------------------------------------------------------
// 激活页
// ---------------------------------------------------------------------------
// 未绑定时的唯一界面：把 6 位配对码放到最大，其余元素都退成陪衬。
// 常驻控件刻意控制在 8 个：LVGL 内部堆只有 24KB，装饰性控件（比如四角 L 形括号）
// 每多一个都在跟字形抢内存，而它们在 240x320 上并不比一圈细边框更好看。
static void build_activation_page(cyber_ui_t *ui)
{
    lv_obj_t *page = make_page(ui->screen);
    ui->pair_page = page;
    if (page == NULL) return;

    ui->pair_title = make_label(page, &cyber_font_title, CYBER_CYAN);
    lv_label_set_text(ui->pair_title, "PAIRING CODE");
    place_label(ui->pair_title, CYBER_PAIR_TITLE_X, CYBER_PAIR_TITLE_Y,
                CYBER_PAIR_TITLE_W, CYBER_PAIR_TITLE_H, LV_TEXT_ALIGN_CENTER);

    ui->pair_frame = make_rect(page, CYBER_PAIR_FRAME_X, CYBER_PAIR_FRAME_Y,
                               CYBER_PAIR_FRAME_W, CYBER_PAIR_FRAME_H,
                               CYBER_PANEL, LV_OPA_COVER);
    if (ui->pair_frame != NULL) {
        lv_obj_set_style_border_width(ui->pair_frame, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(ui->pair_frame, lv_color_hex(CYBER_CYAN), LV_PART_MAIN);
        lv_obj_set_style_border_opa(ui->pair_frame, LV_OPA_COVER, LV_PART_MAIN);
    }

    // 整屏唯一一处双色渐变：把视线钉在数字上。
    ui->pair_rule = make_neon_rule(page, CYBER_PAIR_RULE_X, CYBER_PAIR_RULE_Y,
                                   CYBER_PAIR_RULE_W, CYBER_PAIR_RULE_H);

    // 两行数字。文本由 cyber_pair_split_code 保证恒为 3 字符，两行等宽、不跳动。
    ui->pair_row1 = make_label(page, &cyber_font_code, CYBER_CYAN);
    place_label(ui->pair_row1, CYBER_PAIR_ROW_X, CYBER_PAIR_ROW1_Y,
                CYBER_PAIR_ROW_W, CYBER_PAIR_ROW_H, LV_TEXT_ALIGN_CENTER);

    ui->pair_row2 = make_label(page, &cyber_font_code, CYBER_CYAN);
    place_label(ui->pair_row2, CYBER_PAIR_ROW_X, CYBER_PAIR_ROW2_Y,
                CYBER_PAIR_ROW_W, CYBER_PAIR_ROW_H, LV_TEXT_ALIGN_CENTER);

    ui->pair_state = make_label(page, &cyber_font_sm, CYBER_TEXT_DIM);
    place_label(ui->pair_state, CYBER_PAIR_STATE_X, CYBER_PAIR_STATE_Y,
                CYBER_PAIR_STATE_W, CYBER_PAIR_STATE_H, LV_TEXT_ALIGN_CENTER);

    // 扫描条：暗轨 + 来回滑动的亮块。用一个移动的块而不是一排跑马灯，
    // 是为了把常驻控件数从十几个压到 2 个。位置由模型给，这里不做推导。
    ui->pair_scan_track = make_rect(page, CYBER_PAIR_SCAN_X, CYBER_PAIR_SCAN_Y,
                                    CYBER_PAIR_SCAN_W, CYBER_PAIR_SCAN_H,
                                    CYBER_GRID, LV_OPA_COVER);
    ui->pair_scan_head = make_rect(page, CYBER_PAIR_SCAN_X, CYBER_PAIR_SCAN_Y,
                                   CYBER_PAIR_SCAN_HEAD_W, CYBER_PAIR_SCAN_H,
                                   CYBER_CYAN, LV_OPA_COVER);
}

// ---------------------------------------------------------------------------
// 建/删
// ---------------------------------------------------------------------------
cyber_ui_t *cyber_ui_create(void)
{
    cyber_ui_t *ui = (cyber_ui_t *)lv_malloc(sizeof(*ui));
    if (ui == NULL) return NULL;
    memset(ui, 0, sizeof(*ui));

    ui->screen = lv_obj_create(NULL);
    if (ui->screen == NULL) {
        lv_free(ui);
        return NULL;
    }
    lv_obj_remove_style_all(ui->screen);
    lv_obj_set_size(ui->screen, CYBER_SCREEN_W, CYBER_SCREEN_H);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(CYBER_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ui->screen, LV_OBJ_FLAG_SCROLLABLE);

    // 层级从低到高：背景网格 → 四个页面 → 状态栏/提示条 → 抖动层。
    ui->bg = cyber_bg_create(ui->screen, CYBER_SCREEN_W, CYBER_SCREEN_H);
    build_boot_page(ui);
    build_home_page(ui);
    build_voice_page(ui);
    build_activation_page(ui);
    build_statusbar(ui);
    build_hint(ui);
    ui->glitch = cyber_glitch_create(ui->screen, CYBER_SCREEN_W, CYBER_SCREEN_H);

    set_hidden(ui->home_page, true);
    set_hidden(ui->voice_page, true);
    set_hidden(ui->pair_page, true);

    lv_screen_load(ui->screen);
    return ui;
}

void cyber_ui_delete(cyber_ui_t *ui)
{
    if (ui == NULL) return;
    // 删除屏幕会级联释放全部子对象（含自绘控件在 DELETE 事件里释放的上下文）。
    if (ui->screen != NULL) lv_obj_delete(ui->screen);
    lv_free(ui);
}

// ---------------------------------------------------------------------------
// 增量刷新
// ---------------------------------------------------------------------------
void cyber_ui_apply(cyber_ui_t *ui, const cyber_ui_model_t *m)
{
    if (ui == NULL || m == NULL) return;

    const bool first = !ui->applied.valid;
    const bool screen_changed = first || ui->applied.screen != m->screen;
    const cyber_emotion_t emo = cyber_model_visible_emotion(m);

    // --- 页面可见性 ---
    if (screen_changed) {
        if (m->screen == CYBER_SCREEN_BOOT) set_hidden(ui->boot_page, false);
        else destroy_boot_page(ui);              // 开机动画播完即回收
        set_hidden(ui->home_page, m->screen != CYBER_SCREEN_HOME);
        set_hidden(ui->voice_page, m->screen != CYBER_SCREEN_VOICE);
        set_hidden(ui->pair_page, m->screen != CYBER_SCREEN_ACTIVATION);
    }

    // --- 状态栏 ---
    if (first || ui->applied.battery_soc != m->battery_soc) {
        update_battery(ui, m->battery_soc);
    }
    if (first || ui->applied.net_connected != m->net_connected) {
        lv_label_set_text(ui->link_label, cyber_model_link_label(m));
        lv_obj_set_style_text_color(ui->link_label,
            lv_color_hex(m->net_connected ? CYBER_CYAN : CYBER_TEXT_DIM), LV_PART_MAIN);
    }
    if (first || screen_changed || ui->applied.voice != m->voice ||
        ui->applied.pair != m->pair) {
        lv_label_set_text(ui->hint, hint_for(m));
    }

    // --- 抖动层：只在开机自检阶段出现。故障态交给脸自己的错位色带，
    //     否则整屏闪动会盖住错误信息，反而看不清。 ---
    const bool glitch = (m->screen == CYBER_SCREEN_BOOT) &&
                        cyber_boot_glitch(m->screen_ms, CYBER_BOOT_MS);
    cyber_glitch_render(ui->glitch, glitch, m->anim_ms);

    // --- 开机页 ---
    if (m->screen == CYBER_SCREEN_BOOT && ui->boot_page != NULL) {
        // 进度只增不减，且不再靠"呼吸"来回动：抖动/回缩恰恰是这次要消除的观感。
        // 进度读的是模型里锁存过的 boot_pct，它在等待期由真实链路阶段抬升。
        const uint8_t progress = cyber_boot_meter(m);
        if (first || progress != ui->applied.boot_progress) {
            lv_obj_set_width(ui->boot_bar_fill,
                             CYBER_BOOT_BAR_INNER_W * progress / 100u);
            lv_label_set_text_fmt(ui->boot_pct, "%u%%", (unsigned)progress);
            ui->applied.boot_progress = progress;
        }

        const uint8_t logs = cyber_boot_log_count(m->screen_ms, CYBER_BOOT_MS,
                                                 CYBER_BOOT_LOG_LINES);
        if (first || logs != ui->applied.boot_logs) {
            for (int i = 0; i < CYBER_BOOT_LOG_LINES; i++) {
                set_hidden(ui->boot_logs[i], i >= (int)logs);
            }
            ui->applied.boot_logs = logs;
        }
    }

    // --- 主界面 ---
    if (m->screen == CYBER_SCREEN_HOME) {
        cyber_face_render(ui->home_face, emo, m->anim_ms, false);
        if (first || ui->applied.emotion != emo) {
            lv_label_set_text_fmt(ui->home_mode, "MODE %s", cyber_emotion_label(emo));
            lv_obj_set_style_text_color(ui->home_mode,
                lv_color_hex(cyber_emotion_color(emo)), LV_PART_MAIN);
        }
    }

    // --- 语音页 ---
    if (m->screen == CYBER_SCREEN_VOICE) {
        const bool speaking = (m->voice == CYBER_VOICE_SPEAKING);
        cyber_face_render(ui->voice_face, emo, m->anim_ms, speaking);

        if (first || ui->applied.voice != m->voice) {
            lv_label_set_text(ui->voice_label, cyber_voice_label(m->voice));
            lv_obj_set_style_text_color(ui->voice_label,
                lv_color_hex(cyber_voice_color(m->voice)), LV_PART_MAIN);
        }

        cyber_vis_mode_t vis = CYBER_VIS_NONE;
        switch (m->voice) {
        case CYBER_VOICE_LISTENING: vis = CYBER_VIS_WAVE;     break;
        case CYBER_VOICE_THINKING:  vis = CYBER_VIS_SCAN;     break;
        case CYBER_VOICE_SPEAKING:  vis = CYBER_VIS_SPECTRUM; break;
        default:                    vis = CYBER_VIS_NONE;     break;
        }
        cyber_vis_render(ui->voice_vis, vis, m->anim_ms);

        if (first || strcmp(ui->applied.subtitle, m->subtitle) != 0) {
            lv_label_set_text(ui->voice_sub, m->subtitle);
            strncpy(ui->applied.subtitle, m->subtitle, sizeof(ui->applied.subtitle) - 1u);
            ui->applied.subtitle[sizeof(ui->applied.subtitle) - 1u] = '\0';
        }
    }

    // --- 激活页 ---
    if (m->screen == CYBER_SCREEN_ACTIVATION) {
        // screen_changed 必须参与判定：从开机页切进来时 pair/pair_code 往往
        // 已经等于模型当前值（开机那 2.6s 里它们就被记录过了），少了这一项
        // 数字标签会一次都不写，屏幕上是空的。
        if (first || screen_changed || ui->applied.pair != m->pair ||
            strcmp(ui->applied.pair_code, m->pair_code) != 0) {
            char row1[CYBER_PAIR_CODE_ROW + 1];
            char row2[CYBER_PAIR_CODE_ROW + 1];
            cyber_pair_split_code(m->pair_code, row1, sizeof(row1), row2, sizeof(row2));

            lv_label_set_text(ui->pair_row1, row1);
            lv_label_set_text(ui->pair_row2, row2);
            lv_label_set_text(ui->pair_state, cyber_pair_label(m->pair));

            // 数字、状态行、面板边框共用一个强调色：一眼就能看出当前是"等绑定"
            // 还是"连不上"，不必去读那行小字。
            const uint32_t color = cyber_pair_color(m->pair);
            lv_obj_set_style_text_color(ui->pair_row1, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_text_color(ui->pair_row2, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_text_color(ui->pair_state, lv_color_hex(color), LV_PART_MAIN);
            if (ui->pair_frame != NULL) {
                lv_obj_set_style_border_color(ui->pair_frame, lv_color_hex(color),
                                              LV_PART_MAIN);
            }
        }

        // 扫描条只在位置真的变了才 set：lv_obj_set_x 每次都会 invalidate，
        // 每帧无脑调用会让静止画面也持续重绘。
        const int scan = cyber_visor_scan_index(m->anim_ms, CYBER_PAIR_SCAN_SPAN);
        if (first || scan != ui->applied.pair_scan) {
            lv_obj_set_x(ui->pair_scan_head, CYBER_PAIR_SCAN_X + scan);
            ui->applied.pair_scan = scan;
        }
    }

    // --- 记录本次已应用值 ---
    ui->applied.screen = m->screen;
    ui->applied.voice = m->voice;
    ui->applied.emotion = emo;
    ui->applied.battery_soc = m->battery_soc;
    ui->applied.net_connected = m->net_connected;
    ui->applied.pair = m->pair;
    strncpy(ui->applied.pair_code, m->pair_code, sizeof(ui->applied.pair_code) - 1u);
    ui->applied.pair_code[sizeof(ui->applied.pair_code) - 1u] = '\0';
    ui->applied.valid = true;
}
