#include <cstdio>

#include "modules/led_frame_composer.hpp"

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
    std::puts("[LED_FRAME_COMPOSER_OWNER] start");

    using modules::SideToMove;
    using modules::UiOwner;

    // Logical ownership never changes with physical end placement.
    {
        Check(modules::led_frame_composer::OwnerForSideToMove(SideToMove::White, false) == UiOwner::P1,
              "first=P1: white -> P1");
        Check(modules::led_frame_composer::OwnerForSideToMove(SideToMove::Black, false) == UiOwner::P2,
              "first=P1: black -> P2");
    }

    // GAME_FIRST_PLAYER=2 only moves P1 to the opposite physical end.
    {
        Check(modules::led_frame_composer::OwnerForSideToMove(SideToMove::White, true) == UiOwner::P1,
              "opposite end: white remains P1");
        Check(modules::led_frame_composer::OwnerForSideToMove(SideToMove::Black, true) == UiOwner::P2,
              "opposite end: black remains P2");
    }

    if (g_failures == 0) {
        std::puts("[LED_FRAME_COMPOSER_OWNER] ok");
        return 0;
    }

    std::printf("[LED_FRAME_COMPOSER_OWNER] failures=%d\n", g_failures);
    return 1;
}
