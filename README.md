# Voice Stick

Voice Stick turns an M5Stack StickS3 into a Bluetooth push-to-talk input device for macOS.

Hold the front button on the StickS3 to record. When you release it, the macOS menu bar app sends the audio to ASR, shows the recognized text, and pastes the final result into the currently focused input field after a short confirmation countdown. By default it only pastes text and does not press Return; `auto_enter` can be enabled in settings.

## Project Layout

- `firmware/`: ESP-IDF firmware for M5Stack StickS3 / ESP32-S3.
- `desktop/`: Swift Package for the native macOS menu bar app.
- `docs/protocol.md`: BLE protocol between StickS3 and macOS.
- `docs/volcengine-asr.md`: trimmed Volcengine ASR notes used by the desktop client.
- `scripts/`: sprite slicing, palette tuning, and LVGL ARGB binary conversion helpers.

## Current Features

- StickS3 advertises as `VS-XXXX`, where `XXXX` is derived from the last two bytes of the eFuse MAC.
- The macOS app only connects to paired `VS-XXXX` devices. If no device is paired, the menu bar app can open a pairing window to scan nearby VoiceStick devices.
- The front button starts a recording session on press and ends it on release.
- The firmware reads 16 kHz mono PCM from the ES8311 microphone, encodes it as Opus, and sends it over BLE notifications.
- The macOS app wraps incoming Opus payloads into Ogg Opus and forwards them to ASR over WebSocket.
- ASR providers can be direct Volcengine or VoiceStick Cloud relay.
- During recognition, the macOS app shows a floating overlay and menu bar status. Final text enters a 1.2 second confirmation countdown.
- Pressing the front button during the countdown pauses auto-paste. Pressing the front button again confirms paste; pressing the side button cancels it.
- Pressing the side button while idle restores the last recoverable input confirmation.
- Optional debug audio cache saves each valid recognition session as Ogg Opus.
- The firmware screen shows pairing, ready, listening, error, and battery states. It dims after 30 seconds of inactivity. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes it from deep sleep.

## Hardware Target

- Board: M5Stack StickS3 / ESP32-S3-PICO-1-N8R8
- Front button: GPIO11, push-to-talk and deep-sleep wake
- Side button: GPIO12, cancel or restore the last input confirmation
- PMIC IRQ: GPIO13
- Audio codec: ES8311 over I2S, 16 kHz / 16 bit / mono
- Display: 135 x 240 ST7789P3 portrait screen
- LCD backlight: GPIO38 PWM

Main pin definitions live in `firmware/components/stick_s3_board/include/stick_s3_board.h`.

## Interaction Model

| State | Front button | Side button |
| --- | --- | --- |
| Unpaired / disconnected | No recording; screen shows `VS-XXXX` | No effective action |
| Connected idle | Hold to record | Restore last input confirmation |
| Recording | Release to finish recording | Does not cancel the active recording |
| Finalizing | New recording is ignored | No effective action |
| Final text countdown | Pause auto-paste and show confirmation | Cancel pending text |
| Paused confirmation | Confirm paste | Cancel pending text |

By default the paste flow does not press Return. Enable `Press Return after paste` in settings, or set `auto_enter = true` in the config file, to send Return after paste.

## Audio Path

```text
StickS3 mic -> ES8311/I2S PCM -> Opus -> BLE -> macOS -> Ogg Opus -> ASR -> paste
```

The desktop app does not decode Opus back to PCM for ASR. It forwards Ogg Opus directly.

## BLE Protocol Summary

GATT service:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> Mac | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> Mac | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | Mac -> StickS3 | write without response |

See `docs/protocol.md` for the full frame format.

## Firmware Build

Prepare ESP-IDF. The commands below use the local path `~/esp/v5.5.1/esp-idf`; replace it if your ESP-IDF checkout lives elsewhere.

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
```

If `export.sh` reports that the ESP-IDF Python virtual environment is missing, run the matching installer once:

```sh
"$HOME/esp/v5.5.1/esp-idf/install.sh" esp32s3
```

Flash and monitor:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Firmware dependencies are declared through the ESP-IDF component manager:

- `espressif/button`
- `espressif/esp_codec_dev`
- `78/esp-opus`
- `lvgl/lvgl`

## macOS Desktop Build

The desktop app is a Swift Package targeting macOS 12 or newer.

```sh
cd desktop
swift build
```

Run it:

```sh
swift run VoiceStickApp
```

The app is a menu bar accessory app and requests Bluetooth permission. Text insertion uses simulated `Command-V` plus optional Return. If macOS blocks the keyboard events, grant Accessibility permission to the running terminal or app in System Settings.

For a distributable macOS release with Sparkle updates:

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

The build script writes `build/VoiceStick-<version>.app`, `build/VoiceStick-<version>.zip`, and a Sparkle signature file. Upload the DMG and ZIP to GitHub Releases, then update `website/appcast.xml` for the GitHub Pages update feed.

GitHub Actions can do the release path automatically when a `v<version>` tag is pushed. The tag must match `VERSION`, for example `VERSION=0.1.0` pairs with `v0.1.0`. The release workflow publishes the GitHub Release assets and deploys the website/appcast to GitHub Pages.

## Local Config

Config path:

```text
~/Library/Application Support/VoiceStick/config.toml
```

Create it from the example:

```sh
mkdir -p "$HOME/Library/Application Support/VoiceStick"
cp desktop/Config/config.example.toml "$HOME/Library/Application Support/VoiceStick/config.toml"
```

Example:

```toml
asr_provider = "volcengine"
voicestick_api_key = ""
voicestick_cloud_url = "wss://api.voicestick.app/v1/asr"
volcengine_api_key = "your_volcengine_asr_api_key"
resource_id = "volc.bigasr.sauc.duration"
paired_device_ids = ""
auto_enter = false
debug_audio_cache = false
# debug_audio_dir = "~/Library/Application Support/VoiceStick/DebugAudio"
```

Fields:

| Field | Description |
| --- | --- |
| `asr_provider` | `volcengine` or `voicestick_cloud` |
| `volcengine_api_key` | Direct Volcengine API key, sent as `X-Api-Key` |
| `voicestick_api_key` | VoiceStick Cloud relay API key, sent as `X-Api-Key` |
| `voicestick_cloud_url` | Cloud relay WebSocket URL |
| `resource_id` | Volcengine resource ID |
| `paired_device_ids` | Comma-separated 4-digit hex IDs, for example `C3D8,09AF` |
| `auto_enter` | Whether to press Return after paste |
| `debug_audio_cache` | Whether to save debug Ogg Opus files |
| `debug_audio_dir` | Debug audio output directory |

Supported Volcengine `resource_id` values:

- `volc.seedasr.sauc.duration`
- `volc.seedasr.sauc.concurrent`
- `volc.bigasr.sauc.duration`
- `volc.bigasr.sauc.concurrent`

Do not commit API keys.

## Pairing Flow

1. Flash and boot the StickS3. The screen shows `VS-XXXX`.
2. Start the macOS desktop app.
3. Open the pairing window from the menu bar app, or use `Settings... -> Pair...`.
4. Select the matching `VS-XXXX` in the scan list and click `Pair`.
5. After saving, the desktop app scans for and connects to that device.

You can also edit `paired_device_ids` manually. When multiple IDs are saved, the desktop app ignores nearby unpaired VoiceStick devices.

## Debug Audio

Enable:

```toml
debug_audio_cache = true
```

Default output directory:

```text
~/Library/Application Support/VoiceStick/DebugAudio
```

Each valid recognition session is saved as a playable Ogg Opus file. Recordings shorter than 0.5 seconds are discarded by the desktop app and are not sent to ASR.
