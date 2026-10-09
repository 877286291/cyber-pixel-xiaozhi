<p align="right">
  <a href="xiaozhi-activation.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Xiaozhi Service Activation and Configuration Handoff

Joining Wi-Fi is only the first of two layers required by the official
xiaozhi.me service. This document specifies the second layer: **device
activation** (binding this device to a user account) and the **configuration
handoff** the device must complete before it can open a conversation channel.

This is a specification, not an implementation guide. It records the externally
observable contract and the constraints that contract imposes on this board, so
that an implementation can be written against facts rather than assumptions.

## Scope

In scope:

- the OTA check and activate request/response contract;
- the activation state machine and what the user must be shown;
- the configuration the device must persist;
- the constraints this imposes on an ESP32-C3 board with no PSRAM.

Out of scope:

- Wi-Fi provisioning, which is the first layer. See
  [Wi-Fi provisioning](wifi-provisioning.md);
- the conversation protocol itself (WebSocket audio frames and JSON control messages)
  once configuration has been obtained. See the
  [Xiaozhi conversation channel](xiaozhi-conversation.md);
- self-hosted server deployments, whose activation rules differ.

## Why activation is required

The configuration endpoint returns the conversation settings (the WebSocket URI
and its token, or the MQTT credentials) **only after the device is bound to an
account**. An unbound device instead receives an `activation` object containing a
six-digit code. Until a user enters that code in the xiaozhi.me console, the
device cannot obtain a conversation endpoint, so a successful Wi-Fi association
alone is not sufficient to talk to the service.

Binding is stored server-side and persists across reboots, so the code is entered
once per device. It must be re-done if the device identity changes, meaning if
NVS is erased or the MAC address changes.

## Endpoint and identity

| Item | Value |
| --- | --- |
| Default endpoint | `https://api.tenclass.net/xiaozhi/ota/` |
| Endpoint override | NVS namespace `wifi`, key `ota_url`, falling back to the build default |
| Activate endpoint | The check endpoint with `activate` appended (insert a `/` when the check URL does not already end in one) |
| Transport | HTTPS for the check and activate calls; the conversation channel uses `wss://` |

The endpoint must remain configurable at runtime rather than compiled in, because
the service host and the deployment kind both change over time.

Identity headers sent on every request:

| Header | Value |
| --- | --- |
| `Device-Id` | The device MAC address |
| `Client-Id` | A per-device UUID that the firmware generates once and persists |
| `Activation-Version` | `"2"` when an eFuse serial number is present, otherwise `"1"` |
| `Serial-Number` | The eFuse user-data serial, sent **only** when `Activation-Version` is `"2"` |
| `User-Agent` | Board name and firmware version |
| `Accept-Language` | Device language preference, for example `zh-CN` |
| `Content-Type` | `application/json` |

### This board uses activation version 1

Activation version 2 binds the request to hardware by signing the server
challenge with the ESP32 HMAC peripheral and an eFuse key, using the serial
number stored in the eFuse user-data block. That requires factory eFuse
programming.

This board has **no programmed eFuse serial number**, so the firmware must send
`Activation-Version: 1`, omit `Serial-Number`, and never perform the HMAC
challenge step. Version 1 activation is therefore the simpler path: display the
code, wait for the user to bind, and re-check until configuration arrives.

The gate for the HMAC step is **having a serial number, not receiving a
challenge**. The live service returns an `activation.challenge` even to this
board (see the verification section below), while the reference implementation
builds its HMAC payload entirely from the eFuse serial number and sends `{}`
without one. Attempting the step on this board would therefore POST an empty
payload and achieve nothing, so it must be skipped deliberately rather than
triggered by the presence of a challenge.

## Check request contract

The reference firmware sends `POST` when it has a body and `GET` otherwise; with
a populated system-info body the call is a `POST`.

```json
{
  "flash_size": 4194304,
  "minimum_free_heap_size": 150000,
  "mac_address": "aa:bb:cc:dd:ee:ff",
  "chip_model_name": "esp32c3",
  "chip_info": { "model": 5, "cores": 1, "revision": 3, "features": 0 },
  "application": { "name": "cyber-pixel-xiaozhi", "version": "0.1.0", "idf_version": "v5.5.3" },
  "partition_table": [],
  "ota": { "label": "factory" },
  "board": { "type": "cyber-pixel-xiaozhi", "mac": "aa:bb:cc:dd:ee:ff" }
}
```

> [!IMPORTANT]
> The `Device-Id` header and the MAC inside the body must agree. A mismatch is
> rejected by servers implementing this contract with an `Invalid OTA request`
> error, which is easy to misread as a Wi-Fi or TLS problem. Derive both from one
> source so they cannot drift.

## Check response contract

Every field is optional; the firmware must tolerate any subset.

| Top-level object | Meaning | Handling |
| --- | --- | --- |
| `activation` | The device is not yet bound | Show `message` and `code`; see the state machine below |
| `websocket` | Conversation endpoint | Persist `url` and `token`; the device needs this to talk |
| `mqtt` | Alternative conversation transport | Persist if desired; not required when WebSocket is used |
| `server_time` | Server clock | Set the system time from `timestamp` and `timezone_offset` |
| `firmware` | Firmware upgrade offer | **Not applicable to this board; see the constraints below** |

The `activation` object itself:

| Field | Meaning |
| --- | --- |
| `message` | Human-readable instruction to show the user |
| `code` | The six-digit code the user enters in the console |
| `challenge` | Server nonce for the HMAC flow. The server returns it even to a version 1 device, so **its presence is not the signal to run the HMAC step**; see below |
| `timeout_ms` | Server-side validity window for the code, default 30000 |

When `websocket` is present and `activation` is absent, the device is bound and
configured, and may open the conversation channel.

## Activation state machine

1. **Check.** `POST` to the check endpoint with the identity headers and body.
2. **Branch on the response.**
   - `websocket` present, `activation` absent: bound. Persist configuration and
     finish. This is the normal steady state on every later boot.
   - `activation.code` present: unbound. Continue to step 3.
   - Neither present: the device most likely received test credentials instead of
     a binding code. Treat as a distinct failure; see the risks below.
3. **Display.** Render `activation.message` and `activation.code`. The code must be
   large and unambiguous, because the user retypes it into a web console.
4. **Wait and re-check.** Re-run the check on a bounded interval until step 2
   reports bound, or until the code expires. `timeout_ms` is the server's window,
   not a client polling interval; the client picks its own interval and must cap
   the total wait rather than polling forever.
5. **Skip the activate call on this board.** The trigger for the HMAC step is an
   eFuse serial number, not a `challenge`, so this board never posts to the
   activate endpoint. For completeness, a serial-numbered device would `POST` the
   HMAC payload and interpret the status codes below.

Activate status codes, for completeness:

| Status | Meaning |
| --- | --- |
| `200` | Activation accepted |
| `202` | Still waiting for the user to enter the code; keep re-checking |
| other | Failure; log the body, which carries the server reason |

### Polling policy as implemented

Step 4 leaves the poll interval and the failure handling to the client. This build
chooses them in `main/cyber_ota_util.h` (pure policy, covered by a host test) and
`main/cyber_ota.c` (the worker that applies it):

| Situation | Behavior | Why |
| --- | --- | --- |
| Waiting for the user to bind | Successive checks are spaced 2 s, 4 s, 6 s, then a steady 8 s | The user is standing at the device retyping the code; faster only burns power and traffic, slower makes a completed binding feel unresponsive |
| Bound, but the service still returns temporary credentials | Re-check every 60 s | The device is usable and proceeds to the main screen. Re-checking sooner than the 30 min ready interval picks up a credential switch, or an unbind or rebind done in the console. Deliberately **not** counted as a network failure |
| Ready | Re-check every 30 min | The token can rotate; asking more often is pointless |
| Per-request timeout | 10 s | Bounds how long a stalled TLS session can hold the worker |
| Consecutive failures | After 5, publish `ERROR` and stop polling until the link drops and comes back | An endless retry loop on a device that cannot reach the service only drains the battery |
| Link down | Return to `IDLE` and drop the displayed code | Otherwise a code that is no longer valid stays on screen |

The interval is derived from a **saturating** counter rather than a wrapping one.
A plain `uint8_t` counter wraps after 255 polls, which would make the computed
delay collapse to zero and silently turn the poll loop into a busy loop.

> [!IMPORTANT]
> **The total wait cap required by step 4 is not implemented.** While the device is
> online and unbound it polls indefinitely at the 8 s interval. The rationale is
> that the code is keyed to the MAC address and has no client-side expiry, and that
> an unbound device normally sits on a charger while the user completes setup. The
> cost is one TLS session roughly every 8 s for as long as the device is left
> unbound, which is real power draw and real traffic against the service. Capping
> the fast interval and falling back to a slow re-check is the intended resolution.
> It is recorded here as **outstanding** rather than quietly dropped.

### Activation screen strings are firmware-owned

The screen does not render the server's `activation.message`. It shows the code in
a dedicated 48 px pixel font, a short state label, and one fixed hint
(`ENTER CODE AT XIAOZHI.ME`, or `WAITING FOR SERVER` before a code arrives). The
state label is one of `LINKING`, `WAIT FOR BIND`, `READY`, `TEST ACCOUNT`,
`NO LINK`, or `LINK ERROR`.

Two consequences are worth recording. The display does not depend on the server's
localization, so no Chinese glyphs are needed on this screen and the project's
default fonts remain sufficient. And the code font is intentionally a **subset** —
space, `-`, `:`, and the digits only — because printable ASCII at 48 px would be
roughly 140 KB of C source against about 11 KB for the subset. Any other character
renders as nothing at all, silently, so that font must not be used for prose.

## Verified against the live service

The contract above was confirmed by sending one real check request to the default
endpoint, using this board's MAC address and a freshly generated `Client-Id` so
that the request represented a device that had never contacted the service. No
binding was created; the request only asked for configuration.

The response contained all of the following at once:

| Observed | Interpretation |
| --- | --- |
| `activation.code` with a six-digit value | The service **does** offer a binding code to this board's `board.type`. The activation screen is a viable feature. |
| `activation.message` of the form `xiaozhi.me\n<code>` | The server composes the instruction text and repeats the code inside it. Rendering `message` alone already contains the code. |
| `activation.challenge` present | Confirms the challenge is not gated on `Activation-Version`; see the correction above. |
| `websocket.token` equal to `test-token`, and `mqtt.client_id` containing `GID_test` | These are the **pre-binding** credentials. An unbound device is given a working temporary account so it functions out of the box. |
| `firmware.url` empty | No upgrade offered, consistent with ignoring the `firmware` object. |

The key clarification: `GID_test` and `test-token` in the **unbound** state are
normal and expected. They only indicate a problem if they persist **after** the
user has bound the device, which is the situation reported in the field.

### Reproduced by the firmware over HTTPS

The same contract was then exercised by the firmware itself, over HTTPS with the
bundled root certificates. Three facts emerged that the desktop probe could not
show:

- **Certificate validation succeeds with no clock.** The certificate bundle
  reported the certificate as validated, confirming that the absent time/date
  support is not a problem in practice, as described above.
- **The activation code is keyed to the MAC address, not to `Client-Id`.** A code
  obtained from a desktop request carrying a freshly generated `Client-Id`, and the
  code later obtained by the firmware carrying a different `Client-Id`, were
  identical, and the code stayed constant across repeated polls. Persisting
  `Client-Id` is therefore about session identity, not about which code is issued.
- **An unbound device receives a binding code *and* temporary WebSocket
  credentials in the same response.** This contradicts the intuitive rule "if a
  WebSocket endpoint is present, the device is bound". The implemented rule is
  therefore: a present `activation.code` means activation is required regardless
  of any WebSocket object; a WebSocket object appearing **without** a code counts
  as bound.

The second half of that rule was then checked directly against the live service,
by sending check requests from a desktop for several MAC addresses:

| MAC | `activation` returned | Notes |
| --- | --- | --- |
| this board, already bound | no | only `websocket` with `test-token`, and an `mqtt.client_id` containing `GID_test` |
| a MAC the service had never seen | yes | a six-digit `code`, plus the same temporary credentials |
| another unseen MAC | yes | same shape |

The service offers a code to devices it has not seen, and stops offering it once a
device is bound. "Not asked to activate" therefore means "bound", and there is no
response that is both silent about activation and genuinely unbound. Board type is
not a factor: repeating the probe with this board's custom `board.type` and with
official ones (`bread-compact-wifi`, `esp-box-3`) changes nothing.

A bound device can still be handed temporary credentials. That is
[xiaozhi-esp32#1785](https://github.com/78/xiaozhi-esp32/issues/1785), observed
directly on this board: after the code was entered in the console and the console
listed the device as bound, every later check still returned `test-token` /
`GID_test` with no `activation`. Treating that as "not ready" pins the device on
the activation screen forever, because the real token never arrives, so the
firmware proceeds to the main screen and records the temporary-account condition
in the log instead.

An early implementation got this wrong by checking the WebSocket object first, and
reported ready while sitting on a temporary account, never showing the code. The
device exposed the bug immediately; the desktop probe, which only inspects a single
response, could not have. That failure mode stays prevented, because
`activation.code` is still checked first — only the *blocking* treatment of
temporary credentials was dropped.

Resource figures measured on this board (ESP32-C3, no PSRAM), with Wi-Fi, the
display, and LVGL already running:

| Moment | Free heap | Largest contiguous block |
| --- | --- | --- |
| Before the TLS request | ~129 KB | 112 KB |
| Peak during the TLS session | ~82 KB (low-water mark) | 88 KB |
| After the client is released | ~132 KB | 88 KB |

A TLS session therefore costs roughly 44 KB at peak, and the heap returns to its
prior level once the client is released. The 88 KB contiguous block that remains is
the figure that matters for the audio pipeline to be added later.

Over the observed run the post-release free heap stayed within a few hundred bytes
of the previous cycle, which is consistent with allocation noise rather than a
leak, but a longer soak is still required to rule out a slow drift. That check is
recorded as outstanding rather than assumed.

## Configuration to persist

| Namespace | Content |
| --- | --- |
| A device-identity namespace | The generated `Client-Id` UUID, so it survives reboot |
| A conversation namespace | `websocket.url`, `websocket.token`, and any further `websocket` fields the server adds |

Persist every string field of the returned `websocket` and `mqtt` objects rather
than a hardcoded subset, because the server is free to add keys without notice.
Writing configuration only after a successful check keeps a failed boot from
overwriting a working configuration with nothing.

## Board-specific constraints

These are consequences of this repository's hardware and structure, and are the
reason the specification matters more than the reference implementation.

### Do not enable the firmware upgrade path

This repository's partition table deliberately contains only NVS, PHY data, and
one `factory` application. There is no second application partition, so
`esp_ota_get_next_update_partition()` returns `NULL` and the reference firmware's
upgrade routine cannot run. The firmware must ignore the `firmware` object rather
than act on it. Reacting to it would either fail or, if the partition table were
changed carelessly, risk overwriting the running image. See
[firmware layout](firmware-layout.md).

### Budget TLS and JSON as one system cost

Activation adds an HTTPS client and a JSON parser to a firmware that already runs
a display, a decoder, and the Wi-Fi stack. On a chip with no PSRAM the failure
mode is running out of one large contiguous block while total free heap still
looks healthy. Size the JSON parse capacity from the accepted response limit
rather than the request size, and reject an oversized response before parsing.
The measurement discipline in
[Network audio streaming and memory budgeting](../../reference/phoenixzhc/network-audio-streaming-and-memory.md)
applies unchanged to the activation client.

### Clock and certificate validation interact, but not in this build

Validating a server certificate normally requires a correct clock, and the only
clock source available before the first successful check is the server itself. On
this build the problem does not arise: ESP-IDF compiles mbedTLS **without**
time/date support by default (`CONFIG_MBEDTLS_HAVE_TIME_DATE` is not set), so
certificate validity dates are never examined. Chain and signature validation
remain enabled, so the certificate is still checked against the bundled root set.

The constraint becomes real the moment that option is enabled, for example to
enforce certificate expiry. Record the decision rather than rediscover it as an
unexplained TLS failure on a freshly erased device.

### Keep network work off the LVGL task

The check and activate calls block and must run in a worker task. LVGL is not
thread-safe, so any screen update driven by the result must be marshalled onto
the LVGL task under the existing lock. The activation screen is a new screen and
must own its lifecycle: nothing may left running that can touch it after it is
deleted.

### The activation screen is new UI

The six-digit code needs a large, legible rendering. This is a new screen in the
application's own visual language, not a reuse of a demo screen. Verify on-device
that the chosen font actually contains digit glyphs at the size used, following
[Chinese fonts and missing glyphs](lvgl-chinese-fonts.md).

## Known risks and failure modes

| Symptom | Likely cause | Action |
| --- | --- | --- |
| No `activation` code ever appears, and an unbind is requested | The device is bound to another account | Request unbinding through xiaozhi.me, which is a manual step measured in business days |
| Binding appears to succeed but traffic still runs on a shared default agent | `GID_test` / `test-token` persisted **after** binding | Observed on this board; it is [#1785](https://github.com/78/xiaozhi-esp32/issues/1785), a service-side defect. The firmware logs the condition and proceeds to the main screen rather than pinning itself on the activation page. Correcting the account requires an unbind or rebind through xiaozhi.me |
| `Invalid OTA request` | `Device-Id` and the body MAC disagree | Derive both from one source |
| TLS handshake fails on a freshly erased device | Only possible if `CONFIG_MBEDTLS_HAVE_TIME_DATE` is enabled and no clock is available | Keep the default (time/date disabled), or add a time source before the first call |
| Configuration never arrives although the user entered the code | The check is not being repeated after binding, or the code expired | Confirm the re-check loop and the wait cap |
| Re-activation demanded after a firmware update | NVS was erased, or the stored `Client-Id` was regenerated | Persist `Client-Id`; do not regenerate it per boot |

## Open questions to settle before implementation

Recorded deliberately rather than guessed, because they change the design:

1. **Custom board acceptance — answered, with one caveat.** The service returns an
   `activation.code` for this board's `board.type`; the code was entered in the
   console and the console listed the device as bound, so the custom board type is
   accepted end to end. The caveat is that after that real binding the device still
   received temporary `test-token` credentials on every subsequent check
   ([#1785](https://github.com/78/xiaozhi-esp32/issues/1785)), so an
   account-specific `websocket.token` was never observed. That is a service-side
   defect, not a rejection of the board type.
2. **Client polling cadence and total wait cap — partially resolved.** The cadence
   is implemented, host-tested, and recorded in "Polling policy as implemented"
   above. The total wait cap required by step 4 is **still outstanding**; see the
   note in that section.
3. **Certificate and clock bootstrap — resolved.** The build does not examine
   certificate validity dates, because ESP-IDF omits time/date support by default,
   so no bootstrap clock is needed. Revisit only if that option is enabled.
4. **Language of the activation screen — resolved.** The screen renders
   firmware-owned strings rather than the server's localized `message`, so the
   display does not depend on the server's language and no Chinese glyphs are
   required. See "Activation screen strings are firmware-owned" above.

## References

- A one-off check request against the default endpoint, reproduced during this
  specification's preparation, is recorded in the verification section above.
- Reference implementation, check and activate logic:
  [`main/ota.cc`](https://github.com/78/xiaozhi-esp32/blob/main/main/ota.cc) and
  [`main/ota.h`](https://github.com/78/xiaozhi-esp32/blob/main/main/ota.h)
- Official protocol specification referenced from that source:
  <https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd>
- Identity mismatch reported against a compatible server:
  [xiaozhi-esp32-server#1619](https://github.com/xinnan-tech/xiaozhi-esp32-server/issues/1619)
- Temporary test credentials that persisted after binding:
  [xiaozhi-esp32#1785](https://github.com/78/xiaozhi-esp32/issues/1785)
- Device already bound to another account:
  [xiaozhi-esp32#1471](https://github.com/78/xiaozhi-esp32/issues/1471)
