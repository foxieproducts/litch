#include <cstdio>
#include <cstring>

#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "logger.hpp"
#include "runtime/clock.hpp"
#include "trace/trace_recorder.hpp"

namespace {

struct Capture {
    bool foundSettings = false;
    char settingsLine[256] = {};
    bool foundEnd = false;
    char endLine[256] = {};
    bool foundState = false;
    char stateLine[256] = {};
    bool foundBoardObservation = false;
    char boardObservationLine[256] = {};
    bool foundInteraction = false;
    uint32_t interactionCount = 0;
    bool interactionLinesComplete = true;
    char interactionLine[512] = {};
    bool foundPositionAck = false;
    bool positionAckLineComplete = false;
    char positionAckLine[256] = {};
    bool foundActionResult = false;
    bool foundAdapterResult = false;
};

} // namespace

int main() {
    fxe::test_support::TestResults results("TRACE_RECORDER");
    results.Start();

    logger::SetMinLevel(logger::Level::TRACE);
    logger::ClearBufferedLines();

    runtime::ManualClock clock;
    fxe::test_support::ManualMillisGuard millisGuard;
    clock.SetNowMs(1000);

    trace::TraceRecorder recorder;

    trace::TraceSettingsSnapshot settings;
    settings.brightness = 5;
    settings.gameFlipSides = 0;
    settings.vsCpuLevel = 0;
    settings.timer = 0;
    settings.takebackGraceSeconds = 7;

    recorder.Start("test_build", "test_board", 0, settings);
    recorder.RecordSettings(settings);

    clock.SetNowMs(1005);
    recorder.RecordCheckpointState("Idle", false, false, false, false);

    clock.SetNowMs(1010);
    recorder.RecordTick();

    clock.SetNowMs(1020);
    recorder.RecordSensorDelta(12, false);

    clock.SetNowMs(1025);
    recorder.RecordBoardObservation((1ULL << 56) | (1ULL << 60), 0, 7);
    recorder.RecordControl(8, "undo", 2);

    runtime::ChessInteractionModel interaction;
    runtime::ResetChessInteractionModel(interaction);
    interaction.phase.tag = runtime::ChessInteractionPhase::ExecutingPlan;
    interaction.phase.plan.kind = runtime::PhysicalTransitionKind::Castling;
    interaction.phase.plan.progress.relocationsLiftedMask = 3;
    interaction.phase.requiredAction = runtime::RequiredPhysicalAction::PlacePrimary;
    interaction.acknowledged.occupiedMask = (1ULL << 56) | (1ULL << 60);
    interaction.board.occupiedMask = 0;
    recorder.RecordCheckpointInteraction(
        interaction,
        runtime::InteractionInvariantStatus{});
    runtime::ChessInteractionModel longestInteraction = interaction;
    longestInteraction.phase.tag = runtime::ChessInteractionPhase::AwaitingAction;
    longestInteraction.phase.plan.kind = runtime::PhysicalTransitionKind::EnPassant;
    longestInteraction.phase.requiredAction =
        runtime::RequiredPhysicalAction::AwaitAcknowledgement;
    longestInteraction.phase.pendingActionToken = UINT32_MAX;
    longestInteraction.acknowledged.occupiedMask = UINT64_MAX;
    longestInteraction.phase.plan.progress.relocationsLiftedMask = UINT8_MAX;
    longestInteraction.phase.plan.progress.relocationsPlacedMask = UINT8_MAX;
    longestInteraction.phase.plan.progress.removalComplete = true;
    longestInteraction.phase.plan.progress.actionAcknowledged = true;
    runtime::InteractionInvariantStatus longestDivergence;
    longestDivergence.code =
        runtime::InteractionInvariantStatusCode::InvariantViolation;
    recorder.RecordCheckpointInteraction(longestInteraction, longestDivergence);
    clock.SetNowMs(UINT32_MAX);
    recorder.RecordPositionAcknowledged(
        UINT32_MAX,
        UINT64_MAX,
        UINT64_MAX,
        UINT64_MAX,
        UINT64_MAX,
        UINT64_MAX,
        UINT64_MAX,
        UINT8_MAX,
        UINT8_MAX,
        true,
        true,
        true,
        UINT64_MAX,
        true,
        UINT64_MAX);
    clock.SetNowMs(1025);
    recorder.RecordActionResult(9, 3, true);
    recorder.RecordAdapterResult(10, "persist", true);

    clock.SetNowMs(1030);
    recorder.RecordButton("P1", "press");

    clock.SetNowMs(1050);
    recorder.Stop();

    Capture cap;
    logger::DumpBufferedLines([&cap](const char* line) {
        if (!line) {
            return;
        }

        if (std::strstr(line, "\"type\":\"end\"") == nullptr) {
            if (std::strstr(line, "\"type\":\"settings\"") != nullptr) {
                cap.foundSettings = true;
                std::strncpy(cap.settingsLine, line,
                             sizeof(cap.settingsLine) - 1U);
            }
            if (std::strstr(line, "\"type\":\"checkpoint_state\"") != nullptr) {
                cap.foundState = true;
                std::strncpy(cap.stateLine, line, sizeof(cap.stateLine));
                cap.stateLine[sizeof(cap.stateLine) - 1] = '\0';
            }
            if (std::strstr(line, "\"type\":\"board_observation\"") != nullptr) {
                cap.foundBoardObservation = true;
                std::strncpy(cap.boardObservationLine, line, sizeof(cap.boardObservationLine));
                cap.boardObservationLine[sizeof(cap.boardObservationLine) - 1] = '\0';
            }
            if (std::strstr(line, "\"type\":\"checkpoint_interaction\"") != nullptr) {
                ++cap.interactionCount;
                const size_t length = std::strlen(line);
                cap.interactionLinesComplete = cap.interactionLinesComplete &&
                                               length < 256U &&
                                               length != 0U &&
                                               line[length - 1U] == '}';
                if (!cap.foundInteraction) {
                    cap.foundInteraction = true;
                    std::strncpy(cap.interactionLine, line, sizeof(cap.interactionLine));
                    cap.interactionLine[sizeof(cap.interactionLine) - 1] = '\0';
                }
            }
            if (std::strstr(line, "\"type\":\"position_ack\"") != nullptr) {
                cap.foundPositionAck = true;
                const size_t length = std::strlen(line);
                cap.positionAckLineComplete =
                    length != 0U && length < 256U &&
                    line[length - 1U] == '}';
                std::strncpy(cap.positionAckLine, line,
                             sizeof(cap.positionAckLine) - 1U);
            }
            if (std::strstr(line, "\"type\":\"action_result\"") != nullptr) {
                cap.foundActionResult = true;
            }
            if (std::strstr(line, "\"type\":\"adapter_result\"") != nullptr) {
                cap.foundAdapterResult = true;
            }
            return;
        }

        cap.foundEnd = true;
        std::strncpy(cap.endLine, line, sizeof(cap.endLine));
        cap.endLine[sizeof(cap.endLine) - 1] = '\0';
    });

    results.Check(cap.foundEnd, "end record present");
    results.Check(cap.foundSettings,
                  "settings record present");
    results.Check(std::strstr(
                      cap.settingsLine,
                      "\"TAKEBACK_GRACE_SECONDS\":7") != nullptr,
                  "settings record includes takeback grace duration");
    results.Check(cap.foundState, "state checkpoint record present");
    results.Check(cap.foundBoardObservation,
                  "atomic board observation record present");
    results.Check(cap.foundInteraction,
                  "interaction model checkpoint record present");
    results.Check(cap.interactionCount == 2U,
                  "both interaction checkpoints are buffered");
    results.Check(cap.interactionLinesComplete,
                  "worst-case interaction checkpoint fits logger line");
    results.Check(cap.foundPositionAck,
                  "position acknowledgement event record present");
    results.Check(cap.positionAckLineComplete,
                  "worst-case position acknowledgement fits logger line");
    results.Check(std::strstr(cap.positionAckLine,
                              "\"t\":4294966295") != nullptr &&
                      std::strstr(cap.positionAckLine,
                                  "\"f\":255,\"l\":\"0xffffffffffffffff\"") != nullptr,
                  "position acknowledgement uses compact canonical fields");
    results.Check(cap.foundActionResult,
                  "action result event record present");
    results.Check(cap.foundAdapterResult,
                  "adapter result event record present");

    const char* expectedState =
        "{\"type\":\"checkpoint_state\",\"t_ms\":5,\"game_state\":\"Idle\",\"placement\":0,\"cpu\":0,\"human_illegal\":0,\"cpu_illegal\":0}";
    results.Check(std::strcmp(cap.stateLine, expectedState) == 0, "state checkpoint line matches expected format");

    const char* expectedBoardObservation =
        "{\"type\":\"board_observation\",\"t_ms\":25,\"seq\":7,\"changed_mask\":\"0x1100000000000000\",\"occupied_mask\":\"0x0000000000000000\"}";
    results.Check(std::strcmp(cap.boardObservationLine,
                              expectedBoardObservation) == 0,
                  "atomic board observation line matches expected format");

    const char* expectedInteractionPrefix =
        "{\"type\":\"checkpoint_interaction\",\"t_ms\":25,\"ph\":\"ExecutingPlan\",\"pl\":\"Castling\",\"req\":\"PlacePrimary\",\"exp\":\"0x1100000000000000\",\"l\":3,\"p\":0,\"r\":0,\"s\":0,\"a\":0,\"tok\":0,\"h\":\"0x";
    results.Check(std::strstr(cap.interactionLine,
                              expectedInteractionPrefix) == cap.interactionLine,
                  "interaction checkpoint includes stable phase and progress");
    results.Check(std::strstr(cap.interactionLine,
                              "\",\"d\":\"None\"}") != nullptr,
                  "interaction checkpoint includes model hash and divergence");

    const char* expected =
        "{\"type\":\"end\",\"t_ms\":50,\"stats\":{\"events\":8,\"frames\":0,\"engine\":0}}";
    results.Check(std::strcmp(cap.endLine, expected) == 0, "end record line matches expected format");

    return results.Finish();
}
