import CoreBluetooth

struct ConnectTaskController: PeripheralTaskController {

    typealias TaskSpec = ConnectTaskSpec

    private let task: SubjectTask

    init(_ task: SubjectTask) {
        self.task = task
    }

    func connect(centralManager: CBCentralManager, peripheral: CBPeripheral, diagnostics: ConnectionDiagnostics? = nil) -> SubjectTask {
        guard case .pending = task.state
        else {
            diagnostics?.record("central_connect_skipped", peripheralId: peripheral.identifier)
            assert(false)
            return task
        }

        diagnostics?.record("central_connect_requested", peripheralId: peripheral.identifier)
        centralManager.connect(peripheral)

        return task.with(state: task.state.processing(.connecting))
    }

    func handleConnectionChange(_ connectionChange: ConnectionChange) -> SubjectTask {
        guard case .processing(since: _, .connecting) = task.state
        else {
            assert(false)
            return task
        }

        return task.with(state: task.state.finished(connectionChange))
    }

    func cancel(centralManager: CBCentralManager, peripheral: CBPeripheral, error: Error?) -> SubjectTask {
        switch task.state {
        case .pending:
            return task.with(state: task.state.finished(.failedToConnect(error)))
        case .processing(since: _, .connecting):
            centralManager.cancelPeripheralConnection(peripheral)
            return task.with(state: task.state.finished(.failedToConnect(error)))
        case .finished:
            assert(false)
            return task
        }
    }
}
