#include <array>
#include <cstdio>

#include "modules/led_text_renderer.hpp"

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
    std::puts("[LED_TEXT_RENDERER] start");

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> grid;
    grid.fill(modules::RGB_BLACK);

    // Draw a '1' at (0,0) using the 6x4 font.
    modules::led_text_renderer::DrawTextBlended(grid, "1", 0, 0, modules::Rgb(255, 0, 0), 128);

    const modules::Rgb px = grid[0 * 8 + 1];
    Check(px.r >= 120 && px.r <= 135, "alpha blend roughly half red");
    Check(px.g == 0 && px.b == 0, "alpha blend keeps other channels");

    // Pixels not covered by the glyph should remain unchanged.
    const modules::Rgb untouched = grid[7 * 8 + 7];
    Check(untouched.r == 0 && untouched.g == 0 && untouched.b == 0, "untouched pixel stays black");

    // Full alpha overwrites.
    grid.fill(modules::Rgb(10, 20, 30));
    modules::led_text_renderer::DrawTextBlended(grid, "!", 0, 0, modules::Rgb(100, 0, 0), 255);
    const modules::Rgb px2 = grid[0 * 8 + 1];
    Check(px2.r == 100, "alpha=255 overwrites r");

    if (g_failures == 0) {
        std::puts("[LED_TEXT_RENDERER] ok");
        return 0;
    }

    std::printf("[LED_TEXT_RENDERER] failures=%d\n", g_failures);
    return 1;
}
