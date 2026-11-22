// Test-only SDK identifiers, scalar state, graph references and API counts.
// No task guards, registry selection, callbacks, timers or admission behavior.
@_exported import Foundation
public enum CBManagerState: Int { case unknown = 0, poweredOn = 5 }
public enum CBPeripheralState: Int { case disconnected, connecting, connected, disconnecting }
public enum CBCharacteristicWriteType { case withResponse, withoutResponse }
public struct CBCharacteristicProperties: OptionSet {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let read = Self(rawValue: 1)
    public static let write = Self(rawValue: 2)
    public static let writeWithoutResponse = Self(rawValue: 4)
    public static let notify = Self(rawValue: 8)
    public static let notifyEncryptionRequired = Self(rawValue: 16)
    public static let indicate = Self(rawValue: 32)
    public static let indicateEncryptionRequired = Self(rawValue: 64)
}
public final class CBUUID: NSObject {
    let label: String
    public init(string: String) { label = string }
    public init(data: Data) { label = data.map { String(format: "%02x", $0) }.joined() }
    public override var hash: Int { label.hashValue }
    public override func isEqual(_ object: Any?) -> Bool { (object as? CBUUID)?.label == label }
    public override var description: String { label }
}
public final class CBCharacteristic: NSObject {
    public let uuid: CBUUID
    public let properties: CBCharacteristicProperties
    public weak var service: CBService?
    public var value: Data?
    public init(uuid: CBUUID, properties: CBCharacteristicProperties) { self.uuid = uuid; self.properties = properties }
}
public final class CBService: NSObject {
    public let uuid: CBUUID
    public weak var peripheral: CBPeripheral?
    public var characteristics: [CBCharacteristic]?
    public init(uuid: CBUUID) { self.uuid = uuid }
}
public protocol CBPeripheralDelegate: AnyObject {
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?)
}
public protocol CBCentralManagerDelegate: AnyObject {
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral)
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?)
}
public struct WriteRequest {
    public let value: Data
    public let characteristic: CBCharacteristic
    public let type: CBCharacteristicWriteType
}
public final class CBPeripheral {
    public let identifier: UUID
    public var state: CBPeripheralState = .connected
    public var services: [CBService]?
    public weak var delegate: CBPeripheralDelegate?
    public var canSendWriteWithoutResponse = true
    public private(set) var writes = [WriteRequest]()
    public var onWrite: ((WriteRequest) -> Void)?
    public init(identifier: UUID) { self.identifier = identifier }
    public func writeValue(_ value: Data, for characteristic: CBCharacteristic, type: CBCharacteristicWriteType) {
        let request = WriteRequest(value: value, characteristic: characteristic, type: type)
        writes.append(request)
        onWrite?(request)
    }
    public func readValue(for characteristic: CBCharacteristic) {}
    public func setNotifyValue(_ value: Bool, for characteristic: CBCharacteristic) {}
    public func discoverServices(_ services: [CBUUID]?) {}
    public func discoverCharacteristics(_ characteristics: [CBUUID]?, for service: CBService) {}
    public func readRSSI() {}
    public func maximumWriteValueLength(for type: CBCharacteristicWriteType) -> Int { 512 }
}
public let CBCentralManagerScanOptionAllowDuplicatesKey = "allowDuplicates"
public final class CBCentralManager {
    public static var knownPeripherals = [CBPeripheral]()
    public static var last: CBCentralManager?
    public var state: CBManagerState = .poweredOn
    public private(set) var connectCount = 0
    public private(set) var cancelCount = 0
    public weak var delegate: CBCentralManagerDelegate?
    public init(delegate: CBCentralManagerDelegate? = nil, queue: DispatchQueue? = nil) { self.delegate = delegate; Self.last = self }
    public func connect(_ peripheral: CBPeripheral) { connectCount += 1 }
    public func cancelPeripheralConnection(_ peripheral: CBPeripheral) { cancelCount += 1 }
    public func retrievePeripherals(withIdentifiers ids: [UUID]) -> [CBPeripheral] { Self.knownPeripherals.filter { ids.contains($0.identifier) } }
    public func scanForPeripherals(withServices: [CBUUID]?, options: [String: Any]?) {}
    public func stopScan() {}
}
