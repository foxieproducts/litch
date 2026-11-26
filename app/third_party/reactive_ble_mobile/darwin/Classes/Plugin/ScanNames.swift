import Foundation

// Keep a received local name when later packets from the same scan omit it.
final class ScanNames {
    private var advertisedNames: [String: String] = [:]

    func resolve(id: String, advertised: String?, cached: @autoclosure () -> String?) -> String {
        let fresh = advertised?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let fresh = fresh, !fresh.isEmpty {
            advertisedNames[id] = fresh
            return fresh
        }
        return advertisedNames[id] ?? cached()?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }
}
