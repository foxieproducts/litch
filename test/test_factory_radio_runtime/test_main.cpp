#include "factory_radio_runtime_fixture.hpp"

namespace {
int failures = 0;
void Check(const bool ok, const char* why) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); }
}
using Stage = modules::ManufacturingTestStage;
using Result = modules::FactoryBleDutResult;

void DutProofJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    radio.Update(true, true, Stage::Radio, ble, 0U, 1U);
    Check(!radio.ConsumeDutBleVerified() && !radio.ConsumeVerifierBleVerified(),
          "discovery and role hints do not invent either BLE proof");
    ble.candidate = true;
    ble.result = Result::Verified;
    ble.cleanupReady = false;
    radio.Update(true, true, Stage::Radio, ble, 10U, 1U);
    Check(!radio.ConsumeDutBleVerified(),
          "matching exchange remains pending while the BLE owner retains SDK resources");
    ble.cleanupReady = true;
    radio.Update(true, true, Stage::Radio, ble, 11U, 1U);
    Check(radio.ConsumeDutBleVerified() && !radio.ConsumeDutBleVerified(),
          "matching cleaned BLE exchange alone qualifies DUT exactly once");
    Check(!ble.configured && !ble.allowed && !radio.HasPendingTerminalReply(),
          "completed client does not retain temporary verifier eligibility or server grace");
    const unsigned polls = ble.dutPolls;
    radio.Update(true, true, Stage::Persisting, ble, 2000U, 1U);
    Check(ble.dutPolls == polls && !radio.ConsumeDutBleVerified(),
          "completed DUT does not allocate another proof attempt while persistence runs");
}

void FreshProofRetryJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    ble.result = Result::Verified;
    ble.wrongSession = true;
    radio.Update(true, true, Stage::Radio, ble, 0U, 2U);
    const auto previousSession = ble.lastSession;
    const auto previousNonce = ble.lastDutNonce;
    Check(!radio.ConsumeDutBleVerified(), "stale session returned by the BLE seam cannot qualify");
    radio.Update(true, true, Stage::Radio, ble, 1003U, 2U);
    Check(ble.dutPolls == 1U, "failed proof retains address-based bounded retry deadline");
    ble.wrongSession = false;
    ble.wrongNonce = true;
    radio.Update(true, true, Stage::Radio, ble, 1004U, 2U);
    Check(!radio.ConsumeDutBleVerified() && ble.lastSession != previousSession &&
          ble.lastDutNonce != previousNonce, "retry uses fresh session and DUT nonce and rejects a stale nonce");
    ble.wrongNonce = false;
    radio.Update(true, true, Stage::Radio, ble, 2004U, 2U);
    Check(radio.ConsumeDutBleVerified(), "bounded retry recovers with the exact fresh exchange");
}

void LateHigherAddressPeerJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    ble.result = Result::NoPeer;
    radio.Update(true, true, Stage::Radio, ble, 0U, 4U);
    Check(ble.dutPolls == 1U && !radio.IsAutomaticVerifier() && !radio.ConsumeDutBleVerified(),
          "the initial unrestricted scan has no advertisement and cannot qualify");

    ble.advertisedPeer = 2U; // The inactive saved verifier only advertises and never searches outbound.
    ble.result = Result::Verified;
    for (uint32_t nowMs : {2000U, 4000U}) {
        radio.Update(true, true, Stage::Radio, ble, nowMs, 4U);
        Check(radio.IsAutomaticVerifier() && ble.configured && ble.allowed && !radio.ConsumeDutBleVerified(),
              "ordinary elected attempts keep the downward routing and serving role");
    }
    ble.cleanupReady = false;
    radio.Update(true, true, Stage::Radio, ble, 6000U, 4U);
    Check(ble.candidate && radio.IsAutomaticVerifier() && !ble.allowed && !radio.ConsumeDutBleVerified(),
          "the next attempt discovers the later higher-address verifier and fences inbound proof during cleanup");
    ble.cleanupReady = true;
    radio.Update(true, true, Stage::Radio, ble, 6001U, 4U);
    Check(radio.ConsumeDutBleVerified() && !radio.ConsumeDutBleVerified() && !ble.configured,
          "an already-elected DUT escapes the lower-address miss through one cleaned BLE proof");
}

void SimultaneousElectedPeerJourney() {
    firmware::FactoryRadioFixtureCoordinator lower, higher;
    modules::FactoryRadioFixtureBle lowerBle, higherBle;
    higherBle.identity = 2U;
    lowerBle.result = higherBle.result = Result::NoPeer;
    for (uint32_t nowMs : {0U, 2000U}) {
        lower.Update(true, true, Stage::Radio, lowerBle, nowMs, 5U);
        higher.Update(true, true, Stage::Radio, higherBle, nowMs, 6U);
    }
    Check(lower.IsAutomaticVerifier() && higher.IsAutomaticVerifier(),
          "two locally ready boards can both elect responders before seeing an advertisement");
    lowerBle.advertisedPeer = higherBle.identity;
    higherBle.advertisedPeer = lowerBle.identity;
    lowerBle.result = higherBle.result = Result::Verified;
    lower.Update(true, true, Stage::Radio, lowerBle, 4000U, 5U);
    higher.Update(true, true, Stage::Radio, higherBle, 4000U, 6U);
    Check(!lower.ConsumeDutBleVerified() && lowerBle.allowed && higher.ConsumeDutBleVerified(),
          "in normal elected attempts only the higher board initiates toward the lower responder");
    lowerBle.serverProof = true;
    lower.Update(true, true, Stage::Radio, lowerBle, 4001U, 5U);
    Check(lower.ConsumeVerifierBleVerified() && !lower.ConsumeDutBleVerified() && lower.HasPendingTerminalReply(),
          "the initial responder retains its exact independent proof and terminal grace");
}

void ServerTerminalReplyJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    ble.configured = true; // Explicit inactive saved-verifier service.
    radio.Update(false, false, Stage::Inactive, ble, 100U);
    Check(ble.allowed && !radio.ConsumeVerifierBleVerified(),
          "explicit saved-verifier eligibility admits traffic without manufacturing proof");
    ble.serverProof = true;
    ble.cleanupReady = false;
    radio.Update(false, false, Stage::Inactive, ble, 101U);
    Check(ble.serverProof && !radio.ConsumeVerifierBleVerified(),
          "real callback proof slot stays queued until BLE client/task cleanup finishes");
    ble.cleanupReady = true;
    radio.Update(false, false, Stage::Inactive, ble, 102U);
    Check(radio.ConsumeVerifierBleVerified() && !radio.ConsumeVerifierBleVerified() &&
          radio.IsVerifierBleProofActive() && radio.HasPendingTerminalReply() && !ble.allowed,
          "cleaned server proof qualifies once and fences writes for terminal READ grace");
    const unsigned consumes = ble.serverConsumes;
    radio.Update(false, false, Stage::Inactive, ble, 1101U);
    Check(radio.HasPendingTerminalReply() && ble.serverConsumes == consumes,
          "server retains the full one-second terminal grace without consuming another proof");
    radio.PollTerminalReply(ble, 1102U);
    Check(!radio.HasPendingTerminalReply(), "terminal grace ends at its exact deadline");
    radio.Update(false, false, Stage::Inactive, ble, 1103U);
    Check(ble.allowed, "explicit saved verifier accepts later boards after terminal grace");
}

void LocalReinspectionJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    radio.Update(true, true, Stage::Radio, ble, 0U, 9U);
    radio.Update(true, true, Stage::Radio, ble, 1501U, 9U);
    Check(radio.IsAutomaticVerifier() && ble.configured,
          "locally ready unit may elect a temporary verifier");
    ble.serverProof = true;
    ble.cleanupReady = false;
    const unsigned consumes = ble.serverConsumes;
    radio.Update(true, false, Stage::Local, ble, 1502U, 9U);
    Check(!ble.configured && !ble.allowed && !ble.serverProof &&
          ble.serverConsumes == consumes && !radio.ConsumeVerifierBleVerified(),
          "local reinspection revokes temporary routing and callback proof during pending cleanup");
    ble.cleanupReady = true;
    radio.Update(true, false, Stage::Local, ble, 1503U, 9U);
    Check(!radio.IsAutomaticVerifier() && !ble.allowed &&
          ble.serverConsumes == consumes, "actual active local run never falls into inactive saved-verifier admission");
    radio.Update(true, true, Stage::Radio, ble, 1504U, 9U);
    Check(!radio.IsAutomaticVerifier() && ble.dutPolls > 1U,
          "leaving reinspection restarts fresh bounded discovery for the same real run");
    radio.Update(false, false, Stage::Inactive, ble, 1505U, 9U);
    Check(!ble.configured && !ble.allowed, "stopping a temporary run cannot turn it into saved service");
}

void EndpointRecoveryJourney() {
    firmware::FactoryRadioFixtureCoordinator radio;
    modules::FactoryRadioFixtureBle ble;
    ble.endpointFails = true;
    radio.Update(true, true, Stage::Radio, ble, 0U, 3U);
    radio.Update(true, true, Stage::Radio, ble, 1501U, 3U);
    const unsigned attempts = ble.endpointAttempts;
    Check(radio.BleEndpointStartFailed() && attempts == 1U,
          "endpoint failure is visible without manufacturing proof");
    radio.Update(true, true, Stage::Radio, ble, 2500U, 3U);
    Check(ble.endpointAttempts == attempts, "endpoint failures retry no faster than once per second");
    ble.endpointFails = false;
    radio.Update(true, true, Stage::Radio, ble, 2501U, 3U);
    Check(ble.endpointAttempts == attempts + 1U && !radio.BleEndpointStartFailed(),
          "endpoint initialization recovers at its bounded retry deadline");
}
} // namespace

int main() {
    DutProofJourney();
    FreshProofRetryJourney();
    LateHigherAddressPeerJourney();
    SimultaneousElectedPeerJourney();
    ServerTerminalReplyJourney();
    LocalReinspectionJourney();
    EndpointRecoveryJourney();
    std::printf("[FACTORY_RADIO_RUNTIME] BLE-only proof, cleanup, terminal grace, reinspection; RF/SDK controlled; failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
