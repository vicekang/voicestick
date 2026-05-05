import CoreBluetooth
import Foundation

struct ConnectedVoiceStickDevice {
    let name: String
    let deviceID: String
}

final class BleCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var pairedDeviceIDs: Set<String>
    private var central: CBCentralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var discoveredDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var connectedDevices: [UUID: ConnectedVoiceStickDevice] = [:]
    private var controlCharacteristics: [UUID: CBCharacteristic] = [:]

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
        onConnectionChange?(nil)
        scanIfReady()
    }

    func sendControl(event: String, text: String) {
        let data = BleProtocol.controlPayload(event: event, text: text)
        for (id, characteristic) in controlCharacteristics {
            peripherals[id]?.writeValue(data, for: characteristic, type: .withoutResponse)
        }
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
        default:
            break
        }
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
