<p align="right">
  <a href="wifi-provisioning.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Wi-Fi Connection via Bluetooth Provisioning

The device receives the Wi-Fi SSID and password over BLE using BLUFI, stores
them in NVS, and then connects as a Wi-Fi station. The phone-side client is the
WeChat mini program named in the
[Chinese document](wifi-provisioning.zh_CN.md#mini-program-name).

## Implementation in this application

The code lives in `main/cyber_prov*.c`, with a hard boundary against `cyber_net`:

| Module | Responsibility |
| --- | --- |
| `cyber_prov_util` | Pure logic: provisioning-phase derivation, credential accumulation and validation (host-tested) |
| `cyber_prov_security` | BLUFI security negotiation (classic protocol: DH + MD5 + AES-CFB) |
| `cyber_prov` | BLE + BLUFI protocol; delegates all Wi-Fi and credential persistence to `cyber_net` |
| `cyber_net` | Sole owner of the Wi-Fi driver and NVS credentials; exposes `cyber_net_prov_*` for provisioning to borrow |

The Wi-Fi driver and default netif are **global singletons** in ESP-IDF, so
provisioning does not create its own Wi-Fi stack. It borrows the existing one
via `cyber_net_prov_begin/scan/commit/end`: `begin` starts the stack and
suppresses auto-reconnect, `scan` reuses the same stack for AP scanning, and
`commit` validates, persists to NVS, and connects.

### Entry and exit

- **Enter**: short-press OK on the network error screen
  (`CYBER_SCREEN_NET_ERROR`), which appears exactly when provisioning is needed.
- **Exit**: long-press OK on the provisioning screen, or **automatically after
  success** (the BLE stack is torn down as soon as the AP is joined).

### Key constraints and trade-offs

- **BLE lives only during provisioning.** `cyber_prov_stop()` fully tears down
  NimBLE and returns its RAM. With no PSRAM on the C3 this is required: a
  resident BLE stack would fight Wi-Fi coexistence for internal RAM while the
  application also runs Opus and LVGL.
- **The crypto scheme must follow the phone, not the example.** The mini program
  uses the **classic BLUFI protocol**: a **1024-bit free DH group + MD5-derived
  PSK + AES-128-CFB128** (per-packet IV = all zero with the first byte replaced
  by the iv8 from the protocol). This is **wire-incompatible** with the
  ESP-IDF 6.1 example (RFC7919 3072-bit + SHA-256 + AES-CTR); pick exactly one.
  Also, IDF 6.1's mbedtls 4.x removed the public `mbedtls_dhm/aes/md5` headers,
  so the implementation uses `mbedtls_mpi` for DH (`bignum.h` is a wrapper that
  exposes the full API and uses the RSA hardware accelerator) and PSA for MD5
  and CFB. See the header comment of `main/cyber_prov_security.c`.
- **STA only**: the device does not act as an AP; BLUFI AP-mode requests are
  ignored.

### Memory budget (no PSRAM — this decides whether provisioning can start)

BLE stack init needs about **45 KB**, including a **4 KB contiguous stack** for
the NimBLE host task. That task hides a trap: `esp_nimble_enable()` returns
`ESP_OK` unconditionally after calling `xTaskCreatePinnedToCore`, so a failed
task creation is swallowed — the log still prints "provisioning started" while
the host never runs and never advertises, which shows up as **the phone not
finding the device** (observed when `largest` was 3072 < 4096). A contiguous
block must therefore be available before BLE starts.

The project reclaims memory **without changing behavior**. The mechanisms are to
**release the audio path during provisioning** — `cyber_audio_suspend()` stops the
audio worker (28 KB stack) and calls `bsp_audio_deinit()` to free the I2S DMA
(~11.5 KB); `cyber_audio_resume()` brings it back afterwards, so `largest` rises
from 43008 to **69632** (+26.6 KB), leaving 38912 after `btc_init` — and to trim
the Wi-Fi **receive** side (see below). The `BSP_LVGL_DRAW_BUFFER_LINES` change
(40 → 28 lines, 5.8 KB) is kept as well: it has independent value.

**Shrink only the Wi-Fi *receive*-side buffers; never the transmit side.** This
distinction is empirical and getting it wrong breaks voice:

- Shrinking the **transmit** side (`ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` 32→16, LWIP TCP
  send buffer 5760→2880 B) makes each Opus frame block longer while sending; the
  4-deep uplink queue then overflows and the server hangs up. Measured: the uplink
  drop rate went from 4% to 83% and no downlink frames arrived.
- Shrinking the **receive** side (`ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→4,
  `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 32→16) is safe, and it saves memory at Wi-Fi
  init time while the heap is still clean, which helps later contiguous allocations
  the most.

Why this matters: with BLE enabled the heap is tighter, and the **Opus codecs each
need about 20 KB of contiguous memory** (see `cyber_audio.c`). The receive-side trim
raises the largest free block at the `audio ready` stage from 40960 to **53248**
(+12.3 KB), still 49152 after `btc_init`. Without it, only about 12.8 KB is left at
capture time, the encoder cannot open, no audio is captured, the server never
replies, and the UI **sticks on "thinking"** — that is the real cause of the stuck
thinking symptom, and it is not protocol related.

**Keep the transmit side at the IDF defaults**: do not change
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM`, `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, or
`CONFIG_LWIP_TCP_WND_DEFAULT`.

After changing any memory item, **re-verify that provisioning starts** (the
`largest` in the two heap lines around `btc_init` in `cyber_prov.c` must be at
least 4096). Do not look only at `free`: provisioning needs a **contiguous
block**, and LVGL's malloc pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`) is a static
array, so shrinking it does not help provisioning at all.

Kept trims that are BLE-only or otherwise safe: `BT_CTRL_BLE_MAX_ACT` 6 → 2, the
NimBLE pools sized for a single connection (all are single whole-buffer callocs,
so too small is worse), and hardware AES disabled in favor of software AES (the
hardware DMA path needs 16-byte-aligned buffers that the fragmented heap cannot
provide during provisioning).

## Reference code

The upstream reference lives in the
[`demo/blufi-provisioning` branch](https://github.com/FoloToy/ai-passport/tree/demo/blufi-provisioning),
but **its `demo_blufi_security.c` does not compile under IDF 6.1** (it depends
on removed mbedtls headers). For the API surface, follow the IDF 6.1 in-tree
example (`$IDF_PATH/examples/bluetooth/blufi/main/blufi_security.c`), but note
that its crypto scheme differs from what the mini program expects (see above).

## Mini program name

Use the companion mini program identified by the
[exact Chinese search name](wifi-provisioning.zh_CN.md#mini-program-name).

This is the mini program's name, not the device's BLE advertising name. This
application advertises as `BLUFI_FoloPassport` (`CYBER_PROV_DEVICE_NAME`). The
mini program only lists devices whose name starts with `BLUFI`, so renaming it
makes the device undiscoverable.

The intended flow is to open provisioning on the device, enable Bluetooth and
the required permissions on the phone, open the mini program, select the target
device, and provide a 2.4 GHz Wi-Fi network's credentials. Confirm that the
device obtains an IP address, then test the application's actual network
request; a successful BLE connection alone does not prove Internet access.

## Integration and acceptance

Start from the application's current baseline and adapt only the relevant
networking logic. Do not merge the entire demo or overwrite current BSP,
partitions, or configuration with the branch's older versions.

Provisioning entry/exit, timeouts, retry limits, credential persistence, and
`cyber_net_clear_credentials()` for explicit clearing are described above.
Never log or commit Wi-Fi passwords.

Run the [validation gate](build-and-test.md), then follow the
[on-device testing handoff](../ai-guide.md#offer-on-device-testing). Test with
the named mini program: discovery, successful provisioning, wrong-password and
unavailable-network failures, reconnect/restart behavior, credential clearing,
repeated entry/exit, and an actual application network request. Build success
does not prove mini program compatibility or on-device networking; keep
unperformed checks under `Unverified`.
