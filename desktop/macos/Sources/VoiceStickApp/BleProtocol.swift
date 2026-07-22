import Foundation

struct AudioFrame {
    let sessionID: UInt32
    let seq: UInt32
    let flags: UInt8
    let payload: Data

    var isStart: Bool { flags & 0x01 != 0 }
    var isEnd: Bool { flags & 0x02 != 0 }
}

struct AudioBatch {
    let transportVersion: UInt8
    let sessionID: UInt32
    let firstSeq: UInt32
    let packetCount: UInt8
    let flags: UInt8
    let frames: [AudioFrame]

    var acknowledgedNextSeq: UInt32 {
        firstSeq &+ UInt32(packetCount)
    }
}

struct StateEvent: Decodable {
    let event: String
    let button: String?
    let sessionID: UInt32?
    let durationMs: UInt32?
    let hardware: String?
    let firmwareVersion: String?
    let buttons: [String]?
    let uiStates: [String]?

    enum CodingKeys: String, CodingKey {
        case event
        case button
        case sessionID = "session_id"
        case durationMs = "duration_ms"
        case hardware
        case firmwareVersion = "firmware_version"
        case buttons
        case uiStates = "ui_states"
    }
}

struct FirmwareOTAStateEvent: Decodable {
    let event: String
    let transferID: UInt32?
    let written: UInt32?
    let size: UInt32?
    let code: String?
    let espErr: Int?
    let rebootMs: Int?

    enum CodingKeys: String, CodingKey {
        case event
        case transferID = "transfer_id"
        case written
        case size
        case code
        case espErr = "esp_err"
        case rebootMs = "reboot_ms"
    }
}

enum BleProtocol {
    static let serviceUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5100"
    static let audioUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5101"
    static let stateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5102"
    static let controlUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5103"
    static let otaRXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5104"
    static let otaStateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5105"

    static let otaTypeBegin: UInt8 = 0x20
    static let otaTypeData: UInt8 = 0x21
    static let otaTypeEnd: UInt8 = 0x22
    static let otaTypeAbort: UInt8 = 0x23
    static let otaTypeState: UInt8 = 0x30
    static let audioType: UInt8 = 0x01
    static let audioAckType: UInt8 = 0x02

    static func parseAudioFrame(_ data: Data) -> AudioFrame? {
        guard let batch = parseAudioBatch(data), batch.frames.count == 1 else { return nil }
        return batch.frames[0]
    }

    static func parseAudioBatch(_ data: Data) -> AudioBatch? {
        guard data.count >= 16 else { return nil }
        let version = data[0]
        guard (version == 1 || version == 2), data[1] == audioType else { return nil }

        let headerLength = UInt16(littleEndianBytes: data[2..<4])
        guard headerLength == 16, data.count >= Int(headerLength) else { return nil }

        let sessionID = UInt32(littleEndianBytes: data[4..<8])
        let seq = UInt32(littleEndianBytes: data[8..<12])
        let flags = data[12]
        let packetCount = version == 2 ? data[13] : 1
        let payloadLength = Int(UInt16(littleEndianBytes: data[14..<16]))
        guard data.count == 16 + payloadLength else { return nil }

        if version == 1 {
            let legacyPacketCount: UInt8 = payloadLength == 0 && flags & 0x02 != 0 ? 0 : 1
            let frame = AudioFrame(
                sessionID: sessionID,
                seq: seq,
                flags: flags,
                payload: data.subdata(in: 16..<(16 + payloadLength))
            )
            return AudioBatch(
                transportVersion: version,
                sessionID: sessionID,
                firstSeq: seq,
                packetCount: legacyPacketCount,
                flags: flags,
                frames: [frame]
            )
        }

        if packetCount == 0 {
            guard payloadLength == 0, flags & 0x02 != 0 else { return nil }
            let endFrame = AudioFrame(sessionID: sessionID, seq: seq, flags: flags, payload: Data())
            return AudioBatch(
                transportVersion: version,
                sessionID: sessionID,
                firstSeq: seq,
                packetCount: 0,
                flags: flags,
                frames: [endFrame]
            )
        }

        var cursor = 16
        var frames: [AudioFrame] = []
        frames.reserveCapacity(Int(packetCount))
        for index in 0..<Int(packetCount) {
            guard cursor < data.count else { return nil }
            let packetLength = Int(data[cursor])
            cursor += 1
            guard packetLength > 0, cursor + packetLength <= data.count else { return nil }

            var frameFlags: UInt8 = 0
            if index == 0 { frameFlags |= flags & 0x01 }
            if index == Int(packetCount) - 1 { frameFlags |= flags & 0x02 }
            frames.append(AudioFrame(
                sessionID: sessionID,
                seq: seq &+ UInt32(index),
                flags: frameFlags,
                payload: data.subdata(in: cursor..<(cursor + packetLength))
            ))
            cursor += packetLength
        }
        guard cursor == data.count else { return nil }
        return AudioBatch(
            transportVersion: version,
            sessionID: sessionID,
            firstSeq: seq,
            packetCount: packetCount,
            flags: flags,
            frames: frames
        )
    }

    static func parseStateEvent(_ data: Data) -> StateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == 0x10 else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        return try? JSONDecoder().decode(StateEvent.self, from: payload)
    }

    static func parseFirmwareOTAStateEvent(_ data: Data) -> FirmwareOTAStateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == otaTypeState else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        return try? JSONDecoder().decode(FirmwareOTAStateEvent.self, from: payload)
    }

    static func uiStatePayload(state: String, text: String) -> Data {
        let payload = [
            "event": "ui_state",
            "state": state,
            "text": text
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    static func interactionModePayload(_ mode: String) -> Data {
        let payload = [
            "event": "interaction_mode",
            "mode": mode
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    static func audioTransportPayload(version: UInt8 = 2) -> Data {
        let payload: [String: Any] = [
            "event": "audio_transport",
            "version": version,
            "profile": "low_latency"
        ]
        return (try? JSONSerialization.data(withJSONObject: payload)) ?? Data()
    }

    static func audioAckPayload(sessionID: UInt32, nextSeq: UInt32) -> Data {
        var data = Data([2, audioAckType, 12, 0])
        data.appendLittleEndian(sessionID)
        data.appendLittleEndian(nextSeq)
        return data
    }

    static func otaBeginPayload(imageSize: UInt32, transferID: UInt32) -> Data {
        var data = Data([1, otaTypeBegin, 12, 0])
        data.appendLittleEndian(imageSize)
        data.appendLittleEndian(transferID)
        return data
    }

    static func otaDataPayload(transferID: UInt32, offset: UInt32, chunk: Data) -> Data {
        var data = Data([1, otaTypeData, 12, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(offset)
        data.append(chunk)
        return data
    }

    static func otaEndPayload(transferID: UInt32, imageSize: UInt32) -> Data {
        var data = Data([1, otaTypeEnd, 12, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(imageSize)
        return data
    }

    static func otaAbortPayload(transferID: UInt32) -> Data {
        var data = Data([1, otaTypeAbort, 8, 0])
        data.appendLittleEndian(transferID)
        return data
    }
}

private extension UInt16 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt16($1.element) << UInt16($1.offset * 8) }
    }
}

private extension UInt32 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt32($1.element) << UInt32($1.offset * 8) }
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt32) {
        append(UInt8(value & 0xff))
        append(UInt8((value >> 8) & 0xff))
        append(UInt8((value >> 16) & 0xff))
        append(UInt8((value >> 24) & 0xff))
    }
}
