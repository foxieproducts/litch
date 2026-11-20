#include <cstdio>

#include "modules/factory_provisioning_policy.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

struct Store {
    modules::FactoryProvisioningState state =
        modules::FactoryProvisioningState::Absent;
    bool writeSucceeds = true;
    uint8_t reads = 0U;
    uint8_t writes = 0U;
};

modules::FactoryProvisioningState Read(void* const context) {
    auto& store = *static_cast<Store*>(context);
    ++store.reads;
    return store.state;
}

bool Write(void* const context,
           const modules::FactoryProvisioningState state) {
    auto& store = *static_cast<Store*>(context);
    ++store.writes;
    if (!store.writeSucceeds) {
        return false;
    }
    store.state = state;
    return true;
}

void CheckRecordCodec() {
    for (const auto state : {
             modules::FactoryProvisioningState::Pending,
             modules::FactoryProvisioningState::Passed,
             modules::FactoryProvisioningState::Override,
             modules::FactoryProvisioningState::Required,
         }) {
        modules::FactoryProvisioningRecord record{};
        Check(modules::EncodeFactoryProvisioningRecord(state, record),
              "supported state encodes");
        Check(modules::DecodeFactoryProvisioningRecord(record) == state,
              "supported state round trips");
        record[4U] ^= 1U;
        Check(modules::DecodeFactoryProvisioningRecord(record) ==
                  modules::FactoryProvisioningState::Invalid,
              "CRC corruption is rejected");
    }
    modules::FactoryProvisioningRecord record{};
    Check(!modules::EncodeFactoryProvisioningRecord(
              modules::FactoryProvisioningState::Absent, record),
          "absence is never persisted as a record");
}

void CheckLegacyBoardsAreExempt() {
    for (const auto revision : {
             hardware::BoardRevision::V1C3,
         }) {
        Store store;
        modules::FactoryProvisioningPolicy policy;
        policy.Begin(revision, false, false, &Read, &Write, &store);
        Check(policy.State() ==
                  modules::FactoryProvisioningState::LegacyExempt,
              "V1 missing state is legacy-exempt");
        Check(!policy.ShouldAutoEnterHardwareTest(),
              "V1 missing state does not auto-enter factory test");
        Check(store.reads == 0U && store.writes == 0U,
              "legacy boards never touch the provisioning store");
    }
}

void CheckFreshAndUpgradePaths() {
    Store fresh;
    modules::FactoryProvisioningPolicy policy;
    policy.Begin(hardware::BoardRevision::V3S3, false, false,
                 &Read, &Write, &fresh);
    Check(fresh.state == modules::FactoryProvisioningState::Pending,
          "fresh V3 records pending durably");
    Check(policy.ShouldAutoEnterHardwareTest(),
          "fresh V3 auto-enters the hardware test");
    Check(policy.AllowsNewBoardIdentity(),
          "durable pending marker permits fresh identity persistence");

    Store upgrade;
    modules::FactoryProvisioningPolicy upgraded;
    upgraded.Begin(hardware::BoardRevision::V3S3, true, false,
                   &Read, &Write, &upgrade);
    Check(upgrade.state == modules::FactoryProvisioningState::Passed,
          "existing V3 identity is promoted once");
    Check(!upgraded.ShouldAutoEnterHardwareTest(),
          "ordinary V3 upgrade does not mimic factory-new");

    Store reset{modules::FactoryProvisioningState::Passed};
    modules::FactoryProvisioningPolicy afterReset;
    afterReset.Begin(hardware::BoardRevision::V3S3, false, false,
                     &Read, &Write, &reset);
    Check(!afterReset.ShouldAutoEnterHardwareTest() && reset.writes == 0U,
          "preserved NVS pass keeps user factory reset out of factory test");
}

void CheckPendingWriteFailureCannotBecomeAnUpgrade() {
    Store store{modules::FactoryProvisioningState::Absent, false};
    modules::FactoryProvisioningPolicy firstBoot;
    firstBoot.Begin(hardware::BoardRevision::V3S3, false, false,
                    &Read, &Write, &store);
    bool identityPersisted = false;
    if (firstBoot.AllowsNewBoardIdentity()) {
        identityPersisted = true;
    }
    Check(!identityPersisted && firstBoot.ShouldAutoEnterHardwareTest(),
          "failed pending write withholds the identity upgrade discriminator");

    store.writeSucceeds = true;
    modules::FactoryProvisioningPolicy secondBoot;
    secondBoot.Begin(hardware::BoardRevision::V3S3, identityPersisted, false,
                     &Read, &Write, &store);
    Check(store.state == modules::FactoryProvisioningState::Pending &&
              secondBoot.ShouldAutoEnterHardwareTest() &&
              secondBoot.AllowsNewBoardIdentity(),
          "next boot establishes pending before identity can become durable");
    identityPersisted = secondBoot.AllowsNewBoardIdentity();

    modules::FactoryProvisioningPolicy thirdBoot;
    thirdBoot.Begin(hardware::BoardRevision::V3S3, identityPersisted, false,
                    &Read, &Write, &store);
    Check(store.state == modules::FactoryProvisioningState::Pending &&
              thirdBoot.ShouldAutoEnterHardwareTest() &&
              !thirdBoot.AllowsCompletedHardwareTestResult(),
          "durable identity cannot promote a board whose pending marker survived reboot");
}

void CheckDurableCompletionAndFailures() {
    Store pending{modules::FactoryProvisioningState::Pending};
    modules::FactoryProvisioningPolicy policy;
    policy.Begin(hardware::BoardRevision::V3S3, false, false,
                 &Read, &Write, &pending);
    Check(!policy.OnDurableHardwareTestRecord(false),
          "wrong-revision record cannot complete provisioning");
    Check(policy.OnDurableHardwareTestRecord(true),
          "durable matching record promotes provisioning");
    Check(policy.AllowsCompletedHardwareTestResult(),
          "hardware-test PASS is released only after NVS promotion");

    Store failed{modules::FactoryProvisioningState::Pending, false};
    modules::FactoryProvisioningPolicy unavailable;
    unavailable.Begin(hardware::BoardRevision::V3S3, false, true,
                      &Read, &Write, &failed);
    Check(unavailable.ShouldAutoEnterHardwareTest(),
          "failed promotion remains in automatic test mode");
    Check(!unavailable.AllowsCompletedHardwareTestResult(),
          "failed promotion suppresses completed result");

    Store invalid{modules::FactoryProvisioningState::Invalid};
    modules::FactoryProvisioningPolicy corrupt;
    corrupt.Begin(hardware::BoardRevision::V3S3, true, false,
                  &Read, &Write, &invalid);
    Check(corrupt.ShouldAutoEnterHardwareTest() && invalid.writes == 0U,
          "invalid NVS fails closed without overwrite");
}

void CheckOverrideRequiredAndPowerCuts() {
    using S = modules::FactoryProvisioningState;
    for (auto revision : {hardware::BoardRevision::V2S3, hardware::BoardRevision::V3S3, hardware::BoardRevision::V3_3S3}) {
        Store store; modules::FactoryProvisioningPolicy p;
        p.Begin(revision, false, false, Read, Write, &store);
        Check(p.ShouldAutoEnterHardwareTest() && store.state == S::Pending, "supported fresh S3 profiles require factory");
        Check(!p.OnDurableHardwareTestRecord(true, true, false), "stale override snapshot is not fresh completion");
        Check(p.OnDurableHardwareTestRecord(true, true, true) && store.state == S::Override && !p.AllowsCompletedHardwareTestResult(), "fresh durable override admits normal boot without PASS");
        modules::FactoryProvisioningPolicy reboot;
        reboot.Begin(revision, true, false, Read, Write, &store);
        Check(!reboot.ShouldAutoEnterHardwareTest() && !reboot.AllowsCompletedHardwareTestResult(), "saved Override survives reboot without becoming verified");
        Check(!reboot.OnDurableHardwareTestRecord(true, false, false) && reboot.State() == S::Override,
              "unrelated stale verified save cannot convert Override into Passed");
        Check(reboot.RequireFactoryTest() && store.state == S::Required, "explicit override Up/clear writes Required before record removal");
        for (bool staleVerified : {false, true}) {
            modules::FactoryProvisioningPolicy interrupted;
            interrupted.Begin(revision, true, staleVerified, Read, Write, &store);
            Check(interrupted.ShouldAutoEnterHardwareTest() && interrupted.State() == S::Required, "power cut after Required dominates old override or old verified settings");
            Check(!interrupted.OnDurableHardwareTestRecord(true, true, false) &&
                  !interrupted.OnDurableHardwareTestRecord(true, false, false), "unrelated/stale save cannot supersede Required");
            Check(interrupted.OnDurableHardwareTestRecord(true, false, true), "exact fresh result can promote Required to Passed");
            Check(interrupted.RequireFactoryTest(), "explicit clear may invalidate a verified pass");
        }
        store.writeSucceeds = false;
        Check(!reboot.RequireFactoryTest() && reboot.State() == S::Unavailable && reboot.ShouldAutoEnterHardwareTest(), "failed Required write reports unavailable and never claims durable re-entry");
        store.writeSucceeds = true; store.state = S::Passed;
        modules::FactoryProvisioningPolicy saved;
        saved.Begin(revision, true, true, Read, Write, &store);
        const auto writesBeforeUp = store.writes;
        Check(saved.State() == S::Passed && !saved.ShouldAutoEnterHardwareTest() && saved.AllowsPhysicalUpEntry(),
              "saved PASS boots normally but permits explicit physical Up retest");
        Check(store.state == S::Passed && store.writes == writesBeforeUp,
              "physical Up admission preserves historical PASS without writing provisioning");
    }
}

} // namespace

int main() {
    std::puts("[FACTORY_PROVISIONING_POLICY] start");
    CheckRecordCodec();
    CheckOverrideRequiredAndPowerCuts();
    CheckLegacyBoardsAreExempt();
    CheckFreshAndUpgradePaths();
    CheckPendingWriteFailureCannotBecomeAnUpgrade();
    CheckDurableCompletionAndFailures();
    if (g_failures == 0) {
        std::puts("[FACTORY_PROVISIONING_POLICY] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
