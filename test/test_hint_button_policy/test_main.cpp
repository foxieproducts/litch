#include <cstdio>

#include "modules/hint_button_policy.hpp"

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
    std::puts("[HINT_BUTTON_POLICY] start");

    {
        const modules::HintButtonRoutingDecision decision = modules::ResolveHintButtonRouting(
            true, true, modules::UiOwner::P1, modules::UiOwner::P1);
        Check(decision.shouldTrackGesture, "active player Circle is tracked by the hint gesture controller");
        Check(decision.shouldRequestHintOnRelease, "active player Circle tap still toggles hint when beginner mode is off");
    }

    {
        const modules::HintButtonRoutingDecision decision = modules::ResolveHintButtonRouting(
            true, true, modules::UiOwner::P2, modules::UiOwner::P1);
        Check(decision.shouldTrackGesture, "off-turn Circle is still tracked so either player can hold for beginner mode");
        Check(!decision.shouldRequestHintOnRelease, "off-turn Circle tap does not toggle hint");
    }

    {
        const modules::HintButtonRoutingDecision decision = modules::ResolveHintButtonRouting(
            true, true, modules::UiOwner::P1, modules::UiOwner::P1);
        Check(decision.shouldTrackGesture, "active player Circle still tracks hold gestures while beginner mode is enabled");
        Check(decision.shouldRequestHintOnRelease, "active player Circle requests assistance; coordinator chooses Hint or Show Move");
    }

    {
        const modules::HintButtonRoutingDecision decision = modules::ResolveHintButtonRouting(
            false, true, modules::UiOwner::P1, modules::UiOwner::P1);
        Check(!decision.shouldTrackGesture, "required Circle contexts do not route through the optional gesture path");
        Check(!decision.shouldRequestHintOnRelease, "blocked contexts do not toggle hint");
    }

    {
        const auto decision = modules::ResolveHintButtonRouting(
            true, true, modules::UiOwner::P1, modules::UiOwner::P1, false);
        Check(decision.shouldTrackGesture && !decision.shouldRequestHintOnRelease,
              "setup tracks saved preference holds while Hint remains unavailable");
    }

    if (g_failures == 0) {
        std::puts("[HINT_BUTTON_POLICY] ok");
        return 0;
    }

    std::printf("[HINT_BUTTON_POLICY] failures=%d\n", g_failures);
    return 1;
}
