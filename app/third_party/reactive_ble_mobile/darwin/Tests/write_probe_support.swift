import Foundation
// Shape-only stubs for unused Flutter/protobuf dependencies. All Central,
// characteristic addressing, delegate, registry, and task behavior is real.
typealias UUID = Foundation.UUID
typealias Data = Foundation.Data
typealias Date = Foundation.Date
typealias TimeInterval = Foundation.TimeInterval
enum PluginError: Error {
    case internalInconcictency(details: String?)
    case connectionLost
    case invalidMethodCall(method: String, details: String)
}
struct CharacteristicAddress {
    var hasCharacteristicUuid = false
    var hasServiceUuid = false
    var deviceID = ""
    var characteristicUuid = UUIDValue()
    var serviceUuid = UUIDValue()
    var characteristicInstanceID = "0"
    var serviceInstanceID = "0"
    struct UUIDValue { var data = Data() }
}
