#include "bounded_hint_engine.hpp"
#include "virtual_litch_frame_checks.hpp"
#include <array>
#include "foxie_embedded/millis.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "virtual_litch_session.hpp"
#include "hardware/led_layout.hpp"
#include "modules/async_think_worker.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/coach_insight.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/square_mapping.hpp"

int RunCpuDuckUndoVirtualLitchJourneys();
int RunCpuDuckDurabilityVirtualLitchJourneys();
int RunSettleEffectsVirtualLitchJourneys();
int RunRapidSettleHandoffVirtualLitchJourneys();
int RunOConfirmVirtualLitchJourneys();

namespace {

constexpr auto PHYSICAL_TOPOLOGY =
    hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
constexpr uint64_t INITIAL_OCCUPANCY = UINT64_C(0xFFFF00000000FFFF);
constexpr const char* INITIAL_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

struct Move {
    uint8_t from;
    uint8_t to;
};

struct CastleJourney {
    const char* name;
    std::array<Move, 10> setupMoves;
    size_t setupMoveCount;
    uint8_t kingFrom;
    uint8_t kingTo;
    uint8_t rookFrom;
    uint8_t rookTo;
    uint8_t recoverySquare;
    bool moverBlack;
};

struct CpuCastleJourney {
    const char* name;
    const char* fen;
    uint64_t before;
    uint8_t kingFrom;
    uint8_t kingTo;
    uint8_t rookFrom;
    uint8_t rookTo;
    bool moverBlack;
};

enum class JourneyExit : uint8_t {
    Complete,
    Cancel,
    CorrectAndComplete,
};

enum class CastleScan : uint8_t {
    SplitKing,
    AtomicKing,
    AtomicKingWithRookHeld,
};

enum class AssistanceMode : uint8_t {
    None,
    Hint,
    Coaching,
};

int g_failures = 0;

void Check(const bool condition, const char* journey, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s: %s\n", journey, message);
    ++g_failures;
}

// Keeps real mcu-max position/rules ownership while making only the background
// search boundary deterministic for raw VirtualLitch journeys.
class ControlledCoachHintEngine final : public modules::IEngine {
  public:
    ControlledCoachHintEngine() {
        (void)m_initialMove.Assign("a2a3");
        (void)m_automaticMove.Assign("e2e4");
    }

    void Reset() override {
        m_rules.Reset();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_startCount = 0U;
        m_completionCount = 0U;
        m_resultReadCount = 0U;
        m_unsafeResultReadCount = 0U;
        m_lastDifficulty = 0U;
        m_active = false;
        m_cancelRequested = false;
        m_tailReleased = false;
        m_tailEntryCount = 0U;
        m_cancellationTailReleased = false;
        m_cancellationTailEntryCount = 0U;
        m_lastState = modules::ThinkState::Idle;
        m_lastMove.Clear();
    }

    bool SetFen(const char* fen) override { return m_rules.SetFen(fen); }
    bool GetFen(char* outFen, const size_t outSize) const override {
        return m_rules.GetFen(outFen, outSize);
    }
    bool ApplyUciMove(const char* move) override {
        return m_rules.ApplyUciMove(move);
    }
    modules::SideToMove GetSideToMove() const override {
        return m_rules.GetSideToMove();
    }
    bool IsInCheck(const modules::SideToMove side) const override {
        return m_rules.IsInCheck(side);
    }
    bool IsInCheckmate(const modules::SideToMove side) const override {
        return m_rules.IsInCheckmate(side);
    }
    bool IsInStalemate(const modules::SideToMove side) const override {
        return m_rules.IsInStalemate(side);
    }
    bool GetLegalDestinationsMask(
        const uint8_t fromSquare,
        uint64_t& outDestMask64) override {
        return m_rules.GetLegalDestinationsMask(
            fromSquare, outDestMask64);
    }

    bool StartThink(const modules::ThinkConfig& config) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_startCount;
        m_lastDifficulty = config.difficulty;
        m_lastConfig = config;
        m_lastStartAtMs = fxe::Millis();
        m_currentSearch = m_startCount;
        m_cancelRequested = false;
        m_tailReleased = false;
        m_cancellationTailReleased = false;
        if (m_currentSearch > 1U && m_failAutomaticStartsRemaining != 0U) {
            --m_failAutomaticStartsRemaining;
            m_active = false;
            return false;
        }
        m_active = true;
        return true;
    }

    // Controlled engine-boundary publication while the real async worker
    // remains active. Rules and physical session ownership are unchanged.
    void PublishLiveMove(const char* move) {
        std::lock_guard<std::mutex> lock(m_mutex);
        litch_max::ParsedUciMove parsed;
        if (!litch_max::TryParseUciMove(move, parsed)) { return; }
        (void)(m_currentSearch > 1U ? m_automaticMove : m_initialMove).Assign(move);
        modules::ThinkCandidate candidate;
        candidate.fromSquare = parsed.fromSquare;
        candidate.toSquare = parsed.toSquare;
        candidate.promotion = parsed.promotionSuffixLower;
        // The scripted boundary represents a completed scored root. Rules,
        // worker ownership and physical delivery remain production code.
        candidate.completedDepth = 1U;
        candidate.rootKind = modules::ThinkRootKind::ClassicFull;
        candidate.scoreUnits = modules::ThinkScoreUnits::ClassicPawn100;
        candidate.scorePerspective = GetSideToMove();
        candidate.scoreValid = true;
        candidate.completedNodes = 1U;
        m_lastConfig.candidateObserver.RootCompleted(candidate);
        m_lastConfig.candidateObserver.Publish(candidate);
    }

    void CancelThink() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelRequested = true;
        m_changed.notify_all();
    }

    modules::ThinkState PollThinkProgress(
        uint8_t& outProgressPercent,
        modules::UciMove& outBestMoveUci) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        outProgressPercent = 0U;
        outBestMoveUci.Clear();
        if (m_cancelRequested) {
            WaitAtCancellationTail(lock);
            m_active = false;
            m_lastState = modules::ThinkState::Canceled;
            ++m_completionCount;
            m_changed.notify_all();
            return m_lastState;
        }
        if (!m_active) {
            return modules::ThinkState::Idle;
        }

        const uint32_t searchNumber = m_currentSearch;
        if ((searchNumber == 1U && m_holdInitialTail) ||
            (searchNumber > 1U && m_holdAutomaticTail)) {
            ++m_tailEntryCount;
            m_changed.notify_all();
            (void)m_changed.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() { return m_tailReleased || m_cancelRequested; });
        }

        if (m_cancelRequested) {
            WaitAtCancellationTail(lock);
            m_lastState = modules::ThinkState::Canceled;
            m_lastMove.Clear();
        } else {
            m_lastState = modules::ThinkState::Done;
            if (searchNumber > 1U &&
                m_automaticFailuresRemaining != 0U) {
                --m_automaticFailuresRemaining;
                m_lastState = m_automaticFailureState;
            }
            const char* move = searchNumber > 1U
                                   ? m_automaticMove.CStr()
                                   : m_initialMove.CStr();
            if (m_lastState == modules::ThinkState::Done) {
                (void)m_lastMove.Assign(move);
                outProgressPercent = 100U;
                outBestMoveUci = m_lastMove;
                litch_max::ParsedUciMove parsed;
                if (litch_max::TryParseUciMove(move, parsed)) {
                    modules::ThinkCandidate candidate;
                    candidate.fromSquare = parsed.fromSquare;
                    candidate.toSquare = parsed.toSquare;
                    candidate.promotion = parsed.promotionSuffixLower;
                    candidate.completedDepth = 1U;
                    candidate.rootKind = modules::ThinkRootKind::ClassicFull;
                    candidate.scoreUnits = modules::ThinkScoreUnits::ClassicPawn100;
                    candidate.scorePerspective = GetSideToMove();
                    candidate.scoreValid = true;
                    candidate.completedNodes = 1U;
                    m_lastConfig.candidateObserver.RootCompleted(candidate);
                    m_lastConfig.candidateObserver.Publish(candidate);
                }
            } else {
                m_lastMove.Clear();
            }
        }
        m_active = false;
        ++m_completionCount;
        m_changed.notify_all();
        return m_lastState;
    }

    bool GetLastThinkStats(modules::ThinkStats& outStats) const override {
        outStats.nodeCount = 1U;
        return true;
    }

    bool GetLastClassicThinkResult(
        modules::ClassicThinkResult& outResult) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_resultReadCount;
        if (m_active) {
            ++m_unsafeResultReadCount;
        }
        outResult = modules::ClassicThinkResult{};
        litch_max::ParsedUciMove parsed;
        const char* analyzedMove = m_mismatchAutomaticMetadata &&
                                           m_currentSearch > 1U
                                       ? m_initialMove.CStr()
                                       : m_lastMove.CStr();
        if (m_lastState != modules::ThinkState::Done ||
            !litch_max::TryParseUciMove(analyzedMove, parsed)) {
            return false;
        }
        outResult.candidates[0] = {
            parsed.fromSquare,
            parsed.toSquare,
            parsed.hasPromotion ? parsed.promotionSuffixLower : '\0',
        };
        outResult.candidateCount = 1U;
        outResult.bestScoreValid = true;
        outResult.bestMoveFacts = modules::ClassicThinkOutcomeKnown;
        return true;
    }

    bool WaitForCompletion(const uint32_t count) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() { return m_completionCount >= count; });
    }

    bool WaitForTailEntry(const uint32_t count = 1U) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock,
            std::chrono::seconds(1),
            [&]() { return m_tailEntryCount >= count; });
    }

    void ReleaseTail() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tailReleased = true;
        m_changed.notify_all();
    }

    void SetAutomaticFailures(const modules::ThinkState state,
                            const uint8_t count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_automaticFailureState = state;
        m_automaticFailuresRemaining = count;
    }

    void SetFailAutomaticStarts(const uint8_t count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failAutomaticStartsRemaining = count;
    }

    void SetHoldAutomaticTail(const bool hold) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_holdAutomaticTail = hold;
    }

    void SetHoldInitialTail(const bool hold) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_holdInitialTail = hold;
    }

    void SetHoldCancellationTail(const bool hold) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_holdCancellationTail = hold;
    }

    bool WaitForCancellationTail(const uint32_t count = 1U) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock,
            std::chrono::seconds(1),
            [&]() { return m_cancellationTailEntryCount >= count; });
    }

    void ReleaseCancellationTail() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancellationTailReleased = true;
        m_changed.notify_all();
    }

    void SetScriptedMoves(const char* initialMove,
                          const char* automaticMove) {
        std::lock_guard<std::mutex> lock(m_mutex);
        (void)m_initialMove.Assign(initialMove);
        (void)m_automaticMove.Assign(automaticMove);
    }

    void SetMismatchAutomaticMetadata(const bool mismatch) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mismatchAutomaticMetadata = mismatch;
    }

    uint32_t StartCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_startCount;
    }
    uint32_t LastStartAtMs() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastStartAtMs;
    }
    uint8_t LastDifficulty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastDifficulty;
    }
    modules::ThinkConfig LastConfig() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastConfig;
    }
    uint32_t ResultReadCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resultReadCount;
    }
    uint32_t UnsafeResultReadCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_unsafeResultReadCount;
    }
    uint32_t TailEntryCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tailEntryCount;
    }
    uint32_t CancellationTailEntryCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancellationTailEntryCount;
    }

  private:
    void WaitAtCancellationTail(std::unique_lock<std::mutex>& lock) {
        if (!m_holdCancellationTail) {
            return;
        }
        ++m_cancellationTailEntryCount;
        m_changed.notify_all();
        (void)m_changed.wait_for(
            lock,
            std::chrono::seconds(3),
            [&]() { return m_cancellationTailReleased; });
    }

    modules::LitchMaxEngine m_rules;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    uint32_t m_startCount = 0U;
    uint32_t m_completionCount = 0U;
    mutable uint32_t m_resultReadCount = 0U;
    mutable uint32_t m_unsafeResultReadCount = 0U;
    uint32_t m_currentSearch = 0U;
    uint8_t m_lastDifficulty = 0U;
    modules::ThinkConfig m_lastConfig;
    uint32_t m_lastStartAtMs = 0U;
    bool m_active = false;
    bool m_cancelRequested = false;
    bool m_holdInitialTail = false;
    bool m_holdAutomaticTail = true;
    bool m_holdCancellationTail = false;
    bool m_tailReleased = false;
    uint32_t m_tailEntryCount = 0U;
    bool m_cancellationTailReleased = false;
    uint32_t m_cancellationTailEntryCount = 0U;
    uint8_t m_failAutomaticStartsRemaining = 0U;
    uint8_t m_automaticFailuresRemaining = 0U;
    bool m_mismatchAutomaticMetadata = false;
    modules::ThinkState m_automaticFailureState = modules::ThinkState::Error;
    modules::ThinkState m_lastState = modules::ThinkState::Idle;
    modules::UciMove m_lastMove;
    modules::UciMove m_initialMove;
    modules::UciMove m_automaticMove;
};

uint16_t ColorEnergy(const virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

bool SameOwnerHue(const virtual_litch_color a, const virtual_litch_color b) {
    const uint8_t left[]{a.r, a.g, a.b}, right[]{b.r, b.g, b.b};
    if ((a.r | a.g | a.b) == 0U || (b.r | b.g | b.b) == 0U) return false;
    for (uint8_t i = 0U; i < 3U; ++i) {
        if ((left[i] == 0U) != (right[i] == 0U)) return false;
        for (uint8_t j = 0U; j < 3U; ++j) {
            const int delta = static_cast<int>(left[i]) * right[j] - static_cast<int>(left[j]) * right[i];
            if (delta < -510 || delta > 510) return false;
        }
    }
    return true;
}

bool ColorsEqual(const virtual_litch_color lhs,
                 const virtual_litch_color rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b &&
           lhs.w == rhs.w;
}

bool IsYellowDominant(const virtual_litch_color color) {
    return color.r > color.b && color.g > color.b;
}

bool StatusIsFullRedFault(const virtual_litch_semantic_frame& frame) {
    for (const virtual_litch_color color : frame.status) {
        if (ColorEnergy(color) == 0U || color.r <= color.g ||
            color.r <= color.b) {
            return false;
        }
    }
    return true;
}

bool StatusIsQuiet(const runtime::VirtualLitchSession& session) {
    // These reset fixtures have no BLE, health, or beginner-mode badges.
    // The current Placement contract deliberately leaves gameplay edges quiet.
    for (uint8_t edge = 0U; edge < hardware::EDGE_LED_COUNT; ++edge) {
        if (ColorEnergy(session.SemanticFrame().status[edge]) != 0U ||
            ColorEnergy(session.PhysicalFrame().pixels[
                PHYSICAL_TOPOLOGY.physicalBoardLedCount + edge]) != 0U) {
            return false;
        }
    }
    return true;
}

uint64_t MapMask(const uint64_t logicalMask, const bool rotate) {
    if (!rotate) {
        return logicalMask;
    }
    uint64_t physicalMask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logicalMask & (UINT64_C(1) << square)) != 0ULL) {
            physicalMask |= UINT64_C(1)
                            << runtime::MapSquareIndex(square, true);
        }
    }
    return physicalMask;
}

virtual_litch_color PhysicalSquareColor(
    const virtual_litch_physical_frame& frame,
    const uint8_t physicalSquare,
    const hardware::SquareLedSlot slot,
    const hardware::BoardRevision revision = hardware::BoardRevision::V3S3) {
    const auto topology = hardware::BoardLedTopologyFor(revision);
    Check(frame.led_count == topology.physicalBoardLedCount + topology.physicalStatusLedCount,
          "physicalprofile", "frame matches the selected board topology");
    const auto targets = hardware::MapSemanticSquareLedTargets(
        revision, physicalSquare, slot, false);
    return frame.pixels[targets.indices[0U]];
}

uint16_t PhysicalSquareEnergy(
    const virtual_litch_physical_frame& frame,
    const uint8_t physicalSquare,
    const hardware::SquareLedSlot slot) {
    return ColorEnergy(PhysicalSquareColor(frame, physicalSquare, slot));
}

bool HasSquareGuidance(const virtual_litch_physical_frame& frame,
                       const uint8_t physicalSquare,
                       const hardware::BoardRevision revision = hardware::BoardRevision::V3S3) {
    return ColorEnergy(PhysicalSquareColor(
               frame, physicalSquare, hardware::SquareLedSlot::Square0, revision)) != 0U ||
           ColorEnergy(PhysicalSquareColor(
               frame, physicalSquare, hardware::SquareLedSlot::Square2, revision)) != 0U;
}

uint8_t PhysicalOwnerValue(const modules::UiOwner owner) {
    return owner == modules::UiOwner::P2
               ? VIRTUAL_LITCH_OWNER_P2
               : VIRTUAL_LITCH_OWNER_P1;
}

bool HasPhysicalCoachingBadge(const virtual_litch_physical_frame& frame,
                             const modules::UiOwner owner) {
    return virtual_litch_test::HasPhysicalCoachingBadge(
        frame, owner, hardware::BoardRevision::V3S3);
}

uint8_t CoachingBitForPhysicalOwner(
    const runtime::VirtualLitchSession& session,
    const modules::UiOwner physicalOwner) {
    const bool squareRotate = modules::ComputeControlBankSwap(
        session.BoardRotate(), session.FirstPlayerIsP2(), false);
    const modules::UiOwner logicalOwner =
        modules::MapOwnerToPhysicalHomeEdge(physicalOwner, squareRotate);
    return logicalOwner == modules::UiOwner::P2 ? 2U : 1U;
}

virtual_litch_config MakeConfig(const bool firstPlayerIsP2,
                                   const bool boardRotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.first_player_is_p2 = firstPlayerIsP2 ? 1U : 0U;
    config.board_rotate = boardRotate ? 1U : 0U;
    config.square_rotate_for_output =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2) ? 1U : 0U;
    return config;
}

modules::UiOwner PhysicalOwnerForSide(
    const modules::SideToMove side,
    const bool firstPlayerIsP2,
    const bool squareRotate) {
    const modules::UiOwner logicalOwner =
        modules::led_frame_composer::OwnerForSideToMove(
            side, firstPlayerIsP2);
    return modules::MapOwnerToPhysicalHomeEdge(
        logicalOwner, squareRotate);
}

bool SetPhysicalODown(runtime::VirtualLitchSession& session,
                      const modules::UiOwner physicalOwner,
                      const bool down) {
    return session.SetButtonDown(
               PhysicalOwnerValue(physicalOwner),
               runtime::VirtualLitchPhysicalButton::O,
               down) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK;
}

bool ToggleCoaching(runtime::VirtualLitchSession& session,
                    const modules::UiOwner physicalOwner,
                    const bool expectedEnabled,
                    const char* runName,
                    const uint8_t expectedHomeMask = UINT8_MAX) {
    bool gestureAccepted = SetPhysicalODown(
        session, physicalOwner, true);
    for (uint8_t sample = 0U; sample < 11U; ++sample) {
        gestureAccepted = gestureAccepted &&
                          session.Advance(100U) == VIRTUAL_LITCH_OK;
    }
    gestureAccepted = gestureAccepted && SetPhysicalODown(
        session, physicalOwner, false);
    modules::ControlMutationOutcome outcome;
    const bool hasOutcome =
        session.TryGetLastBoardControlOutcome(outcome);
    Check(gestureAccepted && hasOutcome &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.action == modules::ControlAction::Coaching,
          runName,
          expectedEnabled
              ? "physical O hold enables Coaching through production controls"
              : "physical O hold disables Coaching through production controls");
    const auto home = session.HomeStateLine();
    const uint8_t resolvedHomeMask =
        expectedHomeMask == UINT8_MAX
            ? (expectedEnabled
                   ? CoachingBitForPhysicalOwner(session, physicalOwner)
                   : 0U)
            : expectedHomeMask;
    char expectedHomeState[12] = {};
    (void)std::snprintf(expectedHomeState,
                        sizeof(expectedHomeState),
                        "\"co\":%u",
                        static_cast<unsigned>(resolvedHomeMask));
    const bool homeProjected =
        std::strstr(home.CStr(), expectedHomeState) != nullptr;
    Check(homeProjected,
          runName,
          "normalized physical O release immediately projects the exact logical P1/P2 Home latch mask");
    const bool advanced = session.Advance(1000U) == VIRTUAL_LITCH_OK;
    const bool badgeVisible = HasPhysicalCoachingBadge(
        session.PhysicalFrame(), physicalOwner);
    Check(advanced && badgeVisible == expectedEnabled,
          runName,
          expectedEnabled
              ? "Coaching renders its persistent badge on the owning physical end"
              : "disabled Coaching removes its persistent badge");
    std::printf(
        "[SPECIAL_MOVE_COACHING_CONTROL] %s gesture=%u outcome=%u "
        "action=%u status=%u physical_owner=%u enabled=%u "
        "home_mask=%u badge=%u\n",
        runName,
        gestureAccepted ? 1U : 0U,
        hasOutcome ? 1U : 0U,
        static_cast<unsigned>(outcome.action),
        static_cast<unsigned>(outcome.status),
        static_cast<unsigned>(physicalOwner),
        expectedEnabled ? 1U : 0U,
        homeProjected ? static_cast<unsigned>(resolvedHomeMask) : UINT8_MAX,
        badgeVisible ? 1U : 0U);
    return gestureAccepted && hasOutcome && homeProjected && advanced &&
           badgeVisible == expectedEnabled &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.action == modules::ControlAction::Coaching;
}

void RunCoachingHomeProjectionJourney(const bool firstPlayerIsP2,
                                      const bool boardRotate) {
    char runName[80] = {};
    (void)std::snprintf(runName,
                        sizeof(runName),
                        "coaching-home/first-p%u/rotate-%u",
                        firstPlayerIsP2 ? 2U : 1U,
                        boardRotate ? 1U : 0U);
    test_support::BoundedHintSession sessionFixture(
        MakeConfig(firstPlayerIsP2, boardRotate));
    auto& session = sessionFixture.session;
    const bool squareRotate =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized,
          runName,
          "Fresh Setup reaches Play Ready before physical Coaching controls");
    if (!initialized) {
        return;
    }
    Check(std::strstr(session.HomeStateLine().CStr(), "\"as\":\"2o\"") !=
              nullptr,
          runName,
          "Play Ready projects an available manual Hint with its request off");

    const uint8_t physicalP1Bit = CoachingBitForPhysicalOwner(
        session, modules::UiOwner::P1);
    const uint8_t physicalP2Bit = CoachingBitForPhysicalOwner(
        session, modules::UiOwner::P2);
    const bool p1Enabled = ToggleCoaching(
        session, modules::UiOwner::P1, true, runName, physicalP1Bit);
    const bool bothEnabled = ToggleCoaching(
        session, modules::UiOwner::P2, true, runName, 3U);
    const bool p1Disabled = ToggleCoaching(
        session, modules::UiOwner::P1, false, runName, physicalP2Bit);
    const bool bothDisabled = ToggleCoaching(
        session, modules::UiOwner::P2, false, runName, 0U);
    Check(p1Enabled && bothEnabled && p1Disabled && bothDisabled,
          runName,
          "both physical O holds round-trip independent logical Coaching latches");
    std::printf(
        "[SPECIAL_MOVE_COACHING_HOME] %s square_rotate=%u "
        "physical_p1_bit=%u physical_p2_bit=%u result=%u\n",
        runName,
        squareRotate ? 1U : 0U,
        static_cast<unsigned>(physicalP1Bit),
        static_cast<unsigned>(physicalP2Bit),
        p1Enabled && bothEnabled && p1Disabled && bothDisabled ? 1U : 0U);
}

bool EnableCoaching(runtime::VirtualLitchSession& session,
                    const modules::UiOwner physicalOwner,
                    const char* runName) {
    return ToggleCoaching(
        session, physicalOwner, true, runName);
}

bool AssertVisibleHint(runtime::VirtualLitchSession& session,
                       const bool squareRotate,
                       const char* runName,
                       const char* assistanceName) {
    uint8_t hintFrom = 255U;
    uint8_t hintTo = 255U;
    bool hintReady = session.TryGetHintMoveSquares(hintFrom, hintTo);
    bool advanced = true;
    uint32_t initialWaitMs = 0U;
    // The first result has a 500 ms reveal window. Once it appears, the
    // existing continuity and physical animation checks remain strict.
    for (uint8_t sample = 0U; sample < 12U && !hintReady; ++sample) {
        advanced = session.Advance(90U) == VIRTUAL_LITCH_OK && advanced;
        initialWaitMs += 90U;
        hintReady = session.TryGetHintMoveSquares(hintFrom, hintTo);
    }
    bool hintNeverMissing = hintReady;
    bool sourceLit = false;
    bool sourceAnimated = false;
    bool destinationGuided = false;
    bool previousSourceValid = false;
    virtual_litch_color previousSource{};
    uint8_t stableSamples = 0U;
    for (uint8_t sample = 0U; sample < 12U; ++sample) {
        advanced = advanced &&
                   session.Advance(90U) == VIRTUAL_LITCH_OK;
        uint8_t currentFrom = 255U;
        uint8_t currentTo = 255U;
        const bool currentReady = session.TryGetHintMoveSquares(
            currentFrom, currentTo);
        if (!currentReady) {
            hintNeverMissing = false;
            hintReady = false;
            continue;
        }
        if (!hintReady || currentFrom != hintFrom || currentTo != hintTo) {
            hintFrom = currentFrom;
            hintTo = currentTo;
            sourceLit = false;
            sourceAnimated = false;
            destinationGuided = false;
            previousSourceValid = false;
            stableSamples = 0U;
        }
        hintReady = true;
        const uint8_t hintFromPhysical =
            runtime::MapSquareIndex(hintFrom, squareRotate);
        const uint8_t hintToPhysical =
            runtime::MapSquareIndex(hintTo, squareRotate);
        const virtual_litch_color currentSource =
            PhysicalSquareColor(
                session.PhysicalFrame(),
                hintFromPhysical,
                hardware::SquareLedSlot::Piece);
        sourceLit = sourceLit || ColorEnergy(currentSource) != 0U;
        if (previousSourceValid &&
            !ColorsEqual(previousSource, currentSource)) {
            sourceAnimated = true;
        }
        previousSource = currentSource;
        previousSourceValid = true;
        destinationGuided = HasSquareGuidance(
            session.PhysicalFrame(), hintToPhysical);
        ++stableSamples;
        if (stableSamples >= 5U && sourceLit && sourceAnimated &&
            destinationGuided) {
            break;
        }
    }
    const bool visible = advanced && session.HintDisplayRequested() &&
                         hintNeverMissing && hintReady && sourceLit &&
                         sourceAnimated && destinationGuided;
    Check(visible,
          runName,
          "assistance physically animates the Hint source and guides its mapped destination before the king lift");
    std::printf(
        "[SPECIAL_MOVE_ASSISTANCE_ACTIVE] %s mode=%s requested=%u "
        "initial_wait_ms=%lu ready=%u continuous=%u from=%u to=%u source_animated=%u "
        "destination_guided=%u\n",
        runName,
        assistanceName,
        session.HintDisplayRequested() ? 1U : 0U,
        static_cast<unsigned long>(initialWaitMs),
        hintReady ? 1U : 0U,
        hintNeverMissing ? 1U : 0U,
        static_cast<unsigned>(hintFrom),
        static_cast<unsigned>(hintTo),
        sourceAnimated ? 1U : 0U,
        destinationGuided ? 1U : 0U);
    return visible;
}

bool ApplyMove(runtime::VirtualLitchSession& session,
               uint64_t& logicalOccupancy,
               const Move move,
               const bool squareRotate,
               const uint32_t postMoveAdvanceMs) {
    logicalOccupancy &= ~(UINT64_C(1) << move.from);
    if (session.ObserveHallMask(
            MapMask(logicalOccupancy, squareRotate)) != VIRTUAL_LITCH_OK) {
        return false;
    }
    logicalOccupancy |= UINT64_C(1) << move.to;
    return session.ObserveHallMask(
               MapMask(logicalOccupancy, squareRotate)) == VIRTUAL_LITCH_OK &&
           session.Advance(postMoveAdvanceMs) == VIRTUAL_LITCH_OK;
}

bool MutateSetting(runtime::VirtualLitchSession& session,
                   const uint16_t requestId,
                   const char* key,
                   const char* valueJson) {
    virtual_litch_setting_request request{};
    virtual_litch_default_setting_request(&request);
    request.request_id = requestId;
    (void)std::snprintf(request.key, sizeof(request.key), "%s", key);
    (void)std::snprintf(
        request.value_json, sizeof(request.value_json), "%s", valueJson);
    modules::ControlMutationOutcome outcome;
    return session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

bool WaitForWorkerInactive(const modules::AsyncThinkWorker& worker) {
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        if (!worker.IsActive()) {
            return true;
        }
        std::this_thread::yield();
    }
    return !worker.IsActive();
}

bool HintEquals(const runtime::VirtualLitchSession& session,
                const uint8_t expectedFrom,
                const uint8_t expectedTo) {
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    return session.TryGetHintMoveSquares(fromSquare, toSquare) &&
           fromSquare == expectedFrom && toSquare == expectedTo;
}

bool HintUnavailable(const runtime::VirtualLitchSession& session) {
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    return !session.TryGetHintMoveSquares(fromSquare, toSquare);
}

bool AssertExpectedHintPhysicalCheckpoint(
    runtime::VirtualLitchSession& session,
    const bool squareRotate,
    const uint8_t expectedFrom,
    const uint8_t expectedTo,
    const char* runName,
    const char* stage) {
    const uint8_t physicalFrom =
        runtime::MapSquareIndex(expectedFrom, squareRotate);
    const uint8_t physicalTo =
        runtime::MapSquareIndex(expectedTo, squareRotate);
    bool continuous = session.HintDisplayRequested() &&
                      HintEquals(session, expectedFrom, expectedTo) &&
                      HasSquareGuidance(
                          session.PhysicalFrame(), physicalTo);
    bool sourceLit = false;
    bool sourceAnimated = false;
    bool previousValid = false;
    uint8_t samplesObserved = 0U;
    virtual_litch_color previous{};
    for (uint8_t sample = 0U; sample < 12U; ++sample) {
        continuous = continuous &&
                     session.Advance(90U) == VIRTUAL_LITCH_OK &&
                     session.HintDisplayRequested() &&
                     HintEquals(session, expectedFrom, expectedTo) &&
                     HasSquareGuidance(
                         session.PhysicalFrame(), physicalTo);
        const virtual_litch_color current = PhysicalSquareColor(
            session.PhysicalFrame(),
            physicalFrom,
            hardware::SquareLedSlot::Piece);
        sourceLit = sourceLit || ColorEnergy(current) != 0U;
        if (previousValid && !ColorsEqual(previous, current)) {
            sourceAnimated = true;
        }
        previous = current;
        previousValid = true;
        ++samplesObserved;
        if (samplesObserved >= 5U && continuous && sourceLit &&
            sourceAnimated) {
            break;
        }
    }
    const bool visible = continuous && sourceLit && sourceAnimated;
    Check(visible,
          runName,
          "expected Hint remains continuous with mapped physical source animation and destination guidance");
    std::printf(
        "[RAW_COACHING_PHYSICAL] %s stage=%s logical=%u-%u "
        "physical=%u-%u continuous=%u source_animated=%u "
        "destination_guided=%u\n",
        runName,
        stage,
        static_cast<unsigned>(expectedFrom),
        static_cast<unsigned>(expectedTo),
        static_cast<unsigned>(physicalFrom),
        static_cast<unsigned>(physicalTo),
        continuous ? 1U : 0U,
        sourceAnimated ? 1U : 0U,
        HasSquareGuidance(session.PhysicalFrame(), physicalTo) ? 1U : 0U);
    return visible;
}

bool InitializeRawCoachFixture(runtime::VirtualLitchSession& session,
                               ControlledCoachHintEngine& engine,
                               const modules::AsyncThinkWorker& worker,
                               const char* runName,
                               const bool latchClassic = false) {
    bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        (!latchClassic || session.LoadExternalFen(INITIAL_FEN));
    engine.SetScriptedMoves("a2a3", "a2a3");
    engine.SetHoldInitialTail(true);
    initialized = initialized &&
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        engine.WaitForTailEntry(1U);
    // A naturally finished continuous search is cached once. Pure latch/cache
    // reuse is separate from the live-publication journey below.
    initialized = initialized && worker.IsActive() &&
        engine.LastConfig().continuousAnalysis &&
        engine.LastConfig().maxNodes == 0U && engine.LastConfig().maxDepth == 0U;
    engine.ReleaseTail();
    const bool ordinaryReady = initialized && engine.WaitForCompletion(1U) &&
        WaitForWorkerInactive(worker) && session.Advance(300U) == VIRTUAL_LITCH_OK &&
        engine.StartCount() == 1U && HintEquals(session, 48U, 40U);
    Check(ordinaryReady, runName,
          "ordinary continuous Hint completes once and caches a2-a3 before Coaching");
    return ordinaryReady;
}

void TestRawCoachingReusesCurrentHint() {
    constexpr const char* RUN = "raw-coaching-reuses-current-cache";
    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(MakeConfig(false, true), engine, worker);
    if (!InitializeRawCoachFixture(session, engine, worker, RUN)) {
        return;
    }
    const uint32_t starts = engine.StartCount();
    const uint32_t reads = engine.ResultReadCount();
    const auto owner = PhysicalOwnerForSide(modules::SideToMove::White, false, true);
    Check(EnableCoaching(session, owner, RUN) &&
              engine.StartCount() == starts && engine.ResultReadCount() == reads &&
              HintEquals(session, 48U, 40U), RUN,
          "Coaching reveals the completed current Hint without restarting its finished ladder");
    (void)AssertExpectedHintPhysicalCheckpoint(session, true, 48U, 40U, RUN, "reused-cache");
    Check(ToggleCoaching(session, owner, false, RUN) &&
              ToggleCoaching(session, owner, true, RUN) && engine.StartCount() == starts &&
              HintEquals(session, 48U, 40U) && engine.UnsafeResultReadCount() == 0U,
          RUN, "disable and re-entry preserve the same cache without touching an active engine");
}

void TestRawCoachingReusesInFlightOrdinarySearch() {
    constexpr const char* RUN = "raw-coaching-inflight-reuse";
    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(MakeConfig(false, false), engine, worker);
    engine.SetHoldInitialTail(true);
    const bool started = session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK && engine.WaitForTailEntry();
    Check(started && worker.IsActive() && HintUnavailable(session) &&
              engine.LastConfig().continuousAnalysis &&
              engine.LastConfig().maxThinkMs == 90000U &&
              engine.LastConfig().maxNodes == 0U && engine.LastConfig().maxDepth == 0U,
          RUN, "one continuous search starts quietly without node/depth strength ceilings");
    const bool enabled = EnableCoaching(session, modules::UiOwner::P1, RUN);
    Check(enabled && engine.StartCount() == 1U && worker.IsActive(), RUN,
          "Coaching reuses the existing worker");
    engine.PublishLiveMove("a2a3");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && HintEquals(session, 48U, 40U) &&
              worker.IsActive() && engine.ResultReadCount() == 0U,
          RUN, "live immutable candidate appears without reading the owned engine");
    const bool physical = AssertExpectedHintPhysicalCheckpoint(
        session, false, 48U, 40U, RUN, "live-initial");
    engine.PublishLiveMove("e2e4");
    const bool refined = session.Advance(1U) == VIRTUAL_LITCH_OK &&
        HintEquals(session, 52U, 36U) && worker.IsActive() && engine.StartCount() == 1U &&
        engine.ResultReadCount() == 0U && engine.UnsafeResultReadCount() == 0U;
    Check(refined, RUN, "later completed candidate refines the same active search at Idle");
    engine.ReleaseTail();
    Check(engine.WaitForCompletion(1U) && WaitForWorkerInactive(worker) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && HintEquals(session, 52U, 36U),
          RUN, "terminal result preserves the most recent actual move");
    std::printf("[RAW_COACHING_INFLIGHT_REUSE] started=%u physical=%u refined=%u starts=%lu\n",
                started, physical, refined, static_cast<unsigned long>(engine.StartCount()));
}

enum class EmptyCoachFailure : uint8_t {
    Start,
    Error,
    Canceled,
};

void TestRawCoachingEmptyCacheFailureRetries(
    const EmptyCoachFailure failure,
    const char* runName) {
    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(
        MakeConfig(false, false), engine, worker);

    const bool reset = session.Reset() == VIRTUAL_LITCH_OK;
    engine.SetHoldInitialTail(true);
    const bool ordinaryTailEntered =
        reset &&
        session.ObserveHallMask(INITIAL_OCCUPANCY) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        engine.WaitForTailEntry(1U) && worker.IsActive() &&
        HintUnavailable(session);
    Check(ordinaryTailEntered,
          runName,
          "ordinary search is held with no cached Hint");
    if (failure == EmptyCoachFailure::Start) {
        engine.SetFailAutomaticStarts(1U);
    } else {
        engine.SetAutomaticFailures(
            failure == EmptyCoachFailure::Error
                ? modules::ThinkState::Error
                : modules::ThinkState::Canceled,
            1U);
    }

    const bool enabled = EnableCoaching(
        session, modules::UiOwner::P1, runName);
    // A visibility setting cannot invalidate search. Explicit position reload
    // requests cancellation, then retries only after the worker relinquishes it.
    const bool reloadAccepted = enabled && session.LoadExternalFen(INITIAL_FEN);
    const bool invalidated = enabled && engine.WaitForCompletion(1U) &&
        WaitForWorkerInactive(worker) && (reloadAccepted || session.LoadExternalFen(INITIAL_FEN)) &&
        session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(invalidated && engine.StartCount() == 2U && HintUnavailable(session),
          runName, "explicit position reload invalidates uncached work before injected retry failure");
    uint32_t retryTailCount = 2U;
    if (failure != EmptyCoachFailure::Start) {
        const bool failedTailEntered = engine.WaitForTailEntry(2U);
        Check(failedTailEntered && worker.IsActive() &&
                  HintUnavailable(session),
              runName,
              "cache-empty bounded search reaches its controlled failure tail");
        if (failedTailEntered) {
            engine.ReleaseTail();
        }
        const bool failedResultHandled =
            failedTailEntered && engine.WaitForCompletion(2U) &&
            WaitForWorkerInactive(worker) &&
            session.Advance(1U) == VIRTUAL_LITCH_OK;
        Check(failedResultHandled,
              runName,
              "cache-empty bounded result reaches coordinator failure handling");
        retryTailCount = 3U;
    }

    // The start failure happens inside the reload's 200 ms settle advance.
    // Measure from that actual attempt; result failures are handled at the
    // current session time after the worker tail is explicitly released.
    const uint32_t failedAtMs = failure == EmptyCoachFailure::Start
        ? engine.LastStartAtMs() : session.State().now_ms;
    const uint32_t sinceFailureMs = session.State().now_ms - failedAtMs;
    const uint32_t beforeRetryMs = sinceFailureMs < 250U ? 250U - sinceFailureMs : 0U;
    const bool retryNotEarly = beforeRetryMs != 0U &&
        session.Advance(beforeRetryMs - 1U) == VIRTUAL_LITCH_OK &&
        engine.StartCount() == 2U && HintUnavailable(session);
    Check(retryNotEarly, runName,
          "the empty-cache failure preserves no Hint and waits the full retry backoff");
    const bool retryTailEntered =
        invalidated && retryNotEarly &&
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
        engine.WaitForTailEntry(retryTailCount) &&
        worker.IsActive() && engine.StartCount() == 3U &&
        engine.LastConfig().continuousAnalysis;
    const bool noFallback = retryTailEntered && HintUnavailable(session);
    Check(noFallback, runName,
          "uncached automatic failure retries after backoff without inventing an analyzed move");
    if (retryTailEntered) {
        engine.ReleaseTail();
    }
    const bool recovered =
        retryTailEntered && engine.WaitForCompletion(3U) &&
        WaitForWorkerInactive(worker) &&
        session.Advance(500U) == VIRTUAL_LITCH_OK &&
        HintEquals(session, 52U, 36U);
    Check(recovered,
          runName,
          "cache-empty Coaching retry publishes its successful bounded move");
    const bool recoveredPhysical = AssertExpectedHintPhysicalCheckpoint(
        session, false, 52U, 36U, runName, "empty-cache-recovered");
    engine.ReleaseTail();
    Check(WaitForWorkerInactive(worker), runName,
          "the controlled refinement relinquishes the worker before fixture exit");
    std::printf(
        "[RAW_COACHING_EMPTY_CACHE_FAILURE] %s kind=%u ordinary_tail=%u "
        "setup=explicit-position-reload invalidated=%u backoff_not_early=%u "
        "no_arbitrary_fallback=%u retry_tail=%u "
        "final=52-36 recovered=%u recovered_physical=%u starts=%lu\n",
        runName,
        static_cast<unsigned>(failure),
        ordinaryTailEntered ? 1U : 0U,
        invalidated ? 1U : 0U,
        retryNotEarly ? 1U : 0U,
        noFallback ? 1U : 0U,
        retryTailEntered ? 1U : 0U,
        recovered ? 1U : 0U,
        recoveredPhysical ? 1U : 0U,
        static_cast<unsigned long>(engine.StartCount()));
}

void TestRawCoachingCancelAndReenter() {
    constexpr const char* RUN = "raw-coaching-toggle-reuses-live-search";
    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(MakeConfig(false, false), engine, worker);
    Check(session.Reset() == VIRTUAL_LITCH_OK, RUN, "session resets");
    engine.SetHoldInitialTail(true);
    Check(session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK && engine.WaitForTailEntry(),
          RUN, "ordinary continuous search is held");
    Check(EnableCoaching(session, modules::UiOwner::P1, RUN), RUN, "Coaching enabled");
    engine.PublishLiveMove("a2a3");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && HintEquals(session, 48U, 40U),
          RUN, "live result appears before terminal completion");
    Check(ToggleCoaching(session, modules::UiOwner::P1, false, RUN) &&
              ToggleCoaching(session, modules::UiOwner::P1, true, RUN) &&
              engine.StartCount() == 1U && engine.CancellationTailEntryCount() == 0U &&
              worker.IsActive() && engine.UnsafeResultReadCount() == 0U,
          RUN, "visibility toggles neither cancel nor overlap background search");
    engine.PublishLiveMove("e2e4");
    Check(session.Advance(1U) == VIRTUAL_LITCH_OK && HintEquals(session, 52U, 36U),
          RUN, "re-enabled Coaching consumes the latest actual candidate");
    engine.ReleaseTail();
    Check(engine.WaitForCompletion(1U) && WaitForWorkerInactive(worker), RUN,
          "one worker finishes cleanly");
}

void TestRawCastlePromptRetainsHeldAutomaticSearch() {
    constexpr const char* RUN = "raw-coaching-castle-authority";
    constexpr const char* CASTLE_FEN =
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
    constexpr uint64_t CASTLE_OCCUPANCY =
        (UINT64_C(1) << 4U) | (UINT64_C(1) << 60U) |
        (UINT64_C(1) << 63U);
    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(
        MakeConfig(false, false), engine, worker);

    const bool loaded =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.LoadExternalFen(CASTLE_FEN);
    engine.SetScriptedMoves("e1g1", "e1g1");
    engine.SetHoldInitialTail(true);
    const bool ordinaryReady =
        loaded &&
        session.ObserveHallMask(CASTLE_OCCUPANCY) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        engine.WaitForTailEntry(1U) && worker.IsActive() && HintUnavailable(session);
    Check(ordinaryReady,
          RUN,
          "external castle position holds its uncached ordinary search");
    if (!ordinaryReady) {
        return;
    }
    const bool coachingEnabled = EnableCoaching(
        session, modules::UiOwner::P1, RUN);
    Check(coachingEnabled && worker.IsActive() &&
              engine.StartCount() == 1U && engine.LastConfig().continuousAnalysis,
          RUN, "raw O hold reuses the held bounded initial castle search");
    engine.PublishLiveMove("e1g1");
    const bool initialCompleted = session.Advance(1U) == VIRTUAL_LITCH_OK &&
        HintEquals(session, 60U, 62U);
    const uint32_t readsBeforeAutomatic = engine.ResultReadCount();
    const bool tailEntered = initialCompleted && worker.IsActive() && engine.StartCount() == 1U;
    Check(tailEntered, RUN, "live castle Hint is retained while its continuous search stays active");
    const bool heldHintPhysical = session.HintDisplayRequested() &&
        HintEquals(session, 60U, 62U) &&
        HasSquareGuidance(session.PhysicalFrame(), 62U);
    // Pickup and the king-to-rook handoff maintain the refinement. Its
    // cancellation tail belongs to the full physical move commit below.
    engine.SetHoldCancellationTail(true);

    uint64_t occupancy =
        CASTLE_OCCUPANCY & ~(UINT64_C(1) << 60U);
    const bool kingLifted =
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK;
    const bool searchMaintained = kingLifted && worker.IsActive() &&
        engine.StartCount() == 1U &&
        engine.CancellationTailEntryCount() == 0U &&
        engine.ResultReadCount() == readsBeforeAutomatic;
    Check(heldHintPhysical && searchMaintained, RUN,
          "matching king pickup retains its visible Hint without canceling the refinement");
    occupancy |= UINT64_C(1) << 62U;
    const bool kingPlaced =
        kingLifted &&
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK;
    const virtual_litch_color kingPiece = PhysicalSquareColor(
        session.PhysicalFrame(), 62U, hardware::SquareLedSlot::Piece);
    const virtual_litch_color rookPiece = PhysicalSquareColor(
        session.PhysicalFrame(), 63U, hardware::SquareLedSlot::Piece);
    const bool rookPromptOwnsBoard =
        kingPlaced && !session.HintDisplayRequested() &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::ExecutingPlan) &&
        session.State().required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::LiftSecondary) &&
        ColorEnergy(kingPiece) != 0U &&
        (ColorEnergy(rookPiece) == 0U || ColorsEqual(kingPiece, rookPiece)) &&
        HasSquareGuidance(session.PhysicalFrame(), 63U) &&
        HasSquareGuidance(session.PhysicalFrame(), 61U) &&
        session.State().human_illegal_mask == 0ULL &&
        session.State().correction_mask == 0ULL &&
        engine.ResultReadCount() == readsBeforeAutomatic &&
        engine.UnsafeResultReadCount() == 0U;
    Check(rookPromptOwnsBoard,
          RUN,
          "king placement maintains the held refinement while the rook plan becomes authoritative");
    const virtual_litch_color kingBloomEarly = PhysicalSquareColor(
        session.PhysicalFrame(), 62U, hardware::SquareLedSlot::Square0);
    const bool kingBloomAnimated =
        session.Advance(100U) == VIRTUAL_LITCH_OK &&
        !ColorsEqual(
            kingBloomEarly,
            PhysicalSquareColor(
                session.PhysicalFrame(),
                62U,
                hardware::SquareLedSlot::Square0)) &&
        worker.IsActive() &&
        HasSquareGuidance(session.PhysicalFrame(), 63U) &&
        HasSquareGuidance(session.PhysicalFrame(), 61U);
    Check(kingBloomAnimated,
          RUN,
          "king bloom animates while the in-flight refinement and rook prompt coexist");
    // The occupied rook center intentionally blinks; the earlier cold-cache
    // handoff can land in its dark phase. Observe a full existing cycle.
    bool kingIdentityStable = true;
    bool rookIdentityLit = ColorsEqual(kingPiece, rookPiece);
    bool rookBlinkDark = ColorEnergy(rookPiece) == 0U;
    for (uint8_t sample = 0U; sample < 8U; ++sample) {
        kingIdentityStable = session.Advance(100U) == VIRTUAL_LITCH_OK &&
            ColorsEqual(kingPiece, PhysicalSquareColor(session.PhysicalFrame(),
                62U, hardware::SquareLedSlot::Piece)) && kingIdentityStable;
        const auto rook = PhysicalSquareColor(session.PhysicalFrame(),
            63U, hardware::SquareLedSlot::Piece);
        rookIdentityLit = rookIdentityLit || ColorsEqual(kingPiece, rook);
        rookBlinkDark = rookBlinkDark || ColorEnergy(rook) == 0U;
        Check((ColorEnergy(rook) == 0U || ColorsEqual(kingPiece, rook)) &&
                  HasSquareGuidance(session.PhysicalFrame(), 63U) &&
                  HasSquareGuidance(session.PhysicalFrame(), 61U) &&
                  !session.HintDisplayRequested() && worker.IsActive() &&
                  engine.CancellationTailEntryCount() == 0U,
              RUN, "rook guidance remains authoritative throughout its center blink");
    }
    Check(kingIdentityStable && rookIdentityLit && rookBlinkDark,
          RUN, "king identity stays steady while the occupied rook blinks in its own piece color");
    std::printf(
        "[RAW_COACHING_CASTLE_INFLIGHT] tail=%u held_physical=%u "
        "search_maintained=%u hint_requested=%u phase=%u required=%u "
        "king_identity=%u king_bloom=%u rook_from=%u rook_to=%u "
        "rook_identity_lit=%u rook_blink_dark=%u "
        "reads=%lu unsafe=%lu\n",
        tailEntered ? 1U : 0U,
        heldHintPhysical ? 1U : 0U,
        searchMaintained ? 1U : 0U,
        session.HintDisplayRequested() ? 1U : 0U,
        static_cast<unsigned>(session.State().interaction_phase),
        static_cast<unsigned>(session.State().required_action),
        kingIdentityStable ? 1U : 0U,
        kingBloomAnimated ? 1U : 0U,
        HasSquareGuidance(session.PhysicalFrame(), 63U) ? 1U : 0U,
        HasSquareGuidance(session.PhysicalFrame(), 61U) ? 1U : 0U,
        rookIdentityLit ? 1U : 0U,
        rookBlinkDark ? 1U : 0U,
        static_cast<unsigned long>(engine.ResultReadCount()),
        static_cast<unsigned long>(engine.UnsafeResultReadCount()));

    occupancy &= ~(UINT64_C(1) << 63U);
    const bool rookLifted =
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        session.State().required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::PlaceSecondary) &&
        HasSquareGuidance(session.PhysicalFrame(), 61U);
    occupancy |= UINT64_C(1) << 61U;
    const bool rookPlaced =
        rookLifted && session.ObserveHallMask(occupancy) ==
                          VIRTUAL_LITCH_OK;
    const bool cancellationHeld = rookPlaced &&
        engine.WaitForCancellationTail() && worker.IsActive();
    Check(cancellationHeld &&
              engine.CancellationTailEntryCount() == 1U &&
              session.State().side_to_move_black == 0U &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == CASTLE_OCCUPANCY &&
              session.State().pending_action_token != 0U &&
              engine.ResultReadCount() == readsBeforeAutomatic &&
              engine.UnsafeResultReadCount() == 0U,
          RUN,
          "full rook placement waits for worker quiescence before committing the castle");
    engine.ReleaseCancellationTail();
    const bool canceled = engine.WaitForCompletion(1U) &&
                          WaitForWorkerInactive(worker);
    engine.SetHoldAutomaticTail(false);
    const bool completed = canceled &&
        session.Advance(1U) == VIRTUAL_LITCH_OK &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::Idle) &&
        session.State().physical_occupied_mask == occupancy &&
        session.State().expected_occupied_mask == occupancy &&
        session.State().side_to_move_black == 1U &&
        session.State().pending_action_token == 0U &&
        session.State().human_illegal_mask == 0ULL &&
        session.State().correction_mask == 0ULL;
    Check(completed,
          RUN,
          "rook pickup and placement commit the held-search castle exactly once");
    const virtual_litch_color completedKingBloom = PhysicalSquareColor(
        session.PhysicalFrame(), 62U, hardware::SquareLedSlot::Square0);
    const virtual_litch_color completedRookBloom = PhysicalSquareColor(
        session.PhysicalFrame(), 61U, hardware::SquareLedSlot::Square0);
    const bool bloomsPresent =
        ColorEnergy(completedKingBloom) != 0U &&
        ColorEnergy(completedRookBloom) != 0U;
    Check(bloomsPresent,
          RUN,
          "completed castle retains overlapping king and rook acknowledgement blooms");
    const bool completionBloomAnimated =
        session.Advance(100U) == VIRTUAL_LITCH_OK &&
        (!ColorsEqual(
             completedKingBloom,
             PhysicalSquareColor(
                 session.PhysicalFrame(),
                 62U,
                 hardware::SquareLedSlot::Square0)) ||
         !ColorsEqual(
             completedRookBloom,
             PhysicalSquareColor(
                 session.PhysicalFrame(),
                 61U,
                 hardware::SquareLedSlot::Square0)));
    Check(completionBloomAnimated,
          RUN,
          "completed castle acknowledgement remains visibly animated before expiry");
    // Step at the 2 s confirmation boundary so the production timer starts
    // its bounded 560 ms afterglow there, then advance beyond that bloom.
    const bool settleFinalized =
        session.Advance(1900U) == VIRTUAL_LITCH_OK;
    const bool expired =
        settleFinalized && session.Advance(600U) == VIRTUAL_LITCH_OK;
    const virtual_litch_color expiredKing = PhysicalSquareColor(
        session.PhysicalFrame(), 62U, hardware::SquareLedSlot::Square0);
    const virtual_litch_color expiredRook = PhysicalSquareColor(
        session.PhysicalFrame(), 61U, hardware::SquareLedSlot::Square0);
    const virtual_litch_color expiredRookSource = PhysicalSquareColor(
        session.PhysicalFrame(), 63U, hardware::SquareLedSlot::Square0);
    const bool periodAdvanced =
        session.Advance(2200U) == VIRTUAL_LITCH_OK;
    const virtual_litch_color laterKing = PhysicalSquareColor(
        session.PhysicalFrame(), 62U, hardware::SquareLedSlot::Square0);
    const virtual_litch_color laterRook = PhysicalSquareColor(
        session.PhysicalFrame(), 61U, hardware::SquareLedSlot::Square0);
    const virtual_litch_color laterRookSource = PhysicalSquareColor(
        session.PhysicalFrame(), 63U, hardware::SquareLedSlot::Square0);
    const bool kingCueRepeats = ColorsEqual(
        expiredKing, laterKing);
    const bool rookDestinationRepeats = ColorsEqual(
        expiredRook, laterRook);
    const bool rookSourceRepeats = ColorsEqual(
        expiredRookSource, laterRookSource);
    const bool stableState =
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::Idle) &&
        session.State().required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::None) &&
        session.State().expected_occupied_mask == occupancy &&
        session.State().human_illegal_mask == 0ULL &&
        session.State().correction_mask == 0ULL &&
        !session.HintDisplayRequested();
    // Castling's persistent last-move cue intentionally continues after the
    // short rook-placement bloom. Sampling one full 2200 ms pulse period later
    // separates that healthy repeatable cue from transient plan guidance.
    const bool expiryStable =
        expired && periodAdvanced && kingCueRepeats &&
        rookDestinationRepeats && rookSourceRepeats && stableState;
    Check(expiryStable,
          RUN,
          "short rook bloom expires into stable last-move play without stale rook guidance");
    std::printf(
        "[RAW_COACHING_CASTLE_COMPLETE] canceled=%u rook_lifted=%u "
        "cancel_tail=%u completed=%u blooms=%u bloom_animated=%u expiry_stable=%u "
        "period=%u king_repeat=%u rook_to_repeat=%u rook_from_repeat=%u "
        "stable_state=%u "
        "board=%016llx "
        "next=Black\n",
        canceled ? 1U : 0U,
        rookLifted ? 1U : 0U,
        cancellationHeld ? 1U : 0U,
        completed ? 1U : 0U,
        bloomsPresent ? 1U : 0U,
        completionBloomAnimated ? 1U : 0U,
        expiryStable ? 1U : 0U,
        periodAdvanced ? 1U : 0U,
        kingCueRepeats ? 1U : 0U,
        rookDestinationRepeats ? 1U : 0U,
        rookSourceRepeats ? 1U : 0U,
        stableState ? 1U : 0U,
        static_cast<unsigned long long>(occupancy));
}

void PrintCastleCheckpoint(const runtime::VirtualLitchSession& session,
                           const CastleJourney& journey,
                           const char* runName,
                           const bool firstPlayerIsP2,
                           const bool boardRotate,
                           const bool squareRotate) {
    const auto& state = session.State();
    const auto& frame = session.PhysicalFrame();
    const uint8_t kingPhysical =
        runtime::MapSquareIndex(journey.kingTo, squareRotate);
    const uint8_t rookFromPhysical =
        runtime::MapSquareIndex(journey.rookFrom, squareRotate);
    const uint8_t rookToPhysical =
        runtime::MapSquareIndex(journey.rookTo, squareRotate);
    const auto rookLeft = PhysicalSquareColor(
        frame, rookToPhysical, hardware::SquareLedSlot::Square0);
    const auto rookRight = PhysicalSquareColor(
        frame, rookToPhysical, hardware::SquareLedSlot::Square2);
    std::printf(
        "[CASTLE_VIRTUAL_LITCH_JOURNEY] %s t=%u first_player=%u rotate=%u "
        "phase=%s required=%u physical=%016llx expected=%016llx "
        "correction=%016llx human_illegal=%016llx king_piece=%u "
        "rook_from=%u rook_to=%u rook_from_physical=%u rook_to_physical=%u "
        "rook_from_piece=%u rook_to_rails=%u/%u/%u/%u:%u/%u/%u/%u "
        "action=%u history=%u next=%s home=%s\n",
        runName,
        state.now_ms,
        firstPlayerIsP2 ? 2U : 1U,
        boardRotate ? 1U : 0U,
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                state.interaction_phase)),
        static_cast<unsigned>(state.required_action),
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.correction_mask),
        static_cast<unsigned long long>(state.human_illegal_mask),
        static_cast<unsigned>(PhysicalSquareEnergy(
            frame, kingPhysical, hardware::SquareLedSlot::Piece)),
        static_cast<unsigned>(HasSquareGuidance(frame, rookFromPhysical)),
        static_cast<unsigned>(HasSquareGuidance(frame, rookToPhysical)),
        rookFromPhysical, rookToPhysical,
        static_cast<unsigned>(PhysicalSquareEnergy(
            frame, rookFromPhysical, hardware::SquareLedSlot::Piece)),
        rookLeft.r, rookLeft.g, rookLeft.b, rookLeft.w,
        rookRight.r, rookRight.g, rookRight.b, rookRight.w,
        state.pending_action_token, session.UndoHistoryMoveCount(),
        state.required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::PlaceSecondary)
            ? "place-held-rook-at-destination" : "lift-rook-from-source",
        session.HomeStateLine().CStr());
}

void RunCastleJourney(const CastleJourney& journey,
                      const bool firstPlayerIsP2,
                      const bool boardRotate,
                      const uint32_t finalPostMoveAdvanceMs,
                      const CastleScan scan,
                      const JourneyExit exit = JourneyExit::Complete,
                      const AssistanceMode assistance = AssistanceMode::None) {
    char runName[128] = {};
    const char* timing = finalPostMoveAdvanceMs == 1U
                             ? "prior-settle-interrupted"
                             : "prior-settle-expired";
    const bool rookAlreadyHeld = scan == CastleScan::AtomicKingWithRookHeld;
    const char* scanName = rookAlreadyHeld ? "coalesced-king-rook-held"
        : (scan == CastleScan::AtomicKing ? "coalesced-king" : "split-king");
    const char* exitName = exit == JourneyExit::Complete
                               ? "complete"
                               : (exit == JourneyExit::Cancel
                                      ? "cancel"
                                      : "correct-and-complete");
    const char* assistanceName = assistance == AssistanceMode::None
                                     ? "plain"
                                     : (assistance == AssistanceMode::Hint
                                            ? "hint"
                                            : "coaching");
    std::snprintf(runName,
                  sizeof(runName),
                  "%s/%s/%s/%s/%s",
                  journey.name,
                  timing,
                  scanName,
                  exitName,
                  assistanceName);

    const virtual_litch_config config =
        MakeConfig(firstPlayerIsP2, boardRotate);
    const bool squareRotate =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    test_support::BoundedHintSession sessionFixture(config);
    auto& session = sessionFixture.session;
    const auto advance = [&](uint32_t duration) {
        if (!rookAlreadyHeld) return session.Advance(duration) == VIRTUAL_LITCH_OK;
        while (duration != 0U) {
            const uint32_t step = duration > 10U ? 10U : duration;
            if (session.Advance(step) != VIRTUAL_LITCH_OK) return false;
            duration -= step;
        }
        return true;
    };
    uint64_t occupancy = INITIAL_OCCUPANCY;
    const modules::SideToMove moverSide =
        journey.moverBlack ? modules::SideToMove::Black
                           : modules::SideToMove::White;
    const modules::UiOwner physicalMoverOwner = PhysicalOwnerForSide(
        moverSide, firstPlayerIsP2, squareRotate);

    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              advance(200U),
          runName,
          "Fresh Setup reaches Play Ready");

    const auto setODown = [&](const bool down) {
        return SetPhysicalODown(
            session, physicalMoverOwner, down);
    };
    if (assistance == AssistanceMode::Coaching) {
        (void)EnableCoaching(
            session, physicalMoverOwner, runName);
    }

    for (size_t index = 0U; index < journey.setupMoveCount; ++index) {
        if (rookAlreadyHeld) {
            occupancy &= ~(UINT64_C(1) << journey.setupMoves[index].from);
            Check(advance(250U) &&
                      session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                          VIRTUAL_LITCH_OK && advance(250U),
                  runName, "setup pickup is observed separately before its landing");
            occupancy |= UINT64_C(1) << journey.setupMoves[index].to;
            Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                      VIRTUAL_LITCH_OK && advance(5001U),
                  runName, "setup move and feedback finish with ten-millisecond loop ticks");
        } else {
            Check(ApplyMove(session,
                            occupancy,
                            journey.setupMoves[index],
                            squareRotate,
                            index + 1U == journey.setupMoveCount
                                ? finalPostMoveAdvanceMs
                                : 5001U),
                  runName,
                  "setup move completes through raw Hall observations");
        }
    }

    if (assistance == AssistanceMode::Hint) {
        modules::ControlMutationOutcome outcome;
        Check(setODown(true) && setODown(false) &&
                  session.TryGetLastBoardControlOutcome(outcome) &&
                  outcome.status == modules::ControlMutationStatus::Applied &&
                  outcome.action == modules::ControlAction::Hint,
              runName,
              "physical O tap enables Hint through production controls");
    }
    if (assistance != AssistanceMode::None) {
        (void)AssertVisibleHint(
            session,
            squareRotate,
            runName,
            assistanceName);
    }
    if (assistance == AssistanceMode::Coaching) {
        const bool badgeVisible = HasPhysicalCoachingBadge(
            session.PhysicalFrame(), physicalMoverOwner);
        Check(badgeVisible,
              runName,
              "Coaching's neutral physical badge remains visible immediately before the king lift");
        std::printf(
            "[SPECIAL_MOVE_COACHING_PRE_CASTLE] %s physical_owner=%u "
            "requested=%u badge=%u\n",
            runName,
            static_cast<unsigned>(physicalMoverOwner),
            session.HintDisplayRequested() ? 1U : 0U,
            badgeVisible ? 1U : 0U);
    }

    const uint64_t beforeCastle = occupancy;
    const uint8_t historyBeforeCastle = session.UndoHistoryMoveCount();
    if (rookAlreadyHeld) {
        Check(session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle) &&
                  session.State().expected_occupied_mask == beforeCastle &&
                  session.State().pending_action_token == 0U,
              runName, "the atomic castle begins from an acknowledged healthy Idle board");
        occupancy &= ~(UINT64_C(1) << journey.rookFrom);
    }
    occupancy &= ~(UINT64_C(1) << journey.kingFrom);
    if (scan == CastleScan::SplitKing) {
        Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK,
              runName,
              "king pickup reaches the production reducer");
    }
    occupancy |= UINT64_C(1) << journey.kingTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
              VIRTUAL_LITCH_OK,
          runName,
          "king placement reaches the production reducer");
    if (assistance == AssistanceMode::Coaching) {
        Check(!session.HintDisplayRequested(),
              runName,
              "automatic Coaching Hint yields when the castling sequence becomes authoritative");
    }

    PrintCastleCheckpoint(session,
                          journey,
                          runName,
                          firstPlayerIsP2,
                          boardRotate,
                          squareRotate);

    const auto& state = session.State();
    const auto& frame = session.PhysicalFrame();
    const uint8_t kingPhysical =
        runtime::MapSquareIndex(journey.kingTo, squareRotate);
    const uint8_t rookFromPhysical =
        runtime::MapSquareIndex(journey.rookFrom, squareRotate);
    const uint8_t rookToPhysical =
        runtime::MapSquareIndex(journey.rookTo, squareRotate);
    const uint8_t ownerReferencePhysical =
        runtime::MapSquareIndex(journey.recoverySquare, squareRotate);
    Check(state.interaction_phase == static_cast<uint8_t>(
                                         runtime::ChessInteractionPhase::ExecutingPlan),
          runName,
          "king placement retains a castling plan");
    Check(state.required_action == static_cast<uint8_t>(
              rookAlreadyHeld ? runtime::RequiredPhysicalAction::PlaceSecondary
                              : runtime::RequiredPhysicalAction::LiftSecondary),
          runName,
          "king placement prompts the remaining physical rook action");
    if (rookAlreadyHeld) {
        const auto destinationLeft = PhysicalSquareColor(
            frame, rookToPhysical, hardware::SquareLedSlot::Square0);
        const auto destinationRight = PhysicalSquareColor(
            frame, rookToPhysical, hardware::SquareLedSlot::Square2);
        const auto ownerColor = PhysicalSquareColor(
            frame, ownerReferencePhysical, hardware::SquareLedSlot::Piece);
        std::printf("[CASTLE_HELD_ROOK_CUE] %s source_absent=%u "
            "owner=%u/%u/%u/%u left=%u/%u/%u/%u right=%u/%u/%u/%u\n",
            runName, (state.physical_occupied_mask &
                (UINT64_C(1) << journey.rookFrom)) == 0ULL ? 1U : 0U,
            ownerColor.r, ownerColor.g, ownerColor.b, ownerColor.w,
            destinationLeft.r, destinationLeft.g, destinationLeft.b, destinationLeft.w,
            destinationRight.r, destinationRight.g, destinationRight.b, destinationRight.w);
        // Empty directed-route cells may carry the moving head on all four
        // emitters. Source occupancy, not transient route light, proves held.
        Check((state.physical_occupied_mask &
                  (UINT64_C(1) << journey.rookFrom)) == 0ULL,
              runName, "the held rook has an empty physical source");
        // The available-destination palette differs from ordinary piece hue.
        // test_castling_led_ux owns its exact floor/color expectation.
        Check(ColorEnergy(destinationLeft) != 0U &&
                  ColorEnergy(destinationRight) != 0U,
              runName, "both physical held-rook destination rails remain visible");
        Check(state.expected_occupied_mask == beforeCastle &&
                  session.UndoHistoryMoveCount() == historyBeforeCastle &&
                  state.pending_action_token == 0U,
              runName, "the held rook leaves no partial acknowledgement");
    }
    Check(PhysicalSquareEnergy(
              frame, kingPhysical, hardware::SquareLedSlot::Piece) != 0U,
          runName,
          "placed king remains lit");
    Check(HasSquareGuidance(frame, rookFromPhysical),
          runName,
          "rook source is prompted");
    Check(HasSquareGuidance(frame, rookToPhysical),
          runName,
          "rook destination is prompted");
    if (assistance != AssistanceMode::None) {
        const char* suppressed = assistance == AssistanceMode::Hint
                                     ? "\"as\":\"1x\""
                                     : "\"as\":\"0x\"";
        Check(std::strstr(session.HomeStateLine().CStr(), suppressed) !=
                  nullptr,
              runName,
              "castling rook choreography suppresses endpoints without inventing a ready Hint");
    }
    Check(ColorsEqual(
              PhysicalSquareColor(
                  frame, kingPhysical, hardware::SquareLedSlot::Piece),
              PhysicalSquareColor(
                  frame,
                  ownerReferencePhysical,
                  hardware::SquareLedSlot::Piece)),
          runName,
          "placed king keeps the mover color used by an unaffected own piece");
    Check(state.human_illegal_mask == 0ULL &&
              state.correction_mask == 0ULL,
          runName,
          "legal castling progress never enters correction feedback");

    const virtual_litch_color kingSettleEarly =
        PhysicalSquareColor(
            frame, kingPhysical, hardware::SquareLedSlot::Square0);
    Check(state.pending_action_token == 0U &&
              advance(100U),
          runName,
          "king feedback is render-only and advances without an action");
    const virtual_litch_color kingSettleLater =
        PhysicalSquareColor(
            session.PhysicalFrame(),
            kingPhysical,
            hardware::SquareLedSlot::Square0);
    Check(!ColorsEqual(kingSettleEarly, kingSettleLater) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().required_action == static_cast<uint8_t>(
                  rookAlreadyHeld ? runtime::RequiredPhysicalAction::PlaceSecondary
                                  : runtime::RequiredPhysicalAction::LiftSecondary) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookFromPhysical) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookToPhysical),
          runName,
          "king settle animates while the rook handoff remains immediate");
    std::printf(
        "[CASTLE_SETTLE_OVERLAP] %s stage=king-feedback "
        "action_token=%lu rook_prompt=1 animated=%u\n",
        runName,
        static_cast<unsigned long>(session.State().pending_action_token),
        static_cast<unsigned>(
            !ColorsEqual(kingSettleEarly, kingSettleLater)));

    if (exit == JourneyExit::Cancel) {
        occupancy &= ~(UINT64_C(1) << journey.kingTo);
        occupancy |= UINT64_C(1) << journey.kingFrom;
        Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                      VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                                                           runtime::ChessInteractionPhase::Idle) &&
                  session.State().physical_occupied_mask == occupancy &&
                  session.State().expected_occupied_mask == occupancy &&
                  session.State().human_illegal_mask == 0ULL,
              runName,
              "returning the king cancels cleanly to the last sound position");
        const virtual_litch_color canceledKingTarget =
            PhysicalSquareColor(
                session.PhysicalFrame(),
                kingPhysical,
                hardware::SquareLedSlot::Square0);
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  ColorsEqual(
                      canceledKingTarget,
                      PhysicalSquareColor(
                          session.PhysicalFrame(),
                          kingPhysical,
                          hardware::SquareLedSlot::Square0)),
              runName,
              "cancel removes the king feedback immediately without a ghost fade");
        std::printf(
            "[CASTLE_VIRTUAL_LITCH_RECOVERY] %s outcome=canceled "
            "authoritative=Idle board=%016llx king_ghost=0 next=play\n",
            runName,
            static_cast<unsigned long long>(occupancy));
        return;
    }

    if (exit == JourneyExit::CorrectAndComplete) {
        occupancy &= ~(UINT64_C(1) << journey.recoverySquare);
        Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                      VIRTUAL_LITCH_OK &&
                  session.State().human_illegal_mask ==
                      (UINT64_C(1) << journey.recoverySquare),
              runName,
              "unrelated pickup is correction, not Undo or a new move");
        occupancy |= UINT64_C(1) << journey.recoverySquare;
        Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                      VIRTUAL_LITCH_OK &&
                  session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                                                           runtime::ChessInteractionPhase::ExecutingPlan) &&
                  session.State().required_action == static_cast<uint8_t>(
                                                         runtime::RequiredPhysicalAction::LiftSecondary) &&
                  session.State().human_illegal_mask == 0ULL,
              runName,
              "restoring the unrelated piece resumes the rook handoff");
    }

    bool rookPickupAccepted = false;
    if (rookAlreadyHeld) {
        rookPickupAccepted = advance(150U) &&
            session.UndoHistoryMoveCount() == historyBeforeCastle &&
            session.State().pending_action_token == 0U;
    } else {
        occupancy &= ~(UINT64_C(1) << journey.rookFrom);
        rookPickupAccepted =
            session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                VIRTUAL_LITCH_OK && advance(100U);
    }
    if (exit == JourneyExit::CorrectAndComplete) {
        std::printf(
            "[CASTLE_VIRTUAL_LITCH_RECOVERY] %s outcome=corrected "
            "phase=%s required=%u rook_target=%u illegal=%016llx\n",
            runName,
            runtime::ChessInteractionPhaseToString(
                static_cast<runtime::ChessInteractionPhase>(
                    session.State().interaction_phase)),
            static_cast<unsigned>(session.State().required_action),
            static_cast<unsigned>(HasSquareGuidance(
                session.PhysicalFrame(), rookToPhysical)),
            static_cast<unsigned long long>(
                session.State().human_illegal_mask));
    }
    Check(rookPickupAccepted &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlaceSecondary) &&
              HasSquareGuidance(session.PhysicalFrame(), rookToPhysical),
          runName,
          "rook pickup advances to its spatial destination prompt");
    occupancy |= UINT64_C(1) << journey.rookTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().side_to_move_black ==
                  static_cast<uint8_t>(!journey.moverBlack) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              (!rookAlreadyHeld ||
               session.UndoHistoryMoveCount() == historyBeforeCastle + 1U) &&
              session.State().pending_action_token == 0U,
          runName,
          "rook placement commits one castle and hands the turn over cleanly");
    const virtual_litch_color overlappingKingFeedback =
        PhysicalSquareColor(
            session.PhysicalFrame(),
            kingPhysical,
            hardware::SquareLedSlot::Square0);
    const virtual_litch_color rookPieceEarly =
        PhysicalSquareColor(
            session.PhysicalFrame(),
            rookToPhysical,
            hardware::SquareLedSlot::Piece);
    Check(ColorEnergy(overlappingKingFeedback) != 0U &&
              ColorEnergy(rookPieceEarly) != 0U &&
              SameOwnerHue(
                  rookPieceEarly,
                  PhysicalSquareColor(
                      session.PhysicalFrame(),
                      ownerReferencePhysical,
                      hardware::SquareLedSlot::Piece)),
          runName,
          "king afterglow and final rook settle overlap in the mover color");
    Check(advance(25U) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle),
          runName,
          "rook settle begins without changing composite authority");
    const virtual_litch_color rookSettleEarly =
        PhysicalSquareColor(
            session.PhysicalFrame(),
            rookToPhysical,
            hardware::SquareLedSlot::Square0);
    Check(ColorEnergy(rookSettleEarly) != 0U &&
              advance(100U) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle) &&
              session.State().expected_occupied_mask == occupancy &&
              !ColorsEqual(
                  overlappingKingFeedback,
                  PhysicalSquareColor(
                      session.PhysicalFrame(),
                      kingPhysical,
                      hardware::SquareLedSlot::Square0)) &&
              !ColorsEqual(
                  rookSettleEarly,
                  PhysicalSquareColor(
                      session.PhysicalFrame(),
                      rookToPhysical,
                      hardware::SquareLedSlot::Square0)),
          runName,
          "rook feedback continues after the single composite commit");
    std::printf(
        "[CASTLE_SETTLE_OVERLAP] %s stage=rook-feedback "
        "authoritative=Idle king=%u rook=%u next=%s\n",
        runName,
        static_cast<unsigned>(ColorEnergy(overlappingKingFeedback) != 0U),
        static_cast<unsigned>(ColorEnergy(rookPieceEarly) != 0U),
        journey.moverBlack ? "White" : "Black");
    Check(advance(2500U) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().expected_occupied_mask == occupancy &&
              (!rookAlreadyHeld ||
               session.UndoHistoryMoveCount() == historyBeforeCastle + 1U) &&
              session.State().pending_action_token == 0U,
          runName,
          "settle expiry preserves the committed castle");
    std::printf(
        "[CASTLE_VIRTUAL_LITCH_COMPLETE] %s outcome=accepted "
        "authoritative=Idle board=%016llx history=%u action=%u next=%s home=%s\n",
        runName,
        static_cast<unsigned long long>(occupancy),
        session.UndoHistoryMoveCount(), session.State().pending_action_token,
        journey.moverBlack ? "White" : "Black",
        session.HomeStateLine().CStr());
}

constexpr CastleJourney WHITE_KINGSIDE{
    "white-kingside",
    {{{62U, 45U}, {8U, 16U}, {52U, 44U}, {16U, 24U}, {61U, 52U}, {24U, 32U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}}},
    6U,
    60U,
    62U,
    63U,
    61U,
    48U,
    false,
};

constexpr CastleJourney WHITE_QUEENSIDE{
    "white-queenside",
    {{{51U, 35U}, {15U, 23U}, {57U, 42U}, {23U, 31U}, {58U, 37U}, {8U, 16U}, {59U, 51U}, {16U, 24U}, {0U, 0U}, {0U, 0U}}},
    8U,
    60U,
    58U,
    56U,
    59U,
    48U,
    false,
};

constexpr CastleJourney BLACK_KINGSIDE{
    "black-kingside",
    {{{48U, 40U}, {6U, 21U}, {40U, 32U}, {12U, 20U}, {32U, 24U}, {5U, 12U}, {55U, 47U}, {0U, 0U}, {0U, 0U}, {0U, 0U}}},
    7U,
    4U,
    6U,
    7U,
    5U,
    8U,
    true,
};

constexpr CastleJourney BLACK_QUEENSIDE{
    "black-queenside",
    {{{55U, 47U}, {11U, 27U}, {47U, 39U}, {1U, 18U}, {48U, 40U}, {2U, 29U}, {40U, 32U}, {3U, 11U}, {54U, 46U}, {0U, 0U}}},
    9U,
    4U,
    2U,
    0U,
    3U,
    8U,
    true,
};

constexpr std::array<const CastleJourney*, 4> CASTLES{
    &WHITE_KINGSIDE,
    &WHITE_QUEENSIDE,
    &BLACK_KINGSIDE,
    &BLACK_QUEENSIDE,
};

constexpr std::array<CpuCastleJourney, 4> CPU_CASTLES{{
    {
        "cpu-white-kingside",
        "r1bqk2r/ppp1bppp/2np1n2/1B2p3/4P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 6",
        UINT64_C(0x9DE72C10122CF79D),
        60U,
        62U,
        63U,
        61U,
        false,
    },
    {
        "cpu-white-queenside",
        "rnb1k2r/ppp1qpbp/5np1/8/8/2N1BNP1/PPP1Q1BP/R3K2R w Qkq - 0 1",
        UINT64_C(0x91D774000060F797),
        60U,
        58U,
        56U,
        59U,
        false,
    },
    {
        "cpu-black-kingside",
        "r1bqk2r/ppp2ppp/2np1n2/4p3/1b2P3/2NP1N2/PPP1BPPP/R1BQK2R b KQkq - 0 6",
        UINT64_C(0x9DF72C12102CE79D),
        4U,
        6U,
        7U,
        5U,
        true,
    },
    {
        "cpu-black-queenside",
        "r3k2r/ppp1q1bp/2n1bnp1/8/8/5NP1/PPP1QPBP/RNB1K2R b KQq - 0 1",
        UINT64_C(0x97F760000074D791),
        4U,
        2U,
        0U,
        3U,
        true,
    },
}};

void CheckCpuCastleWaitingRook(runtime::VirtualLitchSession& session,
                               const CpuCastleJourney& journey,
                               const bool squareRotate,
                               const uint64_t occupancy,
                               const runtime::RequiredPhysicalAction action,
                               const char* runName,
                               const char* stage) {
    const uint8_t rookPhysical =
        runtime::MapSquareIndex(journey.rookFrom, squareRotate);
    // An untouched own pawn on the same checker color is the ordinary-render
    // control. Both corner pawns remain present in these actual CPU fixtures.
    const uint8_t referenceRank = journey.moverBlack ? 1U : 6U;
    const uint8_t rookChecker = static_cast<uint8_t>(
        (journey.rookFrom / 8U + journey.rookFrom % 8U) & 1U);
    const uint8_t referenceLogical = static_cast<uint8_t>(
        referenceRank * 8U + (rookChecker ^ (referenceRank & 1U)));
    const uint8_t referencePhysical =
        runtime::MapSquareIndex(referenceLogical, squareRotate);
    const uint64_t expectedOccupancy = session.State().expected_occupied_mask;
    const uint8_t historyCount = session.UndoHistoryMoveCount();
    const uint32_t startedMs = session.State().now_ms;
    bool ordinaryPiece = true;
    bool ordinarySquare = true;
    bool unchangedPlan = true;
    bool framesAdvanced = true;
    uint8_t samples = 0U;
    // 1200 ms spans both the 800 ms mover blink and 600 ms removal cadence.
    // The old renderer wrongly blinked this noncurrent relocation and painted
    // its base red while the king was still the only prompted piece.
    for (uint8_t sample = 0U; sample <= 12U; ++sample) {
        if (sample != 0U && session.Advance(100U) != VIRTUAL_LITCH_OK) {
            framesAdvanced = false;
            break;
        }
        const auto& frame = session.PhysicalFrame();
        const auto rookPiece = PhysicalSquareColor(
            frame, rookPhysical, hardware::SquareLedSlot::Piece);
        ordinaryPiece = ordinaryPiece && ColorEnergy(rookPiece) != 0U &&
            ColorsEqual(rookPiece, PhysicalSquareColor(
                frame, referencePhysical, hardware::SquareLedSlot::Piece));
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Square2}) {
            ordinarySquare = ordinarySquare && ColorsEqual(
                PhysicalSquareColor(frame, rookPhysical, slot),
                PhysicalSquareColor(frame, referencePhysical, slot));
        }
        unchangedPlan = unchangedPlan &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::ExecutingPlan) &&
            session.State().required_action == static_cast<uint8_t>(action) &&
            session.State().physical_occupied_mask == occupancy &&
            session.State().expected_occupied_mask == expectedOccupancy &&
            session.State().cpu_illegal_mask == 0ULL &&
            session.State().correction_mask == 0ULL &&
            session.UndoHistoryMoveCount() == historyCount;
        ++samples;
    }
    Check(framesAdvanced && unchangedPlan && ordinaryPiece && ordinarySquare,
          runName,
          "waiting CPU castle rook keeps ordinary identity and base through both flash cycles before its relocation is prompted");
    const auto piece = PhysicalSquareColor(
        session.PhysicalFrame(), rookPhysical, hardware::SquareLedSlot::Piece);
    const auto square = PhysicalSquareColor(
        session.PhysicalFrame(), rookPhysical, hardware::SquareLedSlot::Square0);
    std::printf(
        "[CPU_CASTLE_WAITING_ROOK] %s stage=%s next=%s occupancy=%016llx "
        "logical=%u physical=%u samples=%u elapsed=%ums "
        "piece=%u,%u,%u,%u square=%u,%u,%u,%u "
        "ordinary_piece=%u ordinary_base=%u unchanged_plan=%u\n",
        runName, stage, runtime::RequiredPhysicalActionToString(action),
        static_cast<unsigned long long>(session.State().physical_occupied_mask),
        journey.rookFrom, rookPhysical, samples,
        static_cast<unsigned>(session.State().now_ms - startedMs),
        piece.r, piece.g, piece.b, piece.w,
        square.r, square.g, square.b, square.w,
        ordinaryPiece ? 1U : 0U, ordinarySquare ? 1U : 0U,
        unchangedPlan ? 1U : 0U);
}

void RunCpuCastleJourney(const CpuCastleJourney& journey,
                         const bool firstPlayerIsP2,
                         const bool boardRotate,
                         const bool coachingEnabled = false) {
    char runName[128] = {};
    (void)std::snprintf(runName,
                        sizeof(runName),
                        "%s/first-player-%u/rotate-%u/%s",
                        journey.name,
                        firstPlayerIsP2 ? 2U : 1U,
                        boardRotate ? 1U : 0U,
                        coachingEnabled ? "coaching-suppressed" : "plain");
    test_support::BoundedHintSession sessionFixture(
        MakeConfig(firstPlayerIsP2, boardRotate));
    auto& session = sessionFixture.session;
    const bool squareRotate =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    const modules::UiOwner physicalMoverOwner = PhysicalOwnerForSide(
        journey.moverBlack ? modules::SideToMove::Black
                           : modules::SideToMove::White,
        firstPlayerIsP2,
        squareRotate);
    if (initialized && coachingEnabled) {
        initialized = EnableCoaching(
            session, physicalMoverOwner, runName);
    }
    initialized = initialized &&
        // This fixture exercises the searched castle, without intentional
        // novice replacement. Novice policy and level-1 physical execution
        // remain covered by the CPU policy, promotion and placement journeys.
        MutateSetting(session, 801U, "VS_CPU_LEVEL", "3") &&
        MutateSetting(session,
                      802U,
                      "VS_CPU_PLAYER",
                      journey.moverBlack ? "2" : "1") &&
        session.LoadExternalFen(journey.fen) &&
        session.ObserveHallMask(MapMask(journey.before, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized,
          runName,
          "production settings and external-position import reach the CPU position");
    if (!initialized) {
        return;
    }
    if (coachingEnabled) {
        const bool badgeVisible = HasPhysicalCoachingBadge(
            session.PhysicalFrame(), physicalMoverOwner);
        const uint8_t savedBit = journey.moverBlack ? 2U : 1U;
        Check(!session.HintDisplayRequested() && !badgeVisible &&
                  (session.HomeStateSnapshot().coachingMask & savedBit) != 0U,
              runName,
              "CPU inhibition suppresses optional Coaching output while retaining the saved player preference");
        std::printf(
            "[SPECIAL_MOVE_COACHING_CPU_SUPPRESSED] %s requested=%u "
            "badge=%u next=cpu-castle\n",
            runName,
            session.HintDisplayRequested() ? 1U : 0U,
            badgeVisible ? 1U : 0U);
    }

    bool planReady = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample) {
        if (session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::ExecutingPlan) &&
            session.State().required_action == static_cast<uint8_t>(
                                                   runtime::RequiredPhysicalAction::LiftPrimary)) {
            planReady = true;
            break;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) {
            break;
        }
    }
    const uint8_t kingFromPhysical =
        runtime::MapSquareIndex(journey.kingFrom, squareRotate);
    const uint8_t kingToPhysical =
        runtime::MapSquareIndex(journey.kingTo, squareRotate);
    char expectedPlan[32]{};
    (void)std::snprintf(expectedPlan, sizeof(expectedPlan),
                       "\"c\":[\"%c%c\",\"%c%c\"",
                       'a' + journey.kingFrom % 8U, '8' - journey.kingFrom / 8U,
                       'a' + journey.kingTo % 8U, '8' - journey.kingTo / 8U);
    const auto selectedHome = session.HomeStateLine();
    const bool castleQueued = planReady &&
        std::strstr(selectedHome.CStr(), expectedPlan) != nullptr;
    Check(castleQueued &&
              HasSquareGuidance(
                  session.PhysicalFrame(), kingFromPhysical) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), kingToPhysical),
          runName,
          "real engine chooses castling and prompts the king relocation");
    std::printf("[CPU_CASTLE_SELECTED] %s accepted=%u level=3 next=lift-king:%u home=%s\n",
                runName, castleQueued ? 1U : 0U, journey.kingFrom, selectedHome.CStr());
    if (!castleQueued) {
        return;
    }

    CheckCpuCastleWaitingRook(
        session, journey, squareRotate, journey.before,
        runtime::RequiredPhysicalAction::LiftPrimary, runName, "king-on-board");

    uint64_t occupancy =
        journey.before & ~(UINT64_C(1) << journey.kingFrom);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlacePrimary),
          runName,
          "CPU king pickup advances to the king destination");
    CheckCpuCastleWaitingRook(
        session, journey, squareRotate, occupancy,
        runtime::RequiredPhysicalAction::PlacePrimary, runName, "king-held");
    occupancy |= UINT64_C(1) << journey.kingTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
              VIRTUAL_LITCH_OK,
          runName,
          "CPU king placement reaches the shared castle reducer");

    const uint8_t rookFromPhysical =
        runtime::MapSquareIndex(journey.rookFrom, squareRotate);
    const uint8_t rookToPhysical =
        runtime::MapSquareIndex(journey.rookTo, squareRotate);
    Check(session.State().interaction_phase == static_cast<uint8_t>(
                                                   runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::LiftSecondary) &&
              PhysicalSquareEnergy(
                  session.PhysicalFrame(),
                  kingToPhysical,
                  hardware::SquareLedSlot::Piece) != 0U &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookFromPhysical) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookToPhysical) &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          runName,
          "placed CPU king stays lit while both rook endpoints are prompted");

    occupancy &= ~(UINT64_C(1) << journey.rookFrom);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlaceSecondary) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookToPhysical),
          runName,
          "CPU rook pickup advances to its destination");
    occupancy |= UINT64_C(1) << journey.rookTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(2500U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.UndoHistoryMoveCount() == 1U &&
              session.State().side_to_move_black ==
                  static_cast<uint8_t>(!journey.moverBlack) &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          runName,
          "CPU rook placement commits the whole castle once");
    std::printf(
        "[CPU_CASTLE_VIRTUAL_LITCH_COMPLETE] %s outcome=accepted "
        "board=%016llx next=%s\n",
        runName,
        static_cast<unsigned long long>(occupancy),
        journey.moverBlack ? "White" : "Black");
}

void RunHumanVsCpuCastleJourney(const CpuCastleJourney& journey,
                                const bool firstPlayerIsP2,
                                const bool boardRotate,
                                const bool coachingEnabled = false) {
    char runName[144] = {};
    (void)std::snprintf(runName,
                        sizeof(runName),
                        "human-v-cpu/%s/first-player-%u/rotate-%u/%s",
                        journey.name,
                        firstPlayerIsP2 ? 2U : 1U,
                        boardRotate ? 1U : 0U,
                        coachingEnabled ? "coaching" : "plain");
    test_support::BoundedHintSession sessionFixture(
        MakeConfig(firstPlayerIsP2, boardRotate));
    auto& session = sessionFixture.session;
    const bool squareRotate =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    const modules::UiOwner physicalMoverOwner = PhysicalOwnerForSide(
        journey.moverBlack ? modules::SideToMove::Black
                           : modules::SideToMove::White,
        firstPlayerIsP2,
        squareRotate);
    if (initialized && coachingEnabled) {
        initialized = EnableCoaching(
            session, physicalMoverOwner, runName);
    }
    initialized = initialized &&
        MutateSetting(session, 811U, "VS_CPU_LEVEL", "1") &&
        MutateSetting(session,
                      812U,
                      "VS_CPU_PLAYER",
                      journey.moverBlack ? "1" : "2") &&
        session.LoadExternalFen(journey.fen) &&
        session.ObserveHallMask(MapMask(journey.before, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(coachingEnabled ? 5001U : 200U) ==
            VIRTUAL_LITCH_OK;
    Check(initialized,
          runName,
          "human side owns the imported castle position opposite the CPU");
    if (!initialized) {
        return;
    }
    if (coachingEnabled) {
        (void)AssertVisibleHint(
            session, squareRotate, runName, "coaching-v-cpu");
    }

    uint64_t occupancy =
        journey.before & ~(UINT64_C(1) << journey.kingFrom);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
              VIRTUAL_LITCH_OK,
          runName,
          "human king pickup is accepted in versus-CPU mode");
    occupancy |= UINT64_C(1) << journey.kingTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
              VIRTUAL_LITCH_OK,
          runName,
          "human king placement reaches the shared castle reducer");

    const uint8_t kingToPhysical =
        runtime::MapSquareIndex(journey.kingTo, squareRotate);
    const uint8_t rookFromPhysical =
        runtime::MapSquareIndex(journey.rookFrom, squareRotate);
    const uint8_t rookToPhysical =
        runtime::MapSquareIndex(journey.rookTo, squareRotate);
    Check(session.State().interaction_phase == static_cast<uint8_t>(
                                                   runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::LiftSecondary) &&
              PhysicalSquareEnergy(
                  session.PhysicalFrame(),
                  kingToPhysical,
                  hardware::SquareLedSlot::Piece) != 0U &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookFromPhysical) &&
              HasSquareGuidance(
                  session.PhysicalFrame(), rookToPhysical) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          runName,
          "human king stays lit and the rook handoff survives CPU ownership");

    occupancy &= ~(UINT64_C(1) << journey.rookFrom);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlaceSecondary),
          runName,
          "human rook pickup advances to its destination");
    occupancy |= UINT64_C(1) << journey.rookTo;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().side_to_move_black ==
                  static_cast<uint8_t>(!journey.moverBlack) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL,
          runName,
          "human castle commits once before the CPU response begins");
    std::printf(
        "[HUMAN_VS_CPU_CASTLE_VIRTUAL_LITCH_COMPLETE] %s outcome=accepted "
        "board=%016llx next=cpu-%s\n",
        runName,
        static_cast<unsigned long long>(occupancy),
        journey.moverBlack ? "white" : "black");
}

bool CpuPromotionCueMatches(const runtime::VirtualLitchSession& session,
                            const bool squareRotate,
                            const bool redOwner) {
    const uint64_t queen = modules::promotion_piece_cue::BoardMask(
        modules::PromotionPiece::Queen);
    for (uint8_t row = 1U; row <= 6U; ++row) {
        for (uint8_t file = 1U; file <= 6U; ++file) {
            const uint8_t logical = static_cast<uint8_t>(row * 8U + file);
            // Observe final hardware pixels, not an injected render snapshot.
            const uint8_t physical = squareRotate ? 63U - logical : logical;
            const auto color = PhysicalSquareColor(
                session.PhysicalFrame(), physical,
                hardware::SquareLedSlot::Square0);
            if ((queen & (UINT64_C(1) << logical)) == 0ULL) {
                if (ColorEnergy(color) != 0U) return false;
            } else if (redOwner ? color.r <= color.g || color.r <= color.b
                                : color.b <= color.r || color.b <= color.g) {
                return false;
            }
        }
    }
    // CPU chooses the piece; neither physical side advertises Left/Right.
    for (const uint8_t edge : {11U, 12U, 27U, 28U}) {
        if (ColorEnergy(session.PhysicalFrame().pixels[
                PHYSICAL_TOPOLOGY.physicalBoardLedCount + edge]) != 0U) return false;
    }
    return true;
}

void RunCpuPromotionJourney(const bool moverBlack,
                             const bool firstPlayerIsP2,
                             const bool boardRotate) {
    // Existing human-promotion seeds. This journey must establish (rather
    // than assume) that real level-1 search chooses their last-rank pawn move.
    const char* const fen = moverBlack
        ? "7k/8/8/8/8/8/p7/7K b - - 0 1"
        : "7k/P7/8/8/8/8/8/7K w - - 0 1";
    const uint8_t from = moverBlack ? 48U : 8U;
    const uint8_t to = moverBlack ? 56U : 0U;
    const uint64_t before = (UINT64_C(1) << 7U) |
                            (UINT64_C(1) << 63U) |
                            (UINT64_C(1) << from);
    const bool squareRotate = modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    const bool redOwner = moverBlack != firstPlayerIsP2;
    char name[96] = {};
    (void)std::snprintf(name, sizeof(name),
                        "cpu-promotion-%s/first-player-%u/rotate-%u",
                        moverBlack ? "black" : "white",
                        firstPlayerIsP2 ? 2U : 1U, boardRotate ? 1U : 0U);
    // Initialize the normal reference palettes before changing placement.
    // Booting an empty store already at P2 instead exercises legacy-alias
    // migration, which correctly preserves different initial player colors.
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        MutateSetting(session, 941U, "WHITE_SQUARES_ON", "false") &&
        MutateSetting(session, 942U, "GAME_FIRST_PLAYER", firstPlayerIsP2 ? "2" : "1") &&
        MutateSetting(session, 943U, "BOARD_ROTATE", boardRotate ? "true" : "false") &&
        session.FirstPlayerIsP2() == firstPlayerIsP2 &&
        session.BoardRotate() == boardRotate &&
        MutateSetting(session, 944U, "VS_CPU_LEVEL", "1") &&
        MutateSetting(session, 945U, "VS_CPU_PLAYER", moverBlack ? "2" : "1") &&
        session.LoadExternalFen(fen) &&
        session.ObserveHallMask(MapMask(before, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized, name, "settings/FEN/raw opening reach a real CPU turn");
    if (!initialized) return;

    bool planReady = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample) {
        planReady = session.State().interaction_phase == static_cast<uint8_t>(
                        runtime::ChessInteractionPhase::ExecutingPlan) &&
                    session.State().required_action == static_cast<uint8_t>(
                        runtime::RequiredPhysicalAction::LiftPrimary);
        if (planReady || session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    const auto selected = session.HomeStateLine();
    const char* const expectedMove = moverBlack
        ? "\"c\":[\"a2\",\"a1\",\"--\"]"
        : "\"c\":[\"a7\",\"a8\",\"--\"]";
    const bool promotionSelected = planReady &&
        std::strstr(selected.CStr(), expectedMove) != nullptr;
    std::printf("[CPU_PROMOTION_SELECTED] %s accepted=%u input=engine-fen:%s home=%s\n",
                name, promotionSelected ? 1U : 0U, fen, selected.CStr());
    Check(promotionSelected, name,
          "real CPU search selects the exact last-rank pawn relocation");
    if (!promotionSelected) return;
    const uint8_t physicalFrom = squareRotate ? 63U - from : from;
    const uint8_t physicalTo = squareRotate ? 63U - to : to;
    Check(HasSquareGuidance(session.PhysicalFrame(), physicalFrom) &&
              HasSquareGuidance(session.PhysicalFrame(), physicalTo),
          name, "CPU prompt shows both pawn pickup and last-rank destination");
    const bool cueMatches = session.Advance(800U) == VIRTUAL_LITCH_OK &&
              CpuPromotionCueMatches(session, squareRotate, redOwner) &&
              !session.PromotionActive();
    Check(cueMatches,
          name, "after prelude the selected Queen silhouette has mover color and no selection controls");
    std::printf("[CPU_PROMOTION_CUE] %s accepted=%u next=lift-pawn:%u place-Queen:%u "
                "expected_board=Queen-silhouette expected_color=%s expected_controls=none display=%.*s\n",
                name, cueMatches ? 1U : 0U, physicalFrom, physicalTo, redOwner ? "red" : "blue",
                static_cast<int>(session.DisplayText().size()),
                session.DisplayText().data());

    uint64_t occupancy = before & ~(UINT64_C(1) << from);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().required_action == static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::PlacePrimary) &&
              HasSquareGuidance(session.PhysicalFrame(), physicalTo),
          name, "raw pawn pickup keeps the physical promotion destination guided");
    occupancy |= UINT64_C(1) << to;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.State().expected_occupied_mask == occupancy,
          name, "placing the selected replacement acknowledges the CPU move");

    // Occupancy-only sensors cannot verify identity; the engine result below
    // must independently prove the Queen suffix survived search and queueing.
    const char* const promotedFen = moverBlack
        ? "\"fen\":\"7k/8/8/8/8/8/8/q6K w "
        : "\"fen\":\"Q6k/8/8/8/8/8/8/7K b ";
    Check(std::strstr(session.HomeStateLine().CStr(), promotedFen) != nullptr,
          name, "authoritative engine position contains the promoted Queen and opposite turn");
    Check(session.ObserveHallMask(MapMask(
              occupancy & ~(UINT64_C(1) << to), squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(100U) == VIRTUAL_LITCH_OK &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              !session.IsUndoActionPending() &&
              session.State().required_action != static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::CorrectBoard),
          name, "brief replacement reseat stays calm within CPU settle without Undo");
    // Both promoted Queens check the opposite king. Red status alone is not
    // proof of an illegal placement; assert the correction/Undo authorities.
    const bool completedCleanly = session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(2100U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle) &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().side_to_move_black == static_cast<uint8_t>(!moverBlack) &&
              session.State().pending_action_token == 0U &&
              session.State().correction_mask == 0ULL &&
              !session.PromotionActive();
    Check(completedCleanly,
          name, "replacement reseats and settles to one completed promotion, with no modal");
    const auto completed = session.HomeStateLine();
    const char* const expectedLastMove = moverBlack
        ? "\"m\":\"----a2a1b\"" : "\"m\":\"a7a8----w\"";
    const bool stableResult = std::strstr(completed.CStr(), promotedFen) != nullptr &&
              std::strstr(completed.CStr(), expectedLastMove) != nullptr &&
              session.Advance(2500U) == VIRTUAL_LITCH_OK &&
              std::strcmp(completed.CStr(), session.HomeStateLine().CStr()) == 0;
    Check(stableResult,
          name, "settle expiry neither repeats the promotion nor changes its one-move record");
    std::printf("[CPU_PROMOTION_COMPLETE] %s accepted=%u input=place/re-lift100ms/reseat "
                "authoritative=%s required=%u expected_next=%s home=%s\n",
                name, completedCleanly && stableResult ? 1U : 0U,
                runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(
                    session.State().interaction_phase)),
                session.State().required_action,
                moverBlack ? "White" : "Black", completed.CStr());
}

void RunEnPassantCarryoverJourney() {
    constexpr const char* NAME =
        "en-passant/prior-settle-interrupted/coalesced-complete";
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    uint64_t occupancy = INITIAL_OCCUPANCY;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK,
          NAME,
          "Fresh Setup reaches Play Ready");
    constexpr std::array<Move, 4> SETUP{{
        {52U, 36U},
        {8U, 16U},
        {36U, 28U},
        {11U, 27U},
    }};
    for (size_t index = 0U; index < SETUP.size(); ++index) {
        Check(ApplyMove(session,
                        occupancy,
                        SETUP[index],
                        false,
                        index + 1U == SETUP.size() ? 1U : 5001U),
              NAME,
              "en-passant setup move completes through raw Hall input");
    }

    occupancy &= ~(UINT64_C(1) << 28U);
    occupancy &= ~(UINT64_C(1) << 27U);
    occupancy |= UINT64_C(1) << 19U;
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Settling) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              PhysicalSquareEnergy(
                  session.PhysicalFrame(),
                  19U,
                  hardware::SquareLedSlot::Piece) != 0U,
          NAME,
          "coalesced pawn move and capture removal remain positive progress");
    const bool enPassantAdvanced =
        session.Advance(5001U) == VIRTUAL_LITCH_OK;
    std::printf(
        "[SPECIAL_MOVE_EN_PASSANT] phase=%s required=%u physical=%016llx "
        "expected=%016llx side=%u illegal=%016llx\n",
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                session.State().interaction_phase)),
        static_cast<unsigned>(session.State().required_action),
        static_cast<unsigned long long>(
            session.State().physical_occupied_mask),
        static_cast<unsigned long long>(
            session.State().expected_occupied_mask),
        static_cast<unsigned>(session.State().side_to_move_black),
        static_cast<unsigned long long>(
            session.State().human_illegal_mask));
    Check(enPassantAdvanced &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().side_to_move_black != 0U &&
              session.State().human_illegal_mask == 0ULL,
          NAME,
          "en-passant commits once and hands the turn to Black");
    std::printf(
        "[SPECIAL_MOVE_VIRTUAL_LITCH_COMPLETE] %s outcome=accepted "
        "authoritative=Idle board=%016llx next=Black\n",
        NAME,
        static_cast<unsigned long long>(occupancy));
}

void RunInvalidCarryoverCorrectionJourney() {
    constexpr const char* NAME =
        "invalid-followup/prior-settle-interrupted/coalesced-recovery";
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    uint64_t occupancy = INITIAL_OCCUPANCY;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              ApplyMove(session, occupancy, {52U, 36U}, false, 1U),
          NAME,
          "opening move enters its success settle");

    occupancy &= ~(UINT64_C(1) << 51U);
    occupancy |= UINT64_C(1) << 43U;
    const uint64_t correction =
        (UINT64_C(1) << 51U) | (UINT64_C(1) << 43U);
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Correction) &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::CorrectBoard) &&
              session.State().correction_mask == correction &&
              session.State().human_illegal_mask == correction &&
              session.State().pending_action_token == 0U,
          NAME,
          "invalid next-side input becomes correction, never Undo or a move");

    occupancy &= ~(UINT64_C(1) << 43U);
    occupancy |= UINT64_C(1) << 51U;
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().human_illegal_mask == 0ULL,
          NAME,
          "restoring the piece returns to the last acknowledged game");
    std::printf(
        "[SPECIAL_MOVE_VIRTUAL_LITCH_RECOVERY] %s outcome=corrected "
        "authoritative=Idle board=%016llx next=Black\n",
        NAME,
        static_cast<unsigned long long>(occupancy));
}

void RunDuckCompoundHintJourney() {
    constexpr const char* NAME =
        "duck-vs-cpu/compound-hint/fast-chess-to-duck";
    constexpr uint8_t DUCK_FROM = 24U;
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    uint64_t occupancy =
        INITIAL_OCCUPANCY | (UINT64_C(1) << DUCK_FROM);
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        MutateSetting(session, 901U, "VS_CPU_LEVEL", "1") &&
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized,
          NAME,
          "Duck VS-CPU opening reaches stable human play");
    if (!initialized) {
        return;
    }

    uint8_t chessFrom = 255U;
    uint8_t chessTo = 255U;
    uint8_t duckFrom = 255U;
    uint8_t duckTo = 255U;
    bool hintReady = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample) {
        if (session.TryGetHintMoveSquares(chessFrom, chessTo) &&
            session.TryGetHintDuckMoveSquares(duckFrom, duckTo)) {
            hintReady = true;
            break;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) {
            break;
        }
    }
    Check(hintReady && duckFrom == DUCK_FROM && duckTo < 64U &&
              duckTo != DUCK_FROM,
          NAME,
          "background Duck search prepares one compound choice");
    if (!hintReady) {
        return;
    }

    const auto setODown = [&](const bool down) {
        return session.SetButtonDown(
                   VIRTUAL_LITCH_OWNER_P1,
                   runtime::VirtualLitchPhysicalButton::O,
                   down) == VIRTUAL_LITCH_OK &&
               session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    modules::ControlMutationOutcome outcome;
    Check(setODown(true) && setODown(false) &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint &&
              outcome.status == modules::ControlMutationStatus::Applied,
          NAME,
          "physical O reveals the compound hint once");

    const auto& hintedFrame = session.PhysicalFrame();
    Check(HasSquareGuidance(hintedFrame, chessFrom) &&
              HasSquareGuidance(hintedFrame, chessTo) &&
              HasSquareGuidance(hintedFrame, duckFrom) &&
              HasSquareGuidance(hintedFrame, duckTo),
          NAME,
          "one frame shows both chess and Duck endpoint pairs");
    Check(IsYellowDominant(PhysicalSquareColor(
              hintedFrame,
              duckFrom,
              hardware::SquareLedSlot::Square0)) &&
              IsYellowDominant(PhysicalSquareColor(
                  hintedFrame,
                  duckTo,
                  hardware::SquareLedSlot::Square0)),
          NAME,
          "yellow distinguishes the Duck pair from the player-colored chess pair");
    char chessFromText[3] = {};
    char chessToText[3] = {};
    char duckFromText[3] = {};
    char duckToText[3] = {};
    const bool endpointsFormatted =
        litch_max::TryFormatUciSquare(
            chessFrom, chessFromText, sizeof(chessFromText)) &&
        litch_max::TryFormatUciSquare(
            chessTo, chessToText, sizeof(chessToText)) &&
        litch_max::TryFormatUciSquare(
            duckFrom, duckFromText, sizeof(duckFromText)) &&
        litch_max::TryFormatUciSquare(
            duckTo, duckToText, sizeof(duckToText));
    char expectedCompound[32] = {};
    (void)std::snprintf(
        expectedCompound,
        sizeof(expectedCompound),
        "\"as\":\"3b%s%s%s%s\"",
        chessFromText,
        chessToText,
        duckFromText,
        duckToText);
    Check(endpointsFormatted &&
              std::strstr(session.HomeStateLine().CStr(),
                          expectedCompound) != nullptr,
          NAME,
          "Home preserves the selected chess and Duck endpoint pairs as one compound Hint");

    occupancy &= ~(UINT64_C(1) << chessFrom);
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK,
          NAME,
          "hinted chess source pickup is accepted");
    occupancy |= UINT64_C(1) << chessTo;
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          NAME,
          "hinted chess placement enters its acknowledgement window");

    // Move the Duck immediately instead of waiting for the chess settle. This
    // reproduces the fast physical cadence that previously looked like an
    // unsolicited backward/Undo prompt.
    occupancy &= ~(UINT64_C(1) << duckFrom);
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::DuckRelocation) &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlacePrimary) &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              !session.IsUndoActionPending(),
          NAME,
          "fast Duck pickup advances the same compound turn without Undo");
    uint8_t remainingFrom = 255U;
    uint8_t remainingTo = 255U;
    Check(session.TryGetHintMoveSquares(remainingFrom, remainingTo) &&
              remainingFrom == duckFrom && remainingTo == duckTo &&
              !session.TryGetHintDuckMoveSquares(
                  remainingFrom, remainingTo),
          NAME,
          "after the chess step only the preselected Duck relocation remains");
    char remainingFromText[3] = {};
    char remainingToText[3] = {};
    const bool remainingEndpointsFormatted =
        litch_max::TryFormatUciSquare(
            remainingFrom,
            remainingFromText,
            sizeof(remainingFromText)) &&
        litch_max::TryFormatUciSquare(
            remainingTo,
            remainingToText,
            sizeof(remainingToText));
    char expectedRelocation[24] = {};
    (void)std::snprintf(
        expectedRelocation,
        sizeof(expectedRelocation),
        "\"as\":\"3d%s%s\"",
        remainingFromText,
        remainingToText);
    Check(remainingEndpointsFormatted &&
              std::strstr(session.HomeStateLine().CStr(),
                          expectedRelocation) != nullptr,
          NAME,
          "Home carries the selected Duck destination through the fast chess-step handoff");

    occupancy |= UINT64_C(1) << duckTo;
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              session.Advance(2000U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy &&
              session.DuckSquare() == duckTo &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              !session.IsUndoActionPending(),
          NAME,
          "preselected Duck placement commits once with no backward prompt");
    std::printf(
        "[DUCK_COMPOUND_HINT_VIRTUAL_LITCH_COMPLETE] %s chess=%u-%u "
        "duck=%u-%u board=%016llx undo=0\n",
        NAME,
        static_cast<unsigned>(chessFrom),
        static_cast<unsigned>(chessTo),
        static_cast<unsigned>(duckFrom),
        static_cast<unsigned>(duckTo),
        static_cast<unsigned long long>(occupancy));
}

void RunDuckKingCaptureHintJourney() {
    constexpr const char* NAME =
        "duck/hint/terminal-king-capture/chess-only";
    constexpr uint8_t DUCK_A5 = 24U;
    constexpr uint8_t DUCK_C5 = 26U;
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    uint64_t occupancy =
        INITIAL_OCCUPANCY | (UINT64_C(1) << DUCK_A5);
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized,
          NAME,
          "Duck opening reaches stable human play");
    if (!initialized) {
        return;
    }

    const auto playTurn = [&](const Move chess,
                              const Move duck) {
        return ApplyMove(session, occupancy, chess, false, 1U) &&
               ApplyMove(session, occupancy, duck, false, 2000U);
    };
    const auto playCaptureTurn = [&](const Move chess,
                                     const Move duck) {
        occupancy &= ~(UINT64_C(1) << chess.to);
        if (session.ObserveHallMask(occupancy) !=
            VIRTUAL_LITCH_OK) {
            return false;
        }
        occupancy &= ~(UINT64_C(1) << chess.from);
        if (session.ObserveHallMask(occupancy) !=
            VIRTUAL_LITCH_OK) {
            return false;
        }
        occupancy |= UINT64_C(1) << chess.to;
        return session.ObserveHallMask(occupancy) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(1U) == VIRTUAL_LITCH_OK &&
               ApplyMove(session, occupancy, duck, false, 2000U);
    };
    const bool prepared =
        playTurn({52U, 36U}, {DUCK_A5, DUCK_C5}) && // e2-e4
        playTurn({9U, 17U}, {DUCK_C5, DUCK_A5}) &&  // b7-b6
        playTurn({59U, 31U}, {DUCK_A5, DUCK_C5}) && // Qd1-h5
        playTurn({17U, 25U}, {DUCK_C5, DUCK_A5}) && // b6-b5
        playCaptureTurn(
            {31U, 13U}, {DUCK_A5, DUCK_C5}) &&      // Qh5xf7
        playTurn({15U, 23U}, {DUCK_C5, DUCK_A5});   // h7-h6
    Check(prepared,
          NAME,
          "production Hall observations prepare White's immediate king capture");
    if (!prepared) {
        return;
    }

    bool cached = false;
    uint8_t hintFrom = 64U;
    uint8_t hintTo = 64U;
    uint8_t duckFrom = 64U;
    uint8_t duckTo = 64U;
    for (uint8_t sample = 0U; sample < 50U; ++sample) {
        if (session.TryGetHintMoveSquares(hintFrom, hintTo) &&
            hintFrom == 13U && hintTo == 4U &&
            !session.TryGetHintDuckMoveSquares(duckFrom, duckTo)) {
            cached = true;
            break;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) {
            break;
        }
    }
    Check(cached,
          NAME,
          "real Duck search caches terminal Qf7xe8 without a Duck destination");
    if (!cached) {
        return;
    }

    modules::ControlMutationOutcome outcome;
    const bool revealed =
        SetPhysicalODown(session, modules::UiOwner::P1, true) &&
        SetPhysicalODown(session, modules::UiOwner::P1, false) &&
        session.TryGetLastBoardControlOutcome(outcome) &&
        outcome.action == modules::ControlAction::Hint &&
        outcome.status == modules::ControlMutationStatus::Applied;
    const auto home = session.HomeStateLine();
    Check(revealed && session.HintDisplayRequested() &&
              HintEquals(session, 13U, 4U) &&
              !session.TryGetHintDuckMoveSquares(duckFrom, duckTo) &&
              HasSquareGuidance(session.PhysicalFrame(), 4U) &&
              std::strstr(home.CStr(), "\"as\":\"3hf7e8\"") !=
                  nullptr,
          NAME,
          "physical O and Home both expose the existing chess-only winning Hint");
    std::printf(
        "[DUCK_KING_CAPTURE_HINT_VIRTUAL_LITCH] %s requested=%u "
        "chess=f7-e8 duck=none home=3hf7e8 accepted=%u\n",
        NAME,
        session.HintDisplayRequested() ? 1U : 0U,
        revealed ? 1U : 0U);

    constexpr uint8_t FROM = 13U; // f7
    constexpr uint8_t TO = 4U;    // e8
    constexpr uint8_t NEAR = 5U;  // f8
    constexpr uint8_t FAR = 2U;   // c8
    occupancy &= ~(UINT64_C(1) << TO);
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK,
          NAME, "remove the opposing king before placing the winning queen");
    Check(ApplyMove(session, occupancy, {FROM, TO}, false, 0U),
          NAME, "play the winning capture through Hall input");
    for (uint8_t poll = 0U; poll < 110U &&
             !session.HomeStateSnapshot().outcome.IsTerminal(); ++poll) {
        Check(session.Advance(20U) == VIRTUAL_LITCH_OK, NAME, "observe capture confirmation");
    }
    const auto terminal = session.HomeStateSnapshot();
    Check(terminal.outcome.reason == modules::VariantOutcomeReason::KingCaptured &&
          terminal.outcome.winner == modules::GameWinner::White &&
          session.DuckSquare() == DUCK_A5 && session.UndoHistoryMoveCount() == 7U,
          NAME, "winning capture ends the game once, with no Duck relocation");
    if (!terminal.outcome.IsTerminal()) return;

    const auto warm = [](const virtual_litch_color c) { return int(c.r) - int(c.b); };
    const auto capturer = session.SemanticFrame().board[TO][1U];
    const auto display = session.DisplayText();
    const auto checkpoint = [&](const char* stage, const uint32_t age) {
        const auto& frame = session.SemanticFrame();
        std::printf("[DUCK_FIRE_LOB] stage=%s age=%u occupied=%016llx "
                    "outcome=KingCaptured winner=White display=\"%.*s\" "
                    "fire-f7=%d fire-e8=%d fire-f8=%d fire-c8=%d "
                    "queen-e8=%u/%u/%u/%u next=New-Game-or-Undo\n",
                    stage, age, static_cast<unsigned long long>(occupancy),
                    static_cast<int>(display.size()), display.empty() ? "" : display.data(),
                    warm(frame.board[FROM][0U]), warm(frame.board[TO][0U]),
                    warm(frame.board[NEAR][0U]), warm(frame.board[FAR][0U]),
                    frame.board[TO][1U].r, frame.board[TO][1U].g,
                    frame.board[TO][1U].b, frame.board[TO][1U].w);
    };
    const auto at = [&](const uint8_t square) {
        return warm(session.SemanticFrame().board[square][0U]);
    };
    const int launch = at(FROM);
    const int targetBeforeTravel = at(TO);
    checkpoint("launch", 0U);
    Check(session.Advance(600U) == VIRTUAL_LITCH_OK &&
          launch > targetBeforeTravel + 20 && at(TO) > targetBeforeTravel + 20 &&
          at(FROM) < launch, NAME, "Fire Lob travels from the queen's old square to the captured king");
    checkpoint("travel", 600U);
    Check(session.Advance(310U) == VIRTUAL_LITCH_OK &&
          at(TO) > at(FAR) + 30 && ColorEnergy(capturer) != 0U &&
          ColorsEqual(capturer, session.SemanticFrame().board[TO][1U]),
          NAME, "capture square flares while the winning queen stays lit in its owner's color");
    checkpoint("impact", 910U);
    Check(session.Advance(160U) == VIRTUAL_LITCH_OK, NAME, "sample the first ember ring");
    const int earlyFar = at(FAR);
    Check(at(NEAR) > earlyFar + 20, NAME, "the burst first lights the nearest neighbors");
    checkpoint("near-embers", 1070U);
    Check(session.Advance(230U) == VIRTUAL_LITCH_OK &&
          at(FAR) > earlyFar + 15 && at(FAR) > at(NEAR), NAME,
          "the burst spreads beyond the nearest neighbors before fading");
    checkpoint("far-embers", 1300U);
    Check(MutateSetting(session, 66U, "MINIMAL_ANIMATIONS", "true"),
          NAME, "select Minimal Animations through the shared setting owner");
    const auto minimalHash = session.SemanticFrame().board_hash;
    const int minimalTarget = at(TO);
    const int minimalSource = at(FROM);
    Check(session.Advance(800U) == VIRTUAL_LITCH_OK &&
          session.SemanticFrame().board_hash == minimalHash &&
          ColorsEqual(capturer, session.SemanticFrame().board[TO][1U]) &&
          minimalTarget < launch && minimalSource < launch,
          NAME, "Minimal shows a still result with the winning queen and no capture replay");
    checkpoint("Minimal-still", 2100U);
}

void RunLegalGuideHintLatchJourney() {
    constexpr const char* NAME =
        "assistance/legal-lift/manual-toggle/recovery";
    test_support::BoundedHintSession sessionFixture(MakeConfig(false, false));
    auto& session = sessionFixture.session;
    uint64_t occupancy = INITIAL_OCCUPANCY;
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        MutateSetting(session, 1400U, "WHITE_SQUARES_ON", "false") &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized, NAME, "Fresh Setup reaches Play Ready");
    if (!initialized) {
        return;
    }

    modules::ControlMutationOutcome physicalOutcome;
    const bool requested =
        SetPhysicalODown(session, modules::UiOwner::P1, true) &&
        SetPhysicalODown(session, modules::UiOwner::P1, false) &&
        session.TryGetLastBoardControlOutcome(physicalOutcome) &&
        physicalOutcome.action == modules::ControlAction::Hint &&
        physicalOutcome.status == modules::ControlMutationStatus::Applied;
    uint8_t shownFrom = 64U, shownTo = 64U;
    Check(requested && session.HintDisplayRequested() &&
              session.TryGetHintMoveSquares(shownFrom, shownTo),
          NAME,
          "physical O requests Hint through production controls");
    Check(SetPhysicalODown(session, modules::UiOwner::P1, true) &&
              SetPhysicalODown(session, modules::UiOwner::P1, false) &&
              !session.HintDisplayRequested(),
          NAME, "Coaching OFF retap hides the manual Hint");
    Check(SetPhysicalODown(session, modules::UiOwner::P1, true) &&
              SetPhysicalODown(session, modules::UiOwner::P1, false) &&
              session.HintDisplayRequested() && HintEquals(session, shownFrom, shownTo),
          NAME, "Coaching OFF reveal reuses the exact current-position Hint");

    constexpr uint8_t SOURCE = 52U; // e2
    constexpr uint8_t NEAR = 44U; // e3
    constexpr uint8_t FAR = 36U; // e4
    constexpr uint8_t ILLEGAL = 28U; // e5
    constexpr uint64_t DESTINATIONS = (UINT64_C(1) << NEAR) | (UINT64_C(1) << FAR);
    constexpr uint64_t WRONG_LANDING = UINT64_C(1) << ILLEGAL;
    constexpr uint64_t LIFTED = INITIAL_OCCUPANCY & ~(UINT64_C(1) << SOURCE);
    constexpr std::array<uint16_t, 2U> DARK_RAILS{};
    const auto legalRails = [&]() {
        const auto& board = session.SemanticFrame().board;
        return std::array{ColorEnergy(board[NEAR][0U]), ColorEnergy(board[FAR][0U])};
    };
    const auto checkpoint = [&](const char* stage, const char* next, const char* outcome = "accepted") {
        const auto& state = session.State();
        const auto& board = session.SemanticFrame().board;
        const auto display = session.DisplayText();
        std::printf("[ASSISTANCE_LEGAL_REVEAL] stage=%s t=%u phase=%s outcome=%s occupancy=%016llx "
                    "legal=%016llx illegal=%016llx history=%u display=\"%.*s\" "
                    "e3=%u/%u e4=%u/%u next=%s\n",
                    stage, state.now_ms, runtime::ChessInteractionPhaseToString(
                        static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                    outcome,
                    static_cast<unsigned long long>(state.physical_occupied_mask),
                    static_cast<unsigned long long>(state.valid_destination_mask),
                    static_cast<unsigned long long>(state.human_illegal_mask),
                    static_cast<unsigned>(session.UndoHistoryMoveCount()),
                    static_cast<int>(display.size()), display.data(),
                    ColorEnergy(board[NEAR][0U]), ColorEnergy(board[NEAR][2U]),
                    ColorEnergy(board[FAR][0U]), ColorEnergy(board[FAR][2U]), next);
    };

    occupancy = LIFTED;
    const bool lifted =
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::PieceLifted) &&
        session.State().valid_destination_mask == DESTINATIONS;
    const auto requestedGuide = session.HomeStateLine();
    Check(lifted &&
              std::strstr(requestedGuide.CStr(),
                          "\"as\":\"1le2") != nullptr &&
              HasSquareGuidance(session.PhysicalFrame(), SOURCE),
          NAME,
          "legal pickup guidance outranks the retained requested Hint");

    virtual_litch_action_request hint{};
    virtual_litch_default_action_request(&hint);
    hint.request_id = 1401U;
    hint.action = VIRTUAL_LITCH_CONTROL_HINT;
    hint.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome hintOutcome;
    const uint64_t heldDestinations = session.State().valid_destination_mask;
    const bool hiddenAction =
        session.TryGetHintMoveSquares(shownFrom, shownTo) &&
        session.DispatchControlAction(hint, hintOutcome) == VIRTUAL_LITCH_OK &&
        hintOutcome.status == modules::ControlMutationStatus::Applied &&
        !session.HintDisplayRequested();
    const auto hiddenGuide = session.HomeStateLine();
    const bool hiddenWhileLifted = hiddenAction &&
        session.State().selected_square == SOURCE &&
        session.State().valid_destination_mask == heldDestinations &&
        std::strstr(hiddenGuide.CStr(), "\"as\":\"0le2") != nullptr &&
        HasSquareGuidance(session.PhysicalFrame(), SOURCE);
    Check(hiddenWhileLifted, NAME,
          "direct Hint hides manual guidance while the unrelated pickup keeps its exact legal mask and physical guide");

    Check(legalRails() == DARK_RAILS, NAME, "the first frame admits both legal destinations before the wave reaches either");
    for (uint8_t target : {NEAR, FAR}) {
        const auto& square = session.SemanticFrame().board[target];
        const auto& frame = session.PhysicalFrame();
        Check(ColorEnergy(square[0U]) == 0U && ColorEnergy(square[2U]) == 0U &&
                  ColorEnergy(square[1U]) == 0U &&
                  !HasSquareGuidance(frame, target) &&
                  PhysicalSquareEnergy(frame, target, hardware::SquareLedSlot::Piece) == 0U,
              NAME, "waiting legal destinations begin dark without false piece lights");
    }
    Check(!HasSquareGuidance(session.PhysicalFrame(), ILLEGAL), NAME,
          "the sweep leaves e5 unlit because it is outside the legal mask");
    checkpoint("pickup", "place-e3-or-e4-or-return-e2");
    Check(session.Advance(66U) == VIRTUAL_LITCH_OK, NAME,
          "advance the held pickup past the first distance band");
    const auto progressing = legalRails();
    Check(progressing[0U] > 0U && progressing[1U] == 0U &&
              HasSquareGuidance(session.PhysicalFrame(), NEAR) &&
              !HasSquareGuidance(session.PhysicalFrame(), FAR) &&
              session.State().valid_destination_mask == DESTINATIONS,
          NAME, "e3 lights first while the already legal e4 remains dark");
    checkpoint("near-first", "place-e3-or-e4-or-return-e2");
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK && legalRails() == progressing,
          NAME, "an unchanged Hall observation does not restart the reveal");
    Check(session.Advance(66U) == VIRTUAL_LITCH_OK, NAME, "advance into the e4 reveal");
    const auto farProgress = legalRails();
    Check(farProgress[0U] > progressing[0U] && farProgress[1U] > 0U &&
              farProgress[1U] < farProgress[0U] && HasSquareGuidance(session.PhysicalFrame(), FAR),
          NAME, "e4 brightens after e3 has reached its steady level");
    Check(session.Advance(33U) == VIRTUAL_LITCH_OK, NAME, "advance past the e4 reveal");
    const auto full = legalRails();
    const auto fullColor = session.SemanticFrame().board[NEAR][0U];
    Check(full[0U] == farProgress[0U] && full[1U] > farProgress[1U] &&
              ColorsEqual(fullColor, session.SemanticFrame().board[FAR][0U]) &&
              session.State().valid_destination_mask == DESTINATIONS,
          NAME, "both rails reach the same steady cue with the original legal mask");
    checkpoint("full", "place-e3-or-e4-or-return-e2");

    hint.request_id = 1402U;
    uint8_t revealedFrom = 64U, revealedTo = 64U;
    const bool revealedAction =
        session.DispatchControlAction(hint, hintOutcome) == VIRTUAL_LITCH_OK &&
        hintOutcome.status == modules::ControlMutationStatus::Applied &&
        session.HintDisplayRequested() &&
        session.TryGetHintMoveSquares(revealedFrom, revealedTo) &&
        revealedFrom == shownFrom && revealedTo == shownTo;
    const auto revealedGuide = session.HomeStateLine();
    const bool revealedWhileLifted = revealedAction &&
        session.State().selected_square == SOURCE &&
        session.State().valid_destination_mask == heldDestinations &&
        std::strstr(revealedGuide.CStr(), "\"as\":\"1le2") != nullptr &&
        HasSquareGuidance(session.PhysicalFrame(), SOURCE);
    Check(revealedWhileLifted, NAME,
          "direct Hint reveals the exact retained tuple while legal pickup guidance remains authoritative");

    occupancy |= UINT64_C(1) << SOURCE;
    const bool recovered =
        session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::Idle) &&
        session.HintDisplayRequested() && HintEquals(session, shownFrom, shownTo) &&
        std::strstr(session.HomeStateLine().CStr(),
                    "\"as\":\"3h") != nullptr;
    Check(recovered,
          NAME,
          "restoring the lifted piece recovers the still-requested Hint presentation");
    std::printf(
        "[ASSISTANCE_LEGAL_GUIDE_VIRTUAL_LITCH] requested=1 coaching=0 source=e2 "
        "mask=%016llx hidden=%u revealed=%u recovery=shown-Hint accepted=%u\n",
        static_cast<unsigned long long>(heldDestinations),
        hiddenWhileLifted ? 1U : 0U, revealedWhileLifted ? 1U : 0U,
        hiddenWhileLifted && revealedWhileLifted && recovered ? 1U : 0U);

    hint.request_id = 1403U;
    Check(session.DispatchControlAction(hint, hintOutcome) == VIRTUAL_LITCH_OK &&
              hintOutcome.status == modules::ControlMutationStatus::Applied &&
              !session.HintDisplayRequested(), NAME, "hide Hint before the ordinary pickup recovery");
    Check(session.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK && legalRails() == DARK_RAILS,
          NAME, "returning and lifting e2 again starts one fresh reveal");
    checkpoint("new-pickup", "place-e3-or-e4-or-return-e2");
    Check(session.ObserveHallMask(LIFTED | WRONG_LANDING) == VIRTUAL_LITCH_OK &&
              session.State().human_illegal_mask == WRONG_LANDING && session.UndoHistoryMoveCount() == 0U,
          NAME, "placing on e5 is rejected immediately during the reveal");
    checkpoint("e5-rejected", "lift-e5", "rejected");
    Check(session.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK &&
              session.State().human_illegal_mask == 0ULL && session.State().correction_mask == 0ULL &&
              session.State().valid_destination_mask == DESTINATIONS && legalRails() == full,
          NAME, "lifting e5 clears correction and restores steady legal guidance");
    checkpoint("recovered", "place-e3-or-e4-or-return-e2");
    Check(session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask == 0ULL && legalRails() == DARK_RAILS,
          NAME, "returning the recovered piece clears its legal destinations");
    Check(session.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK && legalRails() == DARK_RAILS &&
              session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK && legalRails() == DARK_RAILS &&
              session.Advance(400U) == VIRTUAL_LITCH_OK && legalRails() == DARK_RAILS &&
              session.UndoHistoryMoveCount() == 0U,
          NAME, "returning e2 during a fresh reveal clears both destinations through expiry");
    checkpoint("returned", "lift-a-piece");

    Check(MutateSetting(session, 1404U, "MINIMAL_ANIMATIONS", "true") &&
              session.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask == DESTINATIONS &&
              ColorsEqual(fullColor, session.SemanticFrame().board[NEAR][0U]) &&
              ColorsEqual(fullColor, session.SemanticFrame().board[FAR][0U]) &&
              legalRails() == full,
          NAME, "Minimal shows both destinations at the steady full color from pickup");
    checkpoint("minimal-pickup", "place-e3-or-e4-or-return-e2");
    Check(session.Advance(400U) == VIRTUAL_LITCH_OK && legalRails() == full &&
              session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask == 0ULL && session.UndoHistoryMoveCount() == 0U,
          NAME, "Minimal stays steady while held and returns without committing a move");

    const auto mappedRails = [&](const uint8_t target) {
        const auto& frame = session.PhysicalFrame();
        return std::array{
            PhysicalSquareEnergy(frame, target, hardware::SquareLedSlot::Square0),
            PhysicalSquareEnergy(frame, target, hardware::SquareLedSlot::Square2)};
    };
    Check(MutateSetting(session, 1405U, "MINIMAL_ANIMATIONS", "false") &&
              MutateSetting(session, 1406U, "BRIGHTNESS", "0") &&
              session.ObserveHallMask(LIFTED) == VIRTUAL_LITCH_OK &&
              session.State().valid_destination_mask == DESTINATIONS && legalRails() == full,
          NAME, "minimum brightness preserves full legal guidance immediately with motion otherwise enabled");
    const auto nearFirst = mappedRails(NEAR);
    const auto farFirst = mappedRails(FAR);
    checkpoint("minimum-brightness-pickup", "place-e3-or-e4-or-return-e2");
    Check(nearFirst[0U] > 0U && nearFirst[1U] > 0U && farFirst[0U] > 0U && farFirst[1U] > 0U &&
              session.Advance(400U) == VIRTUAL_LITCH_OK && legalRails() == full &&
              mappedRails(NEAR) == nearFirst && mappedRails(FAR) == farFirst,
          NAME, "both mapped rails at minimum brightness are visible from pickup at their steady level");
}

void RunLegalRevealGeometryJourney() {
    constexpr const char* NAME = "assistance/legal-reveal/V3.3-bishop-and-knight";
    constexpr auto REVISION = hardware::BoardRevision::V3_3S3;
    constexpr uint8_t BISHOP = 56U; // a1
    constexpr uint8_t KNIGHT = 57U; // b1
    const auto bit = [](const uint8_t square) { return UINT64_C(1) << square; };
    const uint64_t occupied = bit(0U) | bit(BISHOP) | bit(KNIGHT) | bit(63U);
    constexpr std::array<uint8_t, 7U> RAY{49U, 42U, 35U, 28U, 21U, 14U, 7U};
    uint64_t bishopMoves = 0ULL;
    for (const auto square : RAY) bishopMoves |= bit(square);
    const uint64_t knightMoves = bit(40U) | bit(42U) | bit(51U); // a3, c3, d2

    auto config = MakeConfig(false, false);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(INITIAL_OCCUPANCY) == VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        MutateSetting(session, 1410U, "WHITE_SQUARES_ON", "false") &&
        MutateSetting(session, 1411U, "SETTLE_ANIM_STYLE", "\"none\"") &&
        session.LoadExternalFen("k7/8/8/8/8/8/8/BN5K w - - 0 1") &&
        session.ObserveHallMask(occupied) == VIRTUAL_LITCH_OK && session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(ready, NAME, "external FEN and matching raw Hall occupancy prepare the seven-square bishop ray");
    if (!ready) return;

    const auto sample = [&](const uint8_t source, const uint64_t legal, const uint64_t expected,
                            const uint32_t age) {
        uint64_t left = 0ULL, right = 0ULL;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (square == source) continue;
            const auto& frame = session.PhysicalFrame();
            if (ColorEnergy(PhysicalSquareColor(frame, square, hardware::SquareLedSlot::Square0, REVISION)))
                left |= bit(square);
            if (ColorEnergy(PhysicalSquareColor(frame, square, hardware::SquareLedSlot::Square2, REVISION)))
                right |= bit(square);
        }
        const auto& state = session.State();
        const auto display = session.DisplayText();
        std::printf("[LEGAL_REVEAL_V33] source=%u age=%u phase=%u occupied=%016llx legal=%016llx "
                    "rails=%016llx/%016llx expected=%016llx text=\"%.*s\" next=place-or-return\n",
                    source, age, state.interaction_phase,
                    static_cast<unsigned long long>(state.physical_occupied_mask),
                    static_cast<unsigned long long>(state.valid_destination_mask),
                    static_cast<unsigned long long>(left), static_cast<unsigned long long>(right),
                    static_cast<unsigned long long>(expected), static_cast<int>(display.size()), display.data());
        Check(state.interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                  state.selected_square == source && state.valid_destination_mask == legal &&
                  state.correction_mask == 0ULL && left == expected && right == expected,
              NAME, "mapped rails expose precisely the legal destinations reached by this frame");
    };
    const uint64_t bishopLifted = occupied & ~bit(BISHOP);
    Check(session.ObserveHallMask(bishopLifted) == VIRTUAL_LITCH_OK, NAME, "lift the a1 bishop by raw Hall scan");
    constexpr std::array<uint8_t, 12U> VISIBLE_BISHOP{0U, 1U, 1U, 2U, 3U, 3U, 4U, 5U, 5U, 6U, 7U, 7U};
    uint64_t visible = 0ULL;
    uint8_t reached = 0U;
    uint32_t age = 0U;
    for (const auto count : VISIBLE_BISHOP) {
        while (reached < count) visible |= bit(RAY[reached++]);
        age += 33U;
        Check(session.Advance(33U) == VIRTUAL_LITCH_OK, NAME, "sample a physical-cadence bishop frame");
        sample(BISHOP, bishopMoves, visible, age);
        if (age == 132U) {
            Check(session.ObserveHallMask(bishopLifted) == VIRTUAL_LITCH_OK, NAME, "send an unchanged held scan");
            sample(BISHOP, bishopMoves, visible, age);
        }
    }
    Check(session.ObserveHallMask(occupied) == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(occupied & ~bit(KNIGHT)) == VIRTUAL_LITCH_OK,
          NAME, "return the bishop, then lift the b1 knight by raw Hall scan");
    for (age = 33U; age <= 165U; age += 33U) {
        Check(session.Advance(33U) == VIRTUAL_LITCH_OK, NAME, "sample a physical-cadence knight frame");
        sample(KNIGHT, knightMoves, age < 100U ? 0ULL : knightMoves, age);
    }
}

void RunCpuCaptureCorrectionJourney(const bool tentativeEntry,
                                    const bool returnCapturePieces) {
    constexpr const char* FEN =
        "7k/8/8/3p4/8/8/8/3R2NK w - - 0 1";
    constexpr uint8_t SOURCE = 59U; // d1
    constexpr uint8_t TARGET = 27U; // d5
    constexpr uint8_t EXTRA = 62U; // g1
    constexpr uint8_t WRONG = 47U; // h3
    constexpr uint64_t SOURCE_MASK = UINT64_C(1) << SOURCE;
    constexpr uint64_t TARGET_MASK = UINT64_C(1) << TARGET;
    constexpr uint64_t EXTRA_MASK = UINT64_C(1) << EXTRA;
    constexpr uint64_t WRONG_MASK = UINT64_C(1) << WRONG;
    constexpr uint64_t BEFORE = (UINT64_C(1) << 7U) |
        TARGET_MASK | SOURCE_MASK | EXTRA_MASK | (UINT64_C(1) << 63U);
    constexpr uint64_t AFTER = BEFORE & ~SOURCE_MASK;
    char runName[96] = {};
    (void)std::snprintf(runName, sizeof(runName),
        "cpu-capture-correction/%s/%s",
        tentativeEntry ? "tentative-entry" : "hard-entry",
        returnCapturePieces ? "returned-victim-resets-proof" : "retain-vacancy");

    ControlledCoachHintEngine engine;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession session(MakeConfig(false, false), engine, worker);
    const auto advance = [&](const uint32_t durationMs) {
        for (uint32_t elapsed = 0U; elapsed < durationMs; elapsed += 10U) {
            if (session.Advance(10U) != VIRTUAL_LITCH_OK) return false;
            std::this_thread::yield();
        }
        return true;
    };
    if (!InitializeRawCoachFixture(session, engine, worker, runName)) return;
    engine.SetScriptedMoves("a2a3", "d1d5");
    engine.SetHoldAutomaticTail(false);
    const bool initialized =
        MutateSetting(session, 1461U, "VS_CPU_LEVEL", "2") &&
        MutateSetting(session, 1462U, "VS_CPU_PLAYER", "1") &&
        MutateSetting(session, 1463U, "MOVE_CONFIRM", "\"settle_2s\"") &&
        session.LoadExternalFen(FEN) &&
        session.ObserveHallMask(BEFORE) == VIRTUAL_LITCH_OK &&
        advance(2000U) && engine.WaitForCompletion(2U) &&
        WaitForWorkerInactive(worker) && advance(10U);
    const auto fixedPlanPresent = [&]() {
        return std::strstr(session.HomeStateLine().CStr(),
            "\"c\":[\"d1\",\"d5\",\"d5\"]") != nullptr;
    };
    Check(initialized && fixedPlanPresent() &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::ExecutingPlan) &&
              session.State().required_action == static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::LiftPrimary) &&
              session.UndoHistoryMoveCount() == 0U,
          runName, "real rules and production settings queue the scripted CPU rook capture");
    if (!initialized || !fixedPlanPresent()) return;

    uint64_t occupancy = BEFORE;
    const auto uncommitted = [&]() {
        return session.UndoHistoryMoveCount() == 0U &&
            session.State().expected_occupied_mask == BEFORE &&
            session.State().side_to_move_black == 0U &&
            session.State().pending_action_token == 0U &&
            std::strstr(session.HomeStateLine().CStr(),
                "\"fen\":\"7k/8/8/3p4/8/8/8/3R2NK w ") != nullptr;
    };
    const auto checkpoint = [&](const char* input, const char* next) {
        const auto& state = session.State();
        const auto left = PhysicalSquareColor(session.PhysicalFrame(), TARGET,
            hardware::SquareLedSlot::Square0);
        const auto right = PhysicalSquareColor(session.PhysicalFrame(), TARGET,
            hardware::SquareLedSlot::Square2);
        std::printf("[CPU_CAPTURE_CORRECTION_JOURNEY] %s t=%u input=%s "
            "occupancy=%016llx expected=%016llx phase=%s action=%s "
            "cpu_invalid=%016llx token=%u history=%u fixed_plan=%u "
            "victim_rails=%u/%u/%u/%u:%u/%u/%u/%u victim_piece=%u "
            "g1_guided=%u h3_guided=%u red_fault=%u text=%.*s next=%s home=%s\n",
            runName, state.now_ms, input,
            static_cast<unsigned long long>(state.physical_occupied_mask),
            static_cast<unsigned long long>(state.expected_occupied_mask),
            runtime::ChessInteractionPhaseToString(
                static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
            runtime::RequiredPhysicalActionToString(
                static_cast<runtime::RequiredPhysicalAction>(state.required_action)),
            static_cast<unsigned long long>(state.cpu_illegal_mask),
            state.pending_action_token, session.UndoHistoryMoveCount(),
            fixedPlanPresent() ? 1U : 0U,
            left.r, left.g, left.b, left.w, right.r, right.g, right.b, right.w,
            PhysicalSquareEnergy(session.PhysicalFrame(), TARGET,
                hardware::SquareLedSlot::Piece),
            HasSquareGuidance(session.PhysicalFrame(), EXTRA) ? 1U : 0U,
            HasSquareGuidance(session.PhysicalFrame(), WRONG) ? 1U : 0U,
            StatusIsFullRedFault(session.SemanticFrame()) ? 1U : 0U,
            static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
            next, session.HomeStateLine().CStr());
    };
    const auto observe = [&](const char* input, const char* next) {
        // Each actual handling step has its own observed Hall snapshot and
        // 250 ms dwell; no coalesced or unobserved vacancy is required.
        Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK && advance(250U),
            runName, "separated physical input and ten-millisecond loop ticks are admitted");
        checkpoint(input, next);
    };
    checkpoint("CPU-capture-ready", "lift-d1-or-remove-d5");

    if (returnCapturePieces) {
        occupancy &= ~TARGET_MASK;
        observe("remove-d5", "lift-d1");
    }
    if (tentativeEntry || returnCapturePieces) {
        occupancy &= ~SOURCE_MASK;
        observe("lift-d1", tentativeEntry ? "temporarily-place-rook-h3" : "remove-g1");
    }
    if (tentativeEntry) {
        occupancy |= WRONG_MASK;
        observe("place-rook-h3", "lift-unrelated-g1");
        const auto amber = PhysicalSquareColor(session.PhysicalFrame(), WRONG,
            hardware::SquareLedSlot::Square0);
        Check(session.State().cpu_illegal_mask == 0ULL && uncommitted() &&
                  amber.r > amber.g && amber.g > amber.b &&
                  !StatusIsFullRedFault(session.SemanticFrame()),
            runName, "wrong-square mover placement enters calm tentative correction");
    }
    occupancy &= ~EXTRA_MASK;
    observe("lift-unrelated-g1", returnCapturePieces ? "return-rook-d1-and-victim-d5" : "remove-d5");
    Check((session.State().cpu_illegal_mask & EXTRA_MASK) != 0ULL &&
              uncommitted() && fixedPlanPresent(),
        runName, "hard correction retains the exact CPU capture without an engine action");

    if (returnCapturePieces) {
        if (tentativeEntry) {
            occupancy &= ~WRONG_MASK;
            observe("lift-rook-h3", "return-rook-d1");
        }
        occupancy |= SOURCE_MASK;
        observe("return-rook-d1", "return-victim-d5");
        occupancy |= TARGET_MASK;
        observe("return-victim-d5", "lift-rook-d1-again");
        Check(uncommitted() && fixedPlanPresent(), runName,
            "full controlled-piece restoration behind correction preserves the fixed CPU plan");
        occupancy &= ~SOURCE_MASK;
        observe("lift-rook-d1-again", "restore-g1-then-remove-d5");
    } else {
        occupancy &= ~TARGET_MASK;
        observe("remove-d5-during-correction", tentativeEntry ? "lift-rook-h3" : "lift-rook-d1");
        if (tentativeEntry) {
            occupancy &= ~WRONG_MASK;
            observe("lift-rook-h3", "place-rook-d5");
        } else {
            occupancy &= ~SOURCE_MASK;
            observe("lift-rook-d1-during-correction", "place-rook-d5");
        }
        occupancy |= TARGET_MASK;
        observe("place-rook-d5-during-correction", "restore-g1");
    }

    Check(advance(2100U) && uncommitted() && fixedPlanPresent(), runName,
        "holding the unrelated error past the two-second deadline cannot commit the capture");
    for (const uint8_t owner : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
        Check(session.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::O, true) ==
                  VIRTUAL_LITCH_OK && advance(30U) &&
                  session.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::O, false) ==
                  VIRTUAL_LITCH_OK && advance(30U) && uncommitted() && fixedPlanPresent(),
            runName, "neither physical O can create an action behind CPU correction");
    }
    checkpoint("deadline-expired-and-both-O-tapped", "restore-g1");

    occupancy |= EXTRA_MASK;
    Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
              advance(190U) && uncommitted(),
        runName, "clearing correction cannot bypass the normal 200 ms placement stability gate");
    if (returnCapturePieces) {
        Check(advance(410U) && uncommitted() && fixedPlanPresent() &&
                  session.State().cpu_illegal_mask == 0ULL &&
                  session.State().required_action == static_cast<uint8_t>(
                      runtime::RequiredPhysicalAction::RemoveCapture),
            runName, "restored victim invalidates old proof and requests fresh capture removal");
        const auto left = PhysicalSquareColor(session.PhysicalFrame(), TARGET,
            hardware::SquareLedSlot::Square0);
        const auto right = PhysicalSquareColor(session.PhysicalFrame(), TARGET,
            hardware::SquareLedSlot::Square2);
        const auto isWhite = [](const virtual_litch_color color) {
            return color.w != 0U || (color.r != 0U && color.r == color.g && color.g == color.b);
        };
        Check((isWhite(left) || isWhite(right)) &&
                  PhysicalSquareEnergy(session.PhysicalFrame(), TARGET,
                      hardware::SquareLedSlot::Piece) == 0U &&
                  !StatusIsFullRedFault(session.SemanticFrame()),
            runName, "still-present victim receives physical white removal rails with its piece LEDs dark");
        checkpoint("g1-restored-old-proof-rejected", "remove-victim-d5");
        occupancy &= ~TARGET_MASK;
        observe("fresh-remove-d5", "place-rook-d5");
        Check(uncommitted(), runName, "fresh vacancy alone cannot commit while the rook is held");
        occupancy |= TARGET_MASK;
        Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
                  advance(190U) && uncommitted(),
            runName, "fresh capture landing still requires normal stability");
    }
    Check(advance(10U) && session.UndoHistoryMoveCount() == 1U &&
              session.State().expected_occupied_mask == AFTER &&
              session.State().side_to_move_black == 1U &&
              session.State().pending_action_token == 0U &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              std::strstr(session.HomeStateLine().CStr(),
                  "\"fen\":\"7k/8/8/3R4/8/8/8/6NK b ") != nullptr,
        runName, "valid capture commits exactly once after correction and physical stability");
    checkpoint("stable-capture-acknowledged", "Black-human-turn");
    Check(advance(2500U) && session.UndoHistoryMoveCount() == 1U &&
              session.State().expected_occupied_mask == AFTER &&
              session.State().physical_occupied_mask == AFTER &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle) &&
              session.State().required_action == static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::None) &&
              !fixedPlanPresent() && !StatusIsFullRedFault(session.SemanticFrame()),
        runName, "CPU settle expiry retains one acknowledged capture and clean human turn");
    checkpoint("CPU-settle-expired", "Black-human-move");
}

void RunCpuPlacementSettleJourney(const bool firstPlayerIsP2,
                                  const bool boardRotate,
                                  const bool leaveDestinationAbsent) {
    constexpr const char* FEN =
        "4k3/8/8/8/8/8/4P3/4K3 b - - 0 1";
    constexpr uint64_t BEFORE = (UINT64_C(1) << 4U) |
                                (UINT64_C(1) << 52U) |
                                (UINT64_C(1) << 60U);
    char runName[96] = {};
    (void)std::snprintf(runName,
                        sizeof(runName),
                        "cpu-placement-%s/first-player-%u/rotate-%u",
                        leaveDestinationAbsent ? "sustained-removal" : "bounce",
                        firstPlayerIsP2 ? 2U : 1U,
                        boardRotate ? 1U : 0U);

    test_support::BoundedHintSession sessionFixture(
        MakeConfig(firstPlayerIsP2, boardRotate));
    auto& session = sessionFixture.session;
    const bool squareRotate =
        modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2);
    const bool initialized =
        session.Reset() == VIRTUAL_LITCH_OK &&
        session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        MutateSetting(session, 921U, "VS_CPU_LEVEL", "1") &&
        MutateSetting(session, 922U, "VS_CPU_PLAYER", "2") &&
        session.LoadExternalFen(FEN) &&
        session.ObserveHallMask(MapMask(BEFORE, squareRotate)) ==
            VIRTUAL_LITCH_OK &&
        session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(initialized,
          runName,
          "production settings and external position start the CPU turn");
    if (!initialized) {
        return;
    }

    bool planReady = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample) {
        if (session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::ExecutingPlan) &&
            session.State().required_action == static_cast<uint8_t>(
                                                   runtime::RequiredPhysicalAction::LiftPrimary)) {
            planReady = true;
            break;
        }
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) {
            break;
        }
    }
    Check(planReady,
          runName,
          "real engine supplies a prompted CPU relocation");
    if (!planReady) {
        return;
    }

    constexpr uint8_t source = 4U;
    uint64_t occupancy = BEFORE & ~(UINT64_C(1) << source);
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.State().required_action == static_cast<uint8_t>(
                                                     runtime::RequiredPhysicalAction::PlacePrimary),
          runName,
          "prompted CPU piece pickup advances to placement");
    // This deliberately sparse position has one stable level-1 result in the
    // real engine: e8-e7. Keep the physical journey explicit so a change in
    // engine choice fails at the prompt instead of silently testing another
    // interaction.
    constexpr uint8_t destination = 12U;
    occupancy |= UINT64_C(1) << destination;
    Check(session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == occupancy,
          runName,
          "stable destination acknowledges the CPU move and starts settling");
    const uint32_t settleStartedMs = session.State().now_ms;
    const uint64_t cpuResultOccupancy = occupancy;
    const uint8_t physicalDestination =
        runtime::MapSquareIndex(destination, squareRotate);
    const auto seatedSquare0 = PhysicalSquareColor(
        session.PhysicalFrame(), physicalDestination,
        hardware::SquareLedSlot::Square0);
    const auto seatedSquare2 = PhysicalSquareColor(
        session.PhysicalFrame(), physicalDestination,
        hardware::SquareLedSlot::Square2);
    const auto seatedPiece = PhysicalSquareColor(
        session.PhysicalFrame(), physicalDestination,
        hardware::SquareLedSlot::Piece);
    const bool redMover = seatedPiece.r > seatedPiece.g &&
                          seatedPiece.r > seatedPiece.b;
    const bool blueMover = seatedPiece.b > seatedPiece.r &&
                           seatedPiece.b > seatedPiece.g;
    Check(redMover || blueMover, runName,
          "seated CPU king has an identifiable default player palette");

    occupancy &= ~(UINT64_C(1) << destination);
    const bool immediateBounceObserved =
        session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
            VIRTUAL_LITCH_OK;
    // The ordinary pulse starts at zero. Preserve its same-time square cue
    // instead of inventing a new nonzero-at-entry requirement for this fix.
    const bool immediateCuePreserved = immediateBounceObserved &&
        session.State().now_ms == settleStartedMs &&
        session.State().physical_occupied_mask == occupancy &&
        session.State().expected_occupied_mask == cpuResultOccupancy &&
        session.State().interaction_phase == static_cast<uint8_t>(
            runtime::ChessInteractionPhase::Idle) &&
        session.State().required_action == static_cast<uint8_t>(
            runtime::RequiredPhysicalAction::None) &&
        session.State().side_to_move_black == 0U &&
        session.State().human_illegal_mask == 0ULL &&
        session.State().cpu_illegal_mask == 0ULL &&
        session.State().correction_mask == 0ULL &&
        ColorsEqual(seatedSquare0, PhysicalSquareColor(
            session.PhysicalFrame(), physicalDestination,
            hardware::SquareLedSlot::Square0)) &&
        ColorsEqual(seatedSquare2, PhysicalSquareColor(
            session.PhysicalFrame(), physicalDestination,
            hardware::SquareLedSlot::Square2)) &&
        PhysicalSquareEnergy(session.PhysicalFrame(), physicalDestination,
                             hardware::SquareLedSlot::Piece) == 0U &&
        StatusIsQuiet(session);
    Check(immediateCuePreserved, runName,
          "immediate destination re-lift preserves settle presentation without a transient fault");
    std::printf("[CPU_PLACEMENT_IMMEDIATE_FRAME] %s elapsed=%ums accepted=%u "
                "input=lift:%u expected_next=reseat:%u square_energy=%u/%u "
                "quiet_status=%u\n", runName,
                static_cast<unsigned>(session.State().now_ms - settleStartedMs),
                immediateCuePreserved ? 1U : 0U,
                physicalDestination, physicalDestination,
                ColorEnergy(seatedSquare0), ColorEnergy(seatedSquare2),
                StatusIsQuiet(session) ? 1U : 0U);

    const bool bounceObserved = session.Advance(100U) == VIRTUAL_LITCH_OK;
    const auto& semanticDestination = session.SemanticFrame().board[destination];
    const auto physicalSquare0 = PhysicalSquareColor(
        session.PhysicalFrame(), physicalDestination,
        hardware::SquareLedSlot::Square0);
    const auto physicalSquare2 = PhysicalSquareColor(
        session.PhysicalFrame(), physicalDestination,
        hardware::SquareLedSlot::Square2);
    const auto isMoverColor = [redMover, blueMover](const virtual_litch_color color) {
        return (redMover && color.r > color.g && color.r > color.b) ||
               (blueMover && color.b > color.r && color.b > color.g);
    };
    const bool destinationCuePreserved =
        ColorsEqual(semanticDestination[0], semanticDestination[2]) &&
        isMoverColor(semanticDestination[0]) &&
        ColorEnergy(semanticDestination[1]) == 0U &&
        ColorsEqual(physicalSquare0, physicalSquare2) &&
        isMoverColor(physicalSquare0) &&
        PhysicalSquareEnergy(session.PhysicalFrame(), physicalDestination,
                             hardware::SquareLedSlot::Piece) == 0U;
    std::printf(
        "[CPU_PLACEMENT_BOUNCE_FRAME] %s phase=%s human=%016llx "
        "cpu=%016llx correction=%016llx red_fault=%u "
        "expected_next=reseat:%u mover_color=%s cue_preserved=%u square_rgbw=%u/%u/%u/%u\n",
        runName,
        runtime::ChessInteractionPhaseToString(
            static_cast<runtime::ChessInteractionPhase>(
                session.State().interaction_phase)),
        static_cast<unsigned long long>(
            session.State().human_illegal_mask),
        static_cast<unsigned long long>(session.State().cpu_illegal_mask),
        static_cast<unsigned long long>(session.State().correction_mask),
        StatusIsFullRedFault(session.SemanticFrame()) ? 1U : 0U,
        physicalDestination, redMover ? "red" : "blue",
        destinationCuePreserved ? 1U : 0U,
        physicalSquare0.r, physicalSquare0.g, physicalSquare0.b, physicalSquare0.w);
    Check(bounceObserved &&
              destinationCuePreserved && StatusIsQuiet(session) &&
              session.State().interaction_phase == static_cast<uint8_t>(
                  runtime::ChessInteractionPhase::Idle) &&
              session.State().required_action == static_cast<uint8_t>(
                  runtime::RequiredPhysicalAction::None) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == cpuResultOccupancy &&
              session.State().side_to_move_black == 0U &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              !StatusIsFullRedFault(session.SemanticFrame()),
          runName,
          "brief destination re-lift stays inside the physical settle window without red fault feedback");

    if (leaveDestinationAbsent) {
        const bool calmBeforeDeadline =
            session.Advance(1899U) == VIRTUAL_LITCH_OK &&
            session.State().now_ms - settleStartedMs == 1999U &&
            session.State().interaction_phase == static_cast<uint8_t>(
                runtime::ChessInteractionPhase::Idle) &&
            session.State().required_action == static_cast<uint8_t>(
                runtime::RequiredPhysicalAction::None) &&
            session.State().physical_occupied_mask == occupancy &&
            session.State().expected_occupied_mask == cpuResultOccupancy &&
            session.State().human_illegal_mask == 0ULL &&
            session.State().cpu_illegal_mask == 0ULL &&
            session.State().correction_mask == 0ULL &&
            StatusIsQuiet(session) &&
            isMoverColor(PhysicalSquareColor(
                session.PhysicalFrame(), physicalDestination,
                hardware::SquareLedSlot::Square0));
        Check(calmBeforeDeadline, runName,
              "missing destination retains its calm mover cue through the last millisecond of settle");
        std::printf("[CPU_PLACEMENT_BEFORE_EXPIRY] %s elapsed=%ums accepted=%u "
                    "input=still-absent:%u expected_next=reseat:%u quiet_status=%u\n",
                    runName,
                    static_cast<unsigned>(session.State().now_ms - settleStartedMs),
                    calmBeforeDeadline ? 1U : 0U,
                    physicalDestination, physicalDestination,
                    StatusIsQuiet(session) ? 1U : 0U);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK,
              runName,
              "persistent destination removal crosses the settle boundary");
        const auto isMissingSquareCorrection = [](const virtual_litch_color first,
                                                  const virtual_litch_color piece,
                                                  const virtual_litch_color last) {
            return ColorEnergy(first) != 0U && ColorEnergy(piece) == 0U &&
                   ColorEnergy(last) != 0U && !ColorsEqual(first, last);
        };
        const auto& correctionSquare = session.SemanticFrame().board[destination];
        const bool correctionCue = isMissingSquareCorrection(
            correctionSquare[0], correctionSquare[1], correctionSquare[2]) &&
            isMissingSquareCorrection(
                PhysicalSquareColor(session.PhysicalFrame(), physicalDestination,
                                    hardware::SquareLedSlot::Square0),
                PhysicalSquareColor(session.PhysicalFrame(), physicalDestination,
                                    hardware::SquareLedSlot::Piece),
                PhysicalSquareColor(session.PhysicalFrame(), physicalDestination,
                                    hardware::SquareLedSlot::Square2));
        const bool actionable =
            session.State().now_ms - settleStartedMs == 2000U &&
            session.State().interaction_phase == static_cast<uint8_t>(
                                                     runtime::ChessInteractionPhase::Correction) &&
            session.State().required_action == static_cast<uint8_t>(
                                                   runtime::RequiredPhysicalAction::CorrectBoard) &&
            session.State().correction_mask ==
                (UINT64_C(1) << destination) &&
            correctionCue &&
            StatusIsFullRedFault(session.SemanticFrame());
        std::printf(
            "[CPU_PLACEMENT_SUSTAINED_FRAME] %s phase=%s action=%s "
            "human=%016llx cpu=%016llx correction=%016llx "
            "destination_guidance=%u red_fault=%u elapsed=%ums "
            "expected_next=reseat:%u cue=mismatch-rainbow-pair empty-center=dark\n",
            runName,
            runtime::ChessInteractionPhaseToString(
                static_cast<runtime::ChessInteractionPhase>(
                    session.State().interaction_phase)),
            runtime::RequiredPhysicalActionToString(
                static_cast<runtime::RequiredPhysicalAction>(
                    session.State().required_action)),
            static_cast<unsigned long long>(
                session.State().human_illegal_mask),
            static_cast<unsigned long long>(
                session.State().cpu_illegal_mask),
            static_cast<unsigned long long>(
                session.State().correction_mask),
            correctionCue ? 1U : 0U,
            StatusIsFullRedFault(session.SemanticFrame()) ? 1U : 0U,
            static_cast<unsigned>(session.State().now_ms - settleStartedMs),
            physicalDestination);
        Check(actionable,
              runName,
              "persistent removal becomes visible and actionable after settling instead of remaining suppressed");
        const auto physicalBlack = modules::MapOwnerToPhysicalHomeEdge(modules::UiOwner::P2, squareRotate);
        const uint8_t circle = modules::PlayerHomeLedIndex(physicalBlack, 5U);
        Check(session.Advance(2999U) == VIRTUAL_LITCH_OK &&
                  session.State().now_ms - settleStartedMs == 4999U &&
                  !session.HomeStateSnapshot().outcome.IsTerminal(), runName,
              "long post-CPU king hold retains initial pickup age through4999");
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
                  session.State().now_ms - settleStartedMs == 5000U &&
                  ColorEnergy(session.PhysicalFrame().pixels[256U + circle]) != 0U &&
                  !StatusIsFullRedFault(session.SemanticFrame()) &&
                  session.HomeStateSnapshot().action == modules::HomeStateAction::Lift &&
                  !session.HomeStateSnapshot().outcome.IsTerminal(), runName,
              "long post-CPU king hold arms owning physical Circle at5000 from initial pickup");
        std::printf("[CPU_PLACEMENT_LONG_KING_HOLD] %s elapsed=%u circle=%u red_fault=%u next=fresh-owning-Circle-or-return-king\n",
            runName, session.State().now_ms - settleStartedMs,
            ColorEnergy(session.PhysicalFrame().pixels[256U + circle]),
            StatusIsFullRedFault(session.SemanticFrame()) ? 1U : 0U);
    }

    occupancy |= UINT64_C(1) << destination;
    const bool reseated =
        session.ObserveHallMask(MapMask(occupancy, squareRotate)) ==
                  VIRTUAL_LITCH_OK &&
        session.Advance(leaveDestinationAbsent ? 1U : 1901U) ==
                  VIRTUAL_LITCH_OK;
    const modules::UiOwner humanHome = PhysicalOwnerForSide(
        modules::SideToMove::White, firstPlayerIsP2, squareRotate);
    const uint8_t humanHomeStart = humanHome == modules::UiOwner::P2 ? 0U : 16U;
    bool humanTurnVisible = false;
    bool onlyHumanHomeLit = true;
    for (uint8_t edge = 0U; edge < hardware::EDGE_LED_COUNT; ++edge) {
        if (ColorEnergy(session.PhysicalFrame().pixels[
                PHYSICAL_TOPOLOGY.physicalBoardLedCount + edge]) == 0U) {
            continue;
        }
        humanTurnVisible = true;
        if (edge < humanHomeStart || edge >= humanHomeStart + 8U) {
            onlyHumanHomeLit = false;
        }
    }
    const bool completedCleanly = reseated &&
              session.State().now_ms - settleStartedMs >= 2000U &&
              session.State().interaction_phase == static_cast<uint8_t>(
                                                       runtime::ChessInteractionPhase::Idle) &&
              session.State().required_action == static_cast<uint8_t>(
                                                       runtime::RequiredPhysicalAction::None) &&
              session.State().physical_occupied_mask == occupancy &&
              session.State().expected_occupied_mask == cpuResultOccupancy &&
              session.State().side_to_move_black == 0U &&
              session.State().pending_action_token == 0U &&
              session.State().human_illegal_mask == 0ULL &&
              session.State().cpu_illegal_mask == 0ULL &&
              session.State().correction_mask == 0ULL &&
              !session.IsUndoActionPending() &&
              humanTurnVisible && onlyHumanHomeLit &&
              !StatusIsFullRedFault(session.SemanticFrame());
    Check(completedCleanly,
          runName,
          "reseating returns to the acknowledged CPU position and correctly mapped human turn after expiry");
    const uint32_t completedElapsedMs = session.State().now_ms - settleStartedMs;
    const auto completedHome = session.HomeStateLine();
    const bool stableResult =
        std::strstr(completedHome.CStr(),
                    "\"fen\":\"8/4k3/8/8/8/8/4P3/4K3 w ") != nullptr &&
        std::strstr(completedHome.CStr(), "\"m\":\"----e8e7b\"") != nullptr &&
        session.Advance(2500U) == VIRTUAL_LITCH_OK &&
        std::strcmp(completedHome.CStr(), session.HomeStateLine().CStr()) == 0 &&
        session.State().correction_mask == 0ULL &&
        session.State().human_illegal_mask == 0ULL &&
        session.State().cpu_illegal_mask == 0ULL &&
        session.State().pending_action_token == 0U;
    Check(stableResult, runName,
          "later rendering retains the acknowledged CPU position and unchanged public last-move projection without a delayed correction");
    std::printf(
        "[CPU_PLACEMENT_VIRTUAL_LITCH_COMPLETE] %s move=%u-%u "
        "input=reseat:%u elapsed=%ums accepted=%u stable=%u "
        "expected_next=White human_home=%u turn_visible=%u board=%016llx home=%s\n",
        runName,
        static_cast<unsigned>(source),
        static_cast<unsigned>(destination),
        physicalDestination, static_cast<unsigned>(completedElapsedMs),
        completedCleanly ? 1U : 0U, stableResult ? 1U : 0U,
        humanHome == modules::UiOwner::P2 ? 2U : 1U,
        humanTurnVisible && onlyHumanHomeLit ? 1U : 0U,
        static_cast<unsigned long long>(occupancy), completedHome.CStr());
}

} // namespace

void RunHumanPromotionJourney(const virtual_litch_config config,
                              const bool choose, const bool requireO,
                              const bool cancel = false,
                              const bool minimal = false) {
    constexpr uint64_t BEFORE = (UINT64_C(1) << 7U) | (UINT64_C(1) << 8U) |
        (UINT64_C(1) << 50U) | (UINT64_C(1) << 63U);
    constexpr char FEN[] = "7k/P7/8/8/8/8/2P5/7K w - - 0 1";
    const bool rotate = modules::ComputeSquareRotate(config.board_rotate, config.first_player_is_p2);
    const bool dedicated = config.hardware == VIRTUAL_LITCH_HARDWARE_V3_3;
    const char* name = "Human promotion: automatic Queen and explicit Choose recovery";
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t physical = BEFORE;
    const auto observe = [&]() {
        return session.ObserveHallMask(MapMask(physical, rotate)) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](const runtime::VirtualLitchPhysicalButton button, const uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
        return session.SetButtonDown(owner, button, true) == VIRTUAL_LITCH_OK &&
            session.Advance(20U) == VIRTUAL_LITCH_OK &&
            session.SetButtonDown(owner, button, false) == VIRTUAL_LITCH_OK &&
            session.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    const auto controlColor = [&](const uint8_t bank, const hardware::ControlLedRole role) {
        const auto owner = bank ? modules::UiOwner::P2 : modules::UiOwner::P1;
        const uint8_t index = dedicated
            ? hardware::V3_3PhysicalIndexForControl(bank != 0U, role)
            : modules::PlayerHomeLedIndex(owner, role == hardware::ControlLedRole::O ? 5U
                : role == hardware::ControlLedRole::Left ? 3U : 4U);
        return session.PhysicalFrame().pixels[256U + index];
    };
    const auto controlEnergy = [&](const uint8_t bank, const hardware::ControlLedRole role) {
        return ColorEnergy(controlColor(bank, role));
    };
    const auto controlsVisible = [&]() {
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            for (const auto role : {hardware::ControlLedRole::O,
                                    hardware::ControlLedRole::Left,
                                    hardware::ControlLedRole::Right}) {
                if (controlEnergy(bank, role) == 0U) return false;
            }
        }
        return true;
    };
    const auto checkpoint = [&](const char* label, const char* next) {
        const auto home = session.HomeStateLine();
        const auto p1 = controlColor(0U, hardware::ControlLedRole::O);
        const auto p2 = controlColor(1U, hardware::ControlLedRole::O);
        const char* result = std::strstr(label, "blocked") != nullptr ? "rejected-O"
            : std::strstr(label, "acknowledged") != nullptr ? "accepted" : "pending";
        std::printf("[HUMAN_PROMOTION_CONTROL_RGB] checkpoint=%s result=%s p1O=%u,%u,%u p2O=%u,%u,%u\n",
                    label, result, p1.r, p1.g, p1.b, p2.r, p2.g, p2.b);
        uint32_t semantic = 0U;
        for (uint8_t i = 0U; i < 32U; ++i)
            if (ColorEnergy(session.SemanticFrame().status[i])) semantic |= UINT32_C(1) << i;
        std::printf("[HUMAN_PROMOTION_JOURNEY] checkpoint=%s topology=%s rotate=%u firstP2=%u mode=%s normalO=%u cancel=%u t=%u occupancy=%016llx phase=%u action=%u history=%u selector=%u selection=%u semantic=%08x controls=P1(O:%u,L:%u,R:%u),P2(O:%u,L:%u,R:%u) display=%.*s home=%s next=%s expiry=none\n",
            label, dedicated ? "V3.3" : "V3", config.board_rotate, config.first_player_is_p2,
            choose ? "choose" : "absent-default-queen", requireO, cancel, session.State().now_ms,
            static_cast<unsigned long long>(session.State().physical_occupied_mask),
            session.State().interaction_phase, session.State().required_action,
            session.UndoHistoryMoveCount(), session.PromotionActive(), unsigned(session.PromotionSelection()), semantic,
            controlEnergy(0, hardware::ControlLedRole::O), controlEnergy(0, hardware::ControlLedRole::Left),
            controlEnergy(0, hardware::ControlLedRole::Right), controlEnergy(1, hardware::ControlLedRole::O),
            controlEnergy(1, hardware::ControlLedRole::Left), controlEnergy(1, hardware::ControlLedRole::Right),
            int(session.DisplayText().size()), session.DisplayText().data(), home.CStr(), next);
    };
    const auto verifyReveal = [&](const modules::PromotionPiece piece) {
        // The long Circle-settle scenario verifies the normal move gate. Its
        // existing two-second advance can already consume most of the reveal.
        if (requireO) return;
        constexpr uint64_t CENTER = UINT64_C(0x007E7E7E7E7E7E00);
        const uint8_t destination = runtime::MapSquareIndex(0U, rotate);
        const auto revision = dedicated ? hardware::BoardRevision::V3_3S3
                                        : hardware::BoardRevision::V3S3;
        const auto sample = [&](const char* stage) {
            uint64_t center = 0ULL;
            const auto& frame = session.PhysicalFrame();
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint64_t bit = UINT64_C(1) << square;
                if ((CENTER & bit) != 0ULL && HasSquareGuidance(
                        frame, runtime::MapSquareIndex(square, rotate), revision)) {
                    center |= bit;
                }
            }
            const auto landing = PhysicalSquareColor(
                frame, destination, hardware::SquareLedSlot::Square0, revision);
            std::printf("[PROMOTION_REVEAL] stage=%s mode=%s minimal=%u t=%u center=%016llx destination=%u rgbw=%u,%u,%u,%u\n",
                        stage, choose ? "choose-knight" : "automatic-queen",
                        minimal, session.State().now_ms,
                        static_cast<unsigned long long>(center), destination,
                        landing.r, landing.g, landing.b, landing.w);
            return std::make_pair(center, landing);
        };
        const auto [start, bright] = sample("opening");
        Check(start == (minimal ? 0ULL : modules::promotion_piece_cue::BoardMask(piece)) &&
                  ColorEnergy(bright) != 0U,
              name, "promotion confirmation shows the accepted piece or steady bright destination");
        Check(session.Advance(520U) == VIRTUAL_LITCH_OK, name, "advance inside Piece Reveal");
        const auto [travel, travelLanding] = sample("travel");
        Check(minimal ? travel == 0ULL && ColorsEqual(travelLanding, bright)
                      : travel != 0ULL && travel != start,
              name, "the silhouette moves only with animations enabled");
        Check(session.Advance(360U) == VIRTUAL_LITCH_OK, name, "advance to the destination cue");
        const auto [landed, landing] = sample("landed");
        Check(landed == 0ULL && ColorEnergy(landing) != 0U &&
                  (!minimal || ColorsEqual(landing, bright)),
              name, "the destination stays bright after the silhouette clears");
        Check(session.Advance(450U) == VIRTUAL_LITCH_OK, name, "advance past the confirmation");
        const auto [expired, ordinary] = sample("expired");
        Check(expired == 0ULL && ColorEnergy(ordinary) < ColorEnergy(bright) &&
                  session.UndoHistoryMoveCount() == 1U,
              name, "the confirmation expires to ordinary last-move guidance without replay");
    };
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              session.ObserveHallMask(MapMask(INITIAL_OCCUPANCY, rotate)) == VIRTUAL_LITCH_OK &&
              session.Advance(200U) == VIRTUAL_LITCH_OK &&
              MutateSetting(session, 980U, "WHITE_SQUARES_ON", "false") &&
              (!minimal || MutateSetting(session, 984U, "MINIMAL_ANIMATIONS", "true")) &&
              (!choose || MutateSetting(session, 981U, "PROMOTION_MODE", "\"choose\"")) &&
              (!requireO || MutateSetting(session, 982U, "MOVE_CONFIRM", "\"press_circle_to_confirm\"")) &&
              session.LoadExternalFen(FEN) && observe() && session.Advance(200U) == VIRTUAL_LITCH_OK,
          name, "real settings mutation, FEN seed and raw Hall scan initialize the production journey");
    physical &= ~(UINT64_C(1) << 8U);
    Check(observe(), name, "raw pawn lift accepted");
    physical |= UINT64_C(1);
    Check(observe() && session.Advance(25U) == VIRTUAL_LITCH_OK,
          name, "raw last-rank placement begins the ordinary move gate");
    const auto arrivalEnergy = [&]() {
        const auto revision = dedicated ? hardware::BoardRevision::V3_3S3
                                        : hardware::BoardRevision::V3S3;
        return ColorEnergy(PhysicalSquareColor(session.PhysicalFrame(),
            runtime::MapSquareIndex(0U, rotate), hardware::SquareLedSlot::Piece, revision));
    };
    const auto arrival = arrivalEnergy();
    auto crestEnergy = arrival;
    for (uint8_t sample = 0U; sample < 26U; ++sample) {
        Check(session.Advance(25U) == VIRTUAL_LITCH_OK, name, "sample the arrival cadence");
        crestEnergy = std::max(crestEnergy, arrivalEnergy());
    }
    Check(!session.PromotionActive() &&
              session.UndoHistoryMoveCount() == 0U && arrival != 0U &&
              (minimal ? arrival == crestEnergy && arrival == arrivalEnergy()
                       : arrival < crestEnergy && arrivalEnergy() < crestEnergy),
          name, "pawn swells once before the choice or automatic commit; Minimal stays bright");
    std::printf("[PROMOTION_ARRIVAL] start=%u crest=%u hold=%u minimal=%u\n",
                arrival, crestEnergy, arrivalEnergy(), minimal);
    Check(session.Advance(1325U) == VIRTUAL_LITCH_OK, name,
          "raw last-rank placement reaches normal move gate");
    if (requireO) {
        Check(session.UndoHistoryMoveCount() == 0U && !session.PromotionActive(), name,
              "automatic Queen and Choose both preserve the ordinary O move-confirmation gate");
        checkpoint("normal-move-confirmation", "press O to confirm the physical move");
        Check(tap(runtime::VirtualLitchPhysicalButton::O) && session.Advance(2000U) == VIRTUAL_LITCH_OK,
              name, "normal physical O completes its existing settle");
    }
    if (!choose) {
        Check(!session.PromotionActive() && session.UndoHistoryMoveCount() == 1U &&
                  std::strstr(session.HomeStateLine().CStr(), "Q6k/") != nullptr,
              name, "absent setting commits one explicit Queen without an extra promotion O");
        checkpoint("automatic-queen-acknowledged", "continue the game; replace pawn with Queen");
        verifyReveal(modules::PromotionPiece::Queen);
        Check(session.Advance(10000U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U,
              name, "automatic promotion acknowledgement is exactly once");
        return;
    }
    Check(session.PromotionActive() && session.UndoHistoryMoveCount() == 0U && controlsVisible(),
          name, "Choose keeps engine/history pending and shows both accepted banks' Left/Right/O");
    Check(tap(runtime::VirtualLitchPhysicalButton::Left) &&
              session.PromotionSelection() == modules::PromotionPiece::Knight &&
              tap(runtime::VirtualLitchPhysicalButton::Right, VIRTUAL_LITCH_OWNER_P2) &&
              session.PromotionSelection() == modules::PromotionPiece::Queen,
          name, "raw Left and Right from both accepted banks cycle the selection");
    for (const auto piece : {modules::PromotionPiece::Rook, modules::PromotionPiece::Bishop, modules::PromotionPiece::Knight}) {
        Check(tap(runtime::VirtualLitchPhysicalButton::Right) && session.PromotionSelection() == piece,
              name, "all four promotion pieces remain reachable");
    }
    const auto crest = controlEnergy(0, hardware::ControlLedRole::O);
    checkpoint("choose-crest", "Left/Right changes piece; O confirms; X cancels");
    Check(session.Advance(760U) == VIRTUAL_LITCH_OK && controlsVisible() &&
              controlEnergy(0, hardware::ControlLedRole::O) < crest,
          name, "gentle pulse has a readable trough on actual final control pixels");
    checkpoint("choose-trough", "O confirms Knight; X cancels");
    Check(MutateSetting(session, 983U, "PROMOTION_MODE", "\"queen\"") &&
              session.Advance(10000U) == VIRTUAL_LITCH_OK && session.PromotionActive() &&
              session.PromotionSelection() == modules::PromotionPiece::Knight && session.UndoHistoryMoveCount() == 0U,
          name, "setting change and inactivity cannot commit an already-open Choose modal");
    physical &= ~(UINT64_C(1) << 50U);
    Check(observe() && tap(runtime::VirtualLitchPhysicalButton::O), name, "missing unrelated piece and O reach real correction input");
    Check(session.PromotionActive() && session.UndoHistoryMoveCount() == 0U && !controlsVisible(),
          name, "correction suppresses selection controls and consumes the blocked O");
    checkpoint("blocked-o-correction", "restore c2; blocked O will not commit later");
    physical |= UINT64_C(1) << 50U;
    Check(observe() && session.Advance(200U) == VIRTUAL_LITCH_OK && session.PromotionActive() &&
              session.PromotionSelection() == modules::PromotionPiece::Knight && controlsVisible() &&
              session.UndoHistoryMoveCount() == 0U,
          name, "restoration resumes the same piece with a fresh O still required");
    checkpoint("restored-fresh-o-required", cancel ? "X cancels; return pawn to a7" : "press a fresh O for Knight");
    if (cancel) {
        Check(tap(runtime::VirtualLitchPhysicalButton::X) && !session.PromotionActive() &&
                  session.UndoHistoryMoveCount() == 0U && std::strstr(session.HomeStateLine().CStr(), "P7/") != nullptr,
              name, "raw X cancels the pending promotion without accepting a move");
        physical &= ~UINT64_C(1); Check(observe(), name, "cancel removes replacement");
        physical |= UINT64_C(1) << 8U; Check(observe() && session.Advance(200U) == VIRTUAL_LITCH_OK,
              name, "cancel restores the pre-move physical board");
        checkpoint("cancel-reconciled", "continue from the unchanged position");
    } else {
        Check(tap(runtime::VirtualLitchPhysicalButton::O, VIRTUAL_LITCH_OWNER_P2) &&
                  !session.PromotionActive() && session.UndoHistoryMoveCount() == 1U &&
                  std::strstr(session.HomeStateLine().CStr(), "N6k/") != nullptr,
              name, "fresh raw O commits the explicit Knight exactly once");
        checkpoint("knight-acknowledged", "continue the game; replace pawn with Knight");
        verifyReveal(modules::PromotionPiece::Knight);
    }
}

int main() {
    std::puts("[SPECIAL_MOVE_VIRTUAL_LITCH_JOURNEYS] start");
    for (const auto hardware : {VIRTUAL_LITCH_HARDWARE_V3, VIRTUAL_LITCH_HARDWARE_V3_3}) {
        for (const bool firstP2 : {false, true}) {
            for (const bool rotate : {false, true}) {
                auto config = MakeConfig(firstP2, rotate);
                config.hardware = hardware;
                for (const bool requireO : {false, true}) {
                    RunHumanPromotionJourney(config, false, requireO);
                    RunHumanPromotionJourney(config, true, requireO);
                }
                RunHumanPromotionJourney(config, true, false, true);
            }
        }
        auto minimalConfig = MakeConfig(false, true);
        minimalConfig.hardware = hardware;
        RunHumanPromotionJourney(minimalConfig, false, false, false, true);
        RunHumanPromotionJourney(minimalConfig, true, false, false, true);
    }


    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            RunCoachingHomeProjectionJourney(
                firstPlayerIsP2, boardRotate);
        }
    }

    // Exact field symptom matrix: the whole king move lands in one Hall scan
    // while the prior move's success animation still owns the board.
    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            for (const CastleJourney* journey : CASTLES) {
                RunCastleJourney(*journey,
                                 firstPlayerIsP2,
                                 boardRotate,
                                 1U,
                                 CastleScan::AtomicKing);
            }
        }
    }

    // The ordinary split-scan path remains covered for every logical castle.
    for (const CastleJourney* journey : CASTLES) {
        RunCastleJourney(*journey, false, false, 5001U, CastleScan::SplitKing);
    }
    RunCastleJourney(WHITE_KINGSIDE, false, false, 1U, CastleScan::SplitKing);
    RunCastleJourney(WHITE_QUEENSIDE, false, false, 1U, CastleScan::SplitKing);

    // Intentional atomic input: both sources disappear while the king lands.
    // Four journeys cover every legal castle and each owner/rotation pairing.
    for (size_t index = 0U; index < CASTLES.size(); ++index) {
        RunCastleJourney(*CASTLES[index], (index & 1U) != 0U,
                         (index & 2U) != 0U, 5001U,
                         CastleScan::AtomicKingWithRookHeld);
    }

    RunCastleJourney(WHITE_KINGSIDE,
                     false,
                     false,
                     1U,
                     CastleScan::AtomicKing,
                     JourneyExit::Cancel);
    RunCastleJourney(WHITE_QUEENSIDE,
                     true,
                     true,
                     1U,
                     CastleScan::AtomicKing,
                     JourneyExit::CorrectAndComplete);
    // Both assistance modes must survive every logical castle and every
    // physical owner/orientation mapping. These settled journeys prove the
    // Hint is genuinely eligible and visible before the king is lifted.
    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            for (const CastleJourney* journey : CASTLES) {
                for (const AssistanceMode assistance : {
                         AssistanceMode::Hint,
                         AssistanceMode::Coaching}) {
                    RunCastleJourney(*journey,
                                     firstPlayerIsP2,
                                     boardRotate,
                                     5001U,
                                     CastleScan::SplitKing,
                                     JourneyExit::Complete,
                                     assistance);
                }
            }
        }
    }

    // The closed Bishop-b5 position has developed minors and a sheltered
    // kingside. The queenside position has no central pawn shelter, a clear
    // castle path and an open d-file for its rook. Color/rank mirrors preserve
    // those reasons for Black. Exercise every physical-orientation quadrant
    // so actual CPU-selected castles prove the same king-to-rook handoff.
    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            for (const CpuCastleJourney& journey : CPU_CASTLES) {
                RunCpuCastleJourney(
                    journey, firstPlayerIsP2, boardRotate);
                RunHumanVsCpuCastleJourney(
                    journey, firstPlayerIsP2, boardRotate);
            }
        }
    }
    RunCpuCastleJourney(
        CPU_CASTLES[0], true, false, true);
    RunCpuCastleJourney(
        CPU_CASTLES[3], false, true, true);
    RunHumanVsCpuCastleJourney(
        CPU_CASTLES[1], true, false, true);
    RunHumanVsCpuCastleJourney(
        CPU_CASTLES[2], false, true, true);

    for (const bool moverBlack : {false, true}) {
        for (const bool firstPlayerIsP2 : {false, true}) {
            for (const bool boardRotate : {false, true}) {
                RunCpuPromotionJourney(moverBlack, firstPlayerIsP2, boardRotate);
            }
        }
    }
    RunEnPassantCarryoverJourney();
    RunInvalidCarryoverCorrectionJourney();
    RunLegalGuideHintLatchJourney();
    RunLegalRevealGeometryJourney();
    RunDuckCompoundHintJourney();
    g_failures += RunCpuDuckUndoVirtualLitchJourneys();
    g_failures += RunCpuDuckDurabilityVirtualLitchJourneys();
    RunDuckKingCaptureHintJourney();
    TestRawCoachingReusesCurrentHint();
    TestRawCoachingReusesInFlightOrdinarySearch();
    TestRawCoachingEmptyCacheFailureRetries(
        EmptyCoachFailure::Start,
        "raw-coaching-empty-cache-start-failure");
    TestRawCoachingEmptyCacheFailureRetries(
        EmptyCoachFailure::Error,
        "raw-coaching-empty-cache-error");
    TestRawCoachingEmptyCacheFailureRetries(
        EmptyCoachFailure::Canceled,
        "raw-coaching-empty-cache-canceled");
    TestRawCoachingCancelAndReenter();
    TestRawCastlePromptRetainsHeldAutomaticSearch();
    for (const bool tentativeEntry : {false, true}) {
        RunCpuCaptureCorrectionJourney(tentativeEntry, false);
        RunCpuCaptureCorrectionJourney(tentativeEntry, true);
    }
    for (const bool firstPlayerIsP2 : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            RunCpuPlacementSettleJourney(
                firstPlayerIsP2, boardRotate, false);
            RunCpuPlacementSettleJourney(
                firstPlayerIsP2, boardRotate, true);
        }
    }

    g_failures += RunSettleEffectsVirtualLitchJourneys();
    g_failures += RunRapidSettleHandoffVirtualLitchJourneys();
    g_failures += RunOConfirmVirtualLitchJourneys();

    if (g_failures == 0) {
        std::puts("[SPECIAL_MOVE_VIRTUAL_LITCH_JOURNEYS] ok");
        return 0;
    }
    std::printf("[SPECIAL_MOVE_VIRTUAL_LITCH_JOURNEYS] failures=%d\n",
                g_failures);
    return 1;
}
