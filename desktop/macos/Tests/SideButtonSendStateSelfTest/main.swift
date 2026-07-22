import Foundation

private var failures = 0
private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() {
        failures += 1
        fputs("FAIL: \(message)\n", stderr)
    }
}

let first = UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
let second = UUID(uuidString: "00000000-0000-0000-0000-000000000002")!
let now = Date(timeIntervalSince1970: 1000)

var state = SideButtonSendState(recentPasteWindow: 30)
state.queueSend(for: first)
expect(!state.consumeQueuedSend(for: second), "queued send is device scoped")
expect(state.consumeQueuedSend(for: first), "queued send is consumed once")
expect(!state.consumeQueuedSend(for: first), "queued send cannot repeat")

state.recordPaste(peripheralID: first, enterWasSent: false, at: now)
expect(state.consumeRecentUnsentPaste(for: first, at: now.addingTimeInterval(5)),
       "recent unsent VoiceStick paste can be sent")
expect(!state.consumeRecentUnsentPaste(for: first, at: now.addingTimeInterval(6)),
       "recent paste is consumed once")

state.recordPaste(peripheralID: first, enterWasSent: false, at: now)
expect(!state.consumeRecentUnsentPaste(for: first, at: now.addingTimeInterval(31)),
       "expired paste cannot send Return")

state.recordPaste(peripheralID: first, enterWasSent: true, at: now)
expect(!state.consumeRecentUnsentPaste(for: first, at: now),
       "already-sent paste does not arm side button")

if failures > 0 { exit(1) }
print("SideButtonSendState self-tests passed")
