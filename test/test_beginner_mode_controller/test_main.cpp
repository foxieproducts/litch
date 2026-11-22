#include <cstdio>

#include "modules/beginner_mode_controller.hpp"

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
    std::puts("[BEGINNER_MODE_CONTROLLER] start");

    modules::SettingsStore settings;
    modules::BeginnerModeController controller(settings);

    Check(!controller.IsEnabled(modules::UiOwner::P1), "P1 starts disabled");
    Check(!controller.IsEnabled(modules::UiOwner::P2), "P2 starts disabled");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P1, true) == modules::UiOwner::None,
          "disabled P1 does not auto-show on P1 turn");

    settings.Doc()["P1_AUTO_HINTS"] = !controller.IsEnabled(modules::UiOwner::P1);
    Check(controller.IsEnabled(modules::UiOwner::P1), "P1 toggle enables P1");
    Check(!controller.IsEnabled(modules::UiOwner::P2), "P1 toggle leaves P2 unchanged");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P1, true) == modules::UiOwner::P1,
          "enabled P1 auto-shows on P1 turn");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P2, true) == modules::UiOwner::None,
          "enabled P1 does not leak onto P2 turn");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P1, false) == modules::UiOwner::None,
          "CPU turn suppresses beginner mode auto-show");

    settings.Doc()["P2_AUTO_HINTS"] = !controller.IsEnabled(modules::UiOwner::P2);
    Check(controller.IsEnabled(modules::UiOwner::P2), "P2 toggle enables P2");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P2, true) == modules::UiOwner::P2,
          "enabled P2 auto-shows on P2 turn");

    settings.Doc()["P1_AUTO_HINTS"] = !controller.IsEnabled(modules::UiOwner::P1);
    Check(!controller.IsEnabled(modules::UiOwner::P1), "second P1 toggle disables P1");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P1, true) == modules::UiOwner::None,
          "disabled P1 clears its own turn auto-show");
    Check(controller.ActiveOwnerForTurn(modules::UiOwner::P2, true) == modules::UiOwner::P2,
          "P2 remains enabled after P1 disables itself");

    settings.Doc()["P1_AUTO_HINTS"] = false;
    settings.Doc()["P2_AUTO_HINTS"] = false;
    Check(!controller.IsEnabled(modules::UiOwner::P1), "reset clears P1");
    Check(!controller.IsEnabled(modules::UiOwner::P2), "reset clears P2");

    if (g_failures == 0) {
        std::puts("[BEGINNER_MODE_CONTROLLER] ok");
        return 0;
    }

    std::printf("[BEGINNER_MODE_CONTROLLER] failures=%d\n", g_failures);
    return 1;
}
