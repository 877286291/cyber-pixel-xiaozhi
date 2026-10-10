// main/cyber_ota_util.h
// 小智服务激活的【纯逻辑】：不依赖 ESP-IDF / LVGL，可直接跑宿主单测。
//
// 与 cyber_net_util 同一定位，但管的是"激活"而非"联网"：
//   1. 激活码格式校验；
//   2. 等用户绑定期间的轮询节奏；
//   3. 由检查响应判定下一步（要激活 / 可对话 / 无可用配置）；
//   4. 测试凭证识别（GID_test / test-token）；
//   5. 检查请求体的构造。
// 规格见 docs/development/engineering/xiaozhi-activation.md。
//
// 为什么请求体构造也放在纯逻辑里：请求体是我们自己完全可控的一小组字段，
// 手写 JSON 既不需要分配、又能被单测逐字节钉住；而响应是外部输入、字段可变，
// 必须交给真正的 JSON 解析器（在 cyber_ota.c 里用 cJSON）。这个分工是有意的。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 激活码固定 6 位十进制数字。缓冲需 +1 容纳结尾。
#define CYBER_OTA_CODE_LEN   6
#define CYBER_OTA_CODE_CAP   (CYBER_OTA_CODE_LEN + 1)

// 轮询节奏上下限（毫秒）。
// 下限 2s：用户就坐在屏幕前输码，再快只是白耗电和流量。
// 上限 8s：绑定完成后最迟 8s 内设备就会拿到配置，观感上"立刻就绪"。
#define CYBER_OTA_POLL_MIN_MS 2000u
#define CYBER_OTA_POLL_MAX_MS 8000u

// 一次检查的去向。
typedef enum {
    // 服务端给了激活码：显示它，等用户在控制台绑定。
    CYBER_OTA_NEED_ACTIVATION = 0,
    // 拿到的是【非临时】对话端点：已绑定，可以开对话通道。
    CYBER_OTA_READY,
    // 有对话端点但 token 是临时的，且没有激活码：卡在测试账号上（issue #1785 的特征）。
    // 单列出来是为了让日志/界面能说清"能对话但绑定不生效"，而不是伪装成就绪。
    CYBER_OTA_TEST_ONLY,
    // 两者都没有：服务端没给可用配置（异常或协议变更），应视为失败而非成功。
    CYBER_OTA_NO_CONFIG,
} cyber_ota_outcome_t;

// 检查请求体的输入。字段名与官方协议一致，见规格文档。
typedef struct {
    const char *mac;          // "aa:bb:cc:dd:ee:ff"
    const char *board_type;   // 本应用的板型标识
    const char *app_name;
    const char *app_version;
    const char *idf_version;  // 例如 "v6.1"
    uint32_t flash_size;
    uint32_t min_free_heap;
} cyber_ota_body_t;

// 激活码是否为恰好 6 位 ASCII 数字。NULL 或长度不符一律为假。
bool cyber_ota_code_valid(const char *code);

// 第 attempt 次轮询前的等待毫秒数（attempt 从 1 开始）。
// attempt == 0 返回 0（无需等待）。序列：2s, 4s, 6s，之后稳定在 8s。
uint32_t cyber_ota_poll_delay_ms(uint8_t attempt);

// 递增轮询计数（饱和到 4，绝不回绕）。
// 与 Wi-Fi 重连同样的理由：无饱和的 uint8_t 计数一旦回绕，delay 会变成 0，
// 轮询就此静默停止。
uint8_t cyber_ota_poll_bump(uint8_t attempt);

// 由响应中的三个事实判定下一步。
//
// 判定顺序是实测出来的，不是想当然（见规格文档的"Verified against the live service"）：
// 未绑定的设备会【同时】拿到 activation.code 和一份临时 websocket 凭证
// （token 字面量为 "test-token"）。因此"有 websocket 就算就绪"是错的——那会让设备
// 自信地报 READY 并一直挂在临时账号上，激活码永远不显示。
// 真正表示"需要绑定"的信号是 activation.code 是否存在。
//
// 判定规则（实测得出，见规格文档的"Verified against the live service"）：
// 服务端【只对尚未绑定的设备】下发 activation。一旦绑定生效，它就不再返回
// activation，但可能仍返回临时凭证（#1785）。所以"不再要求激活 + 有可用端点"
// 就是"已绑定"的判据——不存在"既不要求激活、又确实未绑定"的响应。
//
// 因此 TEST_ONLY 不是阻塞态：它表示"端点可用，但服务端给的仍是临时账号"。
// 设备照常进主界面（临时账号本身是可用的），只把这一事实记进日志与快照供排查。
cyber_ota_outcome_t cyber_ota_decide(bool has_activation_code, bool has_websocket,
                                     bool is_test_credentials);

// 是否为服务端的临时测试 token（字面量 "test-token"）。
// 未绑定状态下服务端会下发临时账号，这是【正常】的；只有在绑定之后仍如此才是问题。
// 显式识别它，是为了让日志能说清"当前是临时账号"，而不是让排查者自己猜。
bool cyber_ota_is_test_token(const char *token);

// client_id 是否形如 "GID_test@@@..."，含义同 cyber_ota_is_test_token。
bool cyber_ota_is_test_client_id(const char *client_id);

// 构造检查请求体（写入 out，含结尾 '\0'）。
// 返回写入的字节数（不含结尾）；任何输入含 JSON 需要转义的字符、或 cap 不足时返回 0。
// 不转义而是拒绝：这些字段全部来自固件自身常量与 MAC，出现引号/反斜杠只可能是
// 调用方传错了东西，静默转义会把一个 bug 变成一份看起来正常却语义错误的请求。
size_t cyber_ota_build_body(char *out, size_t cap, const cyber_ota_body_t *b);
