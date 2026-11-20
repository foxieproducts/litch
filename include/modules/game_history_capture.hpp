#pragma once

#include <cstdint>

#include "modules/game_history_journal.hpp"

namespace modules::game_history {

struct GameHistoryCaptureStep {
    Transition transition{};
    Position afterPosition{};
};

using GameHistoryCaptureStepReader = bool (*)(
    void* context, uint16_t index, GameHistoryCaptureStep& outStep);

// Immutable, allocation-free view of one successfully persisted current-game
// snapshot. The reader is called synchronously and may reuse its own storage.
struct GameHistoryCaptureSnapshot {
    GameId gameId{};
    uint64_t lineRevision = 0U;
    Variant variant = Variant::Classic;
    uint16_t basePly = 0U;
    Position basePosition{};
    uint16_t transitionCount = 0U;
    void* stepContext = nullptr;
    GameHistoryCaptureStepReader readStep = nullptr;
    bool shouldSeal = false;
    ArchiveStatus status = ArchiveStatus::Unfinished;
    Outcome outcome = Outcome::Ongoing;
    Winner winner = Winner::None;

    [[nodiscard]] uint16_t CurrentPly() const {
        return static_cast<uint16_t>(basePly + transitionCount);
    }
};

enum class GameHistoryCaptureResult : uint8_t {
    Applied = 0U,
    NoChange,
    Bounds,
    Corrupt,
    Capacity,
    IoFailure,
    Exhausted,
};

// Reconciles one durable settings snapshot into the bounded outbox. It never
// owns filesystem or task lifecycle; callers invoke it only after the matching
// settings snapshot is durable and from the storage worker lane.
class GameHistoryCapture final {
  public:
    explicit GameHistoryCapture(GameHistoryJournal& journal)
        : m_journal(journal) {}

    GameHistoryCaptureResult Reconcile(
        const GameHistoryCaptureSnapshot& snapshot);

  private:
    bool ValidateSnapshot(const GameHistoryCaptureSnapshot& snapshot) const;
    bool BuildPriorSeal(const Record& latest, Record& seal) const;
    bool ReadPositionAt(const GameHistoryCaptureSnapshot& snapshot,
                        uint16_t ply,
                        Position& outPosition) const;
    GameHistoryCaptureResult AppendLineBase(
        const GameHistoryCaptureSnapshot& snapshot);
    GameHistoryCaptureResult AppendRebasedLine(
        GameHistoryCaptureSnapshot& snapshot);
    GameHistoryCaptureResult AppendSuffix(
        const GameHistoryCaptureSnapshot& snapshot,
        uint16_t firstPly, const Position* continuation);
    GameHistoryCaptureResult AppendSeal(
        const GameId& gameId,
        uint64_t lineRevision,
        ArchiveStatus status,
        Outcome outcome,
        Winner winner);
    GameHistoryCaptureResult MapMutation(
        JournalMutationResult result) const;

    GameHistoryJournal& m_journal;
};

} // namespace modules::game_history
