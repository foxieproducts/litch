#include "modules/game_history_protocol.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace modules::game_history {
namespace {

constexpr uint8_t SIDE_BLACK = 1U << 0U;
constexpr uint8_t CASTLE_WHITE_KING = 1U << 1U;
constexpr uint8_t CASTLE_WHITE_QUEEN = 1U << 2U;
constexpr uint8_t CASTLE_BLACK_KING = 1U << 3U;
constexpr uint8_t CASTLE_BLACK_QUEEN = 1U << 4U;
constexpr uint8_t POSITION_KNOWN_FLAGS = SIDE_BLACK | CASTLE_WHITE_KING |
                                         CASTLE_WHITE_QUEEN |
                                         CASTLE_BLACK_KING |
                                         CASTLE_BLACK_QUEEN;

uint8_t PieceCode(const char piece) {
    switch (piece) {
        case 'P': return 1U;
        case 'N': return 2U;
        case 'B': return 3U;
        case 'R': return 4U;
        case 'Q': return 5U;
        case 'K': return 6U;
        case 'p': return 7U;
        case 'n': return 8U;
        case 'b': return 9U;
        case 'r': return 10U;
        case 'q': return 11U;
        case 'k': return 12U;
        default: return 0xFFU;
    }
}

char PieceToken(const uint8_t code) {
    constexpr char PIECES[] = " PNBRQKpnbrqk";
    return code < sizeof(PIECES) - 1U ? PIECES[code] : '\0';
}

bool ParseUnsigned32(const char*& cursor,
                     const bool allowZero,
                     uint32_t& out) {
    if (cursor == nullptr || !std::isdigit(
            static_cast<unsigned char>(*cursor))) {
        return false;
    }
    uint32_t value = 0U;
    while (std::isdigit(static_cast<unsigned char>(*cursor))) {
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        ++cursor;
    }
    if ((!allowZero && value == 0U) ||
        (*cursor != ' ' && *cursor != '\0')) {
        return false;
    }
    out = value;
    return true;
}

bool ParseSquare(const char* token, uint8_t& out) {
    if (token == nullptr || token[0] < 'a' || token[0] > 'h' ||
        token[1] < '1' || token[1] > '8' || token[2] != '\0') {
        return false;
    }
    const uint8_t file = static_cast<uint8_t>(token[0] - 'a');
    const uint8_t rankFromTop = static_cast<uint8_t>('8' - token[1]);
    out = static_cast<uint8_t>(rankFromTop * 8U + file);
    return true;
}

void WriteLe32(uint8_t* const out, const uint32_t value) {
    for (uint8_t index = 0U; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> static_cast<uint8_t>(index * 8U)) & 0xFFU);
    }
}

uint32_t ReadLe32(const uint8_t* const bytes) {
    uint32_t value = 0U;
    for (uint8_t index = 0U; index < 4U; ++index) {
        value |= static_cast<uint32_t>(bytes[index]) <<
                 static_cast<uint8_t>(index * 8U);
    }
    return value;
}

} // namespace

bool GameId::IsZero() const {
    for (const uint8_t byte : bytes) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

bool IsValidGameId(const GameId& gameId) {
    return !gameId.IsZero();
}

bool IsValidVariant(const Variant variant) {
    return variant == Variant::Classic || variant == Variant::Duck;
}

bool EncodePosition(const char* const fen,
                    const Variant variant,
                    const uint8_t duckSquare,
                    Position& out) {
    if (fen == nullptr || !IsValidVariant(variant) ||
        (variant == Variant::Classic && duckSquare != INVALID_SQUARE) ||
        (variant == Variant::Duck && duckSquare >= 64U)) {
        return false;
    }

    size_t fenLength = 0U;
    while (fenLength < litch_max::FEN_BUFFER_CAPACITY && fen[fenLength] != '\0') {
        ++fenLength;
    }
    if (fenLength == litch_max::FEN_BUFFER_CAPACITY) {
        return false;
    }

    std::array<uint8_t, 64U> squares{};
    uint8_t square = 0U;
    uint8_t rankSeparators = 0U;
    const char* cursor = fen;
    while (*cursor != '\0' && *cursor != ' ') {
        if (*cursor == '/') {
            if (rankSeparators >= 7U ||
                square != static_cast<uint8_t>(
                    (rankSeparators + 1U) * 8U)) {
                return false;
            }
            ++rankSeparators;
            ++cursor;
            continue;
        }
        if (*cursor >= '1' && *cursor <= '8') {
            const uint8_t empty = static_cast<uint8_t>(*cursor - '0');
            if (square > 64U - empty ||
                ((square % 8U) + empty) > 8U) {
                return false;
            }
            square = static_cast<uint8_t>(square + empty);
            ++cursor;
            continue;
        }
        const uint8_t code = PieceCode(*cursor);
        if (code == 0xFFU || square >= 64U) {
            return false;
        }
        squares[square++] = code;
        ++cursor;
    }
    if (square != 64U || rankSeparators != 7U || *cursor != ' ') {
        return false;
    }
    ++cursor;

    uint8_t flags = 0U;
    if (*cursor == 'b') {
        flags |= SIDE_BLACK;
    } else if (*cursor != 'w') {
        return false;
    }
    ++cursor;
    if (*cursor++ != ' ') {
        return false;
    }

    if (*cursor == '-') {
        ++cursor;
    } else {
        if (*cursor == ' ' || *cursor == '\0') {
            return false;
        }
        uint8_t seen = 0U;
        while (*cursor != '\0' && *cursor != ' ') {
            uint8_t flag = 0U;
            switch (*cursor) {
                case 'K': flag = CASTLE_WHITE_KING; break;
                case 'Q': flag = CASTLE_WHITE_QUEEN; break;
                case 'k': flag = CASTLE_BLACK_KING; break;
                case 'q': flag = CASTLE_BLACK_QUEEN; break;
                default: return false;
            }
            if ((seen & flag) != 0U) {
                return false;
            }
            seen |= flag;
            flags |= flag;
            ++cursor;
        }
    }
    if (*cursor++ != ' ') {
        return false;
    }

    uint8_t enPassant = INVALID_SQUARE;
    if (*cursor == '-') {
        ++cursor;
    } else {
        if (*cursor == '\0' || cursor[1] == '\0') {
            return false;
        }
        char token[3] = {cursor[0], cursor[1], '\0'};
        if (!ParseSquare(token, enPassant) ||
            (token[1] != '3' && token[1] != '6')) {
            return false;
        }
        cursor += 2;
    }
    if (*cursor++ != ' ') {
        return false;
    }

    uint32_t halfmove = 0U;
    if (!ParseUnsigned32(cursor, true, halfmove) || *cursor++ != ' ') {
        return false;
    }
    uint32_t fullmove = 0U;
    if (!ParseUnsigned32(cursor, false, fullmove) || *cursor != '\0') {
        return false;
    }

    for (uint8_t index = 0U; index < 32U; ++index) {
        out.bytes[index] = static_cast<uint8_t>(
            squares[index * 2U] |
            static_cast<uint8_t>(squares[index * 2U + 1U] << 4U));
    }
    out.bytes[32] = flags;
    out.bytes[33] = enPassant;
    WriteLe32(out.bytes.data() + 34U, halfmove);
    WriteLe32(out.bytes.data() + 38U, fullmove);
    out.bytes[42] = duckSquare;
    return duckSquare >= 64U || squares[duckSquare] == 0U;
}

bool DecodePosition(const Position& position,
                    const Variant variant,
                    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY>& outFen,
                    uint8_t& outDuckSquare) {
    outFen.Clear();
    outDuckSquare = INVALID_SQUARE;
    if (!IsValidVariant(variant)) {
        return false;
    }
    const uint8_t flags = position.bytes[32];
    const uint8_t enPassant = position.bytes[33];
    const uint32_t fullmove = ReadLe32(position.bytes.data() + 38U);
    const uint8_t duck = position.bytes[42];
    if ((flags & static_cast<uint8_t>(~POSITION_KNOWN_FLAGS)) != 0U ||
        (enPassant != INVALID_SQUARE &&
         (enPassant >= 64U ||
          (enPassant / 8U != 2U && enPassant / 8U != 5U))) ||
        fullmove == 0U ||
        (variant == Variant::Classic && duck != INVALID_SQUARE) ||
        (variant == Variant::Duck && duck >= 64U)) {
        return false;
    }

    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    size_t written = 0U;
    for (uint8_t rank = 0U; rank < 8U; ++rank) {
        uint8_t empty = 0U;
        for (uint8_t file = 0U; file < 8U; ++file) {
            const uint8_t square = static_cast<uint8_t>(rank * 8U + file);
            const uint8_t packed = position.bytes[square / 2U];
            const uint8_t code = (square % 2U) == 0U
                                     ? static_cast<uint8_t>(packed & 0x0FU)
                                     : static_cast<uint8_t>(packed >> 4U);
            if (code == 0U) {
                ++empty;
                continue;
            }
            const char token = PieceToken(code);
            if (token == '\0' || square == duck ||
                written + 2U >= sizeof(fen)) {
                return false;
            }
            if (empty != 0U) {
                fen[written++] = static_cast<char>('0' + empty);
                empty = 0U;
            }
            fen[written++] = token;
        }
        if (empty != 0U) {
            fen[written++] = static_cast<char>('0' + empty);
        }
        if (rank != 7U) {
            fen[written++] = '/';
        }
    }

    const char side = (flags & SIDE_BLACK) != 0U ? 'b' : 'w';
    int suffix = std::snprintf(
        fen + written,
        sizeof(fen) - written,
        " %c ", side);
    if (suffix <= 0 || static_cast<size_t>(suffix) >= sizeof(fen) - written) {
        return false;
    }
    written += static_cast<size_t>(suffix);
    const size_t castleStart = written;
    if ((flags & CASTLE_WHITE_KING) != 0U) fen[written++] = 'K';
    if ((flags & CASTLE_WHITE_QUEEN) != 0U) fen[written++] = 'Q';
    if ((flags & CASTLE_BLACK_KING) != 0U) fen[written++] = 'k';
    if ((flags & CASTLE_BLACK_QUEEN) != 0U) fen[written++] = 'q';
    if (written == castleStart) fen[written++] = '-';
    fen[written++] = ' ';
    if (enPassant == INVALID_SQUARE) {
        fen[written++] = '-';
    } else {
        fen[written++] = static_cast<char>('a' + (enPassant % 8U));
        fen[written++] = static_cast<char>('8' - (enPassant / 8U));
    }
    suffix = std::snprintf(
        fen + written,
        sizeof(fen) - written,
        " %lu %lu",
        static_cast<unsigned long>(ReadLe32(position.bytes.data() + 34U)),
        static_cast<unsigned long>(fullmove));
    if (suffix <= 0 || static_cast<size_t>(suffix) >= sizeof(fen) - written) {
        return false;
    }
    if (!outFen.Assign(fen)) {
        return false;
    }
    outDuckSquare = duck;
    return true;
}

bool SamePositionAuthority(const Position& left, const Position& right) {
    return std::memcmp(left.bytes.data(), right.bytes.data(), 34U) == 0 &&
           left.bytes[42U] == right.bytes[42U];
}

bool AdvancePositionCounters(const Position& before, const Transition& move,
                             Position& after) {
    if (move.from >= 64U || (move.side != Side::White && move.side != Side::Black)) {
        return false;
    }
    const uint8_t packed = before.bytes[move.from / 2U];
    const uint8_t piece = (move.from & 1U) ? packed >> 4U : packed & 0x0FU;
    const auto increment = [](uint32_t value) { return value == UINT32_MAX ? value : value + 1U; };
    const uint32_t halfmove = ReadLe32(before.bytes.data() + 34U);
    const uint32_t fullmove = ReadLe32(before.bytes.data() + 38U);
    WriteLe32(after.bytes.data() + 34U,
              piece == 1U || piece == 7U || move.captureSquare != INVALID_SQUARE
                  ? 0U : increment(halfmove));
    WriteLe32(after.bytes.data() + 38U,
              move.side == Side::Black ? increment(fullmove) : fullmove);
    return true;
}

} // namespace modules::game_history
