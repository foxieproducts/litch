#pragma once

#include <cstdint>

#include "modules/led_power_envelope.hpp"
#include "runtime/runtime_health.hpp"

namespace firmware {

// CC advertisements describe a source; losing an advertisement is not evidence
// of brownout, cable droop, or an unsafe load.
inline bool MakePowerSourceRuntimeHealthFact(
    const modules::StrongSourceConfirmationTransition transition,
    const uint32_t nowMs,
    runtime::RuntimeHealthFact& fact) {
    if (transition != modules::StrongSourceConfirmationTransition::Gained) {
        return false;
    }

    fact = runtime::RuntimeHealthFact{};
    fact.tMs = nowMs;
    fact.kind = runtime::RuntimeHealthFactKind::PowerSource;
    fact.source = runtime::RuntimeHealthSource::Stable;
    return true;
}

constexpr uint16_t EffectiveLedHeadroomMa(
    const modules::LedPowerEnvelope& envelope) {
    const uint16_t totalHeadroom =
        envelope.maxTotalCurrentMa > envelope.fixedSystemCurrentMa
            ? static_cast<uint16_t>(envelope.maxTotalCurrentMa -
                                    envelope.fixedSystemCurrentMa)
            : 0U;
    return envelope.maxLedCurrentMa < totalHeadroom
               ? envelope.maxLedCurrentMa
               : totalHeadroom;
}

constexpr uint8_t EffectiveS3LedHeadroomPercent(
    const modules::LedPowerEnvelope& envelope) {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(EffectiveLedHeadroomMa(envelope)) * 100U) /
        modules::S3_MAX_LED_CURRENT_MA);
}

// Ordinary source classification and already-latched recovery limits are not
// severe-envelope evidence. The settings owner must have a valid normal policy.
inline bool MakePowerEnvelopeRuntimeHealthFact(
    const bool isS3,
    const bool normalPolicyAvailable,
    const bool protectionLatched,
    const modules::LedPowerEnvelope& envelope,
    const uint32_t nowMs,
    runtime::RuntimeHealthFact& fact) {
    if (!isS3 || !normalPolicyAvailable || protectionLatched) {
        return false;
    }

    fact = runtime::RuntimeHealthFact{};
    fact.tMs = nowMs;
    fact.kind = runtime::RuntimeHealthFactKind::PowerEnvelope;
    fact.powerEnvelopePercent = EffectiveS3LedHeadroomPercent(envelope);
    return true;
}

inline bool MakeBrownoutRuntimeHealthFact(
    const bool brownoutLatched,
    const uint32_t nowMs,
    runtime::RuntimeHealthFact& fact) {
    if (!brownoutLatched) {
        return false;
    }

    fact = runtime::RuntimeHealthFact{};
    fact.tMs = nowMs;
    fact.kind = runtime::RuntimeHealthFactKind::Brownout;
    fact.brownoutDetected = true;
    return true;
}

// Used by the firmware boot owner and native boot/reboot fault injection. One
// confirmed reset is sufficient, and a retained latch republishes immediately.
template <typename PersistLatch, typename PublishFact>
void RestorePowerProtectionAfterBoot(
    const bool confirmedBrownoutReset,
    const bool storedLatch,
    const uint32_t nowMs,
    PersistLatch persistLatch,
    PublishFact publishFact) {
    if (confirmedBrownoutReset) persistLatch();
    runtime::RuntimeHealthFact fact;
    if (MakeBrownoutRuntimeHealthFact(
            confirmedBrownoutReset || storedLatch, nowMs, fact)) {
        publishFact(fact);
    }
}

// Source qualification has one owner: the same three guarded full-pair samples
// used by diagnostics. Unavailable safety settings retain the known fact even
// when source diagnostics qualify. Weak/unknown CC cannot clear or set protection.
template <typename ClearStoredLatch, typename PublishFact>
bool ClearPowerProtectionForConfirmedSource(
    const modules::StrongSourceConfirmationTransition transition,
    const bool storedLatch,
    const bool safetySettingsAvailable,
    const uint32_t nowMs,
    ClearStoredLatch clearStoredLatch,
    PublishFact publishFact) {
    if (!safetySettingsAvailable ||
        transition != modules::StrongSourceConfirmationTransition::Gained ||
        !storedLatch) {
        return false;
    }
    clearStoredLatch();
    runtime::RuntimeHealthFact reset;
    reset.tMs = nowMs;
    reset.kind = runtime::RuntimeHealthFactKind::ResetLatchedProtection;
    publishFact(reset);
    return true;
}

// The existing explicit action resets protection only after this validation boundary.
// On S3 it is a newly completed full CC pair; C3 has the fixed policy boundary.
constexpr bool CanClearPowerProtection(
    const bool isS3,
    const bool freshCompleteS3Sample) {
    return !isS3 || freshCompleteS3Sample;
}

// Shared orchestration used by the real firmware owner and native fault
// injection. Hardware sampling and persistence stay behind the supplied
// production operations; this helper owns only their safety-critical order.
template <typename ResetAndRefreshSource,
          typename RecomputeSafeEnvelope,
          typename ClearStoredLatch,
          typename PublishAuthoritativeReset>
bool RunValidatedPowerProtectionClear(
    const bool isS3,
    ResetAndRefreshSource resetAndRefreshSource,
    RecomputeSafeEnvelope recomputeSafeEnvelope,
    ClearStoredLatch clearStoredLatch,
    PublishAuthoritativeReset publishAuthoritativeReset) {
    const bool freshCompleteS3Sample = resetAndRefreshSource();
    if (!CanClearPowerProtection(isS3, freshCompleteS3Sample)) {
        return false;
    }

    recomputeSafeEnvelope();
    clearStoredLatch();
    recomputeSafeEnvelope();
    publishAuthoritativeReset();
    return true;
}

} // namespace firmware
