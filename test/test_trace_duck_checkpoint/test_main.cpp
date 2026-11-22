#include <cstring>

#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "logger.hpp"
#include "modules/game_variant_state.hpp"
#include "runtime/clock.hpp"
#include "trace/trace_jsonl_event_reader.hpp"
#include "trace/trace_recorder.hpp"

int main() {
    fxe::test_support::TestResults results("TRACE_DUCK_CHECKPOINT");
    results.Start();

    logger::SetMinLevel(logger::Level::TRACE);
    logger::ClearBufferedLines();
    char checkpoint[256] = {};
    char interactionCheckpoint[256] = {};
    logger::SetOutputLineCallback(
        [&checkpoint, &interactionCheckpoint](const char* line) {
            if (line == nullptr) {
                return;
            }
            if (std::strstr(
                    line, "\"type\":\"checkpoint_variant\"") != nullptr) {
                std::strncpy(checkpoint, line, sizeof(checkpoint) - 1U);
            }
            if (std::strstr(
                    line, "\"type\":\"checkpoint_interaction\"") != nullptr) {
                std::strncpy(interactionCheckpoint, line,
                             sizeof(interactionCheckpoint) - 1U);
            }
        });
    runtime::ManualClock clock;
    fxe::test_support::ManualMillisGuard millisGuard;
    clock.SetNowMs(1000U);

    trace::TraceRecorder recorder;
    trace::TraceSettingsSnapshot settings;
    recorder.Start("test", "native", 1U, settings);

    runtime::ChessInteractionModel interaction;
    runtime::ResetChessInteractionModel(interaction);
    modules::GameVariantStateSnapshot duck;
    duck.variant = modules::GameVariant::Duck;
    duck.duckSquare = 18U;
    duck.duckTurnPhase = modules::DuckTurnPhase::DuckRelocation;
    duck.pendingChessMove = true;
    duck.pendingMoveFrom = 52U;
    duck.pendingMoveTo = 36U;
    duck.pendingPromotionSuffix = 'q';
    recorder.RecordCheckpointInteraction(
        interaction, runtime::InteractionInvariantStatus{});
    recorder.RecordCheckpointVariant(
        duck, runtime::ComputeChessInteractionModelHash(interaction));
    recorder.Stop();
    logger::SetOutputLineCallback(nullptr);
    const size_t length = std::strlen(checkpoint);
    results.Check(length > 0U && length < 256U &&
                      checkpoint[length - 1U] == '}',
                  "Duck checkpoint fits bounded trace line");

    trace::JsonlEventRecord record;
    results.Check(trace::TryParseJsonlEventRecord(checkpoint, record),
                  "Duck checkpoint parses");
    results.Check(record.type == trace::JsonlEventType::CheckpointVariant,
                  "Duck checkpoint uses bounded variant record");
    results.Check(record.hasGameVariant && record.gameVariant == 2U &&
                      record.hasDuckSquare && record.duckSquare == 18U &&
                      record.hasDuckTurnPhase && record.duckTurnPhase == 1U,
                  "Duck identity fields are recorded");
    results.Check(record.hasPendingChessMove && record.pendingChessMove &&
                      record.hasPendingMoveFrom && record.pendingMoveFrom == 52U &&
                      record.hasPendingMoveTo && record.pendingMoveTo == 36U &&
                      record.hasPendingPromotion && record.pendingPromotion == 'q',
                  "pending Duck move is recorded");
    results.Check(record.hasVariantOutcomeReason &&
                      record.variantOutcomeReason == 0U &&
                      record.hasVariantWinner && record.variantWinner == 0U,
                  "Duck outcome is recorded");
    const uint64_t modelHash =
        runtime::ComputeChessInteractionModelHash(interaction);
    trace::JsonlEventRecord interactionRecord;
    results.Check(trace::TryParseJsonlEventRecord(
                      interactionCheckpoint, interactionRecord) &&
                      interactionRecord.hasModelHash &&
                      interactionRecord.modelHash == modelHash,
                  "paired interaction checkpoint retains legacy model hash");
    results.Check(record.hasVariantStateHash &&
                      record.variantStateHash ==
                          modules::ComputeGameVariantStateHash(duck),
                  "variant hash includes Duck state");
    results.Check(record.hasAppStateHash &&
                      record.appStateHash ==
                          modules::ComputeAppStateHash(modelHash, duck),
                  "app hash includes Duck state");

    return results.Finish();
}
