import CoreBluetooth
import Foundation

struct ConnectedVoiceStickDevice {
    let name: String
    let deviceID: String
}

struct FirmwareUpdateProgress {
    let writtenBytes: Int
    let totalBytes: Int
    let isDeviceConfirmed: Bool

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return Double(writtenBytes) / Double(totalBytes)
    }
}

final class BleCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum FirmwareUpdateError: LocalizedError {
        case noConnectedDevice
        case otaCharacteristicUnavailable
        case imageTooLarge
        case transferAlreadyActive
        case firmwareUpdateCancelled
        case peripheralWriteFailed(String)
        case deviceError(String)

        var errorDescription: String? {
            switch self {
            case .noConnectedDevice:
                return "No VoiceStick is connected."
            case .otaCharacteristicUnavailable:
                return "The connected firmware does not expose BLE OTA."
            case .imageTooLarge:
                return "Firmware image is larger than the OTA partition."
            case .transferAlreadyActive:
                return "A firmware update is already running."
            case .firmwareUpdateCancelled:
                return "Firmware update cancelled."
            case .peripheralWriteFailed(let message):
                return "BLE write failed: \(message)"
            case .deviceError(let code):
                return "Device rejected OTA: \(code)"
            }
        }
    }

    private struct FirmwareUpdateSession {
        let peripheralID: UUID
        let transferID: UInt32
        let image: Data
        let chunkSize: Int
        var began = false
        var offset = 0
        var lastQueuedProgressOffset = 0
        var ended = false
        let progress: (FirmwareUpdateProgress) -> Void
        let completion: (Result<Void, Error>) -> Void
    }

    private var pairedDeviceIDs: Set<String>
    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var discoveredDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var connectedDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var controlCharacteristics: [UUID: CBCharacteristic] = [:]
    private var otaCharacteristics: [UUID: CBCharacteristic] = [:]
    private var firmwareUpdateSession: FirmwareUpdateSession?

    var onConnectionChange: ((ConnectedVoiceStickDevice?) -> Void)?
    var onAudioFrame: ((AudioFrame) -> Void)?
    var onStateEvent: ((StateEvent) -> Void)?

    init(pairedDeviceIDs: [String]) {
        self.pairedDeviceIDs = Set(pairedDeviceIDs)
        super.init()
    }

    func start() {
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = Set(deviceIDs)
        for peripheral in peripherals.values {
            central.cancelPeripheralConnection(peripheral)
        }
        peripherals.removeAll()
        discoveredDevices.removeAll()
        connectedDevices.removeAll()
        controlCharacteristics.removeAll()
        otaCharacteristics.removeAll()
        failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        onConnectionChange?(nil)
        scanIfReady()
    }

    func sendControl(event: String, text: String) {
        let data = BleProtocol.controlPayload(event: event, text: text)
        for (id, characteristic) in controlCharacteristics {
            peripherals[id]?.writeValue(data, for: characteristic, type: .withoutResponse)
        }
    }

    func updateFirmware(image: Data, progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        guard firmwareUpdateSession == nil else {
            completion(.failure(FirmwareUpdateError.transferAlreadyActive))
            return
        }
        guard let peripheralID = currentConnectedPeripheralID,
              let peripheral = peripherals[peripheralID] else {
            completion(.failure(FirmwareUpdateError.noConnectedDevice))
            return
        }
        guard otaCharacteristics[peripheralID] != nil else {
            completion(.failure(FirmwareUpdateError.otaCharacteristicUnavailable))
            return
        }
        guard image.count <= 3 * 1024 * 1024 else {
            completion(.failure(FirmwareUpdateError.imageTooLarge))
            return
        }

        let maxWrite = peripheral.maximumWriteValueLength(for: .withoutResponse)
        let chunkSize = max(20, min(maxWrite - 12, 244))
        firmwareUpdateSession = FirmwareUpdateSession(
            peripheralID: peripheralID,
            transferID: UInt32.random(in: 1...UInt32.max),
            image: image,
            chunkSize: chunkSize,
            progress: progress,
            completion: completion
        )
        progress(FirmwareUpdateProgress(
            writtenBytes: 0,
            totalBytes: image.count,
            isDeviceConfirmed: true
        ))
        sendNextFirmwareUpdateFrame()
    }

    func cancelFirmwareUpdate() {
        failFirmwareUpdate(FirmwareUpdateError.firmwareUpdateCancelled)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else { return }
        scanIfReady()
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard shouldConnect(localName: localName, peripheralName: peripheral.name) else { return }
        guard peripherals[peripheral.identifier] == nil else { return }
        if let device = connectedDevice(localName: localName, peripheralName: peripheral.name) {
            discoveredDevices[peripheral.identifier] = device
        }
        peripherals[peripheral.identifier] = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedDevices[peripheral.identifier] = discoveredDevices[peripheral.identifier]
            ?? connectedDevice(localName: nil, peripheralName: peripheral.name)
        onConnectionChange?(currentConnectedDevice)
        peripheral.discoverServices([CBUUID(string: BleProtocol.serviceUUID)])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        peripherals.removeValue(forKey: peripheral.identifier)
        discoveredDevices.removeValue(forKey: peripheral.identifier)
        connectedDevices.removeValue(forKey: peripheral.identifier)
        controlCharacteristics.removeValue(forKey: peripheral.identifier)
        otaCharacteristics.removeValue(forKey: peripheral.identifier)
        if firmwareUpdateSession?.peripheralID == peripheral.identifier {
            failFirmwareUpdate(FirmwareUpdateError.noConnectedDevice)
        }
        onConnectionChange?(currentConnectedDevice)
        scanIfReady()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        peripheral.services?.forEach {
            peripheral.discoverCharacteristics(nil, for: $0)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        service.characteristics?.forEach { characteristic in
            switch characteristic.uuid.uuidString.uppercased() {
            case BleProtocol.audioUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.stateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleProtocol.controlUUID:
                controlCharacteristics[peripheral.identifier] = characteristic
                sendControl(event: "connected", text: "")
            case BleProtocol.otaRXUUID:
                otaCharacteristics[peripheral.identifier] = characteristic
            case BleProtocol.otaStateUUID:
                peripheral.setNotifyValue(true, for: characteristic)
            default:
                break
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        switch characteristic.uuid.uuidString.uppercased() {
        case BleProtocol.audioUUID:
            if let frame = BleProtocol.parseAudioFrame(data) {
                onAudioFrame?(frame)
            }
        case BleProtocol.stateUUID:
            if let event = BleProtocol.parseStateEvent(data) {
                onStateEvent?(event)
            }
        case BleProtocol.otaStateUUID:
            if let event = BleProtocol.parseFirmwareOTAStateEvent(data) {
                handleFirmwareUpdateStateEvent(event)
            }
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid.uuidString.uppercased() == BleProtocol.otaRXUUID,
              firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        if let error {
            failFirmwareUpdate(FirmwareUpdateError.peripheralWriteFailed(error.localizedDescription))
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        guard firmwareUpdateSession?.peripheralID == peripheral.identifier else {
            return
        }
        sendNextFirmwareUpdateFrame()
    }

    private func shouldConnect(localName: String?, peripheralName: String?) -> Bool {
        let advertisedName = localName ?? peripheralName ?? ""
        if !pairedDeviceIDs.isEmpty {
            guard let deviceID = Self.deviceID(from: advertisedName) else { return false }
            return pairedDeviceIDs.contains(deviceID)
        }

        return false
    }

    private func connectedDevice(localName: String?, peripheralName: String?) -> ConnectedVoiceStickDevice? {
        let advertisedName = localName ?? peripheralName ?? ""
        guard let deviceID = Self.deviceID(from: advertisedName) else { return nil }
        return ConnectedVoiceStickDevice(
            name: advertisedName.isEmpty ? "VS-\(deviceID)" : advertisedName,
            deviceID: deviceID
        )
    }

    private var currentConnectedDevice: ConnectedVoiceStickDevice? {
        connectedDevices.values.sorted { $0.name < $1.name }.first
    }

    private var currentConnectedPeripheralID: UUID? {
        connectedDevices.keys.sorted { lhs, rhs in
            (connectedDevices[lhs]?.name ?? "") < (connectedDevices[rhs]?.name ?? "")
        }.first
    }

    private func sendNextFirmwareUpdateFrame() {
        guard var session = firmwareUpdateSession,
              let peripheral = peripherals[session.peripheralID],
              let characteristic = otaCharacteristics[session.peripheralID] else {
            failFirmwareUpdate(FirmwareUpdateError.otaCharacteristicUnavailable)
            return
        }

        if !session.began {
            let payload = BleProtocol.otaBeginPayload(
                imageSize: UInt32(session.image.count),
                transferID: session.transferID
            )
            session.began = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else if session.offset < session.image.count {
            while session.offset < session.image.count && peripheral.canSendWriteWithoutResponse {
                let end = min(session.offset + session.chunkSize, session.image.count)
                let chunk = session.image.subdata(in: session.offset..<end)
                let payload = BleProtocol.otaDataPayload(
                    transferID: session.transferID,
                    offset: UInt32(session.offset),
                    chunk: chunk
                )
                peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
                session.offset = end
                if session.offset - session.lastQueuedProgressOffset >= 64 * 1024 ||
                    session.offset == session.image.count {
                    session.lastQueuedProgressOffset = session.offset
                    session.progress(FirmwareUpdateProgress(
                        writtenBytes: session.offset,
                        totalBytes: session.image.count,
                        isDeviceConfirmed: false
                    ))
                }
            }
            firmwareUpdateSession = session
            if session.offset == session.image.count {
                sendNextFirmwareUpdateFrame()
            }
        } else if !session.ended {
            let payload = BleProtocol.otaEndPayload(
                transferID: session.transferID,
                imageSize: UInt32(session.image.count)
            )
            session.ended = true
            firmwareUpdateSession = session
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
        } else {
            return
        }
    }

    private func handleFirmwareUpdateStateEvent(_ event: FirmwareOTAStateEvent) {
        guard let session = firmwareUpdateSession else { return }
        if let transferID = event.transferID, transferID != session.transferID {
            return
        }

        switch event.event {
        case "progress":
            if let written = event.written, let size = event.size, size > 0 {
                session.progress(FirmwareUpdateProgress(
                    writtenBytes: Int(written),
                    totalBytes: Int(size),
                    isDeviceConfirmed: true
                ))
            }
        case "done":
            firmwareUpdateSession = nil
            session.progress(FirmwareUpdateProgress(
                writtenBytes: session.image.count,
                totalBytes: session.image.count,
                isDeviceConfirmed: true
            ))
            session.completion(.success(()))
        case "error":
            failFirmwareUpdate(FirmwareUpdateError.deviceError(event.code ?? "unknown"))
        default:
            break
        }
    }

    private func failFirmwareUpdate(_ error: Error) {
        guard let session = firmwareUpdateSession else { return }
        if let peripheral = peripherals[session.peripheralID],
           let characteristic = otaCharacteristics[session.peripheralID] {
            let payload = BleProtocol.otaAbortPayload(transferID: session.transferID)
            peripheral.writeValue(payload, for: characteristic, type: .withoutResponse)
        }
        firmwareUpdateSession = nil
        session.completion(.failure(error))
    }

    private func scanIfReady() {
        guard let central, central.state == .poweredOn else { return }
        if pairedDeviceIDs.isEmpty {
            central.stopScan()
        } else {
            central.scanForPeripherals(withServices: [CBUUID(string: BleProtocol.serviceUUID)])
        }
    }

    static func deviceID(from name: String) -> String? {
        let upper = name.uppercased()
        guard upper.hasPrefix("VS-") else { return nil }
        let id = String(upper.dropFirst(3).prefix(4))
        guard id.count == 4, id.allSatisfy(\.isHexDigit) else { return nil }
        return id
    }
}
