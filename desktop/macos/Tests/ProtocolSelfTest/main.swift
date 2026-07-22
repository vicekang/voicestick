import Foundation

private var failures = 0

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() {
        failures += 1
        fputs("FAIL: \(message)\n", stderr)
    }
}

private func makeHeader(version: UInt8, sessionID: UInt32, seq: UInt32,
                        flags: UInt8, packetCount: UInt8, payload: Data) -> Data {
    func littleEndian(_ value: UInt32) -> [UInt8] {
        [
            UInt8(value & 0xff), UInt8((value >> 8) & 0xff),
            UInt8((value >> 16) & 0xff), UInt8((value >> 24) & 0xff)
        ]
    }
    var data = Data([version, 0x01, 16, 0])
    data.append(contentsOf: littleEndian(sessionID))
    data.append(contentsOf: littleEndian(seq))
    data.append(flags)
    data.append(packetCount)
    data.append(UInt8(payload.count & 0xff))
    data.append(UInt8((payload.count >> 8) & 0xff))
    data.append(payload)
    return data
}

let legacy = BleProtocol.parseAudioBatch(makeHeader(
    version: 1, sessionID: 0x11223344, seq: 7, flags: 1,
    packetCount: 0, payload: Data([0xaa, 0xbb])
))
expect(legacy?.transportVersion == 1, "v1 transport version")
expect(legacy?.frames.first?.payload == Data([0xaa, 0xbb]), "v1 payload")
expect(legacy?.frames.first?.isStart == true, "v1 start flag")

let legacyEnd = BleProtocol.parseAudioBatch(makeHeader(
    version: 1, sessionID: 0x11223344, seq: 8, flags: 2,
    packetCount: 0, payload: Data()
))
expect(legacyEnd?.packetCount == 0, "v1 end is not counted as audio")
expect(legacyEnd?.frames.first?.isEnd == true, "v1 end marker")

let bundled = BleProtocol.parseAudioBatch(makeHeader(
    version: 2, sessionID: 9, seq: 40, flags: 1,
    packetCount: 2, payload: Data([2, 0xaa, 0xbb, 1, 0xcc])
))
expect(bundled?.packetCount == 2, "v2 packet count")
expect(bundled?.acknowledgedNextSeq == 42, "v2 cumulative ack")
expect(bundled?.frames.map(\.seq) == [40, 41], "v2 sequence expansion")
expect(bundled?.frames.map(\.payload) == [Data([0xaa, 0xbb]), Data([0xcc])],
       "v2 payload expansion")

let end = BleProtocol.parseAudioBatch(makeHeader(
    version: 2, sessionID: 3, seq: 22, flags: 2,
    packetCount: 0, payload: Data()
))
expect(end?.packetCount == 0, "v2 end has no Opus packets")
expect(end?.frames.first?.isEnd == true, "v2 end marker")
expect(end?.acknowledgedNextSeq == 22, "v2 end ack does not invent a frame")

let malformed = makeHeader(
    version: 2, sessionID: 1, seq: 0, flags: 0,
    packetCount: 1, payload: Data([3, 0xaa])
)
expect(BleProtocol.parseAudioBatch(malformed) == nil, "reject truncated v2 packet")

var trailing = makeHeader(
    version: 1, sessionID: 1, seq: 0, flags: 0,
    packetCount: 0, payload: Data([0xaa])
)
trailing.append(0xbb)
expect(BleProtocol.parseAudioBatch(trailing) == nil, "reject trailing bytes")

expect(
    BleProtocol.audioAckPayload(sessionID: 0x11223344, nextSeq: 0x55667788) ==
    Data([2, 2, 12, 0, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55]),
    "compact audio ack bytes"
)

if failures > 0 {
    exit(1)
}
print("BleProtocol self-tests passed")
