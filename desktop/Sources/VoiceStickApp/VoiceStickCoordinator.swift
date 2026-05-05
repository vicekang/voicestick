import AppKit
import Foundation

final class VoiceStickCoordinator {
    private enum PendingPasteState {
        case idle
        case waitingToPaste(text: String)
        case paused(text: String)

        var isIdle: Bool {
            if case .idle = self {
                return true
            }
            return false
        }
    }

    private let config: AppConfig
    private let statusController: StatusController
    private let ble: BleCentral
    private let asr: VolcengineASRClient
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    private let inputInjector = InputInjector()
    private let debugAudioRecorder: DebugAudioRecorder
    private let minimumRecordingDuration: TimeInterval = 0.5

    private var activeSessionID: UInt32?
    private var activeSessionStartedAt: Date?
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
    private var pendingPasteState = PendingPasteState.idle
    private var lastRecoverableText: String?
    private var pairedDeviceIDs: [String]

    init(config: AppConfig, statusController: StatusController) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = BleCentral(pairedDeviceIDs: config.pairedDeviceIDs)
        self.asr = VolcengineASRClient(config: config)
        self.debugAudioRecorder = DebugAudioRecorder(
            enabled: config.debugAudioCache,
            directory: config.debugAudioDirectory
        )
    }

    func start() {
        ble.onConnectionChange = { [weak self] connectedDevice in
            guard let self else { return }
            self.statusController.setConnectedDevice(connectedDevice)
            if connectedDevice != nil {
                self.statusController.setStatus("Connected")
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Scanning")
            }
        }

        ble.onStateEvent = { [weak self] event in
            self?.handleStateEvent(event)
        }

        ble.onAudioFrame = { [weak self] frame in
            self?.handleAudioFrame(frame)
        }

        asr.onPartial = { [weak self] text in
            self?.statusController.showPartial(text)
            self?.ble.sendControl(event: "partial", text: text)
        }

        asr.onFinal = { [weak self] text in
            self?.finishWithFinalText(text)
        }

        asr.onError = { [weak self] message in
            NSLog("ASR error: \(message)")
            self?.statusController.setStatus("ASR error: \(message)")
            self?.ble.sendControl(event: "error", text: message)
            self?.finishRecognitionCycle()
        }

        asr.onUpgradeURL = { url in
            DispatchQueue.main.async {
                let alert = NSAlert()
                alert.messageText = "ASR quota reached"
                alert.informativeText = "Your VoiceStick Cloud quota has been used. Upgrade your account to continue."
                alert.addButton(withTitle: "Upgrade")
                alert.addButton(withTitle: "Cancel")
                if alert.runModal() == .alertFirstButtonReturn {
                    NSWorkspace.shared.open(url)
                }
            }
        }

        ble.start()
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        statusController.setConnectedDevice(nil)
        statusController.setStatus(deviceIDs.isEmpty ? "Pair a VoiceStick" : "Scanning")
        ble.updatePairedDeviceIDs(deviceIDs)
    }

    func updateFirmware(from url: URL, progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        do {
            let image = try Data(contentsOf: url)
            ble.updateFirmware(image: image, progress: progress) { result in
                DispatchQueue.main.async {
                    completion(result)
                }
            }
        } catch {
            completion(.failure(error))
        }
    }

    func cancelFirmwareUpdate() {
        ble.cancelFirmwareUpdate()
    }

    private func handleStateEvent(_ event: StateEvent) {
        switch event.event {
        case "press_start":
            if handleFrontButtonDuringPendingPaste() {
                return
            }
            if isWaitingForFinalText {
                NSLog("Ignoring press_start while previous recording is finalizing")
                return
            }
            activeSessionID = event.sessionID
            activeSessionStartedAt = Date()
            receivedAudioFrames = 0
            bufferedOggChunks.removeAll(keepingCapacity: true)
            asrStarted = false
            sentFinalAudioChunk = false
            pastedFinalText = false
            pendingPasteState = .idle
            oggMuxer.reset()
            debugAudioRecorder.start(sessionID: event.sessionID)
            statusController.showListening()
        case "press_end":
            if activeSessionID == nil {
                return
            }
            let recordingDuration = currentRecordingDuration
            let isValidRecording = recordingDuration >= minimumRecordingDuration
            activeSessionID = nil
            activeSessionStartedAt = nil
            if !isValidRecording {
                cancelShortRecording()
            } else if receivedAudioFrames == 0 {
                let message = "No audio frames from device"
                NSLog("ASR error: \(message)")
                statusController.setStatus("ASR error: \(message)")
                statusController.hideOverlay()
                ble.sendControl(event: "error", text: message)
                debugAudioRecorder.discard()
            } else {
                sendFinalOggChunkIfNeeded(recordingDuration: recordingDuration)
            }
        case "cancel":
            cancelPendingPaste()
        default:
            break
        }
    }

    private func handleAudioFrame(_ frame: AudioFrame) {
        guard frame.sessionID == activeSessionID else { return }

        if frame.isEnd && frame.payload.isEmpty {
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }

        guard !frame.payload.isEmpty else { return }
        receivedAudioFrames += 1
        let oggChunk = oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        debugAudioRecorder.append(oggChunk)
        sendOrBufferOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: currentRecordingDuration >= minimumRecordingDuration
        )
        if frame.isEnd {
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            activeSessionID = nil
            activeSessionStartedAt = nil
            if !asrStarted && recordingDuration < minimumRecordingDuration {
                cancelShortRecording()
            } else if asrStarted {
                debugAudioRecorder.finish()
                statusController.setStatus("Finalizing")
            } else {
                debugAudioRecorder.finish()
                startASRAndFlushBufferedChunks(lastChunkIsFinal: true)
                statusController.setStatus("Finalizing")
            }
        }
    }

    private func sendFinalOggChunkIfNeeded(recordingDuration: TimeInterval) {
        guard !sentFinalAudioChunk else { return }
        sentFinalAudioChunk = true
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            activeSessionID = nil
            activeSessionStartedAt = nil
            cancelShortRecording()
            return
        }

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        activeSessionID = nil
        activeSessionStartedAt = nil
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
        statusController.setStatus("Finalizing")
    }

    private var currentRecordingDuration: TimeInterval {
        guard let activeSessionStartedAt else { return 0 }
        return Date().timeIntervalSince(activeSessionStartedAt)
    }

    private func sendOrBufferOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool) {
        if asrStarted {
            asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }

        bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        startASRAndFlushBufferedChunks(lastChunkIsFinal: isLast)
    }

    private func startASRAndFlushBufferedChunks(lastChunkIsFinal: Bool) {
        guard !asrStarted else { return }
        guard asr.start() else {
            bufferedOggChunks.removeAll(keepingCapacity: true)
            return
        }

        asrStarted = true
        for bufferedChunk in bufferedOggChunks.dropLast() {
            asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = bufferedOggChunks.last {
            asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        bufferedOggChunks.removeAll(keepingCapacity: true)
    }

    private func cancelShortRecording() {
        NSLog("Ignoring short recording under \(minimumRecordingDuration)s")
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asr.cancel()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        debugAudioRecorder.discard()
        statusController.setStatus("Connected")
        statusController.hideOverlay()
    }

    private func finishWithFinalText(_ text: String) {
        guard !pastedFinalText else { return }
        if text.isEmpty {
            pastedFinalText = true
            pendingPasteState = .idle
            statusController.showFinal(text) { [weak self] in
                self?.finishRecognitionCycle()
                self?.ble.sendControl(event: "paste_cancelled", text: "")
            }
            return
        }

        pastedFinalText = true
        lastRecoverableText = text
        statusController.setHasRecoverableInput(true)
        pendingPasteState = .waitingToPaste(text: text)
        statusController.showFinal(text) { [weak self] in
            self?.commitPendingPaste(text: text)
        }
        ble.sendControl(event: "final", text: text)
    }

    private func commitPendingPaste(text: String) {
        guard pendingPasteText == text else {
            return
        }

        completePendingPaste(text: text)
    }

    private func completePendingPaste(text: String) {
        pendingPasteState = .idle
        finishRecognitionCycle()
        inputInjector.paste(text: text, pressEnter: config.autoEnter)
        statusController.setStatus("Ready")
        ble.sendControl(event: "paste_done", text: "")
    }

    private var isWaitingForFinalText: Bool {
        activeSessionID == nil &&
            sentFinalAudioChunk &&
            asrStarted &&
            pendingPasteState.isIdle &&
            !pastedFinalText
    }

    private var pendingPasteText: String? {
        switch pendingPasteState {
        case .idle:
            return nil
        case .waitingToPaste(let text), .paused(let text):
            return text
        }
    }

    func restoreLastInputConfirmation() -> Bool {
        guard
            pendingPasteState.isIdle,
            activeSessionID == nil,
            !isWaitingForFinalText,
            let text = lastRecoverableText,
            !text.isEmpty
        else {
            return false
        }

        pendingPasteState = .paused(text: text)
        statusController.showPausedFinal(text)
        return true
    }

    private func handleFrontButtonDuringPendingPaste() -> Bool {
        switch pendingPasteState {
        case .idle:
            return false
        case .waitingToPaste(let text):
            pendingPasteState = .paused(text: text)
            statusController.showPausedFinal(text)
            return true
        case .paused(let text):
            statusController.hideOverlay { [weak self] in
                self?.commitPendingPaste(text: text)
            }
            return true
        }
    }

    private func cancelPendingPaste() {
        if pendingPasteText == nil {
            _ = restoreLastInputConfirmation()
            return
        }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        ble.sendControl(event: "paste_cancelled", text: "")
    }

    private func finishRecognitionCycle() {
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        bufferedOggChunks.removeAll(keepingCapacity: true)
    }
}
