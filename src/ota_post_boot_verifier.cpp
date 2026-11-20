#include "modules/ota_post_boot_verifier.hpp"

#include <limits>

#include "modules/ota_slot_policy.hpp"

namespace modules {
namespace {

constexpr size_t STATE_OFFSET = 24U;
constexpr size_t CRC_OFFSET = 28U;
constexpr uint32_t STATE_PENDING = 1U;
constexpr uint32_t STATE_VALID = 2U;
constexpr uint32_t STATE_INVALID = 3U;
constexpr uint32_t STATE_ABORTED = 4U;

using Selector = OtaPostBootSelectorEvidence;

uint32_t ReadLe32(const uint8_t* const bytes) {
    return static_cast<uint32_t>(bytes[0U]) |
           (static_cast<uint32_t>(bytes[1U]) << 8U) |
           (static_cast<uint32_t>(bytes[2U]) << 16U) |
           (static_cast<uint32_t>(bytes[3U]) << 24U);
}

uint32_t SequenceCrc(const uint8_t* const bytes) {
    // ESP-IDF's ota-select CRC covers only the four sequence bytes. This is
    // binascii.crc32(sequence_le, 0xffffffff), not the old record codec's CRC.
    uint32_t crc = 0U;
    for (size_t index = 0U; index < sizeof(uint32_t); ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

Selector Decode(const std::array<uint8_t, OTA_POST_BOOT_SELECTOR_BYTES>& raw) {
    return {ReadLe32(raw.data()), ReadLe32(raw.data() + STATE_OFFSET),
            ReadLe32(raw.data() + CRC_OFFSET) == SequenceCrc(raw.data())};
}

bool SlotsMatch(const OtaPostBootSlotIdentity& left,
                const OtaPostBootSlotIdentity& right) {
    return left.address == right.address && left.subtype == right.subtype;
}

bool SupportedSlots(const OtaPostBootSnapshot& snapshot) {
    return snapshot.slots[0U].subtype == OTA_SLOT_0_SUBTYPE &&
           snapshot.slots[1U].subtype == OTA_SLOT_1_SUBTYPE &&
           snapshot.slots[0U].address != 0U &&
           snapshot.slots[1U].address != 0U &&
           (snapshot.slots[0U].address & 0xFFFFU) == 0U &&
           (snapshot.slots[1U].address & 0xFFFFU) == 0U &&
           snapshot.slots[0U].address != snapshot.slots[1U].address;
}

bool SdkSelectable(const Selector& entry) {
    // The pinned SDK also selects sequence zero and unknown states. Model that
    // selection first, then refuse them rather than silently choosing a backup.
    return entry.sequence != std::numeric_limits<uint32_t>::max() &&
           entry.crcValid && entry.state != STATE_INVALID &&
           entry.state != STATE_ABORTED;
}

uint8_t SelectedEntry(const std::array<Selector, 2U>& entries) {
    const bool first = SdkSelectable(entries[0U]);
    const bool second = SdkSelectable(entries[1U]);
    if (first && second) {
        return entries[0U].sequence >= entries[1U].sequence ? 0U : 1U;
    }
    return first ? 0U : (second ? 1U : OTA_POST_BOOT_NO_ENTRY);
}

} // namespace

bool DecodeCoherentOtaPostBootSnapshot(
    const OtaPostBootSnapshot& snapshot,
    std::array<OtaPostBootSelectorEvidence, 2U>& outSelectors) {
    if (!SupportedSlots(snapshot)) return false;
    const std::array<Selector, 2U> entries{
        Decode(snapshot.selectors[0U]), Decode(snapshot.selectors[1U])};
    const uint8_t selected = SelectedEntry(entries);
    if (selected == OTA_POST_BOOT_NO_ENTRY ||
        snapshot.selectedEntry != selected) return false;
    const auto& entry = entries[selected];
    if (entry.sequence == 0U ||
        snapshot.selectedSequence != entry.sequence ||
        !SlotsMatch(snapshot.slots[(entry.sequence - 1U) % 2U],
                    snapshot.selected) ||
        (!SlotsMatch(snapshot.running, snapshot.slots[0U]) &&
         !SlotsMatch(snapshot.running, snapshot.slots[1U]))) return false;
    outSelectors = entries;
    return true;
}

OtaPostBootEvaluation EvaluateOtaPostBootVerification(
    const OtaPostBootSnapshot& snapshot, const OtaPostBootHealth health) {
    OtaPostBootEvaluation result;
    std::array<Selector, 2U> entries{};
    if (!DecodeCoherentOtaPostBootSnapshot(snapshot, entries) ||
        !SlotsMatch(snapshot.selected, snapshot.running)) return result;
    const uint8_t selected = snapshot.selectedEntry;
    const auto& entry = entries[selected];
    if (entry.state == STATE_VALID) {
        result.runningState = OtaPostBootRunningState::Valid;
        result.decision = OtaPostBootDecision::AlreadyValid;
        if (health == OtaPostBootHealth::Passed) {
            result.action = OtaPostBootAction::AcceptValid;
        }
        return result;
    }
    if (entry.state != STATE_PENDING) return result;
    result.runningState = OtaPostBootRunningState::PendingVerify;
    if (health == OtaPostBootHealth::NotReported) {
        result.decision = OtaPostBootDecision::AwaitingHealth;
        result.action = OtaPostBootAction::AwaitHealth;
    } else if (health == OtaPostBootHealth::Passed) {
        result.decision = OtaPostBootDecision::HealthPassed;
        result.action = OtaPostBootAction::Confirm;
    } else if (health == OtaPostBootHealth::Failed) {
        result.decision = OtaPostBootDecision::NoValidFallback;
        const uint8_t backup = 1U - selected;
        const auto& fallback = entries[backup];
        if (fallback.crcValid && fallback.sequence != 0U &&
            fallback.sequence != std::numeric_limits<uint32_t>::max() &&
            fallback.state == STATE_VALID) {
            const auto slot = snapshot.slots[(fallback.sequence - 1U) % 2U];
            if (!SlotsMatch(slot, snapshot.running)) {
                result.decision = OtaPostBootDecision::HealthFailed;
                result.action = OtaPostBootAction::Rollback;
                result.fallbackEntry = backup;
                result.fallback = slot;
            }
        }
    }
    return result;
}

bool OtaPostBootSnapshotsMatch(const OtaPostBootSnapshot& left,
                               const OtaPostBootSnapshot& right) {
    return left.selectors == right.selectors &&
           SlotsMatch(left.slots[0U], right.slots[0U]) &&
           SlotsMatch(left.slots[1U], right.slots[1U]) &&
           SlotsMatch(left.running, right.running) &&
           SlotsMatch(left.selected, right.selected) &&
           left.selectedEntry == right.selectedEntry &&
           left.selectedSequence == right.selectedSequence;
}

bool IsOtaPostBootConfirmationReadback(const OtaPostBootSnapshot& pending,
                                      const OtaPostBootSnapshot& valid) {
    if (EvaluateOtaPostBootVerification(pending, OtaPostBootHealth::Passed).action !=
            OtaPostBootAction::Confirm ||
        EvaluateOtaPostBootVerification(valid, OtaPostBootHealth::Passed).action !=
            OtaPostBootAction::AcceptValid) return false;
    OtaPostBootSnapshot expected = pending;
    auto& raw = expected.selectors[expected.selectedEntry];
    raw[STATE_OFFSET] = static_cast<uint8_t>(STATE_VALID);
    raw[STATE_OFFSET + 1U] = 0U;
    raw[STATE_OFFSET + 2U] = 0U;
    raw[STATE_OFFSET + 3U] = 0U;
    return OtaPostBootSnapshotsMatch(expected, valid);
}

} // namespace modules
