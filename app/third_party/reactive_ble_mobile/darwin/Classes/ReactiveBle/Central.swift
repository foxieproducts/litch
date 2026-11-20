import CoreBluetooth

typealias RSSI = Int

typealias PeripheralID = UUID
typealias ServiceID = CBUUID
typealias ServiceInstanceID = String
typealias CharacteristicID = CBUUID
typealias CharacteristicInstanceID = String

typealias ServiceData = [ServiceID: Data]
typealias AdvertisementData = [String: Any]

final class Central {

    typealias StateChangeHandler = (Central, CBManagerState) -> Void
    typealias DiscoveryHandler = (Central, CBPeripheral, AdvertisementData, RSSI) -> Void
    typealias ConnectionChangeHandler = (Central, CBPeripheral, ConnectionChange) -> Void
    typealias ServicesWithCharacteristicsDiscoveryHandler = (Central, CBPeripheral, [Error]) -> Void
    typealias CharacteristicNotifyCompletionHandler = (Central, Error?) -> Void
    typealias CharacteristicValueUpdateHandler = (Central, CharacteristicInstance, Data?, Error?) -> Void
    typealias CharacteristicWriteCompletionHandler = (Central, CharacteristicInstance, Error?) -> Void

    private let diagnostics: ConnectionDiagnostics?
    private let onServicesWithCharacteristicsInitialDiscovery: ServicesWithCharacteristicsDiscoveryHandler

    private var peripheralDelegate: PeripheralDelegate!
    private var centralManagerDelegate: CentralManagerDelegate!
    private var centralManager: CBCentralManager!

    private(set) var isScanning = false
    private(set) var activePeripherals = [PeripheralID: CBPeripheral]()
    private(set) var connectRegistry = PeripheralTaskRegistry<ConnectTaskController>()
    private let servicesWithCharacteristicsDiscoveryRegistry = PeripheralTaskRegistry<ServicesWithCharacteristicsDiscoveryTaskController>()
    private let characteristicNotifyRegistry = PeripheralTaskRegistry<CharacteristicNotifyTaskController>()
    private let characteristicWriteRegistry = PeripheralTaskRegistry<CharacteristicWriteTaskController>()
    private let writeWithoutResponseAdmission = WriteWithoutResponseAdmission()
    private let readRssiRegistry = PeripheralTaskRegistry<ReadRssiTaskController>()
    // Cancellation completes at the native terminal callback, after old GATT
    // ownership is retired. No replacement task may consume that callback.
    private var disconnectCompletions = [PeripheralID: [() -> Void]]()
    private var disposed = false

    init(
        diagnostics: ConnectionDiagnostics? = nil,
        onStateChange: @escaping StateChangeHandler,
        onDiscovery: @escaping DiscoveryHandler,
        onConnectionChange: @escaping ConnectionChangeHandler,
        onServicesWithCharacteristicsInitialDiscovery: @escaping ServicesWithCharacteristicsDiscoveryHandler,
        onCharacteristicValueUpdate: @escaping CharacteristicValueUpdateHandler
    ) {
        self.diagnostics = diagnostics
        self.onServicesWithCharacteristicsInitialDiscovery = onServicesWithCharacteristicsInitialDiscovery
        self.centralManagerDelegate = CentralManagerDelegate(
            onStateChange: papply(weak: self) { central, state in
                guard !central.disposed else { return }
                if state != .poweredOn {
                    central.activePeripherals.forEach { _, peripheral in
                        let error = Failure.notPoweredOn(actualState: state)
                        central.connectRegistry.updateTask(
                            key: peripheral.identifier,
                            action: { $0.handleConnectionChange(.disconnected(error)) }
                        )
                        central.eject(peripheral, error: error)
                        onConnectionChange(central, peripheral, .disconnected(error))
                        central.completeDisconnect(peripheral.identifier)
                    }
                    for id in Array(central.disconnectCompletions.keys) {
                        central.completeDisconnect(id)
                    }
                }
                onStateChange(central, state)
            },
            onDiscovery: papply(weak: self, onDiscovery),
            onConnectionChange: papply(weak: self) { central, peripheral, change in
                guard !central.disposed else { return }
                if case .connected = change,
                    central.disconnectCompletions[peripheral.identifier] != nil {
                    return
                }
                let terminal: Bool
                switch change {
                case .connected: terminal = false
                case .failedToConnect, .disconnected: terminal = true
                }
                if terminal && central.disconnectCompletions[peripheral.identifier] == nil {
                    central.disconnectCompletions[peripheral.identifier] = []
                }
                let phase: String
                let error: Error?
                switch change {
                case .connected:
                    phase = "did_connect"
                    error = nil
                case .failedToConnect(let cause):
                    phase = "did_fail_connect"
                    error = cause
                case .disconnected(let cause):
                    phase = "did_disconnect"
                    error = cause
                }
                central.diagnostics?.record(phase, peripheralId: peripheral.identifier,
                    fields: ["state": peripheral.state.rawValue, "has_error": error != nil,
                             "error_code": error.map { ($0 as NSError).code } ?? 0,
                             "task_match": central.connectRegistry.hasTask(key: peripheral.identifier)])
                central.connectRegistry.updateTask(
                    key: peripheral.identifier,
                    action: { $0.handleConnectionChange(change) }
                )

                switch change {
                case .connected:
                    central.writeWithoutResponseAdmission.connected(peripheral.identifier)
                case .failedToConnect(let error), .disconnected(let error):
                    central.eject(peripheral, error: error ?? PluginError.connectionLost)
                }

                onConnectionChange(central, peripheral, change)
                if terminal { central.completeDisconnect(peripheral.identifier) }
            }
        )
        self.peripheralDelegate = PeripheralDelegate(
            onServicesDiscovery: papply(weak: self) { central, peripheral, error in
                central.diagnostics?.record("services_callback", peripheralId: peripheral.identifier,
                    fields: ["service_count": peripheral.services?.count ?? 0, "has_error": error != nil,
                             "error_code": error.map { ($0 as NSError).code } ?? 0,
                             "task_match": central.servicesWithCharacteristicsDiscoveryRegistry.hasTask(key: peripheral.identifier)])
                central.servicesWithCharacteristicsDiscoveryRegistry.updateTask(
                    key: peripheral.identifier,
                    action: { $0.handleServicesDiscovery(peripheral: peripheral, error: error) }
                )
            },
            onCharacteristicsDiscovery: papply(weak: self) { central, service, error in
                central.diagnostics?.record("characteristics_callback", peripheralId: service.peripheral?.identifier,
                    fields: ["service_slot": service.peripheral?.services?.firstIndex(where: { $0 === service }) ?? -1,
                             "characteristic_count": service.characteristics?.count ?? 0, "has_error": error != nil,
                             "error_code": error.map { ($0 as NSError).code } ?? 0,
                             "task_match": service.peripheral.map { central.servicesWithCharacteristicsDiscoveryRegistry.hasTask(key: $0.identifier) } ?? false])
                central.servicesWithCharacteristicsDiscoveryRegistry.updateTask(
                    key: service.peripheral!.identifier,
                    action: { $0.handleCharacteristicsDiscovery(service: service, error: error) }
                )
            },
            onCharacteristicNotificationStateUpdate: papply(weak: self) { central, characteristic, error in
                guard let q = try? CharacteristicInstance(characteristic)
                else {
                    return
                }

                central.characteristicNotifyRegistry.updateTask(
                    key: q,
                    action: { $0.complete(error: error) }
                )
            },
            onCharacteristicValueUpdate: papply(weak: self) { central, characteristic, error in
                guard let q = try? CharacteristicInstance(characteristic)
                else {
                    return
                }

                onCharacteristicValueUpdate(central, q, characteristic.value, error)
            },
            onCharacteristicValueWrite: papply(weak: self) { central, characteristic, error in
                guard let q = try? CharacteristicInstance(characteristic)
                else {
                    return
                }

                central.characteristicWriteRegistry.updateTask(
                    key: q,
                    action: { $0.handleWrite(error: error) }
                )

                guard let peripheral = characteristic.service?.peripheral,
                      central.activePeripherals[q.peripheralID] === peripheral,
                      peripheral.state == .connected else { return }
                central.characteristicWriteRegistry.updateTask(
                    key: q,
                    action: { $0.start(peripheral: peripheral) }
                )
            },
            onReadRssi: papply(weak: self) { central, peripheral, rssi, error in
                central.readRssiRegistry.updateTask(
                    key: peripheral.identifier,
                    action: { $0.handleReadRssi(rssi: rssi, error: error) }
                )
            },
            onWriteWithoutResponseReady: papply(weak: self) { central, peripheral in
                guard central.activePeripherals[peripheral.identifier] === peripheral,
                      peripheral.state == .connected else { return }
                central.writeWithoutResponseAdmission.ready(peripheral.identifier)
            }
        )
        self.centralManager = CBCentralManager(
            delegate: centralManagerDelegate,
            queue: nil
        )
    }

    var state: CBManagerState { return centralManager.state }

    func scanForDevices(with services: [ServiceID]?) {
        isScanning = true
        centralManager.scanForPeripherals(
            withServices: services,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
    }

    func stopScan() {
        centralManager.stopScan()
        isScanning = false
    }

    func connect(to peripheralID: PeripheralID, discover servicesWithCharacteristicsToDiscover: ServicesWithCharacteristicsToDiscover, timeout: TimeInterval?) throws {
        guard !disposed else { throw Failure.disposed }
        guard centralManager.state == .poweredOn else {
            throw Failure.notPoweredOn(actualState: centralManager.state)
        }
        guard disconnectCompletions[peripheralID] == nil else {
            throw Failure.peripheralIsDisconnecting(peripheralID)
        }
        let peripheral = try resolve(known: peripheralID)

        writeWithoutResponseAdmission.beginConnection(peripheralID)
        peripheral.delegate = peripheralDelegate
        activePeripherals[peripheral.identifier] = peripheral

        let existingTask = connectRegistry.hasTask(key: peripheralID)
        connectRegistry.registerTask(
            key: peripheralID,
            params: .init(),
            timeout: timeout.map { timeout in (
                duration: timeout,
                handler: papply(weak: self) { (central: Central) -> Void in
                    central.disconnect(from: peripheralID)
                }
            )},
            completion: papply(weak: self) { central, connectionChange in
                switch connectionChange {
                case .connected:
                    peripheral.delegate = central.peripheralDelegate

                    central.discoverServicesWithCharacteristics(
                        for: peripheral,
                        discover: servicesWithCharacteristicsToDiscover,
                        completion: central.onServicesWithCharacteristicsInitialDiscovery
                    )
                case .failedToConnect, .disconnected:
                    break
                }
            }
        )

        diagnostics?.record("connect_registered", peripheralId: peripheralID,
            fields: ["existing_task": existingTask])
        connectRegistry.updateTask(
            key: peripheralID,
            action: {
                return $0.connect(centralManager: centralManager, peripheral: peripheral, diagnostics: diagnostics)
            }
        )
    }

    func disconnect(from peripheralID: PeripheralID, completion: @escaping () -> Void = {}) {
        guard !disposed else { completion(); return }
        if disconnectCompletions[peripheralID] != nil {
            disconnectCompletions[peripheralID]!.append(completion)
            return
        }
        disconnectCompletions[peripheralID] = [completion]
        writeWithoutResponseAdmission.cancel(peripheralID, error: PluginError.connectionLost)
        guard let peripheral = activePeripherals[peripheralID] ?? (try? resolve(known: peripheralID)) else {
            completeDisconnect(peripheralID)
            return
        }
        // A native state change can precede delegate delivery. Only an already
        // retired adapter, not the scalar alone, proves there is no old callback.
        let retired = activePeripherals[peripheralID] == nil
        if (peripheral.state == .disconnected && retired) || centralManager.state != .poweredOn {
            connectRegistry.updateTask(
                key: peripheralID,
                action: { $0.handleConnectionChange(.disconnected(nil)) }
            )
            eject(peripheral, error: PluginError.connectionLost)
            completeDisconnect(peripheralID)
            return
        }

        if peripheral.state != .disconnected {
            diagnostics?.record("cancel_requested", peripheralId: peripheralID, fields: ["state": peripheral.state.rawValue])
            centralManager.cancelPeripheralConnection(peripheral)
        }
    }

    private func completeDisconnect(_ peripheralID: PeripheralID) {
        let completions = disconnectCompletions.removeValue(forKey: peripheralID) ?? []
        completions.forEach { $0() }
    }

    func dispose() {
        guard !disposed else { return }
        disposed = true
        stopScan()
        for peripheral in Array(activePeripherals.values) {
            if disconnectCompletions[peripheral.identifier] == nil && peripheral.state != .disconnected {
                centralManager.cancelPeripheralConnection(peripheral)
            }
            connectRegistry.updateTask(
                key: peripheral.identifier,
                action: { $0.handleConnectionChange(.disconnected(nil)) }
            )
            eject(peripheral, error: PluginError.connectionLost)
        }
        for id in Array(disconnectCompletions.keys) { completeDisconnect(id) }
    }

    func discoverServicesWithCharacteristics(
        for peripheralID: PeripheralID,
        discover servicesWithCharacteristicsToDiscover: ServicesWithCharacteristicsToDiscover,
        completion: @escaping ServicesWithCharacteristicsDiscoveryHandler
    ) throws {
        let peripheral = try resolve(connected: peripheralID)

        discoverServicesWithCharacteristics(
            for: peripheral,
            discover: servicesWithCharacteristicsToDiscover,
            completion: completion
        )
    }

    func peripheral(for peripheralID: PeripheralID) throws -> CBPeripheral {
        return try resolve(connected: peripheralID)
    }

    private func discoverServicesWithCharacteristics(
        for peripheral: CBPeripheral,
        discover servicesWithCharacteristicsToDiscover: ServicesWithCharacteristicsToDiscover,
        completion: @escaping ServicesWithCharacteristicsDiscoveryHandler
    ) {
        servicesWithCharacteristicsDiscoveryRegistry.registerTask(
            key: peripheral.identifier,
            params: .init(servicesWithCharacteristicsToDiscover: servicesWithCharacteristicsToDiscover, diagnostics: diagnostics),
            completion: papply(weak: self) { central, result in
                central.diagnostics?.record("discovery_completed", peripheralId: peripheral.identifier,
                    fields: ["error_count": result.count, "state": peripheral.state.rawValue])
                completion(central, peripheral, result)
            }
        )
        servicesWithCharacteristicsDiscoveryRegistry.updateTask(
            key: peripheral.identifier,
            action: { $0.start(peripheral: peripheral) }
        )
    }

    func turnNotifications(_ state: OnOff, for characteristicInstance: CharacteristicInstance, completion: @escaping CharacteristicNotifyCompletionHandler) throws {
        let characteristic = try resolve(characteristic: characteristicInstance)

        guard [CBCharacteristicProperties.notify, .notifyEncryptionRequired, .indicate, .indicateEncryptionRequired]
                .contains(where: characteristic.properties.contains)
        else { throw Failure.notificationsNotSupported(characteristicInstance) }

        characteristicNotifyRegistry.registerTask(
            key: characteristicInstance,
            params: .init(state: state),
            completion: papply(weak: self) { central, result in
                completion(central, result)
            }
        )

        characteristicNotifyRegistry.updateTask(
            key: characteristicInstance,
            action: { $0.start(characteristic: characteristic) }
        )
    }

    func read(characteristic characteristicInstance: CharacteristicInstance) throws {
        let characteristic = try resolve(characteristic: characteristicInstance)

        guard characteristic.properties.contains(.read)
        else { throw Failure.notReadable(characteristicInstance) }

        guard let peripheral = characteristic.service?.peripheral
        else { throw Failure.peripheralIsUnknown(characteristicInstance.peripheralID) }

        peripheral.readValue(for: characteristic)
    }

    func writeWithResponse(
        value: Data,
        characteristic characteristicInstance: CharacteristicInstance,
        completion: @escaping CharacteristicWriteCompletionHandler
    ) throws {
        let characteristic = try resolve(characteristic: characteristicInstance)

        guard characteristic.properties.contains(.write)
        else { throw Failure.notWritable(characteristicInstance) }

        let qualifiedChar = try CharacteristicInstance(characteristic)

        characteristicWriteRegistry.registerTask(
            key: qualifiedChar,
            params: .init(value: value),
            completion: papply(weak: self) { central, error in
                completion(central, qualifiedChar, error)
            }
        )

        guard let peripheral = characteristic.service?.peripheral
        else { throw Failure.peripheralIsUnknown(qualifiedChar.peripheralID) }

        characteristicWriteRegistry.updateTask(
            key: qualifiedChar,
            action: { $0.start(peripheral: peripheral) }
        )
    }

    func writeWithoutResponse(
        value: Data,
        characteristic characteristicInstance: CharacteristicInstance,
        completion: @escaping (Error?) -> Void
    ) throws {
        let characteristic = try resolve(characteristic: characteristicInstance)

        guard characteristic.properties.contains(.writeWithoutResponse)
        else { throw Failure.notWritable(characteristicInstance) }

        guard let peripheral = characteristic.service?.peripheral
        else { throw Failure.characteristicNotFound(characteristicInstance) }

        guard value.count <= peripheral.maximumWriteValueLength(for: .withoutResponse)
        else { throw PluginError.invalidMethodCall(method: "writeWithoutResponse", details: "value exceeds native maximum write length") }

        writeWithoutResponseAdmission.submit(
            peripheral.identifier,
            canSend: { peripheral.state == .connected && peripheral.canSendWriteWithoutResponse },
            send: { peripheral.writeValue(value, for: characteristic, type: .withoutResponse) },
            completion: completion
        )
    }

    func writeWithoutResponseBatch(
        values: [Data],
        characteristic characteristicInstance: CharacteristicInstance,
        completion: @escaping (Error?) -> Void
    ) throws {
        let characteristic = try resolve(characteristic: characteristicInstance)

        guard characteristic.properties.contains(.writeWithoutResponse)
        else { throw Failure.notWritable(characteristicInstance) }

        guard let peripheral = characteristic.service?.peripheral
        else { throw Failure.characteristicNotFound(characteristicInstance) }

        let maximum = peripheral.maximumWriteValueLength(for: .withoutResponse)
        guard values.allSatisfy({ !$0.isEmpty && $0.count <= maximum })
        else {
            throw PluginError.invalidMethodCall(
                method: "writeWithoutResponseBatch",
                details: "a packet is empty or exceeds native maximum write length"
            )
        }

        writeWithoutResponseAdmission.submitBatch(
            peripheral.identifier,
            values: values,
            canSend: {
                peripheral.state == .connected && peripheral.canSendWriteWithoutResponse
            },
            send: { value in
                peripheral.writeValue(value, for: characteristic, type: .withoutResponse)
            },
            completion: completion
        )
    }

    func maximumWriteValueLength(for peripheral: PeripheralID, type: CBCharacteristicWriteType) throws -> Int {
        let peripheral = try resolve(connected: peripheral)
        return peripheral.maximumWriteValueLength(for: type)
    }

    
    func readRssi(for peripheralId: PeripheralID, completion: @escaping (Failable<Int>) -> Void) throws {
        let peripheral = try resolve(connected: peripheralId)

        readRssiRegistry.registerTask(
            key: peripheralId,
            params: .init(),
            completion: completion
        )

        readRssiRegistry.updateTask(
            key: peripheralId,
            action: {
                $0.start(peripheral: peripheral)
            }
        )
    }

    private func eject(_ peripheral: CBPeripheral, error: Error) {
        diagnostics?.record("eject_started", peripheralId: peripheral.identifier,
            fields: ["state": peripheral.state.rawValue, "error_code": (error as NSError).code,
                     "task_match": servicesWithCharacteristicsDiscoveryRegistry.hasTask(key: peripheral.identifier)])
        writeWithoutResponseAdmission.cancel(peripheral.identifier, error: error)
        peripheral.delegate = nil
        activePeripherals[peripheral.identifier] = nil

        servicesWithCharacteristicsDiscoveryRegistry.updateTasks(
            in: peripheral.identifier,
            action: { $0.cancel(error: error) }
        )
        characteristicNotifyRegistry.updateTasks(
            in: peripheral.identifier,
            action: { $0.cancel(error: error) }
        )
        characteristicWriteRegistry.updateTasks(
            in: peripheral.identifier,
            action: { $0.cancel(error: error) }
        )
        diagnostics?.record("eject_completed", peripheralId: peripheral.identifier)
    }

    private func resolve(known peripheralID: PeripheralID) throws -> CBPeripheral {
        guard let peripheral = centralManager.retrievePeripherals(withIdentifiers: [peripheralID]).first
        else { throw Failure.peripheralIsUnknown(peripheralID) }

        return peripheral
    }

    private func resolve(connected peripheralID: PeripheralID) throws -> CBPeripheral {
        guard let peripheral = activePeripherals[peripheralID]
        else { throw Failure.peripheralIsUnknown(peripheralID) }

        guard peripheral.state == .connected
        else { throw Failure.peripheralIsNotConnected(peripheralID) }

        return peripheral
    }

    private func resolve(characteristic characteristicInstance: CharacteristicInstance) throws -> CBCharacteristic {
        let peripheral = try resolve(connected: characteristicInstance.peripheralID)

        guard let service = peripheral.services?.filter({ $0.uuid == characteristicInstance.serviceID })[Int(characteristicInstance.serviceInstanceID) ?? 0]
        else { throw Failure.serviceNotFound(characteristicInstance.serviceID, characteristicInstance.peripheralID) }

        guard let characteristic = service.characteristics?.filter({ $0.uuid == characteristicInstance.id })[Int(characteristicInstance.instanceID) ?? 0]
        else { throw Failure.characteristicNotFound(characteristicInstance) }

        return characteristic
    }

    private enum Failure: Error, CustomStringConvertible {

        case notPoweredOn(actualState: CBManagerState)
        case peripheralIsDisconnecting(PeripheralID)
        case disposed
        case peripheralIsUnknown(PeripheralID)
        case peripheralIsNotConnected(PeripheralID)
        case serviceNotFound(ServiceID, PeripheralID)
        case characteristicNotFound(CharacteristicInstance)
        case notificationsNotSupported(CharacteristicInstance)
        case notReadable(CharacteristicInstance)
        case notWritable(CharacteristicInstance)

        var description: String {
            switch self {
            case .disposed:
                return "The BLE central has been disposed"
            case .peripheralIsDisconnecting(let peripheralID):
                return "The peripheral \(peripheralID) is still disconnecting"
            case .notPoweredOn(let actualState):
                return "Bluetooth is not powered on (the current state code is \(actualState.rawValue))"
            case .peripheralIsUnknown(let peripheralID):
                return "A peripheral \(peripheralID.uuidString) is unknown (make sure it has been discovered)"
            case .peripheralIsNotConnected(let peripheralID):
                return "The peripheral \(peripheralID.uuidString) is not connected"
            case .serviceNotFound(let serviceID, let peripheralID):
                return "A service \(serviceID) is not found in the peripheral \(peripheralID) (make sure it has been discovered)"
            case .characteristicNotFound(let characteristicInstance):
                return "A characteristic \(characteristicInstance.id) is not found in the service \(characteristicInstance.serviceID) of the peripheral \(characteristicInstance.peripheralID) (make sure it has been discovered)"
            case .notificationsNotSupported(let characteristicInstance):
                return "The characteristic \(characteristicInstance.id) of the service \(characteristicInstance.serviceID) of the peripheral \(characteristicInstance.peripheralID) does not support either notifications or indications"
            case .notReadable(let characteristicInstance):
                return "The characteristic \(characteristicInstance.id) of the service \(characteristicInstance.serviceID) of the peripheral \(characteristicInstance.peripheralID) is not readable"
            case .notWritable(let characteristicInstance):
                return "The characteristic \(characteristicInstance.id) of the service \(characteristicInstance.serviceID) of the peripheral \(characteristicInstance.peripheralID) is not writable"
            }
        }
    }
}
