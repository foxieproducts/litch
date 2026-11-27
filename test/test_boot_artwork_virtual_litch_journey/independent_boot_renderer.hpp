#include "firmware/led_animation_renderer.hpp"
#include "hardware/neopixel_led_renderer.hpp"
#include "../../src/virtual_litch_session_internal.hpp"

namespace {
// Run the two firmware owners separately. Only the physical RGB strip and
// time are simulated. Confirmed frames feed the next control capture.
void IndependentBootRenderer(bool craft, bool hasArtwork, bool delayed, bool firstPageFails = false) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    stream::FrameStream pager;
    pager.BeginInitialRecovery();
    const auto recover = [&]() {
        if (hasArtwork) SeedBootPages(frames, pager, craft, !firstPageFails);
        medium.failReads = firstPageFails;
        pager.CompleteInitialRecovery();
    };
    if (!delayed) recover();

    modules::LitchcraftSession art;
    art.AttachFrameStream(&pager);
    art.SetIndependentPresentation(true);
    modules::BootDisplay boot;
    boot.Begin(modules::BootDisplayChoice::StoredArtwork, true, {});
    const auto profile = hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3);
    hardware::NeoPixelLedRenderer strip(256U, 47U, 44U, 33U, true);
    runtime::virtual_litch_internal::CaptureLedRenderer pixels(strip.GetLedCount(), false);
    strip.SetNativeRgbOutput(pixels);
    firmware::LedAnimationRenderer renderer;
    renderer.Bind(strip, profile, {&pager, nullptr, nullptr, true});
    firmware::GameLedPresentation game{};
    renderer.InitializeGameBackground(game);
    renderer.InitializeBoot(boot);
    firmware::LedAnimationFrameResult result;
    modules::SettingsStore settings;
    std::vector<unsigned> shown;
    bool sawCrossfade = false;
    for (uint32_t now = 0U; now <= 1200U; now += 20U) {
        fxe::ManualMillis::SetNowMs(now);
        if (delayed && now == 100U) recover();
        Service(pager, frames);
        art.Update();
        boot.ObserveBoard(!art.IsInitialStoreModePending(), 0U, !art.IsActive());
        const bool unavailable = !hasArtwork && !art.IsInitialStoreModePending();
        if (boot.NeedsArtwork() && !unavailable && !art.IsBootArtworkPlaying())
            art.BeginBootArtworkOnce();
        if (boot.IsComplete()) art.CancelBootArtwork();

        firmware::LedAnimationPresentation capture{};
        capture.sequence = now / 20U + 1U;
        capture.capturedAtMs = now;
        capture.scene = game;
        capture.settings = modules::led_frame_composer::CaptureComposeSettings(settings);
        capture.startupPieceAccent = true;
        capture.startupBoardObserved = true;
        capture.initialStoreModePending = art.IsInitialStoreModePending();
        capture.artworkModeGeneration = art.ModeGeneration();
        capture.artworkCanvasGeneration = art.CanvasGeneration();
        capture.artworkUnavailable = unavailable;
        capture.output.autoBrightness = false;
        capture.output.brightnessLevel = 8U;
        capture.output.canvasVisible = art.IsCanvasVisible();
        if (art.IsCanvasVisible() || art.IsBootArtworkPlaying())
            art.CapturePresentation(capture.scene.emplace<modules::litchcraft_presentation::Inputs>());
        renderer.Render(capture, result);
        Check(result.receipt.completed, "independent boot transfer completes");
        renderer.NoteArtworkPresented(result.receipt.artwork);
        boot.SynchronizePresentation(result.receipt.boot);
        art.ApplyPresentationFeedback(result.receipt.artwork);
        const auto pixel = pixels.Pixels()[0];
        if (boot.UsesArtwork() && result.receipt.artwork.ready &&
            pixel.r > 0U && pixel.g == 0U && pixel.b == 0U) {
            const auto frame = result.receipt.artwork.playback.selectedFrame;
            if (shown.empty() || shown.back() != frame) shown.push_back(frame);
        }
        sawCrossfade |= boot.IsArtworkCrossfading();
    }
    if (hasArtwork && !firstPageFails) {
        Check(shown == std::vector<unsigned>{0U, 1U, 2U},
              "independent output visibly presents each boot frame once in order");
        Check(sawCrossfade, "independent output fades from the final artwork frame");
    } else Check(!sawCrossfade && shown.empty(), "unavailable artwork skips the cosmetic episode");
    Check(result.receipt.startupComplete && boot.IsComplete(),
          "confirmed independent output releases startup without a button press");
    Check(art.IsActive() == craft && !art.IsBootArtworkPlaying() && !art.IsAnimationPlaying(),
          "independent boot restores the original mode and paused playback");
}
} // namespace
