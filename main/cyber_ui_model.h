// main/cyber_ui_model.h
// 赛博像素 AI 助手的界面模型：【纯逻辑】，不依赖 ESP-IDF / LVGL，可直接跑宿主单测。
//
// 这里集中三类"可以脱离硬件验证"的东西：
//   1. 屏幕/表情/语音状态机（含按键语义与超时回落）；
//   2. 动画与进度计算（开机进度、眨眼、声波条）——纯函数，输入时间输出数值；
//   3. 面部几何（像素网格 → 横带）——布局计算，便于单测覆盖边界。
// 渲染（lv_*）只在 cyber_face.c / cyber_ui.c 里出现，见 docs/development/ai-guide.md
// 关于"把可测逻辑与 ESP-IDF/LVGL 隔离"的要求。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyber_theme.h"

// ---------------------------------------------------------------------------
// 屏幕
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_SCREEN_BOOT = 0,   // 开机自检动画
    CYBER_SCREEN_HOME,       // 主界面：机甲脸 + 状态栏
    CYBER_SCREEN_VOICE,      // 语音反馈：聆听/思考/说话/出错
    CYBER_SCREEN_ACTIVATION, // 未绑定：显示配对码等用户在控制台绑定
} cyber_screen_t;

// ---------------------------------------------------------------------------
// 配对状态：小智服务的激活流程在界面上的投影。
//
// 与 cyber_ota_state_t 是两个不同的枚举，看起来重复，但分开是有意的：
//   - cyber_ui_model 必须能脱离 ESP-IDF 单独编译和单测，不能 include cyber_ota.h；
//   - 界面关心的是"该给用户看什么"，而不是"HTTP 走到了哪一步"。映射在 main.c 里做，
//     那是唯一同时看得到两边的地方。
// ---------------------------------------------------------------------------
typedef enum {
    // 激活流程未启用。阶段 1 与宿主单测都用这个值，界面行为与接入激活前完全一致。
    CYBER_PAIR_OFF = 0,
    CYBER_PAIR_LINKING,     // 已联网，正在向服务端查询
    CYBER_PAIR_WAITING,     // 已拿到配对码，等用户在控制台绑定
    CYBER_PAIR_READY,       // 已绑定，可以开始对话
    CYBER_PAIR_TEST_ONLY,   // 只有临时测试凭证：能连上但绑定没生效
    CYBER_PAIR_OFFLINE,     // 未联网，查不了
    CYBER_PAIR_ERROR,       // 查询失败（网络/TLS/协议）
    CYBER_PAIR_COUNT,
} cyber_pair_state_t;

// ---------------------------------------------------------------------------
// 表情：决定眼睛形状与霓虹主色
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_EMO_NEUTRAL = 0,   // 待机方块眼，青
    CYBER_EMO_HAPPY,         // 上弧眼，青柠
    CYBER_EMO_CURIOUS,       // 大小眼，琥珀
    CYBER_EMO_FOCUS,         // 窄缝眼，青
    CYBER_EMO_ALERT,         // 圆睁 + 高光，品红
    CYBER_EMO_SLEEPY,        // 闭眼横线，紫
    CYBER_EMO_GLITCH,        // 错位碎眼，红
    CYBER_EMO_COUNT,
} cyber_emotion_t;

// ---------------------------------------------------------------------------
// 语音状态：阶段 1 由本地占位流程驱动；阶段 2 改由小智下行事件驱动，
// 状态机本身不变（见 cyber_model_voice_event）。
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_VOICE_IDLE = 0,
    CYBER_VOICE_LISTENING,   // 收音中
    CYBER_VOICE_THINKING,    // 等待回复
    CYBER_VOICE_SPEAKING,    // 播放回复
    CYBER_VOICE_ERROR,       // 失败
} cyber_voice_state_t;

// ---------------------------------------------------------------------------
// 按键：与 BSP 解耦（bsp_btn_t / bsp_btn_ev_t 在编排层换算）
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_BTN_UP = 0,
    CYBER_BTN_DOWN,
    CYBER_BTN_OK,
    CYBER_BTN_COUNT,
} cyber_btn_t;

typedef enum {
    CYBER_BTN_EV_CLICK = 0,
    CYBER_BTN_EV_DOUBLE,
    CYBER_BTN_EV_LONG,
} cyber_btn_ev_t;

// ---------------------------------------------------------------------------
// 模型输出动作：编排层执行。
// 需要带参数的动作（目前只有背光）从模型字段读取，见各字段注释。
// ---------------------------------------------------------------------------
typedef enum {
    CYBER_ACT_NONE = 0,
    CYBER_ACT_START_LISTEN,   // 开始收音
    CYBER_ACT_STOP_LISTEN,    // 结束收音
    CYBER_ACT_ABORT,          // 打断本轮（阶段 2 会下发服务器 abort）
    CYBER_ACT_BACKLIGHT,      // 按 model->backlight_percent 设背光
} cyber_action_t;

// 空闲档位：可穿戴设备靠降背光省电，渲染不停，按键立即恢复。
typedef enum {
    CYBER_IDLE_ACTIVE = 0,
    CYBER_IDLE_DIM,           // 暗背光
    CYBER_IDLE_OFF,           // 背光全灭
} cyber_idle_t;

// 开机页的放行方式。
//
// HOLD 是为了消除一次观感错误：设备重启后"是否已绑定"要等首个 OTA 响应才知道，
// 而 NVS 里的旧结论（paired）会让界面先跳到主界面，几秒后再被激活页顶掉。
// 用户看到的是"机甲脸闪一下然后跳到配对码"，像是设备出错了。
// HOLD 让开机动画一直演到有结论为止，之后直接落到正确的那一屏。
typedef enum {
    CYBER_BOOT_GATE_AUTO = 0,  // 播完 CYBER_BOOT_MS 自动进下一屏（默认，宿主单测沿用）
    CYBER_BOOT_GATE_HOLD,      // 停在开机页等外部放行；最长 CYBER_BOOT_HOLD_MAX_MS
} cyber_boot_gate_t;

// 开机页进度条的数据源：只描述【本次启动的链路走到哪一步】。
//
// 刻意不含 NVS 里的旧绑定结论：进度条要反映的是这次真实完成了什么，而不是上一轮
// 存下的状态——否则一台"曾绑定过"的设备一上电就会把进度顶到满，等于撒谎。
// 映射由 main.c 从 cyber_ota_state_t 换算（那是唯一同时看得到两边的地方）。
typedef enum {
    CYBER_BOOT_LINK_IDLE = 0,  // 尚未联网
    CYBER_BOOT_LINK_NET,       // 已联网，等激活客户端发问
    CYBER_BOOT_LINK_CHECKING,  // 检查请求在途
    CYBER_BOOT_LINK_DONE,      // 已拿到结论（随后切屏）
    CYBER_BOOT_LINK_OFFLINE,   // 连不上服务端（随后切屏）
    CYBER_BOOT_LINK_COUNT,
} cyber_boot_link_t;

// 占位字幕缓冲：阶段 1 放本地 mock 短句，阶段 2 放服务器 LLM/TTS 文本。
#define CYBER_SUBTITLE_MAX 96

// ---------------------------------------------------------------------------
// 模型状态
// ---------------------------------------------------------------------------
typedef struct {
    cyber_screen_t screen;
    cyber_emotion_t emotion;     // 主界面用户选择的表情
    cyber_emotion_t voice_emotion; // 说话态由外部（服务器）给的情绪
    cyber_voice_state_t voice;
    cyber_idle_t idle;

    uint32_t uptime_ms;          // 自启动累计
    uint32_t screen_ms;          // 进入当前屏幕后累计
    uint32_t voice_ms;           // 进入当前语音状态后累计
    uint32_t anim_ms;            // 自由运行动画时钟（眨眼、扫描线、声波）
    uint32_t idle_ms;            // 距上次按键的累计时间

    uint8_t backlight_user;      // 用户偏好亮度；降亮/关屏是它的临时覆盖
    uint8_t backlight_percent;   // 当前实际背光，CYBER_ACT_BACKLIGHT 的取值来源
    uint8_t mock_index;          // 阶段 1 占位回复轮转序号
    int battery_soc;             // -1 = 读不到，UI 侧优雅降级
    bool net_connected;

    cyber_pair_state_t pair;     // 配对流程状态（CYBER_PAIR_OFF = 未启用）
    char pair_code[CYBER_PAIR_CODE_LEN + 1];  // 仅 WAITING 时有 6 位数字；否则为 ""

    cyber_boot_gate_t boot_gate; // 开机页是否要等首个 OTA 结果再放行
    cyber_boot_link_t boot_link; // 开机页进度条的链路阶段（由 main.c 从 OTA 状态映射）
    uint8_t boot_pct;            // 开机页当前进度 0..100，只增不减

    char subtitle[CYBER_SUBTITLE_MAX + 1];
} cyber_ui_model_t;

// ---------------------------------------------------------------------------
// 初始化与推进
// ---------------------------------------------------------------------------
void cyber_model_init(cyber_ui_model_t *m);

// 推进 dt_ms。超时类状态迁移产生的动作由此返回（无则 CYBER_ACT_NONE）。
cyber_action_t cyber_model_tick(cyber_ui_model_t *m, uint32_t dt_ms);

// 输入一个按键事件，返回要执行的动作。
cyber_action_t cyber_model_button(cyber_ui_model_t *m, cyber_btn_t btn,
                                  cyber_btn_ev_t ev);

// 外部状态注入（电量/网络）。带 -1 或 false 时 UI 必须优雅降级。
void cyber_model_set_battery(cyber_ui_model_t *m, int soc);
void cyber_model_set_net(cyber_ui_model_t *m, bool connected);

// 外部状态注入（配对流程）。code 可为 NULL；只有 WAITING 时它才非空。
// "是否为合法 6 位数字"的校验发生在 cyber_ota 这一侧的边界上（cyber_ota_code_valid），
// 模型不再重复判断：外部输入在入口处验一次，比每个消费方各验一次更不容易漂移。
void cyber_model_set_pair(cyber_ui_model_t *m, cyber_pair_state_t state,
                          const char *code);

// 设置开机页放行方式。默认 CYBER_BOOT_GATE_AUTO（播完就走），
// 联网设备在启动时设为 CYBER_BOOT_GATE_HOLD，等 main.c 拿到首个 OTA 结果后放行。
// 置回 AUTO 是幂等的：放行之后本函数可以随便重复调用。
void cyber_model_set_boot_gate(cyber_ui_model_t *m, cyber_boot_gate_t gate);

// 设置开机页进度条的链路阶段。main.c 每个 tick 从 OTA 状态映射后写入。
void cyber_model_set_boot_link(cyber_ui_model_t *m, cyber_boot_link_t link);

// 开机页进度条应显示的百分比（0..100），读的是模型里"只增不减"的 boot_pct。
uint8_t cyber_boot_meter(const cyber_ui_model_t *m);

// 链路阶段的短标签（开机页底部提示条用）。
const char *cyber_boot_link_label(cyber_boot_link_t link);

// 开机动画已播完、正在等首个 OTA 结果。渲染层据此切换"100% 卡住"的观感，
// 否则一块不动的画面会被用户当成死机。
bool cyber_boot_waiting(const cyber_ui_model_t *m);

// 阶段 2 接口：把小智下行事件灌进状态机。text 可为 NULL（非说话态）。
// 阶段 1 不调用它，但状态机与 UI 已按同一套状态设计，接服务器时无需改渲染。
void cyber_model_voice_event(cyber_ui_model_t *m, cyber_voice_state_t state,
                             cyber_emotion_t emo, const char *text);

// ---------------------------------------------------------------------------
// 派生查询（渲染层只读消费）
// ---------------------------------------------------------------------------
// 当前该显示的表情：语音页由语音状态覆盖，主界面用用户选择。
cyber_emotion_t cyber_model_visible_emotion(const cyber_ui_model_t *m);
// 表情主色。
uint32_t cyber_emotion_color(cyber_emotion_t e);
// 状态栏左侧文字（"ONLINE" / "OFFLINE"）。
const char *cyber_model_link_label(const cyber_ui_model_t *m);
// 语音状态标签与强调色。
const char *cyber_voice_label(cyber_voice_state_t s);
uint32_t cyber_voice_color(cyber_voice_state_t s);
// 主界面显示的表情名（短，供状态行用）。
const char *cyber_emotion_label(cyber_emotion_t e);

// 配对状态是否必须占住屏幕（即：未绑定或连不上服务端）。
// 为真时界面停在激活页：主界面上的语音入口在绑定成功前根本走不通，
// 先给用户一个"能按但没用"的脸会误导人。CYBER_PAIR_OFF 与 CYBER_PAIR_READY 为假。
bool cyber_pair_blocks_ui(cyber_pair_state_t s);
// 配对状态的短标签与强调色。
const char *cyber_pair_label(cyber_pair_state_t s);
uint32_t cyber_pair_color(cyber_pair_state_t s);

// ---------------------------------------------------------------------------
// 动画与进度：纯函数，可单测
// ---------------------------------------------------------------------------
// 开机进度 0..100，elapsed >= duration 时恒为 100。
uint8_t cyber_boot_progress(uint32_t elapsed_ms, uint32_t duration_ms);
// 等 OTA 期间的开机进度 0..100：自检时间成分只走到 CYBER_BOOT_PCT_SELFTEST，
// 其余由链路阶段（已联网/检查在途/有结论）抬升。纯函数，便于单测钉住单调性。
uint8_t cyber_boot_phase_pct(uint32_t elapsed_ms, uint32_t duration_ms,
                             cyber_boot_link_t link);
// 已出现的自检行数 0..total（前 70% 时间走完所有行）。
uint8_t cyber_boot_log_count(uint32_t elapsed_ms, uint32_t duration_ms, uint8_t total);
// 开机信号抖动是否处于激活窗口（开头一段 + 70% 处瞬时一次）。
bool cyber_boot_glitch(uint32_t elapsed_ms, uint32_t duration_ms);
// 眨眼：anim_ms 落在每个眨眼周期起始的短窗口内时为真。
bool cyber_face_blink(uint32_t anim_ms);
// 声波条高度 0..255：确定性、随时间变化，用于聆听态 VU 表。
uint8_t cyber_wave_level(uint32_t anim_ms, uint8_t index, uint8_t count);
// 呼吸亮度 0..255：三角波，用于额头核心、天线顶灯等"有能量涨落"的部件。
// period_ms < 2 时恒返回 255（无周期可言，按常亮处理），避免除零。
uint8_t cyber_pulse(uint32_t anim_ms, uint32_t period_ms);
// HUD 跑马灯灯位，恒落在 [0, track_len)。track_len <= 0 返回 0。
// phase 为灯位偏移，传负数即可得到"拖尾"（同一序列里错开若干格）。
int cyber_hud_chase(uint32_t anim_ms, int track_len, int phase);
// 往返扫描位置，恒落在 [0, span)。span <= 0 返回 0。用于面罩扫描线。
int cyber_visor_scan_index(uint32_t anim_ms, int span);
// 占位字幕应停留的毫秒数（按字数估算，带上下限）。
uint32_t cyber_speak_duration_ms(size_t text_len);

// 把配对码均分到两行（各 CYBER_PAIR_CODE_ROW 个字符 + 结尾 NUL）。
// 不足位用 '-' 补齐，保证两行等宽、数字面板不会因为码短而左右跳动；
// code 为 NULL 或空串时两行全是占位符，画面依旧稳定。
// row1/row2 至少要有 CYBER_PAIR_CODE_ROW + 1 字节。
void cyber_pair_split_code(const char *code, char *row1, size_t cap1,
                           char *row2, size_t cap2);

// ---------------------------------------------------------------------------
// 面部几何：像素网格 → 可绘制的横带
// ---------------------------------------------------------------------------
// 一"横带"= 若干连续行，半宽相同，渲染时合并成一个矩形，避免逐行画 40 个矩形。
typedef struct {
    int row0;   // 起始行（含），格坐标
    int row1;   // 结束行（含），格坐标
    int half;   // 半宽（格）；实际列范围 [CENTER-half, CENTER+half]
} cyber_face_band_t;

// 面部网格中心列。
#define CYBER_FACE_CENTER (CYBER_FACE_GRID_W / 2)

// 第 row 行的半宽（格）；返回 <= 0 表示该行无实体（天线区/越界）。
int cyber_face_half_width(int row);

// 把等宽行合并为横带，返回写入的带数（不超过 max_bands）。
int cyber_face_head_bands(cyber_face_band_t *out, int max_bands);
