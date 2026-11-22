import CoreBluetooth
import XCTest

// Calls the real Central submission and installed PeripheralDelegate. The SDK
// shim counts requests. Explicit callbacks drive the production task owners.
private final class WriteFixture {
    let peripheral = CBPeripheral(identifier: UUID(uuidString: "00000000-0000-0000-0000-000000000001")!)
    let first = CBCharacteristic(uuid: CBUUID(string: "c1"), properties: .write)
    let second = CBCharacteristic(uuid: CBUUID(string: "c2"), properties: .write)
    let service = CBService(uuid: CBUUID(string: "s1"))
    let central: Central
    var completions = [String]()

    init(onConnectionChange: @escaping (ConnectionChange) -> Void = { _ in }) throws {
        central = Central(
            onStateChange: { _, _ in },
            onDiscovery: { _, _, _, _ in },
            onConnectionChange: { _, _, change in onConnectionChange(change) },
            onServicesWithCharacteristicsInitialDiscovery: { _, _, _ in },
            onCharacteristicValueUpdate: { _, _, _, _ in }
        )
        peripheral.services = [service]
        service.peripheral = peripheral
        service.characteristics = [first, second]
        first.service = service
        second.service = service
        CBCentralManager.knownPeripherals = [peripheral]
        try central.connect(to: peripheral.identifier, discover: .some([:]), timeout: nil)
        let manager = CBCentralManager.last!
        manager.delegate!.centralManager(manager, didConnect: peripheral)
        peripheral.onWrite = { request in
            print("SDK_REQUEST char=\(request.characteristic.uuid) payload=\(Array(request.value))")
        }
    }

    func submit(_ value: UInt8, to characteristic: CBCharacteristic? = nil, then action: (() -> Void)? = nil) throws {
        let c = characteristic ?? first
        print("SUBMIT char=\(c.uuid) payload=[\(value)]")
        try central.writeWithResponse(value: Data([value]), characteristic: CharacteristicInstance(c)) { _, _, error in
            let result = "\(value):\(error == nil ? "ok" : "error")"
            self.completions.append(result)
            print("COMPLETION \(result)")
            action?()
        }
    }

    func acknowledge(_ characteristic: CBCharacteristic? = nil, error: Error? = nil) {
        let c = characteristic ?? first
        print("SDK_CALLBACK char=\(c.uuid) error=\(error != nil)")
        peripheral.delegate?.peripheral(peripheral, didWriteValueFor: c, error: error)
    }

    var sent: [UInt8] { peripheral.writes.map { $0.value.first! } }
}

final class CharacteristicWriteProbeTests: XCTestCase {
    func testInitialDiscoveryPreservesFinalErrorAndHealthyCompletion() throws {
        let missingPairing = NSError(domain: "CBErrorDomain", code: 14)
        let earlierFailure = NSError(domain: "CBATTErrorDomain", code: 5)
        let scenarios: [[NSError?]] = [
            [missingPairing],
            [nil, missingPairing],
            [earlierFailure, missingPairing],
            [nil, nil],
        ]
        for callbackErrors in scenarios {
            var completions = [[NSError]]()
            let central = Central(
                onStateChange: { _, _ in },
                onDiscovery: { _, _, _, _ in },
                onConnectionChange: { _, _, _ in },
                onServicesWithCharacteristicsInitialDiscovery: { _, _, errors in
                    completions.append(errors.map { $0 as NSError })
                },
                onCharacteristicValueUpdate: { _, _, _, _ in }
            )
            defer { central.dispose() }
            let peripheral = CBPeripheral(identifier: UUID(uuidString: "00000000-0000-0000-0000-000000000001")!)
            let services = callbackErrors.indices.map { index -> CBService in
                let service = CBService(uuid: CBUUID(string: "s\(index)"))
                service.peripheral = peripheral
                return service
            }
            peripheral.services = services
            CBCentralManager.knownPeripherals = [peripheral]
            try central.connect(to: peripheral.identifier, discover: .all, timeout: nil)
            let manager = CBCentralManager.last!
            manager.delegate!.centralManager(manager, didConnect: peripheral)
            let delegate = try XCTUnwrap(peripheral.delegate as? PeripheralDelegate)
            delegate.peripheral(peripheral, didDiscoverServices: nil)
            for (index, service) in services.enumerated() {
                XCTAssertTrue(completions.isEmpty, "Discovery must await every requested service")
                delegate.peripheral(peripheral, didDiscoverCharacteristicsFor: service, error: callbackErrors[index])
            }
            XCTAssertEqual(completions.count, 1)
            XCTAssertEqual(completions.first, callbackErrors.compactMap { $0 })
            delegate.peripheral(peripheral, didDiscoverCharacteristicsFor: services.last!, error: missingPairing)
            XCTAssertEqual(completions.count, 1, "A late callback cannot complete retired discovery again")
        }
    }

    func testSameCharacteristicOverlapSendsEachPayloadBeforeItsCompletion() throws {
        let overlap = try WriteFixture()
        try overlap.submit(0xa1)
        try overlap.submit(0xb2)
        XCTAssertEqual(overlap.sent, [0xa1])
        overlap.acknowledge()
        XCTAssertEqual(overlap.sent, [0xa1, 0xb2])
        XCTAssertEqual(overlap.completions, ["161:ok"])
        overlap.acknowledge()
        XCTAssertEqual(overlap.completions, ["161:ok", "178:ok"])
        XCTAssertEqual(overlap.sent, [0xa1, 0xb2])
    }

    func testSequentialWritesCompleteNormally() throws {
        let sequential = try WriteFixture()
        try sequential.submit(1)
        sequential.acknowledge()
        try sequential.submit(2)
        sequential.acknowledge()
        XCTAssertEqual(sequential.sent, [1, 2])
        XCTAssertEqual(sequential.completions, ["1:ok", "2:ok"])
    }

    func testDistinctCharacteristicsProgressIndependently() throws {
        let independent = try WriteFixture()
        try independent.submit(1)
        try independent.submit(2, to: independent.second)
        XCTAssertEqual(independent.sent, [1, 2])
        independent.acknowledge(independent.second)
        independent.acknowledge()
        XCTAssertEqual(independent.completions, ["2:ok", "1:ok"])
    }

    func testDisconnectCancelsActiveAndQueuedWritesWithoutSendingQueuedPayload() throws {
        let cancel = try WriteFixture()
        try cancel.submit(1)
        try cancel.submit(2)
        cancel.peripheral.state = .disconnected
        let manager = CBCentralManager.last!
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: cancel.peripheral, error: PluginError.connectionLost)
        XCTAssertEqual(cancel.completions, ["1:error", "2:error"])
        XCTAssertEqual(cancel.sent, [1])
    }

    func testReconnectCannotOvertakeCancelledNativeConnection() throws {
        let reconnect = try WriteFixture()
        let manager = CBCentralManager.last!
        reconnect.central.disconnect(from: reconnect.peripheral.identifier)
        reconnect.peripheral.state = .disconnecting

        // The prior physical connection still owns its terminal callback.
        // Registering a replacement now lets that old callback finish it.
        XCTAssertThrowsError(try reconnect.central.connect(
            to: reconnect.peripheral.identifier, discover: .some([:]), timeout: nil
        ))
        XCTAssertEqual(manager.connectCount, 1)
        XCTAssertEqual(manager.cancelCount, 1)

        reconnect.peripheral.state = .disconnected
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: reconnect.peripheral, error: nil)
        try reconnect.central.connect(to: reconnect.peripheral.identifier, discover: .some([:]), timeout: nil)
        XCTAssertEqual(manager.connectCount, 2)
        reconnect.peripheral.state = .connected
        manager.delegate!.centralManager(manager, didConnect: reconnect.peripheral)
        try reconnect.submit(3)
        reconnect.acknowledge()
        XCTAssertEqual(reconnect.completions, ["3:ok"])
    }

    func testDisconnectCompletionFollowsCleanupAndAllowsReentrantReconnect() throws {
        var events = [String]()
        let reconnect = try WriteFixture(onConnectionChange: { change in
            if case .disconnected = change { events.append("disconnected") }
        })
        let manager = CBCentralManager.last!
        try reconnect.submit(1)
        try reconnect.submit(2)
        reconnect.central.disconnect(from: reconnect.peripheral.identifier) {
            events.append("completion")
            XCTAssertNil(reconnect.peripheral.delegate)
            XCTAssertNil(reconnect.central.activePeripherals[reconnect.peripheral.identifier])
            XCTAssertFalse(reconnect.central.connectRegistry.hasTask(key: reconnect.peripheral.identifier))
            XCTAssertEqual(reconnect.completions, ["1:error", "2:error"])
            do {
                try reconnect.central.connect(to: reconnect.peripheral.identifier, discover: .some([:]), timeout: nil)
            } catch {
                XCTFail("Completed native cleanup must admit reconnect: \(error)")
            }
        }
        XCTAssertTrue(events.isEmpty)
        XCTAssertEqual(manager.connectCount, 1)
        reconnect.peripheral.state = .disconnected
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: reconnect.peripheral, error: nil)
        XCTAssertEqual(events, ["disconnected", "completion"])
        XCTAssertEqual(manager.connectCount, 2)
        XCTAssertTrue(reconnect.central.connectRegistry.hasTask(key: reconnect.peripheral.identifier))
        reconnect.peripheral.state = .connected
        manager.delegate!.centralManager(manager, didConnect: reconnect.peripheral)
        try reconnect.submit(3)
        reconnect.acknowledge()
        XCTAssertEqual(reconnect.completions, ["1:error", "2:error", "3:ok"])
    }

    func testAlreadyDisconnectedCompletionCleansUpBeforeReentrantReconnect() throws {
        let reconnect = try WriteFixture()
        let manager = CBCentralManager.last!
        try reconnect.submit(1)
        reconnect.peripheral.state = .disconnected
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: reconnect.peripheral, error: nil)
        var completions = 0
        reconnect.central.disconnect(from: reconnect.peripheral.identifier) {
            completions += 1
            XCTAssertNil(reconnect.peripheral.delegate)
            XCTAssertNil(reconnect.central.activePeripherals[reconnect.peripheral.identifier])
            XCTAssertEqual(reconnect.completions, ["1:error"])
            do {
                try reconnect.central.connect(to: reconnect.peripheral.identifier, discover: .some([:]), timeout: nil)
            } catch {
                XCTFail("Already-disconnected cleanup must admit reconnect: \(error)")
            }
        }
        XCTAssertEqual(completions, 1)
        XCTAssertEqual(manager.cancelCount, 0)
        XCTAssertEqual(manager.connectCount, 2)
        XCTAssertTrue(reconnect.central.connectRegistry.hasTask(key: reconnect.peripheral.identifier))
    }

    func testDisconnectedNativeStateStillWaitsForTheOldTerminalCallback() throws {
        let cancel = try WriteFixture()
        let manager = CBCentralManager.last!
        // CoreBluetooth updates this property before dispatching the delegate.
        // The adapter still owns the old connection until that callback runs.
        cancel.peripheral.state = .disconnected
        var completions = 0
        cancel.central.disconnect(from: cancel.peripheral.identifier) { completions += 1 }
        XCTAssertEqual(completions, 0)
        XCTAssertThrowsError(try cancel.central.connect(
            to: cancel.peripheral.identifier, discover: .some([:]), timeout: nil
        ))
        XCTAssertEqual(manager.connectCount, 1)
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: cancel.peripheral, error: nil)
        XCTAssertEqual(completions, 1)
        try cancel.central.connect(to: cancel.peripheral.identifier, discover: .some([:]), timeout: nil)
        XCTAssertEqual(manager.connectCount, 2)
    }

    func testRepeatedDisconnectWaitsOnceAndIgnoresConnectedWhileCancelling() throws {
        var connected = 0
        let cancel = try WriteFixture(onConnectionChange: { change in
            if case .connected = change { connected += 1 }
        })
        let manager = CBCentralManager.last!
        var completions = [Int]()
        cancel.central.disconnect(from: cancel.peripheral.identifier) { completions.append(1) }
        cancel.central.disconnect(from: cancel.peripheral.identifier) { completions.append(2) }
        XCTAssertEqual(manager.cancelCount, 1)
        XCTAssertTrue(completions.isEmpty)
        manager.delegate!.centralManager(manager, didConnect: cancel.peripheral)
        XCTAssertEqual(connected, 1)
        XCTAssertTrue(completions.isEmpty)
        cancel.peripheral.state = .disconnected
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: cancel.peripheral, error: nil)
        XCTAssertEqual(completions, [1, 2])
        cancel.central.disconnect(from: cancel.peripheral.identifier) { completions.append(3) }
        XCTAssertEqual(completions, [1, 2, 3])
        XCTAssertEqual(manager.cancelCount, 1)
    }

    func testNativeConnectFailureCompletesPendingDisconnectAfterCleanup() throws {
        let cancel = try WriteFixture()
        let manager = CBCentralManager.last!
        var completions = 0
        cancel.central.disconnect(from: cancel.peripheral.identifier) {
            completions += 1
            XCTAssertNil(cancel.peripheral.delegate)
            XCTAssertTrue(cancel.central.activePeripherals.isEmpty)
        }
        cancel.peripheral.state = .disconnected
        (manager.delegate as! CentralManagerDelegate).centralManager(
            manager, didFailToConnect: cancel.peripheral, error: PluginError.connectionLost
        )
        XCTAssertEqual(completions, 1)
        cancel.central.dispose()
        XCTAssertEqual(completions, 1)
    }

    func testPowerOffCompletesCancellationOnceWithoutAllowingReconnect() throws {
        let cancel = try WriteFixture()
        let manager = CBCentralManager.last!
        try cancel.submit(1)
        var completions = 0
        cancel.central.disconnect(from: cancel.peripheral.identifier) {
            completions += 1
            XCTAssertNil(cancel.peripheral.delegate)
            XCTAssertTrue(cancel.central.activePeripherals.isEmpty)
            XCTAssertEqual(cancel.completions, ["1:error"])
            do {
                try cancel.central.connect(to: cancel.peripheral.identifier, discover: .some([:]), timeout: nil)
                XCTFail("Power-off cannot admit a connection")
            } catch {}
        }
        manager.state = .unknown
        let delegate = manager.delegate as! CentralManagerDelegate
        delegate.centralManagerDidUpdateState(manager)
        delegate.centralManagerDidUpdateState(manager)
        XCTAssertEqual(completions, 1)
        XCTAssertEqual(manager.connectCount, 1)
        cancel.central.dispose()
        XCTAssertEqual(completions, 1)
    }

    func testDisposeCompletesCancellationOnceAndRetiresLateCallbacks() throws {
        var disconnected = 0
        let cancel = try WriteFixture(onConnectionChange: { change in
            if case .disconnected = change { disconnected += 1 }
        })
        let manager = CBCentralManager.last!
        try cancel.submit(1)
        var completions = 0
        cancel.central.disconnect(from: cancel.peripheral.identifier) {
            completions += 1
            XCTAssertNil(cancel.peripheral.delegate)
            XCTAssertTrue(cancel.central.activePeripherals.isEmpty)
            XCTAssertEqual(cancel.completions, ["1:error"])
            do {
                try cancel.central.connect(to: cancel.peripheral.identifier, discover: .some([:]), timeout: nil)
                XCTFail("Disposed Central cannot admit a connection")
            } catch {}
        }
        cancel.central.dispose()
        cancel.central.dispose()
        XCTAssertEqual(completions, 1)
        XCTAssertEqual(manager.cancelCount, 1)
        cancel.peripheral.state = .disconnected
        manager.delegate!.centralManager(manager, didDisconnectPeripheral: cancel.peripheral, error: nil)
        XCTAssertEqual(disconnected, 0)
        XCTAssertEqual(completions, 1)
        cancel.central.disconnect(from: cancel.peripheral.identifier) { completions += 1 }
        XCTAssertEqual(completions, 2)
    }

    func testReentrantCompletionSendsTheNewPayload() throws {
        let reentrant = try WriteFixture()
        try reentrant.submit(1, then: { try! reentrant.submit(2) })
        reentrant.acknowledge()
        XCTAssertEqual(reentrant.sent, [1, 2])
        reentrant.acknowledge()
        XCTAssertEqual(reentrant.completions, ["1:ok", "2:ok"])
    }

    func testAlreadyQueuedWritePrecedesReentrantSubmission() throws {
        let fifo = try WriteFixture()
        try fifo.submit(1, then: { try! fifo.submit(3) })
        try fifo.submit(2)
        XCTAssertEqual(fifo.sent, [1])
        fifo.acknowledge()
        XCTAssertEqual(fifo.sent, [1, 2])
        fifo.acknowledge()
        XCTAssertEqual(fifo.sent, [1, 2, 3])
        fifo.acknowledge()
        XCTAssertEqual(fifo.completions, ["1:ok", "2:ok", "3:ok"])
    }

    func testWriteErrorCompletesActiveTaskAndAdvancesPendingWrite() throws {
        let failure = try WriteFixture()
        try failure.submit(1)
        try failure.submit(2)
        failure.acknowledge(error: PluginError.internalInconcictency(details: "controlled SDK failure"))
        XCTAssertEqual(failure.completions, ["1:error"])
        XCTAssertEqual(failure.sent, [1, 2])
        failure.acknowledge()
        XCTAssertEqual(failure.completions, ["1:error", "2:ok"])
    }
}

@main
struct CharacteristicWriteProbeTestRunner {
    static func main() {
        let suite = CharacteristicWriteProbeTests.defaultTestSuite
        suite.run()
        guard let result = suite.testRun, result.executionCount == 16, result.hasSucceeded else {
            exit(1)
        }
    }
}
