// tests/test_cyber_ui_layout.c
// cyber_layout.h 的宿主单测：验证每个元素都画在"屏幕圆角遮罩切不到"的地方。
//
// 这个测试存在的理由很具体：屏幕四角有 30px 圆角遮罩，而状态栏顶格贴边。
// 用裸眼估 8px 边距看起来没问题，实际在 y=6 处可见区域已经收到 x<=227，
// 电池正极凸点会被切掉。几何是可算的，就不该靠上机才发现。
//
// 复用 BSP 的 bsp_display_rounded_row_span()，而不是在测试里重写一遍遮罩公式：
// 重写等于把被验证的逻辑又复制一份，两边一旦漂移测试就失去意义。
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "bsp_display_rounding.h"
#include "cyber_layout.h"
#include "cyber_theme.h"

typedef struct {
    const char *name;
    int x, y, w, h;
} rect_t;

#define R(name, x_, y_, w_, h_) { name, (x_), (y_), (w_), (h_) }

// 状态栏与提示条：紧贴上下边缘，必须逐行过遮罩检查。
static const rect_t EDGE_CONTENT[] = {
    R("status.link",     CYBER_LINK_X,      CYBER_LINK_Y,      CYBER_LINK_W,      CYBER_LINK_H),
    R("status.batt_pct", CYBER_BATT_PCT_X,  CYBER_BATT_PCT_Y,  CYBER_BATT_PCT_W,  CYBER_BATT_PCT_H),
    R("status.batt_body", CYBER_BATT_BODY_X, CYBER_BATT_BODY_Y, CYBER_BATT_BODY_W, CYBER_BATT_BODY_H),
    R("status.batt_nub", CYBER_BATT_NUB_X,  CYBER_BATT_NUB_Y,  CYBER_BATT_NUB_W,  CYBER_BATT_NUB_H),
    R("status.batt_fill", CYBER_BATT_FILL_X, CYBER_BATT_FILL_Y, CYBER_BATT_FILL_W, CYBER_BATT_FILL_H),
    R("status.vol_pct",   CYBER_VOL_PCT_X,   CYBER_LINK_Y,      CYBER_VOL_PCT_W,   CYBER_LINK_H),
    R("status.rule",     CYBER_STATUS_RULE_X, CYBER_STATUS_RULE_Y, CYBER_STATUS_RULE_W, CYBER_STATUS_RULE_H),
    R("hint.rule",       CYBER_HINT_RULE_X, CYBER_HINT_RULE_Y, CYBER_HINT_RULE_W, CYBER_HINT_RULE_H),
    R("hint.text",       CYBER_HINT_X,      CYBER_HINT_Y,      CYBER_HINT_W,      CYBER_HINT_H),
};

// 中段页面内容：整块都必须落在直边安全区内。
static const rect_t MID_CONTENT[] = {
    R("boot.title",  CYBER_BOOT_TITLE_X,  CYBER_BOOT_TITLE_Y,  CYBER_BOOT_TITLE_W, CYBER_BOOT_TITLE_H),
    R("boot.title2", CYBER_BOOT_TITLE_X,  CYBER_BOOT_TITLE2_Y, CYBER_BOOT_TITLE_W, CYBER_BOOT_TITLE_H),
    R("boot.sub",    CYBER_BOOT_SUB_X,    CYBER_BOOT_SUB_Y,    CYBER_BOOT_SUB_W,   CYBER_BOOT_SUB_H),
    R("boot.div",    CYBER_BOOT_DIV_X,    CYBER_BOOT_DIV_Y,    CYBER_BOOT_DIV_W,   CYBER_BOOT_DIV_H),
    R("boot.bar",    CYBER_BOOT_BAR_X,    CYBER_BOOT_BAR_Y,    CYBER_BOOT_BAR_W,   CYBER_BOOT_BAR_H),
    R("boot.pct",    CYBER_BOOT_PCT_X,    CYBER_BOOT_PCT_Y,    CYBER_BOOT_PCT_W,   CYBER_BOOT_PCT_H),
    R("boot.log0",   CYBER_BOOT_LOG_X,    CYBER_BOOT_LOG_Y + 0 * CYBER_BOOT_LOG_STEP, CYBER_BOOT_LOG_W, CYBER_BOOT_LOG_H),
    R("boot.log1",   CYBER_BOOT_LOG_X,    CYBER_BOOT_LOG_Y + 1 * CYBER_BOOT_LOG_STEP, CYBER_BOOT_LOG_W, CYBER_BOOT_LOG_H),
    R("boot.log2",   CYBER_BOOT_LOG_X,    CYBER_BOOT_LOG_Y + 2 * CYBER_BOOT_LOG_STEP, CYBER_BOOT_LOG_W, CYBER_BOOT_LOG_H),
    R("boot.log3",   CYBER_BOOT_LOG_X,    CYBER_BOOT_LOG_Y + 3 * CYBER_BOOT_LOG_STEP, CYBER_BOOT_LOG_W, CYBER_BOOT_LOG_H),
    R("home.title",  CYBER_HOME_TITLE_X,  CYBER_HOME_TITLE_Y,  CYBER_HOME_TITLE_W, CYBER_HOME_TITLE_H),
    R("home.face",   CYBER_HOME_FACE_X,   CYBER_HOME_FACE_Y,   CYBER_FACE_HOME_PX, CYBER_FACE_HOME_PX),
    R("home.mode",   CYBER_HOME_MODE_X,   CYBER_HOME_MODE_Y,   CYBER_HOME_MODE_W,  CYBER_HOME_MODE_H),
    R("voice.label", CYBER_VOICE_LABEL_X, CYBER_VOICE_LABEL_Y, CYBER_VOICE_LABEL_W, CYBER_VOICE_LABEL_H),
    R("voice.face",  CYBER_VOICE_FACE_X,  CYBER_VOICE_FACE_Y,  CYBER_FACE_VOICE_PX, CYBER_FACE_VOICE_PX),
    R("voice.vis",   CYBER_VOICE_VIS_X,   CYBER_VOICE_VIS_Y,   CYBER_VOICE_VIS_W,  CYBER_VOICE_VIS_H),
    R("voice.sub",   CYBER_VOICE_SUB_X,   CYBER_VOICE_SUB_Y,   CYBER_VOICE_SUB_W,  CYBER_VOICE_SUB_H),
    R("pair.title",  CYBER_PAIR_TITLE_X,  CYBER_PAIR_TITLE_Y,  CYBER_PAIR_TITLE_W, CYBER_PAIR_TITLE_H),
    R("pair.frame",  CYBER_PAIR_FRAME_X,  CYBER_PAIR_FRAME_Y,  CYBER_PAIR_FRAME_W, CYBER_PAIR_FRAME_H),
    R("pair.rule",   CYBER_PAIR_RULE_X,   CYBER_PAIR_RULE_Y,   CYBER_PAIR_RULE_W,  CYBER_PAIR_RULE_H),
    R("pair.row1",   CYBER_PAIR_ROW_X,    CYBER_PAIR_ROW1_Y,   CYBER_PAIR_ROW_W,   CYBER_PAIR_ROW_H),
    R("pair.row2",   CYBER_PAIR_ROW_X,    CYBER_PAIR_ROW2_Y,   CYBER_PAIR_ROW_W,   CYBER_PAIR_ROW_H),
    R("pair.state",  CYBER_PAIR_STATE_X,  CYBER_PAIR_STATE_Y,  CYBER_PAIR_STATE_W, CYBER_PAIR_STATE_H),
    R("pair.scan",   CYBER_PAIR_SCAN_X,   CYBER_PAIR_SCAN_Y,   CYBER_PAIR_SCAN_W,  CYBER_PAIR_SCAN_H),
    R("neterr.title",  CYBER_NETERR_TITLE_X,  CYBER_NETERR_TITLE_Y,  CYBER_NETERR_TITLE_W, CYBER_NETERR_TITLE_H),
    R("neterr.rule",   CYBER_NETERR_RULE_X,   CYBER_NETERR_RULE_Y,   CYBER_NETERR_RULE_W,  CYBER_NETERR_RULE_H),
    R("neterr.reason", CYBER_NETERR_TEXT_X,   CYBER_NETERR_REASON_Y, CYBER_NETERR_TEXT_W,  CYBER_NETERR_TEXT_H),
    R("neterr.sugg",   CYBER_NETERR_TEXT_X,   CYBER_NETERR_SUGGEST_Y,CYBER_NETERR_TEXT_W,  CYBER_NETERR_TEXT_H),
    R("prov.title",    CYBER_PROV_TITLE_X,    CYBER_PROV_TITLE_Y,    CYBER_PROV_TITLE_W,   CYBER_PROV_TITLE_H),
    R("prov.rule",     CYBER_PROV_RULE_X,     CYBER_PROV_RULE_Y,     CYBER_PROV_RULE_W,    CYBER_PROV_RULE_H),
    R("prov.name",     CYBER_PROV_NAME_X,     CYBER_PROV_NAME_Y,     CYBER_PROV_NAME_W,    CYBER_PROV_NAME_H),
    R("prov.state",    CYBER_PROV_STATE_X,    CYBER_PROV_STATE_Y,    CYBER_PROV_STATE_W,   CYBER_PROV_STATE_H),
    R("prov.detail",   CYBER_PROV_DETAIL_X,   CYBER_PROV_DETAIL_Y,   CYBER_PROV_DETAIL_W,  CYBER_PROV_DETAIL_H),
};

// 逐行确认矩形完全落在圆角遮罩保留的可见范围内。
static void assert_visible(const rect_t *r)
{
    assert(r->w > 0 && r->h > 0);
    assert(r->x >= 0 && r->x + r->w <= CYBER_SCREEN_W);
    assert(r->y >= 0 && r->y + r->h <= CYBER_SCREEN_H);

    for (int y = r->y; y < r->y + r->h; y++) {
        int32_t x1 = 0, x2 = 0;
        const bool visible = bsp_display_rounded_row_span(
            y, CYBER_SCREEN_W, CYBER_SCREEN_H, CYBER_SCREEN_RADIUS, &x1, &x2);
        assert(visible);
        if (r->x < x1 || r->x + r->w - 1 > x2) {
            fprintf(stderr,
                    "layout: %s row %d spans x=%d..%d, outside visible x=%d..%d\n",
                    r->name, y, r->x, r->x + r->w - 1, (int)x1, (int)x2);
            assert(0);
        }
    }
}

static void test_edge_content_avoids_rounded_corners(void)
{
    for (size_t i = 0; i < sizeof(EDGE_CONTENT) / sizeof(EDGE_CONTENT[0]); i++) {
        assert_visible(&EDGE_CONTENT[i]);
    }
}

static void test_middle_content_avoids_rounded_corners(void)
{
    for (size_t i = 0; i < sizeof(MID_CONTENT) / sizeof(MID_CONTENT[0]); i++) {
        assert_visible(&MID_CONTENT[i]);
    }
}

static void test_middle_content_inside_straight_band(void)
{
    // 直边安全区内的行整幅宽度可用，中段内容不该越出这个带。
    for (size_t i = 0; i < sizeof(MID_CONTENT) / sizeof(MID_CONTENT[0]); i++) {
        const rect_t *r = &MID_CONTENT[i];
        assert(r->y >= CYBER_SAFE_TOP);
        assert(r->y + r->h - 1 <= CYBER_SAFE_BOTTOM);
    }
}

static void test_corner_region_is_actually_cut(void)
{
    // 如果圆角半径为 0，上面的检查会全部通过而毫无意义。
    // 这里钉住"圆角确实在切"，让前面那些边距检查真正有约束力。
    int32_t x1 = 0, x2 = 0;

    // 屏幕最上一行只保留中间一段。
    assert(bsp_display_rounded_row_span(0, CYBER_SCREEN_W, CYBER_SCREEN_H,
                                        CYBER_SCREEN_RADIUS, &x1, &x2));
    assert(x1 == CYBER_SCREEN_RADIUS);
    assert(x2 == CYBER_SCREEN_W - 1 - CYBER_SCREEN_RADIUS);

    // 最下一行同理。
    assert(bsp_display_rounded_row_span(CYBER_SCREEN_H - 1, CYBER_SCREEN_W,
                                        CYBER_SCREEN_H, CYBER_SCREEN_RADIUS, &x1, &x2));
    assert(x1 == CYBER_SCREEN_RADIUS);
    assert(x2 == CYBER_SCREEN_W - 1 - CYBER_SCREEN_RADIUS);

    // 直边安全区里必须是整幅可用，否则中段内容的边距就失去参照。
    assert(bsp_display_rounded_row_span(CYBER_SAFE_TOP, CYBER_SCREEN_W, CYBER_SCREEN_H,
                                        CYBER_SCREEN_RADIUS, &x1, &x2));
    assert(x1 == 0 && x2 == CYBER_SCREEN_W - 1);
    assert(bsp_display_rounded_row_span(CYBER_SAFE_BOTTOM, CYBER_SCREEN_W,
                                        CYBER_SCREEN_H, CYBER_SCREEN_RADIUS, &x1, &x2));
    assert(x1 == 0 && x2 == CYBER_SCREEN_W - 1);

    // 回归锚点：把电量按"离右缘 8px"摆放会被圆角切掉。
    // 电池正极凸点的最高一行，可见右界比 8px 边距更靠左。
    assert(bsp_display_rounded_row_span(CYBER_BATT_NUB_Y, CYBER_SCREEN_W,
                                        CYBER_SCREEN_H, CYBER_SCREEN_RADIUS, &x1, &x2));
    assert(x2 < CYBER_SCREEN_W - 8);
    // 当前布局的凸点右边缘仍然安全落在可见范围内。
    assert(CYBER_BATT_NUB_X + CYBER_BATT_NUB_W - 1 <= x2);
}

static void test_vertical_order(void)
{
    // 同屏内容按上下顺序排开，不互相压盖。
    assert(CYBER_STATUS_H <= CYBER_HOME_TITLE_Y);
    assert(CYBER_STATUS_H <= CYBER_BOOT_TITLE_Y);
    assert(CYBER_STATUS_H <= CYBER_VOICE_LABEL_Y);
    assert(CYBER_STATUS_H <= CYBER_PAIR_TITLE_Y);

    // 开机页
    assert(CYBER_BOOT_TITLE_Y + CYBER_BOOT_TITLE_H <= CYBER_BOOT_TITLE2_Y);
    assert(CYBER_BOOT_TITLE2_Y + CYBER_BOOT_TITLE_H <= CYBER_BOOT_SUB_Y);
    assert(CYBER_BOOT_SUB_Y + CYBER_BOOT_SUB_H <= CYBER_BOOT_DIV_Y);
    assert(CYBER_BOOT_DIV_Y + CYBER_BOOT_DIV_H <= CYBER_BOOT_BAR_Y);
    assert(CYBER_BOOT_BAR_Y + CYBER_BOOT_BAR_H <= CYBER_BOOT_PCT_Y);
    assert(CYBER_BOOT_PCT_Y + CYBER_BOOT_PCT_H <= CYBER_BOOT_LOG_Y);
    // 最后一条自检行不能撞到底部提示条。
    assert(CYBER_BOOT_LOG_Y + (CYBER_BOOT_LOG_LINES - 1) * CYBER_BOOT_LOG_STEP +
           CYBER_BOOT_LOG_H <= CYBER_HINT_RULE_Y);

    // 主界面
    assert(CYBER_HOME_TITLE_Y + CYBER_HOME_TITLE_H <= CYBER_HOME_FACE_Y);
    assert(CYBER_HOME_FACE_Y + CYBER_FACE_HOME_PX <= CYBER_HOME_MODE_Y);
    assert(CYBER_HOME_MODE_Y + CYBER_HOME_MODE_H <= CYBER_HINT_RULE_Y);

    // 语音页
    assert(CYBER_VOICE_LABEL_Y + CYBER_VOICE_LABEL_H <= CYBER_VOICE_FACE_Y);
    assert(CYBER_VOICE_FACE_Y + CYBER_FACE_VOICE_PX <= CYBER_VOICE_VIS_Y);
    assert(CYBER_VOICE_VIS_Y + CYBER_VOICE_VIS_H <= CYBER_VOICE_SUB_Y);
    assert(CYBER_VOICE_SUB_Y + CYBER_VOICE_SUB_H <= CYBER_HINT_RULE_Y);

    // 激活页：标题 → 面板 → 状态行 → 扫描条，逐段顺手排开，直到提示条。
    assert(CYBER_PAIR_TITLE_Y + CYBER_PAIR_TITLE_H <= CYBER_PAIR_FRAME_Y);
    assert(CYBER_PAIR_FRAME_Y + CYBER_PAIR_FRAME_H <= CYBER_PAIR_STATE_Y);
    assert(CYBER_PAIR_STATE_Y + CYBER_PAIR_STATE_H <= CYBER_PAIR_SCAN_Y);
    assert(CYBER_PAIR_SCAN_Y + CYBER_PAIR_SCAN_H <= CYBER_HINT_RULE_Y);

    // 网络错误页：标题 → 警示线 → 原因 → 建议，直到提示条。
    assert(CYBER_STATUS_H <= CYBER_NETERR_TITLE_Y);
    assert(CYBER_NETERR_TITLE_Y + CYBER_NETERR_TITLE_H <= CYBER_NETERR_RULE_Y);
    assert(CYBER_NETERR_RULE_Y + CYBER_NETERR_RULE_H <= CYBER_NETERR_REASON_Y);
    assert(CYBER_NETERR_REASON_Y + CYBER_NETERR_TEXT_H <= CYBER_NETERR_SUGGEST_Y);
    assert(CYBER_NETERR_SUGGEST_Y + CYBER_NETERR_TEXT_H <= CYBER_HINT_RULE_Y);

    // 蓝牙配网页：标题 → 霓虹线 → 广播名 → 阶段 → 中文说明，直到提示条。
    assert(CYBER_STATUS_H <= CYBER_PROV_TITLE_Y);
    assert(CYBER_PROV_TITLE_Y + CYBER_PROV_TITLE_H <= CYBER_PROV_RULE_Y);
    assert(CYBER_PROV_RULE_Y + CYBER_PROV_RULE_H <= CYBER_PROV_NAME_Y);
    assert(CYBER_PROV_NAME_Y + CYBER_PROV_NAME_H <= CYBER_PROV_STATE_Y);
    assert(CYBER_PROV_STATE_Y + CYBER_PROV_STATE_H <= CYBER_PROV_DETAIL_Y);
    assert(CYBER_PROV_DETAIL_Y + CYBER_PROV_DETAIL_H <= CYBER_HINT_RULE_Y);
}

// 激活页的内部几何：数字要被面板包住、一行要真放得下三位数、
// 扫描亮块在整段行程里都不能跑出暗轨。
static void test_pair_page_geometry(void)
{
    // 两行数字必须被面板完整包住，否则字会压到边框上。
    assert(CYBER_PAIR_ROW_X >= CYBER_PAIR_FRAME_X + 1);
    assert(CYBER_PAIR_ROW_X + CYBER_PAIR_ROW_W <=
           CYBER_PAIR_FRAME_X + CYBER_PAIR_FRAME_W - 1);
    assert(CYBER_PAIR_ROW1_Y >= CYBER_PAIR_FRAME_Y + 1);
    assert(CYBER_PAIR_ROW2_Y + CYBER_PAIR_ROW_H <=
           CYBER_PAIR_FRAME_Y + CYBER_PAIR_FRAME_H - 1);

    // 两行数字不能贴在一起。
    assert(CYBER_PAIR_ROW1_Y + CYBER_PAIR_ROW_H <= CYBER_PAIR_ROW2_Y);

    // 一行三位数要真放得下：Press Start 2P 的字形步进等于字号，而字号就是行高。
    // 这条断言是"48px 一行放不下六位、所以拆两行"这个决定的回归锚点。
    assert(CYBER_PAIR_CODE_ROW * CYBER_PAIR_ROW_H <= CYBER_PAIR_ROW_W);
    assert(CYBER_PAIR_CODE_ROW * CYBER_PAIR_ROW_H > CYBER_SCREEN_W / 2);

    // 亮块在行程两端都落在暗轨里。
    assert(CYBER_PAIR_SCAN_HEAD_W < CYBER_PAIR_SCAN_W);
    assert(CYBER_PAIR_SCAN_X + CYBER_PAIR_SCAN_SPAN - 1 + CYBER_PAIR_SCAN_HEAD_W <=
           CYBER_PAIR_SCAN_X + CYBER_PAIR_SCAN_W);

    // 强调线压在面板边框内侧。
    assert(CYBER_PAIR_RULE_Y > CYBER_PAIR_FRAME_Y);
    assert(CYBER_PAIR_RULE_X > CYBER_PAIR_FRAME_X);
    assert(CYBER_PAIR_RULE_X + CYBER_PAIR_RULE_W <
           CYBER_PAIR_FRAME_X + CYBER_PAIR_FRAME_W);

    // 居中元素左右留白之差不超过 1px。
    const int cx[] = { CYBER_PAIR_TITLE_X, CYBER_PAIR_FRAME_X, CYBER_PAIR_ROW_X,
                       CYBER_PAIR_STATE_X, CYBER_PAIR_SCAN_X };
    const int cw[] = { CYBER_PAIR_TITLE_W, CYBER_PAIR_FRAME_W, CYBER_PAIR_ROW_W,
                       CYBER_PAIR_STATE_W, CYBER_PAIR_SCAN_W };
    for (size_t i = 0; i < sizeof(cx) / sizeof(cx[0]); i++) {
        const int leftover = CYBER_SCREEN_W - cw[i];
        assert(leftover >= 0);
        assert(cx[i] == leftover / 2);
        assert(leftover - 2 * cx[i] <= 1);
    }
}

static void test_centered_boxes_are_symmetric(void)
{
    // 居中元素左右留白差不得超过 1px：宽度为奇数时必然差一格，
    // 其余情况下由 (W-w)/2 推导，不允许手写字面量把元素写偏。
    const int centered_x[] = {
        CYBER_HINT_X, CYBER_BOOT_TITLE_X, CYBER_BOOT_SUB_X, CYBER_BOOT_PCT_X,
        CYBER_HOME_TITLE_X, CYBER_HOME_MODE_X, CYBER_VOICE_LABEL_X, CYBER_VOICE_SUB_X,
        CYBER_BOOT_BAR_X, CYBER_HOME_FACE_X, CYBER_VOICE_FACE_X, CYBER_VOICE_VIS_X,
        CYBER_NETERR_TITLE_X, CYBER_NETERR_TEXT_X,
        CYBER_PROV_TITLE_X, CYBER_PROV_NAME_X, CYBER_PROV_STATE_X, CYBER_PROV_DETAIL_X,
    };
    const int centered_w[] = {
        CYBER_HINT_W, CYBER_BOOT_TITLE_W, CYBER_BOOT_SUB_W, CYBER_BOOT_PCT_W,
        CYBER_HOME_TITLE_W, CYBER_HOME_MODE_W, CYBER_VOICE_LABEL_W, CYBER_VOICE_SUB_W,
        CYBER_BOOT_BAR_W, CYBER_FACE_HOME_PX, CYBER_FACE_VOICE_PX, CYBER_VOICE_VIS_W,
        CYBER_NETERR_TITLE_W, CYBER_NETERR_TEXT_W,
        CYBER_PROV_TITLE_W, CYBER_PROV_NAME_W, CYBER_PROV_STATE_W, CYBER_PROV_DETAIL_W,
    };
    for (size_t i = 0; i < sizeof(centered_x) / sizeof(centered_x[0]); i++) {
        const int leftover = CYBER_SCREEN_W - centered_w[i];
        assert(leftover >= 0);
        // 由 (W-w)/2 推导，不允许手写字面量把元素写偏。
        assert(centered_x[i] == leftover / 2);
        // 左右留白之差不超过 1px（宽度为奇数时必然差一格）。
        assert(leftover - 2 * centered_x[i] <= 1);
    }
}

static void test_battery_and_status_bar_do_not_overlap(void)
{
    // 百分比读数必须完全在电池壳左侧，不能压到壳上。
    assert(CYBER_BATT_PCT_X + CYBER_BATT_PCT_W <= CYBER_BATT_BODY_X);
    // 电池壳与正极凸点相接。
    assert(CYBER_BATT_NUB_X == CYBER_BATT_BODY_X + CYBER_BATT_BODY_W);
    // 电池整体必须在链路文字右侧，两者不能挤在一起。
    assert(CYBER_LINK_X + CYBER_LINK_W < CYBER_BATT_PCT_X);
    // 填充条必须落在壳体内（含 1px 边框）。
    assert(CYBER_BATT_FILL_X == CYBER_BATT_BODY_X + 2);
    assert(CYBER_BATT_FILL_X + CYBER_BATT_FILL_W <= CYBER_BATT_BODY_X + CYBER_BATT_BODY_W - 1);
    assert(CYBER_BATT_FILL_Y + CYBER_BATT_FILL_H <= CYBER_BATT_BODY_Y + CYBER_BATT_BODY_H - 1);
    // 状态栏分隔线不压在最上排内容上。
    assert(CYBER_STATUS_RULE_Y >= CYBER_BATT_BODY_Y + CYBER_BATT_BODY_H);
    assert(CYBER_STATUS_RULE_Y >= CYBER_LINK_Y + CYBER_LINK_H);
}

int main(void)
{
    test_edge_content_avoids_rounded_corners();
    test_middle_content_avoids_rounded_corners();
    test_middle_content_inside_straight_band();
    test_corner_region_is_actually_cut();
    test_vertical_order();
    test_pair_page_geometry();
    test_centered_boxes_are_symmetric();
    test_battery_and_status_bar_do_not_overlap();

    printf("test_cyber_ui_layout: PASS\n");
    return 0;
}
