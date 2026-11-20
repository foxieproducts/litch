#ifdef ARDUINO
#include "firmware/firmware_internal.hpp"

namespace firmware_internal {
void ConfigureLitchcraftPresentationAdmission(FirmwareSystem& sys) {
    sys.gamePersistence.SetLitchcraftPresentationAdmission({&sys,
        [](void* context, const modules::LitchcraftSession::PresentationAccess access) {
            auto& system = *static_cast<FirmwareSystem*>(context);
            modules::litchcraft_presentation::Feedback shown;
            const auto kind = access == modules::LitchcraftSession::PresentationAccess::Frame
                ? firmware::ArtworkMutation::Frame : firmware::ArtworkMutation::Control;
            const uint32_t lease = system.ledAnimation.TryBeginArtworkMutation(kind, shown);
            if (lease != 0U) system.gamePersistence.Litchcraft().ApplyPresentationFeedback(shown);
            return lease;
        },
        [](void* context, const uint32_t lease) {
            auto& system = *static_cast<FirmwareSystem*>(context);
            // Publish semantic intent before reopening candidate admission.
            (void)RenderLeds();
            (void)system.ledAnimation.EndArtworkMutation(lease);
#if defined(LITCH_ESP32S3)
            const auto& artwork = system.gamePersistence.Litchcraft();
            if (artwork.IsBootArtworkPlaying())
                system.settingsPersistence.NotifyLitchcraftFramePlayback(artwork.IsAnimationPlaying());
            else system.settingsPersistence.NotifyLitchcraftFrameSession(
                artwork.IsAnimationPlaying(), artwork.SessionId(), artwork.IsActive());
#endif
        }});
}

bool BeginPhysicalLitchcraftPresentation(FirmwareSystem& sys,
    const modules::LitchcraftSession::PresentationAccess access) {
    if (!sys.gamePersistence.TryBeginLitchcraftPresentation(access)) return false;
    if (access == modules::LitchcraftSession::PresentationAccess::Frame &&
        sys.gamePersistence.Litchcraft().PresentationFramePending()) {
        sys.gamePersistence.EndLitchcraftPresentation();
        return false;
    }
    return true;
}
} // namespace firmware_internal
#endif
