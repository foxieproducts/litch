#include "modules/game_persistence_coordinator.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/draw_rules.hpp"

namespace modules {

namespace {

uint8_t CountThreefoldFromFenHistory(const FenString& currentFen,
                                     const game_history::Position* fenRing,
                                     const uint8_t ringStart,
                                     const uint8_t ringSize,
                                     const uint8_t ringCapacity) {
    if (fenRing == nullptr || ringCapacity == 0U || ringSize == 0U) return 0U;
    game_history::Position current;
    if (!game_history::EncodePosition(currentFen.CStr(),
            game_history::Variant::Classic, game_history::INVALID_SQUARE,
            current)) return 0U;
    const uint8_t count = ringSize > ringCapacity ? ringCapacity : ringSize;
    uint8_t repetitions = 0U;
    for (uint8_t i = 0U; i < count; ++i) {
        const uint8_t index = static_cast<uint8_t>((ringStart + i) % ringCapacity);
        const auto& previous = fenRing[index];
        if (game_history::SamePositionAuthority(current, previous)) {
            ++repetitions;
        } else {
            // The journal/cache codec keeps raw EP authority. Only repetition
            // may equate two entries whose sole authority difference is EP.
            auto samePrefix = current;
            samePrefix.bytes[33U] = previous.bytes[33U];
            FenString previousFen;
            uint8_t ignoredDuck = game_history::INVALID_SQUARE;
            if (game_history::SamePositionAuthority(samePrefix, previous) &&
                game_history::DecodePosition(previous, game_history::Variant::Classic,
                                             previousFen, ignoredDuck) &&
                SameClassicRepetitionPosition(currentFen.CStr(), previousFen.CStr())) {
                ++repetitions;
            }
        }
    }
    return repetitions;
}

} // namespace

void GamePersistenceCoordinator::RefreshDrawStateFromTracking() {
    const uint32_t nowMs = fxe::Millis();

    if (IsDuckGame()) {
        m_engineDerived.inCheck = false;
        m_engineDerived.inCheckmate = false;
        m_engineDerived.inStalemate = false;
        m_engineDerived.inDraw = false;
        m_engineDerived.drawReason = DrawReason::None;
        m_engineDerived.threefoldRepetitionCount = 0U;
        m_engineDerived.halfmoveClockPlies = m_halfmoveClockPlies;
        m_engineDerived.threatenedKingSquareValid = false;
        m_engineDerived.threatenedKingSquare = 0U;
        m_engineDerived.threatenedKingMask = 0ULL;
        m_engineDerived.checkmateStartValid = false;
        m_engineDerived.checkmateStartMs = 0U;
        m_engineDerived.UpdateDrawStartTiming(nowMs);
        return;
    }

    const uint8_t repetitionCount = CountThreefoldFromFenHistory(m_cachedFen,
                                                                 m_fenHistory,
                                                                 m_fenHistoryStart,
                                                                 m_fenHistorySize,
                                                                 FEN_HISTORY_CAPACITY);

    m_engineDerived.threefoldRepetitionCount = repetitionCount;
    m_engineDerived.halfmoveClockPlies = m_halfmoveClockPlies;

    DrawReason reason = DrawReason::None;
    if (!m_engineDerived.inCheckmate) {
        if (m_engineDerived.inStalemate) {
            reason = DrawReason::Stalemate;
        } else if (modules::IsInsufficientMaterialDraw(m_cachedFen.CStr())) {
            reason = DrawReason::InsufficientMaterial;
        } else if (repetitionCount >= 3U) {
            reason = DrawReason::ThreefoldRepetition;
        } else if (modules::IsFiftyMoveDraw(m_halfmoveClockPlies)) {
            reason = DrawReason::FiftyMoveRule;
        }
    }

    m_engineDerived.inDraw = reason != DrawReason::None;
    m_engineDerived.drawReason = reason;
    m_engineDerived.UpdateDrawStartTiming(nowMs);
}

} // namespace modules
