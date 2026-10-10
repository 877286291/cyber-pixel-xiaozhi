// main/cyber_prov_security.h
// BLUFI 的安全协商：DH 交换 → 共享密钥 → MD5 派生 PSK → AES-128-CFB 加解密，
// 并对每帧做 CRC16 校验。没有这套回调，手机侧无法完成协商，凭证就只能明文下行。
//
// 【协议代际：以手机端为准】
// 实测手机小程序用的是【经典 BLUFI 协议】：1024 位自由 DH 参数 + MD5 派生 +
// AES-128-CFB128（每包 IV = 全零、首字节替换为协议给的 iv8）。这与 ESP-IDF 6.1
// 自带示例的 RFC7919 3072 位 + SHA-256 + AES-CTR 【互不兼容】，两者只能二选一；
// 因此本实现跟随手机端，采用经典协议。
//
// 【本文件为什么不用 mbedtls_dhm / mbedtls_aes / mbedtls_md5】
// IDF 6.1 的 mbedtls 4.x 已移除这些旧 API 的公开头文件（只剩 private/ 与库内符号），
// 照抄旧 demo 会编译失败。替代方案见 .c 顶部说明：
//   DH  → mbedtls_mpi（bignum.h 是 wrapper，完整 API 可用，且走 RSA 硬件加速）
//   MD5 → PSA（PSA_ALG_MD5）
//   CFB → PSA（PSA_ALG_CFB）
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 分配安全上下文。可重复调用（会先释放上一份）。0 表示成功。
int cyber_prov_security_init(void);

// 释放安全上下文并清零密钥材料。幂等。
void cyber_prov_security_deinit(void);

// 下列四个即 esp_blufi_callbacks_t 的对应字段，直接挂上去。
void cyber_prov_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free);
int cyber_prov_encrypt(uint8_t iv8, uint8_t *data, int len);
int cyber_prov_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t cyber_prov_checksum(uint8_t iv8, uint8_t *data, int len);
