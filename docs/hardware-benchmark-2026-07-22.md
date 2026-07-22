# VoiceStick 0.4.0 hardware benchmark — VS-C2A8

## Setup

- Firmware: 0.4.0 v2 transport build
- Mac: Apple Silicon, CoreBluetooth
- ASR: existing VoiceStick Cloud configuration
- ATT result: four-frame bundles were observed in normal traffic
- Audio: 16 kHz mono, 60 ms Opus frames, MTU-adaptive low-latency bitrate

## Stable baseline

The previous v1 build sent one 60 ms frame per notification. Two measured
sessions needed about 7.7 seconds and 15.7 seconds after physical release for
the final audio to reach the Mac. Cloud finalization itself took only about
0.3–0.4 seconds.

## v2 results

| Session | Physical duration | Audio frames | BLE notifications | Sequence gaps | ASR final after END | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 5 | 2.809 s | 43 | 12 incl. END | 0 | 0.383 s | recognized |
| 6 | 2.905 s | 45 | 13 incl. END | 0 | 0.394 s | recognized |
| Final 1 | 2.256 s | 34 | 10 incl. END | 0 | 0.343 s | recognized |
| Final 2 | 3.010 s | 46 | 13 incl. END | 0 | 0.507 s | recognized |

The data notifications carried an average of 3.9 and 3.75 Opus frames,
respectively. Audio END arrived before the delayed `button_up` state event in
all four normal sessions, so no post-release audio tail remained to hold ASR
open. The two `Final` rows were recorded after rebuilding the release app,
flashing the exact final firmware artifact, and reconnecting that app.

## First-session regression and fix

The first v2 run had one sequence gap while the app created its first ASR
WebSocket. The app originally acknowledged the BLE bundle only after forwarding
its frames into the coordinator. First-time network setup briefly delayed that
ACK, and the firmware's bounded window correctly stopped queuing but eventually
dropped a bundle.

The ACK is now written immediately after a bundle passes protocol validation,
before any ASR work. Five post-fix sessions had zero sequence gaps; all four
normal-length sessions produced recognized text. A 0.724-second control session
also had zero gaps but was intentionally too short to produce useful speech
after the start cue.

## Runtime feature checks

- Device reported firmware 0.4.0 after OTA and reconnected normally.
- Transport logs confirmed v2 on every post-upgrade session.
- Side-button runtime logs confirmed both the recent-paste Return path and the
  pending-text commit-with-Return path fired.
- Dice renderer, BMI270 driver, collision audio, and rainbow recording sources
  are byte-identical to the backed-up stable implementation.

Visual feel and audible collision quality still require the user's subjective
acceptance; these cannot be inferred from protocol logs.
