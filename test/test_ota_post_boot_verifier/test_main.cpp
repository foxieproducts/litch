#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "modules/ota_post_boot_verifier.hpp"

namespace {

using modules::OtaPostBootAction;
using modules::OtaPostBootDecision;
using modules::OtaPostBootHealth;
using modules::OtaPostBootRunningState;
using modules::OtaPostBootSnapshot;
using Raw = std::array<uint8_t, modules::OTA_POST_BOOT_SELECTOR_BYTES>;

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
    snapshot.running = snapshot.selected = snapshot.slots[1U];
    snapshot.selectedEntry = 1U;
    snapshot.selectedSequence = 20U;
    return snapshot;
}

bool NoAction(const OtaPostBootSnapshot& snapshot, const char* const why) {
    for (const auto health : {OtaPostBootHealth::NotReported,
                              OtaPostBootHealth::Passed,
                              OtaPostBootHealth::Failed}) {
        if (!Expect(modules::EvaluateOtaPostBootVerification(snapshot, health).action ==
                        OtaPostBootAction::None, why)) return false;
    }
    return true;
}

bool TestPendingAndValidHealthDecisions() {
    auto snapshot = Pending();
    const auto waiting = modules::EvaluateOtaPostBootVerification(
        snapshot, OtaPostBootHealth::NotReported);
    const auto confirm = modules::EvaluateOtaPostBootVerification(
        snapshot, OtaPostBootHealth::Passed);
    const auto rollback = modules::EvaluateOtaPostBootVerification(
        snapshot, OtaPostBootHealth::Failed);
    if (!Expect(waiting.action == OtaPostBootAction::AwaitHealth &&
                    waiting.runningState == OtaPostBootRunningState::PendingVerify,
                "selected pending trial waits for reported health") ||
        !Expect(confirm.action == OtaPostBootAction::Confirm,
                "passed current pending trial requires confirmation") ||
        !Expect(rollback.action == OtaPostBootAction::Rollback &&
                    rollback.fallbackEntry == 0U &&
                    rollback.fallback.address == snapshot.slots[0U].address,
                "failed pending trial identifies distinct raw VALID backup")) return false;
    Write32(snapshot.selectors[1U], 24U, 2U);
    if (!Expect(modules::EvaluateOtaPostBootVerification(
                    snapshot, OtaPostBootHealth::Passed).action ==
                        OtaPostBootAction::AcceptValid,
                "already VALID requires normal health acceptance without mutation")) return false;
    for (const auto health : {OtaPostBootHealth::NotReported, OtaPostBootHealth::Failed}) {
        if (!Expect(modules::EvaluateOtaPostBootVerification(snapshot, health).action ==
                        OtaPostBootAction::None,
                    "VALID neither accepts nor invalidates on unpassed health")) return false;
    }
    return true;
}

bool TestExactSdkSelectionAndStrictStates() {
    for (const uint32_t state : {0U, 3U, 4U, 5U, 99U, 0xFFFFFFFFU}) {
        auto snapshot = Pending();
        Write32(snapshot.selectors[1U], 24U, state);
        if (!NoAction(snapshot, "NEW, invalid, aborted and unknown selected state cannot mutate")) return false;
    }
    auto zero = Pending();
    zero.selectors[0U] = Entry(0U, 2U, 0xFFFFFFFFU);
    zero.selectors[1U].fill(0xFFU);
    zero.selectedEntry = 0U;
    zero.selectedSequence = 0U;
    zero.selected = zero.running = zero.slots[1U];
    if (!NoAction(zero, "SDK-selectable sequence zero is strictly refused")) return false;
    // A CRC-valid unknown newer entry must not be hidden by choosing old A.
    auto unknown = Pending();
    Write32(unknown.selectors[1U], 24U, 99U);
    unknown.selectedEntry = 0U;
    unknown.selectedSequence = 19U;
    unknown.selected = unknown.running = unknown.slots[0U];
    if (!NoAction(unknown, "SDK selection of unknown newer state prevents fallback authorization")) return false;
    auto duplicate = Pending();
    duplicate.selectors[1U] = Entry(21U, 1U, 0x98385852U);
    duplicate.selectedSequence = 21U;
    duplicate.selected = duplicate.running = duplicate.slots[0U];
    const auto pending = modules::EvaluateOtaPostBootVerification(duplicate, OtaPostBootHealth::Passed);
    const auto failed = modules::EvaluateOtaPostBootVerification(duplicate, OtaPostBootHealth::Failed);
    if (!Expect(pending.action == OtaPostBootAction::Confirm &&
                    pending.runningState == OtaPostBootRunningState::PendingVerify,
                "newer same-slot pending outranks first matching old VALID entry") ||
        !Expect(failed.decision == OtaPostBootDecision::NoValidFallback,
                "duplicate same-slot VALID is not a distinct backup")) return false;
    duplicate.selectors[0U] = Entry(21U, 2U, 0x98385852U);
    duplicate.selectedEntry = 1U;
    if (!NoAction(duplicate, "equal SDK sequence chooses first entry, not claimed second")) return false;
    return true;
}

bool TestMetadataAndFallbackFaults() {
    for (unsigned fault = 0U; fault < 10U; ++fault) {
        auto snapshot = Pending();
        switch (fault) {
            case 0U: snapshot.selectedEntry = 2U; break;
            case 1U: ++snapshot.selectedSequence; break;
            case 2U: snapshot.selected = snapshot.slots[0U]; break;
            case 3U: snapshot.running = snapshot.slots[0U]; break;
            case 4U: snapshot.slots[0U].address = 0U; break;
            case 5U: snapshot.slots[0U].address = snapshot.slots[1U].address; break;
            case 6U: ++snapshot.slots[1U].address; break;
            case 7U: snapshot.slots[0U].subtype = 0x12U; break;
            case 8U: snapshot.selectors[1U][28U] ^= 1U; break;
            case 9U: snapshot.selectors[0U].fill(0xFFU); snapshot.selectors[1U].fill(0xFFU); break;
        }
        if (!NoAction(snapshot, "incoherent SDK/slot/CRC observation has no authority")) return false;
    }
    for (const uint32_t state : {0U, 1U, 3U, 4U, 99U, 0xFFFFFFFFU}) {
        auto snapshot = Pending();
        Write32(snapshot.selectors[0U], 24U, state);
        if (!Expect(modules::EvaluateOtaPostBootVerification(
                        snapshot, OtaPostBootHealth::Failed).decision ==
                            OtaPostBootDecision::NoValidFallback,
                    "fallback state must be raw VALID")) return false;
    }
    for (const bool badCrc : {false, true}) {
        auto snapshot = Pending();
        snapshot.selectors[0U] = badCrc ? Entry(19U, 2U, 0U)
                                      : Entry(18U, 2U, 0x05EF60EBU);
        if (!Expect(modules::EvaluateOtaPostBootVerification(
                        snapshot, OtaPostBootHealth::Failed).decision ==
                            OtaPostBootDecision::NoValidFallback,
                    "fallback requires valid CRC and a different physical slot")) return false;
    }
    return true;
}

bool TestPythonSelectorCrcVectors() {
    // Independent binascii.crc32(struct.pack('<I', seq), 0xffffffff) outputs.
    constexpr std::array<std::array<uint32_t, 2U>, 10U> VECTORS{{
        {{1U, 0x4743989AU}}, {{2U, 0x55F63774U}}, {{16U, 0xAFE6A860U}},
        {{18U, 0x05EF60EBU}}, {{19U, 0xBD53078EU}}, {{20U, 0x20843F37U}},
        {{21U, 0x98385852U}}, {{22U, 0x8A8DF7BCU}}, {{42U, 0x3070B0A5U}},
        {{0xFFFFFFFEU, 0x99F8B879U}},
    }};
    for (const auto& vector : VECTORS) {
        auto snapshot = Pending();
        snapshot.selectors[0U] = Entry(vector[0U], 1U, vector[1U]);
        snapshot.selectors[1U].fill(0xFFU);
        snapshot.selectedEntry = 0U;
        snapshot.selectedSequence = vector[0U];
        snapshot.selected = snapshot.running = snapshot.slots[(vector[0U] - 1U) % 2U];
        if (!Expect(modules::EvaluateOtaPostBootVerification(snapshot, OtaPostBootHealth::Passed).action ==
                        OtaPostBootAction::Confirm, "raw SDK CRC agrees with independent Python values")) return false;
        snapshot.selectors[0U][28U] ^= 1U;
        if (!NoAction(snapshot, "flipped selector CRC cannot select an image")) return false;
    }
    return true;
}

bool TestExactFreshAndConfirmationComparison() {
    const auto pending = Pending();
    auto valid = pending;
    Write32(valid.selectors[1U], 24U, 2U);
    if (!Expect(modules::IsOtaPostBootConfirmationReadback(pending, valid),
                "only selected PENDING to VALID is accepted readback")) return false;
    for (size_t entry = 0U; entry < 2U; ++entry) {
        for (size_t byte = 0U; byte < 32U; ++byte) {
            auto changed = pending;
            changed.selectors[entry][byte] ^= 1U;
            if (!Expect(!modules::OtaPostBootSnapshotsMatch(pending, changed),
                        "fresh guard compares every raw selector byte")) return false;
            changed = valid;
            changed.selectors[entry][byte] ^= 1U;
            if (!Expect(!modules::IsOtaPostBootConfirmationReadback(pending, changed),
                        "confirmation readback rejects every additional raw change")) return false;
        }
    }
    valid.running.address = 0x670000U;
    return Expect(!modules::IsOtaPostBootConfirmationReadback(pending, valid),
                  "confirmation cannot switch running partition");
}

bool TestCapturedUsb3AbortedTrialPreservedWhileAIsHealthy() {
    // Exact32-byte entries from the read-only USB3 capture (seq19VALID/20ABORTED).
    auto snapshot = Pending();
    snapshot.selectors[1U] = {{
        0x14U,0U,0U,0U,0x4CU,0x55U,0x53U,0x42U,0x4BU,0x93U,0xC7U,0xB5U,
        0x75U,0xEAU,0x11U,0x7EU,0x0DU,0xABU,0xFDU,0x06U,0x1AU,0xC5U,0xF3U,0x86U,
        4U,0U,0U,0U,0x37U,0x3FU,0x84U,0x20U}};
    snapshot.running = snapshot.selected = snapshot.slots[0U];
    snapshot.selectedEntry = 0U;
    snapshot.selectedSequence = 19U;
    const auto retained = snapshot;
    const auto health = modules::EvaluateOtaPostBootVerification(snapshot, OtaPostBootHealth::Passed);
    return Expect(health.action == OtaPostBootAction::AcceptValid &&
                      health.runningState == OtaPostBootRunningState::Valid &&
                      snapshot.running.subtype == 0x10U,
                  "fallback A health is current A evidence, never B trial success") &&
           Expect(modules::OtaPostBootSnapshotsMatch(snapshot, retained) &&
                      snapshot.selectors[1U][24U] == 4U,
                  "ABORTED20 remains owned raw diagnosis after evaluating healthy A");
}

} // namespace

int main() {
    if (!TestPendingAndValidHealthDecisions() || !TestExactSdkSelectionAndStrictStates() ||
        !TestMetadataAndFallbackFaults() || !TestPythonSelectorCrcVectors() ||
        !TestExactFreshAndConfirmationComparison() ||
        !TestCapturedUsb3AbortedTrialPreservedWhileAIsHealthy()) return 1;
    std::printf("PASS SDK post-boot verifier\n");
    return 0;
}
