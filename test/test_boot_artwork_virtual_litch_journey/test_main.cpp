#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/boot_display.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/settings_registry.hpp"
#include "modules/settings_state_wire_codec.hpp"

namespace {
namespace store = modules::litchcraft_frame_store;
namespace stream = modules::litchcraft_frame_stream;
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::printf("[BOOT_ART_FAIL] %s\n", message);
    }
}
struct Medium {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY, 0xffU);
    bool failReads = false;
    bool corruptReads = false;
    static bool Read(void* ctx, size_t at, uint8_t* out, size_t size) {
        auto& m = *static_cast<Medium*>(ctx);
        if (m.failReads)
            return false;
        if (at > m.bytes.size() || size > m.bytes.size() - at)
            return false;
        std::memcpy(out, m.bytes.data() + at, size);
        if (m.corruptReads && size != 0U)
            out[0U] ^= 1U;
        return true;
    }
    static bool Write(void* ctx, size_t at, const uint8_t* in, size_t size) {
        auto& m = *static_cast<Medium*>(ctx);
        if (at > m.bytes.size() || size > m.bytes.size() - at)
            return false;
        std::memcpy(m.bytes.data() + at, in, size);
        return true;
    }
    static bool Sync(void*) { return true; }
    store::FrameStore::Ops Ops() { return {this, &Read, &Write, &Sync}; }
};
void Service(stream::FrameStream& pager, store::FrameStore& frames) {
    for (unsigned i = 0; i < 20; ++i) {
        const auto result = pager.Service(frames, false);
        if (result != stream::FrameStream::ServiceResult::Loading && result != stream::FrameStream::ServiceResult::LoadedRequested && result != stream::FrameStream::ServiceResult::PrefetchedNext)
            break;
    }
}
void SavedArtworkOnce(uint16_t count, bool craft, bool skip, bool playing = false, bool skipFade = false) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    Check(frames.Recover() == store::Status::NoArtwork, "empty Store starts through production recovery");
    store::Manifest manifest{};
    manifest.frameCount = count;
    manifest.active = craft;
    manifest.playbackOnEntry = playing;
    manifest.sparkleSeed = 1U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok, "saved artwork begins normal durable transaction");
    for (uint16_t i = 0; i < count; ++i) {
        store::Frame frame{};
        frame.durationMs = uint16_t(50U + (i % 3U) * 20U);
        for (auto& emitter : frame.emitters)
            emitter.color = {uint8_t(20U + i % 180U), uint8_t(80U + i % 120U), 8U};
        Check(frames.WriteFrame(token, i, frame) == store::Status::Ok, "all authored frames saved through real Store");
    }
    Check(frames.Commit(token) == store::Status::Ok, "complete saved root publishes");
    const auto saved = medium.bytes;
    const auto generation = frames.Generation();
    stream::FrameStream pager;
    Check(pager.Configure(frames.CurrentManifest(), generation), "existing pager adopts saved root");
    Service(pager, frames);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.strong_power_source = 1U;
    config.max_led_current_ma = 12000U;
    config.brightness_scale_256 = 256U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    auto& art = board.Litchcraft();
    art.AttachFrameStream(&pager);
    board.SettingsForPersistenceTest().Doc()["BOOT_DISPLAY"] = "artwork";
    board.ObserveBootHallMask(0ULL);
    board.Advance(0U);
    if (playing) {
        const uint8_t play[] = {1U, 0U, 1U, 0U, 0U, 0U};
        if (craft)
            Check(art.ApplyConnectionOrdered(2U, play, sizeof(play)) == modules::LitchcraftSession::Status::Ok, "normal Play preference established before boot seam");
    }
    const auto normalPixel = board.PhysicalFrame().pixels[0U].r;
    const bool resume = art.IsAnimationPlaying();
    const uint8_t selected = art.SelectedFrame();
    const auto revision = art.CanvasGeneration();
    const bool controlledOld = std::getenv("LITCH_BOOT_REPRO_OLD") != nullptr;
    if (!controlledOld)
        board.BeginBootDisplay();
    Check(board.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK, "real empty Hall admits stored boot presentation");
    Check(art.IsBootArtworkPlaying() && art.SelectedFrame() == 0U, "stored intro starts first frame without entering another mode");
    const auto firstPixel = board.PhysicalFrame().pixels[0U];
    Check(firstPixel.r == 5U && firstPixel.g == 20U && firstPixel.b == 2U,
          "first saved RGB frame reaches actual V3.3 gain and canvas scaling");
    if (controlledOld)
        return; // Equivalent missing boot-admission seam; expectations stay intact.
    uint16_t visited = 0U;
    for (uint16_t i = 0; i < count; ++i) {
        Check(art.SelectedFrame() == i && art.IsBootArtworkPlaying(), "each authored frame reaches actual rendering in order");
        Check(board.PhysicalFrame().pixels[0U].r != 0U, "stored pixels reach mapped/current-limited RGB output");
        ++visited;
        if (skip && i == 0U) {
            modules::ControlMutationOutcome ack{};
            Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P2, runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK && board.Advance(25U) == VIRTUAL_LITCH_OK, "raw X skips through production debounce");
            board.SetButtonDown(VIRTUAL_LITCH_OWNER_P2, runtime::VirtualLitchPhysicalButton::X, false);
            board.Advance(25U);
            Check(!board.TryGetLastBoardControlOutcome(ack), "skip gesture has no Undo/Redo/new-game acknowledgement");
            break;
        }
        const uint16_t dwell = uint16_t(50U + (i % 3U) * 20U);
        Check(board.Advance(dwell - 1U) == VIRTUAL_LITCH_OK && art.SelectedFrame() == i, "full authored dwell remains on current frame");
        Service(pager, frames);
        Check(board.Advance(1U) == VIRTUAL_LITCH_OK, "timed output reaches authored boundary");
        Service(pager, frames);
        board.Advance(0U);
    }
    Check(!art.IsBootArtworkPlaying(), "one pass ends instead of wrapping frame255 to zero");
    Check(art.IsActive() == craft && art.IsAnimationPlaying() == resume && art.SelectedFrame() == selected,
          "saved mode, selection and playback preference restore at the final dwell boundary");
    if (!skip) {
        const auto finalPixel = board.PhysicalFrame().pixels[0U].r;
        board.Advance(125U);
        const auto midpoint = board.PhysicalFrame().pixels[0U].r;
        if (!craft && count > 1U)
            Check(midpoint > std::min(finalPixel, normalPixel) && midpoint < std::max(finalPixel, normalPixel),
                  "final stored frame crossfades into fresh chess output at midpoint");
        if (skipFade) {
            modules::ControlMutationOutcome ack{};
            board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
            board.Advance(25U);
            board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
            board.Advance(25U);
            Check(!board.TryGetLastBoardControlOutcome(ack), "raw X during final fade skips without a normal control acknowledgement");
        } else
            board.Advance(125U);
        if (!craft)
            Check(board.PhysicalFrame().pixels[0U].r == normalPixel,
                  "250 ms crossfade expires into restored chessboard");
    }
    Check(art.IsActive() == craft && art.IsAnimationPlaying() == resume, "saved mode and normal playback preference remain restored while normal time advances");
    Check(art.CanvasGeneration() == revision && frames.Generation() == generation && medium.bytes == saved, "cosmetic playback never rewrites artwork, mode or preference bytes");
    std::printf("[StoredArtworkBootOnceV33] count=%u visited=%u skip=%u fade_skip=%u restored=%s saved_bytes=unchanged next=normal-mode bypass=SDK/radio/electrical\n", count, visited, skip, skipFade, craft ? "Litchcraft" : "Play");
}
void SettingContract() {
    modules::SettingsStore settings;
    modules::settings_state_wire::Payload before{}, after{};
    Check(modules::settings_state_wire::Encode(settings, before), "deployed Settings snapshot encodes");
    for (uint8_t choice = 0U; choice < 3U; ++choice) {
        settings.Doc()["BOOT_DISPLAY"] = choice == 0U ? "fox" : choice == 1U ? "artwork"
                                                                             : "off";
        const auto encoded = modules::settings_state_wire::EncodeBootDisplay(settings);
        Check(encoded.size() == 6U && encoded[0U] == 1U && encoded[5U] == choice,
              "State44 strictly encodes all three startup choices");
        Check(modules::settings_state_wire::Encode(settings, after) && before == after,
              "startup extension preserves every deployed 84-byte Settings field");
    }
    const auto* descriptor = modules::settings_registry::LookupByKey("BOOT_DISPLAY");
    Check(descriptor && descriptor->allowedCount == 3U && std::strcmp(descriptor->defaultStr, "fox") == 0, "Fox/default Stored artwork/Off metadata complete");
    for (uint16_t i = 0; i < 3U; ++i) {
        const char* key = nullptr;
        StaticJsonDocument<64> value;
        JsonVariantConst out;
        Check(modules::settings_state_wire::DecodeMutation(47U, i, key, value, out) && std::strcmp(key, "BOOT_DISPLAY") == 0, "typed append-only index47 decodes each choice");
    }
    const char* key = nullptr;
    StaticJsonDocument<64> value;
    JsonVariantConst out;
    Check(!modules::settings_state_wire::DecodeMutation(47U, 3U, key, value, out), "unknown choice rejected at firmware boundary");
}
} // namespace
#include "boot_artwork_delivery_journeys.hpp"
#include "independent_boot_renderer.hpp"

int main() {
    SettingContract();
    for (bool delayed : {false, true}) {
        IndependentBootRenderer(true, true, delayed);
        IndependentBootRenderer(false, true, delayed);
        IndependentBootRenderer(false, false, delayed);
    }
    IndependentBootRenderer(false, true, true, true);
    if (std::getenv("LITCH_BOOT_REPRO_OLD") == nullptr) {
        StoredArtworkComposeClockBoundary();
        StoredArtworkDelayedAndDenied();
        StoredArtworkRequestedPageFailure(false);
        StoredArtworkRequestedPageFailure(true);
    }
    SavedArtworkOnce(1U, false, false);
    SavedArtworkOnce(256U, false, false);
    SavedArtworkOnce(1U, true, false);
    SavedArtworkOnce(256U, true, false, true);
    SavedArtworkOnce(256U, true, true);
    SavedArtworkOnce(256U, false, false, false, true);
    return failures ? 1 : 0;
}
