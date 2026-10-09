<p align="right">
  <a href="xiaozhi-conversation.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Xiaozhi Conversation Channel (WebSocket)

[Device activation](xiaozhi-activation.md) ends with the device holding a
`wss://` URI and a bearer token. This document specifies the layer above it: the
WebSocket conversation channel, from the TLS upgrade to the point where the
server closes the session.

It records the externally observable contract and, separately, what was measured
on the real board. Measurement is what keeps this file honest: three of the
facts below are not derivable from the protocol documentation and were found
only by reading a live session.

## Scope

In scope:

- the connection handshake: request headers and the `hello` exchange;
- the JSON control messages this firmware acts on;
- audio parameters, and why the two directions do not share a sample rate;
- the server's keepalive frame;
- the session state machine and the measured timeouts;
- the heap cost of holding a session open on this board.

Out of scope:

- activation and configuration handoff. See [device activation](xiaozhi-activation.md);
- Wi-Fi provisioning. See [Wi-Fi provisioning](wifi-provisioning.md);
- wake word, MCP tool calling, and binary protocol versions 2 and 3. This
  firmware implements protocol version 1 only, which sends raw Opus frames and
  relies on the WebSocket opcode to separate audio from text.

## Endpoint and identity

The URI and token come from NVS through `cyber_ota_get_ws_config()`, so they are
re-read on every connection attempt rather than captured at boot. A token that
rotates therefore does not require a reboot.

| Header | Value | Source |
| --- | --- | --- |
| `Authorization` | `Bearer <token>` | OTA configuration handoff |
| `Protocol-Version` | `1` | Matches `version` in the `hello` body |
| `Device-Id` | Station MAC, lowercase hex | `cyber_ota_get_device_mac()` |
| `Client-Id` | UUID generated once per device | `cyber_ota_get_client_id()` |

`Client-Id` lives in NVS and is therefore stable across reboots but changes when
NVS is erased. The server treats it as the software identity, so erasing NVS
creates a new device as far as the service is concerned.

## Handshake

1. Open the TLS connection and upgrade to WebSocket with the headers above.
2. Send the client `hello`.
3. Wait for a server `hello` containing `session_id`.

The client must accept a server `hello` for 10 seconds and then treat the
attempt as failed. Waiting longer leaves the UI in a state that never resolves.

Client `hello`, as built by `cyber_ws_build_hello()`:

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

Server `hello`, as observed:

```json
{
  "type": "hello",
  "transport": "websocket",
  "session_id": "64ad7344",
  "audio_params": {
    "format": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

The `session_id` must be echoed on every subsequent control message that carries
one, so a session id that arrives empty is a handshake failure rather than a
detail.

## Audio parameters: the two directions differ

The uplink is fixed at **16000 Hz, mono, 60 ms**, and the request above states
it. The downlink is not ours to choose: the server declares it in its `hello`
and the measured value is **24000 Hz**.

This is expected, not a negotiation failure. The upstream protocol document
states that the server may use 24000 Hz for downlink audio to get better music
quality, and that a device whose own rate differs resamples after decoding. A
device that assumes one rate for both directions will build a link that
handshakes cleanly and then plays silence or gibberish, which is the most
expensive class of bug on this board because nothing in the logs looks wrong.

Frame arithmetic, which the audio path must agree with:

| Direction | Rate | Frame | Samples per frame | PCM bytes per frame |
| --- | --- | --- | --- | --- |
| Uplink | 16000 Hz | 60 ms | 960 | 1920 |
| Downlink (measured) | 24000 Hz | 60 ms | 1440 | 2880 |

Per-frame sample count is `rate / 1000 * 60`; PCM bytes double it for 16-bit
mono. These values must be recomputed, not copied, if the server ever declares a
different frame duration.

## Server keepalive

The server sends an **empty text frame every ~10 seconds**. The protocol
documentation does not define it, which is why an implementation that parses
every text frame reports a decode failure ten times a minute.

It is not the documented application-level heartbeat. That mechanism is
`{"type": "ping"}` sent by the device, answered with `{"type": "pong"}`, and the
server has it disabled by default. Nothing needs to be sent in reply to the
empty frame, and the measured session survives seven consecutive unanswered
keepalives.

Correct handling: treat a zero-length text frame as a no-op.

## Session lifecycle

Server-driven states this firmware tracks, and the client messages it sends:

| Server message | Session state | Client response |
| --- | --- | --- |
| `hello` | `READY` | `listen` with `state: start` if the talk key was already pressed |
| `stt` | unchanged | none; text becomes the subtitle |
| `tts` with `state: start` | `SPEAKING` | none |
| `tts` with `state: sentence_start` | unchanged | none; text becomes the subtitle |
| `tts` with `state: stop` | `READY` | none |
| socket closed | `IDLE` | none; the next talk press reconnects |

Client messages: `listen` (`start` / `stop` / `detect` with `mode: manual`),
`abort` (with a reason such as `user_interrupt`).

The connection is opened lazily, on the first talk request, rather than when the
channel becomes available. Holding a session open costs about 8 KB of free heap
plus the TLS buffers and the server closes an idle session anyway, so an idle
connection buys nothing.

## Measured results

Board: ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3. Firmware `4e9dcb1` with
the `hello` diagnostics added. Credentials were a temporary test account
returned by the activation endpoint, and the endpoint was
`wss://api.tenclass.net/xiaozhi/v1/`.

| Measurement | Result |
| --- | --- |
| TLS handshake plus WebSocket upgrade | 4.5 s and 5.4 s in two runs |
| `hello` round trip | about 0.4 s |
| Talk key press to `READY` | 4.9 s and 5.8 s |
| Keepalive interval | 10.1 s |
| Server-declared audio parameters | `opus` / 24000 Hz / 1 channel / 60 ms |
| Session id | 8 hex characters, for example `64ad7344` |

The handshake is accepted with a temporary test token, and the server identifies
the device but warns that the conversation may be served by a shared default
agent. Any conclusion about reply quality from this credential is therefore not
trustworthy; the transport conclusion is.

Closing behaviour: with a `listen start` sent and no audio frames following, the
server closed the connection **65.8 seconds** later. Sending `listen stop`
instead kept the session open past 130 seconds. The server is therefore timing
the recording, and the audio path must start streaming well inside that window.

Heap, measured across the same run:

| Moment | Free | Largest block |
| --- | --- | --- |
| After Wi-Fi, before the channel | 126696 | 110592 |
| After the channel is armed, no connection | 123180 | 110592 |
| Dialling, before the TLS upgrade | 114224 | 69632 |
| During the session | 121560 | 69632 |
| Session plus a concurrent OTA check (lowest point) | 32716 | 65536 |
| After teardown | 120944 | 77824 |

The lowest point, 32716 bytes free, is what a firmware that overlaps a
conversation session with an OTA check must survive. It is the tightest figure
this project has measured so far and is the number to check against before
adding another concurrent TLS client.

## What is not verified

The transport, the handshake, the keepalive, and the state machine are verified
on hardware. The audio path is not implemented, so:

- no `stt` or `tts` message has been observed end to end, and the subtitle
  rendering has only been exercised by the host tests;
- the server returned nothing at all for a `listen` cycle with zero bytes of
  audio, so an empty recording is not a usable substitute for a real one;
- the 24000 Hz downlink rate is recorded from the server's declaration. It has
  not been confirmed against a received audio frame, because nothing decodes
  frames yet.

## References

- [Xiaozhi WebSocket protocol](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket_zh.md)
  (upstream, Chinese): handshake, message types, and the downlink sample rate note.
