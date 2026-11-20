#include "modules/game_persistence_coordinator.hpp"

#include <cctype>
#include <charconv>

#include "logger.hpp"
#include "modules/game_persistence_engine_helpers.hpp"

namespace modules {

void GamePersistenceCoordinator::CopyFenWithCommittedFullmove(
    const FenString& fen, const uint16_t retainedPlies, FenString& out) const {
    const auto base = m_undoBaseFen.View();
    const size_t turn = base.find(' ');
    const size_t number = base.rfind(' ');
    const size_t suffix = fen.View().rfind(' ');
    if (turn == std::string_view::npos || turn + 2U >= base.size() ||
        (base[turn + 1U] != 'w' && base[turn + 1U] != 'b') ||
        number <= turn || suffix == std::string_view::npos) {
        out = fen;
        return;
    }
    uint32_t fullmove = 0U;
    const char* const end = base.data() + base.size();
    const auto parsed = std::from_chars(base.data() + number + 1U, end, fullmove);
    if (parsed.ec != std::errc{} || parsed.ptr != end || fullmove == 0U) {
        out = fen;
        return;
    }

    const bool blackAtBase = base[turn + 1U] == 'b';
    const uint16_t retained = GetUndoHistoryMoveCount();
    const auto& history = m_gameHistoryPersistence;
    if (history.valid && history.present && history.currentPly > retained) {
        const unsigned prefix = history.currentPly - retained;
        if (!history.captureIncomplete) {
            // Older saves normalized a rolled base to move 1. A complete game
            // retains its absolute ply, including when the opening was Black.
            fullmove = 1U + (prefix + (blackAtBase ? 0U : 1U)) / 2U;
        } else if (fullmove == 1U && (prefix > 2U || history.IsCaptureExhausted())) {
            // A partial capture starts at sentinel ply 1. After at least two
            // dropped plies, a base still saying move 1 has lost its number.
            out = fen;
            return;
        }
    }
    const uint32_t blackMoves = (retainedPlies + (blackAtBase ? 1U : 0U)) / 2U;
    fullmove = blackMoves > UINT32_MAX - fullmove ? UINT32_MAX : fullmove + blackMoves;
    char digits[10];
    const auto formatted = std::to_chars(digits, digits + sizeof(digits), fullmove);
    if (!out.Assign(fen.View().substr(0U, suffix + 1U)) ||
        !out.Append(std::string_view(digits, formatted.ptr - digits))) {
        out = fen;
    }
}

const char* GamePersistenceCoordinator::GetHomePresentationFen() const {
    const auto& fen = m_clockTerminalFen.Empty() ? m_cachedFen : m_clockTerminalFen;
    CopyFenWithCommittedFullmove(fen, GetUndoHistoryMoveCount(), m_homePresentationFen);
    return m_homePresentationFen.CStr();
}

bool GamePersistenceCoordinator::ReadFenHistory(const uint8_t index, FenString& out) const {
    if (index >= FEN_HISTORY_CAPACITY) return false;
    uint8_t ignoredDuck = game_history::INVALID_SQUARE;
    FenString decoded;
    if (!game_history::DecodePosition(m_fenHistory[index],
            game_history::Variant::Classic, decoded, ignoredDuck)) return false;
    out = decoded;
    return true;
}

void GamePersistenceCoordinator::ClearFenHistory() {
    m_fenHistoryStart = 0;
    m_fenHistorySize = 0;
    for (auto& s : m_fenHistory) {
        s = {};
    }
    for (auto& v : m_halfmoveHistoryPlies) {
        v = 0;
    }
    m_cancelUndoFenValid = false;
    m_cancelUndoFen.Clear();

    m_cancelUndoHalfmoveValid = false;
    m_cancelUndoHalfmovePlies = 0;
}

void GamePersistenceCoordinator::SeedFenHistoryFromCurrentFen() {
    ClearFenHistory();
    if (!m_cachedFen.Empty() && !PushFenHistory(m_cachedFen.CStr(), m_halfmoveClockPlies))
        logger::Warn("engine: invalid FEN during history seed");
}

bool GamePersistenceCoordinator::PushFenHistory(const char* fen, const uint16_t halfmoveClockPlies) {
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }

    game_history::Position tmp;
    if (!game_history::EncodePosition(fen, game_history::Variant::Classic,
                                      game_history::INVALID_SQUARE, tmp)) {
        return false;
    }

    // Any new engine-visible move invalidates cancel-undo.
    m_cancelUndoFenValid = false;
    m_cancelUndoFen.Clear();

    if (m_fenHistorySize == 0) {
        m_fenHistoryStart = 0;
        m_fenHistory[0] = tmp;
        m_halfmoveHistoryPlies[0] = halfmoveClockPlies;
        m_fenHistorySize = 1;
        return true;
    }

    if (m_fenHistorySize < FEN_HISTORY_CAPACITY) {
        const uint8_t idx =
            static_cast<uint8_t>((m_fenHistoryStart + m_fenHistorySize) % FEN_HISTORY_CAPACITY);
        m_fenHistory[idx] = tmp;
        m_halfmoveHistoryPlies[idx] = halfmoveClockPlies;
        ++m_fenHistorySize;
        return true;
    }

    // Full: drop oldest and append at the freed slot.
    m_fenHistoryStart = static_cast<uint8_t>((m_fenHistoryStart + 1U) % FEN_HISTORY_CAPACITY);
    const uint8_t idx =
        static_cast<uint8_t>((m_fenHistoryStart + (m_fenHistorySize - 1U)) % FEN_HISTORY_CAPACITY);
    m_fenHistory[idx] = tmp;
    m_halfmoveHistoryPlies[idx] = halfmoveClockPlies;
    return true;
}

bool GamePersistenceCoordinator::RebuildFenHistoryFromUndoState() {
    if (m_cachedFen.Empty()) {
        return false;
    }

    if (m_undoHistoryFieldCount == 0U) {
        SeedFenHistoryFromCurrentFen();
        return true;
    }

    if (m_undoBaseFen.Empty()) {
        SeedFenHistoryFromCurrentFen();
        return false;
    }

    ClearFenHistory();

    if (!m_engine.SetFen(m_undoBaseFen.CStr())) {
        logger::Warn("engine: SetFen(base) failed during fen-history rebuild");
        (void)m_engine.SetFen(m_cachedFen.CStr());
        SeedFenHistoryFromCurrentFen();
        return false;
    }

    FenString rebuiltFen;
    if (IsDuckGame()) {
        rebuiltFen = m_undoBaseFen;
    } else if (!game_persistence_engine_helpers::SafeGetFen(
                   m_engine, rebuiltFen, false) ||
               rebuiltFen.Empty()) {
        rebuiltFen = m_undoBaseFen;
    }
    uint8_t replayDuckSquare = INVALID_DUCK_SQUARE;
    if (!InitializeUndoHistoryReplayDuck(replayDuckSquare)) {
        (void)m_engine.SetFen(m_cachedFen.CStr());
        SeedFenHistoryFromCurrentFen();
        return false;
    }

    uint16_t runningHalfmoveClockPlies = 0;
    if (!PushFenHistory(rebuiltFen.CStr(), runningHalfmoveClockPlies)) {
        (void)m_engine.SetFen(m_cachedFen.CStr());
        SeedFenHistoryFromCurrentFen();
        return false;
    }

    for (uint16_t i = 0;
         (i + UNDO_HISTORY_FIELDS_PER_MOVE) <= m_undoHistoryFieldCount;
         i = static_cast<uint16_t>(i + UNDO_HISTORY_FIELDS_PER_MOVE)) {
        const char movedPiece = static_cast<char>(m_undoHistoryFields[static_cast<uint16_t>(i + 1)]);
        const char capturedPiece = static_cast<char>(m_undoHistoryFields[static_cast<uint16_t>(i + 2)]);
        if (!ApplyUndoHistoryReplayMove(
                i, rebuiltFen, replayDuckSquare)) {
            logger::Warn("engine: failed replay during fen-history rebuild");
            (void)m_engine.SetFen(m_cachedFen.CStr());
            SeedFenHistoryFromCurrentFen();
            return false;
        }

        const char movedLower = static_cast<char>(std::tolower(static_cast<unsigned char>(movedPiece)));
        const bool pawnMove = (movedLower == 'p');
        const bool capture = (capturedPiece != 0);
        if (pawnMove || capture) {
            runningHalfmoveClockPlies = 0;
        } else if (runningHalfmoveClockPlies != 0xFFFFU) {
            ++runningHalfmoveClockPlies;
        }

        if (rebuiltFen.Empty() || !PushFenHistory(
                rebuiltFen.CStr(), runningHalfmoveClockPlies)) {
            logger::Warn("engine: failed snapshot during fen-history rebuild");
            (void)m_engine.SetFen(m_cachedFen.CStr());
            SeedFenHistoryFromCurrentFen();
            return false;
        }
    }

    const uint8_t lastIdx =
        static_cast<uint8_t>((m_fenHistoryStart + (m_fenHistorySize - 1U)) % FEN_HISTORY_CAPACITY);
    if (!ReadFenHistory(lastIdx, m_cachedFen)) return false;
    m_halfmoveClockPlies = m_halfmoveHistoryPlies[lastIdx];
    return true;
}

bool GamePersistenceCoordinator::TryUndoUsingFenHistory() {
    if (m_fenHistorySize < 2U) {
        return false;
    }

    const uint8_t curIdx =
        static_cast<uint8_t>((m_fenHistoryStart + (m_fenHistorySize - 1U)) % FEN_HISTORY_CAPACITY);
    FenString previous;
    const uint8_t prevIdx = static_cast<uint8_t>(
        (m_fenHistoryStart + m_fenHistorySize - 2U) % FEN_HISTORY_CAPACITY);
    if (!ReadFenHistory(prevIdx, previous) ||
        !ReadFenHistory(curIdx, m_cancelUndoFen)) return false;
    m_cancelUndoFenValid = !m_cancelUndoFen.Empty();

    m_cancelUndoHalfmovePlies = m_halfmoveHistoryPlies[curIdx];
    m_cancelUndoHalfmoveValid = true;

    if (!m_engine.SetFen(previous.CStr())) {
        return false;
    }
    --m_fenHistorySize;
    m_cachedFen = previous;
    m_halfmoveClockPlies = m_halfmoveHistoryPlies[prevIdx];
    return true;
}

bool GamePersistenceCoordinator::TryCancelUndoUsingFenHistory() {
    if (!m_cancelUndoFenValid || m_cancelUndoFen.Empty() || !m_cancelUndoHalfmoveValid) {
        return false;
    }

    if (!m_engine.SetFen(m_cancelUndoFen.CStr())) {
        return false;
    }
    m_cachedFen = m_cancelUndoFen;
    m_halfmoveClockPlies = m_cancelUndoHalfmovePlies;

    (void)PushFenHistory(m_cancelUndoFen.CStr(), m_halfmoveClockPlies);
    m_cancelUndoFenValid = false;
    m_cancelUndoFen.Clear();

    m_cancelUndoHalfmoveValid = false;
    m_cancelUndoHalfmovePlies = 0;
    return true;
}

} // namespace modules
