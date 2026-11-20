#pragma once

#include <cassert>
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

namespace duck_coordinator_test {

inline constexpr uint64_t INITIAL_OCCUPANCY =
    0xFFFF00000000FFFFULL;

class FakeEngine final : public modules::IEngine {
  public:
    FakeEngine() { Reset(); }

    void Reset() override {
        (void)m_fen.Assign(modules::INITIAL_FEN);
        ++resetCalls;
    }

    bool SetFen(const char* fen) override {
        ++setFenCalls;
        return fen != nullptr && m_fen.Assign(fen);
    }

    bool GetFen(char* outFen, const size_t outSize) const override {
        if (outFen == nullptr || outSize <= m_fen.Size()) {
            return false;
        }
        std::memcpy(outFen, m_fen.CStr(), m_fen.Size() + 1U);
        return true;
    }

    bool ApplyUciMove(const char* uci) override {
        ++applyMoveCalls;
        if (uci == nullptr) {
            return false;
        }
        // Undo-history reconstruction only needs a deterministic pseudo-legal
        // chess replay. The neutral piece is absent from FEN, so select the
        // first empty square that does not obstruct this move.
        for (uint8_t duckSquare = 0U; duckSquare < 64U; ++duckSquare) {
            litch_max::DuckChessMoveResult result;
            if (litch_max::TryApplyDuckMove(
                    m_fen.CStr(), duckSquare, uci, result)) {
                return m_fen.Assign(result.resultingFen.CStr());
            }
        }
        return false;
    }

    modules::SideToMove GetSideToMove() const override {
        return std::strstr(m_fen.CStr(), " b ") != nullptr
                   ? modules::SideToMove::Black
                   : modules::SideToMove::White;
    }

    bool IsInCheck(modules::SideToMove) const override { return false; }
    bool IsInCheckmate(modules::SideToMove) const override { return false; }
    bool IsInStalemate(modules::SideToMove) const override { return false; }

    bool GetLegalDestinationsMask(uint8_t, uint64_t& outMask) override {
        ++legalQueryCalls;
        outMask = 0ULL;
        return true;
    }

    bool StartThink(const modules::ThinkConfig&) override {
        ++thinkStartCalls;
        return false;
    }

    void CancelThink() override { ++cancelThinkCalls; }

    modules::ThinkState PollThinkProgress(
        uint8_t& outProgress,
        modules::UciMove& outMove) override {
        outProgress = 0U;
        outMove.Clear();
        return modules::ThinkState::Idle;
    }

    bool GetLastThinkStats(modules::ThinkStats&) const override {
        return false;
    }

    const char* Fen() const { return m_fen.CStr(); }

    uint32_t resetCalls = 0U;
    uint32_t setFenCalls = 0U;
    uint32_t applyMoveCalls = 0U;
    uint32_t legalQueryCalls = 0U;
    uint32_t thinkStartCalls = 0U;
    uint32_t cancelThinkCalls = 0U;

  private:
    modules::FenString m_fen;
};

inline modules::SettingsStoreConfig MakeSettingsConfig(const char* path) {
    modules::SettingsStoreConfig config;
    config.path = path;
    config.deferredSaveDelayMs = 999999U;
    config.mustSaveDelayMs = 100U;
    return config;
}

inline runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

inline void RemoveSettingsFiles(const char* path) {
    (void)std::remove(path);
    char sibling[192] = {};
    (void)std::snprintf(sibling, sizeof(sibling), "%s.bak", path);
    (void)std::remove(sibling);
    (void)std::snprintf(sibling, sizeof(sibling), "%s.tmp", path);
    (void)std::remove(sibling);
}

struct Harness {
    explicit Harness(const char* path, const bool clearStore = true)
        : settingsConfig(MakeSettingsConfig(path)),
          stateMachineConfig(MakeStateMachineConfig()),
          settings(settingsConfig),
          stateMachine(stateMachineConfig),
          coordinator(settings, engine, stateMachine) {
        if (clearStore) {
            RemoveSettingsFiles(path);
        }
        stateMachine.Reset();
    }

    bool LoadFresh(const bool enableClock = false) {
        if (!settings.Load()) {
            return false;
        }
        settings.Doc()["VS_CPU_LEVEL"] = 0U;
        settings.Doc()["TIMER"] = enableClock ? 1U : 0U;
        return coordinator.LoadOrInitialize();
    }

    bool LoadExisting() {
        return settings.Load() && coordinator.LoadOrInitialize();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateMachineConfig;
    modules::SettingsStore settings;
    FakeEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator coordinator;
};

inline uint64_t FenOccupancy(const char* fen) {
    uint64_t occupied = 0ULL;
    uint8_t rank = 0U;
    uint8_t file = 0U;
    for (const char* cursor = fen;
         cursor != nullptr && *cursor != '\0' && *cursor != ' ';
         ++cursor) {
        if (*cursor == '/') {
            ++rank;
            file = 0U;
        } else if (*cursor >= '1' && *cursor <= '8') {
            file = static_cast<uint8_t>(file + (*cursor - '0'));
        } else {
            occupied |= 1ULL << static_cast<uint8_t>(rank * 8U + file);
            ++file;
        }
    }
    return occupied;
}

inline void Observe(runtime::GameStateMachine& stateMachine,
                    const uint64_t changed,
                    const uint64_t occupied) {
    stateMachine.HandleBoardObservation(
        changed, occupied, fxe::Millis(),
        stateMachine.GetLastBoardObservationSequence() + 1U);
    // These coordinator fixtures observe completed Duck turns. Exercise the
    // real timer (normally shortened to 10 ms here), rather than treating a
    // valid human placement as an immediate action. Exact deadline and relift
    // assertions live in the production Duck settling VirtualLitch journey.
    const auto& phase = stateMachine.GetChessInteractionModel().phase;
    if (phase.plan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
        phase.tag == runtime::ChessInteractionPhase::Settling) {
        assert(!stateMachine.HasCommittedMovePending() &&
               "selectable Duck placement has no action before confirmation");
        uint32_t elapsed = 0U;
        uint32_t duration = 0U;
        if (stateMachine.TryGetSettleElapsedAndDurationMs(elapsed, duration)) {
            fxe::ManualMillis::AdvanceMs(duration - elapsed);
            stateMachine.Tick();
        }
    }
}

inline bool SeedPersistedDuckPosition(Harness& harness,
                                      const char* fen,
                                      const uint8_t duckSquare,
                                      const modules::VariantOutcome outcome = {}) {
    if (!harness.settings.Load()) {
        return false;
    }
    modules::GamePersistenceState state;
    if (!state.fen.Assign(fen) || !state.undoBaseFen.Assign(fen)) {
        return false;
    }
    modules::DuckGameDurableState duck;
    duck.variant = modules::GameVariant::Duck;
    duck.duckSquare = duckSquare;
    duck.outcome = outcome;
    char encoded[32] = {};
    if (!modules::EncodeDuckGameDurableState(
            duck, encoded, sizeof(encoded)) ||
        !state.variantState.Assign(encoded) ||
        !modules::WriteGamePersistenceState(harness.settings.Doc(), state)) {
        return false;
    }
    return harness.settings.SaveNow();
}

} // namespace duck_coordinator_test
