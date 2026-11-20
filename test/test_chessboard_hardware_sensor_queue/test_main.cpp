#include <cstdio>

#include "hardware/chessboard_hardware.hpp"
#include "runtime/clock.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

class StubBoardInput final : public modules::IBoardInput {
  public:
    bool TryReadSnapshot(uint64_t&) override {
        return false;
    }

    bool TryReadDelta(modules::SquareDelta& outDelta) override {
        if (index >= deltaCount) {
            return false;
        }

        outDelta = deltas[index++];
        return true;
    }

    void Resync() override {}

    void Push(const uint8_t square, const bool nowOccupied) {
        if (deltaCount >= 4) {
            return;
        }

        deltas[deltaCount++] = modules::SquareDelta{square, nowOccupied};
    }

  private:
    modules::SquareDelta deltas[4]{};
    uint8_t deltaCount = 0;
    uint8_t index = 0;
};

class AtomicBoardInput final : public modules::IBoardInput {
  public:
    explicit AtomicBoardInput(const uint64_t initialOccupancy)
        : occupancy(initialOccupancy) {}

    bool TryReadSnapshot(uint64_t& outOcc64) override {
        outOcc64 = occupancy;
        return true;
    }

    bool TryReadDelta(modules::SquareDelta&) override { return false; }

    bool TryReadObservation(modules::BoardObservation& outObservation) override {
        if (!pending) {
            return false;
        }

        outObservation = observation;
        occupancy = observation.occupiedMask;
        pending = false;
        return true;
    }

    bool SupportsAtomicObservations() const override { return true; }

    void Resync() override {}

    void PushObservation(const uint64_t changedMask,
                         const uint64_t occupiedMask) {
        observation = modules::BoardObservation{changedMask, occupiedMask};
        pending = true;
    }

  private:
    uint64_t occupancy = 0;
    modules::BoardObservation observation{};
    bool pending = false;
};

class TypedAtomicBoardInput final : public modules::IBoardInput {
  public:
    explicit TypedAtomicBoardInput(const uint64_t initialOccupancy)
        : occupancy(initialOccupancy) {}

    bool TryReadSnapshot(uint64_t& outOcc64) override {
        outOcc64 = occupancy;
        return true;
    }

    modules::BoardScanAttemptResult CaptureScanAttempt() override {
        const modules::BoardScanAttemptResult result = nextResult;
        nextResult = {};
        return result;
    }

    bool TryReadDelta(modules::SquareDelta&) override { return false; }
    bool SupportsAtomicObservations() const override { return true; }
    void Resync() override {}

    void PushSuccessfulScan(const uint64_t rawOccupiedMask) {
        nextResult = {
            modules::BoardScanAttemptStatus::Succeeded,
            rawOccupiedMask,
        };
    }

    void PushFailure() {
        nextResult = {modules::BoardScanAttemptStatus::Failed, 0ULL};
    }

  private:
    uint64_t occupancy = 0ULL;
    modules::BoardScanAttemptResult nextResult{};
};

} // namespace

int main() {
    std::puts("[CHESSBOARD_HARDWARE_SENSOR_QUEUE] start");

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    StubBoardInput input;
    input.Push(12, false);
    input.Push(20, true);

    hardware::ChessboardHardware hw(input);
    hw.SetFlipSides(false);
    hw.PollAndDispatch(sm);

    Check(hw.GetSensorChangeCount() == 2, "dispatches all queued deltas");
    Check(sm.IsMoveSettlingActive(), "FIFO drain preserves move progression order");
    Check(sm.GetLastSensorSquare() == 20, "last sensor square reflects final queued delta");
    Check(!hw.GetOccupied(12), "pickup updates occupancy after drain");
    Check(hw.GetOccupied(20), "placement updates occupancy after drain");

    StubBoardInput rotatedInput;
    rotatedInput.Push(0, false);
    rotatedInput.Push(7, true);

    runtime::GameStateMachine rotatedSm(cfg);
    rotatedSm.Reset();

    hardware::ChessboardHardware rotatedHw(rotatedInput);
    rotatedHw.SetFlipSides(true);
    rotatedHw.PollAndDispatch(rotatedSm);

    Check(rotatedSm.GetLastSensorSquare() == 56, "queued deltas use logical squares after rotation");
    Check(!rotatedHw.GetOccupied(63), "rotated pickup clears mapped square");
    Check(rotatedHw.GetOccupied(56), "rotated placement sets mapped square");

    const uint64_t queensidePair = (1ULL << 56) | (1ULL << 60);
    AtomicBoardInput atomicInput(queensidePair);
    runtime::GameStateMachine atomicSm(cfg);
    atomicSm.Reset();
    atomicSm.DebugSetPhysicalOccupancyMask(queensidePair);

    runtime::ChessPositionState expected;
    expected.occupiedMask = queensidePair;
    expected.moverOccupiedMask = queensidePair;
    expected.kingMask = (1ULL << 60);
    expected.rookMask = (1ULL << 56);
    atomicSm.DebugSetExpectedPositionModel(expected);

    hardware::ChessboardHardware atomicHw(atomicInput);
    atomicHw.Resync();
    atomicInput.PushObservation(queensidePair, 0);
    atomicHw.PollAndDispatch(atomicSm);

    Check(atomicSm.IsHumanPieceInHand(),
          "hardware forwards same-scan castle pickup atomically");
    Check(atomicSm.GetChessInteractionModel().phase.plan.moveFrom == 60,
          "hardware atomic pickup preserves king origin");
    Check(!atomicSm.HasHumanIllegal(),
          "hardware atomic pickup does not invent rook-first chronology");
    Check(atomicSm.GetLastBoardObservationSequence() == 1,
          "hardware assigns a monotonic atomic observation sequence");
    Check(atomicHw.GetSensorChangeCount() == 2,
          "hardware atomic observation counts both changed squares");

    TypedAtomicBoardInput typedInput(1ULL << 0U);
    hardware::ChessboardHardware typedHw(typedInput);
    typedHw.SetFlipSides(true);
    typedHw.Resync();

    runtime::GameStateMachine typedSm(cfg);
    typedSm.Reset();
    runtime::InteractionDispatcher typedDispatcher(typedSm);

    typedInput.PushSuccessfulScan(1ULL << 7U);
    const modules::BoardScanAttemptResult captured =
        typedHw.CaptureScanAttempt();
    Check(captured.status == modules::BoardScanAttemptStatus::Succeeded,
          "hardware capture exposes fresh typed scan evidence");
    Check(captured.rawOccupiedMask == (1ULL << 7U),
          "hardware capture preserves raw square orientation");
    Check(typedHw.GetOccupied(63U),
          "capture does not mutate accepted rotated occupancy");
    Check(!typedHw.GetOccupied(56U),
          "capture does not expose unaccepted occupancy");
    Check(typedHw.GetSensorChangeCount() == 0U,
          "capture does not advance sensor-change count");
    Check(typedSm.GetLastBoardObservationSequence() == 0U,
          "capture does not reach the interaction dispatcher");

    Check(typedHw.AcceptAndDispatchScan(captured, typedDispatcher),
          "explicit acceptance dispatches changed fresh scan");
    Check(!typedHw.GetOccupied(63U) && typedHw.GetOccupied(56U),
          "explicit acceptance applies 180-degree square mapping");
    Check(typedHw.GetSensorChangeCount() == 2U,
          "explicit acceptance counts relative changed squares");
    Check(typedSm.GetLastBoardObservationSequence() == 1U,
          "explicit acceptance advances observation sequence once");
    Check(typedSm.GetLastBoardObservationChangedMask() ==
              ((1ULL << 63U) | (1ULL << 56U)),
          "explicit acceptance computes changed mask from accepted occupancy");
    Check(typedSm.GetLastBoardObservationOccupancyMask() == (1ULL << 56U),
          "explicit acceptance dispatches mapped complete occupancy");

    Check(!typedHw.AcceptAndDispatchScan(captured, typedDispatcher),
          "changed-only compatibility acceptance suppresses unchanged scan");
    Check(typedSm.GetLastBoardObservationSequence() == 1U,
          "suppressed unchanged scan does not advance sequence");

    Check(typedHw.AcceptAndDispatchScan(
              captured,
              typedDispatcher,
              hardware::ChessboardHardware::ObservationDispatchMode::Always),
          "recovery acceptance dispatches chosen unchanged fresh snapshot");
    Check(typedSm.GetLastBoardObservationSequence() == 2U,
          "forced fresh snapshot dispatch advances sequence exactly once");
    Check(typedSm.GetLastBoardObservationChangedMask() == 0ULL,
          "forced unchanged snapshot carries an empty relative changed mask");
    Check(typedHw.GetSensorChangeCount() == 2U,
          "unchanged recovery dispatch does not invent sensor changes");

    typedInput.PushFailure();
    const auto capturedFailure = typedHw.CaptureScanAttempt();
    Check(capturedFailure.status == modules::BoardScanAttemptStatus::Failed,
          "hardware capture preserves failed-attempt status");
    Check(!typedHw.AcceptAndDispatchScan(
              capturedFailure,
              typedDispatcher,
              hardware::ChessboardHardware::ObservationDispatchMode::Always),
          "failed attempt cannot be accepted or dispatched");
    Check(typedSm.GetLastBoardObservationSequence() == 2U,
          "failed attempt does not advance observation sequence");
    Check(typedHw.GetOccupied(56U),
          "failed attempt preserves last accepted occupancy");

    typedInput.PushSuccessfulScan(1ULL << 8U);
    typedHw.PollAndDispatch(typedDispatcher);
    Check(typedSm.GetLastBoardObservationSequence() == 3U,
          "compatibility polling captures and dispatches a changed scan once");
    Check(typedHw.GetOccupied(55U) && !typedHw.GetOccupied(56U),
          "compatibility polling maps and accepts the changed snapshot");
    typedHw.PollAndDispatch(typedDispatcher);
    Check(typedSm.GetLastBoardObservationSequence() == 3U,
          "compatibility polling ignores no-scan-due result");

    TypedAtomicBoardInput backpressuredInput(1ULL << 2U);
    hardware::ChessboardHardware backpressuredHw(backpressuredInput);
    backpressuredHw.Resync();
    runtime::GameStateMachine backpressuredSm(cfg);
    backpressuredSm.Reset();
    runtime::AppEventQueue fullQueue;
    Check(fullQueue.Push(runtime::AppEvent::MakeTick(1U)),
          "queue-full regression fills the bounded production ingress");
    runtime::InteractionDispatcher backpressuredDispatcher(
        backpressuredSm, fullQueue);

    backpressuredInput.PushSuccessfulScan(1ULL << 3U);
    const modules::BoardScanAttemptResult backpressuredScan =
        backpressuredHw.CaptureScanAttempt();
    Check(!backpressuredHw.AcceptAndDispatchScan(
              backpressuredScan, backpressuredDispatcher),
          "queue-full dispatch is not reported as accepted");
    Check(backpressuredHw.GetOccupied(2U) &&
              !backpressuredHw.GetOccupied(3U),
          "queue-full dispatch preserves the last accepted occupancy");
    Check(backpressuredHw.GetSensorChangeCount() == 0U,
          "queue-full dispatch preserves accepted sensor count");
    Check(backpressuredSm.GetLastBoardObservationSequence() == 0U,
          "queue-full dispatch cannot reach or advance game state");

    runtime::AppEvent queuedEvent;
    Check(fullQueue.TryPop(queuedEvent),
          "queue-full regression releases the occupied ingress slot");
    Check(backpressuredHw.AcceptAndDispatchScan(
              backpressuredScan, backpressuredDispatcher),
          "the exact rejected fresh snapshot remains retryable");
    Check(!backpressuredHw.GetOccupied(2U) &&
              backpressuredHw.GetOccupied(3U),
          "successful retry commits the fresh occupancy exactly once");
    Check(backpressuredHw.GetSensorChangeCount() == 2U,
          "successful retry counts only the actual changed squares");

    clock.AdvanceMs(1);
    (void)clock;

    if (g_failures == 0) {
        std::puts("[CHESSBOARD_HARDWARE_SENSOR_QUEUE] ok");
        return 0;
    }

    std::printf("[CHESSBOARD_HARDWARE_SENSOR_QUEUE] failures=%d\n", g_failures);
    return 1;
}
