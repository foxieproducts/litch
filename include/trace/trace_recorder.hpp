#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "runtime/game_state_machine.hpp"
#include "trace/support_action_history.hpp"

namespace modules {
struct GameVariantStateSnapshot;
} // namespace modules

namespace trace {

struct TraceSettingsSnapshot {
    uint32_t brightness = 5;
    uint32_t gameFlipSides = 0;
    uint32_t gameFirstPlayer = 1;
    uint32_t vsCpuLevel = 0;
    uint32_t vsCpuPlayer = 2;
    uint32_t timer = 0;
    uint32_t takebackGraceSeconds = 3;
};

class TraceRecorder {
  public:
    TraceRecorder() = default;

    SupportActionHistory& UserActions() { return m_userActions; }
    const SupportActionHistory& UserActions() const { return m_userActions; }

    bool IsActive() const { return m_active; }

    void Start(const char* buildId, const char* boardId, uint32_t rngSeed, const TraceSettingsSnapshot& settings);
    void Stop();
    void RecordSettings(const TraceSettingsSnapshot& settings);

    void RecordTick();
    void RecordSensorDelta(uint8_t square, bool nowOccupied);
    void RecordBoardObservation(uint64_t changedMask,
                                uint64_t occupiedMask,
                                uint32_t sequence,
                                uint8_t deferredDuckDestinationSquare = 255U);
    void RecordButton(const char* id, const char* evt);
    void RecordControl(uint32_t sequence, const char* control, uint8_t count);
    void RecordPositionAcknowledged(uint32_t sequence,
                                    uint64_t occupiedMask,
                                    uint64_t kingMask,
                                    uint64_t rookMask,
                                    uint64_t moverMask = 0,
                                    uint64_t opponentMask = 0,
                                    uint64_t pawnMask = 0,
                                    uint8_t enPassantTargetSquare = 255U,
                                    uint8_t enPassantCaptureSquare = 255U,
                                    bool sideToMoveValid = false,
                                    bool sideToMoveBlack = false,
                                    bool legalCaptureTargetsValid = false,
                                    uint64_t legalCaptureTargetsMask = 0,
                                    bool legalCastlingTargetsValid = false,
                                    uint64_t legalCastlingKingTargetsMask = 0,
                                    const runtime::CaptureLegality& captureLegality = {});
    void RecordActionResult(uint32_t sequence, uint32_t token, bool succeeded);
    void RecordAdapterResult(uint32_t sequence,
                             const char* adapter,
                             bool succeeded);
    void RecordCmd(const char* cmd);
    void RecordCheckpointState(const char* stateLabel,
                               bool placementModeActive,
                               bool cpuThinking,
                               bool humanIllegal,
                               bool cpuIllegal);
    void RecordCheckpointInteraction(
        const runtime::ChessInteractionModel& model,
        const runtime::InteractionInvariantStatus& invariantStatus);
    void RecordCheckpointVariant(
        const modules::GameVariantStateSnapshot& variantState,
        uint64_t interactionModelHash);
    void RecordCheckpointEngine(const char* fen);
    void RecordCheckpointFrame(uint64_t frameHash, const char* kind);

  private:
    uint32_t NowTraceMs() const;

    void WriteJsonLine(const char* jsonLine);

    SupportActionHistory m_userActions;
    bool m_active = false;
    fxe::ElapsedTime m_elapsedSinceStart;
    uint32_t m_recordCount = 0;
    uint32_t m_inputEventCount = 0;
    uint32_t m_frameCheckpointCount = 0;
    uint32_t m_engineCheckpointCount = 0;
};

} // namespace trace
