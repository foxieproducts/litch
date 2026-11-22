#include <cstdio>

#include "runtime/interaction_domain_types.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[SETUP_LIFECYCLE] start");

    Check(runtime::ClassifySetupLifecycle(
              modules::GameVariant::Undecided,
              modules::DuckTurnPhase::ChessMove,
              0ULL) == runtime::SetupLifecycle::FreshSetup,
          "an untouched undecided game is fresh setup");
    Check(runtime::ClassifySetupLifecycle(
              modules::GameVariant::Duck,
              modules::DuckTurnPhase::ChessMove,
              0ULL) == runtime::SetupLifecycle::FreshSetup,
          "a latched initial Duck remains fresh setup");
    Check(runtime::ClassifySetupLifecycle(
              modules::GameVariant::Classic,
              modules::DuckTurnPhase::ChessMove,
              0ULL) == runtime::SetupLifecycle::Gameplay,
          "a committed Classic game is gameplay");
    Check(runtime::ClassifySetupLifecycle(
              modules::GameVariant::Undecided,
              modules::DuckTurnPhase::ChessMove,
              1ULL) == runtime::SetupLifecycle::Gameplay,
          "recorded chess movement ends fresh setup");
    Check(runtime::ClassifySetupLifecycle(
              modules::GameVariant::Duck,
              modules::DuckTurnPhase::DuckRelocation,
              0ULL) == runtime::SetupLifecycle::Gameplay,
          "Duck relocation is gameplay, not setup correction");

    if (g_failures == 0) {
        std::puts("[SETUP_LIFECYCLE] ok");
        return 0;
    }

    std::printf("[SETUP_LIFECYCLE] failures=%d\n", g_failures);
    return 1;
}
