// main/cyber_prov_security.c
// BLUFI 安全协商的实现。
//
// 【协议代际：为什么是 1024 位 DH + MD5 + AES-CFB，而不是 IDF 6.1 自带示例那套】
// 实测手机端"蓝牙配网-FoloToy AI PASSPORT"小程序使用的是【经典 BLUFI 协议】：
//   - 1024 位自由 DH 参数（客户端下发 P / G / 公钥），而非 RFC7919 标准组；
//   - PSK = MD5(共享密钥)；
//   - 对称加密 AES-128-CFB128，每包 IV = 全零并将首字节替换为协议给的 iv8。
// 这与 IDF 6.1 自带示例（AES-CTR + SHA-256 + RFC7919 3072 位）【互不兼容】。
// 两者只能二选一，必须以手机端为准——所以这里按经典协议实现。
//
// 【为什么不用 mbedtls_dhm / mbedtls_aes / mbedtls_md5】
// IDF 6.1 的 mbedtls 4.x 已把这些旧 API 的【公开头文件】移除（只剩 private/ 与
// 编译进库的符号），照抄旧 demo 会直接编译失败。可用替代：
//   - DH  : mbedtls_mpi（大数运算）—— port/include/mbedtls/bignum.h 是个 wrapper，
//           会 #include_next 到 mbedtls/private/bignum.h，因此完整 MPI API 可用，
//           且 exp_mod 走 ESP32-C3 的 RSA 硬件加速器，1024 位一次只要毫秒级；
//   - MD5 : PSA（PSA_ALG_MD5，由 CONFIG_MBEDTLS_MD5_C 启用）；
//   - CFB : PSA（PSA_ALG_CFB，由 CONFIG_MBEDTLS_CIPHER_MODE_CFB 启用）。
// 这三者都已在 sdkconfig.defaults 里确认开启，改动时必须一并核对。
#include "cyber_prov_security.h"

#include <stdlib.h>
#include <string.h>

#include "esp_blufi_api.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/bignum.h"
#include "psa/crypto.h"

static const char *TAG = "cyber_prov_sec";

#define SEC_TYPE_DH_PARAM_LEN   0x00
#define SEC_TYPE_DH_PARAM_DATA  0x01

#define DH_PARAM_LEN_MAX   1024   // 参数体总长上限（含 P/G/公钥三段）
#define DH_KEY_LEN_MAX     512    // 支持到 4096 位；经典协议实际只用 128（1024 位）
#define PSK_LEN            16     // MD5 输出 = AES-128 密钥
#define BLUFI_IV_SIZE      16

typedef struct {
    uint8_t *dh_param;
    int dh_param_len;
    // AES 密钥（PSA），由 MD5(共享密钥) 导入。
    psa_key_id_t aes_key;
    bool have_key;
} blufi_sec_t;

static blufi_sec_t *s_sec;

// CFB 是流密码：输出长度恒等于输入长度。PSA 的一次性接口不允许输入输出重叠，
// 所以需要一个同长的中转缓冲。BLUFI 单包不会超过这个长度（分片由协议层负责）。
#define CFB_BOUNCE_LEN 512
static uint8_t s_cfb_bounce[CFB_BOUNCE_LEN];

// BLUFI 内部错误上报入口（由 blufi 组件提供）。协商失败必须回具体错误码，
// 否则手机侧只会看到连接卡住而拿不到原因。
extern void btc_blufi_report_error(esp_blufi_error_state_t state);

// mbedtls_mpi_fill_random 需要 RNG 回调；用 ESP 的硬件随机数。
static int mpi_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

static void reset_sec(void)
{
    if (s_sec == NULL) return;
    free(s_sec->dh_param);
    s_sec->dh_param = NULL;
    s_sec->dh_param_len = 0;
    if (s_sec->have_key) {
        psa_destroy_key(s_sec->aes_key);
        s_sec->aes_key = 0;
        s_sec->have_key = false;
    }
}

// 从 DH 参数体里解出三段：P、G、对端公钥。返回 false 表示格式非法。
static bool parse_dh_param(const uint8_t *param, size_t total,
                           const uint8_t **p, size_t *p_len,
                           const uint8_t **g, size_t *g_len,
                           const uint8_t **pub, size_t *pub_len)
{
    size_t off = 0;
    // 每段都是「2 字节大端长度 + 内容」，逐段做边界检查——完全由对端控制的输入，
    // 任何一段越界都会读穿缓冲。
    if (total < 2) return false;
    *p_len = (size_t)((param[0] << 8) | param[1]);
    off = 2;
    if (off + *p_len > total) return false;
    *p = param + off;
    off += *p_len;

    if (off + 2 > total) return false;
    *g_len = (size_t)((param[off] << 8) | param[off + 1]);
    off += 2;
    if (off + *g_len > total) return false;
    *g = param + off;
    off += *g_len;

    if (off + 2 > total) return false;
    *pub_len = (size_t)((param[off] << 8) | param[off + 1]);
    off += 2;
    if (off + *pub_len > total) return false;
    *pub = param + off;

    return (*p_len > 0 && *g_len > 0 && *pub_len > 0);
}

// 经典 DH：本端私钥随机生成，公钥 = G^x mod P，共享密钥 = peer^x mod P。
// 全部用 mbedtls_mpi（走 RSA 硬件加速），不依赖 PSA 的 FFDH（后者只支持标准组）。
static bool dh_key_exchange(const uint8_t *p, size_t p_len,
                            const uint8_t *g, size_t g_len,
                            const uint8_t *peer, size_t peer_len,
                            uint8_t *out_pub, size_t *out_pub_len,
                            uint8_t *out_shared, size_t *out_shared_len)
{
    mbedtls_mpi P, G, X, GX, GY, K;
    bool ok = false;

    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&G);
    mbedtls_mpi_init(&X);
    mbedtls_mpi_init(&GX);
    mbedtls_mpi_init(&GY);
    mbedtls_mpi_init(&K);

    if (mbedtls_mpi_read_binary(&P, p, p_len) != 0) goto done;
    if (mbedtls_mpi_read_binary(&G, g, g_len) != 0) goto done;
    if (mbedtls_mpi_read_binary(&GY, peer, peer_len) != 0) goto done;

    // 私钥长度取模数长度（与经典 BLUFI 一致：1024 位组用 128 字节私钥）。
    const size_t key_len = mbedtls_mpi_size(&P);
    if (key_len == 0 || key_len > DH_KEY_LEN_MAX) goto done;

    if (mbedtls_mpi_fill_random(&X, key_len, mpi_rng, NULL) != 0) goto done;
    // 公钥：G^X mod P。
    if (mbedtls_mpi_exp_mod(&GX, &G, &X, &P, NULL) != 0) goto done;
    if (mbedtls_mpi_write_binary(&GX, out_pub, key_len) != 0) goto done;
    *out_pub_len = key_len;

    // 共享密钥：peer^X mod P。写定长（左侧补零）——经典实现同样按模长输出，
    // 补零与否会改变 MD5 输入，必须与对端一致。
    if (mbedtls_mpi_exp_mod(&K, &GY, &X, &P, NULL) != 0) goto done;
    if (mbedtls_mpi_write_binary(&K, out_shared, key_len) != 0) goto done;
    *out_shared_len = key_len;

    ok = true;

done:
    // 私钥与共享密钥是敏感材料：释放前先清零。
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&GX);
    mbedtls_mpi_free(&GY);
    mbedtls_mpi_free(&K);
    return ok;
}

void cyber_prov_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free)
{
    if (data == NULL || len < 3) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }
    if (output_data == NULL || output_len == NULL || need_free == NULL) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }
    if (s_sec == NULL) {
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    const uint8_t type = data[0];

    if (type == SEC_TYPE_DH_PARAM_LEN) {
        // 第一步：手机先下行参数长度（两字节大端），设备据此分配缓冲。
        const int param_len = (data[1] << 8) | data[2];
        if (param_len <= 0 || param_len > DH_PARAM_LEN_MAX) {
            ESP_LOGE(TAG, "DH 参数长度非法: %d", param_len);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        // 允许重新协商：先把上一轮清干净，否则旧密钥会污染这一轮。
        reset_sec();
        s_sec->dh_param = malloc((size_t)param_len);
        if (s_sec->dh_param == NULL) {
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            return;
        }
        s_sec->dh_param_len = param_len;
        return;
    }

    if (type != SEC_TYPE_DH_PARAM_DATA) {
        ESP_LOGW(TAG, "未知的协商包类型: 0x%02x", type);
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    if (s_sec->dh_param == NULL || len < s_sec->dh_param_len + 1) {
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }
    memcpy(s_sec->dh_param, data + 1, (size_t)s_sec->dh_param_len);

    const uint8_t *P = NULL, *G = NULL, *PUB = NULL;
    size_t p_len = 0, g_len = 0, pub_len = 0;
    if (!parse_dh_param(s_sec->dh_param, (size_t)s_sec->dh_param_len,
                        &P, &p_len, &G, &g_len, &PUB, &pub_len)) {
        ESP_LOGE(TAG, "DH 参数解析失败（长度 %d）", s_sec->dh_param_len);
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }

    ESP_LOGI(TAG, "DH 参数：P=%u 字节 G=%u 字节 对端公钥=%u 字节（%u 位）",
             (unsigned)p_len, (unsigned)g_len, (unsigned)pub_len,
             (unsigned)(pub_len * 8u));

    // 自己算 DH。注意经典协议用的是【自由参数 1024 位组】，PSA 的 FFDH 只支持
    // RFC7919 标准组，所以这里必须用 mbedtls_mpi 手工做。
    static uint8_t pub_out[DH_KEY_LEN_MAX];
    static uint8_t shared[DH_KEY_LEN_MAX];
    size_t pub_out_len = 0, shared_len = 0;
    if (!dh_key_exchange(P, p_len, G, g_len, PUB, pub_len,
                         pub_out, &pub_out_len, shared, &shared_len)) {
        ESP_LOGE(TAG, "DH 计算失败");
        free(s_sec->dh_param);
        s_sec->dh_param = NULL;
        btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }
    free(s_sec->dh_param);
    s_sec->dh_param = NULL;

    // PSK = MD5(共享密钥)——经典协议的固定派生方式（新版才改成 SHA-256）。
    uint8_t psk[PSK_LEN];
    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_MD5, shared, shared_len,
                                       psk, sizeof(psk), &hash_len);
    // 共享密钥用完即清零：它等价于长期密钥。
    memset(shared, 0, sizeof(shared));
    if (st != PSA_SUCCESS || hash_len != PSK_LEN) {
        ESP_LOGE(TAG, "MD5 派生 PSK 失败: %d", (int)st);
        btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
        return;
    }

    // 导入 AES-128 密钥（CFB 模式）。
    reset_sec();
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, PSK_LEN * 8);
    psa_set_key_algorithm(&attr, PSA_ALG_CFB);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    st = psa_import_key(&attr, psk, sizeof(psk), &s_sec->aes_key);
    psa_reset_key_attributes(&attr);
    memset(psk, 0, sizeof(psk));
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES 密钥导入失败: %d", (int)st);
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }
    s_sec->have_key = true;

    *output_data = pub_out;
    *output_len = (int)pub_out_len;
    *need_free = false;
    ESP_LOGI(TAG, "安全协商完成（MD5 + AES-128-CFB，%u 位 DH）",
             (unsigned)(pub_len * 8u));
}

// AES-CFB128：每包 IV = 全零、首字节替换为协议给的 iv8（经典 BLUFI 的约定）。
// 注意必须【每包重新 setup + set_iv】——CFB 的计数器若跨包延续，双方会算出不同的
// 密钥流。PSA 的一次性接口不接受 IV，所以这里用多段接口。
static int cfb_crypt(bool encrypt, uint8_t iv8, uint8_t *data, int len)
{
    if (s_sec == NULL || data == NULL || len < 0) return -1;
    if (len == 0) return 0;
    if (!s_sec->have_key) {
        ESP_LOGE(TAG, "AES 密钥尚未建立");
        return -1;
    }
    if ((size_t)len > sizeof(s_cfb_bounce)) {
        ESP_LOGE(TAG, "单包过长（%d > %u），超出中转缓冲", len,
                 (unsigned)sizeof(s_cfb_bounce));
        return -1;
    }

    uint8_t iv[BLUFI_IV_SIZE] = { 0 };
    iv[0] = iv8;

    psa_cipher_operation_t op = psa_cipher_operation_init();
    // CFB 是流密码，加解密用同一个 setup（PSA_ALG_CFB 对两个方向都接受）。
    psa_status_t st = encrypt
        ? psa_cipher_encrypt_setup(&op, s_sec->aes_key, PSA_ALG_CFB)
        : psa_cipher_decrypt_setup(&op, s_sec->aes_key, PSA_ALG_CFB);
    if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, iv, sizeof(iv));

    size_t out_len = 0, fin_len = 0;
    if (st == PSA_SUCCESS) {
        st = psa_cipher_update(&op, data, (size_t)len,
                               s_cfb_bounce, sizeof(s_cfb_bounce), &out_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_cipher_finish(&op, s_cfb_bounce + out_len,
                               sizeof(s_cfb_bounce) - out_len, &fin_len);
    }
    psa_cipher_abort(&op);

    if (st != PSA_SUCCESS || (out_len + fin_len) != (size_t)len) {
        ESP_LOGE(TAG, "AES-CFB 失败: %d（out=%u+%u len=%d）", (int)st,
                 (unsigned)out_len, (unsigned)fin_len, len);
        return -1;
    }
    memcpy(data, s_cfb_bounce, out_len + fin_len);
    return (int)(out_len + fin_len);
}

int cyber_prov_encrypt(uint8_t iv8, uint8_t *data, int len)
{
    return cfb_crypt(true, iv8, data, len);
}

int cyber_prov_decrypt(uint8_t iv8, uint8_t *data, int len)
{
    return cfb_crypt(false, iv8, data, len);
}

uint16_t cyber_prov_checksum(uint8_t iv8, uint8_t *data, int len)
{
    (void)iv8;
    return esp_crc16_be(0, data, len);
}

int cyber_prov_security_init(void)
{
    // PSA 子系统可能已被其他组件初始化；这里是幂等的。
    const psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init 失败: %d", (int)st);
        return -1;
    }

    cyber_prov_security_deinit();
    s_sec = calloc(1, sizeof(*s_sec));
    if (s_sec == NULL) return -1;
    return 0;
}

void cyber_prov_security_deinit(void)
{
    if (s_sec == NULL) return;
    reset_sec();
    memset(s_sec, 0, sizeof(*s_sec));
    free(s_sec);
    s_sec = NULL;
}
