#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/voicestick-transport-tests.XXXXXX")"
trap 'rm -rf -- "$TEST_TMP_DIR"' EXIT

clang -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT_DIR/firmware/components/audio_transport/include" \
    "$ROOT_DIR/firmware/components/audio_transport/audio_transport.c" \
    "$ROOT_DIR/firmware/components/audio_transport/test/test_audio_transport.c" \
    -o "$TEST_TMP_DIR/audio_transport_test"
"$TEST_TMP_DIR/audio_transport_test"

if command -v swiftc >/dev/null 2>&1; then
    MACOS_DIR="$ROOT_DIR/desktop/macos"
    swiftc \
        "$MACOS_DIR/Sources/VoiceStickApp/BleProtocol.swift" \
        "$MACOS_DIR/Tests/ProtocolSelfTest/main.swift" \
        -o "$TEST_TMP_DIR/protocol_selftest"
    "$TEST_TMP_DIR/protocol_selftest"

    swiftc \
        "$MACOS_DIR/Sources/VoiceStickApp/SideButtonSendState.swift" \
        "$MACOS_DIR/Tests/SideButtonSendStateSelfTest/main.swift" \
        -o "$TEST_TMP_DIR/side_button_selftest"
    "$TEST_TMP_DIR/side_button_selftest"
else
    echo "swiftc unavailable; skipped macOS parser and side-button self-tests"
fi
