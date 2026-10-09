// main/cyber_theme.h
// 赛博朋克像素风主题：【纯常量】，不依赖 LVGL / ESP-IDF，可被宿主单测直接包含。
//
// 颜色以 0xRRGGBB 存储，渲染层再转 lv_color_hex()，避免把 lv_color_t 泄进纯逻辑。
#pragma once

#include <stdint.h>

// ============================================================================
// 配色：近黑底 + 霓虹前景
// 只保留少量高饱和霓虹色，靠大面积暗底拉开对比，这是赛博朋克观感的来源。
// 颜色名描述"用途"而非"色相"，换配色时不至于改遍全工程。
// ============================================================================
#define CYBER_BG            0x05070DU  // 全局底色，接近纯黑但不刺眼
#define CYBER_PANEL         0x0C1424U  // 面板/机甲底色
#define CYBER_PANEL_HI      0x16233AU  // 面板高光面（面部受光侧）
#define CYBER_GRID          0x1B2A44U  // 网格线、扫描线等弱结构线

#define CYBER_CYAN          0x22F0FFU  // 主强调色：系统/待机
#define CYBER_MAGENTA       0xFF3BD4U  // 次强调色：警示/能量
#define CYBER_PURPLE        0x8A4BFFU  // 辅助色：睡眠/低功耗
#define CYBER_LIME          0xA8FF3EU  // 正向反馈：开心/成功
#define CYBER_AMBER         0xFFB22EU  // 提示色：聆听/等待
#define CYBER_RED           0xFF3B5CU  // 故障色：错误

#define CYBER_TEXT          0xD8F6FFU  // 正文
#define CYBER_TEXT_DIM      0x5E7C9EU  // 次要文字、提示条

// ============================================================================
// 屏幕几何：ST7789P3 240x320 竖屏
// 四角有 30px 圆角遮罩（见 bsp_display.h 的 BSP_LVGL_SCREEN_RADIUS），
// 因此四条边附近不放关键内容，文字一律居中或留足内缩。
// ============================================================================
#define CYBER_SCREEN_W      240
#define CYBER_SCREEN_H      320

#define CYBER_STATUS_H      22   // 顶部状态栏（电量固定在此栏右上角）
#define CYBER_STATUS_RULE   2    // 状态栏下沿的霓虹分隔线厚度
#define CYBER_HINT_TOP      (CYBER_SCREEN_H - 28)  // 底部提示条上边界

// ============================================================================
// 像素网格：面部用统一网格描述，换尺寸只需换"单位格边长"
// 网格固定 45x45；渲染尺寸取格的整数倍，保证像素不被半像素采样糊掉。
// ============================================================================
#define CYBER_FACE_GRID_W   45
#define CYBER_FACE_GRID_H   45

#define CYBER_FACE_HEAD_TOP   5    // 第 0..4 行留给天线
#define CYBER_FACE_HALF_MAX  22    // 头部最宽处半宽（格），即 44 格 ≈ 176px@4
#define CYBER_FACE_ROUND      4    // 上下圆角阶梯行数

// 面部在两种页面下的渲染尺寸（都必须能被网格整除）
#define CYBER_FACE_HOME_PX   180   // 180/45 = 4px/格
#define CYBER_FACE_VOICE_PX  135   // 135/45 = 3px/格

// ============================================================================
// 配对码
// ============================================================================
// 服务端固定下发 6 位十进制码（见 docs/development/engineering/xiaozhi-activation.md）。
// 拆成两行三列而非一行六列：48px 字体下一行六位要 288px，超过 240px 屏宽；
// 而 3 位一行只要 144px，能把字号撑到"隔着手臂距离也读得清"。
#define CYBER_PAIR_CODE_LEN    6
#define CYBER_PAIR_CODE_ROW    3    // 每行位数，必须能整除 CYBER_PAIR_CODE_LEN
// 成对：拆分函数按半长切分，奇数位会让两行不等宽、面板跳动。
_Static_assert(CYBER_PAIR_CODE_LEN % 2 == 0, "配对码位数必须为偶数才能均分两行");
_Static_assert(CYBER_PAIR_CODE_ROW * 2 == CYBER_PAIR_CODE_LEN,
               "两行位数之和必须等于配对码位数");

// ============================================================================
// 时序（毫秒）
// ============================================================================
#define CYBER_BOOT_MS          2600  // 开机动画总时长，之后自动进主界面
#define CYBER_BOOT_GLITCH_MS    420  // 开头信号不稳的时长
#define CYBER_BOOT_LOG_LINES      4  // 开机自检行数
// 开机页把守上限：等待首个 OTA 结果时最多额外停留这么久。
// 没有它，断网或服务端不响应就会让设备永远停在开机自检画面上。
#define CYBER_BOOT_HOLD_MAX_MS  12000

// 开机页进度条的里程碑刻度（%）。
// 等待首个 OTA 结果期间，进度不能再按时间硬凑：时间进度到点就满，之后只能靠
// "呼吸"来回动，看起来正是进度条在反复横跳。这里改成按【本次启动真实走过的阶段】
// 抬升——自检动画 → 已联网 → 检查在途 → 拿到结论，且只增不减。
// 刻度一律不取 100：进度一旦显示 100 就该切屏，停在 100 只会被当成死机。
#define CYBER_BOOT_PCT_SELFTEST  45  // 自检动画播完时的上限
#define CYBER_BOOT_PCT_NET       62  // Wi-Fi 已拿到地址
#define CYBER_BOOT_PCT_CHECK     80  // 激活检查请求在途
#define CYBER_BOOT_PCT_DONE      96  // 已拿到结论，本 tick 内即切屏
// 等待首个 OTA 结果时，Wi-Fi 仍没上来的宽限时间。超过它就别再等 OTA 了：
// 没有链路就没有结果可等，早点把 OFFLINE 摆到激活页上比空转的开机页诚实。
// 实测 Wi-Fi 拿到 IP 约 1.9s，4s 已经是很宽松的上限。
#define CYBER_BOOT_NET_GRACE_MS  4000

#define CYBER_LISTEN_TIMEOUT_MS 6000  // 聆听自动收尾（阶段 1 占位；联网后由服务器决定）
#define CYBER_THINK_TIMEOUT_MS  1400  // 思考最短展示时长
#define CYBER_SPEAK_MS_PER_CHAR   90  // 每字符朗读时长，用于估算占位字幕停留时间
#define CYBER_SPEAK_MIN_MS      1200
#define CYBER_SPEAK_MAX_MS      8000
#define CYBER_ERROR_MS          2200  // 错误提示自动消失

#define CYBER_BLINK_PERIOD_MS   3800  // 眨眼周期
#define CYBER_BLINK_MS           150  // 单次眨眼时长
#define CYBER_WAVE_PERIOD_MS     700  // 声波条基础周期

// 机甲脸细节层的动画节奏。集中放在这里而不是散在 cyber_face.c，
// 是为了让"动画快慢"能像配色一样一处调、并留给宿主单测覆盖。
#define CYBER_HUD_STEP_MS        150  // 面罩跑马灯每步时长（灯位推进一格）
#define CYBER_SCAN_PERIOD_MS    1100  // 面罩扫描线一个往返
#define CYBER_PULSE_PERIOD_MS   1600  // 额头核心的呼吸周期
#define CYBER_DATA_FLICKER_MS    130  // 数据总线闪烁步长

// 空闲降亮：可穿戴小电池设备，长时间亮屏是最主要耗电来源。
#define CYBER_IDLE_DIM_MS      20000  // 无操作 20s 降到暗背光
#define CYBER_IDLE_OFF_MS      45000  // 无操作 45s 关背光（保留渲染，按键即亮）
#define CYBER_BL_DIM_PERCENT      25
#define CYBER_BL_STEP_PERCENT     20  // 长按上/下调一档背光
