#pragma once

// Actual production coordinator with a controlled BLE owner boundary. The
// separate BLE lifecycle suite runs the real GATT callback and SDK cleanup.
#include <cstdio>

#include "modules/ble_service.hpp"
#include "modules/factory_radio_role_gate.hpp"

namespace modules {

struct FactoryRadioFixtureBle {
    FactoryBleDutResult result = FactoryBleDutResult::Pending;
    FactoryBleProof proof{100U, 200U, 300U};
    bool configured = false, allowed = false, endpointFails = false;
    bool candidate = false, running = false, cleanupReady = true;
    bool serverProof = false, wrongSession = false, wrongNonce = false;
    unsigned endpointAttempts = 0U, dutPolls = 0U, serverConsumes = 0U;
    uint32_t lastSession = 0U, lastDutNonce = 0U;
    uint64_t identity = 1U;
    uint64_t advertisedPeer = 0U;

    FactoryBleDutResult PollFactoryBleDut(
        uint32_t sessionId, uint32_t dutNonce, bool lowerAddressOnly, FactoryBleProof& out) {
        ++dutPolls;
        lastSession = sessionId;
        lastDutNonce = dutNonce;
        auto observation = result;
        if (advertisedPeer != 0U) {
            candidate = FactoryRadioRoleGate::PeerEligible(identity, advertisedPeer, lowerAddressOnly);
            if (!candidate) observation = FactoryBleDutResult::NoPeer;
        }
        running = observation == FactoryBleDutResult::Pending || !cleanupReady;
        if (running) return FactoryBleDutResult::Pending;
        if (observation == FactoryBleDutResult::Verified) {
            out = {sessionId + (wrongSession ? 1U : 0U),
                   dutNonce + (wrongNonce ? 1U : 0U), 300U};
        }
        return observation;
    }
    bool HasFactoryBleCandidate() const { return candidate; }
    bool IsFactoryBleDutRunning() const { return running; }
    bool QuiesceFactoryBleDut() {
        if (!cleanupReady) return false;
        running = false;
        return true;
    }
    uint64_t FactoryRadioIdentity() const { return identity; }
    void SetFactoryVerifierConfiguredEnabled(bool enabled) {
        if (configured && !enabled) serverProof = false;
        configured = enabled;
        if (!enabled) allowed = false;
    }
    bool IsFactoryVerifierConfiguredEnabled() const { return configured; }
    bool EnableTemporaryFactoryVerifierEndpoint() {
        ++endpointAttempts;
        if (endpointFails) return false;
        configured = true;
        return true;
    }
    void SetFactoryVerifierRuntimeAllowed(bool enabled) { allowed = configured && enabled; }
    bool ConsumeFactoryBleVerifierProof(FactoryBleProof& out) {
        ++serverConsumes;
        if (!serverProof || !QuiesceFactoryBleDut()) return false;
        serverProof = false;
        out = proof;
        return true;
    }
};

} // namespace modules

#define ARDUINO 1
#define LITCH_HW_VARIANT_S3 1
#define BleService FactoryRadioFixtureBle
#define FactoryRadioCoordinator FactoryRadioFixtureCoordinator
#include "firmware/factory_radio_coordinator.hpp"
#include "../../src/factory_radio_coordinator.cpp"
#undef FactoryRadioCoordinator
#undef BleService
#undef LITCH_HW_VARIANT_S3
#undef ARDUINO
