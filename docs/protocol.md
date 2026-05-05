# Voice Stick Protocol

This document describes the protocol implemented by the current firmware and macOS desktop app.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> Mac | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> Mac | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | Mac -> StickS3 | write without response |

The desktop app scans for this service and only connects to devices whose `VS-XXXX` ID is present in the local paired-device list.

## Audio Frame

All multibyte fields are little-endian.

```text
struct AudioBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x01 audio
  uint16_t header_len;    // 16
  uint32_t session_id;
  uint32_t seq;
  uint8_t  flags;         // bit0=start, bit1=end
  uint8_t  reserved;      // currently 0
  uint16_t payload_len;
  uint8_t  payload[payload_len];
}
```

The payload contains one raw Opus packet when `payload_len > 0`. The firmware currently encodes 60 ms of 16 kHz mono audio per packet. When recording stops, the firmware also sends an end frame with `flags & 0x02` and an empty payload.

The macOS app wraps incoming Opus packets into an Ogg Opus stream before sending them to ASR. It does not decode Opus to PCM.

## State Event

All multibyte fields are little-endian.

```text
struct StateBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x10 state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

Currently emitted state events:

```json
{"event":"press_start","session_id":1234}
{"event":"press_end","session_id":1234}
{"event":"cancel"}
```

`press_start` and `press_end` bracket a push-to-talk session. `cancel` is sent by the side button only outside active recording. The macOS app uses `cancel` to cancel pending text, or to restore the last input confirmation when no text is pending.

## Control Event

The Mac writes compact JSON to `control_rx`. The current firmware logs the payload and does not otherwise act on it.

Current desktop events:

```json
{"event":"connected","text":""}
{"event":"partial","text":"hello"}
{"event":"final","text":"hello world"}
{"event":"paste_done","text":""}
{"event":"paste_cancelled","text":""}
{"event":"error","text":"ASR timeout"}
```

The desktop helper always includes a `text` field, even for events without text content.

## Runtime State Machine

StickS3:

```text
boot -> advertising -> connected -> idle -> recording -> idle
```

The firmware also dims the display after 30 seconds of idle time. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes the device from deep sleep.

macOS:

```text
needs_pairing -> scanning -> connected -> listening -> finalizing -> countdown -> paste_done
```

During the final-text countdown, pressing the front button pauses auto-paste. A second front-button press confirms paste. A side-button `cancel` cancels pending text. When idle, side-button `cancel` restores the last recoverable input confirmation.

Recordings shorter than 0.5 seconds are discarded locally and are not sent to ASR.

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared.

Volcengine endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

VoiceStick Cloud default endpoint:

```text
wss://api.voicestick.app/v1/asr
```

The first request payload currently sent by the desktop app is:

```json
{
  "user": {"uid": "voice-stick-local"},
  "audio": {
    "format": "ogg",
    "codec": "opus",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_nonstream": true,
    "show_utterances": false,
    "enable_ddc": true
  }
}
```

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On button release, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors should use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message should be JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.
