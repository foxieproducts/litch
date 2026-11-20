#include "modules/duck_game_persistence.hpp"

#include <cstring>

namespace modules {
namespace {

constexpr uint8_t DURABLE_BYTE_COUNT = 10U;

char HexDigit(const uint8_t value) {
    const uint8_t nibble = static_cast<uint8_t>(value & 0x0FU);
    return static_cast<char>(nibble < 10U ? '0' + nibble
                                          : 'a' + (nibble - 10U));
}

bool DecodeHexDigit(const char value, uint8_t& out) {
    if (value >= '0' && value <= '9') {
        out = static_cast<uint8_t>(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        out = static_cast<uint8_t>(10U + value - 'a');
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        out = static_cast<uint8_t>(10U + value - 'A');
        return true;
    }
    return false;
}

bool EncodeBytes(const uint8_t* bytes,
                 const size_t count,
                 char* out,
                 const size_t outSize) {
    if (bytes == nullptr || out == nullptr || outSize < (count * 2U) + 1U) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i * 2U] = HexDigit(static_cast<uint8_t>(bytes[i] >> 4U));
        out[(i * 2U) + 1U] = HexDigit(bytes[i]);
    }
    out[count * 2U] = '\0';
    return true;
}

bool DecodeBytes(const char* encoded,
                 uint8_t* out,
                 const size_t count) {
    if (encoded == nullptr || out == nullptr ||
        std::strlen(encoded) != count * 2U) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t high = 0U;
        uint8_t low = 0U;
        if (!DecodeHexDigit(encoded[i * 2U], high) ||
            !DecodeHexDigit(encoded[(i * 2U) + 1U], low)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((high << 4U) | low);
    }
    return true;
}

bool IsPromotionSuffixValid(const char suffix) {
    return suffix == '\0' || suffix == 'q' || suffix == 'r' ||
           suffix == 'b' || suffix == 'n';
}

bool IsHistoryPairValid(const uint8_t from, const uint8_t to) {
    const bool absent = from == INVALID_DUCK_SQUARE &&
                        to == INVALID_DUCK_SQUARE;
    const bool present = from < 64U && to < 64U && from != to;
    return absent || present;
}

bool IsDurableStateValid(const DuckGameDurableState& state) {
    if (!IsValidGameVariant(state.variant) ||
        !IsValidVariantOutcome(state.outcome) ||
        !IsPromotionSuffixValid(state.pendingPromotionSuffix)) {
        return false;
    }
    if (state.variant != GameVariant::Duck) {
        return state.duckSquare == INVALID_DUCK_SQUARE &&
               state.phase == DuckTurnPhase::ChessMove &&
               (state.outcome.reason == VariantOutcomeReason::None ||
                (state.variant == GameVariant::Classic &&
                 state.outcome.reason == VariantOutcomeReason::Resigned)) &&
               !state.pendingChessMoveValid;
    }
    if (!IsValidDuckSquare(state.duckSquare)) {
        return false;
    }
    if (state.pendingChessMoveValid) {
        return state.phase == DuckTurnPhase::DuckRelocation &&
               state.pendingMoveFrom < 64U &&
               state.pendingMoveTo < 64U &&
               state.pendingMoveFrom != state.pendingMoveTo &&
               !state.outcome.IsTerminal();
    }
    return state.phase == DuckTurnPhase::ChessMove &&
           state.pendingMoveFrom == INVALID_DUCK_SQUARE &&
           state.pendingMoveTo == INVALID_DUCK_SQUARE &&
           state.pendingPromotionSuffix == '\0';
}

} // namespace

bool EncodeDuckGameDurableState(const DuckGameDurableState& state,
                                char* out,
                                const size_t outSize) {
    if (!IsDurableStateValid(state)) {
        return false;
    }
    const uint8_t bytes[DURABLE_BYTE_COUNT] = {
        DUCK_GAME_DURABLE_VERSION,
        static_cast<uint8_t>(state.variant),
        state.duckSquare,
        static_cast<uint8_t>(state.phase),
        static_cast<uint8_t>(state.outcome.reason),
        static_cast<uint8_t>(state.outcome.winner),
        static_cast<uint8_t>(state.pendingChessMoveValid ? 1U : 0U),
        state.pendingMoveFrom,
        state.pendingMoveTo,
        static_cast<uint8_t>(state.pendingPromotionSuffix),
    };
    return EncodeBytes(bytes, DURABLE_BYTE_COUNT, out, outSize);
}

bool DecodeDuckGameDurableState(const char* encoded,
                                DuckGameDurableState& out) {
    uint8_t bytes[DURABLE_BYTE_COUNT] = {};
    if (!DecodeBytes(encoded, bytes, DURABLE_BYTE_COUNT) ||
        bytes[0] != DUCK_GAME_DURABLE_VERSION || bytes[6] > 1U) {
        return false;
    }
    DuckGameDurableState candidate;
    candidate.variant = static_cast<GameVariant>(bytes[1]);
    candidate.duckSquare = bytes[2];
    candidate.phase = static_cast<DuckTurnPhase>(bytes[3]);
    candidate.outcome.reason = static_cast<VariantOutcomeReason>(bytes[4]);
    candidate.outcome.winner = static_cast<GameWinner>(bytes[5]);
    candidate.pendingChessMoveValid = bytes[6] != 0U;
    candidate.pendingMoveFrom = bytes[7];
    candidate.pendingMoveTo = bytes[8];
    candidate.pendingPromotionSuffix = static_cast<char>(bytes[9]);
    if (!IsDurableStateValid(candidate)) {
        return false;
    }
    out = candidate;
    return true;
}

bool EncodeDuckUndoHistory(const uint8_t* fromSquares,
                           const uint8_t* toSquares,
                           const uint16_t moveCount,
                           char* out,
                           const size_t outSize) {
    if (out == nullptr || outSize < (static_cast<size_t>(moveCount) * 4U) + 1U ||
        (moveCount != 0U && (fromSquares == nullptr || toSquares == nullptr))) {
        return false;
    }
    for (uint16_t i = 0U; i < moveCount; ++i) {
        if (!IsHistoryPairValid(fromSquares[i], toSquares[i])) {
            return false;
        }
        const size_t offset = static_cast<size_t>(i) * 4U;
        out[offset] = HexDigit(static_cast<uint8_t>(fromSquares[i] >> 4U));
        out[offset + 1U] = HexDigit(fromSquares[i]);
        out[offset + 2U] = HexDigit(static_cast<uint8_t>(toSquares[i] >> 4U));
        out[offset + 3U] = HexDigit(toSquares[i]);
    }
    out[static_cast<size_t>(moveCount) * 4U] = '\0';
    return true;
}

bool DecodeDuckUndoHistory(const char* encoded,
                           uint8_t* outFromSquares,
                           uint8_t* outToSquares,
                           const uint16_t capacity,
                           const uint16_t expectedMoveCount) {
    if (encoded == nullptr || outFromSquares == nullptr ||
        outToSquares == nullptr || expectedMoveCount > capacity ||
        std::strlen(encoded) != static_cast<size_t>(expectedMoveCount) * 4U) {
        return false;
    }
    for (uint16_t i = 0U; i < expectedMoveCount; ++i) {
        const size_t offset = static_cast<size_t>(i) * 4U;
        uint8_t nibbles[4] = {};
        for (uint8_t n = 0U; n < 4U; ++n) {
            if (!DecodeHexDigit(encoded[offset + n], nibbles[n])) {
                return false;
            }
        }
        const uint8_t from = static_cast<uint8_t>((nibbles[0] << 4U) | nibbles[1]);
        const uint8_t to = static_cast<uint8_t>((nibbles[2] << 4U) | nibbles[3]);
        if (!IsHistoryPairValid(from, to)) {
            return false;
        }
        outFromSquares[i] = from;
        outToSquares[i] = to;
    }
    return true;
}

} // namespace modules
