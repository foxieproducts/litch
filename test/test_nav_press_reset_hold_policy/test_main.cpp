#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_reset_hold_policy.hpp"

namespace {

modules::GameResetHoldContext MakeBaseContext() {
    return modules::GameResetHoldContext{};
}

} // namespace

int main() {
    fxe::test_support::TestResults results("GAME_RESET_HOLD_POLICY");
    results.Start();

    {
        const auto ctx = MakeBaseContext();
        results.Check(modules::IsGameResetHoldEligible(ctx),
                      "idle non-modal context allows game reset hold");
    }

    {
        auto ctx = MakeBaseContext();
        ctx.quickSettingsOverlayActive = true;
        results.Check(!modules::IsGameResetHoldEligible(ctx),
                      "quick settings overlay blocks game reset hold");
    }

    {
        auto ctx = MakeBaseContext();
        ctx.menuOverlayActive = true;
        results.Check(!modules::IsGameResetHoldEligible(ctx),
                      "text or menu overlay blocks game reset hold");
    }

    {
        auto ctx = MakeBaseContext();
        ctx.textOverlayActive = true;
        results.Check(!modules::IsGameResetHoldEligible(ctx),
                      "state-machine text overlay blocks game reset hold");
    }

    {
        auto ctx = MakeBaseContext();
        ctx.promotionActive = true;
        results.Check(!modules::IsGameResetHoldEligible(ctx),
                      "promotion blocks game reset hold");
    }

    return results.Finish();
}
