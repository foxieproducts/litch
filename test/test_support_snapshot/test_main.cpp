#include <array>
#include <cstdio>
#include "foxie_embedded/byte_order.hpp"
#include "modules/support_snapshot.hpp"
#include "trace/trace_recorder.hpp"
namespace {
int failures = 0;
uint64_t now = 0U;
uint64_t Clock() { return now; }
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::printf("[FAIL] %s\n", message); }
}
}
int main() {
    trace::SupportActionHistory actions(&Clock);
    now = (uint64_t{1U} << 32U) + 100U;
    const auto paint = actions.Begin(trace::SupportActionKind::Paint, trace::SupportActionOrigin::App);
    now += 25U;
    actions.Finish(paint, trace::SupportActionOutcome::Accepted);
    Check(actions.Size() == 1U && actions.At(0U).outcome == trace::SupportActionOutcome::Accepted,
          "SupportUserActionJourney: acceptance is one logical row, not completion");
    now += 35U;
    actions.Finish(paint, trace::SupportActionOutcome::Applied, 4U);
    actions.Finish(paint, trace::SupportActionOutcome::Rejected);
    Check(actions.Size() == 1U && actions.At(0U).durationMs == 60U &&
          actions.At(0U).outcome == trace::SupportActionOutcome::Applied,
          "terminal replay cannot change/duplicate an applied gesture");
    for (unsigned i = 0U; i < 25U; ++i) {
        ++now;
        const auto action = actions.Begin(trace::SupportActionKind::PiecesChanged,
            trace::SupportActionOrigin::Board, 2U, 1U);
        actions.Finish(action, trace::SupportActionOutcome::Observed);
    }
    Check(actions.Size() == 20U && actions.Omitted() == 6U && actions.At(0U).sequence == 7U,
          "ring keeps newest20 with explicit omissions and source sequence");
    std::array<uint8_t, modules::support_snapshot::CAPACITY> bytes{};
    modules::SupportRuntimeSnapshot stats{};
    stats.available = true; stats.uptimeMs = now;
    const auto size = modules::support_snapshot::EncodeHistory(bytes.data(), bytes.size(), 42U, stats, 7U, 2U, 0U, actions);
    Check(size == 480U && bytes[7U] == 20U && fxe::ReadU32LE(bytes.data() + 2U) == 42U &&
          fxe::ReadU64LE(bytes.data() + 80U) == actions.At(0U).uptimeMs,
          "maximum history capsule is exactly480bytes, full64 timestamps, no common capacity increase");
    Check(modules::support_snapshot::EncodeHistory(bytes.data(), 479U, 42U, stats, 7U, 2U, 0U, actions) == 0U,
          "undersized output refuses a partial support capsule");
    modules::support_snapshot::PublicHealth health{};
    health.validity = 4U; health.uptimeMs = now; health.releaseLabel = "0.9.5-test";
    Check(modules::support_snapshot::EncodePublic(bytes.data(), bytes.size(), 99U, health) == 106U &&
          bytes[1U] == 4U && fxe::ReadU32LE(bytes.data() + 82U) == 99U &&
          fxe::ReadU64LE(bytes.data() + 10U) == now,
          "public capsule exactly106bytes version2 with correlation and explicit unavailable health");
    runtime::ProgressSnapshot progress{};
    Check(modules::support_snapshot::EncodeIncident(bytes.data(), bytes.size(), 9U, progress, 7U, 2U, 3U) == 136U &&
          fxe::ReadU32LE(bytes.data() + 127U) == 7U && bytes[135U] == 3U,
          "typed compact incident preserves existing inventory without Console");
    trace::TraceRecorder recorder;
    recorder.RecordBoardObservation(0x0fU, 0x03U, 10U);
    recorder.RecordTick(); recorder.RecordCheckpointFrame(123U, "board");
    Check(!recorder.IsActive() && recorder.UserActions().Size() == 1U &&
          recorder.UserActions().At(0U).count == 2U && recorder.UserActions().At(0U).secondCount == 2U &&
          recorder.UserActions().At(0U).outcome == trace::SupportActionOutcome::Observed,
          "production atomic observation records simultaneous lift/place counts even when raw trace is off; ticks are not user actions");
    std::printf("[SUPPORT_SNAPSHOT] failures=%d ring_bytes=%zu max_wire=%zu\n", failures, sizeof(actions), size);
    return failures ? 1 : 0;
}
