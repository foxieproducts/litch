#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/settle_animation_setting.hpp"
#include "modules/control_led_thinking_feedback.hpp"
#include "modules/animation_timing.hpp"
#include "modules/persisted_side.hpp"
#include "modules/promotion_ux.hpp"

namespace runtime {

struct GameStateMachineLastMoveInfo {
    bool valid = false;
    uint8_t from = 0;
    uint8_t to = 0;
};

struct PromotionConfirmation {
    static constexpr uint32_t REVEAL_MS = 1100U;
    static constexpr uint32_t NOTICE_MS = 2400U;

    uint8_t square = 255U;
    modules::PromotionPiece piece = modules::PromotionPiece::Queen;
    modules::PersistedSide side = modules::PersistedSide::Unknown;
    bool automatic = false;
    uint32_t startedAtMs = 0U;
};

inline constexpr uint32_t SETTLE_AFTERGLOW_MS =
    modules::settle_animation_setting::RAINBOW_ORBIT_DURATION_MS;
inline constexpr uint32_t LEGAL_DESTINATION_STEP_MS = 50U;
inline constexpr uint32_t LEGAL_DESTINATION_FADE_MS = 50U;
inline constexpr uint32_t LEGAL_DESTINATION_REVEAL_MS =
    7U * LEGAL_DESTINATION_STEP_MS + LEGAL_DESTINATION_FADE_MS;

enum class MoveRenderTransitionKind : uint8_t {
    None = 0,
    CancelUndo,
};

struct MoveRenderTransition {
    MoveRenderTransitionKind kind = MoveRenderTransitionKind::None;
    uint8_t fromSquare = 255;
    uint8_t toSquare = 255;
    bool hasCastlingRook = false; // Recorded Undo plan, never square geometry alone.
};

inline constexpr uint32_t HINT_REFINEMENT_DURATION_MS = 240U;

struct HintPresentationTuple {
    uint8_t fromSquare = 255U;
    uint8_t toSquare = 255U;
    uint8_t duckFromSquare = 255U;
    uint8_t duckToSquare = 255U;
    bool operator==(const HintPresentationTuple& other) const {
        return fromSquare == other.fromSquare && toSquare == other.toSquare &&
            duckFromSquare == other.duckFromSquare && duckToSquare == other.duckToSquare;
    }
};

struct HintRefinementPresentation {
    fxe::ElapsedTime elapsed;
    uint32_t durationMs = HINT_REFINEMENT_DURATION_MS;
    HintPresentationTuple outgoing{};
    HintPresentationTuple incoming{};
};

// A bounded presentation envelope owned by the authoritative interaction model.
// It does not change move acceptance, settlement deadlines or player ownership.
struct TurnIndicatorPresentation {
    static constexpr uint32_t FADE_MS = 180U;
    static constexpr uint16_t ENTRY_SCALE256 = 64U;
    bool initialized = false;
    bool previousBlack = false;
    uint16_t from = 0U;
    uint16_t target = 0U;
    fxe::ElapsedTime transition;
    fxe::ElapsedTime motion;

    uint16_t Scale256() const { return Scale256(fxe::Millis()); }
    uint16_t Scale256(const uint32_t nowMs) const {
        if (!initialized) return 0U;
        const uint32_t elapsed = transition.Started() ? nowMs - transition.StartMs() : 0U;
        if (elapsed >= FADE_MS) return target;
        const int32_t delta = static_cast<int32_t>(target) - from;
        return static_cast<uint16_t>(from + delta * static_cast<int32_t>(elapsed) / static_cast<int32_t>(FADE_MS));
    }

    void Request(const bool visible, const bool black) {
        const uint16_t desired = visible ? 256U : 0U;
        const bool handoff = initialized && previousBlack != black;
        if (initialized && target == desired && !handoff) return;
        // A coalesced next-side pickup already owns placement guidance. Never
        // carry the preceding player's fade into that acknowledged plan.
        from = handoff ? 0U : Scale256();
        previousBlack = black;
        if (desired != 0U && from == 0U) {
            // Quarter intensity is the existing positive-guidance level.
            // Authoritative owner selection remains with the snapshot/composer.
            from = ENTRY_SCALE256;
            motion.Reset();
        }
        target = desired;
        initialized = true;
        transition.Reset();
    }
};

struct AllPiecesFlashPresentation {
    fxe::ElapsedTime elapsed;
    uint32_t durationMs = 0U;

    bool IsActive(const uint32_t nowMs) const {
        return elapsed.Started() && nowMs - elapsed.StartMs() < durationMs;
    }

    bool IsOn(const uint32_t nowMs) const {
        static constexpr uint32_t PERIOD_MS = 200U;
        if (!IsActive(nowMs)) return false;
        // Keep the ordinary uptime phase without lengthening a pulse when
        // millis wraps: a uint32 uptime cycle is not a multiple of 400 ms.
        const uint32_t phase = (elapsed.StartMs() % (2U * PERIOD_MS) +
            (nowMs - elapsed.StartMs()) % (2U * PERIOD_MS)) % (2U * PERIOD_MS);
        return phase < PERIOD_MS;
    }

    void RetireExpired(const uint32_t nowMs) {
        // Retire at the runtime owner so a later full uptime cycle cannot
        // revive an old cue in subsequent immutable publications.
        if (elapsed.Started() && !IsActive(nowMs)) *this = {};
    }
};

struct GameStateMachineRenderState {
    TurnIndicatorPresentation turnIndicator;
    AllPiecesFlashPresentation allPiecesFlash;

    uint64_t initialKingPlacementPromptMask64 = 0;
    bool initialKingPlacementPromptActive = false;
    fxe::ElapsedTime initialKingPlacementPromptElapsed;

    fxe::ElapsedTime legalDestinationReveal;

    bool settleAfterglowActive = false;
    uint8_t settleAfterglowSquare = 255;
    bool settleAfterglowAbort = false;
    fxe::ElapsedTime settleAfterglowElapsed;

    uint8_t settleSquare = 255;
    uint8_t promotionArrivalSquare = 255U;
    uint8_t settleAnimVariant = 0;
    bool settleAnimStartEvent = false;
    fxe::ElapsedTime settleVisualElapsed;
    bool settleMoverSideKnown = false;
    bool settleMoverIsBlack = false;

    MoveRenderTransition moveTransition{};

    GameStateMachineLastMoveInfo lastMoveWhite{};
    GameStateMachineLastMoveInfo lastMoveBlack{};
    modules::PersistedSide lastMoveMostRecentSide = modules::PersistedSide::Unknown;
    PromotionConfirmation promotionConfirmation;

    HintRefinementPresentation hintRefinement{};
    bool hintRefinementReady = false;
    uint32_t hintPresentationEpoch = 0U;
    bool hintDisplayRequested = false;
    bool hintThinkingActive = false;
    bool manualHintRequested = false;
    bool manualHintAcknowledgmentValid = false;
    bool hintReplayCurrent = false;
    modules::ControlLedThinkingPresentation hintBrewing;
    modules::ControlLedThinkingPresentation thinkingControls;
    bool thinkingControlsCpu = false;
    bool hintFeedbackPending = false;
    fxe::ElapsedTime hintFeedbackElapsed;
    bool hintCancelRequested = false;
    bool hintMoverSideKnown = false;
    bool hintMoverIsBlack = false;
    bool hintMoveValid = false;
    uint8_t hintMoveFromSquare = 0;
    uint8_t hintMoveToSquare = 0;
    bool hintDuckMoveValid = false;
    uint8_t hintDuckMoveFromSquare = 0;
    uint8_t hintDuckMoveToSquare = 0;
};

inline void ResetGameStateMachineRenderState(GameStateMachineRenderState& state,
                                             const uint8_t invalidSquare = 255U) {
    state.turnIndicator = TurnIndicatorPresentation{};
    state.allPiecesFlash = {};

    state.initialKingPlacementPromptMask64 = 0;
    state.initialKingPlacementPromptActive = false;
    state.initialKingPlacementPromptElapsed = fxe::ElapsedTime();
    state.legalDestinationReveal = {};

    state.settleAfterglowActive = false;
    state.settleAfterglowSquare = invalidSquare;
    state.settleAfterglowAbort = false;
    state.settleAfterglowElapsed = fxe::ElapsedTime();

    state.settleSquare = invalidSquare;
    state.settleAnimVariant = 0;
    state.settleAnimStartEvent = false;
    state.promotionArrivalSquare = 255U;
    state.settleVisualElapsed = fxe::ElapsedTime();
    state.settleMoverSideKnown = false;
    state.settleMoverIsBlack = false;
    state.moveTransition = MoveRenderTransition{};

    state.lastMoveWhite = GameStateMachineLastMoveInfo{};
    state.lastMoveBlack = GameStateMachineLastMoveInfo{};
    state.lastMoveMostRecentSide = modules::PersistedSide::Unknown;
    state.promotionConfirmation = {};

    state.hintRefinement = HintRefinementPresentation{};
    state.hintRefinementReady = false;
    state.hintDisplayRequested = false;
    state.hintThinkingActive = false;
    state.manualHintRequested = false;
    state.manualHintAcknowledgmentValid = false;
    state.hintReplayCurrent = false;
    state.hintBrewing = modules::ControlLedThinkingPresentation{};
    state.thinkingControls = modules::ControlLedThinkingPresentation{};
    state.thinkingControlsCpu = false;
    state.hintFeedbackPending = false;
    state.hintFeedbackElapsed = fxe::ElapsedTime{};
    state.hintCancelRequested = false;
    state.hintMoverSideKnown = false;
    state.hintMoverIsBlack = false;
    state.hintMoveValid = false;
    state.hintMoveFromSquare = 0;
    state.hintMoveToSquare = 0;
    state.hintDuckMoveValid = false;
    state.hintDuckMoveFromSquare = 0;
    state.hintDuckMoveToSquare = 0;
}

inline void TriggerAllPiecesFlash(GameStateMachineRenderState& state,
                                  const uint32_t nowMs,
                                  const uint32_t durationMs) {
    state.allPiecesFlash.elapsed = fxe::ElapsedTime(nowMs);
    state.allPiecesFlash.durationMs = (durationMs == 0U) ? 1U : durationMs;
}

inline bool IsAllPiecesFlashActive(const GameStateMachineRenderState& state, const uint32_t nowMs) {
    return state.allPiecesFlash.IsActive(nowMs);
}

inline bool IsAllPiecesFlashOn(const GameStateMachineRenderState& state, const uint32_t nowMs) {
    return state.allPiecesFlash.IsOn(nowMs);
}

inline void TriggerInitialKingPlacementPrompt(
    GameStateMachineRenderState& state,
    const uint64_t kingSquaresMask64) {
    state.initialKingPlacementPromptMask64 = kingSquaresMask64;
    state.initialKingPlacementPromptActive = kingSquaresMask64 != 0ULL;
    if (state.initialKingPlacementPromptActive) {
        state.initialKingPlacementPromptElapsed.Reset();
    } else {
        state.initialKingPlacementPromptElapsed = fxe::ElapsedTime();
    }
}

inline void CancelInitialKingPlacementPrompt(
    GameStateMachineRenderState& state) {
    state.initialKingPlacementPromptMask64 = 0ULL;
    state.initialKingPlacementPromptActive = false;
    state.initialKingPlacementPromptElapsed = fxe::ElapsedTime();
}

inline bool TryGetInitialKingPlacementPrompt(
    const GameStateMachineRenderState& state,
    uint64_t& outSquaresMask64) {
    outSquaresMask64 = state.initialKingPlacementPromptMask64;
    return state.initialKingPlacementPromptActive &&
           outSquaresMask64 != 0ULL;
}

inline void StartSettleAfterglow(GameStateMachineRenderState& state,
                                 const uint8_t square,
                                 const bool abort,
                                 const uint8_t invalidSquare = 255U) {
    if (square >= 64U) {
        state.settleAfterglowActive = false;
        state.settleAfterglowSquare = invalidSquare;
        state.settleAfterglowAbort = false;
        return;
    }

    state.settleAfterglowSquare = square;
    state.settleAfterglowActive = true;
    state.settleAfterglowElapsed.Reset();
    state.settleAfterglowAbort = abort;
}

inline void ClearExpiredSettleAfterglow(GameStateMachineRenderState& state,
                                        const uint8_t invalidSquare = 255U) {
    if (!state.settleAfterglowActive || !state.settleAfterglowElapsed.HasElapsed(SETTLE_AFTERGLOW_MS)) {
        return;
    }

    state.settleAfterglowActive = false;
    state.settleAfterglowSquare = invalidSquare;
    state.settleAfterglowAbort = false;
}

inline bool TryGetSettleAfterglow(const GameStateMachineRenderState& state,
                                  uint8_t& outSquare,
                                  uint32_t& outElapsedMs) {
    if (!state.settleAfterglowActive || state.settleAfterglowSquare >= 64U) {
        return false;
    }

    const uint32_t elapsedMs = state.settleAfterglowElapsed.ElapsedMs();
    if (elapsedMs >= SETTLE_AFTERGLOW_MS) {
        return false;
    }

    outSquare = state.settleAfterglowSquare;
    outElapsedMs = elapsedMs;
    return true;
}

inline bool ConsumeSettleAnimationStarted(GameStateMachineRenderState& state, uint8_t& outVariant) {
    if (!state.settleAnimStartEvent) {
        return false;
    }

    state.settleAnimStartEvent = false;
    outVariant = state.settleAnimVariant;
    return true;
}

inline void SetLastMoveForSide(GameStateMachineRenderState& state,
                               const modules::PersistedSide side,
                               const uint8_t fromSquare,
                               const uint8_t toSquare) {
    if (side == modules::PersistedSide::White) {
        state.lastMoveWhite = GameStateMachineLastMoveInfo{true, fromSquare, toSquare};
    } else if (side == modules::PersistedSide::Black) {
        state.lastMoveBlack = GameStateMachineLastMoveInfo{true, fromSquare, toSquare};
    }
}

inline void ClearLastMoveForSide(GameStateMachineRenderState& state, const modules::PersistedSide side) {
    if (side == modules::PersistedSide::White) {
        state.lastMoveWhite = GameStateMachineLastMoveInfo{};
    } else if (side == modules::PersistedSide::Black) {
        state.lastMoveBlack = GameStateMachineLastMoveInfo{};
    }
}

inline bool TryGetLastMoveForSide(const GameStateMachineRenderState& state,
                                  const modules::PersistedSide side,
                                  uint8_t& outFromSquare,
                                  uint8_t& outToSquare) {
    const GameStateMachineLastMoveInfo* info = nullptr;
    if (side == modules::PersistedSide::White) {
        info = &state.lastMoveWhite;
    } else if (side == modules::PersistedSide::Black) {
        info = &state.lastMoveBlack;
    } else {
        return false;
    }

    if (!info->valid) {
        return false;
    }

    outFromSquare = info->from;
    outToSquare = info->to;
    return true;
}

inline void SetLastMoveMostRecentSide(GameStateMachineRenderState& state, const modules::PersistedSide side) {
    state.lastMoveMostRecentSide = side;
}

inline void ClearLastMoveMostRecentSide(GameStateMachineRenderState& state) {
    state.lastMoveMostRecentSide = modules::PersistedSide::Unknown;
}

inline bool TryGetLastMoveMostRecentSide(const GameStateMachineRenderState& state,
                                         modules::PersistedSide& outSide) {
    if (state.lastMoveMostRecentSide == modules::PersistedSide::Unknown) {
        return false;
    }

    outSide = state.lastMoveMostRecentSide;
    return true;
}

inline HintPresentationTuple CurrentHintPresentation(const GameStateMachineRenderState& state) {
    return {state.hintMoveValid ? state.hintMoveFromSquare : uint8_t{255U},
        state.hintMoveValid ? state.hintMoveToSquare : uint8_t{255U},
        state.hintDuckMoveValid ? state.hintDuckMoveFromSquare : uint8_t{255U},
        state.hintDuckMoveValid ? state.hintDuckMoveToSquare : uint8_t{255U}};
}

inline void ClearHintRefinement(GameStateMachineRenderState& state) {
    state.hintRefinement = HintRefinementPresentation{};
    state.hintRefinementReady = false;
}

inline void BeginHintRefinement(GameStateMachineRenderState& state) {
    // Coalesce roots before fade-in, then retain the single already visible tuple.
    if (state.hintRefinement.elapsed.Started()) return;
    const auto tuple = CurrentHintPresentation(state);
    state.hintRefinement = {fxe::ElapsedTime::StartNow(),
        HINT_REFINEMENT_DURATION_MS, tuple, tuple};
}

inline void RefreshHintRefinementIncoming(GameStateMachineRenderState& state) {
    auto& refinement = state.hintRefinement;
    if (refinement.elapsed.Started() &&
        refinement.elapsed.ElapsedMs() < refinement.durationMs / 2U) {
        refinement.incoming = CurrentHintPresentation(state);
    }
}

inline void AdvanceHintRefinement(HintRefinementPresentation& refinement,
                                  const HintPresentationTuple& latest,
                                  const uint32_t nowMs) {
    if (!refinement.elapsed.Started() ||
        nowMs - refinement.elapsed.StartMs() < refinement.durationMs) return;
    if (!(refinement.incoming == latest)) {
        // A queued refinement begins at the preceding handoff's absolute end.
        // Sampling late must neither restart nor slow its presentation clock.
        const uint32_t startedMs = refinement.elapsed.StartMs() + refinement.durationMs;
        const auto outgoing = refinement.incoming;
        refinement = {fxe::ElapsedTime(startedMs),
            HINT_REFINEMENT_DURATION_MS / 2U, outgoing, latest};
        if (nowMs - startedMs < refinement.durationMs) return;
    }
    refinement = HintRefinementPresentation{};
}

inline void AdvanceHintRefinement(GameStateMachineRenderState& state) {
    AdvanceHintRefinement(state.hintRefinement, CurrentHintPresentation(state), fxe::Millis());
}

// Shared by synchronous state queries and immutable presentation sampling.
inline bool SampleSettleFlashOn(const uint32_t elapsedMs,
                               const uint32_t durationMs,
                               const uint32_t startPeriodMs) {
    const uint32_t duration = durationMs == 0U ? 1U : durationMs;
    const uint32_t elapsed = elapsedMs > duration ? duration : elapsedMs;
    const uint32_t startPeriod = startPeriodMs == 0U ? 1U : startPeriodMs;
    const uint32_t minPeriod = startPeriod > 40U ? 40U
        : startPeriod > 1U ? startPeriod / 2U : 1U;
    const uint32_t span = startPeriod - minPeriod;
    const uint32_t period = startPeriod - span * elapsed / duration;
    return ((elapsed / period) % 2U) == 0U;
}

inline void SetHintDisplayRequested(GameStateMachineRenderState& state, const bool requested) {
    if (state.hintDisplayRequested == requested) {
        return;
    }
    ClearHintRefinement(state);
    ++state.hintPresentationEpoch;
    state.hintDisplayRequested = requested;
    state.hintFeedbackElapsed = requested ? fxe::ElapsedTime::StartNow()
                                         : fxe::ElapsedTime{};
}

inline void ToggleHintDisplayRequested(GameStateMachineRenderState& state) {
    SetHintDisplayRequested(state, !state.hintDisplayRequested);
}

inline HintPresentationTuple VisibleHintPresentation(const GameStateMachineRenderState& state) {
    if (state.hintMoveValid && state.hintRefinement.elapsed.Started() &&
        state.hintRefinement.elapsed.ElapsedMs() < state.hintRefinement.durationMs) {
        const auto& refinement = state.hintRefinement;
        return refinement.elapsed.ElapsedMs() < refinement.durationMs / 2U
            ? refinement.outgoing : refinement.incoming;
    }
    return CurrentHintPresentation(state);
}

inline void RetainVisibleHintPresentation(GameStateMachineRenderState& state,
    const HintPresentationTuple& shown) {
    // A physical gesture or retap owns the tuple actually shown, including
    // the outgoing half of a refinement, rather than a newer unseen candidate.
    state.hintMoveValid = shown.fromSquare < 64U && shown.toSquare < 64U;
    state.hintMoveFromSquare = shown.fromSquare;
    state.hintMoveToSquare = shown.toSquare;
    state.hintDuckMoveFromSquare = shown.duckFromSquare;
    state.hintDuckMoveToSquare = shown.duckToSquare;
    state.hintDuckMoveValid = shown.duckFromSquare < 64U && shown.duckToSquare < 64U;
}

inline void RetainVisibleHintPresentation(GameStateMachineRenderState& state) {
    RetainVisibleHintPresentation(state, VisibleHintPresentation(state));
}

inline void ReplayHintMove(GameStateMachineRenderState& state) {
    RetainVisibleHintPresentation(state);
    ClearHintRefinement(state);
    state.hintReplayCurrent = state.hintMoveValid;
    state.hintDisplayRequested = true;
    state.hintFeedbackElapsed.Reset();
}

inline bool IsHintReplayActive(const GameStateMachineRenderState& state) {
    return state.hintReplayCurrent && state.hintMoveValid && state.hintDisplayRequested &&
        state.hintFeedbackElapsed.Started() && state.hintFeedbackElapsed.ElapsedMs() <
            modules::animation::LED_FRAME_TOKENS.hintBrewAcknowledgment.phase.periodMs;
}

inline void SetHintFeedbackPending(GameStateMachineRenderState& state, const bool pending) {
    if (state.hintFeedbackPending != pending && state.hintDisplayRequested &&
        !state.hintMoveValid) {
        state.hintFeedbackElapsed.Reset();
    }
    state.hintFeedbackPending = pending;
}

inline void SetHintThinkingActive(GameStateMachineRenderState& state, const bool active) {
    state.hintThinkingActive = active;
    if (active) {
        state.hintFeedbackPending = true;
    }
}

inline void ClearHintMove(GameStateMachineRenderState& state) {
    ++state.hintPresentationEpoch;
    ClearHintRefinement(state);
    state.hintMoveValid = false;
    state.hintDuckMoveValid = false;
    state.hintFeedbackElapsed = state.hintDisplayRequested && state.hintFeedbackPending
        ? fxe::ElapsedTime::StartNow() : fxe::ElapsedTime{};
}

inline void SetHintMoveSquares(GameStateMachineRenderState& state,
                               const uint8_t fromSquare,
                               const uint8_t toSquare,
                               const bool allowRefinement = true) {
    if (IsHintReplayActive(state)) return;
    AdvanceHintRefinement(state);
    state.hintReplayCurrent = false;
    const bool changed = !state.hintMoveValid || state.hintMoveFromSquare != fromSquare ||
                         state.hintMoveToSquare != toSquare;
    const bool feedbackAllowed = !state.hintMoveValid || state.hintRefinementReady;
    const bool coalescing = state.hintRefinement.elapsed.Started();
    if (changed && allowRefinement && state.hintRefinementReady &&
        state.hintMoveValid && state.hintDisplayRequested) {
        BeginHintRefinement(state);
    }
    state.hintMoveFromSquare = fromSquare;
    state.hintMoveToSquare = toSquare;
    state.hintMoveValid = true;
    RefreshHintRefinementIncoming(state);
    state.hintRefinementReady = allowRefinement && state.hintDisplayRequested;
    if (changed && state.hintDisplayRequested && !coalescing && feedbackAllowed) {
        state.hintFeedbackElapsed.Reset();
    }
}

inline bool TryGetHintMoveSquares(const GameStateMachineRenderState& state,
                                  uint8_t& outFromSquare,
                                  uint8_t& outToSquare) {
    if (!state.hintMoveValid) {
        return false;
    }

    outFromSquare = state.hintMoveFromSquare;
    outToSquare = state.hintMoveToSquare;
    return true;
}

inline void ClearHintDuckMove(GameStateMachineRenderState& state) {
    ClearHintRefinement(state);
    state.hintDuckMoveValid = false;
}

inline void SetHintDuckMoveSquares(GameStateMachineRenderState& state,
                                   const uint8_t fromSquare,
                                   const uint8_t toSquare,
                                   const bool allowRefinement = true) {
    if (IsHintReplayActive(state)) return;
    AdvanceHintRefinement(state);
    const bool changed = !state.hintDuckMoveValid || state.hintDuckMoveFromSquare != fromSquare ||
                         state.hintDuckMoveToSquare != toSquare;
    const bool feedbackAllowed = !state.hintDuckMoveValid || state.hintRefinementReady;
    const bool coalescing = state.hintRefinement.elapsed.Started();
    if (changed && allowRefinement && state.hintRefinementReady && state.hintDuckMoveValid &&
        state.hintMoveValid && state.hintDisplayRequested) {
        BeginHintRefinement(state);
    }
    state.hintDuckMoveFromSquare = fromSquare;
    state.hintDuckMoveToSquare = toSquare;
    state.hintDuckMoveValid = true;
    RefreshHintRefinementIncoming(state);
    if (changed && state.hintDisplayRequested && !coalescing && feedbackAllowed) {
        state.hintFeedbackElapsed.Reset();
    }
}

inline bool TryGetHintDuckMoveSquares(
    const GameStateMachineRenderState& state,
    uint8_t& outFromSquare,
    uint8_t& outToSquare) {
    if (!state.hintDuckMoveValid) {
        return false;
    }
    outFromSquare = state.hintDuckMoveFromSquare;
    outToSquare = state.hintDuckMoveToSquare;
    return true;
}

inline void SetHintMoverIsBlack(GameStateMachineRenderState& state, const bool moverIsBlack) {
    state.hintMoverIsBlack = moverIsBlack;
    state.hintMoverSideKnown = true;
}

inline void ClearHintMoverSide(GameStateMachineRenderState& state) {
    state.hintMoverSideKnown = false;
}

inline bool ConsumeHintCancelRequested(GameStateMachineRenderState& state) {
    const bool value = state.hintCancelRequested;
    state.hintCancelRequested = false;
    return value;
}

} // namespace runtime
