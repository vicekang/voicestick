# VoiceStick refactor audit — 2026-07-22

## Reference and scope

- Upstream: `https://github.com/78/voicestick`
- Locked commit: `9f67f706de41bc99fa13100b2ebf7f7790a73fe2`
- Upstream repository version at audit time: `0.3.4`
- Refactor line: `codex/ble-audio-refactor`, version `0.4.0`
- Preserved local product behavior: physical dice, collision/start/end audio,
  rainbow recording state, cloud ASR, and side-button Return.

The installed stable app and the known-good firmware backup are not modified by
this work. The refactor is built in the isolated `voicestick-vnext` checkout.

## Findings

### P0 — notify success was treated as delivery

`ble_gatts_notify_custom` returning success only proved that NimBLE accepted a
notification. The original pipeline could enqueue 50 audio frames, then enqueue
many more notifications inside the Bluetooth stack. When macOS scheduled the
link slowly, the user could release the button while several seconds of audio
were still waiting locally.

Resolution: v2 cumulative ACK plus a two-bundle delivery window. Firmware now
uses proof of CoreBluetooth delivery instead of local enqueue success.

### P0 — one notification per 60 ms did not fit observed link capacity

The original 20 kbps CBR stream produced about 150 payload bytes and required
16.7 notifications per second. Real sessions on the target Mac were observed at
roughly 5–6 notifications per second, creating an ever-growing tail.

Resolution: ATT-MTU-aware bundling. At MTU 247 the low-latency profile normally
sends four 60 ms Opus packets per notification. The encoder bitrate is selected
from the actual per-packet budget, never below 7 kbps. Smaller MTUs choose a
smaller bundle; v1 is the final fallback.

### P1 — stop sentinel could overtake the last encoded frame

The controller task inserted an END sentinel immediately after clearing the
running flag. The capture task could already be inside a blocking codec read and
enqueue one more frame after that sentinel.

Resolution: the capture task now owns the sentinel and inserts it only after its
last possible encoded frame.

### P1 — connection interval requests were mistaken for guarantees

Requesting 15–30 ms does not force a macOS central to schedule notifications at
that rate. Keeping the fast interval helped neither the measured mean nor the
large outliers by itself.

Resolution: fast interval remains a hint, while throughput correctness comes
from fewer notifications and application-level backpressure.

### P1 — version sources disagreed

The repository reported 0.3.4, ESP-IDF built firmware as 0.3.2 from Git tags,
and the macOS plist still reported 0.2.2. This can corrupt update, downgrade, and
support decisions.

Resolution: one root `VERSION` drives the firmware build and desktop packaging,
the obsolete `firmware/version.txt` duplicate was removed, and the development
Mac plist is aligned to 0.4.0.

### P1 — protocol parsing had no regression tests

The Mac accepted truncated frames with trailing data, and there was no automated
compatibility proof for a new transport.

Resolution: exact-length validation, v1/v2 parser self-tests, compact ACK byte
tests, and pure-C firmware serializer tests.

### P2 — per-frame info logging added real-time work

The firmware logged heap data for every 60 ms packet at info level. This did not
cause the full delay, but it added avoidable scheduling and USB-log work.

Resolution: per-notification logs are debug level; one summary is emitted per
session with frames, notifications, queue drops, and transport version.

### P1 — local macOS packaging produced two false-success states

The upstream script enabled hardened runtime for an ad-hoc main executable and
embedded Sparkle framework. With no common Team ID, macOS library validation
refused to load the framework even though `codesign --verify` passed. The same
script also wrote the `sign_update` error message into a `.signature` file when
the private Sparkle key was absent.

Resolution: hardened runtime is used only with a real Developer ID identity;
ad-hoc local test builds omit it. A Sparkle signature file is created only when
an EdDSA signature was actually extracted. An unsigned local ZIP is explicitly
marked as non-publishable.

## Compatibility matrix

| Firmware | Mac app | Result |
| --- | --- | --- |
| old v1 | old v1 | unchanged |
| old v1 | new v2-capable | app accepts v1; firmware ignores negotiation |
| new v2-capable | old v1 | firmware defaults to v1 for that connection |
| new v2-capable | new v2-capable | bundled v2 plus cumulative ACK |

## Verification gates

1. Pure-C serializer tests pass with warnings treated as errors.
2. Swift protocol self-tests pass against the production parser source.
3. macOS Swift Package builds.
4. ESP-IDF firmware builds and reports version 0.4.0.
5. Before replacing the stable installation, hardware tests must show:
   - no sequence gaps or queue drops in three normal dictations;
   - release-to-audio-END below 1.5 seconds for all three;
   - acceptable Chinese recognition at the negotiated bitrate;
   - dice physics/collision audio and rainbow recording state unchanged;
   - side button sends Return only for VoiceStick-produced text.

If any gate fails, the device remains on the backed-up stable image.
