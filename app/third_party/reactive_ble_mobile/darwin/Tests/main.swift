import Foundation
import XCTest

final class WriteAdmissionTests: XCTestCase {
    private final class Fixture {
        let peripheral = UUID()
        var now: TimeInterval = 0
        var credits = 0
        var writes = [Int]()
        var completions = [Int: [Error?]]()
        var timers = [() -> Void]()
        var cancelledTimers = Set<Int>()
        lazy var admission = WriteWithoutResponseAdmission(
            now: { self.now },
            scheduleTimeout: { duration, action in
                XCTAssertEqual(duration, 5)
                let index = self.timers.count
                self.timers.append(action)
                return { self.cancelledTimers.insert(index) }
            }
        )

        func connect() {
            admission.beginConnection(peripheral)
            admission.connected(peripheral)
        }

        func submit(_ packet: Int, then: (() -> Void)? = nil) {
            admission.submit(peripheral, canSend: { self.credits > 0 }, send: {
                XCTAssertGreaterThan(self.credits, 0, "Never write into a full native buffer")
                self.credits -= 1
                self.writes.append(packet)
            }, completion: { error in
                self.completions[packet, default: []].append(error)
                if error == nil { then?() }
            })
        }

        func assertSuccess(_ packet: Int, file: StaticString = #file, line: UInt = #line) {
            XCTAssertEqual(completions[packet]?.count, 1, file: file, line: line)
            XCTAssertNil(completions[packet]?.first!, file: file, line: line)
        }

        func assertFailure(_ packet: Int, _ expected: WriteWithoutResponseAdmission.Failure,
                           file: StaticString = #file, line: UInt = #line) {
            XCTAssertEqual(completions[packet]?.count, 1, file: file, line: line)
            let failure = completions[packet]?.first! as? WriteWithoutResponseAdmission.Failure
            XCTAssertEqual(failure, expected, file: file, line: line)
        }
    }

    func testFullBufferDefersCompletionUntilOneDispatch() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [])
        XCTAssertNil(f.completions[1])
        f.credits = 1
        f.admission.ready(f.peripheral)
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1])
        f.assertSuccess(1)
        XCTAssertEqual(f.cancelledTimers, [0])
        f.timers[0]() // Even an already enqueued timeout cannot complete twice.
        f.assertSuccess(1)
    }

    func testBoundedSlotRejectsConcurrentWriteWithoutDisplacingPacket() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.submit(2)
        f.assertFailure(2, .busy)
        XCTAssertEqual(f.timers.count, 1)
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1])
        f.assertSuccess(1)
    }

    func testAwaitedChunksKeepCheckpointAfterLastNativeAdmission() {
        let f = Fixture()
        f.connect()
        var events = [String]()
        f.credits = 1
        f.submit(1) {
            events.append("chunk 1 admitted")
            f.submit(2) {
                events.append("chunk 2 admitted")
                events.append("checkpoint with response")
            }
        }
        XCTAssertEqual(f.writes, [1])
        XCTAssertEqual(events, ["chunk 1 admitted"])
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1, 2])
        XCTAssertEqual(events, ["chunk 1 admitted", "chunk 2 admitted", "checkpoint with response"])
        f.assertSuccess(1)
        f.assertSuccess(2)
    }

    func testBatchDrainsAvailableCreditsWithoutAnotherPlatformCall() {
        let f = Fixture()
        f.connect()
        f.credits = 3
        var completionCount = 0
        f.admission.submitBatch(
            f.peripheral,
            values: [Data([1]), Data([2]), Data([3])],
            canSend: { f.credits > 0 },
            send: { value in
                XCTAssertGreaterThan(f.credits, 0)
                f.credits -= 1
                f.writes.append(Int(value[0]))
            },
            completion: { error in
                XCTAssertNil(error)
                completionCount += 1
            }
        )
        XCTAssertEqual(f.writes, [1, 2, 3])
        XCTAssertEqual(completionCount, 1)
        XCTAssertEqual(f.credits, 0)
    }

    func testBatchResumesOnLaterNativeCreditAndKeepsOneDeadline() {
        let f = Fixture()
        f.connect()
        f.credits = 1
        var completionCount = 0
        f.admission.submitBatch(
            f.peripheral,
            values: [Data([1]), Data([2])],
            canSend: { f.credits > 0 },
            send: { value in
                XCTAssertGreaterThan(f.credits, 0)
                f.credits -= 1
                f.writes.append(Int(value[0]))
            },
            completion: { error in
                XCTAssertNil(error)
                completionCount += 1
            }
        )
        XCTAssertEqual(f.writes, [1])
        XCTAssertEqual(completionCount, 0)
        XCTAssertEqual(f.timers.count, 1)

        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1, 2])
        XCTAssertEqual(completionCount, 1)
    }

    func testControlPreemptsRetainedBatchAtNativeReadinessBoundary() {
        let f = Fixture()
        f.connect()
        f.credits = 1
        var batchCompleted = false
        f.admission.submitBatch(f.peripheral, values: [Data([1]), Data([2])],
            canSend: { f.credits > 0 }, send: { value in
                f.credits -= 1
                f.writes.append(Int(value[0]))
            }, completion: { error in
                XCTAssertNil(error)
                batchCompleted = true
            })
        f.submit(9)
        XCTAssertEqual(f.writes, [1])
        XCTAssertNil(f.completions[9])
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1, 9])
        f.assertSuccess(9)
        XCTAssertFalse(batchCompleted)
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [1, 9, 2])
        XCTAssertTrue(batchCompleted)
    }

    func testDisconnectCancelsExactlyOnceAndLateConnectedCannotReopen() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.admission.cancel(f.peripheral, error: WriteWithoutResponseAdmission.Failure.disconnected)
        f.admission.cancel(f.peripheral, error: WriteWithoutResponseAdmission.Failure.disconnected)
        f.credits = 4
        f.admission.ready(f.peripheral)
        f.admission.connected(f.peripheral)
        f.submit(2)
        f.timers[0]()
        XCTAssertEqual(f.writes, [])
        f.assertFailure(1, .disconnected)
        f.assertFailure(2, .disconnected)
    }

    func testSupersededGenerationCannotDispatchOrTimeoutNewPacket() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.admission.beginConnection(f.peripheral)
        f.submit(2)
        f.assertFailure(1, .disconnected)
        f.assertFailure(2, .disconnected)
        f.admission.connected(f.peripheral)
        f.submit(3)
        f.admission.connected(f.peripheral) // duplicate physical Connected
        f.timers[0]() // stale generation deadline
        f.admission.ready(f.peripheral) // stale ready event has no native credit
        XCTAssertEqual(f.writes, [])
        XCTAssertNil(f.completions[3])
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [3])
        f.assertSuccess(3)
    }

    func testDeadlineRetiresPacketAndAllowsSameLinkRecovery() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.now = 5
        f.timers[0]()
        f.assertFailure(1, .timedOut)
        f.submit(2)
        f.timers[0]() // cannot expire the replacement packet
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [2])
        f.assertSuccess(2)
    }

    func testOverdueReadinessCannotRaceDelayedDeadlineCallback() {
        let f = Fixture()
        f.connect()
        f.submit(1)
        f.now = 6
        f.credits = 1
        f.admission.ready(f.peripheral)
        XCTAssertEqual(f.writes, [])
        f.assertFailure(1, .timedOut)
        f.timers[0]()
        f.assertFailure(1, .timedOut)
    }
}

final class ConnectionDiagnosticsTests: XCTestCase {
    func testMonotonicTimeLifetimeAndRequestContext() {
        var now: TimeInterval = 100
        let diagnostics = ConnectionDiagnostics(now: { now })
        let id = UUID()
        diagnostics.beginConnection(id)
        now += 0.25
        diagnostics.record("did_connect", peripheralId: id)
        diagnostics.beginConnection(id)
        diagnostics.record("did_disconnect", peripheralId: id)
        let records = diagnostics.snapshot(for: id)
        XCTAssertEqual(records.map { $0["seq"] as! Int }, [1, 2, 3, 4])
        XCTAssertEqual(records.map { $0["native_attempt"] as! Int }, [1, 1, 2, 2])
        XCTAssertEqual(records[1]["native_ms"] as? Double, 250)
        let lifetime = records[0]["native_lifetime"] as! String
        XCTAssertNotNil(UUID(uuidString: lifetime))
        XCTAssertEqual(Set(records.map { $0["native_lifetime"] as! String }), [lifetime])
        let other = ConnectionDiagnostics()
        other.beginConnection(id)
        XCTAssertNotEqual(other.snapshot(for: id)[0]["native_lifetime"] as? String, lifetime)
        // The late callback carries current request context, never its origin.
        XCTAssertEqual(records[3]["phase"] as? String, "did_disconnect")
    }

    func testBoundedRetentionAndCumulativeDropsAcrossManyPeripherals() {
        let diagnostics = ConnectionDiagnostics()
        let target = UUID()
        for _ in 0..<40 { diagnostics.beginConnection(UUID()) }
        diagnostics.beginConnection(target)
        for _ in 0..<32 { diagnostics.record("services_callback", peripheralId: target) }
        let records = diagnostics.snapshot(for: target)
        XCTAssertEqual(records.count, 32)
        XCTAssertEqual(records.first?["seq"] as? Int, 42)
        XCTAssertEqual(records.last?["seq"] as? Int, 73)
        XCTAssertTrue(records.allSatisfy { ($0["drops"] as? Int) == 41 })
    }

    func testSnapshotIncludesGlobalSinkEventsButFiltersOtherDevice() {
        let diagnostics = ConnectionDiagnostics()
        let first = UUID(), second = UUID()
        diagnostics.record("sink_listen")
        diagnostics.beginConnection(first)
        diagnostics.beginConnection(second)
        diagnostics.record("sink_cancel")
        let firstRecords = diagnostics.snapshot(for: first)
        XCTAssertEqual(firstRecords.map { $0["phase"] as! String }, ["sink_listen", "connect_requested", "sink_cancel"])
        XCTAssertEqual(diagnostics.snapshot(for: UUID()).count, 2)
        XCTAssertEqual(firstRecords[1]["native_attempt"] as? Int, 1)
        XCTAssertEqual(diagnostics.snapshot(for: second)[1]["native_attempt"] as? Int, 2)
    }

    func testPerRecordFieldBudgetAndInvalidNames() {
        let diagnostics = ConnectionDiagnostics()
        let id = UUID()
        var fields = [String: Any]()
        for index in 0..<50 { fields["field_\(index)"] = index }
        diagnostics.record("services_callback", peripheralId: id, fields: fields)
        diagnostics.record("invalid phase", peripheralId: id)
        let records = diagnostics.snapshot(for: id)
        XCTAssertEqual(records.count, 1)
        XCTAssertEqual(records[0].count, 18, "Six fixed fields plus at most twelve caller fields")
        XCTAssertEqual(records[0]["native_attempt"] as? Int, 0)
    }

    func testPrivacyNumericFieldsAndReadOnlyExport() throws {
        let diagnostics = ConnectionDiagnostics()
        let id = UUID()
        diagnostics.beginConnection(id)
        diagnostics.record("services_callback", peripheralId: id, fields: [
            "has_error": true, "error_code": 531, "service_count": 3,
            "device_uuid": 123, "payload": 456, "error_text": "private failure",
            "private_string": id.uuidString, "nested": ["secret": 1],
            "infinite": Double.infinity, "seq": 999, "native_attempt": 999,
        ])
        diagnostics.record(id.uuidString, peripheralId: id)
        let records = diagnostics.snapshot(for: id)
        XCTAssertEqual(records.count, 2)
        XCTAssertEqual(records[1]["has_error"] as? Bool, true)
        XCTAssertEqual(records[1]["error_code"] as? Int, 531)
        XCTAssertEqual(records[1]["service_count"] as? Int, 3)
        XCTAssertEqual(records[1]["seq"] as? Int, 2)
        XCTAssertEqual(records[1]["native_attempt"] as? Int, 1)
        for key in ["device_uuid", "payload", "error_text", "private_string", "nested", "infinite"] {
            XCTAssertNil(records[1][key])
        }
        let export = try JSONSerialization.data(withJSONObject: records, options: [.sortedKeys])
        XCTAssertFalse(String(data: export, encoding: .utf8)!.contains(id.uuidString))
        let repeated = try JSONSerialization.data(withJSONObject: diagnostics.snapshot(for: id), options: [.sortedKeys])
        XCTAssertEqual(export, repeated, "Snapshot must not add records or change request ownership")
    }
}

for suite in [WriteAdmissionTests.defaultTestSuite, ConnectionDiagnosticsTests.defaultTestSuite] {
    suite.run()
    guard let result = suite.testRun, result.executionCount > 0, result.hasSucceeded else {
        exit(1)
    }
}
