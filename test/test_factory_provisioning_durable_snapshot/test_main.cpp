#include <cstdio>
#include <string>
#include "modules/hardware_test_record.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/hardware_test_boot_entry.hpp"
#include "modules/board_identity_persistence.hpp"
#include "foxie_embedded/millis.hpp"
#include "nvs.h"
void FactoryProvisioningDelay(unsigned){}
#define ARDUINO 1
#define FactoryProvisioning SnapshotFixtureProvisioning
#define vTaskDelay FactoryProvisioningDelay
#define pdMS_TO_TICKS(x) (x)
#include "../../src/firmware_factory_provisioning.cpp"
#undef pdMS_TO_TICKS
#undef vTaskDelay
#undef FactoryProvisioning
#undef ARDUINO
namespace {
int failures=0;
void Check(bool ok,const char* msg){if(!ok){++failures;std::printf("[FAIL] %s\n",msg);}}
}
int main(){
    using S=modules::FactoryProvisioningState;using R=modules::ManufacturingTestRadioResult;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);fxe::ManualMillis::SetNowMs(0U);
    for (const bool unknown : {true, false}) {
        const auto runtime = hardware::RuntimeProfileForS3Sample({false, unknown, false});
        factory_provisioning_sdk::Reset();
        hardware::RmtFlashArbiter arbiter;
        DynamicJsonDocument doc(512U);
        firmware::SnapshotFixtureProvisioning fresh;
        // SetupApp itself is not executed here. This uses its detected identity
        // boundary with the actual provisioning/NVS owner and raw strap decode.
        fresh.Begin(runtime.detectedRevision, doc, arbiter);
        Check(runtime.resolved.revision == hardware::BoardRevision::V2S3 &&
                  fresh.ShouldAutoEnterHardwareTest() == !unknown &&
                  fresh.AllowsPhysicalUpEntry() == !unknown &&
                  hardware::ShouldEnterHardwareTestFromBoot(runtime.detectedRevision,
                      runtime.resolved.hasP2Controls, true, true) == !unknown,
              "raw Unknown keeps V2 fallback without factory boot/Up; known V2 enters");
        Check(factory_provisioning_sdk::writes == (unknown ? 0U : 1U),
              "fresh Unknown writes no factory NVS; known V2 writes Pending");
        modules::BoardIdentity identity; identity.bytes.fill(0x41U);
        Check(modules::WriteBoardIdentity(doc, identity), "seed installed identity for boot migration");
        factory_provisioning_sdk::Reset(); // Installed identity with absent factory NVS.
        firmware::SnapshotFixtureProvisioning installed;
        installed.Begin(runtime.detectedRevision, doc, arbiter);
        modules::HardwareTestRecord good{1U, hardware::BoardRevision::V2S3,
            R::Verified, true, 3U, 1U, 2U, 3U};
        Check(modules::WriteHardwareTestRecord(doc, good), "seed eligible V2 record on resolved profile");
        installed.ExpectFactoryResult(good);
        installed.ExpectedReceiptPublication()->store(21U, std::memory_order_release);
        installed.OnDurableSettingsSnapshot(doc, 21U);
        Check(factory_provisioning_sdk::writes == (unknown ? 0U : 1U) &&
                  (unknown ? !factory_provisioning_sdk::exists : installed.State() == S::Passed),
              "Unknown installed identity and durable V2 result cannot promote factory NVS");
        std::printf("[DETECTED_FACTORY_PROVISIONING] raw=%u detected=%u resolved=%u auto=%u up=%u nvs_writes=%u\n",
                    unknown ? 2U : 0U, unsigned(runtime.detectedRevision), unsigned(runtime.resolved.revision),
                    fresh.ShouldAutoEnterHardwareTest(), fresh.AllowsPhysicalUpEntry(), factory_provisioning_sdk::writes);
    }
    for(auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3}){
        factory_provisioning_sdk::Reset();hardware::RmtFlashArbiter arbiter;DynamicJsonDocument doc(512U);
        firmware::SnapshotFixtureProvisioning p;p.Begin(revision,doc,arbiter);
        Check(p.State()==S::Pending&&p.ShouldAutoEnterHardwareTest(),"fresh supported S3 provisioning wrapper writes Pending");
        modules::HardwareTestRecord overrideRecord{1U,revision,R::Override,false,0U,0U,5U,0U};
        Check(modules::WriteHardwareTestRecord(doc,overrideRecord),"Override CRC record encodes");
        p.OnDurableSettingsSnapshot(doc);Check(p.State()==S::Pending,"ordinary old override settings cannot promote provisioning");
        p.ExpectFactoryResult(overrideRecord);
        p.ExpectedReceiptPublication()->store(17U, std::memory_order_release);
        p.OnDurableSettingsSnapshot(doc, 16U);
        Check(p.State()==S::Pending,"identical Override record from an older receipt cannot promote provisioning");
        p.OnDurableSettingsSnapshot(doc, 17U);p.ForgetExpectedResult();
        Check(p.State()==S::Override&&!p.AllowsCompletedHardwareTestResult(),"exact submitted durable Override creates normal-boot exemption without PASS");
        firmware::SnapshotFixtureProvisioning reboot;reboot.Begin(revision,doc,arbiter);
        Check(!reboot.ShouldAutoEnterHardwareTest()&&reboot.State()==S::Override&&reboot.AllowsPhysicalUpEntry(),"Override starts normally and permits explicit Up re-entry");
        Check(reboot.RequireFactoryTest(),"Required NVS is committed before old settings removal");
        firmware::SnapshotFixtureProvisioning cut;cut.Begin(revision,doc,arbiter);
        Check(cut.State()==S::Required&&cut.ShouldAutoEnterHardwareTest(),"power cut after Required with old override still on disk re-enters factory");
        modules::HardwareTestRecord good{1U,revision,R::Verified,true,3U,1U,2U,3U};modules::WriteHardwareTestRecord(doc,good);
        cut.OnDurableSettingsSnapshot(doc);Check(cut.State()==S::Required,"stale verified settings cannot supersede Required");
        auto other=good;other.operatorDurationMs=99U;cut.ExpectFactoryResult(other);
        cut.ExpectedReceiptPublication()->store(18U, std::memory_order_release);
        cut.OnDurableSettingsSnapshot(doc, 18U);
        Check(cut.State()==S::Required,"different receipt record bytes cannot promote Required");
        cut.ExpectFactoryResult(good);
        cut.ExpectedReceiptPublication()->store(19U, std::memory_order_release);
        cut.OnDurableSettingsSnapshot(doc);
        cut.OnDurableSettingsSnapshot(doc, 18U);
        Check(cut.State()==S::Required,"identical verified bytes in untracked or older durable callbacks cannot supersede Required");
        factory_provisioning_sdk::failCommit=true;cut.OnDurableSettingsSnapshot(doc, 19U);
        Check(cut.State()==S::Unavailable&&!cut.AllowsCompletedHardwareTestResult(),"NVS commit failure suppresses visible saved PASS");
        factory_provisioning_sdk::failCommit=false;firmware::SnapshotFixtureProvisioning retry;retry.Begin(revision,doc,arbiter);
        Check(retry.State()==S::Required,"failed NVS commit never resurrects stale completion after reboot");
        retry.ExpectFactoryResult(good);retry.ExpectedReceiptPublication()->store(20U, std::memory_order_release);
        retry.OnDurableSettingsSnapshot(doc, 20U);retry.ForgetExpectedResult();
        const auto writesBeforeUp = factory_provisioning_sdk::writes;
        const auto nvsBeforeUp = factory_provisioning_sdk::committed;
        const auto recordBeforeUp = doc[modules::HARDWARE_TEST_RECORD_SETTING_KEY].as<std::string>();
        Check(retry.State()==S::Passed&&!retry.ShouldAutoEnterHardwareTest()&&retry.AllowsPhysicalUpEntry(),
              "durable PASS permits held-Up inspection while ordinary boot remains normal");
        Check(factory_provisioning_sdk::writes == writesBeforeUp && factory_provisioning_sdk::committed == nvsBeforeUp &&
                  doc[modules::HARDWARE_TEST_RECORD_SETTING_KEY].as<std::string>() == recordBeforeUp,
              "held-Up admission changes neither actual NVS bytes nor saved factory record");
        auto preserved=doc.as<JsonVariant>().size();Check(retry.RequireFactoryTest()&&doc.as<JsonVariant>().size()==preserved,"factory clear changes only factory provisioning, never settings/user data");
        factory_provisioning_sdk::failRead=true;firmware::SnapshotFixtureProvisioning readFailed;readFailed.Begin(revision,doc,arbiter);
        Check(readFailed.State()==S::Unavailable&&!readFailed.AllowsCompletedHardwareTestResult()&&readFailed.ShouldAutoEnterHardwareTest(),"read error reports actual repair outcome rather than inferred empty record");
    }
    factory_provisioning_sdk::Reset();hardware::RmtFlashArbiter arbiter;DynamicJsonDocument legacy(512U);
    modules::HardwareTestRecord forced{1U,hardware::BoardRevision::V3S3,R::Forced,false,0U,0U,0U,0U};modules::WriteHardwareTestRecord(legacy,forced);
    firmware::SnapshotFixtureProvisioning old;old.Begin(hardware::BoardRevision::V3S3,legacy,arbiter);
    Check(old.State()==S::Pending&&!old.AllowsCompletedHardwareTestResult(),"readable old Forced is never promoted to verified PASS");
    std::printf("[FACTORY_PROVISIONING_DURABLE] actual provisioning/NVS codec/snapshot owner; SDK flash/power cuts controlled; failures=%d\n",failures);return failures?1:0;
}
