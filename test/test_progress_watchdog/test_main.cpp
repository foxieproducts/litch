#include <cstdio>
#include <cstring>
#include <type_traits>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "foxie_embedded/console.hpp"
#include "firmware/hybrid_console_transport.hpp"
#include "firmware/firmware_console_logger_adapter.hpp"
#include "runtime/progress_watchdog.hpp"
#include "modules/board_health_notification.hpp"
namespace { int failures=0; void Check(bool ok,const char* message) {
 if(!ok){++failures;std::printf("[FAIL] watchdog: %s\n",message);}
}
struct UnfinishedOperation {};
unsigned diagnosticFeeds = 0U;
unsigned failPauseNumber = 0U;
std::vector<uint32_t> diagnosticPauses;
void FeedDiagnostic() { ++diagnosticFeeds; }
void PauseDiagnostic(const uint32_t milliseconds) {
    diagnosticPauses.push_back(milliseconds);
    if (diagnosticPauses.size() == failPauseNumber) {
        throw UnfinishedOperation{};
    }
}
void TestDiagnosticProgress() {
    diagnosticFeeds = 0U;
    failPauseNumber = 0U;
    diagnosticPauses.clear();
    unsigned iterations = 0U;
    runtime::RunDiagnosticIteration([&]() {
        Check(diagnosticFeeds == 0U, "diagnostic work is not fed before completion");
        ++iterations;
    }, 65535U, PauseDiagnostic, FeedDiagnostic);
    uint32_t totalPause = 0U;
    for (const auto slice : diagnosticPauses) {
        Check(slice > 0U && slice <= 1000U, "deliberate waits are bounded slices");
        totalPause += slice;
    }
    Check(iterations == 1U && totalPause == 65535U &&
          diagnosticPauses.size() == 66U && diagnosticFeeds == 67U,
          "maximum existing diagnostic delay preserves duration and feeds completed slices");

    diagnosticFeeds = 0U;
    diagnosticPauses.clear();
    for (unsigned i = 0U; i < 50U; ++i) {
        runtime::RunDiagnosticIteration([&]() { ++iterations; }, 1000U,
                                        PauseDiagnostic, FeedDiagnostic);
    }
    Check(iterations == 51U && diagnosticPauses.size() == 50U &&
          diagnosticFeeds == 100U && 50000U > runtime::WATCHDOG_TIMEOUT_SECONDS * 1000U,
          "50-second progressing diagnostic feeds each completed iteration and wait");

    diagnosticFeeds = 0U;
    diagnosticPauses.clear();
    runtime::RunDiagnosticIteration([]() {}, 0U, PauseDiagnostic, FeedDiagnostic);
    Check(diagnosticPauses == std::vector<uint32_t>{1U} && diagnosticFeeds == 2U,
          "zero requested delay still yields for idle and BLE supervision");

    diagnosticFeeds = 0U;
    diagnosticPauses.clear();
    try {
        runtime::RunDiagnosticIteration([]() {
            Check(diagnosticFeeds == 0U, "no independent feed runs inside blocked I/O");
            throw UnfinishedOperation{};
        }, 65535U, PauseDiagnostic, FeedDiagnostic);
        Check(false, "unfinished I/O cannot reach its progress boundary");
    } catch (const UnfinishedOperation&) {}
    Check(diagnosticFeeds == 0U && diagnosticPauses.empty(),
          "unfinished I/O neither feeds nor begins a deliberate wait");

    failPauseNumber = 2U;
    try {
        runtime::RunDiagnosticIteration([]() {}, 2500U,
                                        PauseDiagnostic, FeedDiagnostic);
        Check(false, "unfinished delay cannot reach its progress boundary");
    } catch (const UnfinishedOperation&) {}
    Check(diagnosticFeeds == 2U && diagnosticPauses.size() == 2U,
          "blocked second delay has only work and first-delay completion feeds");
    failPauseNumber = 0U;
    std::puts("[PLAYBACK] diagnostic: 50-second progressing run -> completion feeds; delay=65535 -> 66 bounded slices; delay=0 -> one scheduler yield; unfinished I/O -> zero feeds; unfinished delay -> no completion feed. SDK timeout remains HIL.");
}
}
class RecordingTransport final : public fxe::IConsoleTransport {
 public:
    std::string bytes;
    bool TryReadChar(char&) override { return false; }
    void Write(const char* data, size_t size) override { bytes.append(data,size); }
    void Flush() override {}
};
namespace {
class RecordingSerialEndpoint {
 public:
    std::string bytes;
    size_t writeCapacity = std::numeric_limits<size_t>::max();
    size_t maxWriteAcceptance = std::numeric_limits<size_t>::max();
    bool consumeWriteCapacity = false;
    bool connected = true;
    size_t writeCalls = 0U;
    size_t shortWriteCalls = 0U;
    size_t oversizedWriteCalls = 0U;
    size_t flushCalls = 0U;
    int available() const { return 0; }
    int availableForWrite() const {
        const size_t intMax = static_cast<size_t>(std::numeric_limits<int>::max());
        return static_cast<int>(writeCapacity > intMax ? intMax : writeCapacity);
    }
    int read() { return -1; }
    size_t write(const uint8_t* data, size_t size) {
        ++writeCalls;
        if (size > writeCapacity) {
            ++oversizedWriteCalls;
        }
        size_t accepted = size < writeCapacity ? size : writeCapacity;
        if (accepted > maxWriteAcceptance) {
            accepted = maxWriteAcceptance;
        }
        if (accepted < size) {
            ++shortWriteCalls;
        }
        bytes.append(reinterpret_cast<const char*>(data), accepted);
        if (consumeWriteCapacity) {
            writeCapacity -= accepted;
        }
        return accepted;
    }
    void flush() { ++flushCalls; }
    explicit operator bool() const { return connected; }
};
class RecordingBleEndpoint {
 public:
    std::string bytes;
    std::string input;
    size_t inputOffset = 0U;
    bool TryReadConsoleInputChar(char& outChar) {
        if (inputOffset >= input.size()) {
            return false;
        }
        outChar = input[inputOffset++];
        return true;
    }
    void WriteConsoleOutput(const char* data, size_t size) {
        bytes.append(data, size);
    }
};
void TestBackpressuredSerialPreservesBleAndInput(
    const modules::SupportIncidentLine& line) {
    RecordingSerialEndpoint serial;
    serial.writeCapacity = 0U;
    RecordingBleEndpoint ble;
    ble.input = "p";
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    fxe::Console console(transport);

    modules::WriteSupportIncident(console, line);
    char input = '\0';
    const bool inputRead = transport.TryReadChar(input);
    const std::string expected = std::string(line.CStr()) + "\n";
    Check(ble.bytes == expected,
          "zero-capacity serial leaves the complete support record available over BLE");
    Check(serial.writeCalls == 0U && serial.oversizedWriteCalls == 0U,
          "zero-capacity serial is never asked to write or wait");
    Check(serial.flushCalls == 0U,
          "zero-capacity serial is not flushed from the cooperative console path");
    Check(inputRead && input == 'p',
          "BLE console input remains usable while serial has no capacity");
    std::printf(
        "[PLAYBACK] usb-backpressure: support=%zu bytes serial_capacity=0 "
        "write_calls=%zu oversized=%zu flush_calls=%zu ble_complete=%u input=%c.\n",
        line.Size(), serial.writeCalls, serial.oversizedWriteCalls,
        serial.flushCalls, unsigned(ble.bytes == expected), input);
}
void TestSerialQueueBurstDrainOverflowAndDisconnect() {
    const auto writeStartupSupportBurst = [](fxe::Console& console) {
        console.PrintWelcome();
        const std::string retainedLine(255U, 'l');
        for (size_t i = 0U; i < 16U; ++i) {
            console.PrintLn("%s", retainedLine.c_str());
        }
        const std::string maximumSupportPayload(1023U, 's');
        console.BeginBulkOutput();
        console.PrintRaw(maximumSupportPayload.data(),
                         maximumSupportPayload.size());
        console.PrintRaw("\r\n", 2U);
        console.EndBulkOutput();
    };

    RecordingSerialEndpoint referenceSerial;
    RecordingBleEndpoint referenceBle;
    firmware_internal::HybridConsoleTransport referenceTransport(
        referenceSerial, referenceBle);
    fxe::Console referenceConsole(referenceTransport);
    writeStartupSupportBurst(referenceConsole);

    RecordingSerialEndpoint serial;
    serial.writeCapacity = 0U;
    serial.maxWriteAcceptance = 13U;
    serial.consumeWriteCapacity = true;
    RecordingBleEndpoint ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    fxe::Console console(transport);

    writeStartupSupportBurst(console);
    const std::string maximumSupportPayload(1023U, 's');
    Check(transport.DroppedSerialBytes() == 0U &&
              transport.PendingSerialBytes() <= transport.SerialOutputCapacity(),
          "welcome, 16 retained maximum log lines, and maximum support record fit before the next cadence");

    for (size_t i = 0U;
         i < 512U && transport.PendingSerialBytes() != 0U;
         ++i) {
        serial.writeCapacity += 17U;
        char ignored = '\0';
        (void)transport.TryReadChar(ignored);
    }
    Check(transport.PendingSerialBytes() == 0U &&
              transport.DroppedSerialBytes() == 0U &&
              serial.oversizedWriteCalls == 0U &&
              serial.shortWriteCalls != 0U,
          "finite partial endpoint advances by actual short writes without oversized requests");
    Check(serial.bytes.find(maximumSupportPayload + "\r\n") != std::string::npos,
          "healthy partial serial drain preserves the complete maximum support record");
    Check(serial.bytes == referenceSerial.bytes && ble.bytes == referenceBle.bytes,
          "partial drain is byte-exact for the complete welcome, retained logs, and support burst");

    RecordingSerialEndpoint overflowSerial;
    overflowSerial.writeCapacity = 0U;
    RecordingBleEndpoint overflowBle;
    firmware_internal::HybridConsoleTransport overflowTransport(
        overflowSerial, overflowBle);
    const std::string overflow(
        overflowTransport.SerialOutputCapacity() + 19U, 'x');
    overflowTransport.Write(overflow.data(), overflow.size());
    Check(overflowTransport.PendingSerialBytes() ==
                  overflowTransport.SerialOutputCapacity() &&
              overflowTransport.DroppedSerialBytes() == 19U,
          "attached stalled serial keeps the oldest bounded bytes and counts dropped newest bytes");
    overflowSerial.connected = false;
    char ignored = '\0';
    (void)overflowTransport.TryReadChar(ignored);
    const uint32_t droppedAfterDisconnect = overflowTransport.DroppedSerialBytes();
    overflowSerial.connected = true;
    overflowSerial.writeCapacity = 64U;
    overflowTransport.Write("fresh\r\n", 7U);
    Check(overflowSerial.bytes == "fresh\r\n" &&
              overflowTransport.PendingSerialBytes() == 0U &&
              droppedAfterDisconnect ==
                  overflowTransport.SerialOutputCapacity() + 19U,
          "disconnect discards stale queued output before a fresh serial session");
    std::printf(
        "[PLAYBACK] serial-queue: capacity=%zu welcome+logs+support=%zu "
        "partial_writes=%zu exact=%u overflow_drop=19 disconnect_drop=%u "
        "fresh=%zu.\n",
        transport.SerialOutputCapacity(), serial.bytes.size(), serial.writeCalls,
        unsigned(serial.bytes == referenceSerial.bytes &&
                 ble.bytes == referenceBle.bytes),
        droppedAfterDisconnect, overflowSerial.bytes.size());
}

void TestFirmwareLoggerAdapterKeepsBulkRecordsAtomic() {
    RecordingSerialEndpoint serial;
    RecordingBleEndpoint ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    fxe::Console console(transport);
    firmware_internal::FirmwareConsoleLoggerAdapter loggerAdapter;
    const std::string supportPayload(1023U, 'j');

    console.BeginBulkOutput();
    console.PrintRaw(supportPayload.data(), supportPayload.size());
    Check(loggerAdapter.Enqueue("worker-log"),
          "worker logger hands off without entering Console during bulk output");
    Check(ble.bytes.empty(),
          "unterminated bulk record remains untouched by queued worker log");
    console.PrintRaw("\r\n", 2U);
    console.EndBulkOutput();
    Check(loggerAdapter.DrainTo(console) == 1U,
          "cooperative owner drains the queued worker log");
    Check(ble.bytes == supportPayload + "\nworker-log\n",
          "worker log follows the complete support record as a separate line");
    Check(serial.bytes.find(supportPayload + "\r\n") != std::string::npos &&
              serial.bytes.find("worker-log\r\n") != std::string::npos,
          "production logger adapter preserves serial bulk and logger framing");

    for (size_t i = 0U;
         i < loggerAdapter.QUEUED_LINE_CAPACITY;
         ++i) {
        const std::string line = "queued-" + std::to_string(i);
        Check(loggerAdapter.Enqueue(line.c_str()),
              "bounded logger queue accepts its declared live-line capacity");
    }
    Check(!loggerAdapter.Enqueue("dropped-newest") &&
              loggerAdapter.TotalDroppedLines() == 1U,
          "full live logger queue drops newest and counts overflow");
    Check(loggerAdapter.DrainTo(console) ==
                  loggerAdapter.MAX_DRAIN_LINES_PER_UPDATE &&
              loggerAdapter.Size() ==
                  loggerAdapter.QUEUED_LINE_CAPACITY -
                      loggerAdapter.MAX_DRAIN_LINES_PER_UPDATE,
          "logger drain work is capped per cooperative update");
    Check(loggerAdapter.DrainTo(console) ==
                  loggerAdapter.MAX_DRAIN_LINES_PER_UPDATE &&
              loggerAdapter.Size() == 0U &&
              ble.bytes.find("dropped-newest") == std::string::npos &&
              ble.bytes.find("console: omitted 1 live log lines (queue full)\n") !=
                  std::string::npos,
          "logger overflow retains older lines and reports the omitted newest line after draining");

    const std::string traceLine = "{\"type\":\"position_ack\",\"rows\":\"" +
        std::string(850U, 'a') + "\"}";
    const std::string droppedTrace = "{\"type\":\"dropped_trace\",\"rows\":\"" +
        std::string(1600U, 'b') + "\"}";
    ble.bytes.clear();
    const size_t serialOffset = serial.bytes.size();
    Check(loggerAdapter.Enqueue(traceLine.c_str()) &&
              !loggerAdapter.Enqueue(droppedTrace.c_str()) &&
              loggerAdapter.Enqueue("following-short-line") &&
              loggerAdapter.TotalDroppedLines() == 2U,
          "an oversized queued trace is rejected in full while the next short line fits");
    Check(loggerAdapter.DrainTo(console) == 2U && loggerAdapter.Size() == 0U &&
              ble.bytes.find(traceLine + "\nfollowing-short-line\n") == 0U &&
              ble.bytes.find("dropped_trace") == std::string::npos &&
              serial.bytes.find(traceLine + "\r\n", serialOffset) != std::string::npos &&
              serial.bytes.find("dropped_trace", serialOffset) == std::string::npos,
          "fragmented logger handoff transports one exact complete trace record before the next line");
    std::printf(
        "[PLAYBACK] logger-handoff: queued=%zu drain_per_update=%zu "
        "dropped=%u support_atomic=%u.\n",
        loggerAdapter.QUEUED_LINE_CAPACITY,
        loggerAdapter.MAX_DRAIN_LINES_PER_UPDATE,
        loggerAdapter.TotalDroppedLines(),
        unsigned(ble.bytes.find(supportPayload + "\nworker-log\n") == 0U));
}
void TestSupportIncidentHybridTransport(const modules::SupportIncidentLine& line) {
    RecordingSerialEndpoint serial;
    RecordingBleEndpoint ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    fxe::Console console(transport);
    modules::WriteSupportIncident(console, line);
    const std::string expected = std::string(line.CStr()) + "\n";
    const bool complete = ble.bytes == expected;
    Check(complete, "actual hybrid transport preserves one complete support incident line");
    Check(serial.bytes.find(std::string(line.CStr()) + "\r\n") != std::string::npos,
          "hybrid serial output preserves complete support incident");
    std::printf("[PLAYBACK] support: Console bulk/raw -> production Hybrid -> BLE sink; payload=%zu bytes, first_newline=%zu, accepted=%u; expected complete JSON then one newline. Radio delivery remains HIL.\n",
                line.Size(), ble.bytes.find('\n'), unsigned(complete));
}
void TestHybridTransportBoundaryAndEditing() {
    RecordingSerialEndpoint serial;
    RecordingBleEndpoint ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    const std::string boundary(1023U, 'x');
    transport.Write(boundary.data(), boundary.size());
    const bool held = ble.bytes.empty();
    transport.Write("\r", 1U);
    transport.Write("\n", 1U);
    const bool complete = ble.bytes == boundary + "\n";
    Check(held && complete,
          "1023-byte contract boundary waits for terminator and remains one BLE line");
    transport.Write("next\n", 5U);
    const bool reusable = ble.bytes == boundary + "\nnext\n";
    Check(reusable, "synchronous BLE sink retains earlier bytes after buffer reuse");
    Check(serial.bytes == boundary + "\r\nnext\n",
          "boundary serial bytes are unchanged by BLE filtering");

    struct EditCase { const char* input; const char* expected; };
    const EditCase cases[] = {
        {"plain\r\n", "plain\n"},
        {"> \n", ""},
        {" \t\r\n", ""},
        {"ab\bC\n", "aC\n"},
        {"ab\x7f" "C\n", "aC\n"},
        {"old\rnew\r\n", "new\n"},
    };
    bool editsPreserved = true;
    for (const auto& edit : cases) {
        RecordingSerialEndpoint editSerial;
        RecordingBleEndpoint editBle;
        firmware_internal::HybridConsoleTransport editTransport(editSerial, editBle);
        // Byte-at-a-time delivery also exercises CR/LF across Write calls.
        for (const char* p = edit.input; *p != '\0'; ++p) editTransport.Write(p, 1U);
        const bool preserved = editBle.bytes == edit.expected && editSerial.bytes == edit.input;
        Check(preserved, "hybrid preserves terminal editing and serial bytes");
        editsPreserved = editsPreserved && preserved;
    }
    std::printf("[PLAYBACK] hybrid: 1023-byte line held=%u complete=%u; next-line reuse=%u; CRLF/prompt/whitespace/backspace/delete/redraw preserved=%u.\n",
                unsigned(held), unsigned(complete), unsigned(reusable), unsigned(editsPreserved));
}
} // namespace
int main() {
    TestDiagnosticProgress();
    TestHybridTransportBoundaryAndEditing();
    TestSerialQueueBurstDrainOverflowAndDisconnect();
    TestFirmwareLoggerAdapterKeepsBulkRecordsAtomic();
    static_assert(std::is_trivially_default_constructible<runtime::RetainedProgress>::value,
                  "RTC no-init storage must never run a boot initializer");
    runtime::RetainedProgress state{};
    runtime::BeginProgressBoot(state,1);
    runtime::IncidentContext context{};
    context.uptimeMs=23456; context.expectedOccupancy=0xffff00000000ffffULL;
    context.observedOccupancy=context.expectedOccupancy; context.interactionPhase=3;
    runtime::RecordIncidentContext(state,context);
    runtime::RecordProgress(state,0,runtime::ProgressStage::Sensors,23456);
    runtime::RecordProgress(state,1,runtime::ProgressStage::Engine,23450);
    runtime::BeginProgressBoot(state,6);
    const auto snapshot=runtime::SnapshotProgress(state);
    Check(snapshot.previousValid && snapshot.watchdogRestarts==1 && snapshot.resetReason==6,"watchdog restart retained");
    Check(snapshot.previous.context.uptimeMs==23456 && snapshot.previous.context.interactionPhase==3,"bounded pre-reset context retained");
    Check(snapshot.previous.stages[0]==static_cast<uint8_t>(runtime::ProgressStage::Sensors),"blocked stage survives boot");
    const auto line=modules::FormatSupportIncident(snapshot,7,1,0);
    TestSupportIncidentHybridTransport(line);
    Check(!line.Empty() && std::strstr(line.CStr(),"\"prior_up\":23456"),"production report exposes prior uptime");
    RecordingTransport transport;
    fxe::Console console(transport);
    modules::WriteSupportIncident(console,line);
    const std::string framed=std::string(line.CStr())+"\r\n";
    Check(line.Size()>256U && transport.bytes.find(framed)!=std::string::npos,
          "production Console bulk/raw framing preserves complete long incident");
    const auto health=modules::FormatBoardHealth({}, modules::PersistenceDegradation::StorageIo, snapshot, true);
    runtime::RuntimeHealthSnapshot runtimeHealth{};
    runtimeHealth.revision=0x1234U;
    runtimeHealth.state=runtime::RuntimeHealthState::InputPaused;
    runtimeHealth.causes=0x21U;
    const auto healthState=modules::EncodeBoardHealthConnectionState(
        runtimeHealth, modules::PersistenceDegradation::StorageIo, snapshot, true);
    Check(healthState.runtime[0]==1U && healthState.runtime[1]==0x34U &&
              healthState.runtime[2]==0x12U && healthState.runtime[3]==2U &&
              healthState.runtime[4]==0x21U && healthState.runtime[5]==0U,
          "minimum-MTU runtime health preserves version and health truth");
    Check(healthState.context[0]==0x46U && healthState.context[1]==6U &&
              healthState.context[2]==2U && healthState.context[6]==1U,
          "minimum-MTU health context preserves storage, reset and counters");
    const std::string fixture=std::string(health.CStr())+"\n"+line.CStr()+"\n";
    const char* fixturePath="app/test/fixtures/board_health_console.jsonl";
    if (std::getenv("LITCH_UPDATE_HEALTH_FIXTURE")) std::ofstream(fixturePath)<<fixture;
    std::ifstream fixtureFile(fixturePath);
    const std::string recorded{std::istreambuf_iterator<char>(fixtureFile), {}};
    Check(recorded==fixture,"shared app fixture equals production formatted/framed payload");
    auto maximum=snapshot;
    maximum.bootCount=maximum.watchdogRestarts=maximum.resetReason=UINT32_MAX;
    maximum.current.context.uptimeMs=maximum.previous.context.uptimeMs=UINT32_MAX;
    for(unsigned lane=0;lane<4;++lane) {
        maximum.current.stageAtMs[lane]=maximum.previous.stageAtMs[lane]=UINT32_MAX;
    }
    const auto maxLine=modules::FormatSupportIncident(maximum,UINT32_MAX,UINT32_MAX,UINT8_MAX);
    TestBackpressuredSerialPreservesBleAndInput(maxLine);
    TestSupportIncidentHybridTransport(maxLine);
    StaticJsonDocument<2048> parsed;
    Check(!deserializeJson(parsed,maxLine.CStr()) && parsed.containsKey("recent") && maxLine.Size()<1024U,
          "worst-case production incident remains bounded complete JSON");
    std::printf("[PLAYBACK] watchdog input=task-wdt-reset boot=%u reason=%u prior_uptime=23456 prior_stage=sensors current=boot report=%s\n",snapshot.bootCount,snapshot.resetReason,line.CStr());
    Check(std::strstr(line.CStr(), "\"recent\":[9,8,14,14,14,14,14,14]") != nullptr, "partial progress ring ordered and padded idle");
    for (unsigned i=0;i<270;++i) runtime::RecordProgress(state,0,static_cast<runtime::ProgressStage>(i%14),i);
    runtime::BeginProgressBoot(state,6);
    const auto wrapped=modules::FormatSupportIncident(runtime::SnapshotProgress(state),0,0,0);
    Check(std::strstr(wrapped.CStr(), "\"recent\":[10,11,12,13,0,1,2,3]") != nullptr, "progress chronology survives ring and cursor wrap");
    state.checksum^=1U;
    runtime::BeginProgressBoot(state,6);
    Check(!runtime::SnapshotProgress(state).previousValid,"corrupt RTC is unavailable, not invented context");
    runtime::BeginProgressBoot(state,1);
    Check(runtime::SnapshotProgress(state).watchdogRestarts==0,"cold power discards stale RTC counters");
    Check(runtime::WATCHDOG_TIMEOUT_SECONDS>30U,"bounded hang timeout distinct from legitimate work");
    return failures?1:0;
}
