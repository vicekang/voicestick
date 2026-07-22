import Foundation

struct SideButtonSendState {
    private struct UnsentPaste {
        let peripheralID: UUID
        let completedAt: Date
    }

    private var queuedPeripheralID: UUID?
    private var recentUnsentPaste: UnsentPaste?
    private let recentPasteWindow: TimeInterval

    init(recentPasteWindow: TimeInterval = 30) {
        self.recentPasteWindow = recentPasteWindow
    }

    mutating func queueSend(for peripheralID: UUID) {
        queuedPeripheralID = peripheralID
    }

    mutating func consumeQueuedSend(for peripheralID: UUID?) -> Bool {
        guard let peripheralID, queuedPeripheralID == peripheralID else { return false }
        queuedPeripheralID = nil
        return true
    }

    mutating func recordPaste(peripheralID: UUID?, enterWasSent: Bool,
                              at completedAt: Date = Date()) {
        guard !enterWasSent, let peripheralID else {
            recentUnsentPaste = nil
            return
        }
        recentUnsentPaste = UnsentPaste(peripheralID: peripheralID, completedAt: completedAt)
    }

    mutating func consumeRecentUnsentPaste(for peripheralID: UUID,
                                           at now: Date = Date()) -> Bool {
        guard let recentUnsentPaste else { return false }
        self.recentUnsentPaste = nil
        let age = now.timeIntervalSince(recentUnsentPaste.completedAt)
        return recentUnsentPaste.peripheralID == peripheralID &&
            age >= 0 && age <= recentPasteWindow
    }

    mutating func clearQueuedSend() {
        queuedPeripheralID = nil
    }

    mutating func resetForNewRecording() {
        queuedPeripheralID = nil
        recentUnsentPaste = nil
    }
}
