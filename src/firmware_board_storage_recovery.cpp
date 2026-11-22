#include "firmware/board_storage_recovery.hpp"

#ifdef ARDUINO
#include <array>
#include "firmware/firmware_internal.hpp"
#include "modules/board_storage_recovery.hpp"

namespace firmware_internal {
namespace {
using Status = fxe::Console::StorageReformatStatus;
using State = firmware::SettingsPersistenceWorker::StorageReformatState;
using Result = modules::ControlCoordinatorResult;
bool Blocked(FirmwareSystem& sys) {
    // Preserve unresolved OTA authority; erasing it could make a failed
    // record load look like an absent record at the boot-health boundary.
    return sys.safeModeActive || sys.ble.IsOtaInProgress() ||
        (sys.otaBootAwaitingHealth && !sys.ble.IsOtaRuntimeAccepted()) ||
        sys.hardwareTest.IsActive() || sys.amptest.IsActive() ||
        sys.amptestVerify.IsActive() ||
        sys.sleep.GetState() != firmware::SleepState::Awake ||
        sys.rebootPersistencePending ||
        sys.gamePersistence.Litchcraft().IsActive() ||
        sys.gamePersistence.ArtPersistence().FlushState() ==
            modules::LitchcraftPersistence::FlushStatus::Pending;
}
Result ActionResult(const Status status) {
    switch (status) {
        case Status::Pending:
        case Status::Running: return Result::Pending;
        case Status::Succeeded: return Result::Applied;
        case Status::Unavailable:
        case Status::Failed: return Result::StorageUnavailable;
        case Status::Idle:
        case Status::Busy: return Result::Busy;
    }
    return Result::StorageUnavailable;
}
} // namespace

Status BoardStorageRecoveryStatus(FirmwareSystem& sys) {
    if (!sys.settingsPersistence.IsStarted()) return Status::Unavailable;
    switch (sys.settingsPersistence.GetStorageReformatState()) {
        case State::Pending: return Status::Pending;
        case State::Running: return Status::Running;
        case State::Succeeded: return Status::Succeeded;
        case State::Failed: return Status::Failed;
        case State::Idle: break;
    }
    if (!sys.settings.HasConfirmedStorageFailure()) return Status::Unavailable;
    return Blocked(sys) || !sys.settingsPersistence.IsIdle()
        ? Status::Busy : Status::Idle;
}

Status ConfirmBoardStorageRecovery(FirmwareSystem& sys) {
    Status current = BoardStorageRecoveryStatus(sys);
    if (current == Status::Failed && !Blocked(sys) &&
        sys.settingsPersistence.IsIdle() &&
        sys.settingsPersistence.ReleaseStorageReformatResult()) {
        // A new explicit confirmation may retry a failed recovery. Replayed
        // Ordered requests retain their original receipt in the session owner.
        current = BoardStorageRecoveryStatus(sys);
    }
    if (current != Status::Idle) return current;
    StaticJsonDocument<1024U> safe;
    if (!modules::BuildBoardStorageRecoverySnapshot(sys.settings.Doc(), safe)) {
        return Status::Failed;
    }
    // No RAM mutation or default reseeding: the existing live game keeps going.
    // Suspend new ordinary saves before reserving the exact recovery snapshot.
    sys.settings.DisablePersistenceForSession();
    sys.controlMutation.AllowVolatileUserSettings();
    if (!sys.settingsPersistence.RequestStorageReformat(safe)) return Status::Busy;
    if (++sys.storageRecoveryPublication.attempt == 0U) {
        sys.storageRecoveryPublication.attempt = 1U;
    }
    return Status::Pending;
}

Result RequestBoardStorageRecovery(void* context, uint16_t) {
    return ActionResult(ConfirmBoardStorageRecovery(
        *static_cast<FirmwareSystem*>(context)));
}
Result PollBoardStorageRecovery(void* context, uint16_t) {
    return ActionResult(BoardStorageRecoveryStatus(
        *static_cast<FirmwareSystem*>(context)));
}
bool BoardStorageRecoveryBlocksOta(void* context) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    const auto state = sys.settingsPersistence.GetStorageReformatState();
    return state == State::Pending || state == State::Running
#if defined(LITCH_ESP32S3) || defined(LITCH_REBOOT_TEST_STORE)
        || sys.settingsPersistence.LitchcraftFrameClearPending()
#endif
        ;
}
void PublishBoardStorageRecovery(FirmwareSystem& sys) {
    auto& publication = sys.storageRecoveryPublication;
    const auto status = BoardStorageRecoveryStatus(sys);
    const std::array<uint8_t, 3U> body{
        1U, static_cast<uint8_t>(status),
        static_cast<uint8_t>(status == Status::Succeeded ? 1U : 0U)};
    if (body != publication.body || publication.revision == 0U ||
        publication.attempt != publication.publishedAttempt) {
        if (++publication.revision == 0U) publication.revision = 1U;
        publication.body = body;
        publication.publishedAttempt = publication.attempt;
    }
    // Retained truth is republished after session reconnect, with query/repair
    // owned by the shared State stream. No lossy Console result or retry timer.
    (void)sys.ble.PublishConnectionState(
        38U, publication.revision, body.data(), body.size());
}
} // namespace firmware_internal
#endif
