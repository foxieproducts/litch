#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "modules/ota_post_boot_executor.hpp"

namespace {

using modules::OtaPostBootExecutionResult;
using modules::OtaPostBootHealth;
using modules::OtaPostBootImageEvidence;
using modules::OtaPostBootSlotIdentity;
using modules::OtaPostBootSnapshot;
using Raw = std::array<uint8_t, modules::OTA_POST_BOOT_SELECTOR_BYTES>;

enum class Event : uint8_t { Acquire, Snapshot, Release, RunningImage, FallbackImage, Valid, Invalid };

bool Expect(const bool ok, const char* const why) {
    if (!ok) std::printf("FAIL: %s\n", why);
    return ok;
}

void Write32(Raw& raw, const size_t offset, const uint32_t value) {
    for (size_t byte = 0U; byte < 4U; ++byte) {
        raw[offset + byte] = static_cast<uint8_t>(value >> (8U * byte));
    }
}

Raw Entry(const uint32_t sequence, const uint32_t state, const uint32_t crc) {
    Raw raw;
    raw.fill(0xFFU);
    Write32(raw, 0U, sequence);
    Write32(raw, 24U, state);
    Write32(raw, 28U, crc);
    return raw;
}

OtaPostBootSnapshot Pending() {
    OtaPostBootSnapshot snapshot;
    snapshot.slots = {{{0x10000U, 0x10U}, {0x340000U, 0x11U}}};
    snapshot.selectors[0U] = Entry(19U, 2U, 0xBD53078EU);
    snapshot.selectors[1U] = Entry(20U, 1U, 0x20843F37U);
    snapshot.selected = snapshot.running = snapshot.slots[1U];
    snapshot.selectedEntry = 1U;
    snapshot.selectedSequence = 20U;
    return snapshot;
}

OtaPostBootImageEvidence Image(const OtaPostBootSlotIdentity slot) {
    OtaPostBootImageEvidence image;
    image.slot = slot;
    image.buildIdLength = 32U;
    for (size_t byte = 0U; byte < 32U; ++byte) {
        image.buildId[byte] = static_cast<uint8_t>(byte + 1U);
        image.imageSha256[byte] = static_cast<uint8_t>(byte + 33U);
    }
    return image;
}

struct FakeBoot {
    std::array<OtaPostBootSnapshot, 3U> snapshots{};
    OtaPostBootImageEvidence runningImage;
    OtaPostBootImageEvidence fallbackImage;
    std::array<Event, 20U> events{};
    size_t eventCount = 0U;
    unsigned acquires = 0U, reads = 0U, runningReads = 0U, fallbackReads = 0U;
    unsigned validCalls = 0U, invalidCalls = 0U, leaseFaults = 0U;
    unsigned failAcquire = 0U, failSnapshot = 0U;
    bool held = false, runningVerified = true, fallbackVerified = true;
    bool validSucceeds = true, invalidSucceeds = true;

    FakeBoot() {
        snapshots[0U] = snapshots[1U] = snapshots[2U] = Pending();
        Write32(snapshots[2U].selectors[1U], 24U, 2U);
        runningImage = Image(snapshots[0U].running);
        fallbackImage = Image(snapshots[0U].slots[0U]);
    }
    void Note(const Event event) {
        if (eventCount < events.size()) events[eventCount++] = event;
        else ++leaseFaults;
    }
    static FakeBoot& Self(void* const context) { return *static_cast<FakeBoot*>(context); }
    static bool Acquire(void* const context) {
        auto& self = Self(context);
        self.Note(Event::Acquire);
        if (self.held) ++self.leaseFaults;
        if (++self.acquires == self.failAcquire) return false;
        self.held = true;
        return true;
    }
    static void Release(void* const context) {
        auto& self = Self(context);
        self.Note(Event::Release);
        if (!self.held) ++self.leaseFaults;
        self.held = false;
    }
    static bool ReadSnapshot(void* const context, OtaPostBootSnapshot& out) {
        auto& self = Self(context);
        self.Note(Event::Snapshot);
        if (!self.held) ++self.leaseFaults;
        ++self.reads;
        if (self.reads == self.failSnapshot || self.reads > self.snapshots.size()) return false;
        out = self.snapshots[self.reads - 1U];
        return true;
    }
    static bool ReadImage(void* const context, const OtaPostBootSlotIdentity& slot,
                          OtaPostBootImageEvidence& out) {
        auto& self = Self(context);
        if (self.held) ++self.leaseFaults;
        if (slot.address == self.snapshots[0U].running.address) {
            self.Note(Event::RunningImage);
            ++self.runningReads;
            out = self.runningImage;
            return self.runningVerified;
        }
        self.Note(Event::FallbackImage);
        ++self.fallbackReads;
        out = self.fallbackImage;
        return self.fallbackVerified;
    }
    static bool MarkValid(void* const context) {
        auto& self = Self(context);
        self.Note(Event::Valid);
        if (!self.held) ++self.leaseFaults;
        ++self.validCalls;
        return self.validSucceeds;
    }
    static bool MarkInvalid(void* const context) {
        auto& self = Self(context);
        self.Note(Event::Invalid);
        if (!self.held) ++self.leaseFaults;
        ++self.invalidCalls;
        // A real successful SDK call reboots here. Returning models API success
        // only, and must not trigger a fictional pre-reboot selector readback.
        return self.invalidSucceeds;
    }
    modules::OtaPostBootExecutionOps Ops() {
        return {this, &Acquire, &Release, &ReadSnapshot, &ReadImage, &MarkValid, &MarkInvalid};
    }
};

bool Events(const FakeBoot& fake, const std::initializer_list<Event> expected,
             const char* const why) {
    if (!Expect(fake.eventCount == expected.size() && fake.leaseFaults == 0U && !fake.held, why)) return false;
    size_t index = 0U;
    for (const auto event : expected) {
        if (!Expect(fake.events[index++] == event, why)) return false;
    }
    return true;
}

OtaPostBootExecutionResult Run(FakeBoot& fake, const OtaPostBootHealth health) {
    if (health == OtaPostBootHealth::Passed) {
        modules::OtaPostBootPreparedImage prepared;
        const auto result = modules::PrepareOtaPostBootRunningImage(fake.Ops(), prepared);
        return result == OtaPostBootExecutionResult::AwaitingHealth
            ? modules::FinishOtaPostBootRunningImage(fake.Ops(), prepared, health) : result;
    }
    return modules::ExecuteOtaPostBootVerification(fake.Ops(), health);
}

bool TestCheapStartupAndUnknownEvidence() {
    FakeBoot pending;
    if (!Expect(Run(pending, OtaPostBootHealth::NotReported) == OtaPostBootExecutionResult::AwaitingHealth,
                "startup snapshot leaves pending trial unaccepted") ||
        !Events(pending, {Event::Acquire, Event::Snapshot, Event::Release},
                "NotReported performs one cheap leased snapshot, no image hash")) return false;
    for (const uint32_t state : {0U, 3U, 4U, 99U, 0xFFFFFFFFU}) {
        FakeBoot bad;
        Write32(bad.snapshots[0U].selectors[1U], 24U, state);
        if (!Expect(Run(bad, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::NoAction,
                    "unusable selected state never becomes authority") ||
            !Events(bad, {Event::Acquire, Event::Snapshot, Event::Release},
                    "unknown or unselectable evidence does not hash/mutate")) return false;
    }
    FakeBoot valid;
    Write32(valid.snapshots[0U].selectors[1U], 24U, 2U);
    return Expect(Run(valid, OtaPostBootHealth::NotReported) == OtaPostBootExecutionResult::NoAction,
                  "startup VALID is not normal runtime acceptance") &&
           Events(valid, {Event::Acquire, Event::Snapshot, Event::Release},
                  "startup VALID avoids full image hashing");
}

bool TestConfirmationHasNoLeaseGapAndRequiresReadback() {
    FakeBoot fake;
    modules::OtaPostBootPreparedImage prepared;
    if (!Expect(modules::PrepareOtaPostBootRunningImage(fake.Ops(), prepared) ==
                    OtaPostBootExecutionResult::AwaitingHealth &&
                    modules::FinishOtaPostBootRunningImage(fake.Ops(), prepared,
                        OtaPostBootHealth::NotReported) == OtaPostBootExecutionResult::AwaitingHealth &&
                    fake.reads == 1U && fake.validCalls == 0U &&
                    fake.invalidCalls == 0U && !fake.held,
                "full running proof waits for health without SDK mutation or a held lease")) return false;
    return Expect(modules::FinishOtaPostBootRunningImage(fake.Ops(), prepared,
                      OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::Confirmed,
                  "verified healthy current pending trial becomes accepted") &&
           Events(fake, {Event::Acquire, Event::Snapshot, Event::Release, Event::RunningImage,
                         Event::Acquire, Event::Snapshot, Event::Valid, Event::Snapshot, Event::Release},
                  "fresh guard, SDK mark and VALID readback share one uninterrupted flash lease") &&
           Expect(fake.validCalls == 1U && fake.invalidCalls == 0U && fake.fallbackReads == 0U,
                  "confirmation mutates current pending exactly once");
}

bool TestFailedUsbTrialRollsBackWithoutRunningIntegrity() {
    FakeBoot fake;
    // Same current pending20/valid19 production boundary as a failed USB trial.
    // The old direct-ROM history exception waited forever on failed health.
    const uint8_t marker[] = {'L','U','S','B'};
    for (auto& snapshot : fake.snapshots) {
        for (size_t byte = 0U; byte < sizeof(marker); ++byte) {
            snapshot.selectors[1U][4U + byte] = marker[byte];
        }
        for (size_t byte = 0U; byte < 16U; ++byte) {
            snapshot.selectors[1U][8U + byte] = fake.runningImage.imageSha256[byte];
        }
    }
    if (!Expect(Run(fake, OtaPostBootHealth::Failed) == OtaPostBootExecutionResult::RollbackRequested,
                "failed trial requests SDK rollback to completely verified distinct VALID backup")) return false;
    if (!Events(fake, {Event::Acquire, Event::Snapshot, Event::Release, Event::FallbackImage,
                         Event::Acquire, Event::Snapshot, Event::Invalid, Event::Release},
                  "fallback hash precedes fresh guarded invalidation with no invented readback") ||
        !Expect(fake.invalidCalls == 1U && fake.validCalls == 0U && fake.runningReads == 0U,
                "failed health rolls back instead of the old marked-USB AwaitingHealth dead end")) return false;
    FakeBoot brokenRunning;
    brokenRunning.runningVerified = false;
    return Expect(Run(brokenRunning, OtaPostBootHealth::Failed) ==
                      OtaPostBootExecutionResult::RollbackRequested &&
                      brokenRunning.runningReads == 0U && brokenRunning.fallbackReads == 1U,
                  "bad running integrity does not block verified fallback");
}

bool TestValidAcceptanceRequiresFullImageAndFreshValidProof() {
    FakeBoot fake;
    for (auto& snapshot : fake.snapshots) Write32(snapshot.selectors[1U], 24U, 2U);
    if (!Expect(Run(fake, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::Confirmed,
                "previously VALID image becomes normal accepted after full integrity/fresh proof") ||
        !Events(fake, {Event::Acquire, Event::Snapshot, Event::Release, Event::RunningImage,
                       Event::Acquire, Event::Snapshot, Event::Release},
                "already VALID performs no SDK mutation")) return false;
    FakeBoot bad;
    for (auto& snapshot : bad.snapshots) Write32(snapshot.selectors[1U], 24U, 2U);
    bad.runningVerified = false;
    if (!Expect(Run(bad, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::ImageEvidenceUnavailable &&
                    bad.validCalls == 0U && bad.invalidCalls == 0U,
                "raw VALID alone cannot bypass full integrity for normal acceptance")) return false;
    FakeBoot failed;
    Write32(failed.snapshots[0U].selectors[1U], 24U, 2U);
    return Expect(Run(failed, OtaPostBootHealth::Failed) == OtaPostBootExecutionResult::NoAction &&
                      failed.invalidCalls == 0U && failed.runningReads == 0U,
                  "failed health never invalidates an already VALID image");
}

bool TestImageAndBackupProofFailures() {
    for (const auto health : {OtaPostBootHealth::Passed, OtaPostBootHealth::Failed}) {
        for (unsigned fault = 0U; fault < 6U; ++fault) {
            FakeBoot fake;
            auto& image = health == OtaPostBootHealth::Passed ? fake.runningImage : fake.fallbackImage;
            switch (fault) {
                case 0U: fake.runningVerified = fake.fallbackVerified = false; break;
                case 1U: image.slot.address ^= 0x10000U; break;
                case 2U: image.slot.subtype ^= 1U; break;
                case 3U: image.imageSha256.fill(0U); break;
                case 4U: image.buildIdLength = 49U; break;
                case 5U: image.buildId.fill(0U); break;
            }
            if (!Expect(Run(fake, health) == OtaPostBootExecutionResult::ImageEvidenceUnavailable &&
                            fake.validCalls == 0U && fake.invalidCalls == 0U && !fake.held,
                        "full image proof must succeed for the exact owned slot before mutation")) return false;
        }
    }
    FakeBoot noBackup;
    Write32(noBackup.snapshots[0U].selectors[0U], 24U, 4U);
    return Expect(Run(noBackup, OtaPostBootHealth::Failed) == OtaPostBootExecutionResult::NoValidFallback &&
                      noBackup.fallbackReads == 0U && noBackup.invalidCalls == 0U,
                  "missing raw VALID fallback never hashes or invalidates speculatively");
}

bool TestEveryFreshRawChangeBlocksMutation() {
    for (const auto health : {OtaPostBootHealth::Passed, OtaPostBootHealth::Failed}) {
        for (size_t entry = 0U; entry < 2U; ++entry) {
            for (size_t byte = 0U; byte < 32U; ++byte) {
                FakeBoot fake;
                fake.snapshots[1U].selectors[entry][byte] ^= 1U;
                if (!Expect(Run(fake, health) == OtaPostBootExecutionResult::EvidenceChanged &&
                                fake.validCalls == 0U && fake.invalidCalls == 0U &&
                                fake.leaseFaults == 0U && !fake.held,
                            "any selector change during full image verification blocks SDK mutation")) return false;
            }
        }
    }
    for (unsigned fault = 0U; fault < 6U; ++fault) {
        FakeBoot fake;
        auto& fresh = fake.snapshots[1U];
        switch (fault) {
            case 0U: fresh.running = fresh.slots[0U]; break;
            case 1U: fresh.selected = fresh.slots[0U]; break;
            case 2U: fresh.selectedEntry = 0U; break;
            case 3U: ++fresh.selectedSequence; break;
            case 4U: fresh.slots[0U].address += 0x10000U; break;
            case 5U: ++fresh.slots[1U].subtype; break;
        }
        if (!Expect(Run(fake, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::EvidenceChanged &&
                        fake.validCalls == 0U && !fake.held,
                    "every owned SDK partition/selection observation is in the fresh guard")) return false;
    }
    return true;
}

bool TestReadbackAndApiFailuresAreNotAccepted() {
    for (size_t entry = 0U; entry < 2U; ++entry) {
        for (size_t byte = 0U; byte < 32U; ++byte) {
            FakeBoot fake;
            fake.snapshots[2U].selectors[entry][byte] ^= 1U;
            if (!Expect(Run(fake, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::ConfirmReadbackFailed &&
                            fake.validCalls == 1U && fake.invalidCalls == 0U && !fake.held,
                        "successful mark-valid without exact expected readback is not acceptance")) return false;
        }
    }
    FakeBoot markFailed;
    markFailed.validSucceeds = false;
    if (!Expect(Run(markFailed, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::ConfirmFailed &&
                    markFailed.reads == 2U && !markFailed.held,
                "mark-valid error returns unaccepted without imaginary readback")) return false;
    FakeBoot rollbackFailed;
    rollbackFailed.invalidSucceeds = false;
    return Expect(Run(rollbackFailed, OtaPostBootHealth::Failed) == OtaPostBootExecutionResult::RollbackFailed &&
                      rollbackFailed.reads == 2U && rollbackFailed.invalidCalls == 1U && !rollbackFailed.held,
                  "SDK invalidation error reports failure without additional reset or readback");
}

bool TestLeaseAndSnapshotFaultsReleaseExactly() {
    for (const unsigned fail : {1U, 2U}) {
        FakeBoot fake;
        fake.failAcquire = fail;
        if (!Expect(Run(fake, OtaPostBootHealth::Passed) == OtaPostBootExecutionResult::SnapshotUnavailable &&
                        fake.validCalls == 0U && fake.leaseFaults == 0U && !fake.held,
                    "denied initial/final lease cannot mutate or release an unheld lease")) return false;
    }
    for (const unsigned fail : {1U, 2U, 3U}) {
        FakeBoot fake;
        fake.failSnapshot = fail;
        const auto expected = fail == 3U ? OtaPostBootExecutionResult::ConfirmReadbackFailed
                                         : OtaPostBootExecutionResult::SnapshotUnavailable;
        if (!Expect(Run(fake, OtaPostBootHealth::Passed) == expected &&
                        fake.leaseFaults == 0U && !fake.held,
                    "snapshot/readback failure releases its one owned lease")) return false;
    }
    for (unsigned missing = 0U; missing < 6U; ++missing) {
        FakeBoot fake;
        auto ops = fake.Ops();
        switch (missing) {
            case 0U: ops.acquireFlash = nullptr; break;
            case 1U: ops.releaseFlash = nullptr; break;
            case 2U: ops.readSnapshot = nullptr; break;
            case 3U: ops.readImage = nullptr; break;
            case 4U: ops.markValid = nullptr; break;
            case 5U: ops.markInvalidAndReboot = nullptr; break;
        }
        if (!Expect(modules::ExecuteOtaPostBootVerification(ops, OtaPostBootHealth::Passed) ==
                        OtaPostBootExecutionResult::Unsupported && fake.eventCount == 0U,
                    "incomplete worker operation table has no side effects")) return false;
    }
    return true;
}

} // namespace

int main() {
    if (!TestCheapStartupAndUnknownEvidence() ||
        !TestConfirmationHasNoLeaseGapAndRequiresReadback() ||
        !TestFailedUsbTrialRollsBackWithoutRunningIntegrity() ||
        !TestValidAcceptanceRequiresFullImageAndFreshValidProof() ||
        !TestImageAndBackupProofFailures() || !TestEveryFreshRawChangeBlocksMutation() ||
        !TestReadbackAndApiFailuresAreNotAccepted() ||
        !TestLeaseAndSnapshotFaultsReleaseExactly()) return 1;
    std::printf("PASS SDK post-boot executor\n");
    return 0;
}
