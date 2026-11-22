#include <cstdio>

#include "modules/factory_radio_role_gate.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main() {
    std::puts("[FACTORY_RADIO_ROLE_GATE] start");
    modules::FactoryRadioRoleGate election;
    election.Begin(UINT32_MAX - 1000U);
    election.Tick(498U);
    Check(!election.IsVerifier(), "election waits 1500ms across clock wrap");
    election.Tick(499U);
    Check(election.IsVerifier(), "no peer elects verifier at 1500ms across wrap");
    election.Begin(0U);
    election.ObserveEligiblePeer(1400U);
    election.Tick(1500U);
    Check(!election.IsVerifier(), "eligible peer discovery extends search without claiming proof");
    election.AcceptBleProof();
    election.Tick(12000U);
    Check(election.GetRole() == modules::FactoryRadioRoleGate::Role::Complete &&
              !election.CanSearch(),
          "BLE proof completes qualification and cannot be displaced by the no-peer timer");
    election.Begin(12000U);
    election.Tick(13500U);
    Check(election.IsVerifier(), "explicit fresh qualification resumes discovery and may elect verifier");
    Check(!modules::FactoryRadioRoleGate::PeerEligible(10U, 11U, true) &&
          modules::FactoryRadioRoleGate::PeerEligible(11U, 10U, true) &&
          modules::FactoryRadioRoleGate::PeerEligible(10U, 11U, false),
          "simultaneous verifiers descend strictly; subsequent DUT can use any existing verifier");
    for (uint32_t seed = 0U; seed <= 1500U; ++seed) {
        election.Begin(UINT32_MAX - 1000U, seed);
        Check(election.DiscoveryDeadlineMs() == seed, "randomized admission spans every 0..1500ms deadline");
        if (seed) { election.Tick(UINT32_MAX - 1000U + seed - 1U); Check(!election.IsVerifier(), "deadline is not early across wrap"); }
        election.Tick(UINT32_MAX - 1000U + seed); Check(election.IsVerifier(), "no-peer election occurs at the exact randomized deadline");
    }
    election.Stop();
    election.AcceptBleProof();
    Check(!election.IsActive(), "cancelled election ignores late proof");


    if (g_failures == 0) {
        std::puts("[FACTORY_RADIO_ROLE_GATE] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
