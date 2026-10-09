// main/cyber_face.c
// 像素机甲脸的绘制实现（MK-II：加了细节层）。
//
// 层次（自下而上，也就是绘制顺序）：
//   天线（底座 + 两侧翼 + 呼吸顶灯）
//   头部轮廓（横带 + 霓虹描边）
//   面罩（凹底 + 上下霓虹框）
//     面罩 HUD：扫描线 + 跑马灯轨道 + 数据总线
//   额头核心（呼吸菱形徽章）
//   侧舱（左右机械舱体 + 三道发光槽）
//   侧散热口
//   下颚格栅
//   双眼（外扩发光包边 + 表情蒙版）
//   嘴 / 说话态声波条
//   故障色带（仅 GLITCH）
//
// 所有几何都以"格"为单位，渲染时乘 cell 得到像素坐标；cell 由控件尺寸推出，
// 因此同一套像素设计可以同时服务主界面（大脸）和语音页（小脸）。
#include "cyber_face.h"

#include <string.h>

#include "cyber_theme.h"

// ---------------------------------------------------------------------------
// 面部内部布局（单位：格）。这些是该像素设计的内部细节，不外泄给上层。
// ---------------------------------------------------------------------------
#define ANTENNA_TIP_ROW0   0
#define ANTENNA_TIP_ROW1   1
#define ANTENNA_STEM_ROW0  2
#define ANTENNA_STEM_ROW1  4
#define ANTENNA_MOUNT_ROW  4    // 天线底座横梁
#define ANTENNA_MOUNT_HALF 2    // 横梁半宽 → 共 5 格

#define VISOR_ROW0        12
#define VISOR_ROW1        23
#define VISOR_HALF        17    // 面罩比头部两侧各留 5 格，露出机械边框

#define EYE_ROW0          16
#define EYE_INNER          7    // 眼内缘到中心列的格距

#define VENT_ROW0         26
#define VENT_ROW1         27
#define VENT_LEN           4
#define VENT_OFFSET       13

#define MOUTH_ROW         33    // 非说话态的静态嘴
#define BAR_BASE_ROW      38    // 说话态声波条基线（条向上生长）
#define BAR_MAX_H          8
#define BAR_COUNT          7
#define BAR_W              2
#define BAR_GAP            1

// --- 细节层 ---
#define HUD_ROW           (VISOR_ROW0 + 1)   // 跑马灯轨道所在行
#define STRIP_ROW         (VISOR_ROW1 - 1)   // 数据总线所在行
#define HUD_TRACK_IN       6                 // 轨道列范围（含端点）
#define HUD_TRACK_OUT     38
#define HUD_TICK_STEP      3                 // 每隔 3 列一个刻度
#define HUD_LIT            3                 // 同时点亮的灯数（1 盏头灯 + 2 盏拖尾）

#define EMBLEM_ROW0        9                 // 额头核心（菱形）
#define EMBLEM_ROW1       11
#define EMBLEM_HALF        1

#define POD_COL_IN         2                 // 侧舱内缘列（右侧镜像）
#define POD_COL_OUT        4
#define POD_ROW0          14
#define POD_ROW1          21
#define POD_SLOT_COUNT     3

#define JAW_ROW0          25                 // 下颚格栅（凹槽 + 竖条）
#define JAW_ROW1          26
#define JAW_HALF           4
#define JAW_BAR_STEP       2

#define ANTENNA_PULSE_MS  900u               // 顶灯呼吸周期（比核心快，像信号）

// ---------------------------------------------------------------------------
// 编译期护栏：细节层一旦被改出面罩/头部网格，画面上只是"少了一块"，
// 肉眼很难归因，所以把这些约束钉死在编译期。
// ---------------------------------------------------------------------------
_Static_assert(EMBLEM_ROW0 >= CYBER_FACE_HEAD_TOP + CYBER_FACE_ROUND,
               "额头核心必须落在头顶圆角之下的整宽区，否则会被轮廓削掉");
_Static_assert(EMBLEM_ROW1 < VISOR_ROW0, "额头核心不能被面罩压住");
_Static_assert(HUD_ROW > VISOR_ROW0 && HUD_ROW < EYE_ROW0,
               "跑马灯必须夹在面罩上沿与眼睛之间");
_Static_assert(STRIP_ROW > EYE_ROW0 + 5 && STRIP_ROW < VISOR_ROW1,
               "数据总线必须夹在眼睛下缘与面罩下沿之间");
_Static_assert(HUD_TRACK_IN > CYBER_FACE_CENTER - VISOR_HALF &&
               HUD_TRACK_OUT < CYBER_FACE_CENTER + VISOR_HALF,
               "HUD 轨道必须完全落在面罩内");
_Static_assert(POD_COL_OUT < CYBER_FACE_CENTER - VISOR_HALF,
               "侧舱必须在面罩之外，不能盖住面罩");
_Static_assert(POD_ROW0 > VISOR_ROW0 && POD_ROW1 < VISOR_ROW1,
               "侧舱纵向范围必须与面罩对齐");
_Static_assert(JAW_ROW0 > VISOR_ROW1 && JAW_ROW1 < MOUTH_ROW,
               "下颚格栅必须夹在面罩与嘴之间");
_Static_assert(ANTENNA_MOUNT_HALF * 2 + 1 <= CYBER_FACE_GRID_W,
               "天线底座不得超过网格宽度");

// ---------------------------------------------------------------------------
// 眼睛像素蒙版。'#' = 表情主色，'o' = 高光白，'.' = 透明。
// 用字符画而非坐标表，是为了改形状时能直接"看着改"。
// ---------------------------------------------------------------------------
#define CYBER_EYE_MAX_ROWS 6
#define EYE_MASK_W 5     // 实际用到的最大蒙版宽度（发光包边按它外扩）
#define EYE_MASK_H 5     // 实际用到的最大蒙版高度

typedef struct {
    uint8_t w;
    uint8_t h;
    const char *rows[CYBER_EYE_MAX_ROWS];
} cyber_eye_mask_t;

static const cyber_eye_mask_t EYE_ROUND = { 5, 5, { ".###.", "#####", "#####", "#####", ".###." } };
static const cyber_eye_mask_t EYE_ARC   = { 5, 3, { "..#..", ".###.", "#####" } };
static const cyber_eye_mask_t EYE_SLIT  = { 5, 2, { "#####", "#####" } };
static const cyber_eye_mask_t EYE_LINE  = { 5, 1, { "#####" } };
static const cyber_eye_mask_t EYE_WIDE  = { 5, 5, { "#####", "#o###", "#####", "#####", "#####" } };
static const cyber_eye_mask_t EYE_SMALL = { 3, 3, { "###", "###", "###" } };
static const cyber_eye_mask_t EYE_GL_A  = { 5, 2, { "##...", "..###" } };
static const cyber_eye_mask_t EYE_GL_B  = { 5, 2, { ".###.", "##..." } };

_Static_assert(CYBER_EYE_MAX_ROWS >= EYE_MASK_H, "蒙版数组容量必须容得下最高的眼睛");

typedef struct {
    const cyber_eye_mask_t *left;
    const cyber_eye_mask_t *right;
} cyber_eye_pair_t;

// 顺序必须与 cyber_emotion_t 一致。
static const cyber_eye_pair_t EYES[CYBER_EMO_COUNT] = {
    { &EYE_ROUND, &EYE_ROUND },  // NEUTRAL
    { &EYE_ARC,   &EYE_ARC   },  // HAPPY
    { &EYE_SMALL, &EYE_ROUND },  // CURIOUS：大小眼
    { &EYE_SLIT,  &EYE_SLIT  },  // FOCUS
    { &EYE_WIDE,  &EYE_WIDE  },  // ALERT
    { &EYE_LINE,  &EYE_LINE  },  // SLEEPY
    { &EYE_GL_A,  &EYE_GL_B  },  // GLITCH
};

// 非说话态嘴角宽度（格）与左右偏移，按情绪给一点表情差异。
typedef struct {
    int width;
    int offset;
    int split;   // >0 时画成两段（用于 GLITCH 的碎嘴）
} cyber_mouth_t;

static const cyber_mouth_t MOUTH[CYBER_EMO_COUNT] = {
    { 5, 0, 0 },   // NEUTRAL
    { 7, 0, 0 },   // HAPPY
    { 4, 1, 0 },   // CURIOUS
    { 5, 0, 0 },   // FOCUS
    { 3, 0, 0 },   // ALERT
    { 6, 0, 0 },   // SLEEPY
    { 6, 0, 2 },   // GLITCH
};

// ---------------------------------------------------------------------------
// 控件上下文
// ---------------------------------------------------------------------------
typedef struct {
    int cell;                 // 每格边长（px）
    cyber_emotion_t emo;
    uint32_t anim_ms;
    bool speaking;
    bool valid;
} cyber_face_ctx_t;

// ---------------------------------------------------------------------------
// 绘制工具
// ---------------------------------------------------------------------------
// 画一个"格矩形"：(col0,row0)-(col1,row1) 含端点，坐标相对控件左上角。
static void cell_rect(lv_layer_t *layer, const lv_area_t *origin, int cell,
                      int col0, int row0, int col1, int row1, uint32_t color,
                      lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = opa;
    dsc.radius = 0;      // 像素风：绝不圆角

    lv_area_t a;
    a.x1 = origin->x1 + col0 * cell;
    a.y1 = origin->y1 + row0 * cell;
    a.x2 = origin->x1 + (col1 + 1) * cell - 1;
    a.y2 = origin->y1 + (row1 + 1) * cell - 1;
    lv_draw_rect(layer, &dsc, &a);
}

// 画像素蒙版：同色连续像素合并成一段矩形，减少绘制调用。
static void draw_mask(lv_layer_t *layer, const lv_area_t *origin, int cell,
                      const cyber_eye_mask_t *mask, int col, int row,
                      uint32_t color)
{
    for (int r = 0; r < mask->h; r++) {
        const char *line = mask->rows[r];
        int c = 0;
        while (c < mask->w) {
            const char ch = line[c];
            if (ch == '.') {
                c++;
                continue;
            }
            const int start = c;
            while (c < mask->w && line[c] == ch) c++;
            const uint32_t px = (ch == 'o') ? CYBER_TEXT : color;
            cell_rect(layer, origin, cell, col + start, row + r,
                      col + c - 1, row + r, px, LV_OPA_COVER);
        }
    }
}

// 确定性抖动：同一 anim_ms 与同一 lane 结果稳定，避免重绘时闪成随机噪声。
static int glitch_shift(uint32_t anim_ms, int lane)
{
    uint32_t h = anim_ms / 60u + (uint32_t)lane * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (int)(h % 7u) - 3;   // -3..+3 格
}

static int glitch_row(uint32_t anim_ms, int lane)
{
    uint32_t h = anim_ms / 80u + (uint32_t)lane * 40503u;
    h ^= h >> 11;
    h *= 0x9e3779b1u;
    h ^= h >> 15;
    return VISOR_ROW0 + 1 + (int)(h % (uint32_t)(VISOR_ROW1 - VISOR_ROW0));
}

// HUD 强调色：用与表情主色互补的次强调色，让同一屏出现两种霓虹才够"赛博"。
// 但表情本身就是品红系时改用青色，否则 HUD 会和眼睛糊成一片。
static uint32_t hud_accent(cyber_emotion_t emo)
{
    switch (emo) {
    case CYBER_EMO_ALERT:
    case CYBER_EMO_GLITCH:
        return CYBER_CYAN;
    default:
        return CYBER_MAGENTA;
    }
}

// ---------------------------------------------------------------------------
// 各部件绘制
// ---------------------------------------------------------------------------
static void draw_antenna(lv_layer_t *layer, const lv_area_t *o, int cell,
                         uint32_t neon, uint32_t anim_ms)
{
    const int c = CYBER_FACE_CENTER;

    // 底座横梁 + 两翼：把"一根后脑勺的杆"变成有托架的发射机构。
    cell_rect(layer, o, cell, c - ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW,
              c + ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW, CYBER_PANEL_HI, LV_OPA_COVER);
    cell_rect(layer, o, cell, c - ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW - 1,
              c - ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW - 1, neon, LV_OPA_40);
    cell_rect(layer, o, cell, c + ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW - 1,
              c + ANTENNA_MOUNT_HALF, ANTENNA_MOUNT_ROW - 1, neon, LV_OPA_40);

    cell_rect(layer, o, cell, c, ANTENNA_STEM_ROW0, c, ANTENNA_STEM_ROW1,
              CYBER_PANEL_HI, LV_OPA_COVER);

    // 顶灯：呼吸而不是"亮灭开关"。有能量涨落，才像信号塔而不是灯泡。
    const uint8_t lv = cyber_pulse(anim_ms, ANTENNA_PULSE_MS);
    const lv_opa_t tip_opa =
        (lv_opa_t)(LV_OPA_40 + (int)lv * (LV_OPA_COVER - LV_OPA_40) / 255);
    cell_rect(layer, o, cell, c - 1, ANTENNA_TIP_ROW0, c, ANTENNA_TIP_ROW1,
              neon, tip_opa);
}

static void draw_head(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t neon)
{
    cyber_face_band_t bands[CYBER_FACE_GRID_H];
    const int count = cyber_face_head_bands(bands, CYBER_FACE_GRID_H);

    for (int i = 0; i < count; i++) {
        const cyber_face_band_t *b = &bands[i];
        // 上亮下暗的简化受光，让平面的像素轮廓有一点体积感。
        const uint32_t fill = (b->row1 <= VISOR_ROW1) ? CYBER_PANEL_HI : CYBER_PANEL;
        cell_rect(layer, o, cell, CYBER_FACE_CENTER - b->half, b->row0,
                  CYBER_FACE_CENTER + b->half, b->row1, fill, LV_OPA_COVER);
        // 沿轮廓的霓虹描边：每带左右各一条竖条，天然形成阶梯状发光边。
        cell_rect(layer, o, cell, CYBER_FACE_CENTER - b->half, b->row0,
                  CYBER_FACE_CENTER - b->half, b->row1, neon, LV_OPA_COVER);
        cell_rect(layer, o, cell, CYBER_FACE_CENTER + b->half, b->row0,
                  CYBER_FACE_CENTER + b->half, b->row1, neon, LV_OPA_COVER);
        // 描边内侧再压一条更暗的缝：霓虹 | 暗缝 | 面板 三层次叠起来，
        // 头部才像有厚度的机甲壳而不是一条描了边的色块。
        // 用 CYBER_BG 而不是 CYBER_PANEL：下半脸本身就是 PANEL 色，
        // 再用 PANEL 画的缝会完全看不见。
        if (b->half > 2) {
            cell_rect(layer, o, cell, CYBER_FACE_CENTER - b->half + 1, b->row0,
                      CYBER_FACE_CENTER - b->half + 1, b->row1, CYBER_BG, LV_OPA_60);
            cell_rect(layer, o, cell, CYBER_FACE_CENTER + b->half - 1, b->row0,
                      CYBER_FACE_CENTER + b->half - 1, b->row1, CYBER_BG, LV_OPA_60);
        }
    }
}

static void draw_visor(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t neon)
{
    // 面罩底 + 上下霓虹框：底用比面板更深的色，制造"凹进去一块玻璃"的观感。
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - VISOR_HALF, VISOR_ROW0,
              CYBER_FACE_CENTER + VISOR_HALF, VISOR_ROW1, CYBER_BG, LV_OPA_COVER);
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - VISOR_HALF, VISOR_ROW0,
              CYBER_FACE_CENTER + VISOR_HALF, VISOR_ROW0, neon, LV_OPA_COVER);
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - VISOR_HALF, VISOR_ROW1,
              CYBER_FACE_CENTER + VISOR_HALF, VISOR_ROW1, neon, LV_OPA_COVER);
}

// 面罩 HUD：扫描线 + 跑马灯 + 数据总线。三者都在面罩内，画完正好被眼睛压住。
static void draw_visor_hud(lv_layer_t *layer, const lv_area_t *o, int cell,
                           uint32_t anim_ms, uint32_t accent)
{
    const int inner = VISOR_ROW1 - VISOR_ROW0 - 1;   // 可扫描的内区行数

    // 1) 扫描线：CRT 上下来回扫，透明度很低，只做暗示不抢主体。
    const int scan = VISOR_ROW0 + 1 + cyber_visor_scan_index(anim_ms, inner);
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - VISOR_HALF + 1, scan,
              CYBER_FACE_CENTER + VISOR_HALF - 1, scan, accent, LV_OPA_20);

    const int track_n = (HUD_TRACK_OUT - HUD_TRACK_IN) / HUD_TICK_STEP + 1;

    // 2) 跑马灯轨道：等距暗刻度打底，再叠一段流动的高亮灯（头灯 + 两盏拖尾）。
    for (int i = 0; i < track_n; i++) {
        const int c = HUD_TRACK_IN + i * HUD_TICK_STEP;
        cell_rect(layer, o, cell, c, HUD_ROW, c, HUD_ROW, accent, LV_OPA_30);
    }
    for (int k = 0; k < HUD_LIT; k++) {
        const int i = cyber_hud_chase(anim_ms, track_n, -k);
        const int c = HUD_TRACK_IN + i * HUD_TICK_STEP;
        const lv_opa_t opa = (k == 0) ? LV_OPA_COVER
                                      : ((k == 1) ? LV_OPA_60 : LV_OPA_30);
        cell_rect(layer, o, cell, c, HUD_ROW, c, HUD_ROW, accent, opa);
    }

    // 3) 数据总线：等距短线按时间确定性闪动，像是真有数据在总线上跑。
    const uint32_t frame = anim_ms / CYBER_DATA_FLICKER_MS;
    for (int i = 0; i < track_n; i++) {
        const int c = HUD_TRACK_IN + i * HUD_TICK_STEP;
        const bool lit = (((frame + (uint32_t)i) % 3u) == 0u);
        cell_rect(layer, o, cell, c, STRIP_ROW, c + 1, STRIP_ROW, accent,
                  lit ? LV_OPA_70 : LV_OPA_20);
    }
}

// 额头核心：呼吸的菱形徽章。脸部的"视觉重心"，让空荡的额头有主次。
static void draw_emblem(lv_layer_t *layer, const lv_area_t *o, int cell,
                        uint32_t neon, uint32_t anim_ms)
{
    const uint8_t lv = cyber_pulse(anim_ms, CYBER_PULSE_PERIOD_MS);
    const lv_opa_t opa =
        (lv_opa_t)(LV_OPA_30 + (int)lv * (LV_OPA_COVER - LV_OPA_30) / 255);
    const int c = CYBER_FACE_CENTER;
    const int mid = EMBLEM_ROW0 + 1;

    cell_rect(layer, o, cell, c, EMBLEM_ROW0, c, EMBLEM_ROW0, neon, opa);
    cell_rect(layer, o, cell, c - EMBLEM_HALF, mid, c + EMBLEM_HALF, mid, neon, opa);
    cell_rect(layer, o, cell, c, EMBLEM_ROW1, c, EMBLEM_ROW1, neon, opa);
    // 左右光翼：一点点外溢的低透明度像素，避免徽章看起来是"贴纸"。
    cell_rect(layer, o, cell, c - EMBLEM_HALF - 1, mid, c - EMBLEM_HALF - 1, mid,
              neon, LV_OPA_30);
    cell_rect(layer, o, cell, c + EMBLEM_HALF + 1, mid, c + EMBLEM_HALF + 1, mid,
              neon, LV_OPA_30);
}

// 侧舱：面罩两侧的机械舱体，让"脸颊"不再是空面板。
static void draw_pods(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t neon)
{
    const int rows = POD_ROW1 - POD_ROW0 + 1;

    for (int side = 0; side < 2; side++) {
        // 左侧用 [POD_COL_IN, POD_COL_OUT]，右侧镜像到网格另一侧。
        const int c0 = (side == 0) ? POD_COL_IN
                                   : (CYBER_FACE_GRID_W - 1 - POD_COL_OUT);
        const int c1 = (side == 0) ? POD_COL_OUT
                                   : (CYBER_FACE_GRID_W - 1 - POD_COL_IN);

        // 凹槽底：比面板更暗，形成"嵌进去的舱体"。
        cell_rect(layer, o, cell, c0, POD_ROW0, c1, POD_ROW1, CYBER_BG, LV_OPA_COVER);
        // 舱内发光槽：等分高度并留出上下边框。
        for (int i = 1; i <= POD_SLOT_COUNT; i++) {
            const int r = POD_ROW0 + rows * i / (POD_SLOT_COUNT + 1);
            cell_rect(layer, o, cell, c0, r, c1, r, neon, LV_OPA_40);
        }
    }
}

static void draw_vents(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t neon)
{
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - VENT_OFFSET - VENT_LEN, VENT_ROW0,
              CYBER_FACE_CENTER - VENT_OFFSET - 1, VENT_ROW1, neon, LV_OPA_50);
    cell_rect(layer, o, cell, CYBER_FACE_CENTER + VENT_OFFSET + 1, VENT_ROW0,
              CYBER_FACE_CENTER + VENT_OFFSET + VENT_LEN, VENT_ROW1, neon, LV_OPA_50);
}

// 下颚格栅：凹槽 + 竖条，替代"一整块面板"，让下半脸有机械分层。
static void draw_jaw(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t neon)
{
    const int c0 = CYBER_FACE_CENTER - JAW_HALF;
    const int c1 = CYBER_FACE_CENTER + JAW_HALF;

    // 先挖一道比面板更暗的凹槽，格栅才像嵌进去而不是浮在表面。
    // 下半脸用的是 CYBER_PANEL，用同一个色画"压边"等于没画，必须比它更暗。
    cell_rect(layer, o, cell, c0, JAW_ROW0, c1, JAW_ROW1, CYBER_BG, LV_OPA_COVER);

    for (int c = c0; c <= c1; c += JAW_BAR_STEP) {
        cell_rect(layer, o, cell, c, JAW_ROW0, c, JAW_ROW1, neon, LV_OPA_40);
    }
}

static void draw_eyes(lv_layer_t *layer, const lv_area_t *o, int cell,
                      cyber_emotion_t emo, uint32_t neon, uint32_t anim_ms)
{
    if (emo >= CYBER_EMO_COUNT) emo = CYBER_EMO_NEUTRAL;
    const cyber_eye_pair_t *pair = &EYES[emo];

    // 眨眼用一条横线覆盖双眼；连眨眼都算表情，比让眼睛一直瞪着更像活的。
    const bool blink = cyber_face_blink(anim_ms);
    const cyber_eye_mask_t *left = blink ? &EYE_LINE : pair->left;
    const cyber_eye_mask_t *right = blink ? &EYE_LINE : pair->right;

    // 两眼各自贴紧内缘：窄蒙版（如 CURIOUS 的小眼）不会跑偏。
    const int left_col = CYBER_FACE_CENTER - EYE_INNER - (int)left->w + 1;
    const int right_col = CYBER_FACE_CENTER + EYE_INNER;

    // 外扩一格的发光包边：眼睛因此像在发光，而不是"贴"在面罩上。
    // 两侧都按蒙版容量上限取宽，左右辉光才是等宽的；小眼（CURIOUS）也不会
    // 出现"左窄右宽"的歪斜。
    cell_rect(layer, o, cell, CYBER_FACE_CENTER - EYE_INNER - EYE_MASK_W, EYE_ROW0 - 1,
              CYBER_FACE_CENTER - EYE_INNER + 1, EYE_ROW0 + EYE_MASK_H, neon, LV_OPA_20);
    cell_rect(layer, o, cell, CYBER_FACE_CENTER + EYE_INNER - 1, EYE_ROW0 - 1,
              CYBER_FACE_CENTER + EYE_INNER + EYE_MASK_W, EYE_ROW0 + EYE_MASK_H,
              neon, LV_OPA_20);

    draw_mask(layer, o, cell, left, left_col, EYE_ROW0, neon);
    draw_mask(layer, o, cell, right, right_col, EYE_ROW0, neon);
}

static void draw_mouth(lv_layer_t *layer, const lv_area_t *o, int cell,
                       cyber_emotion_t emo, uint32_t neon, bool speaking,
                       uint32_t anim_ms)
{
    if (speaking) {
        // 声波条：高度取自纯函数的 cyber_wave_level，条向上生长，整体居中。
        const int span = BAR_COUNT * BAR_W + (BAR_COUNT - 1) * BAR_GAP;
        const int start = CYBER_FACE_CENTER - span / 2;
        for (int i = 0; i < BAR_COUNT; i++) {
            const uint8_t level = cyber_wave_level(anim_ms, (uint8_t)i, BAR_COUNT);
            const int h = 1 + (int)((uint32_t)level * (BAR_MAX_H - 1) / 255u);
            const int col0 = start + i * (BAR_W + BAR_GAP);
            cell_rect(layer, o, cell, col0, BAR_BASE_ROW - h + 1,
                      col0 + BAR_W - 1, BAR_BASE_ROW, neon, LV_OPA_COVER);
        }
        return;
    }

    if (emo >= CYBER_EMO_COUNT) emo = CYBER_EMO_NEUTRAL;
    const cyber_mouth_t *mth = &MOUTH[emo];
    const int col0 = CYBER_FACE_CENTER - mth->width / 2 + mth->offset;

    if (mth->split > 0) {
        // 碎嘴：中间留缺口，配合 GLITCH 的错位眼。
        cell_rect(layer, o, cell, col0, MOUTH_ROW, col0 + mth->split - 1, MOUTH_ROW,
                  neon, LV_OPA_COVER);
        cell_rect(layer, o, cell, col0 + mth->split + 1, MOUTH_ROW,
                  col0 + mth->width - 1, MOUTH_ROW, neon, LV_OPA_COVER);
    } else {
        cell_rect(layer, o, cell, col0, MOUTH_ROW, col0 + mth->width - 1, MOUTH_ROW,
                  neon, LV_OPA_COVER);
    }
}

static void draw_glitch(lv_layer_t *layer, const lv_area_t *o, int cell, uint32_t anim_ms)
{
    // 三条横贯的错位色带：不做"整脸位移"，避免每帧重画全部部件。
    static const uint32_t COLORS[3] = { CYBER_MAGENTA, CYBER_RED, CYBER_CYAN };
    for (int lane = 0; lane < 3; lane++) {
        const int row = glitch_row(anim_ms, lane);
        if (row < VISOR_ROW0 || row > VISOR_ROW1) continue;
        const int half = VISOR_HALF;
        const int col0 = CYBER_FACE_CENTER - half + glitch_shift(anim_ms, lane);
        cell_rect(layer, o, cell, col0, row, col0 + 2 * half, row,
                  COLORS[lane], LV_OPA_50);
    }
}

// ---------------------------------------------------------------------------
// 主体绘制回调
// ---------------------------------------------------------------------------
static void face_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_face_ctx_t *ctx = (cyber_face_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx == NULL) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) return;

    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);   // LVGL 9 绘制层用屏幕绝对坐标

    const int cell = ctx->cell;
    const uint32_t neon = cyber_emotion_color(ctx->emo);
    const uint32_t accent = hud_accent(ctx->emo);

    // 顺序即层次：外框 → 面罩 → 面罩内的 HUD → 独立细节 → 眼嘴。
    // HUD 必须早于眼睛，否则扫描线会盖在瞳孔上。
    draw_antenna(layer, &origin, cell, neon, ctx->anim_ms);
    draw_head(layer, &origin, cell, neon);
    draw_visor(layer, &origin, cell, neon);
    draw_visor_hud(layer, &origin, cell, ctx->anim_ms, accent);
    draw_emblem(layer, &origin, cell, neon, ctx->anim_ms);
    draw_pods(layer, &origin, cell, neon);
    draw_vents(layer, &origin, cell, neon);
    draw_jaw(layer, &origin, cell, neon);
    draw_eyes(layer, &origin, cell, ctx->emo, neon, ctx->anim_ms);
    draw_mouth(layer, &origin, cell, ctx->emo, neon, ctx->speaking, ctx->anim_ms);

    if (ctx->emo == CYBER_EMO_GLITCH) {
        draw_glitch(layer, &origin, cell, ctx->anim_ms);
    }
}

static void face_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_face_ctx_t *ctx = (cyber_face_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx != NULL) {
        lv_free(ctx);                     // 与 lv_malloc 配对，走 LVGL 内部堆
        lv_obj_set_user_data(obj, NULL);
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
lv_obj_t *cyber_face_create(lv_obj_t *parent, int size_px)
{
    if (parent == NULL) return NULL;
    // 尺寸必须是整数格，否则像素会被半像素采样，边缘发虚、失去像素风。
    if (size_px <= 0 || (size_px % CYBER_FACE_GRID_W) != 0) return NULL;

    cyber_face_ctx_t *ctx = (cyber_face_ctx_t *)lv_malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cell = size_px / CYBER_FACE_GRID_W;
    ctx->emo = CYBER_EMO_NEUTRAL;

    lv_obj_t *face = lv_obj_create(parent);
    if (face == NULL) {
        lv_free(ctx);
        return NULL;
    }
    lv_obj_remove_style_all(face);                       // 不要默认主题的边框/内边距
    lv_obj_set_size(face, size_px, size_px);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(face, ctx);
    lv_obj_add_event_cb(face, face_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(face, face_delete_cb, LV_EVENT_DELETE, NULL);
    return face;
}

void cyber_face_render(lv_obj_t *face, cyber_emotion_t emo, uint32_t anim_ms,
                       bool speaking)
{
    if (face == NULL) return;
    cyber_face_ctx_t *ctx = (cyber_face_ctx_t *)lv_obj_get_user_data(face);
    if (ctx == NULL) return;

    // 只在真正变化时 invalidate：主界面静止时脸不重绘，省 CPU 也省电。
    const bool changed = !ctx->valid || ctx->emo != emo || ctx->speaking != speaking ||
                         (anim_ms / 60u) != (ctx->anim_ms / 60u);
    ctx->emo = emo;
    ctx->anim_ms = anim_ms;
    ctx->speaking = speaking;
    ctx->valid = true;

    if (changed) lv_obj_invalidate(face);
}
