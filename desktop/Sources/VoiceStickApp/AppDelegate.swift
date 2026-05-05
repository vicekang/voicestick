import AppKit
import Sparkle

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusController: StatusController?
    private var coordinator: VoiceStickCoordinator?
    private var settingsWindowController: SettingsWindowController?
    private var pairDeviceWindowController: PairDeviceWindowController?
    private var updaterController: SPUStandardUpdaterController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let config = AppConfig.load()
        let statusController = StatusController(needsPairing: config.pairedDeviceIDs.isEmpty)
        let coordinator = VoiceStickCoordinator(config: config, statusController: statusController)

        self.statusController = statusController
        self.coordinator = coordinator

        statusController.onQuit = { NSApp.terminate(nil) }
        statusController.onOpenSettings = { [weak self] in
            let controller = self?.settingsWindowController ?? SettingsWindowController()
            self?.settingsWindowController = controller
            controller.onPairedDevicesChanged = { [weak self] deviceIDs in
                self?.statusController?.setNeedsPairing(deviceIDs.isEmpty)
                self?.coordinator?.updatePairedDeviceIDs(deviceIDs)
            }
            controller.show()
        }
        statusController.onPairDevice = { [weak self] in
            self?.showPairDeviceWindow()
        }
        statusController.onForgetDevice = { [weak self] deviceID in
            self?.forgetDevice(deviceID)
        }
        statusController.onRestoreLastInput = { [weak self] in
            self?.coordinator?.restoreLastInputConfirmation() ?? false
        }
        if Self.hasSparklePublicKey {
            let updaterController = SPUStandardUpdaterController(
                startingUpdater: true,
                updaterDelegate: nil,
                userDriverDelegate: nil
            )
            self.updaterController = updaterController
            statusController.onCheckForUpdates = {
                updaterController.updater.checkForUpdates()
            }
        }
        statusController.setStatus(config.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Scanning")
        coordinator.start()
    }

    private static var hasSparklePublicKey: Bool {
        guard let publicKey = Bundle.main.object(forInfoDictionaryKey: "SUPublicEDKey") as? String else {
            return false
        }
        return !publicKey.isEmpty && !publicKey.hasPrefix("REPLACE_WITH")
    }

    private func showPairDeviceWindow() {
        var config = AppConfig.load()
        let controller = PairDeviceWindowController(existingDeviceIDs: config.pairedDeviceIDs) { [weak self] deviceID in
            if !config.pairedDeviceIDs.contains(deviceID) {
                config.pairedDeviceIDs.append(deviceID)
            }
            do {
                try config.save()
                self?.statusController?.setNeedsPairing(config.pairedDeviceIDs.isEmpty)
                self?.coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
            } catch {
                self?.statusController?.setStatus("Pair save failed")
            }
        }
        pairDeviceWindowController = controller
        controller.show()
    }

    private func forgetDevice(_ deviceID: String) {
        var config = AppConfig.load()
        config.pairedDeviceIDs.removeAll { $0 == deviceID }
        do {
            try config.save()
            statusController?.setConnectedDevice(nil)
            statusController?.setNeedsPairing(config.pairedDeviceIDs.isEmpty)
            coordinator?.updatePairedDeviceIDs(config.pairedDeviceIDs)
        } catch {
            statusController?.setStatus("Forget device failed")
        }
    }
}
