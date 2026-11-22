#include <cstdio>
#include <cstring>

#include "modules/ble_console_input_buffer.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

size_t Drain(modules::BleConsoleInputBuffer& buffer, char* out, const size_t outCapacity) {
    if (out == nullptr || outCapacity == 0) {
        return 0;
    }

    size_t len = 0;
    char next = '\0';
    while (len + 1U < outCapacity && buffer.TryDequeue(next)) {
        out[len++] = next;
    }
    out[len] = '\0';
    return len;
}

void CheckFragmentedCommandRoundTrips() {
    modules::BleConsoleInputBuffer buffer;
    Check(buffer.Enqueue("set", 3) == 3U, "first fragment enqueued");
    Check(buffer.Enqueue("tings", 5) == 5U, "second fragment enqueued");
    Check(buffer.Enqueue(" status\n", 8) == 8U, "third fragment enqueued");

    char actual[32] = {};
    const size_t len = Drain(buffer, actual, sizeof(actual));
    Check(len == 16U, "fragmented command length preserved");
    Check(std::strcmp(actual, "settings status\n") == 0, "fragmented command bytes preserved");
}

void CheckLargeFragmentedCommandRoundTrips() {
    modules::BleConsoleInputBuffer buffer;
    char expected[245] = {};
    for (size_t i = 0; i < 244U; ++i) {
        expected[i] = static_cast<char>('a' + (i % 26U));
    }
    expected[244] = '\0';

    Check(buffer.Enqueue(expected, 120) == 120U, "large fragment one enqueued");
    Check(buffer.Enqueue(expected + 120, 80) == 80U, "large fragment two enqueued");
    Check(buffer.Enqueue(expected + 200, 44) == 44U, "large fragment three enqueued");
    Check(buffer.Enqueue("\n", 1) == 1U, "newline fragment enqueued");

    char actual[246] = {};
    const size_t len = Drain(buffer, actual, sizeof(actual));
    Check(len == 245U, "large fragmented command drains fully");
    Check(std::memcmp(actual, expected, 244U) == 0, "large fragmented command payload preserved");
    Check(actual[244] == '\n', "large fragmented command newline preserved");
}

void CheckDisconnectResetDropsPartialCommand() {
    modules::BleConsoleInputBuffer buffer;
    Check(buffer.Enqueue("settings st", 11) == 11U, "partial command enqueued");

    buffer.Clear();

    Check(buffer.Enqueue("help\n", 5) == 5U, "post-reset command enqueued");

    char actual[16] = {};
    const size_t len = Drain(buffer, actual, sizeof(actual));
    Check(len == 5U, "only post-reset command remains");
    Check(std::strcmp(actual, "help\n") == 0, "stale partial command removed on reset");
}

void CheckOverflowIsBounded() {
    modules::BleConsoleInputBuffer buffer;
    char oversized[300] = {};
    for (size_t i = 0; i < sizeof(oversized); ++i) {
        oversized[i] = 'x';
    }

    const size_t written = buffer.Enqueue(oversized, sizeof(oversized));
    Check(written == (modules::BleConsoleInputBuffer::kCapacity - 1U), "buffer keeps one slot open for ring semantics");

    char actual[modules::BleConsoleInputBuffer::kCapacity + 1U] = {};
    const size_t len = Drain(buffer, actual, sizeof(actual));
    Check(len == (modules::BleConsoleInputBuffer::kCapacity - 1U), "overflow does not corrupt buffered bytes");
}

void CheckStagedRecordIsAtomic() {
    modules::BleConsoleInputBuffer buffer;
    Check(buffer.BeginStagedRecord(5U), "staged record reserves capacity");
    const uint8_t first[] = {'h', 'e'};
    const uint8_t second[] = {'l', 'p', '\n'};
    Check(buffer.AppendStaged(first, sizeof(first)), "first staged fragment accepted");
    char actual[16] = {};
    Check(Drain(buffer, actual, sizeof(actual)) == 0U,
          "unfinished staged bytes remain invisible");
    Check(buffer.AppendStaged(second, sizeof(second)), "second staged fragment accepted");
    Check(buffer.CommitStagedRecord(), "complete staged record commits");
    Check(Drain(buffer, actual, sizeof(actual)) == 5U,
          "committed staged record becomes visible");
    Check(std::strcmp(actual, "help\n") == 0,
          "committed staged bytes preserve order");
}

void CheckStagedRecordRejectsOverflowWithoutPartialInput() {
    modules::BleConsoleInputBuffer buffer;
    Check(buffer.BeginStagedRecord(3U), "short staged record begins");
    const uint8_t oversized[] = {'b', 'a', 'd', '\n'};
    Check(!buffer.AppendStaged(oversized, sizeof(oversized)),
          "staged overflow is rejected");
    buffer.DiscardStagedRecord();
    Check(buffer.Enqueue("ok\n", 3U) == 3U,
          "direct input resumes after staged rejection");
    char actual[8] = {};
    Check(Drain(buffer, actual, sizeof(actual)) == 3U,
          "rejected staged bytes never enter input");
    Check(std::strcmp(actual, "ok\n") == 0,
          "only later valid input remains");
}

} // namespace

int main() {
    std::puts("[BLE_CONSOLE_INPUT_BUFFER] start");

    CheckFragmentedCommandRoundTrips();
    CheckLargeFragmentedCommandRoundTrips();
    CheckDisconnectResetDropsPartialCommand();
    CheckOverflowIsBounded();
    CheckStagedRecordIsAtomic();
    CheckStagedRecordRejectsOverflowWithoutPartialInput();

    if (g_failures == 0) {
        std::puts("[BLE_CONSOLE_INPUT_BUFFER] ok");
        return 0;
    }

    std::printf("[BLE_CONSOLE_INPUT_BUFFER] failures=%d\n", g_failures);
    return 1;
}
