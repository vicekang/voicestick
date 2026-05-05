import Foundation

struct AudioFrame {
    let sessionID: UInt32
    let seq: UInt32
    let flags: UInt8
    let payload: Data

    var isStart: Bool { flags & 0x01 != 0 }
    var isEnd: Bool { flags & 0x02 != 0 }
}

struct StateEvent: Decodable {
    let event: String
    let sessionID: UInt32?

    enum CodingKeys: String, CodingKey {
        case event
        case sessionID = "session_id"
    }
}

enum BleProtocol {
    static let serviceUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5100"
    static let audioUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5101"
    static let stateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5102"
    static let controlUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5103"

    static func parseAudioFrame(_ data: Data) -> AudioFrame? {
        guard data.count >= 16 else { return nil }
        guard data[0] == 1, data[1] == 0x01 else { return nil }

        let headerLength = UInt16(littleEndianBytes: data[2..<4])
        guard headerLength == 16, data.count >= Int(headerLength) else { return nil }

        let sessionID = UInt32(littleEndianBytes: data[4..<8])
        let seq = UInt32(littleEndianBytes: data[8..<12])
        let flags = data[12]
        let payloadLength = Int(UInt16(littleEndianBytes: data[14..<16]))
        guard data.count >= 16 + payloadLength else { return nil }

        return AudioFrame(
            sessionID: sessionID,
            seq: seq,
            flags: flags,
            payload: data.subdata(in: 16..<(16 + payloadLength))
        )
    }

    static func parseStateEvent(_ data: Data) -> StateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == 0x10 else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        return try? JSONDecoder().decode(StateEvent.self, from: payload)
    }

    static func controlPayload(event: String, text: String) -> Data {
        let json = #"{"event":"\#(event)","text":"\#(text.jsonEscaped)"}"#
        return Data(json.utf8)
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

private extension String {
    var jsonEscaped: String {
        replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
    }
}
