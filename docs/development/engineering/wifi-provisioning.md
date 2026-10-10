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

The project shrank buffers (no behavior change) to raise `free/largest` at the
`audio ready` stage from 54588/45056 to **63904/55296** (+9.3 KB / +10.2 KB):

| Location | Change | Note |
| --- | --- | --- |
| LVGL draw buffer | 40 → 28 lines | `BSP_LVGL_DRAW_BUFFER_LINES`, saves 5.8 KB |
| I2S DMA descriptors | 6 → 4 | saves ~3.8 KB |
| Wi-Fi static RX buffers | 10 → 4 | each ~1.6 KB, saves ~9.6 KB |
| Wi-Fi dynamic RX/TX | 32/32 → 16/16 | |
| LWIP TCP send/window | 5760 → 2880 B | |
| BT controller concurrent adv | `BT_CTRL_BLE_MAX_ACT` 6 → 2 | the application is single-connection |
| NimBLE pools | ACL/EVT/MSYS sized for a single connection | all are **single whole-buffer callocs**; too small is worse |
| Hardware AES | disabled (software AES) | the HW DMA path needs 16-byte-aligned buffers and fails when the heap is fragmented during provisioning |

After changing any of these, **re-verify that provisioning starts** (check that
`largest` is at least 4096 in the two heap log lines printed right before BLE
starts; see `cyber_prov.c`). Do not look only at `free`: provisioning needs a
**contiguous block**, and LVGL's malloc pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`) is
a static array, so shrinking it does not help provisioning at all.

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
