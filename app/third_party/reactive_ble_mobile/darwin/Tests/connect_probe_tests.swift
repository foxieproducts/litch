import CoreBluetooth
import XCTest

final class ConnectProbeTests: XCTestCase {
    private typealias Task = PeripheralTask<ConnectTaskSpec>

    private final class Fixture {
        let id = UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
        let centralManager = CBCentralManager()
        let diagnostics = ConnectionDiagnostics(now: { 0 })
        lazy var peripheral = CBPeripheral(identifier: id)

        var phases: [String] {
            return diagnostics.snapshot(for: id).map { $0["phase"] as! String }
        }

        func pendingTask() -> Task {
            return Task(key: id, params: .init(), timeout: nil, completion: { _ in })
        }

        func connect(_ task: Task) -> Task {
            return ConnectTaskController(task).connect(
                centralManager: centralManager,
                peripheral: peripheral,
                diagnostics: diagnostics
            )
        }
    }

    func testPendingTaskRecordsExactlyOneActualConnectRequest() {
        let f = Fixture()
        let updatedTask = f.connect(f.pendingTask())

        XCTAssertEqual(f.centralManager.connectCount, 1)
        XCTAssertEqual(f.centralManager.cancelCount, 0)
        XCTAssertEqual(f.phases, ["central_connect_requested"])
        guard case .processing(since: _, .connecting) = updatedTask.state else {
            XCTFail("The actual task owner must enter connecting after the API call")
            return
        }
    }

    func testProcessingAndFinishedTasksSkipWithoutClaimingAnAPIRequest() {
        for finished in [false, true] {
            let f = Fixture()
            let pendingTask = f.pendingTask()
            let task = pendingTask.with(state: finished
                ? pendingTask.state.finished(.connected)
                : pendingTask.state.processing(.connecting))
            let updatedTask = f.connect(task)

            XCTAssertEqual(f.centralManager.connectCount, 0)
            XCTAssertEqual(f.centralManager.cancelCount, 0)
            XCTAssertEqual(f.phases, ["central_connect_skipped"])
            switch (task.state, updatedTask.state) {
            case let (.processing(since: before, .connecting), .processing(since: after, .connecting)):
                XCTAssertEqual(after, before)
            case let (.finished(in: before, .connected), .finished(in: after, .connected)):
                XCTAssertEqual(after, before)
            default:
                XCTFail("Declining an API request must preserve the actual task state")
            }
        }
    }

    func testSameUUIDRetrySelectsProcessingTaskThenRecoversAfterCompletion() {
        let f = Fixture()
        let registry = PeripheralTaskRegistry<ConnectTaskController>()
        var completions = [String]()
        registry.registerTask(key: f.id, params: .init()) { _ in completions.append("old") }
        registry.updateTask(key: f.id) {
            $0.connect(centralManager: f.centralManager, peripheral: f.peripheral, diagnostics: f.diagnostics)
        }
        XCTAssertEqual(f.centralManager.connectCount, 1)
        XCTAssertEqual(f.phases, ["central_connect_requested"])

        // A new same-UUID task is appended while the prior one is connecting.
        // Execute production first-match selection and its real pending guard.
        registry.registerTask(key: f.id, params: .init()) { _ in completions.append("retry") }
        registry.updateTask(key: f.id) {
            $0.connect(centralManager: f.centralManager, peripheral: f.peripheral, diagnostics: f.diagnostics)
        }
        XCTAssertEqual(f.centralManager.connectCount, 1)
        XCTAssertEqual(f.phases, ["central_connect_requested", "central_connect_skipped"])
        XCTAssertTrue(completions.isEmpty)
        XCTAssertTrue(registry.hasTask(key: f.id))

        // Feed completion directly to the actual old owner, then the registry
        // selects the retained pending retry. SDK callback dispatch is bypassed.
        registry.updateTask(key: f.id) { $0.handleConnectionChange(.connected) }
        XCTAssertEqual(completions, ["old"])
        XCTAssertTrue(registry.hasTask(key: f.id))
        registry.updateTask(key: f.id) {
            $0.connect(centralManager: f.centralManager, peripheral: f.peripheral, diagnostics: f.diagnostics)
        }
        XCTAssertEqual(f.centralManager.connectCount, 2)
        XCTAssertEqual(f.phases, ["central_connect_requested", "central_connect_skipped", "central_connect_requested"])
        registry.updateTask(key: f.id) { $0.handleConnectionChange(.connected) }
        XCTAssertEqual(completions, ["old", "retry"])
        XCTAssertFalse(registry.hasTask(key: f.id))
        XCTAssertEqual(f.centralManager.cancelCount, 0)
    }
}

@main
struct ConnectProbeTestRunner {
    static func main() {
        guard !_isDebugAssertConfiguration() else {
            fatalError("Compile with -O: the production non-pending assertion must return in release mode")
        }
        let suite = ConnectProbeTests.defaultTestSuite
        suite.run()
        guard let result = suite.testRun, result.executionCount > 0, result.hasSucceeded else {
            exit(1)
        }
    }
}
