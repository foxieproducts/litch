#include "firmware/cold_factory_reset.hpp"

#include "foxie_embedded/millis.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/user_input_timing.hpp"
#include "logger.hpp"

namespace firmware {
namespace {

constexpr uint32_t SAMPLE_MS = 5U;
constexpr uint32_t BLINK_HALF_PERIOD_MS = 250U;
constexpr modules::Rgb AMBER{92U, 34U, 0U};
constexpr modules::Rgb GREEN{0U, 112U, 0U};

RecoveryOwner ReadStableEntryOwner(ColdFactoryResetTarget& target) {
    const auto owner = SelectFactoryResetEntryOwner(target.ReadResetControls());
    if (owner == RecoveryOwner::None) return owner;
    const uint32_t startedMs = fxe::Millis();
    do {
        target.WaitForResetInput(SAMPLE_MS);
        if (SelectFactoryResetEntryOwner(target.ReadResetControls()) != owner) {
            return RecoveryOwner::None;
        }
    } while (fxe::Millis() - startedMs <
             hardware::user_input_timing::BUTTON_DEBOUNCE_MS);
    return owner;
}

} // namespace

ColdFactoryResetTarget::ColdFactoryResetTarget(
    RecoveryPolicy& policy, const hardware::BoardHardwareProfile& profile)
    : m_policy(policy), m_profile(profile) {}

bool ColdFactoryResetTarget::ShowResetControls(const modules::Rgb color) {
    return DeliverResetFrame(
        ComposeFactoryResetControlFrame(m_profile.revision, m_profile.hasP2Controls, color));
}

void ColdFactoryResetTarget::AcknowledgeFactoryReset() {
    if (!m_policy.IsColdFactoryReset()) return;
    for (uint8_t phase = 0U; phase < 4U; ++phase) {
        ShowResetControls(phase % 2U == 0U ? GREEN : modules::RGB_BLACK);
        WaitForResetInput(BLINK_HALF_PERIOD_MS);
    }
}

RecoveryActionFailure RunColdFactoryReset(
    const ColdFactoryResetBoot boot, ColdFactoryResetTarget& target) {
    if (!boot.coldPowerOn || boot.deepSleepWake) return RecoveryActionFailure::None;
    const auto owner = ReadStableEntryOwner(target);
    if (owner == RecoveryOwner::None) return RecoveryActionFailure::None;

    // Do not admit destructive consent until the board has delivered its prompt.
    if (!target.ShowResetControls(AMBER)) {
        logger::Warn("boot: factory reset prompt unavailable; continuing startup");
        return RecoveryActionFailure::None;
    }
    logger::Warn(
        "boot: factory reset armed owner=%s (release Down, then hold X and Circle for two seconds)",
        owner == RecoveryOwner::P1 ? "P1" : "P2");
    auto& policy = target.Policy();
    const uint32_t promptStartedMs = fxe::Millis();
    policy.BeginColdFactoryReset(promptStartedMs);
    bool shownOn = true;

    while (true) {
        const auto input = NormalizeFactoryResetInput(target.ReadResetControls(), owner);
        const uint32_t nowMs = fxe::Millis();
        const auto update = policy.Update(input, nowMs);
        if (update.snapshot.state == RecoveryState::Inactive) break;

        const bool on = ((nowMs - promptStartedMs) / BLINK_HALF_PERIOD_MS) % 2U == 0U;
        if (on != shownOn) {
            if (!target.ShowResetControls(on ? AMBER : modules::RGB_BLACK)) {
                logger::Warn("boot: factory reset prompt lost; continuing startup");
                break;
            }
            shownOn = on;
        }
        if (update.action != RecoveryAction::None) {
            const auto failure = ExecuteRecoveryActionSequence(update.action, target);
            policy.ReportActionFailure(failure, fxe::Millis());
            return failure;
        }
        target.WaitForResetInput(SAMPLE_MS);
    }

    policy.Begin(RecoveryCause::None, RecoveryBeginMode::ArmImmediately, fxe::Millis());
    target.ShowResetControls(modules::RGB_BLACK);
    return RecoveryActionFailure::None;
}

} // namespace firmware
