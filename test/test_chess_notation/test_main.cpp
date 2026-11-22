#include <cstdio>
#include <cstring>

#include <litch_max/chess_notation.hpp>

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckSquareParse(const char* s, const uint8_t expectedSq) {
    uint8_t sq = 0;
    const bool ok = litch_max::TryParseUciSquare(s, sq);
    Check(ok, "litch_max::TryParseUciSquare ok");
    Check(sq == expectedSq, "litch_max::TryParseUciSquare value");
}

void CheckSquareFormat(const uint8_t sq, const char* expected) {
    char buf[3] = {0};
    const bool ok = litch_max::TryFormatUciSquare(sq, buf, sizeof(buf));
    Check(ok, "litch_max::TryFormatUciSquare ok");
    Check(std::strcmp(buf, expected) == 0, "litch_max::TryFormatUciSquare value");
}

} // namespace

int main() {
    std::puts("[CHESS_NOTATION] start");

    // Square mapping: 0=a8 .. 63=h1.
    CheckSquareParse("a8", 0);
    CheckSquareParse("h8", 7);
    CheckSquareParse("a1", 56);
    CheckSquareParse("h1", 63);
    CheckSquareParse("e2", 52);
    CheckSquareParse("e4", 36);

    CheckSquareFormat(0, "a8");
    CheckSquareFormat(7, "h8");
    CheckSquareFormat(56, "a1");
    CheckSquareFormat(63, "h1");
    CheckSquareFormat(52, "e2");
    CheckSquareFormat(36, "e4");

    // Invalid squares.
    {
        uint8_t sq = 0;
        Check(!litch_max::TryParseUciSquare(nullptr, sq), "null square parse rejected");
        Check(!litch_max::TryParseUciSquare("", sq), "empty square parse rejected");
        Check(!litch_max::TryParseUciSquare("i1", sq), "invalid file rejected");
        Check(!litch_max::TryParseUciSquare("a0", sq), "invalid rank rejected");
    }

    // UCI move parsing.
    {
        litch_max::ParsedUciMove mv;
        Check(litch_max::TryParseUciMove("e2e4", mv), "litch_max::TryParseUciMove e2e4");
        Check(mv.fromSquare == 52, "e2e4 from");
        Check(mv.toSquare == 36, "e2e4 to");
        Check(!mv.hasPromotion, "e2e4 no promotion");

        Check(litch_max::TryParseUciMove("e7e8q", mv), "litch_max::TryParseUciMove e7e8q");
        Check(mv.fromSquare == 12, "e7e8q from");
        Check(mv.toSquare == 4, "e7e8q to");
        Check(mv.hasPromotion, "e7e8q has promotion");
        Check(mv.promotionSuffixLower == 'q', "e7e8q promotion suffix");

        Check(litch_max::TryParseUciMove("e7e8Q", mv), "litch_max::TryParseUciMove e7e8Q");
        Check(mv.hasPromotion, "e7e8Q has promotion");
        Check(mv.promotionSuffixLower == 'q', "e7e8Q normalized");

        Check(!litch_max::TryParseUciMove("e2e", mv), "short move rejected");
        Check(!litch_max::TryParseUciMove("e2e4qq", mv), "long move rejected");
        Check(!litch_max::TryParseUciMove("e2e4x", mv), "bad promotion rejected");
    }

    // UCI move formatting.
    {
        litch_max::UciMove uci;
        Check(litch_max::TryFormatUciMove(52, 36, '\0', uci), "litch_max::TryFormatUciMove e2e4 ok");
        Check(std::strcmp(uci.CStr(), "e2e4") == 0, "litch_max::TryFormatUciMove e2e4 value");

        Check(litch_max::TryFormatUciMove(12, 4, 'Q', uci), "litch_max::TryFormatUciMove promo ok");
        Check(std::strcmp(uci.CStr(), "e7e8q") == 0, "litch_max::TryFormatUciMove promo normalized");

        Check(!litch_max::TryFormatUciMove(64, 0, '\0', uci), "format rejects bad from");
        Check(!litch_max::TryFormatUciMove(0, 64, '\0', uci), "format rejects bad to");
        Check(!litch_max::TryFormatUciMove(0, 1, 'x', uci), "format rejects bad promo");
    }

    if (g_failures == 0) {
        std::puts("[CHESS_NOTATION] ok");
        return 0;
    }

    std::printf("[CHESS_NOTATION] failures=%d\n", g_failures);
    return 1;
}
