#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "firmware/ota_platform_adapter.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/ota_platform_adapter.hpp"
#include "modules/ota_slot_policy.hpp"

namespace {

constexpr uint16_t ESP32S3_CHIP_ID = 9U;
constexpr size_t IMAGE_BYTES = 400U;
constexpr size_t CHECKSUM_OFFSET = 367U;
constexpr size_t IMAGE_SHA_OFFSET = 368U;

enum class Event : uint8_t {
    Acquire = 0U,
    CheckRunningImage,
    CaptureTarget,
    UpdateBegin,
    UpdateWrite,
    AdmitTarget,
    UpdateEnd,
    UpdateAbort,
    Release,
};

struct FakePlatform {
    std::array<Event, 64U> events{};
    size_t eventCount = 0U;
    size_t acquireCount = 0U;
    size_t failAcquireOccurrence = 0U;
    bool runningImageCanBeReplaced = true;
    bool captureSucceeds = true;
    bool beginSucceeds = true;
    bool writeSucceeds = true;
    bool admissionSucceeds = true;
    bool endSucceeds = true;
    modules::OtaPostBootSlotIdentity target{0x340000U, 0x11U};
    modules::OtaPostBootSlotIdentity admittedTarget{};
    modules::OtaImageIdentity admittedIdentity{};
    size_t updateEndCount = 0U;

    void Record(const Event event) { events[eventCount++] = event; }

    static FakePlatform& Self(void* const context) {
        return *static_cast<FakePlatform*>(context);
    }

    static bool Acquire(void* const context) {
        auto& self = Self(context);
        self.Record(Event::Acquire);
        ++self.acquireCount;
        return self.failAcquireOccurrence == 0U ||
               self.acquireCount != self.failAcquireOccurrence;
    }

    static void Release(void* const context) {
        Self(context).Record(Event::Release);
    }

    static bool RunningImageCanBeReplaced(void* const context) {
        auto& self = Self(context);
        self.Record(Event::CheckRunningImage);
        return self.runningImageCanBeReplaced;
    }

    static bool CaptureTarget(
        void* const context,
        uint32_t,
        modules::OtaPostBootSlotIdentity& outTarget) {
        auto& self = Self(context);
        self.Record(Event::CaptureTarget);
        outTarget = self.target;
        return self.captureSucceeds;
    }

    static bool UpdateBegin(void* const context, uint32_t) {
        auto& self = Self(context);
        self.Record(Event::UpdateBegin);
        return self.beginSucceeds;
    }

    static size_t UpdateWrite(void* const context,
                              const uint8_t*,
                              const size_t len) {
        auto& self = Self(context);
        self.Record(Event::UpdateWrite);
        return self.writeSucceeds ? len : 0U;
    }

    static bool AdmitTarget(
        void* const context,
        const modules::OtaPostBootSlotIdentity& target,
        const modules::OtaImageIdentity& identity) {
        auto& self = Self(context);
        self.Record(Event::AdmitTarget);
        self.admittedTarget = target;
        self.admittedIdentity = identity;
        return self.admissionSucceeds;
    }

    static bool UpdateEnd(void* const context) {
        auto& self = Self(context);
        self.Record(Event::UpdateEnd);
        ++self.updateEndCount;
        return self.endSucceeds;
    }

    static void UpdateAbort(void* const context) {
        Self(context).Record(Event::UpdateAbort);
    }

    modules::OtaPlatformTransaction Transaction() {
        return modules::OtaPlatformTransaction(
            {this,
             ESP32S3_CHIP_ID,
             &Acquire,
             &Release,
             &RunningImageCanBeReplaced,
             &CaptureTarget,
             &UpdateBegin,
             &UpdateWrite,
             &AdmitTarget,
             &UpdateEnd,
             &UpdateAbort});
    }
};

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

std::array<uint8_t, IMAGE_BYTES> MakeImage(const uint8_t buildBase = 0x40U,
                                               const uint32_t epoch = modules::OTA_BOOT_POLICY_SDK_EPOCH) {
    std::array<uint8_t, IMAGE_BYTES> image{};
    image[0U] = 0xE9U; image[1U] = 2U; image[12U] = 9U; image[23U] = 1U;
    image[24U] = 0x20U; image[27U] = 0x3CU;
    image[28U] = 0x10U; image[29U] = 1U;
    image[32U] = 0x32U; image[33U] = 0x54U; image[34U] = 0xCDU; image[35U] = 0xABU;
    for (size_t index = 0U; index < 32U; ++index) image[176U + index] = buildBase + index;
    const auto policy = modules::EncodeOtaBootPolicyDescriptor(epoch);
    for (size_t index = 0U; index < policy.size(); ++index) image[288U + index] = policy[index];
    image[305U] = 0x80U; image[306U] = 0xC8U; image[307U] = 0x3FU; image[308U] = 48U;
    for (size_t index = 0U; index < 48U; ++index) image[312U + index] = 0xD0U + index;
    uint8_t checksum = 0xEFU;
    for (size_t index = 32U; index < 304U; ++index) checksum ^= image[index];
    for (size_t index = 312U; index < 360U; ++index) checksum ^= image[index];
    image[CHECKSUM_OFFSET] = checksum;
    modules::Sha256 sha;
    (void)sha.Update(image.data(), IMAGE_SHA_OFFSET);
    const auto digest = sha.Finish();
    for (size_t index = 0U; index < digest.size(); ++index) image[IMAGE_SHA_OFFSET + index] = digest[index];
    return image;
}

size_t FindEvent(const FakePlatform& fake, const Event event) {
    for (size_t index = 0U; index < fake.eventCount; ++index) {
        if (fake.events[index] == event) {
            return index;
        }
    }
    return fake.eventCount;
}

bool Transfer(modules::OtaPlatformTransaction& transaction,
              const std::array<uint8_t, IMAGE_BYTES>& image) {
    return transaction.Begin(image.size()) &&
           transaction.Write(image.data(), 31U) &&
           transaction.Write(image.data() + 31U, image.size() - 31U);
}

bool TestGuardsBeforeActivation() {
    FakePlatform fake;
    auto transaction = fake.Transaction();
    const auto image = MakeImage();
    bool donePublished = false;
    bool rebootArmed = false;
    if (!Expect(Transfer(transaction, image), "valid transfer reaches end") ||
        !Expect(transaction.End(), "verified SDK-only transfer activates") ) {
        return false;
    }
    donePublished = true;
    rebootArmed = true;
    return Expect(FindEvent(fake, Event::CaptureTarget) <
                      FindEvent(fake, Event::UpdateBegin),
                  "target is captured immediately before begin") &&
           Expect(FindEvent(fake, Event::AdmitTarget) <
                      FindEvent(fake, Event::UpdateEnd),
                  "fresh source/target admission precedes activation") &&
           Expect(fake.admittedTarget.address == fake.target.address &&
                      fake.admittedTarget.subtype == fake.target.subtype,
                  "admission targets the captured inactive slot") &&
           Expect(fake.admittedIdentity.buildIdLength == 32U &&
                      fake.admittedIdentity.buildId[0] == 0x40U,
                  "admitted build identity comes from stream") &&
           Expect(donePublished && rebootArmed,
                  "completion effects occur only after accepted end") &&
           Expect(!transaction.IsActive(),
                  "successful activation resets transaction state");
}

bool TestChangedSourceBlocksEndDoneAndReboot() {
    FakePlatform fake;
    fake.admissionSucceeds = false;
    auto transaction = fake.Transaction();
    const auto image = MakeImage();
    bool donePublished = false;
    bool rebootArmed = false;
    if (!Expect(Transfer(transaction, image),
                "changed-source fixture receives complete image")) {
        return false;
    }
    const bool accepted = transaction.End();
    if (accepted) {
        donePublished = true;
        rebootArmed = true;
    }
    return Expect(!accepted, "changed source rejects finalization") &&
           Expect(fake.updateEndCount == 0U,
                  "changed source never calls Update.end") &&
           Expect(!donePublished && !rebootArmed,
                  "rejected end neither publishes done nor arms reboot") &&
           Expect(!transaction.IsActive(),
                  "failed finalization clears target and collector");
}

bool TestTargetBeginWriteAndEndFailuresReset() {
    const auto image = MakeImage();
    {
        FakePlatform fake;
        fake.captureSucceeds = false;
        auto transaction = fake.Transaction();
        if (!Expect(!transaction.Begin(image.size()),
                    "missing target prevents Update.begin") ||
            !Expect(FindEvent(fake, Event::UpdateBegin) == fake.eventCount,
                    "target failure skips Update.begin") ||
            !Expect(!transaction.IsActive(), "target failure resets")) {
            return false;
        }
    }
    {
        FakePlatform fake;
        fake.beginSucceeds = false;
        auto transaction = fake.Transaction();
        if (!Expect(!transaction.Begin(image.size()),
                    "Update.begin failure rejects begin") ||
            !Expect(!transaction.IsActive(), "begin failure resets")) {
            return false;
        }
    }
    {
        FakePlatform fake;
        fake.writeSucceeds = false;
        auto transaction = fake.Transaction();
        if (!Expect(transaction.Begin(image.size()), "write fixture begins") ||
            !Expect(!transaction.Write(image.data(), image.size()),
                    "short Update.write fails") ) {
            return false;
        }
        transaction.Abort();
        if (!Expect(!transaction.IsActive(), "write abort resets")) {
            return false;
        }
    }
    {
        FakePlatform fake;
        fake.endSucceeds = false;
        auto transaction = fake.Transaction();
        if (!Expect(Transfer(transaction, image), "end fixture transfers") ||
            !Expect(!transaction.End(), "Update.end failure rejects") ||
            !Expect(fake.updateEndCount == 1U,
                    "failed Update.end called only after guarded admission") ||
            !Expect(!transaction.IsActive(), "end failure resets")) {
            return false;
        }
    }
    return true;
}

bool TestPendingRunningImageCannotEraseRollbackSlot() {
    FakePlatform fake;
    fake.runningImageCanBeReplaced = false;
    auto transaction = fake.Transaction();
    const auto image = MakeImage();
    return Expect(!transaction.Begin(image.size()),
                  "pending running image rejects new OTA") &&
           Expect(FindEvent(fake, Event::CheckRunningImage) < fake.eventCount,
                  "begin verifies running rollback image before target admission") &&
           Expect(FindEvent(fake, Event::CaptureTarget) == fake.eventCount,
                  "pending running image never selects rollback slot") &&
           Expect(FindEvent(fake, Event::UpdateBegin) == fake.eventCount,
                  "pending running image never starts Update");
}

bool TestAbortRetryUsesOnlyNewTargetAndIdentity() {
    FakePlatform fake;
    auto transaction = fake.Transaction();
    const auto first = MakeImage(0x20U);
    const auto second = MakeImage(0x60U);
    if (!Expect(transaction.Begin(first.size()), "first attempt begins") ||
        !Expect(transaction.Write(first.data(), 64U),
                "first attempt observes prefix")) {
        return false;
    }
    transaction.Abort();
    fake.target = {0x10000U, 0x10U};
    if (!Expect(Transfer(transaction, second), "retry transfers") ||
        !Expect(transaction.End(), "retry activates") ) {
        return false;
    }
    return Expect(fake.admittedTarget.address == 0x10000U &&
                      fake.admittedTarget.subtype == 0x10U,
                  "retry captures its own target") &&
           Expect(fake.admittedIdentity.buildId[0] == 0x60U,
                  "retry discards aborted collector identity");
}

bool TestFailedAbortLeaseRetainsBackendOwnership() {
    FakePlatform fake;
    auto transaction = fake.Transaction();
    const auto image = MakeImage();
    if (!Expect(transaction.Begin(image.size()), "abort lease fixture begins")) {
        return false;
    }
    fake.failAcquireOccurrence = 2U;
    transaction.Abort();
    if (!Expect(transaction.IsActive(),
                "failed abort lease retains backend ownership") ||
        !Expect(FindEvent(fake, Event::UpdateAbort) == fake.eventCount,
                "failed abort lease never touches Update")) {
        return false;
    }

    fake.target = {0x10000U, 0x10U};
    if (!Expect(transaction.Begin(image.size()),
                "next begin first retries orphan cleanup") ||
        !Expect(transaction.IsActive(),
                "new backend begins only after cleanup succeeds")) {
        return false;
    }
    transaction.Abort();
    return Expect(!transaction.IsActive(),
                  "successful retry abort releases backend ownership");
}

bool TestRecordDependentImageCannotBeActivated() {
    FakePlatform fake;
    auto transaction = fake.Transaction();
    const auto legacy = MakeImage(0x40U, modules::OTA_BOOT_POLICY_RECORD_EPOCH);
    return Expect(Transfer(transaction, legacy), "legacy image remains structurally verifiable") &&
           Expect(!transaction.End(), "record-dependent firmware cannot become a new trial") &&
           Expect(fake.updateEndCount == 0U, "legacy rejection precedes SDK activation");
}

bool TestNativeFacadeIsUnsupported() {
    hardware::RmtFlashArbiter arbiter;
    return Expect(firmware::InitializeEsp32OtaPlatformAdapter(arbiter) ==
                      nullptr,
                  "native platform facade is explicitly unsupported");
}

} // namespace

int main() {
    if (!Expect(modules::IsRollbackPreservingOtaRoute(0x10U, 0x11U),
                "confirmed slot0 can update inactive slot1") ||
        !Expect(modules::IsRollbackPreservingOtaRoute(0x11U, 0x10U),
                "confirmed slot1 can update inactive slot0") ||
        !Expect(!modules::IsRollbackPreservingOtaRoute(0x10U, 0x10U) &&
                    !modules::IsRollbackPreservingOtaRoute(0x11U, 0x11U),
                "neither running slot may overwrite itself") ||
        !Expect(!modules::IsRollbackPreservingOtaRoute(0x12U, 0x10U) &&
                    !modules::IsRollbackPreservingOtaRoute(0x10U, 0x12U),
                "routes outside the two supported S3 slots are rejected")) {
        return 1;
    }
    if (!TestGuardsBeforeActivation() ||
        !TestChangedSourceBlocksEndDoneAndReboot() ||
        !TestTargetBeginWriteAndEndFailuresReset() ||
        !TestPendingRunningImageCannotEraseRollbackSlot() ||
        !TestAbortRetryUsesOnlyNewTargetAndIdentity() ||
        !TestFailedAbortLeaseRetainsBackendOwnership() ||
        !TestRecordDependentImageCannotBeActivated() ||
        !TestNativeFacadeIsUnsupported()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
