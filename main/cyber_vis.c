// main/cyber_vis.c
// 网格背景 / 语音可视化 / 故障抖动的绘制实现。
#include "cyber_vis.h"

#include <string.h>

#include "cyber_theme.h"
#include "cyber_ui_model.h"   // cyber_wave_level：波形高度来自纯逻辑层，便于单测

// ---------------------------------------------------------------------------
// 通用绘制工具（与 cyber_face.c 同思路：绝对坐标 + 无圆角矩形）
// ---------------------------------------------------------------------------
static void fill_rect(lv_layer_t *layer, const lv_area_t *o,
                      int x, int y, int w, int h, uint32_t color, lv_opa_t opa)
{
    if (w <= 0 || h <= 0) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = opa;
    dsc.radius = 0;

    lv_area_t a;
    a.x1 = o->x1 + x;
    a.y1 = o->y1 + y;
    a.x2 = a.x1 + w - 1;
    a.y2 = a.y1 + h - 1;
    lv_draw_rect(layer, &dsc, &a);
}

// ---------------------------------------------------------------------------
// 静态网格背景
// ---------------------------------------------------------------------------
#define BG_H_LINE_COUNT  9    // 横线数量：控制在个位数，避免每次重绘都很贵
#define BG_V_LINE_COUNT  6
#define BG_CORNER_LEN   18
#define BG_CORNER_INSET 10

static void bg_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) return;

    lv_area_t o;
    lv_obj_get_coords(obj, &o);
    const int w = lv_area_get_width(&o);
    const int h = lv_area_get_height(&o);

    // 横线：等分屏幕。用很低的透明度，只提供"CRT 扫描"的暗示，不抢主体。
    for (int i = 1; i <= BG_H_LINE_COUNT; i++) {
        const int y = h * i / (BG_H_LINE_COUNT + 1);
        fill_rect(layer, &o, 0, y, w, 1, CYBER_GRID, LV_OPA_40);
    }
    for (int i = 1; i <= BG_V_LINE_COUNT; i++) {
        const int x = w * i / (BG_V_LINE_COUNT + 1);
        fill_rect(layer, &o, x, 0, 1, h, CYBER_GRID, LV_OPA_30);
    }

    // 四角括号：四角被 30px 圆角遮罩切掉，故内缩后再画，避免截断。
    const uint32_t neon = CYBER_CYAN;
    const int xr = w - BG_CORNER_INSET - BG_CORNER_LEN;
    const int yb = h - BG_CORNER_INSET - BG_CORNER_LEN;
    fill_rect(layer, &o, BG_CORNER_INSET, BG_CORNER_INSET, BG_CORNER_LEN, 2, neon, LV_OPA_60);
    fill_rect(layer, &o, BG_CORNER_INSET, BG_CORNER_INSET, 2, BG_CORNER_LEN, neon, LV_OPA_60);
    fill_rect(layer, &o, xr, BG_CORNER_INSET, BG_CORNER_LEN, 2, neon, LV_OPA_60);
    fill_rect(layer, &o, xr + BG_CORNER_LEN - 2, BG_CORNER_INSET, 2, BG_CORNER_LEN, neon, LV_OPA_60);
    fill_rect(layer, &o, BG_CORNER_INSET, yb + BG_CORNER_LEN - 2, BG_CORNER_LEN, 2, neon, LV_OPA_60);
    fill_rect(layer, &o, BG_CORNER_INSET, yb, 2, BG_CORNER_LEN, neon, LV_OPA_60);
    fill_rect(layer, &o, xr, yb + BG_CORNER_LEN - 2, BG_CORNER_LEN, 2, neon, LV_OPA_60);
    fill_rect(layer, &o, xr + BG_CORNER_LEN - 2, yb, 2, BG_CORNER_LEN, neon, LV_OPA_60);
}

lv_obj_t *cyber_bg_create(lv_obj_t *parent, int w, int h)
{
    if (parent == NULL) return NULL;
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, bg_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    return obj;
}

// ---------------------------------------------------------------------------
// 语音可视化
// ---------------------------------------------------------------------------
#define VIS_BAR_COUNT 15
#define VIS_BAR_W      8
#define VIS_BAR_GAP    4
#define VIS_STEP_PX    4    // 高度量化步长：让波形保持"像素台阶"而不是平滑曲线
#define VIS_SCAN_MS  900u   // 扫描线一个来回的周期

typedef struct {
    cyber_vis_mode_t mode;
    uint32_t anim_ms;
    bool valid;
} cyber_vis_ctx_t;

// 把 0..255 的强度量化成 VIS_STEP_PX 的整数倍高度。
static int quantize_height(uint8_t level, int max_h)
{
    int h = (int)((uint32_t)level * (uint32_t)max_h / 255u);
    h = (h / VIS_STEP_PX) * VIS_STEP_PX;
    if (h < VIS_STEP_PX) h = VIS_STEP_PX;
    if (h > max_h) h = max_h;
    return h;
}

static void vis_draw_wave(lv_layer_t *layer, const lv_area_t *o, int w, int h,
                          uint32_t anim_ms)
{
    const int mid = h / 2;
    const int max_h = (h / 2) - 2;
    const int span = VIS_BAR_COUNT * VIS_BAR_W + (VIS_BAR_COUNT - 1) * VIS_BAR_GAP;
    const int start = (w - span) / 2;

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const uint8_t level = cyber_wave_level(anim_ms, (uint8_t)i, VIS_BAR_COUNT);
        const int half = quantize_height(level, max_h) / 2;
        const int x = start + i * (VIS_BAR_W + VIS_BAR_GAP);
        // 以中线为轴对称生长，像真正的拾音波形。
        fill_rect(layer, o, x, mid - half, VIS_BAR_W, half * 2, CYBER_AMBER, LV_OPA_COVER);
    }
    // 中线：给波形一个"零轴"参照。
    fill_rect(layer, o, 0, mid, w, 1, CYBER_GRID, LV_OPA_60);
}

static void vis_draw_spectrum(lv_layer_t *layer, const lv_area_t *o, int w, int h,
                              uint32_t anim_ms)
{
    const int max_h = h - 2;
    const int span = VIS_BAR_COUNT * VIS_BAR_W + (VIS_BAR_COUNT - 1) * VIS_BAR_GAP;
    const int start = (w - span) / 2;

    for (int i = 0; i < VIS_BAR_COUNT; i++) {
        const uint8_t level = cyber_wave_level(anim_ms, (uint8_t)i, VIS_BAR_COUNT);
        const int bh = quantize_height(level, max_h);
        const int x = start + i * (VIS_BAR_W + VIS_BAR_GAP);
        fill_rect(layer, o, x, h - bh, VIS_BAR_W, bh, CYBER_CYAN, LV_OPA_COVER);
    }
}

static void vis_draw_scan(lv_layer_t *layer, const lv_area_t *o, int w, int h,
                          uint32_t anim_ms)
{
    // 三角波往返：位置可预测，观感稳定，不像随机跳动那样让人焦虑。
    const uint32_t phase = anim_ms % VIS_SCAN_MS;
    const uint32_t half = VIS_SCAN_MS / 2u;
    const int travel = w - VIS_BAR_W;
    const int x = (int)((uint64_t)((phase < half) ? phase : (VIS_SCAN_MS - phase)) *
                        (uint32_t)travel / half);

    // 尾部拖影：三档递减亮度，制造"扫描"的余晖。
    fill_rect(layer, o, x, 0, VIS_BAR_W, h, CYBER_MAGENTA, LV_OPA_COVER);
    if (x - VIS_BAR_W * 2 >= 0) {
        fill_rect(layer, o, x - VIS_BAR_W * 2, 0, VIS_BAR_W, h, CYBER_MAGENTA, LV_OPA_30);
    }
    if (x + VIS_BAR_W * 3 <= w) {
        fill_rect(layer, o, x + VIS_BAR_W * 2, 0, VIS_BAR_W, h, CYBER_MAGENTA, LV_OPA_30);
    }
    fill_rect(layer, o, 0, h / 2, w, 1, CYBER_GRID, LV_OPA_40);
}

static void vis_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_vis_ctx_t *ctx = (cyber_vis_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx == NULL) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) return;

    lv_area_t o;
    lv_obj_get_coords(obj, &o);
    const int w = lv_area_get_width(&o);
    const int h = lv_area_get_height(&o);

    switch (ctx->mode) {
    case CYBER_VIS_WAVE:     vis_draw_wave(layer, &o, w, h, ctx->anim_ms); break;
    case CYBER_VIS_SPECTRUM: vis_draw_spectrum(layer, &o, w, h, ctx->anim_ms); break;
    case CYBER_VIS_SCAN:     vis_draw_scan(layer, &o, w, h, ctx->anim_ms); break;
    default: break;
    }
}

static void vis_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_vis_ctx_t *ctx = (cyber_vis_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx != NULL) {
        lv_free(ctx);
        lv_obj_set_user_data(obj, NULL);
    }
}

lv_obj_t *cyber_vis_create(lv_obj_t *parent, int w, int h)
{
    if (parent == NULL) return NULL;
    cyber_vis_ctx_t *ctx = (cyber_vis_ctx_t *)lv_malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = CYBER_VIS_NONE;

    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        lv_free(ctx);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(obj, ctx);
    lv_obj_add_event_cb(obj, vis_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(obj, vis_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

void cyber_vis_render(lv_obj_t *vis, cyber_vis_mode_t mode, uint32_t anim_ms)
{
    if (vis == NULL) return;
    cyber_vis_ctx_t *ctx = (cyber_vis_ctx_t *)lv_obj_get_user_data(vis);
    if (ctx == NULL) return;

    if (mode == CYBER_VIS_NONE) {
        // 隐藏而不是画空帧：隐藏对象不参与绘制，省 CPU 与 SPI 带宽。
        ctx->mode = mode;
        ctx->valid = false;
        lv_obj_add_flag(vis, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(vis, LV_OBJ_FLAG_HIDDEN);

    const bool changed = !ctx->valid || ctx->mode != mode ||
                         (anim_ms / 40u) != (ctx->anim_ms / 40u);
    ctx->mode = mode;
    ctx->anim_ms = anim_ms;
    ctx->valid = true;
    if (changed) lv_obj_invalidate(vis);
}

// ---------------------------------------------------------------------------
// 故障抖动层
// ---------------------------------------------------------------------------
typedef struct {
    bool active;
    uint32_t anim_ms;
    bool valid;
} cyber_glitch_ctx_t;

static int glitch_shift(uint32_t anim_ms, int lane)
{
    uint32_t h = anim_ms / 50u + (uint32_t)lane * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (int)(h % 25u) - 12;
}

static void glitch_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_glitch_ctx_t *ctx = (cyber_glitch_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx == NULL || !ctx->active) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) return;

    lv_area_t o;
    lv_obj_get_coords(obj, &o);
    const int w = lv_area_get_width(&o);
    const int h = lv_area_get_height(&o);

    static const uint32_t COLORS[4] = { CYBER_MAGENTA, CYBER_CYAN, CYBER_RED, CYBER_CYAN };
    for (int lane = 0; lane < 4; lane++) {
        const uint32_t seed = ctx->anim_ms / 70u + (uint32_t)lane * 7919u;
        const int y = (int)(seed % (uint32_t)h);
        const int band_h = 2 + (int)((seed >> 5) % 6u);
        const int x = glitch_shift(ctx->anim_ms, lane);
        fill_rect(layer, &o, x, y, w, band_h, COLORS[lane], LV_OPA_40);
    }
}

static void glitch_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    cyber_glitch_ctx_t *ctx = (cyber_glitch_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx != NULL) {
        lv_free(ctx);
        lv_obj_set_user_data(obj, NULL);
    }
}

lv_obj_t *cyber_glitch_create(lv_obj_t *parent, int w, int h)
{
    if (parent == NULL) return NULL;
    cyber_glitch_ctx_t *ctx = (cyber_glitch_ctx_t *)lv_malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        lv_free(ctx);
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(obj, ctx);
    lv_obj_add_event_cb(obj, glitch_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(obj, glitch_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

void cyber_glitch_render(lv_obj_t *glitch, bool active, uint32_t anim_ms)
{
    if (glitch == NULL) return;
    cyber_glitch_ctx_t *ctx = (cyber_glitch_ctx_t *)lv_obj_get_user_data(glitch);
    if (ctx == NULL) return;

    if (!active) {
        ctx->active = false;
        ctx->valid = false;
        lv_obj_add_flag(glitch, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(glitch, LV_OBJ_FLAG_HIDDEN);
    ctx->active = true;
    ctx->anim_ms = anim_ms;
    ctx->valid = true;
    lv_obj_invalidate(glitch);
}
