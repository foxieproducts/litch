#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "firmware/recovery_action.hpp"
#include "firmware/recovery_led_frame.hpp"
#include "firmware/recovery_policy.hpp"
#include "firmware/recovery_status.hpp"
#include "hardware/led_layout.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "runtime/boot_guard.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool EqualRgb(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

bool EqualFrame(const firmware::RecoveryLedFrame& lhs,
                const firmware::RecoveryLedFrame& rhs) {
    for (size_t index = 0U; index < lhs.size(); ++index) {
        if (!EqualRgb(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

size_t LitCount(const firmware::RecoveryLedFrame& frame) {
    size_t count = 0U;
    for (const modules::Rgb color : frame) {
        if (!EqualRgb(color, modules::RGB_BLACK)) {
            ++count;
        }
    }
    return count;
}

firmware::RecoveryInput NeutralInput() {
    return firmware::RecoveryInput{};
}

firmware::RecoveryInput RetryInput() {
    firmware::RecoveryInput input;
    input.retryHeld = true;
    input.controlsReleased = false;
    input.owner = firmware::RecoveryOwner::P1;
    return input;
}

firmware::RecoveryInput DestructiveInput() {
    firmware::RecoveryInput input;
    input.destructiveChordHeld = true;
    input.controlsReleased = false;
    input.owner = firmware::RecoveryOwner::P1;
    return input;
}

firmware::RecoveryPolicy BeginArmed(
    const firmware::RecoveryCause cause,
    const uint32_t nowMs = 0U) {
    firmware::RecoveryPolicy policy;
    policy.Begin(cause, firmware::RecoveryBeginMode::ArmImmediately, nowMs);
    return policy;
}

void TestCauseSelection() {
    firmware::RecoveryEvidence evidence;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::None,
          "empty evidence selects no recovery cause");

    evidence.earlyCrashDetected = true;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::EarlyCrash,
          "early crash evidence selects early-crash recovery");
    evidence.updateFailed = true;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::UpdateFailed,
          "failed update outranks early crash");
    evidence.updatePending = true;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::UpdatePending,
          "pending update outranks failed update");
    evidence.persistedStateCorrupt = true;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::PersistedStateCorrupt,
          "corrupt persisted state outranks update evidence");
    evidence.manualFactoryResetRequested = true;
    Check(firmware::SelectRecoveryCause(evidence) ==
              firmware::RecoveryCause::ManualFactoryReset,
          "explicit manual factory entry wins over automatic evidence");

    firmware::RecoveryPolicy inactive;
    inactive.Begin(firmware::RecoveryCause::None,
                   firmware::RecoveryBeginMode::ArmImmediately, 55U);
    Check(inactive.GetSnapshot().state == firmware::RecoveryState::Inactive,
          "no cause leaves the recovery policy inactive");
}

void TestRawInputNormalization() {
    firmware::RecoveryControlLevels controls;
    Check(firmware::NormalizeSafeRecoveryInput(controls).controlsReleased,
          "safe recovery recognizes a fully neutral sample");

    controls.p1O = true;
    firmware::RecoveryInput input =
        firmware::NormalizeSafeRecoveryInput(controls);
    Check(input.retryHeld && !input.destructiveChordHeld &&
              input.owner == firmware::RecoveryOwner::P1,
          "P1 O alone normalizes to a P1 retry hold");

    controls.p1X = true;
    input = firmware::NormalizeSafeRecoveryInput(controls);
    Check(!input.retryHeld && input.destructiveChordHeld &&
              input.owner == firmware::RecoveryOwner::P1,
          "P1 X+O normalizes to same-owner destructive consent");

    controls.hasP2Controls = true;
    controls.p2O = true;
    input = firmware::NormalizeSafeRecoveryInput(controls);
    Check(!input.retryHeld && !input.destructiveChordHeld &&
              input.owner == firmware::RecoveryOwner::None &&
              !input.controlsReleased,
          "mixed-player safe controls remain active but authorize nothing");

    controls = {};
    controls.p2O = true;
    input = firmware::NormalizeSafeRecoveryInput(controls);
    Check(input.controlsReleased && input.owner == firmware::RecoveryOwner::None,
          "unavailable P2 levels are ignored");

    controls = {};
    controls.p1Down = true;
    Check(firmware::SelectFactoryResetEntryOwner(controls) ==
              firmware::RecoveryOwner::P1,
          "one P1 Down selects the cold-reset owner");
    controls.hasP2Controls = true;
    controls.p2Down = true;
    Check(firmware::SelectFactoryResetEntryOwner(controls) ==
              firmware::RecoveryOwner::None,
          "two Down controls cannot select a cold-reset owner");

    controls.p2Down = false;
    controls.p1O = true;
    auto cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P1);
    Check(!cold.destructiveChordHeld && !cold.controlsReleased,
          "held Down+O is not fresh X+O consent");

    controls.p1Down = false;
    controls.p1X = true;
    cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P1);
    Check(cold.destructiveChordHeld &&
              cold.owner == firmware::RecoveryOwner::P1,
          "the arming bank's fresh X+O qualifies");

    cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P2);
    Check(!cold.destructiveChordHeld && !cold.controlsReleased,
          "the other bank cannot take ownership of a cold reset");

    controls.p2O = true;
    cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P1);
    Check(!cold.destructiveChordHeld && !cold.controlsReleased,
          "other-bank activity invalidates a cold-reset chord");
    controls.p2O = false;

    firmware::RecoveryPolicy boundary;
    boundary.BeginColdFactoryReset(0U);
    boundary.Update(NeutralInput(), 0U);
    boundary.Update(NeutralInput(), 20U);
    cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P1);
    boundary.Update(cold, 100U);
    boundary.Update(cold, 120U);
    controls.p1X = false;
    cold = firmware::NormalizeFactoryResetInput(
        controls, firmware::RecoveryOwner::P1);
    const auto released = boundary.Update(
        cold, 120U + firmware::RecoveryPolicy::COLD_RESET_HOLD_MS);
    Check(released.action == firmware::RecoveryAction::None &&
              released.snapshot.state == firmware::RecoveryState::Cancelled,
          "X release wins over cold reset at the exact boundary");
}

class RecordingRecoveryActionTarget final
    : public firmware::IRecoveryActionTarget {
  public:
    enum class Call : uint8_t {
        PrepareRetry,
        DeleteState,
        ClearGuard,
        Restart,
    };

    void PrepareForRetryReboot() override { Record(Call::PrepareRetry); }

    bool DeletePersistedState() override {
        Record(Call::DeleteState);
        return deleteSucceeds;
    }

    void ClearBootGuard() override { Record(Call::ClearGuard); }

    void Restart() override { Record(Call::Restart); }

    bool deleteSucceeds = true;
    std::array<Call, 4U> calls{};
    size_t callCount = 0U;

  private:
    void Record(const Call call) {
        if (callCount < calls.size()) {
            calls[callCount] = call;
        }
        ++callCount;
    }
};

void TestRecoveryActionOrdering() {
    using Call = RecordingRecoveryActionTarget::Call;

    RecordingRecoveryActionTarget none;
    Check(firmware::ExecuteRecoveryActionSequence(
              firmware::RecoveryAction::None, none) ==
              firmware::RecoveryActionFailure::None &&
              none.callCount == 0U,
          "no recovery action performs no mutation");

    RecordingRecoveryActionTarget retry;
    Check(firmware::ExecuteRecoveryActionSequence(
              firmware::RecoveryAction::RetryBoot, retry) ==
              firmware::RecoveryActionFailure::RestartReturned,
          "a returned retry restart is reported as failure");
    Check(retry.callCount == 2U &&
              retry.calls[0] == Call::PrepareRetry &&
              retry.calls[1] == Call::Restart,
          "retry prepares its latch then restarts without deleting state");

    RecordingRecoveryActionTarget deleteFailure;
    deleteFailure.deleteSucceeds = false;
    Check(firmware::ExecuteRecoveryActionSequence(
              firmware::RecoveryAction::FactoryReset, deleteFailure) ==
              firmware::RecoveryActionFailure::PersistedStateDeleteFailed,
          "factory reset reports a failed persisted-state deletion");
    Check(deleteFailure.callCount == 1U &&
              deleteFailure.calls[0] == Call::DeleteState,
          "delete failure cannot clear the guard or restart");

    RecordingRecoveryActionTarget factoryReset;
    Check(firmware::ExecuteRecoveryActionSequence(
              firmware::RecoveryAction::FactoryReset, factoryReset) ==
              firmware::RecoveryActionFailure::RestartReturned,
          "a returned post-reset restart is reported as failure");
    Check(factoryReset.callCount == 3U &&
              factoryReset.calls[0] == Call::DeleteState &&
              factoryReset.calls[1] == Call::ClearGuard &&
              factoryReset.calls[2] == Call::Restart,
          "factory reset deletes once before guard clear and restart");
}

void TestBootGuardRetryArmLifecycle() {
    runtime::BootGuard guard;
    Check(!guard.GetSnapshot().retryRebootArmed,
          "boot guard starts with no armed later retry");

    guard.ArmRetryReboot();
    Check(guard.GetSnapshot().retryRebootArmed,
          "arming a later retry is visible in the snapshot");

    guard.MarkBootHealthy();
    Check(guard.GetSnapshot().retryRebootArmed,
          "intentional reboot bookkeeping does not consume an armed retry");

    Check(guard.ConsumeRetryRebootArm(),
          "consuming the later retry reports the armed state");
    Check(!guard.GetSnapshot().retryRebootArmed,
          "consuming the later retry clears the volatile arm");
    Check(!guard.ConsumeRetryRebootArm(),
          "an already-consumed retry arm stays cleared");

    guard.ArmRetryReboot();
    guard.PrepareForRetryReboot();
    Check(!guard.GetSnapshot().retryRebootArmed,
          "preparing the retry reboot clears the volatile arm");

    guard.ArmRetryReboot();
    guard.Clear();
    Check(!guard.GetSnapshot().retryRebootArmed,
          "full boot-guard clear also clears the volatile retry arm");
}

void TestRecoveryStatusFormatting() {
    runtime::BootGuard::Snapshot bootGuard;
    bootGuard.safeModeActive = true;
    bootGuard.retryRebootArmed = true;
    bootGuard.bootCount = 7U;
    bootGuard.earlyCrashCount = 3U;
    bootGuard.lastResetReason = 12U;

    firmware::RecoverySnapshot recovery;
    recovery.cause = firmware::RecoveryCause::UpdateFailed;
    recovery.state = firmware::RecoveryState::DestructiveHold;
    recovery.activeOwner = firmware::RecoveryOwner::P2;
    recovery.totalProgressPermille = 875U;
    recovery.actionFailure =
        firmware::RecoveryActionFailure::PersistedStateDeleteFailed;

    const firmware::RecoveryStatusLines lines =
        firmware::ComposeRecoveryStatusLines(
            bootGuard, true, recovery);
    Check(std::string(lines.safemodeSummary.data()) ==
              "safemode: active=1 sys_flag=1 retry_reboot_armed=1",
          "status summary exposes the armed retry flag");
    Check(std::string(lines.bootGuard.data()) ==
              "safemode: boots=7 early_crashes=3 last_reset_reason=12",
          "status diagnostics expose the boot-guard counters");
    Check(std::string(lines.recovery.data()) ==
              "recovery: cause=update_failed state=destructive_hold owner=p2 progress=875/1000 pending=none failure=persisted_state_delete_failed",
          "status recovery line exposes live recovery policy and failure fields");

    recovery.cause = firmware::RecoveryCause::None;
    recovery.state = firmware::RecoveryState::Inactive;
    recovery.activeOwner = firmware::RecoveryOwner::None;
    recovery.pendingAction = firmware::RecoveryAction::RetryBoot;
    recovery.totalProgressPermille = 0U;
    recovery.actionFailure = firmware::RecoveryActionFailure::None;
    const firmware::RecoveryStatusLines inactive =
        firmware::ComposeRecoveryStatusLines(
            runtime::BootGuard::Snapshot{}, false, recovery);
    Check(std::string(inactive.recovery.data()) ==
              "recovery: cause=none state=inactive owner=none progress=0/1000 pending=retry_boot failure=none",
          "status formatting remains stable even outside active recovery");
}

void TestIdleNeverActs() {
    firmware::RecoveryPolicy policy;
    policy.Begin(firmware::RecoveryCause::EarlyCrash,
                 firmware::RecoveryBeginMode::RequireNeutralRelease, 0U);
    Check(policy.GetSnapshot().state ==
              firmware::RecoveryState::WaitForRelease,
          "safe-mode recovery waits for a neutral release");
    Check(policy.Update(NeutralInput(), 10U).snapshot.state ==
              firmware::RecoveryState::Ready,
          "neutral release arms safe-mode recovery");

    constexpr std::array<uint32_t, 5U> IDLE_DEADLINES = {
        1000U, 2500U, 10000U, 60000U, 0xF0000000U,
    };
    for (const uint32_t nowMs : IDLE_DEADLINES) {
        const firmware::RecoveryUpdate update =
            policy.Update(NeutralInput(), nowMs);
        Check(update.action == firmware::RecoveryAction::None,
              "idle recovery never emits a timed action");
        Check(update.snapshot.state == firmware::RecoveryState::Ready,
              "idle recovery remains ready after old auto-action deadlines");
    }
}

void TestRetryBoundariesAndOnceOnly() {
    firmware::RecoveryPolicy policy =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 100U);
    Check(policy.Update(RetryInput(), 100U).snapshot.state ==
              firmware::RecoveryState::RetryHold,
          "retry press begins the deliberate hold");
    Check(policy.Update(RetryInput(), 1599U).action ==
              firmware::RecoveryAction::None,
          "retry does not act one millisecond before its deadline");
    const firmware::RecoveryUpdate boundary =
        policy.Update(RetryInput(), 1600U);
    Check(boundary.action == firmware::RecoveryAction::RetryBoot,
          "retry acts at its exact hold boundary");
    Check(boundary.snapshot.state == firmware::RecoveryState::ActionPending,
          "retry enters action-pending state");
    Check(boundary.snapshot.pendingAction ==
              firmware::RecoveryAction::RetryBoot,
          "retry snapshot identifies its pending action");
    Check(boundary.snapshot.activeOwner == firmware::RecoveryOwner::P1,
          "retry snapshot retains its physical owner");
    Check(boundary.snapshot.totalProgressPermille == 1000U,
          "completed retry reports full progress");
    Check(policy.Update(RetryInput(), 100000U).action ==
              firmware::RecoveryAction::None,
          "pending retry action is emitted exactly once");

    firmware::RecoveryPolicy releasedAtBoundary =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    releasedAtBoundary.Update(RetryInput(), 0U);
    const firmware::RecoveryUpdate released =
        releasedAtBoundary.Update(NeutralInput(),
                                  firmware::RecoveryPolicy::RETRY_HOLD_MS);
    Check(released.action == firmware::RecoveryAction::None &&
              released.snapshot.state == firmware::RecoveryState::Cancelled,
          "release wins over retry action at the exact boundary");
}

void TestDestructiveBoundariesAndCancellation() {
    firmware::RecoveryPolicy policy = BeginArmed(
        firmware::RecoveryCause::PersistedStateCorrupt, 1000U);
    policy.Update(DestructiveInput(), 1000U);
    Check(policy.Update(DestructiveInput(), 5999U).snapshot.state ==
              firmware::RecoveryState::DestructiveHold,
          "destructive hold remains in consent before five seconds");
    const firmware::RecoveryUpdate cancelWindow =
        policy.Update(DestructiveInput(), 6000U);
    Check(cancelWindow.action == firmware::RecoveryAction::None &&
              cancelWindow.snapshot.state ==
                  firmware::RecoveryState::CancelWindow,
          "five-second consent enters the keep-held cancel window");
    Check(cancelWindow.snapshot.totalProgressPermille == 1000U,
          "destructive progress is full throughout the cancel window");
    Check(policy.Update(DestructiveInput(), 7999U).action ==
              firmware::RecoveryAction::None,
          "factory reset waits through the complete cancel window");
    const firmware::RecoveryUpdate boundary =
        policy.Update(DestructiveInput(), 8000U);
    Check(boundary.action == firmware::RecoveryAction::FactoryReset,
          "factory reset acts at the exact total hold boundary");
    Check(boundary.snapshot.state == firmware::RecoveryState::ActionPending,
          "factory reset enters action-pending state");
    Check(policy.Update(DestructiveInput(), 9000U).action ==
              firmware::RecoveryAction::None,
          "factory reset action is emitted exactly once");

    firmware::RecoveryPolicy releaseInWindow = BeginArmed(
        firmware::RecoveryCause::EarlyCrash, 0U);
    releaseInWindow.Update(DestructiveInput(), 0U);
    releaseInWindow.Update(
        DestructiveInput(), firmware::RecoveryPolicy::DESTRUCTIVE_HOLD_MS);
    const firmware::RecoveryUpdate cancelled = releaseInWindow.Update(
        NeutralInput(), firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS);
    Check(cancelled.action == firmware::RecoveryAction::None &&
              cancelled.snapshot.state == firmware::RecoveryState::Cancelled,
          "release cancels even at the final destructive boundary");
}

void TestSnapshotProgress() {
    firmware::RecoveryPolicy policy = BeginArmed(
        firmware::RecoveryCause::EarlyCrash, 0U);
    policy.Update(DestructiveInput(), 0U);
    uint16_t previousProgress = 0U;
    for (const uint32_t nowMs : {0U, 1000U, 2500U, 4999U, 5000U, 6500U}) {
        const firmware::RecoveryUpdate update =
            policy.Update(DestructiveInput(), nowMs);
        Check(update.action == firmware::RecoveryAction::None,
              "destructive progress emits no action before seven seconds");
        Check(update.snapshot.totalProgressPermille >= previousProgress,
              "destructive snapshot progress is monotonic");
        previousProgress = update.snapshot.totalProgressPermille;
    }
    Check(previousProgress == 1000U,
          "destructive snapshot remains full during the cancel window");
}

void TestReleaseRearmAndBounce() {
    firmware::RecoveryPolicy safeMode;
    safeMode.Begin(firmware::RecoveryCause::EarlyCrash,
                   firmware::RecoveryBeginMode::RequireNeutralRelease, 0U);
    Check(safeMode.Update(RetryInput(), 5000U).snapshot.state ==
              firmware::RecoveryState::WaitForRelease,
          "boot-held retry cannot inherit elapsed time or arm recovery");
    safeMode.Update(NeutralInput(), 5001U);
    Check(safeMode.Update(RetryInput(), 5002U).snapshot.state ==
              firmware::RecoveryState::RetryHold,
          "retry can begin only after boot-held controls release");
    const firmware::RecoveryUpdate bounced =
        safeMode.Update(NeutralInput(), 5500U);
    Check(bounced.snapshot.state == firmware::RecoveryState::Cancelled,
          "retry bounce cancels the in-progress hold");
    Check(safeMode.Update(RetryInput(), 9000U).snapshot.state ==
              firmware::RecoveryState::Cancelled,
          "cancelled gesture cannot rearm on another held input");
    Check(safeMode.Update(NeutralInput(), 9001U).snapshot.state ==
              firmware::RecoveryState::Ready,
          "cancelled gesture rearms after a complete neutral release");
    Check(safeMode.GetSnapshot().activeOwner == firmware::RecoveryOwner::None,
          "neutral rearm clears the latched gesture owner");

    firmware::RecoveryPolicy upgraded =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    upgraded.Update(RetryInput(), 0U);
    firmware::RecoveryInput completedChord = DestructiveInput();
    completedChord.retryHeld = true;
    const firmware::RecoveryUpdate upgradedHold =
        upgraded.Update(completedChord, 1400U);
    Check(upgradedHold.snapshot.state ==
              firmware::RecoveryState::DestructiveHold,
          "adding the same-owner chord upgrades retry to destructive consent");
    Check(upgradedHold.snapshot.totalProgressPermille == 0U,
          "upgraded destructive consent resets prior retry progress");
    Check(upgraded.Update(completedChord, 6399U).snapshot.state ==
              firmware::RecoveryState::DestructiveHold,
          "upgraded destructive consent starts a fresh hold timer");
    Check(upgraded.Update(completedChord, 6400U).snapshot.state ==
              firmware::RecoveryState::CancelWindow,
          "upgraded destructive consent reaches its own exact boundary");

    firmware::RecoveryPolicy manual;
    manual.BeginColdFactoryReset(100U);
    Check(manual.Update(DestructiveInput(), 3000U).snapshot.state ==
              firmware::RecoveryState::WaitForRelease,
          "a pre-held X+O never counts as fresh cold-boot consent");
    manual.Update(NeutralInput(), 3100U);
    manual.Update(NeutralInput(), 3120U);
    manual.Update(DestructiveInput(), 3200U);
    Check(manual.Update(DestructiveInput(), 3220U).snapshot.state ==
              firmware::RecoveryState::DestructiveHold,
          "fresh X+O starts the debounced cold-boot hold");
    Check(manual.Update(DestructiveInput(), 5219U).action ==
              firmware::RecoveryAction::None,
          "cold-boot consent does not act before two seconds");
    Check(manual.Update(DestructiveInput(), 5220U).action ==
              firmware::RecoveryAction::FactoryReset,
          "cold-boot consent emits one reset at two seconds");
    Check(manual.Update(DestructiveInput(), 5230U).action ==
              firmware::RecoveryAction::None,
          "holding X+O after consent does not emit another reset");

    firmware::RecoveryPolicy simultaneous =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    firmware::RecoveryInput both = DestructiveInput();
    both.retryHeld = true;
    Check(simultaneous.Update(both, 0U).snapshot.state ==
              firmware::RecoveryState::DestructiveHold,
          "a complete destructive chord wins simultaneous ready inputs");
}

void TestOwnerIsolation() {
    firmware::RecoveryPolicy invalid =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    firmware::RecoveryInput mixed = DestructiveInput();
    mixed.owner = firmware::RecoveryOwner::None;
    Check(invalid.Update(mixed, 0U).snapshot.state ==
              firmware::RecoveryState::Ready,
          "mixed-player chord cannot start destructive consent");
    Check(invalid.Update(
              mixed, firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS)
              .action == firmware::RecoveryAction::None,
          "mixed-player chord cannot accumulate destructive time");

    firmware::RecoveryPolicy transferred =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    transferred.Update(RetryInput(), 0U);
    firmware::RecoveryInput p2Retry = RetryInput();
    p2Retry.owner = firmware::RecoveryOwner::P2;
    const firmware::RecoveryUpdate cancelled =
        transferred.Update(p2Retry, 1000U);
    Check(cancelled.snapshot.state == firmware::RecoveryState::Cancelled,
          "retry owner transfer cancels the gesture");
    Check(cancelled.snapshot.activeOwner == firmware::RecoveryOwner::P1,
          "cancelled owner transfer retains the original owner until release");
    Check(transferred.Update(
              p2Retry, firmware::RecoveryPolicy::RETRY_HOLD_MS + 1000U)
              .action == firmware::RecoveryAction::None,
          "new owner cannot inherit cancelled retry progress");
    const firmware::RecoveryUpdate rearmed =
        transferred.Update(NeutralInput(), 3000U);
    Check(rearmed.snapshot.state == firmware::RecoveryState::Ready &&
              rearmed.snapshot.activeOwner == firmware::RecoveryOwner::None,
          "neutral release clears ownership after a transfer attempt");

    firmware::RecoveryPolicy destructive = BeginArmed(
        firmware::RecoveryCause::EarlyCrash, 0U);
    destructive.Update(DestructiveInput(), 0U);
    firmware::RecoveryInput p2Destructive = DestructiveInput();
    p2Destructive.owner = firmware::RecoveryOwner::P2;
    Check(destructive.Update(p2Destructive, 4999U).snapshot.state ==
              firmware::RecoveryState::Cancelled,
          "destructive owner handoff cancels before consent");
}

void TestActionFailureRequiresRelease() {
    firmware::RecoveryPolicy policy =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, 0U);
    policy.Update(RetryInput(), 0U);
    policy.Update(RetryInput(), firmware::RecoveryPolicy::RETRY_HOLD_MS);
    policy.ReportActionFailure(
        firmware::RecoveryActionFailure::RestartReturned, 1600U);
    Check(policy.GetSnapshot().state == firmware::RecoveryState::ActionFailed,
          "failed action is explicit in the snapshot");
    Check(policy.GetSnapshot().pendingAction ==
              firmware::RecoveryAction::RetryBoot,
          "failed action preserves which operation failed");
    Check(policy.GetSnapshot().actionFailure ==
              firmware::RecoveryActionFailure::RestartReturned,
          "failed action preserves why the operation failed");
    Check(policy.Update(RetryInput(), 10000U).snapshot.state ==
              firmware::RecoveryState::ActionFailed,
          "failed action cannot rearm while a control remains held");
    const firmware::RecoveryUpdate rearmed =
        policy.Update(NeutralInput(), 10001U);
    Check(rearmed.snapshot.state == firmware::RecoveryState::Ready &&
              rearmed.snapshot.pendingAction == firmware::RecoveryAction::None &&
              rearmed.snapshot.actionFailure ==
                  firmware::RecoveryActionFailure::None,
          "failed action detail clears only after a neutral release rearms");

    policy.ReportActionFailure(
        firmware::RecoveryActionFailure::PersistedStateDeleteFailed,
        10002U);
    Check(policy.GetSnapshot().state == firmware::RecoveryState::Ready &&
              policy.GetSnapshot().actionFailure ==
                  firmware::RecoveryActionFailure::None,
          "failure reports outside action-pending are ignored");
}

void TestUnsignedWraparound() {
    constexpr uint32_t START = UINT32_MAX - 500U;
    firmware::RecoveryPolicy retry =
        BeginArmed(firmware::RecoveryCause::EarlyCrash, START);
    retry.Update(RetryInput(), START);
    Check(retry.Update(RetryInput(), START + 1499U).action ==
              firmware::RecoveryAction::None,
          "retry deadline remains pending across unsigned wrap");
    Check(retry.Update(RetryInput(), START + 1500U).action ==
              firmware::RecoveryAction::RetryBoot,
          "retry deadline fires exactly across unsigned wrap");

    firmware::RecoveryPolicy destructive = BeginArmed(
        firmware::RecoveryCause::EarlyCrash, START);
    destructive.Update(DestructiveInput(), START);
    Check(destructive.Update(
              DestructiveInput(),
              START + firmware::RecoveryPolicy::DESTRUCTIVE_HOLD_MS)
              .snapshot.state == firmware::RecoveryState::CancelWindow,
          "destructive consent boundary survives unsigned wrap");
    Check(destructive.Update(
              DestructiveInput(),
              START + firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS)
              .action == firmware::RecoveryAction::FactoryReset,
          "destructive total deadline survives unsigned wrap");

    firmware::RecoveryPolicy sparseUpdates = BeginArmed(
        firmware::RecoveryCause::EarlyCrash, 0U);
    sparseUpdates.Update(DestructiveInput(), 0U);
    const firmware::RecoveryUpdate observedWindow = sparseUpdates.Update(
        DestructiveInput(),
        firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS);
    Check(observedWindow.action == firmware::RecoveryAction::None &&
              observedWindow.snapshot.state ==
                  firmware::RecoveryState::CancelWindow,
          "a sparse update must still enter the visible final window before acting");
    Check(sparseUpdates.Update(
              DestructiveInput(),
              firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS +
                  firmware::RecoveryPolicy::CANCEL_WINDOW_MS - 1U)
              .action == firmware::RecoveryAction::None,
          "a skipped final window cannot authorize deletion retroactively");
    Check(sparseUpdates.Update(
              DestructiveInput(),
              firmware::RecoveryPolicy::DESTRUCTIVE_TOTAL_MS +
                  firmware::RecoveryPolicy::CANCEL_WINDOW_MS)
              .action == firmware::RecoveryAction::FactoryReset,
          "a sparse update acts only after the complete observed final window");
}

void TestRecoveryFrames() {
    constexpr std::array<firmware::RecoveryCause, 5U> CAUSES = {
        firmware::RecoveryCause::EarlyCrash,
        firmware::RecoveryCause::PersistedStateCorrupt,
        firmware::RecoveryCause::UpdatePending,
        firmware::RecoveryCause::UpdateFailed,
        firmware::RecoveryCause::ManualFactoryReset,
    };
    std::array<firmware::RecoveryLedFrame, CAUSES.size()> frames{};
    for (size_t causeIndex = 0U; causeIndex < CAUSES.size(); ++causeIndex) {
        firmware::RecoverySnapshot snapshot;
        snapshot.cause = CAUSES[causeIndex];
        snapshot.state = firmware::RecoveryState::Ready;
        frames[causeIndex] =
            firmware::ComposeRecoveryLedFrame(snapshot, 0U);
        Check(LitCount(frames[causeIndex]) > 0U,
              "every ready recovery cause remains visibly alive");
        Check(LitCount(frames[causeIndex]) <= 8U,
              "ready recovery cause remains sparse");
    }
    for (size_t lhs = 0U; lhs < frames.size(); ++lhs) {
        for (size_t rhs = lhs + 1U; rhs < frames.size(); ++rhs) {
            Check(!EqualFrame(frames[lhs], frames[rhs]),
                  "recovery causes have distinct semantic frames");
        }
    }

    for (const uint32_t nowMs : {0U, 599U, 600U, 1199U, 1200U}) {
        for (const firmware::RecoveryCause cause : CAUSES) {
            firmware::RecoverySnapshot snapshot;
            snapshot.cause = cause;
            snapshot.state = firmware::RecoveryState::Ready;
            Check(LitCount(firmware::ComposeRecoveryLedFrame(snapshot, nowMs)) >
                      0U,
                  "ready animation never produces an all-dark frame");
        }
    }

    firmware::RecoverySnapshot wrappedPhase;
    wrappedPhase.cause = firmware::RecoveryCause::EarlyCrash;
    wrappedPhase.state = firmware::RecoveryState::Ready;
    wrappedPhase.stateEnteredMs = UINT32_MAX - 100U;
    firmware::RecoverySnapshot ordinaryPhase = wrappedPhase;
    ordinaryPhase.stateEnteredMs = 0U;
    Check(EqualFrame(
              firmware::ComposeRecoveryLedFrame(wrappedPhase, 499U),
              firmware::ComposeRecoveryLedFrame(ordinaryPhase, 600U)),
          "recovery animation phase is deterministic across unsigned wrap");

    firmware::RecoverySnapshot retry;
    retry.cause = firmware::RecoveryCause::EarlyCrash;
    retry.state = firmware::RecoveryState::RetryHold;
    retry.totalProgressPermille = 500U;
    firmware::RecoverySnapshot destructive = retry;
    destructive.state = firmware::RecoveryState::DestructiveHold;
    firmware::RecoverySnapshot cancelWindow = destructive;
    cancelWindow.state = firmware::RecoveryState::CancelWindow;
    firmware::RecoverySnapshot cancelled = destructive;
    cancelled.state = firmware::RecoveryState::Cancelled;
    firmware::RecoverySnapshot failed = destructive;
    failed.state = firmware::RecoveryState::ActionFailed;
    const firmware::RecoveryLedFrame retryFrame =
        firmware::ComposeRecoveryLedFrame(retry, 0U);
    const firmware::RecoveryLedFrame destructiveFrame =
        firmware::ComposeRecoveryLedFrame(destructive, 0U);
    const firmware::RecoveryLedFrame cancelWindowFrame =
        firmware::ComposeRecoveryLedFrame(cancelWindow, 0U);
    const firmware::RecoveryLedFrame cancelledFrame =
        firmware::ComposeRecoveryLedFrame(cancelled, 0U);
    const firmware::RecoveryLedFrame failedFrame =
        firmware::ComposeRecoveryLedFrame(failed, 0U);
    Check(!EqualFrame(retryFrame, destructiveFrame),
          "retry and destructive holds are visually distinct");
    Check(!EqualFrame(destructiveFrame, cancelWindowFrame),
          "destructive hold and cancel window are visually distinct");
    Check(!EqualFrame(cancelWindowFrame, cancelledFrame),
          "cancel window and cancelled reason frame are distinct");
    firmware::RecoverySnapshot readyAfterCancellation = cancelled;
    readyAfterCancellation.state = firmware::RecoveryState::Ready;
    readyAfterCancellation.stateEnteredMs = cancelled.stateEnteredMs;
    Check(EqualFrame(
              cancelledFrame,
              firmware::ComposeRecoveryLedFrame(
                  readyAfterCancellation, 0U)),
          "cancelled state exactly matches the ordinary reason frame");
    Check(!EqualFrame(cancelledFrame, failedFrame),
          "cancellation and action failure are visually distinct");

    size_t previousLit = 0U;
    for (const uint16_t progress :
         {0U, 100U, 250U, 500U, 750U, 999U, 1000U}) {
        firmware::RecoverySnapshot snapshot;
        snapshot.cause = firmware::RecoveryCause::ManualFactoryReset;
        snapshot.state = firmware::RecoveryState::DestructiveHold;
        snapshot.totalProgressPermille = progress;
        const size_t currentLit =
            LitCount(firmware::ComposeRecoveryLedFrame(snapshot, 0U));
        Check(currentLit >= previousLit,
              "destructive semantic fill grows monotonically");
        Check(currentLit <= 16U,
              "destructive semantic fill remains low-clutter");
        previousLit = currentLit;
    }

    firmware::RecoverySnapshot finalWindow;
    finalWindow.cause = firmware::RecoveryCause::ManualFactoryReset;
    finalWindow.state = firmware::RecoveryState::CancelWindow;
    finalWindow.totalProgressPermille = 1000U;
    Check(LitCount(firmware::ComposeRecoveryLedFrame(finalWindow, 0U)) >=
              previousLit,
          "full-progress final window does not visually regress");
}

void TestPhysicalMappings() {
    firmware::RecoverySnapshot snapshot;
    snapshot.cause = firmware::RecoveryCause::PersistedStateCorrupt;
    snapshot.state = firmware::RecoveryState::CancelWindow;
    snapshot.totalProgressPermille = 900U;
    const firmware::RecoveryLedFrame semantic =
        firmware::ComposeRecoveryLedFrame(snapshot, 0U);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> dedicated{};
    modules::status_led_frame_adapter::ApplyToDedicatedChain(
        semantic, false, dedicated);
    for (uint16_t led = 0U; led < hardware::BOARD_LED_COUNT; ++led) {
        Check(EqualRgb(dedicated[led], modules::RGB_BLACK),
              "S3 dedicated recovery mapping leaves board LEDs black");
    }
    for (uint8_t semanticIndex = 0U;
         semanticIndex < semantic.size();
         ++semanticIndex) {
        const uint16_t physicalIndex = static_cast<uint16_t>(
            hardware::FirstEdgeLedIndex() + semanticIndex);
        Check(EqualRgb(dedicated[physicalIndex], semantic[semanticIndex]),
              "S3 dedicated recovery mapping preserves every semantic pixel");
    }
    Check(LitCount(semantic) > 0U,
          "S3 dedicated recovery mapping receives a visible semantic frame");

    std::array<bool, hardware::BOARD_LED_COUNT> virtualPerimeter{};
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        const uint16_t led =
            modules::status_led_frame_adapter::
                VirtualLedIndexForPhysicalStatusIndex(index);
        virtualPerimeter[led] = true;
    }
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> c3Virtual{};
    modules::status_led_frame_adapter::ApplyToVirtualPerimeter(
        semantic, false, c3Virtual);
    for (uint8_t semanticIndex = 0U;
         semanticIndex < semantic.size();
         ++semanticIndex) {
        const uint16_t target =
            modules::status_led_frame_adapter::
                VirtualLedIndexForPhysicalStatusIndex(semanticIndex);
        const modules::Rgb expected =
            modules::status_led_frame_adapter::BlendVirtualStatus(
                modules::RGB_BLACK, semantic[semanticIndex]);
        Check(EqualRgb(c3Virtual[target], expected),
              "C3 virtual recovery mapping preserves every semantic target");
    }
    for (uint16_t led = 0U; led < hardware::BOARD_LED_COUNT; ++led) {
        if (!virtualPerimeter[led]) {
            Check(EqualRgb(c3Virtual[led], modules::RGB_BLACK),
                  "C3 virtual recovery mapping leaves non-perimeter LEDs black");
        }
    }
    for (uint16_t led = hardware::BOARD_LED_COUNT;
         led < hardware::TOTAL_LED_COUNT;
         ++led) {
        Check(EqualRgb(c3Virtual[led], modules::RGB_BLACK),
              "C3 virtual recovery mapping does not use a dedicated chain");
    }
}

class StatusRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override {
        return static_cast<uint16_t>(m_frame.size());
    }

    void Clear() override { m_frame.fill(modules::RGB_BLACK); }

    void SetLedRgb(const uint16_t ledIndex,
                   const uint8_t r,
                   const uint8_t g,
                   const uint8_t b) override {
        if (ledIndex < m_frame.size()) {
            m_frame[ledIndex] = modules::Rgb{r, g, b};
        }
    }

    void Show() override {}

  private:
    firmware::RecoveryLedFrame m_frame{};
};

void CheckFrameCurrent(const firmware::RecoverySnapshot& snapshot,
                       const uint32_t nowMs,
                       modules::BudgetAwareLedPipeline& pipeline) {
    const firmware::RecoveryLedFrame frame =
        firmware::ComposeRecoveryLedFrame(snapshot, nowMs);
    pipeline.RenderFrame(frame.data(), frame.size());
    const modules::BudgetAwareLedPipelineStats stats = pipeline.GetLastStats();
    Check(stats.estimatedLedCurrentMa <=
              modules::S3_CONSERVATIVE_LED_HEADROOM_MA,
          "requested recovery current stays under S3 conservative headroom");
    Check(stats.estimatedLedCurrentMa <=
              firmware::RECOVERY_LED_CURRENT_CAP_MA,
          "requested recovery current stays under explicit 120 mA cap");
    Check(stats.appliedLedCurrentMa <=
              modules::S3_CONSERVATIVE_LED_HEADROOM_MA,
          "applied recovery current stays under S3 conservative headroom");
    Check(stats.appliedLedCurrentMa <=
              firmware::RECOVERY_LED_CURRENT_CAP_MA,
          "applied recovery current stays under explicit 120 mA cap");
}

void TestRecoveryCurrentEnvelope() {
    StatusRenderer renderer;
    const modules::LedPowerEnvelope envelope =
        modules::SelectS3LedPowerEnvelope(true);
    modules::PowerBudgetPolicyConfig policyConfig;
    policyConfig.maxLedCurrentMa = firmware::RECOVERY_LED_CURRENT_CAP_MA;
    policyConfig.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
    policyConfig.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    modules::PowerBudgetPolicy policy(policyConfig);

    modules::BudgetAwareLedPipelineConfig pipelineConfig;
    pipelineConfig.enableRgbwAutoMapping = true;
    pipelineConfig.rgbwCoolToWarmThreshold = 32U;
    pipelineConfig.redChannelMaxCurrentMaTimes100 =
        modules::S3_RED_CURRENT_MA_TIMES100;
    pipelineConfig.greenChannelMaxCurrentMaTimes100 =
        modules::S3_GREEN_CURRENT_MA_TIMES100;
    pipelineConfig.blueChannelMaxCurrentMaTimes100 =
        modules::S3_BLUE_CURRENT_MA_TIMES100;
    pipelineConfig.whiteChannelMaxCurrentMaTimes100 =
        modules::S3_WHITE_CURRENT_MA_TIMES100;
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, pipelineConfig);

    constexpr std::array<firmware::RecoveryCause, 5U> CAUSES = {
        firmware::RecoveryCause::EarlyCrash,
        firmware::RecoveryCause::PersistedStateCorrupt,
        firmware::RecoveryCause::UpdatePending,
        firmware::RecoveryCause::UpdateFailed,
        firmware::RecoveryCause::ManualFactoryReset,
    };
    for (const firmware::RecoveryCause cause : CAUSES) {
        for (const firmware::RecoveryState state : {
                 firmware::RecoveryState::WaitForRelease,
                 firmware::RecoveryState::Ready,
                 firmware::RecoveryState::RetryHold,
                 firmware::RecoveryState::DestructiveHold,
                 firmware::RecoveryState::CancelWindow,
                 firmware::RecoveryState::Cancelled,
                 firmware::RecoveryState::ActionPending,
                 firmware::RecoveryState::ActionFailed,
             }) {
            for (const uint16_t progress : {0U, 500U, 1000U}) {
                firmware::RecoverySnapshot snapshot;
                snapshot.cause = cause;
                snapshot.state = state;
                snapshot.pendingAction =
                    (cause == firmware::RecoveryCause::ManualFactoryReset)
                        ? firmware::RecoveryAction::FactoryReset
                        : firmware::RecoveryAction::RetryBoot;
                snapshot.totalProgressPermille = progress;
                CheckFrameCurrent(snapshot, 0U, pipeline);
                CheckFrameCurrent(snapshot, 600U, pipeline);
            }
        }
    }
}

} // namespace

int main() {
    std::puts("[RECOVERY_POLICY] start");

    TestCauseSelection();
    TestRawInputNormalization();
    TestRecoveryActionOrdering();
    TestBootGuardRetryArmLifecycle();
    TestRecoveryStatusFormatting();
    TestIdleNeverActs();
    TestRetryBoundariesAndOnceOnly();
    TestDestructiveBoundariesAndCancellation();
    TestSnapshotProgress();
    TestReleaseRearmAndBounce();
    TestOwnerIsolation();
    TestActionFailureRequiresRelease();
    TestUnsignedWraparound();
    TestRecoveryFrames();
    TestPhysicalMappings();
    TestRecoveryCurrentEnvelope();

    if (g_failures == 0) {
        std::puts("[RECOVERY_POLICY] ok");
        return 0;
    }
    std::printf("[RECOVERY_POLICY] failures=%d\n", g_failures);
    return 1;
}
