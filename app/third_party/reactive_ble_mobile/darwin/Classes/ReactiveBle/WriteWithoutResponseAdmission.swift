import Foundation

// Central and its CoreBluetooth callbacks use the main queue. Keep the pending
// writes in that same owner: one reserved control and one bounded batch.
// Dispatch one packet at actual readiness, choosing control before batch.
final class WriteWithoutResponseAdmission {
    typealias CancelTimeout = () -> Void
    typealias ScheduleTimeout = (TimeInterval, @escaping () -> Void) -> CancelTimeout

    enum Failure: Error, CustomStringConvertible {
        case disconnected
        case busy
        case timedOut

        var description: String {
            switch self {
            case .disconnected:
                return "The write without response connection has ended"
            case .busy:
                return "A write without response is already awaiting native admission"
            case .timedOut:
                return "Timed out waiting for native write without response capacity"
            }
        }
    }

    // Fail before Litchcraft's six-second Dart write deadline. This is an
    // admission deadline, not a pacing delay or a board acknowledgement.
    static let timeout: TimeInterval = 5

    private final class Connection {
        var isConnected = false
        var dispatching = false
        var control: PendingWrite?
        var batch: PendingWrite?
    }

    private final class PendingWrite {
        let token = UUID()
        var isBatch = false
        let deadline: TimeInterval
        let canSend: () -> Bool
        let sendNext: () -> Void
        let isComplete: () -> Bool
        let completion: (Error?) -> Void
        var cancelTimeout: CancelTimeout = {}

        init(deadline: TimeInterval, canSend: @escaping () -> Bool,
             sendNext: @escaping () -> Void, isComplete: @escaping () -> Bool,
             completion: @escaping (Error?) -> Void) {
            self.deadline = deadline
            self.canSend = canSend
            self.sendNext = sendNext
            self.isComplete = isComplete
            self.completion = completion
        }
    }

    private var connections = [UUID: Connection]()
    private let now: () -> TimeInterval
    private let scheduleTimeout: ScheduleTimeout

    init(
        now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
        scheduleTimeout: @escaping ScheduleTimeout = { duration, action in
            let timer = Timer(timeInterval: duration, repeats: false) { _ in action() }
            RunLoop.main.add(timer, forMode: .common)
            return { timer.invalidate() }
        }
    ) {
        self.now = now
        self.scheduleTimeout = scheduleTimeout
    }

    func beginConnection(_ peripheralID: UUID) {
        cancel(peripheralID, error: Failure.disconnected)
        connections[peripheralID] = Connection()
    }

    func connected(_ peripheralID: UUID) {
        // Repeated Connected notifications do not supersede the live session.
        // A late notification after deliberate cancellation cannot reopen it.
        connections[peripheralID]?.isConnected = true
    }

    func cancel(_ peripheralID: UUID, error: Error) {
        guard let connection = connections.removeValue(forKey: peripheralID) else { return }
        let pending = [connection.control, connection.batch].compactMap { $0 }
        connection.control = nil
        connection.batch = nil
        for write in pending {
            write.cancelTimeout()
            write.completion(error)
        }
    }

    func submit(_ peripheralID: UUID, canSend: @escaping () -> Bool,
                send: @escaping () -> Void, completion: @escaping (Error?) -> Void) {
        let pending = PendingWrite(
            deadline: now() + Self.timeout,
            canSend: canSend,
            sendNext: send,
            isComplete: { true },
            completion: completion
        )
        submit(pending, peripheralID: peripheralID)
    }

    // CoreBluetooth exposes write-without-response admission one packet at a
    // time. Keep a batch in this same owner so Dart crosses the platform
    // boundary once while native admission still respects canSendWriteWithoutResponse.
    // The batch is deliberately bounded by the caller (currently 54 packets).
    func submitBatch(_ peripheralID: UUID, values: [Data],
                     canSend: @escaping () -> Bool,
                     send: @escaping (Data) -> Void,
                     completion: @escaping (Error?) -> Void) {
        guard !values.isEmpty else {
            completion(nil)
            return
        }

        var nextIndex = 0
        let pending = PendingWrite(
            deadline: now() + Self.timeout,
            canSend: canSend,
            sendNext: {
                send(values[nextIndex])
                nextIndex += 1
            },
            isComplete: { nextIndex == values.count },
            completion: completion
        )
        pending.isBatch = true
        submit(pending, peripheralID: peripheralID)
    }

    private func owns(_ connection: Connection, _ pending: PendingWrite) -> Bool {
        return pending.isBatch ? connection.batch === pending : connection.control === pending
    }

    private func submit(_ pending: PendingWrite, peripheralID: UUID) {
        guard let connection = connections[peripheralID], connection.isConnected else {
            pending.completion(Failure.disconnected)
            return
        }
        guard (pending.isBatch ? connection.batch : connection.control) == nil else {
            pending.completion(Failure.busy)
            return
        }
        if pending.isBatch { connection.batch = pending } else { connection.control = pending }
        ready(peripheralID)
        guard owns(connection, pending) else { return }
        pending.cancelTimeout = scheduleTimeout(Self.timeout) { [weak self] in
            guard let owner = self,
                  let live = owner.connections[peripheralID], owner.owns(live, pending) else { return }
            owner.finish(peripheralID, pending: pending, error: Failure.timedOut)
        }
    }

    func ready(_ peripheralID: UUID) {
        guard let connection = connections[peripheralID], !connection.dispatching,
              let pending = connection.control ?? connection.batch else { return }
        guard now() < pending.deadline else {
            finish(peripheralID, pending: pending, error: Failure.timedOut)
            return
        }
        guard pending.canSend() else { return }
        finish(peripheralID, pending: pending, error: nil)
    }

    private func finish(_ peripheralID: UUID, pending: PendingWrite, error: Error?) {
        guard let connection = connections[peripheralID], owns(connection, pending) else { return }
        if error == nil {
            // Every packet consumes actual CoreBluetooth readiness. Re-entrant
            // control submissions get the next opening before the bulk suffix.
            connection.dispatching = true
            pending.sendNext()
            connection.dispatching = false
            guard connections[peripheralID] === connection, owns(connection, pending) else { return }
        }
        if error != nil || pending.isComplete() {
            pending.cancelTimeout()
            if pending.isBatch { connection.batch = nil } else { connection.control = nil }
            pending.completion(error)
        }
        ready(peripheralID)
    }
}
