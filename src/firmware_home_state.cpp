#ifdef ARDUINO

#include <array>

#include "firmware/home_state_bridge.hpp"

#include "firmware/firmware_internal.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/home_state_snapshot.hpp"
#include "modules/home_state_wire_codec.hpp"
#include "modules/settings_state_wire_codec.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/board_state_notification.hpp"

namespace firmware_internal {
namespace {

modules::HomeAssistanceRuntimeInputs AssistanceInputs(
    const FirmwareSystem& sys) {
    return {
        .menuOverlayActive = sys.menuOverlay.IsActive(),
        .quickSettingsOverlayActive = sys.quickSettingsOverlayActive,
        .gameResetHoldActive = sys.gameResetHoldActive,
        .coachingForcedHintDisplay =
            sys.beginnerModeForcedHintDisplay,
    };
}

void PublishConnectionHomeState(
    modules::BleService& ble,
    const modules::HomeStateNotification& home) {
    modules::home_state_wire::Payload payload;
    if (!modules::home_state_wire::Encode(home, payload)) return;
    uint32_t identity = fxe::Crc32(payload.data(), payload.size());
    if (identity == 0U) identity = 1U;
    for (uint8_t fragment = 0U;
         fragment < modules::home_state_wire::FRAGMENT_COUNT;
         ++fragment) {
        const uint8_t code = static_cast<uint8_t>(
            modules::home_state_wire::FIRST_STATE_CODE + fragment);
        (void)ble.PublishConnectionState(
            code, identity,
            payload.data() +
                static_cast<size_t>(fragment) *
                    modules::home_state_wire::FRAGMENT_BYTES,
            modules::home_state_wire::FRAGMENT_BYTES);
    }
}

void PublishConnectionSettingsState(modules::BleService& ble,
                                    const modules::SettingsStore& settings) {
    // A held D-pad changes color faster than a complete Settings snapshot can
    // cross a minimum-MTU link. Keep its physical pair/assignment atomic in one
    // replaceable State, without weakening the complete snapshot's authority.
    const auto palette = modules::settings_state_wire::EncodeLivePalette(settings);
    uint32_t paletteIdentity = fxe::Crc32(palette.data(), palette.size());
    if (paletteIdentity == 0U) paletteIdentity = 1U;
    (void)ble.PublishConnectionState(
        modules::settings_state_wire::LIVE_PALETTE_STATE_CODE,
        paletteIdentity, palette.data(), palette.size());
    modules::settings_state_wire::Payload payload;
    if (!modules::settings_state_wire::Encode(settings, payload)) return;
    uint32_t identity = fxe::Crc32(payload.data(), payload.size());
    if (identity == 0U) identity = 1U;
    for (uint8_t fragment = 0U;
         fragment < modules::settings_state_wire::FRAGMENT_COUNT; ++fragment) {
        (void)ble.PublishConnectionState(
            modules::settings_state_wire::FragmentCode(fragment),
            identity,
            payload.data() + static_cast<size_t>(fragment) *
                modules::settings_state_wire::FRAGMENT_BYTES,
            modules::settings_state_wire::FragmentLength(fragment));
    }
    const auto boot = modules::settings_state_wire::EncodeBootDisplay(settings);
    uint32_t bootIdentity = fxe::Crc32(boot.data(), boot.size());
    if (bootIdentity == 0U) bootIdentity = 1U;
    (void)ble.PublishConnectionState(modules::settings_state_wire::BOOT_DISPLAY_STATE_CODE,
        bootIdentity, boot.data(), boot.size());
    const auto assistance = modules::settings_state_wire::EncodeAssistance(settings);
    uint32_t assistanceIdentity = fxe::Crc32(assistance.data(), assistance.size());
    if (assistanceIdentity == 0U) assistanceIdentity = 1U;
    (void)ble.PublishConnectionState(
        modules::settings_state_wire::ASSISTANCE_STATE_CODE,
        assistanceIdentity, assistance.data(), assistance.size());
}

} // namespace

void PublishHomeStateIfNeeded(FirmwareSystem& sys) {
    runtime::SystemContext& ctx = GetSystemContext();
    const auto home = modules::BuildHomeStateSnapshot(
        sys.stateMachine, sys.gamePersistence, sys.settings,
        sys.beginnerModeController, sys.persistenceDegradation,
        AssistanceInputs(sys));
    const auto healthSnapshot = sys.runtimeHealth.Snapshot();
    const auto progressSnapshot = runtime::GetProgressSnapshot();
    const auto healthState = modules::EncodeBoardHealthConnectionState(
        healthSnapshot, home.persistenceDegradation, progressSnapshot,
        sys.settings.IsDisabled());
    (void)sys.ble.PublishConnectionState(
        modules::ConnectionSession::RUNTIME_HEALTH_STATE_CODE,
        static_cast<uint32_t>(healthSnapshot.revision) + 1U,
        healthState.runtime.data(), healthState.runtime.size());
    uint32_t contextIdentity = fxe::Crc32(
        healthState.context.data(), healthState.context.size());
    if (contextIdentity == 0U) contextIdentity = 1U;
    (void)sys.ble.PublishConnectionState(
        modules::ConnectionSession::BOARD_HEALTH_CONTEXT_STATE_CODE,
        contextIdentity, healthState.context.data(), healthState.context.size());
    PublishConnectionSettingsState(sys.ble, sys.settings);
    uint32_t lineRevision = 0U;
    std::array<uint8_t, 2U> lineState{};
    if (sys.currentGameSync.CopyLinePublication(lineRevision, lineState)) {
        (void)sys.ble.PublishConnectionState(
            modules::ConnectionSession::CURRENT_GAME_LINE_STATE_CODE,
            lineRevision, lineState.data(), lineState.size());
    }
    // Do not consume a pending bootstrap while reversible Duck selection
    // temporarily suppresses the authoritative Home projection.
    if (!modules::ShouldPublishHomeStateNotification(
            sys.gamePersistence.GetGameVariantStateSnapshot(),
            sys.stateMachine.GetPhysicalOccupancyMask(),
            sys.gamePersistence.IsInitialDuckRetirementObservation())) {
        return;
    }
    PublishConnectionHomeState(sys.ble, home);
    if (ctx.console == nullptr) return;
    bool force = false;
    uint32_t receipt = 0U;
    if (!sys.ble.TryBeginHomeStatePublication(force, &receipt)) {
        return;
    }
    const auto health = modules::FormatBoardHealth(
        healthSnapshot, home.persistenceDegradation, progressSnapshot,
        sys.settings.IsDisabled());
    modules::HomeStateNotificationLine line;
    (void)modules::TryPrepareHomeStatePublication(
        sys.stateMachine, sys.gamePersistence, sys.settings,
        sys.beginnerModeController, sys.persistenceDegradation,
        AssistanceInputs(sys), sys.homeStatePublisher, force, line);
    modules::WriteBoardStatePublication(*ctx.console, sys.lastBoardHealthLine,
                                        {line, health, sys.settings, force, receipt});
}

} // namespace firmware_internal

#endif
