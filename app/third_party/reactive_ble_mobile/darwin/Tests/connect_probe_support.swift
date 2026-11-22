import Foundation

// The pod's Objective-C umbrella imports Foundation for source files without
// imports. Keep the actual Foundation types in this standalone Swift build.
typealias Date = Foundation.Date
typealias TimeInterval = Foundation.TimeInterval

// Central owns these scalar aliases. Compiling the whole Central would require
// unrelated GATT owners; these aliases add no connection or registry behavior.
typealias PeripheralID = UUID
typealias AdvertisementData = [String: Any]
typealias RSSI = Int

// PeripheralTaskController's unused error helper depends on Flutter/protobuf.
// Preserve its one required error shape without importing those SDKs here.
enum PluginError: Error {
    case internalInconcictency(details: String?)
}
