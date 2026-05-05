import AppKit

final class StatusController {
    private enum AppStatus {
        case needsPairing
        case scanning
        case connected
        case listening
        case transcribing
        case finalizing
        case ready
        case paused
        case error

        init(text: String) {
            let normalized = text.lowercased()
            if normalized.contains("pair") {
                self = .needsPairing
            } else if normalized.contains("scan") || normalized.contains("match") {
                self = .scanning
            } else if normalized.contains("connect") {
                self = .connected
            } else if normalized.contains("listen") {
                self = .listening
            } else if normalized.contains("final") {
                self = .finalizing
            } else if normalized.contains("ready") || normalized.contains("no speech") {
                self = .ready
            } else if normalized.contains("pause") {
                self = .paused
            } else if normalized.contains("error") || normalized.contains("failed") {
                self = .error
            } else {
                self = .transcribing
            }
        }

        var symbolName: String {
            switch self {
            case .needsPairing:
                return "dot.radiowaves.left.and.right"
            case .scanning:
                return "antenna.radiowaves.left.and.right"
            case .connected:
                return "link.circle.fill"
            case .listening:
                return "mic.fill"
            case .transcribing:
                return "waveform"
            case .finalizing:
                return "hourglass"
            case .ready:
                return "link.circle.fill"
            case .paused:
                return "pause.circle"
            case .error:
                return "exclamationmark.triangle"
            }
        }

        var accessibilityDescription: String {
            switch self {
            case .needsPairing:
                return "Pair VoiceStick"
            case .scanning:
                return "Matching"
            case .connected:
                return "Connected"
            case .listening:
                return "Listening"
            case .transcribing:
                return "Transcribing"
            case .finalizing:
                return "Finalizing"
            case .ready:
                return "Ready"
            case .paused:
                return "Paused"
            case .error:
                return "Error"
            }
        }

        var visibleTitle: String? {
            switch self {
            case .needsPairing:
                return "Pair"
            case .scanning:
                return "Matching"
            case .transcribing:
                return "Transcribing"
            case .finalizing:
                return "Finalizing"
            case .error:
                return "Error"
            case .connected, .listening, .ready, .paused:
                return nil
            }
        }
    }

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let overlay = OverlayController()

    var onQuit: (() -> Void)?
    var onOpenSettings: (() -> Void)?
    var onPairDevice: (() -> Void)?
    var onForgetDevice: ((String) -> Void)?
    var onRestoreLastInput: (() -> Bool)?
    private var needsPairing: Bool
    private var hasRecoverableInput = false
    private var connectedDevice: ConnectedVoiceStickDevice?

    init(needsPairing: Bool = false) {
        self.needsPairing = needsPairing
        updateStatusButton(.ready)
        rebuildMenu()
    }

    func setNeedsPairing(_ needsPairing: Bool) {
        guard self.needsPairing != needsPairing else { return }
        self.needsPairing = needsPairing
        rebuildMenu()
    }

    func setConnectedDevice(_ device: ConnectedVoiceStickDevice?) {
        guard connectedDevice?.deviceID != device?.deviceID ||
                connectedDevice?.name != device?.name else { return }
        connectedDevice = device
        rebuildMenu()
    }

    func setHasRecoverableInput(_ hasRecoverableInput: Bool) {
        guard self.hasRecoverableInput != hasRecoverableInput else { return }
        self.hasRecoverableInput = hasRecoverableInput
        rebuildMenu()
    }

    private func rebuildMenu() {
        menu.removeAllItems()
        if hasRecoverableInput {
            menu.addItem(makeMenuItem(
                title: "恢复上一次输入",
                symbolName: "arrow.uturn.backward",
                action: #selector(restoreLastInput)
            ))
            menu.addItem(NSMenuItem.separator())
        }

        if let connectedDevice {
            let deviceItem = makeMenuItem(
                title: connectedDevice.name,
                symbolName: "link.circle.fill",
                action: nil
            )
            let submenu = NSMenu()
            let forgetItem = makeMenuItem(
                title: "忘掉这个设备",
                symbolName: "xmark.circle",
                action: #selector(forgetConnectedDevice)
            )
            forgetItem.representedObject = connectedDevice.deviceID
            submenu.addItem(forgetItem)
            deviceItem.submenu = submenu
            menu.addItem(deviceItem)

            menu.addItem(makeMenuItem(
                title: "Settings...",
                symbolName: "gearshape",
                action: #selector(openSettings),
                keyEquivalent: ","
            ))
        } else if needsPairing {
            menu.addItem(makeMenuItem(
                title: "匹配设备...",
                symbolName: "dot.radiowaves.left.and.right",
                action: #selector(pairDevice)
            ))

            menu.addItem(makeMenuItem(
                title: "Settings...",
                symbolName: "gearshape",
                action: #selector(openSettings),
                keyEquivalent: ","
            ))
        } else {
            menu.addItem(makeMenuItem(
                title: "Settings...",
                symbolName: "gearshape",
                action: #selector(openSettings),
                keyEquivalent: ","
            ))
        }

        menu.addItem(NSMenuItem.separator())

        menu.addItem(makeMenuItem(
            title: "Quit",
            symbolName: "power",
            action: #selector(quitApp),
            keyEquivalent: "q"
        ))

        statusItem.menu = menu
    }

    func setStatus(_ text: String) {
        DispatchQueue.main.async {
            self.updateStatusButton(AppStatus(text: text))
        }
    }

    func showListening() {
        setStatus("Listening")
        overlay.showListening(text: "")
    }

    func showPartial(_ text: String) {
        setStatus(text.isEmpty ? "Listening" : text)
        overlay.showListening(text: text)
    }

    func showFinal(_ text: String, onHidden: (() -> Void)? = nil) {
        setStatus(text.isEmpty ? "No speech" : "Ready")
        overlay.showFinal(text: text, onHidden: onHidden)
    }

    func showPausedFinal(_ text: String) {
        overlay.showPaused(text: text)
    }

    func hideOverlay(onHidden: (() -> Void)? = nil) {
        overlay.hide(onHidden: onHidden)
    }

    private func updateStatusButton(_ status: AppStatus) {
        guard let button = statusItem.button else { return }
        button.image = Self.symbolImage(
            named: status.symbolName,
            accessibilityDescription: status.accessibilityDescription
        )
        button.title = status.visibleTitle ?? ""
        button.imagePosition = status.visibleTitle == nil ? .imageOnly : .imageLeading
        button.toolTip = "VoiceStick: \(status.accessibilityDescription)"
        button.setAccessibilityLabel("VoiceStick: \(status.accessibilityDescription)")
    }

    private func makeMenuItem(
        title: String,
        symbolName: String,
        action: Selector?,
        keyEquivalent: String = ""
    ) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: keyEquivalent)
        item.target = self
        item.image = Self.symbolImage(named: symbolName, accessibilityDescription: title)
        return item
    }

    private static func symbolImage(named name: String, accessibilityDescription: String) -> NSImage? {
        let configuration = NSImage.SymbolConfiguration(pointSize: 14, weight: .regular)
        let image = NSImage(systemSymbolName: name, accessibilityDescription: accessibilityDescription)?
            .withSymbolConfiguration(configuration)
        image?.isTemplate = true
        image?.size = NSSize(width: 16, height: 16)
        return image
    }

    @objc private func openSettings() {
        onOpenSettings?()
    }

    @objc private func pairDevice() {
        onPairDevice?()
    }

    @objc private func forgetConnectedDevice(_ sender: NSMenuItem) {
        guard let deviceID = sender.representedObject as? String else { return }
        onForgetDevice?(deviceID)
    }

    @objc private func restoreLastInput() {
        _ = onRestoreLastInput?()
    }

    @objc private func quitApp() {
        onQuit?()
    }
}
