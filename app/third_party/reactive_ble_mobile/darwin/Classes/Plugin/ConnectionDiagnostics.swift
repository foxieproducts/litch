import Foundation

/// Observations only: native_attempt is the current request context, not a
/// CoreBluetooth callback epoch. UUIDs remain private filtering metadata.
final class ConnectionDiagnostics {
    private struct Record {
        let peripheralId: UUID?
        let fields: [String: Any]
    }

    private static let capacity = 32
    private static let reserved = Set(["seq", "native_ms", "native_lifetime", "native_attempt", "phase", "drops"])
    private let lock = NSLock()
    private let now: () -> TimeInterval
    private let createdAt: TimeInterval
    private let nativeLifetime = UUID().uuidString
    private var nativeAttempt = 0
    private var sequence = 0
    private var drops = 0
    private var records = [Record]()

    init(now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.now = now
        createdAt = now()
    }

    func beginConnection(_ id: UUID) {
        lock.lock()
        defer { lock.unlock() }
        nativeAttempt += 1
        append("connect_requested", peripheralId: id, fields: [:])
    }

    func record(_ phase: String, peripheralId: UUID? = nil, fields: [String: Any] = [:]) {
        lock.lock()
        defer { lock.unlock() }
        append(phase, peripheralId: peripheralId, fields: fields)
    }

    func snapshot(for id: UUID) -> [[String: Any]] {
        lock.lock()
        defer { lock.unlock() }
        return records.filter { $0.peripheralId == nil || $0.peripheralId == id }.map {
            var fields = $0.fields
            fields["drops"] = drops
            return fields
        }
    }

    private func append(_ phase: String, peripheralId: UUID?, fields: [String: Any]) {
        guard Self.safeName(phase, maximum: 64) else { return }
        sequence += 1
        var exported: [String: Any] = [
            "seq": sequence,
            "native_ms": max(0, (now() - createdAt) * 1000),
            "native_lifetime": nativeLifetime,
            "native_attempt": nativeAttempt,
            "phase": phase,
        ]
        // Bound each record as well as the ring. No strings, payloads, nested
        // objects or identifying keys supplied by callers can cross export.
        for key in fields.keys.sorted().prefix(12) {
            guard Self.safeName(key, maximum: 32), !Self.reserved.contains(key),
                !["uuid", "device", "payload", "error_text"].contains(where: key.contains),
                let value = fields[key] as? NSNumber, value.doubleValue.isFinite
            else { continue }
            exported[key] = value
        }
        if records.count == Self.capacity {
            records.removeFirst()
            drops += 1
        }
        records.append(Record(peripheralId: peripheralId, fields: exported))
    }

    private static func safeName(_ value: String, maximum: Int) -> Bool {
        let scalars = value.unicodeScalars
        guard !scalars.isEmpty, scalars.count <= maximum,
            let first = scalars.first, (97...122).contains(first.value)
        else { return false }
        return scalars.allSatisfy {
            (97...122).contains($0.value) || (48...57).contains($0.value) || $0.value == 95
        }
    }
}
