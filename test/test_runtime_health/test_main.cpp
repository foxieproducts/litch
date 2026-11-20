#include <cstdio>

#include "runtime/runtime_health.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

runtime::RuntimeHealthFact Board(const uint32_t tMs,
                                 const runtime::RuntimeHealthBoardScan scan) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::BoardScan;
    fact.boardScan = scan;
    return fact;
}

runtime::RuntimeHealthFact Ambient(const uint32_t tMs, const uint8_t count) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::AmbientSensorCount;
    fact.ambientSensorCount = count;
    return fact;
}

runtime::RuntimeHealthFact Source(const uint32_t tMs,
                                  const runtime::RuntimeHealthSource source) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::PowerSource;
    fact.source = source;
    return fact;
}

runtime::RuntimeHealthFact Envelope(const uint32_t tMs, const uint8_t percent) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::PowerEnvelope;
    fact.powerEnvelopePercent = percent;
    return fact;
}

runtime::RuntimeHealthFact Brownout(const uint32_t tMs) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::Brownout;
    fact.brownoutDetected = true;
    return fact;
}

runtime::RuntimeHealthFact Reset(const uint32_t tMs) {
    runtime::RuntimeHealthFact fact;
    fact.tMs = tMs;
    fact.kind = runtime::RuntimeHealthFactKind::ResetLatchedProtection;
    return fact;
}

void CheckAdvice(const runtime::RuntimeHealthSnapshot& snapshot,
                 const bool allowed, const char* message) {
    Check(snapshot.advice.boardInputAllowed == allowed &&
              snapshot.advice.moveAcceptanceAllowed == allowed &&
              snapshot.advice.clocksAllowed == allowed,
          message);
}

void TestBoardUncertaintyAndRecovery() {
    runtime::RuntimeHealthReducer reducer;
    Check(!reducer.Apply(Board(0U, runtime::RuntimeHealthBoardScan::Uncertain)),
          "transient scan failure is silent");
    Check(!reducer.Apply(Board(99U, runtime::RuntimeHealthBoardScan::Uncertain)),
          "scan uncertainty is silent before 100 ms");
    Check(reducer.Apply(Board(100U, runtime::RuntimeHealthBoardScan::Uncertain)),
          "scan uncertainty enters exactly at 100 ms");
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "sustained scan uncertainty pauses input");
    CheckAdvice(reducer.Snapshot(), false, "paused advice denies board, moves, and clocks");
    const uint16_t pausedRevision = reducer.Snapshot().revision;
    Check(!reducer.Apply(Board(100U, runtime::RuntimeHealthBoardScan::Uncertain)) &&
              reducer.Snapshot().revision == pausedRevision,
          "repeated uncertain sample has no extra revision");

    reducer.Apply(Board(101U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "one stable retry cannot recover paused input");
    reducer.Apply(Board(200U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "stable recovery remains paused before 100 ms");
    Check(reducer.Apply(Board(201U, runtime::RuntimeHealthBoardScan::Stable)),
          "fresh stable snapshot recovers exactly at 100 ms");
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "stable recovery clears board cause");
    CheckAdvice(reducer.Snapshot(), true, "healthy advice allows board, moves, and clocks");

    reducer.Apply(Board(300U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Board(400U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Board(401U, runtime::RuntimeHealthBoardScan::Stable));
    reducer.Apply(Board(450U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Board(500U, runtime::RuntimeHealthBoardScan::Stable));
    reducer.Apply(Board(599U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "flapping stable scan resets recovery confirmation");
    reducer.Apply(Board(600U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "continuous fresh stable interval clears after flap");
}

void TestWrapAndAmbient() {
    runtime::RuntimeHealthReducer reducer;
    reducer.Apply(Board(0xfffffff0U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Board(0x00000054U, runtime::RuntimeHealthBoardScan::Uncertain));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "board uncertainty timer is uint32 wrap-safe");
    reducer.Apply(Board(0x00000055U, runtime::RuntimeHealthBoardScan::Stable));
    reducer.Apply(Board(0x000000b9U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "board recovery timer is uint32 wrap-safe");

    reducer.Apply(Ambient(200U, 1U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable &&
              runtime::HasRuntimeHealthCause(reducer.Snapshot().causes,
                                             runtime::RuntimeHealthCause::OneAmbientSensor),
          "one ambient sensor is usable but degraded");
    CheckAdvice(reducer.Snapshot(), true, "one ambient sensor does not block play");
    reducer.Apply(Ambient(201U, 0U));
    Check(runtime::HasRuntimeHealthCause(reducer.Snapshot().causes,
                                         runtime::RuntimeHealthCause::NoAmbientSensors) &&
              !runtime::HasRuntimeHealthCause(reducer.Snapshot().causes,
                                              runtime::RuntimeHealthCause::OneAmbientSensor),
          "no ambient sensors select deterministic fallback cause only");
    reducer.Apply(Ambient(202U, 2U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "two ambient sensors clear ambient degradation");
}

void TestPowerAndPriority() {
    runtime::RuntimeHealthReducer reducer;
    reducer.Apply(Source(0U, runtime::RuntimeHealthSource::Unstable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable,
          "unstable source is visible but usable");
    CheckAdvice(reducer.Snapshot(), true, "unstable-source degradation allows play");

    runtime::RuntimeHealthFact budget;
    budget.kind = runtime::RuntimeHealthFactKind::LedBudget;
    budget.tMs = 1U;
    Check(!reducer.Apply(budget), "ordinary instantaneous led budget is silent");
    reducer.Apply(Source(2U, runtime::RuntimeHealthSource::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "stable source clears source cause");

    reducer.Apply(Envelope(10U, 50U));
    reducer.Apply(Envelope(1009U, 50U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "severe power does not latch before 1000 ms");
    reducer.Apply(Envelope(1010U, 50U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable,
          "severe power latches a safe usable limit exactly at 1000 ms");
    CheckAdvice(reducer.Snapshot(), true, "latched power limiting allows play");
    reducer.Apply(Envelope(1011U, 100U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable,
          "power recovery cannot clear latched severe protection");
    reducer.Apply(Reset(1012U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "explicit reset clears severe protection latch");

    reducer.Apply(Board(1100U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Board(1200U, runtime::RuntimeHealthBoardScan::Uncertain));
    reducer.Apply(Ambient(1201U, 0U));
    reducer.Apply(Brownout(1202U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "board uncertainty pauses input while power limiting remains safe");
    reducer.Apply(Reset(1203U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "clearing protection reveals still-active paused cause");
    reducer.Apply(Board(1204U, runtime::RuntimeHealthBoardScan::Stable));
    reducer.Apply(Board(1304U, runtime::RuntimeHealthBoardScan::Stable));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable,
          "clearing board cause reveals ambient degradation");
    reducer.Apply(Ambient(1305U, 2U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "cause clearing returns healthy when no cause remains");
}

void TestBrownoutInvalidAndRevisionWrap() {
    runtime::RuntimeHealthReducer reducer;
    reducer.Apply(Brownout(0U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable,
          "brownout latches a safe usable power limit");
    reducer.Apply(Brownout(1U));
    Check(reducer.Snapshot().revision == 1U,
          "repeated brownout has no repeated revision");
    reducer.Apply(Reset(2U));
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "explicit reset clears brownout latch");

    runtime::RuntimeHealthFact invalid;
    invalid.tMs = 3U;
    invalid.kind = static_cast<runtime::RuntimeHealthFactKind>(255U);
    Check(reducer.Apply(invalid) &&
              reducer.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "invalid fact fails closed without undefined behavior");

    runtime::RuntimeHealthReducer wrap;
    for (uint32_t i = 0U; i < 65535U; ++i) {
        wrap.Apply(Ambient(i, (i & 1U) == 0U ? 1U : 2U));
    }
    Check(wrap.Snapshot().revision == 65535U,
          "revision reaches uint16 maximum after observable transitions");
    wrap.Apply(Ambient(65535U, 2U));
    Check(wrap.Snapshot().revision == 0U,
          "revision wraps as uint16 after observable transition");
}

void TestDefaultConstruction() {
    const runtime::RuntimeHealthReducer reducer;
    const runtime::RuntimeHealthSnapshot snapshot = reducer.Snapshot();
    Check(snapshot.state == runtime::RuntimeHealthState::Healthy &&
              snapshot.causes == 0U && snapshot.revision == 0U,
          "default construction is deterministic and healthy");
    CheckAdvice(snapshot, true, "default advice allows board, moves, and clocks");
}

} // namespace

int main() {
    std::puts("[RUNTIME_HEALTH] start");
    TestBoardUncertaintyAndRecovery();
    TestWrapAndAmbient();
    TestPowerAndPriority();
    TestBrownoutInvalidAndRevisionWrap();
    TestDefaultConstruction();
    if (g_failures == 0) {
        std::puts("[RUNTIME_HEALTH] ok");
        return 0;
    }
    std::printf("[RUNTIME_HEALTH] failures=%d\n", g_failures);
    return 1;
}
