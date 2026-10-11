<p align="right">
  <strong>简体中文</strong> · <a href="wifi-provisioning.md">English</a>
</p>

# 通过蓝牙配网连接 Wi-Fi

设备通过 BLE 上的 BLUFI 协议接收 Wi-Fi 名称（SSID）与口令，存进 NVS，随后以 STA
模式连接。手机侧使用微信小程序 **蓝牙配网-FoloToy AI PASSPORT**。

## 本应用的实现

实现位于 `main/cyber_prov*.c`，与 `cyber_net` 的分工是硬边界：

| 模块 | 职责 |
| --- | --- |
| `cyber_prov_util` | 纯逻辑：配网阶段推导、凭证累积与校验（宿主可测） |
| `cyber_prov_security` | BLUFI 安全协商（PSA Crypto：FFDH + SHA-256 + AES-CTR） |
| `cyber_prov` | BLE + BLUFI 协议；Wi-Fi 与凭证持久化全部委托给 `cyber_net` |
| `cyber_net` | Wi-Fi 驱动与 NVS 凭证的唯一归属；新增 `cyber_net_prov_*` 供配网借用 |

**Wi-Fi 驱动与默认 netif 在 ESP-IDF 里是全局单例**，所以配网不自建 Wi-Fi 栈，而是
通过 `cyber_net_prov_begin/scan/commit/end` 借用它：`begin` 起栈并抑制自动重连，
`scan` 借用同一套栈扫描 AP，`commit` 校验后写 NVS 并连接。

### 入口与退出

- **进入**：联网错误页（`CYBER_SCREEN_NET_ERROR`）短按 OK。该页在"无凭证"或
  "连不上"时出现，正是需要配网的场景。
- **退出**：配网页长按 OK；或**配网成功后自动退出**（连上热点即拆掉 BLE，见下）。

### 关键约束与取舍

- **BLE 只在配网期间存活。** `cyber_prov_stop()` 会完整拆除 NimBLE 并归还 RAM。
  这是无 PSRAM 的 C3 上的必要取舍：常驻 BLE 会与 Wi-Fi 共存抢内存，而本应用同时
  还要跑 Opus 编解码与 LVGL。
- **加密方案必须跟随手机端，不能跟随示例。** 实测"蓝牙配网-FoloToy AI PASSPORT"
  小程序用的是【经典 BLUFI 协议】：**1024 位自由 DH 参数 + MD5 派生 PSK +
  AES-128-CFB128**（每包 IV = 全零、首字节替换为协议给的 iv8）。这与 ESP-IDF 6.1
  自带示例的 RFC7919 3072 位 + SHA-256 + AES-CTR **线上不兼容**，两者只能二选一。
  另外 IDF 6.1 的 mbedtls 4.x 已移除 `mbedtls_dhm/aes/md5` 的公开头文件，因此实现
  改用：DH → `mbedtls_mpi`（`bignum.h` 是 wrapper，完整 API 可用，且走 RSA 硬件
  加速）；MD5 / CFB → PSA。详见 `main/cyber_prov_security.c` 顶部说明。
- **只用 STA**：设备不做 AP，BLUFI 的 AP 模式请求一律忽略。

### 内存预算（本板无 PSRAM，这是配网能否跑起来的关键）

BLE 协议栈初始化约需 **45 KB**，其中 NimBLE host 任务要一块 **4 KB 连续栈**。
后者有个隐蔽陷阱：`esp_nimble_enable()` 创建该任务后**无条件返回 `ESP_OK`**，
`xTaskCreatePinnedToCore` 失败会被静默吞掉——日志照常打印"配网已启动"，但主机
从未运行、从不广播，表现为**手机搜不到设备**（实测 `largest` 仅 3072 < 4096 时
正是如此）。因此起 BLE 前必须留足连续内存块。

为此项目在**不改功能**的前提下回收内存，有两个手段：**配网期间释放音频通路**
（`cyber_audio_suspend()` 停掉音频 worker（栈 28 KB）并调 `bsp_audio_deinit()` 释放
I2S DMA（约 11.5 KB），配网结束再 `cyber_audio_resume()` 恢复，实测 `largest` 由
43008 提升到 **69632**，`btc_init` 后仍有 38912），以及**收窄 Wi-Fi 接收侧缓冲**
（见下）。`BSP_LVGL_DRAW_BUFFER_LINES` 40 → 28 行（省 5.8 KB）同样保留。

**只收窄 Wi-Fi 的【接收侧】缓冲，绝不碰发送侧。** 这一区分是实测得来的，写错会
破坏语音：

- 收窄**发送侧**（`ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` 32→16、LWIP TCP 发送缓冲
  5760→2880 B）会让每帧 Opus 的发送阻塞更久；上行队列只有 4 帧深，于是队列溢出、
  服务端收不到完整语音并挂断。实测上行丢帧率由 4% 升到 83%、下行收不到任何帧。
- 收窄**接收侧**（`ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→4、
  `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 32→16）是安全的，且在 Wi-Fi init 时（堆还很整、
  尚未碎片化）就少占内存，对后续的连续分配帮助最大。

为什么这关系重大：BLE 使能后堆更紧，**Opus 编解码器各需约 20 KB 连续内存**
（见 `cyber_audio.c`）。接收侧削减实测让 `audio ready` 的最大连续块从 40960 提升到
**53248**（+12.3 KB），`btc_init` 后仍有 49152；若不做这一步，抓音时只剩约 12.8 KB，
编码器打不开 → 采不到音 → 服务端不回复 → 界面**卡在"思考"**（这就是"卡 thinking"
的真实原因，与协议无关）。

**发送侧一律保持 IDF 默认**：`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM`、
`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`、`CONFIG_LWIP_TCP_WND_DEFAULT` 不要动。

调整任何内存项后**必须重新核对配网能启动**（`cyber_prov.c` 里 `btc_init` 前后的
两行堆日志中 `largest` 需 ≥ 4096）。不要只看 `free`：配网要的是**连续块**，而
LVGL 的 malloc 池（`CONFIG_LV_MEM_SIZE_KILOBYTES`）是静态数组，缩减它对配网毫无帮助。

## 参考代码

上游参考位于
[`demo/blufi-provisioning` 分支](https://github.com/FoloToy/ai-passport/tree/demo/blufi-provisioning)，
但**其 `demo_blufi_security.c` 无法在 IDF 6.1 下编译**（依赖已被移除的 mbedtls 头文件）。
安全协商请以 IDF 6.1 自带示例
（`$IDF_PATH/examples/bluetooth/blufi/main/blufi_security.c`）为准。

<a id="mini-program-name"></a>

## 小程序名称

配套小程序的准确名称为：**蓝牙配网-FoloToy AI PASSPORT**。

请使用上述完整名称，不要自行翻译或改写。这是小程序名称，不是设备的蓝牙广播名；
本应用的广播名为 `BLUFI_FoloPassport`（`CYBER_PROV_DEVICE_NAME`）。小程序只列出以
`BLUFI` 开头的设备，改广播名会导致手机扫不到。

预期流程：设备进入配网状态，手机打开蓝牙并授予所需权限，打开该小程序，选择目标
设备，提供 2.4 GHz Wi-Fi 的名称和密码。确认设备取得 IP 地址后，再验证应用实际需要
的网络请求；仅蓝牙连接成功不能证明能够访问互联网。

## 接入与验收

从应用当前基线出发，仅移植适用的联网逻辑。不要直接合并整个 demo，也不要用该分支的
旧版 BSP、分区表或配置覆盖当前版本。

配网入口、退出、超时、重试上限、凭据持久化及主动清除凭据的方式已在上面说明；
`cyber_net_clear_credentials()` 提供清除途径。禁止记录或提交 Wi-Fi 密码。

运行[验证门禁](build-and-test.zh_CN.md)，再遵循
[真机测试交接流程](../ai-guide.zh_CN.md#主动询问真机测试)。使用上述小程序验证：
发现设备、成功配网、密码错误与网络不可用、重连与重启、清除凭据、反复进入退出配网，
以及应用实际需要的网络请求。编译成功不能证明小程序兼容性或实机联网正常；
未执行的检查列入 `Unverified`。
