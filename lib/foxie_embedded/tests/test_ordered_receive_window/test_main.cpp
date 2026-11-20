#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "foxie_embedded/ordered_receive_window.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

namespace {
using Window = fxe::OrderedReceiveWindow<4U, 8U>;
using Result = Window::Result;
using Disposition = Window::Disposition;
fxe::test_support::TestResults results("ORDERED_RECEIVE_WINDOW");
constexpr uint32_t TOKEN = 17U;
const uint8_t COMMAND[]{0x13U, 5U, 9U};
const uint8_t CHANGED_OPCODE[]{0x13U, 6U, 9U};
static_assert(sizeof(Window) <= 4U * (8U + 6U) + 16U, "fixed payload/outcome storage bound");
static_assert(sizeof(fxe::OrderedReceiveWindow<16U, 20U>) <= 16U * 26U + 16U,
              "interactive stream memory bound");
static_assert(std::is_trivially_destructible<Window>::value, "no owned dynamic resources");
static_assert(Window::GrantThroughFor(UINT32_MAX - 4U) == Window::LAST_SEQUENCE,
              "last grant saturates before reserved sequence");
static_assert(Window::GrantThroughFor(Window::LAST_SEQUENCE) == Window::LAST_SEQUENCE,
              "exhausted grant cannot wrap or reopen sequence one");
static_assert(Window::GrantThroughFor(UINT32_MAX) == Window::LAST_SEQUENCE,
              "even an invalid caller arithmetic query cannot wrap");

void Check(const bool value, const char* message) { results.Check(value, message); }

void ReorderAndRetainOutcomes() {
    Window window;
    Check(!window.Start(0U) && window.Start(TOKEN), "nonzero fresh token starts sequence one");
    Window::Progress progress;
    window.ReadProgress(TOKEN, progress);
    Check(progress.confirmedThrough == 0U && progress.terminalThrough == 0U &&
              progress.nextExpected == 1U && progress.grantThrough == 4U &&
              progress.receivedBits == 0U && progress.missingBits == 15U,
          "initial lease reserves four bounded identities");
    Check(window.Ingress(TOKEN, 3U, COMMAND) == Result::Ok &&
              window.Ingress(TOKEN, 2U, COMMAND) == Result::Ok,
          "out-of-order identities inside lease are admitted");
    Window::Command command;
    Check(window.BeginNext(TOKEN, command) == Result::NotReady,
          "missing first identity prevents later domain application");
    window.ReadProgress(TOKEN, progress);
    Check(progress.receivedBits == 6U && progress.missingBits == 9U,
          "missing/received bits describe the receipt-anchored lease");
    Check(window.Ingress(TOKEN, 5U, COMMAND) == Result::OutsideWindow &&
              window.Ingress(TOKEN, 3U, COMMAND) == Result::Duplicate &&
              window.Ingress(TOKEN, 3U, CHANGED_OPCODE) == Result::ChangedDuplicate,
          "future ingress cannot overwrite; exact retry reuses lease; changed opcode fails");
    Check(window.Ingress(TOKEN, 1U, COMMAND) == Result::Ok &&
              window.BeginNext(TOKEN, command) == Result::Ok && command.sequence == 1U &&
              command.streamToken == TOKEN && command.payload.Size() == sizeof(COMMAND) &&
              command.payload[1] == 5U, "gap repair exposes exact next payload once");
    const auto first = command;
    Check(window.BeginNext(TOKEN, command) == Result::AlreadyApplying &&
              window.ReadApplying(TOKEN, command) && command.sequence == first.sequence,
          "pending domain work is re-readable but cannot be invoked twice");
    Check(window.ConfirmThrough(TOKEN, 1U) == Result::InvalidReceipt &&
              window.Complete(TOKEN, 2U, {}) == Result::InvalidCompletion &&
              window.Complete(TOKEN, 1U, {Disposition::Rejected, 0U}) == Result::InvalidCompletion &&
              window.Complete(TOKEN, 1U, {Disposition::Applied, 7U}) == Result::InvalidCompletion,
          "receipt/out-of-order/invalid outcomes cannot release in-flight work");
    Check(window.Complete(TOKEN, 1U, {Disposition::Rejected, 0x1234U}) == Result::Ok &&
              window.Complete(TOKEN, 1U, {}) == Result::InvalidCompletion,
          "terminal rejection advances once with complete reason");
    Window::Outcome outcome;
    Check(window.ReadOutcome(TOKEN, 1U, outcome) &&
              outcome.disposition == Disposition::Rejected && outcome.reason == 0x1234U &&
              window.Ingress(TOKEN, 1U, CHANGED_OPCODE) == Result::ChangedDuplicate &&
              window.Ingress(TOKEN, 1U, COMMAND) == Result::Duplicate,
          "terminal outcome and byte-exact payload both survive until receipt");
    for (uint32_t sequence = 2U; sequence <= 4U; ++sequence) {
        if (sequence == 4U) Check(window.Ingress(TOKEN, sequence, COMMAND) == Result::Ok,
                                 "last lease slot remains available");
        Check(window.BeginNext(TOKEN, command) == Result::Ok && command.sequence == sequence,
              "only contiguous commands reach domain");
        Check(window.Complete(TOKEN, sequence, sequence == 3U
                  ? Window::Outcome{Disposition::Cancelled, 41U} : Window::Outcome{}) == Result::Ok,
              "applied and cancelled commands have terminal outcomes");
    }
    Check(window.Ingress(TOKEN, 5U, COMMAND) == Result::OutsideWindow &&
              window.BeginNext(TOKEN, command) == Result::NotReady,
          "application does not release semantic capacity or overwrite evidence");
    window.ReadProgress(TOKEN, progress);
    Check(progress.terminalThrough == 4U && progress.confirmedThrough == 0U &&
              progress.grantThrough == 4U && progress.receivedBits == 15U,
          "terminal checkpoint retains the full unconfirmed lease");
    Check(window.ConfirmThrough(TOKEN, 5U) == Result::InvalidReceipt &&
              window.ReadOutcome(TOKEN, 1U, outcome), "invalid cumulative receipt frees nothing");
    Check(window.ConfirmThrough(TOKEN, 2U) == Result::Ok &&
              window.ConfirmThrough(TOKEN, 1U) == Result::Duplicate &&
              !window.ReadOutcome(TOKEN, 1U, outcome) && window.ReadOutcome(TOKEN, 3U, outcome) &&
              outcome.disposition == Disposition::Cancelled && outcome.reason == 41U,
          "cumulative receipt frees only its complete prefix including rejected outcomes");
    window.ReadProgress(TOKEN, progress);
    Check(progress.grantThrough == 6U && progress.receivedBits == 3U && progress.missingBits == 12U,
          "confirmation advances grant without counting duplicate confirmations");
    Check(window.Ingress(TOKEN, 1U, CHANGED_OPCODE) == Result::AlreadyConfirmed &&
              window.Ingress(TOKEN, 5U, COMMAND) == Result::Ok,
          "confirmed replay never re-applies; freed slot accepts next identity");
}

void ReconnectFencesEveryCallback() {
    Window window;
    window.Start(TOKEN);
    window.Ingress(TOKEN, 1U, COMMAND);
    Window::Command old;
    window.BeginNext(TOKEN, old);
    Check(window.Retire(TOKEN) && !window.Start(TOKEN) && window.Start(TOKEN + 1U),
          "retired stream requires a fresh token");
    window.Ingress(TOKEN + 1U, 1U, COMMAND);
    Window::Command current;
    window.BeginNext(TOKEN + 1U, current);
    Window::Progress progress;
    Window::Outcome outcome;
    Check(window.Ingress(TOKEN, 2U, COMMAND) == Result::StaleStream &&
              window.Complete(old.streamToken, old.sequence, {}) == Result::StaleStream &&
              window.ConfirmThrough(TOKEN, 1U) == Result::StaleStream &&
              window.BeginNext(TOKEN, old) == Result::StaleStream &&
              !window.ReadApplying(TOKEN, old) && !window.ReadOutcome(TOKEN, 1U, outcome) &&
              !window.ReadProgress(TOKEN, progress) && !window.Retire(TOKEN),
          "old ingress/completion/receipt/read/retirement cannot affect replacement stream");
    Check(window.ReadApplying(TOKEN + 1U, current) && current.sequence == 1U &&
              window.Complete(TOKEN + 1U, 1U, {}) == Result::Ok,
          "stale completion leaves newer in-flight command intact");
    Check(!window.Start(TOKEN + 1U) && window.ReadOutcome(TOKEN + 1U, 1U, outcome),
          "repeated start does not discard current retained outcome");
}

void PayloadAndCounterBounds() {
    Window window;
    Check(window.Ingress(TOKEN, 1U, COMMAND) == Result::StaleStream,
          "no ingress before fresh stream admission");
    window.Start(TOKEN);
    const uint8_t oversized[9]{};
    Check(window.Ingress(TOKEN, 0U, COMMAND) == Result::InvalidSequence &&
              window.Ingress(TOKEN, UINT32_MAX, COMMAND) == Result::SequenceExhausted &&
              window.Ingress(TOKEN, UINT32_MAX - 1U, COMMAND) == Result::OutsideWindow &&
              window.Ingress(TOKEN, 1U, oversized) == Result::InvalidPayload &&
              window.Ingress(TOKEN, 1U, {nullptr, 1U}) == Result::InvalidPayload,
          "reserved sequence/oversize/null payload cannot mutate window");
    Check(window.Ingress(TOKEN, 1U, {}) == Result::Ok &&
              window.Ingress(TOKEN, 1U, {}) == Result::Duplicate &&
              window.Ingress(TOKEN, 1U, COMMAND) == Result::ChangedDuplicate,
          "empty payload is valid and length participates in duplicate identity");
    fxe::OrderedReceiveWindow<64U, 1U> wide;
    wide.Start(TOKEN);
    decltype(wide)::Progress progress;
    Check(wide.ReadProgress(TOKEN, progress) && progress.missingBits == UINT64_MAX,
          "full-width bitmap does not shift by 64");
}

void SustainedRingReuse() {
    Window window;
    window.Start(TOKEN);
    // Initial fill plus two complete reuse cycles catches stale ring slots.
    for (uint32_t base = 0U; base < 12U; base += 4U) {
        for (uint32_t offset = 4U; offset != 0U; --offset) {
            const uint8_t payload[]{0x13U, uint8_t(base + offset)};
            Check(window.Ingress(TOKEN, base + offset, payload) == Result::Ok,
                  "reverse-order batch fits freshly confirmed window");
        }
        for (uint32_t offset = 1U; offset <= 4U; ++offset) {
            Window::Command command;
            Check(window.BeginNext(TOKEN, command) == Result::Ok &&
                      command.sequence == base + offset && command.payload[1] == uint8_t(base + offset),
                  "ring reuse preserves ordered identity and exact payload");
            Check(window.Complete(TOKEN, command.sequence, {}) == Result::Ok,
                  "sustained application completes once");
        }
        Check(window.ConfirmThrough(TOKEN, base + 4U) == Result::Ok,
              "sustained cumulative receipts release only terminal batches");
    }
}
} // namespace

int main() {
    results.Start();
    ReorderAndRetainOutcomes();
    ReconnectFencesEveryCallback();
    PayloadAndCounterBounds();
    SustainedRingReuse();
    std::printf("[ORDERED_RECEIVE_WINDOW] bytes=%zu interactive16x20=%zu records=12\n",
                sizeof(Window), sizeof(fxe::OrderedReceiveWindow<16U, 20U>));
    return results.Finish();
}
