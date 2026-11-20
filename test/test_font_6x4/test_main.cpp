#include <cstdio>

#include "modules/font_6x4.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[FONT_6X4] start");

    const modules::Glyph6x4 space = modules::font_6x4::GetGlyph(' ');
    for (int i = 0; i < 6; ++i) {
        Check(space.rows[i] == 0, "space is blank");
    }

    const modules::Glyph6x4 a = modules::font_6x4::GetGlyph('A');
    for (int i = 0; i < 6; ++i) {
        Check((a.rows[i] & 0xF0) == 0, "glyph row uses only 4 bits");
    }

    const modules::Glyph6x4 unknown = modules::font_6x4::GetGlyph('\x01');
    const modules::Glyph6x4 question = modules::font_6x4::GetGlyph('?');
    bool equal = true;
    for (int i = 0; i < 6; ++i) {
        if (unknown.rows[i] != question.rows[i]) {
            equal = false;
        }
    }
    Check(equal, "unsupported chars fall back to ?");

    if (g_failures == 0) {
        std::puts("[FONT_6X4] ok");
        return 0;
    }

    std::printf("[FONT_6X4] failures=%d\n", g_failures);
    return 1;
}
