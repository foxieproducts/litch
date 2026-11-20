#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/lis2dw12.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/ble_device_capabilities.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_animation_codec.hpp"
#include "modules/litchcraft_persistence.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/virtual_litch_session.h"
#include "virtual_litch_session.hpp"

namespace {

using Art = modules::LitchcraftSession;
using Button = runtime::VirtualLitchPhysicalButton;
using Persistence = modules::LitchcraftPersistence;

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

virtual_litch_config Config(const uint8_t hardware, const bool rotate = false) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    config.board_rotate = rotate ? 1U : 0U;
    return config;
}

modules::litchcraft_animation::ValidatedDocument Document(
    const uint8_t frameCount, const uint8_t topology,
    const bool playbackOnEntry = false) {
    modules::litchcraft_animation::Document document{};
    document.frameCount = frameCount;
    document.topology = topology;
    document.playbackOnEntry = playbackOnEntry;
    for (uint8_t frame = 0U; frame < frameCount; ++frame) {
        document.frames[frame].durationMs = 2000U;
        document.frames[frame].emitters[frame].color = {
            static_cast<uint8_t>(40U + frame * 10U),
            static_cast<uint8_t>(80U + frame * 10U),
            static_cast<uint8_t>(120U + frame * 10U)};
    }
    modules::litchcraft_animation::ValidatedDocument adopted;
    Check(modules::litchcraft_animation::ValidatedDocument::TryAdopt(
              document, adopted),
          "animation control fixture adopts its bounded document");
    return adopted;
}

struct DeferredAdvancedBackend {
    static Persistence::SaveOutcome Poll(void*, uint32_t) {
        return Persistence::SaveOutcome::Pending;
    }
    static void Release(void*, uint32_t) {}
    static Persistence::AdvancedReadOutcome Read(
        void*, Persistence::AdvancedSlot, uint8_t*, size_t, size_t&) {
        return Persistence::AdvancedReadOutcome::Missing;
    }
    static fxe::SettingsStore::AsyncSaveAdmission Enqueue(
        void*, const uint8_t*, size_t, uint32_t&) {
        return fxe::SettingsStore::AsyncSaveAdmission::Deferred;
    }
};

struct ControlledExitBackend {
    bool rejectAdvanced = true;
    bool invalidCurrent = false;
    std::array<uint8_t, Persistence::ADVANCED_MAX_ENVELOPE_BYTES> advanced{};
    size_t advancedSize = 0U;

    static fxe::SettingsStore::AsyncSaveAdmission EnqueueStatic(
        void*, const JsonDocument&, uint32_t& receipt) {
        receipt = 2U;
        return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
    static Persistence::SaveOutcome Poll(void*, uint32_t) {
        return Persistence::SaveOutcome::Saved;
    }
    static void Release(void*, uint32_t) {}
    static Persistence::AdvancedReadOutcome Read(
        void* context, const Persistence::AdvancedSlot slot, uint8_t* out,
        const size_t capacity, size_t& size) {
        auto& self = *static_cast<ControlledExitBackend*>(context);
        if (slot == Persistence::AdvancedSlot::Current &&
            self.invalidCurrent && self.advancedSize == 0U) {
            if (capacity == 0U) {
                return Persistence::AdvancedReadOutcome::Failed;
            }
            out[0] = 0U;
            size = 1U;
            return Persistence::AdvancedReadOutcome::Present;
        }
        if (slot != Persistence::AdvancedSlot::Current ||
            self.advancedSize == 0U) {
            return Persistence::AdvancedReadOutcome::Missing;
        }
        if (capacity < self.advancedSize) {
            return Persistence::AdvancedReadOutcome::Failed;
        }
        std::copy_n(self.advanced.begin(), self.advancedSize, out);
        size = self.advancedSize;
        return Persistence::AdvancedReadOutcome::Present;
    }
    static fxe::SettingsStore::AsyncSaveAdmission EnqueueAdvanced(
        void* context, const uint8_t* bytes, const size_t size,
        uint32_t& receipt) {
        auto& self = *static_cast<ControlledExitBackend*>(context);
        if (self.rejectAdvanced || size > self.advanced.size()) {
            return fxe::SettingsStore::AsyncSaveAdmission::Rejected;
        }
        std::copy_n(bytes, size, self.advanced.begin());
        self.advancedSize = size;
        receipt = 1U;
        return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
};

Art::Packet EnterPacket() {
    Art::Packet packet{};
    packet.bytes[0] = Art::MAGIC;
    packet.bytes[1] = static_cast<uint8_t>(Art::Op::Enter);
    packet.bytes[6] = 1U;
    packet.size = 8U;
    return packet;
}

Art::Packet LegacyRequest(const Art::Op op, const uint32_t session,
                          const uint16_t request) {
    Art::Packet packet{};
    packet.bytes[0] = Art::MAGIC;
    packet.bytes[1] = static_cast<uint8_t>(op);
    for (uint8_t i = 0U; i < 4U; ++i) {
        packet.bytes[2U + i] = static_cast<uint8_t>(session >> (i * 8U));
    }
    packet.bytes[6] = static_cast<uint8_t>(request);
    packet.bytes[7] = static_cast<uint8_t>(request >> 8U);
    packet.size = 8U;
    return packet;
}

uint8_t DrainLastStatus(Art& art) {
    Art::Packet packet;
    uint8_t status = 0xffU;
    while (art.PeekPacket(packet)) {
        if (packet.size > 12U &&
            packet.bytes[1] == static_cast<uint8_t>(Art::Event::State)) {
            status = packet.bytes[12];
        }
        art.AcceptPacket();
    }
    return status;
}

struct Fixture {
    virtual_litch_config config;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session;
    std::unique_ptr<Persistence::AdvancedStorage> storage;
    uint8_t hardware;
    bool rotate;

    Fixture(const uint8_t hardware, const uint8_t frameCount,
            const bool playbackOnEntry = false,
            const bool configureAdvanced = true,
            const bool rotate = false,
            const bool occupiedWarningBeforeEnter = false)
        : config(Config(hardware, rotate)), engine(), session(config, engine),
          storage(std::make_unique<Persistence::AdvancedStorage>()),
          hardware(hardware), rotate(rotate) {
        session.ObserveBootHallMask(0U);
        Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
              "animation control fixture reaches empty Setup");
        Art& art = session.Litchcraft();
        art.Configure(hardware, 0x641077U + hardware);
        art.BeginConnection();
        if (occupiedWarningBeforeEnter) {
            Check(session.ObserveHallMask(UINT64_C(1)) == VIRTUAL_LITCH_OK,
                  "warning fixture places one physical piece");
            Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::X,
                                        true) == VIRTUAL_LITCH_OK &&
                      session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::O,
                                            true) == VIRTUAL_LITCH_OK &&
                      session.Advance(30U) == VIRTUAL_LITCH_OK,
                  "occupied same-owner X+O raises the production warning cue");
            Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::X,
                                        false) == VIRTUAL_LITCH_OK &&
                      session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::O,
                                            false) == VIRTUAL_LITCH_OK &&
                      session.Advance(30U) == VIRTUAL_LITCH_OK,
                  "occupied same-owner X+O releases cleanly");
            Check(session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK,
                  "warning fixture clears occupancy before app entry");
        }
        const auto enter = EnterPacket();
        const uint32_t enterAt = session.State().now_ms;
        session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK && art.IsActive() &&
                  session.State().now_ms - enterAt == 1U &&
                  (hardware == 1U || std::any_of(
                      std::begin(session.SemanticFrame().status),
                      std::end(session.SemanticFrame().status),
                      [](const auto color) { return color.r || color.g || color.b; })),
              "accepted app Enter publishes active canvas/status on the next 1 ms render without storage or palette wait");
        auto& persistence = session.ArtPersistence();
        if (configureAdvanced) {
            persistence.Configure({nullptr, nullptr,
                &DeferredAdvancedBackend::Poll,
                &DeferredAdvancedBackend::Release,
                &DeferredAdvancedBackend::Read,
                &DeferredAdvancedBackend::Enqueue});
            persistence.ConfigureAdvancedStorage(*storage);
        }
        if (frameCount != 0U) {
            Check(persistence.SetAdvancedDocument(
                      Document(frameCount, hardware, playbackOnEntry), true),
                  "production persistence owner installs advanced authority");
        }
        Check(art.IsActive(), "advanced fixture remains in Litchcraft");
        (void)DrainLastStatus(art);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK,
              "advanced fixture captures its first production frame");
        if (playbackOnEntry && frameCount != 0U) {
            Check(!art.IsAnimationPlaying(),
                  "saved play-on-entry flag leaves the board paused");
            const uint8_t play[] = {1U, 0U};
            Check(art.ApplyConnectionOrdered(2U, play, sizeof(play)) ==
                      Art::Status::Ok,
                  "playing fixture starts through the explicit Play action");
        }
        session.Advance(250U);
    }

    void Set(const uint8_t owner, const Button button, const bool down,
             const uint32_t settleMs = 30U) {
        Check(session.SetButtonDown(owner, button, down) == VIRTUAL_LITCH_OK,
              "raw physical control level enters production adapter");
        Check(session.Advance(settleMs) == VIRTUAL_LITCH_OK,
              "raw physical control settles through production debounce");
    }

    void Tap(const uint8_t owner, const Button button) {
        Set(owner, button, true);
        Set(owner, button, false);
    }

    void ToggleTool(const uint8_t owner) {
        Set(owner, Button::O, true);
        Check(session.Advance(1000U) == VIRTUAL_LITCH_OK,
              "established one-second physical O hold changes Paint/Erase");
        Set(owner, Button::O, false);
    }

    void Chord(const uint8_t owner, const Button arrow,
               const uint32_t arrowHoldMs = 30U) {
        Set(owner, Button::O, true);
        Set(owner, arrow, true);
        if (arrowHoldMs > 30U) {
            Check(session.Advance(arrowHoldMs - 30U) == VIRTUAL_LITCH_OK,
                  "held chord remains in the production input loop");
        }
        Set(owner, arrow, false);
        Set(owner, Button::O, false);
    }

    void ModeChord(const uint8_t owner) {
        Check(session.SetButtonDown(owner, Button::X, true) ==
                  VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(owner, Button::O, true) ==
                  VIRTUAL_LITCH_OK &&
                  session.Advance(30U) == VIRTUAL_LITCH_OK,
              "same-owner X+O settles inside the production chord window");
        const bool activeBeforeRelease = session.Litchcraft().IsActive();
        Check(session.Advance(1470U) == VIRTUAL_LITCH_OK &&
                  session.Litchcraft().IsActive() == activeBeforeRelease,
              "mode chord completes its rainbow while mode waits for release");
        Check(session.SetButtonDown(owner, Button::X, false) ==
                  VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(owner, Button::O, false) ==
                  VIRTUAL_LITCH_OK &&
                  session.Advance(30U) == VIRTUAL_LITCH_OK,
              "same-owner X+O releases through production debounce");
    }
};

uint16_t Energy(const virtual_litch_color color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

bool SameStatusFrame(const virtual_litch_semantic_frame& left,
                     const virtual_litch_semantic_frame& right) {
    for (size_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT; ++index) {
        const auto& leftPixel = left.status[index];
        const auto& rightPixel = right.status[index];
        if (leftPixel.r != rightPixel.r || leftPixel.g != rightPixel.g ||
            leftPixel.b != rightPixel.b || leftPixel.w != rightPixel.w) {
            return false;
        }
    }
    return true;
}

size_t LitStatus(const Fixture& fixture, size_t begin, size_t end);


size_t LitStatus(const Fixture& fixture, const size_t begin,
                 const size_t end) {
    size_t count = 0U;
    for (size_t index = begin; index < end; ++index) {
        if (Energy(fixture.session.SemanticFrame().status[index]) != 0U) ++count;
    }
    return count;
}

size_t LitBoardSquares(const Fixture& fixture) {
    size_t count = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        bool lit = false;
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            lit = lit || Energy(
                fixture.session.SemanticFrame().board[square][slot]) != 0U;
        }
        if (lit) ++count;
    }
    return count;
}

uint64_t BoardSquareMask(const Fixture& fixture) {
    uint64_t mask = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if (Energy(fixture.session.SemanticFrame().board[square][slot]) !=
                0U) {
                mask |= UINT64_C(1) << square;
                break;
            }
        }
    }
    return mask;
}

uint32_t StatusMask(const Fixture& fixture) {
    uint32_t mask = 0U;
    for (uint8_t index = 0U; index < 32U; ++index) {
        if (Energy(fixture.session.SemanticFrame().status[index]) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

bool UsbWarningVisible(const Fixture& fixture, const bool boardRotate) {
    (void)boardRotate;
    const auto& physical = fixture.session.PhysicalFrame();
    const uint16_t statusStart = static_cast<uint16_t>(
        physical.led_count - modules::EDGE_LED_COUNT);
    for (const uint8_t physicalIndex :
         modules::status_led_frame_adapter::USB_PORT_PHYSICAL_INDICES) {
        if (Energy(physical.pixels[statusStart + physicalIndex]) != 0U) {
            return true;
        }
    }
    return false;
}

void Checkpoint(Fixture& fixture, const char* const stage,
                const char* const result, const char* const next) {
    const auto& art = fixture.session.Litchcraft();
    std::printf(
        "[LITCHCRAFT_ANIMATION_BOARD_JOURNEY] stage=%s input=physical "
        "occupancy=%016llx mode=%s frame=%u/%u playback=%s elapsed=%llu "
        "result=%s status_mask=%08lx board_mask=%016llx "
        "display=\"%.*s\" next=%s\n",
        stage, static_cast<unsigned long long>(art.Occupancy()),
        art.IsActive() ? "Litchcraft" : "Setup/Play",
        static_cast<unsigned>(art.SelectedFrame() + 1U),
        static_cast<unsigned>(art.FrameCount()),
        art.IsAnimationPlaying() ? "playing" : "paused",
        static_cast<unsigned long long>(art.AnimationElapsedMs()), result,
        static_cast<unsigned long>(StatusMask(fixture)),
        static_cast<unsigned long long>(BoardSquareMask(fixture)),
        static_cast<int>(fixture.session.DisplayText().size()),
        fixture.session.DisplayText().empty()
            ? "" : fixture.session.DisplayText().data(), next);
}


uint16_t Read16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0U]) |
        static_cast<uint16_t>(bytes[1U]) << 8U;
}

uint32_t Read32(const uint8_t* bytes) {
    uint32_t value = 0U;
    for (uint8_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<uint32_t>(bytes[byte]) << (byte * 8U);
    }
    return value;
}







void RunSharedWarningPreemption() {
    modules::SettingsStore settings;
    settings.DisableForBoot();
    settings.ReportAsyncSaveFailure(modules::SettingsStoreError::WriteFailed,
        modules::SettingsStore::StorageFailureEvidence::Confirmed);
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> shared{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> litchcraft{};
    shared[27U] = {9U, 8U, 7U};
    shared[28U] = {6U, 5U, 4U};
    litchcraft.fill({1U, 2U, 3U});
    modules::led_frame_composer::RestoreLitchcraftStatusWarnings(
        inputs, shared, litchcraft);
    Check(litchcraft[27U].r == 9U && litchcraft[27U].g == 8U &&
              litchcraft[28U].r == 6U && litchcraft[28U].g == 5U &&
              litchcraft[26U].r == 1U && litchcraft[29U].r == 1U,
          "storage degradation restores exact shared USB warning pixels only");

    for (uint8_t index = 0U; index < shared.size(); ++index) {
        shared[index] = {index, static_cast<uint8_t>(index + 1U), 0U};
        litchcraft[index] = {3U, 2U, 1U};
    }
    inputs.edgeCueIntent.kind = modules::EdgeLedCueKind::Unavailable;
    modules::led_frame_composer::RestoreLitchcraftStatusWarnings(
        inputs, shared, litchcraft);
    Check(std::equal(shared.begin(), shared.end(), litchcraft.begin(),
                     [](const modules::Rgb left, const modules::Rgb right) {
                         return left.r == right.r && left.g == right.g &&
                                left.b == right.b;
                     }),
          "active Unavailable warning restores the exact shared status frame");
    litchcraft.fill({3U, 2U, 1U});
    inputs.edgeCueIntent.kind = modules::EdgeLedCueKind::SetupComplete;
    modules::led_frame_composer::RestoreLitchcraftStatusWarnings(
        inputs, shared, litchcraft);
    Check(litchcraft[0U].r == 3U && litchcraft[31U].r == 3U,
          "SetupComplete remains below active Litchcraft status ownership");

    Fixture warning(3U, 2U, true, true, false, true);
    Art& art = warning.session.Litchcraft();
    Check(LitStatus(warning, 0U, 32U) == 32U,
          "occupied-board warning preempts the complete Litchcraft HUD");
    const uint64_t warningElapsed = art.AnimationElapsedMs();
    Check(art.IsAnimationPlaying() && LitStatus(warning, 0U, 32U) == 32U,
          "warning remains visible while non-pausing animation authority runs");
    Checkpoint(warning, "occupied-warning-preemption", "warning-visible",
               "wait-for-expiry");
    warning.session.Advance(
        modules::EdgeLedCueController::OCCUPIED_BOARD_BLOCKED_DURATION_MS);
    Check(art.IsAnimationPlaying() &&
              art.AnimationElapsedMs() >= warningElapsed +
                  modules::EdgeLedCueController::
                      OCCUPIED_BOARD_BLOCKED_DURATION_MS &&
              LitStatus(warning, 0U, 32U) < 32U,
          "warning expiry restores current HUD without rewinding playback");
    Checkpoint(warning, "occupied-warning-expiry", "restored-current-HUD",
               "continue-animation");

    Fixture degraded(3U, 2U, true);
    const uint32_t now = degraded.session.State().now_ms;
    const uint32_t toWarningPhase = (5000U - now % 5000U) % 5000U;
    if (toWarningPhase != 0U) degraded.session.Advance(toWarningPhase);
    const uint64_t degradedElapsed =
        degraded.session.Litchcraft().AnimationElapsedMs();
    virtual_litch_runtime_health_fact fact{};
    virtual_litch_default_runtime_health_fact(&fact);
    fact.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_AMBIENT_SENSOR_COUNT;
    fact.ambient_sensor_count = 1U;
    Check(degraded.session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK,
          "DegradedUsable fact enters the existing production health path");
    Check(UsbWarningVisible(degraded, false) &&
              degraded.session.Litchcraft().IsAnimationPlaying() &&
              degraded.session.Litchcraft().AnimationElapsedMs() ==
                  degradedElapsed,
          "DegradedUsable warning preempts its USB pixel without pausing or rewinding");
    Checkpoint(degraded, "degraded-usable-preemption", "warning-visible",
               "show-Identify");
    degraded.session.ShowIdentifyOverlay(7U, {55U, 215U, 105U});
    Check(degraded.session.DisplayText() == "7" &&
              UsbWarningVisible(degraded, false),
          "DegradedUsable USB warning wins while Identify remains readable elsewhere");
    degraded.session.Advance(250U);
    degraded.session.StopIdentifyOverlay();
    const uint64_t afterIdentifyElapsed =
        degraded.session.Litchcraft().AnimationElapsedMs();
    Check(degraded.session.DisplayText().empty() &&
              UsbWarningVisible(degraded, false) &&
              afterIdentifyElapsed >= degradedElapsed + 250U,
          "Identify expiry restores animation under the still-active health warning");
    Checkpoint(degraded, "health-plus-identify-expiry",
               "health-wins-identify-expired", "publish-healthy-recovery");
    fact.ambient_sensor_count = 2U;
    Check(degraded.session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK &&
              !UsbWarningVisible(degraded, false) &&
              degraded.session.Litchcraft().AnimationElapsedMs() ==
                  afterIdentifyElapsed,
          "health recovery restores the current HUD at the same playback clock");
    degraded.session.Advance(100U);
    Check(degraded.session.Litchcraft().AnimationElapsedMs() >=
              afterIdentifyElapsed + 100U,
          "playback continues forward after warning recovery");
    Checkpoint(degraded, "degraded-usable-recovery",
               "restored-without-rewind", "continue-animation");

    Fixture paused(3U, 2U);
    paused.session.SettingsForPersistenceTest().DisableForBoot();
    virtual_litch_runtime_health_fact uncertain{};
    virtual_litch_default_runtime_health_fact(&uncertain);
    uncertain.kind = VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;
    uncertain.board_scan =
        VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
    Check(paused.session.DispatchRuntimeHealthFact(uncertain) ==
                  VIRTUAL_LITCH_OK &&
              paused.session.Advance(100U) == VIRTUAL_LITCH_OK &&
              paused.session.DispatchRuntimeHealthFact(uncertain) ==
                  VIRTUAL_LITCH_OK,
          "sustained uncertain scans enter paused health with persistence degraded");
    const virtual_litch_semantic_frame severeFrame =
        paused.session.SemanticFrame();
    Check(StatusMask(paused) == UINT32_C(0x33333333),
          "paused health owns the exact severe dashed status frame");
    paused.session.ShowIdentifyOverlay(7U, {55U, 215U, 105U});
    Check(paused.session.DisplayText() == "7" &&
              SameStatusFrame(severeFrame, paused.session.SemanticFrame()) &&
              paused.session.SemanticFrame().status_hash ==
                  severeFrame.status_hash,
          "attempted Identify cannot let persistence degradation overwrite paused health");
    std::printf(
        "[LITCHCRAFT_ANIMATION_BOARD_JOURNEY] "
        "stage=paused-health-plus-persistence-and-identify "
        "health=InputPaused persistence=degraded identify=attempted "
        "status_mask=%08lx exact_severe_frame=1 result=health-wins "
        "next=recover-health\n",
        static_cast<unsigned long>(StatusMask(paused)));
}

void RunControlledExitFailureAndRetry() {
    Fixture fixture(3U, 2U);
    Art& art = fixture.session.Litchcraft();
    ControlledExitBackend backend;
    fixture.session.ArtPersistence().Configure(
        {&backend, &ControlledExitBackend::EnqueueStatic,
         &ControlledExitBackend::Poll, &ControlledExitBackend::Release,
         &ControlledExitBackend::Read,
         &ControlledExitBackend::EnqueueAdvanced});

    fixture.ModeChord(VIRTUAL_LITCH_OWNER_P1);
    Check(!art.IsActive() && fixture.session.ArtPersistence().HasFailure() &&
              fixture.session.ArtPersistence().IsDirty(),
          "rejected background save preserves Play with dirty artwork and diagnostics");
    Checkpoint(fixture, "exit-persistence-failure", "Play-save-failed",
               "continue-playing-or-enter-painting");

    backend.rejectAdvanced = false;
    fixture.session.ArtPersistence().RequestFlush();
    for (uint8_t poll = 0U; poll < 4U; ++poll) {
        fixture.session.Advance(1U);
    }
    Check(!art.IsActive() && backend.advancedSize != 0U,
          "explicit lifecycle flush recovers advanced and static persistence without changing mode");
    Checkpoint(fixture, "background-save-recovered", "saved-Play-preserved",
               "reenter-when-ready");

    Fixture invalidBoot(3U, 0U);
    Art& invalidArt = invalidBoot.session.Litchcraft();
    ControlledExitBackend invalidBackend;
    invalidBackend.invalidCurrent = true;
    invalidBackend.rejectAdvanced = false;
    auto& invalidPersistence = invalidBoot.session.ArtPersistence();
    invalidPersistence.Configure(
        {&invalidBackend, &ControlledExitBackend::EnqueueStatic,
         &ControlledExitBackend::Poll, &ControlledExitBackend::Release,
         &ControlledExitBackend::Read,
         &ControlledExitBackend::EnqueueAdvanced});
    Check(invalidPersistence.RestoreAdvanced() ==
              Persistence::AdvancedRecordStatus::Invalid,
          "human regression recreates the V3 invalid-art boot state");
    invalidBoot.ModeChord(VIRTUAL_LITCH_OWNER_P1);
    invalidBoot.session.Advance(1U);
    Check(!invalidArt.IsActive() && !invalidPersistence.HasFailure() &&
              invalidBoot.session.PlacementModeActive() &&
              invalidBoot.session.State().human_illegal_mask == 0U &&
              BoardSquareMask(invalidBoot) != 0U &&
              StatusMask(invalidBoot) != 0U,
          "X+O exits Litchcraft even when unrelated advanced bytes were invalid at boot");
    Checkpoint(invalidBoot, "invalid-art-physical-exit",
               "saved-static-exit", "place-chess-pieces-or-reenter");
    invalidBoot.ModeChord(VIRTUAL_LITCH_OWNER_P1);
    Check(invalidArt.IsActive() && !invalidPersistence.HasFailure(),
          "the visible empty-Setup recovery accepts a fresh physical Litchcraft entry");
    Checkpoint(invalidBoot, "invalid-art-reentry",
               "accepted-after-setup", "continue-painting");
}


hardware::Lis2dw12Sample ShakeSample(const int32_t xMg,
                                     const int32_t yMg,
                                     const int32_t zMg = 1000) {
    hardware::Lis2dw12Sample sample{};
    sample.milliGx = xMg;
    sample.milliGy = yMg;
    sample.milliGz = zMg;
    return sample;
}

void FeedDeliberateShake(Fixture& fixture, const uint8_t energeticSamples = 27U) {
    fixture.session.ObserveMotionSample(ShakeSample(0, 0));
    for (uint8_t sample = 0U; sample < energeticSamples; ++sample) {
        Check(fixture.session.Advance(
                  modules::LitchcraftShakeDetector::SAMPLE_PERIOD_MS) ==
                  VIRTUAL_LITCH_OK,
              "shake journey advances the deterministic production clock");
        const int32_t direction = sample % 2U == 0U ? 700 : -700;
        fixture.session.ObserveMotionSample(
            ShakeSample(direction, -direction / 2));
    }
}

struct ShakeEventEvidence {
    uint8_t count = 0U;
    uint16_t token = 0U;
    uint32_t revision = 0U;
};

ShakeEventEvidence DrainShakeEvents(Art& art) {
    ShakeEventEvidence evidence;
    Art::Packet packet;
    while (art.PeekPacket(packet)) {
        if (packet.size == 15U && packet.bytes[0] == Art::MAGIC &&
            packet.bytes[1] == static_cast<uint8_t>(Art::Event::Gesture) &&
            packet.bytes[12] == 1U) {
            ++evidence.count;
            evidence.token = Read16(packet.bytes.data() + 13U);
            evidence.revision = Read32(packet.bytes.data() + 6U);
        }
        art.AcceptPacket();
    }
    return evidence;
}

void RunV3ShakeEraseJourney() {
    using namespace modules::litchcraft_animation;
    {
        Fixture emptyFrameFixture(3U, 0U);
        Art& emptyArt = emptyFrameFixture.session.Litchcraft();
        auto& emptyPersistence = emptyFrameFixture.session.ArtPersistence();
        emptyArt.ConfigureShakeErase(true);
        modules::litchcraft_animation::Document playing{};
        playing.frameCount = 2U;
        playing.topology = 3U;
        playing.playbackOnEntry = true;
        playing.frames[0].durationMs = 50U;
        playing.frames[0].emitters[0].color = {80U, 40U, 20U};
        playing.frames[1].durationMs = 5000U;
        ValidatedDocument playingDocument;
        Check(ValidatedDocument::TryAdopt(playing, playingDocument) &&
                  emptyPersistence.SetAdvancedDocument(playingDocument, true),
              "playing-empty shake fixture installs its two-frame authority");
        Check(!emptyArt.IsAnimationPlaying(),
              "playing-empty artwork remains paused after admission");
        const uint8_t play[] = {1U, 0U};
        Check(emptyArt.ApplyConnectionOrdered(2U, play, sizeof(play)) ==
                  Art::Status::Ok,
              "playing-empty shake fixture starts explicitly");
        Check(emptyFrameFixture.session.Advance(100U) == VIRTUAL_LITCH_OK &&
                  emptyArt.IsAnimationPlaying() &&
                  emptyArt.SelectedFrame() == 1U,
              "playing-empty shake fixture reaches the empty displayed frame");
        const auto before = emptyPersistence.AdvancedDocument().Value();
        const auto elapsedBefore = emptyArt.AnimationElapsedMs();
        const auto undoBefore = emptyArt.UndoCount();
        FeedDeliberateShake(emptyFrameFixture);
        Check(emptyArt.IsAnimationPlaying() &&
                  emptyArt.AnimationElapsedMs() > elapsedBefore &&
                  emptyArt.LastShakeEraseToken() == 0U &&
                  emptyArt.UndoCount() == undoBefore &&
                  DocumentsEqual(emptyPersistence.AdvancedDocument().Value(), before),
              "shake on an empty playing frame is a no-op and preserves playback");
        std::printf(
            "[LITCHCRAFT_SHAKE_ERASE_JOURNEY] stage=playing-empty-frame "
            "input=sustained-shake mode=Litchcraft result=no-op "
            "playback=continued undo=unchanged next=keep-playing\n");
    }
    Fixture fixture(3U, 0U);
    Art& art = fixture.session.Litchcraft();
    auto& persistence = fixture.session.ArtPersistence();
    art.ConfigureShakeErase(true);
    Check(art.IsShakeEraseSupported(),
          "V3 production fixture enables capability-gated shake erase");

    modules::litchcraft_animation::Document source{};
    source.frameCount = 2U;
    source.topology = 3U;
    source.frames[0].durationMs = 500U;
    source.frames[1].durationMs = 1000U;
    for (size_t emitter = 0U; emitter < EMITTER_COUNT; ++emitter) {
        auto& pixel = source.frames[0].emitters[emitter];
        pixel.color = {static_cast<uint8_t>(20U + emitter % 80U),
                       static_cast<uint8_t>(40U + emitter % 60U),
                       static_cast<uint8_t>(60U + emitter % 40U)};
        pixel.effect = emitter % 3U == 0U ? Effect::Breathe : Effect::Off;
        pixel.durationMs = pixel.effect == Effect::Off ? 0U : 800U;
    }
    source.frames[1].emitters[7].color = {9U, 19U, 29U};
    source.frames[1].emitters[7].effect = Effect::Sparkle;
    source.frames[1].emitters[7].durationMs = 900U;
    ValidatedDocument adopted;
    Check(ValidatedDocument::TryAdopt(source, adopted) &&
              persistence.SetAdvancedDocument(adopted, true),
          "shake journey installs two-frame animated authority");
    Check(fixture.session.Advance(1U) == VIRTUAL_LITCH_OK,
          "shake journey renders its populated current frame");
    (void)DrainShakeEvents(art);

    Check(fixture.session.ObserveHallMask(UINT64_C(1) << 10U) ==
              VIRTUAL_LITCH_OK,
          "occupied shake journey places a physical brush");
    FeedDeliberateShake(fixture);
    Check(art.LastShakeEraseToken() == 0U &&
              LitBoardSquares(fixture) != 0U,
          "occupied board rejects sustained shake without erasing artwork");
    const auto occupiedPhoneRequest = LegacyRequest(
        Art::Op::ShakeErase, art.SessionId(), 75U);
    fixture.session.SendLitchcraftPacket(
        occupiedPhoneRequest.bytes.data(), occupiedPhoneRequest.size);
    Check(DrainLastStatus(art) == static_cast<uint8_t>(Art::Status::Busy) &&
              art.LastShakeEraseToken() == 0U,
          "phone shake command cannot bypass the held physical brush guard");
    Check(fixture.session.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              fixture.session.Advance(Art::PHYSICAL_RELEASE_MS + 1U) ==
                  VIRTUAL_LITCH_OK,
          "occupied shake journey lifts the brush and closes its normal edit");
    (void)DrainShakeEvents(art);
    std::printf(
        "[LITCHCRAFT_SHAKE_ERASE_JOURNEY] stage=occupied input=shake "
        "occupancy=0000000000000400 mode=Litchcraft result=rejected "
        "display=artwork-intact next=lift-piece-and-retry\n");

    FeedDeliberateShake(fixture, 10U);
    art.EndConnection();
    art.BeginConnection();
    FeedDeliberateShake(fixture, 20U);
    Check(art.LastShakeEraseToken() == 0U,
          "reconnect resets partial shake progress instead of completing it");
    (void)DrainShakeEvents(art);
    std::printf(
        "[LITCHCRAFT_SHAKE_ERASE_JOURNEY] stage=reconnect input=partial-shake "
        "occupancy=0000000000000000 mode=Litchcraft result=cancelled "
        "display=artwork-intact next=start-fresh-shake\n");

    const modules::litchcraft_animation::Document beforeErase =
        persistence.AdvancedDocument().Value();
    const uint8_t undoBefore = art.UndoCount();
    FeedDeliberateShake(fixture);
    const ShakeEventEvidence event = DrainShakeEvents(art);
    modules::litchcraft_animation::Document expectedCleared = beforeErase;
    for (auto& emitter : expectedCleared.frames[0].emitters) emitter = {};
    Check(DocumentsEqual(persistence.AdvancedDocument().Value(),
                         expectedCleared) &&
              art.UndoCount() == static_cast<uint8_t>(undoBefore + 1U),
          "fresh sustained shake clears only selected frame as one history edit");
    Check(event.count == 1U && event.token != 0U &&
              event.token == art.LastShakeEraseToken() &&
              event.revision == persistence.AdvancedGeneration(),
          "shake erase publishes one compact correlated event at its authority revision");
    Check(fixture.session.Advance(1U) == VIRTUAL_LITCH_OK,
          "accepted shake reaches the production renderer immediately");
    Check(LitBoardSquares(fixture) >= 4U &&
              (BoardSquareMask(fixture) & (UINT64_C(1) << 27U)) != 0U &&
              (BoardSquareMask(fixture) & (UINT64_C(1) << 36U)) != 0U,
          "accepted shake visibly reaches separated squares in its inward full-board sweep");
    std::printf(
        "[LITCHCRAFT_SHAKE_ERASE_JOURNEY] stage=accepted input=sustained-shake "
        "occupancy=0000000000000000 mode=Litchcraft result=erased-current-frame "
        "token=%u revision=%lu status=animated next=press-X-to-undo\n",
        static_cast<unsigned>(event.token),
        static_cast<unsigned long>(event.revision));

    Check(fixture.session.Advance(Art::SHAKE_ERASE_ANIMATION_MS + 1U) ==
              VIRTUAL_LITCH_OK &&
              LitBoardSquares(fixture) == 0U,
          "erase animation expires to the cleared selected frame");
    fixture.Tap(VIRTUAL_LITCH_OWNER_P1, Button::X);
    Check(DocumentsEqual(persistence.AdvancedDocument().Value(), beforeErase),
          "one physical Undo restores all selected-frame colors and effects");
    Checkpoint(fixture, "shake-undo", "restored-one-step",
               "continue-painting");

    fixture.Set(VIRTUAL_LITCH_OWNER_P1, Button::O, true);
    const auto menuPhoneRequest = LegacyRequest(
        Art::Op::ShakeErase, art.SessionId(), 76U);
    fixture.session.SendLitchcraftPacket(
        menuPhoneRequest.bytes.data(), menuPhoneRequest.size);
    Check(DrainLastStatus(art) == static_cast<uint8_t>(Art::Status::Busy) &&
              DocumentsEqual(persistence.AdvancedDocument().Value(), beforeErase),
          "phone shake command cannot bypass the held board menu guard");
    fixture.session.Advance(1000U);
    fixture.Set(VIRTUAL_LITCH_OWNER_P1, Button::O, false);

    fixture.Tap(VIRTUAL_LITCH_OWNER_P1, Button::Right);
    Check(art.IsQuickPaletteVisible(),
          "ordinary D-pad input opens the physical quick palette");
    const auto palettePhoneRequest = LegacyRequest(
        Art::Op::ShakeErase, art.SessionId(), 77U);
    fixture.session.SendLitchcraftPacket(
        palettePhoneRequest.bytes.data(), palettePhoneRequest.size);
    Check(DrainLastStatus(art) == static_cast<uint8_t>(Art::Status::Busy) &&
              art.IsQuickPaletteVisible() &&
              DocumentsEqual(persistence.AdvancedDocument().Value(), beforeErase),
          "phone shake command cannot bypass the physical palette guard");
    fixture.session.Advance(Art::QUICK_PALETTE_IDLE_MS + 1U);

    const auto phoneRequest = LegacyRequest(
        Art::Op::ShakeErase, art.SessionId(), 78U);
    fixture.session.SendLitchcraftPacket(
        phoneRequest.bytes.data(), phoneRequest.size);
    const ShakeEventEvidence phoneEvent = DrainShakeEvents(art);
    Check(phoneEvent.count == 1U && phoneEvent.token != event.token &&
              art.UndoCount() == static_cast<uint8_t>(undoBefore + 1U) &&
              !std::any_of(
                  persistence.AdvancedDocument().Value().frames[0].emitters.begin(),
                  persistence.AdvancedDocument().Value().frames[0].emitters.end(),
                  [](const Emitter& emitter) {
                      return emitter.color.r != 0U || emitter.color.g != 0U ||
                          emitter.color.b != 0U || emitter.effect != Effect::Off ||
                          emitter.durationMs != 0U;
                  }),
          "phone and board shake share one erase event and one Undo authority");
    fixture.Tap(VIRTUAL_LITCH_OWNER_P1, Button::X);
    Check(DocumentsEqual(persistence.AdvancedDocument().Value(), beforeErase),
          "Undo restores a phone-requested whole-frame erase atomically");
}

void RunModeCrossfadeJourney() {
    const auto config = Config(3U);
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    art.BeginConnection();
    const auto samePixels = [](const auto& a, const auto& b) {
        for (size_t i = 0U; i < a.led_count; ++i) {
            if (a.pixels[i].r != b.pixels[i].r || a.pixels[i].g != b.pixels[i].g ||
                a.pixels[i].b != b.pixels[i].b || a.pixels[i].w != b.pixels[i].w) return false;
        }
        return true;
    };
    const auto before = session.PhysicalFrame();
    const auto checkpoint = [&](const char* stage) {
        const auto& frame = session.PhysicalFrame();
        uint32_t boardEnergy = 0U;
        uint32_t statusEnergy = 0U;
        for (size_t i = 0U; i < frame.led_count; ++i) {
            (i < 256U ? boardEnergy : statusEnergy) += Energy(frame.pixels[i]);
        }
        Check(boardEnergy + statusEnergy != 0U,
              "mode crossfade has no entirely dark intermediate frame");
        std::printf("[MODE_CROSSFADE_CHECKPOINT] stage=%s t=%u mode=%s board_energy=%u status_energy=%u limited_ma=%u display=\"%.*s\" next=continue-fade-or-paint\n",
                    stage, session.State().now_ms, art.IsActive() ? "Litchcraft" : "Play",
                    boardEnergy, statusEnergy, frame.applied_led_current_ma,
                    static_cast<int>(session.DisplayText().size()), session.DisplayText().data());
    };
    const auto enter = EnterPacket();
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(art.IsActive() && samePixels(before, session.PhysicalFrame()),
          "Enter acknowledges immediately while physical crossfade starts at last displayed Play frame");
    checkpoint("enter-start");
    session.Advance(125U);
    const auto middle = session.PhysicalFrame();
    checkpoint("enter-midpoint");
    Check(!samePixels(before, middle), "Play-to-Craft midpoint visibly advances without waiting for storage");
    session.Advance(125U);
    const auto craft = session.PhysicalFrame();
    checkpoint("enter-complete");
    Check(!samePixels(middle, craft), "Play-to-Craft reaches distinct full canvas at 250 ms");
    session.Advance(1U);
    Check(samePixels(craft, session.PhysicalFrame()), "completed Craft presentation is stable");
    auto exit = LegacyRequest(Art::Op::Exit, art.SessionId(), 2U);
    session.SendLitchcraftPacket(exit.bytes.data(), exit.size);
    Check(!art.IsActive() && samePixels(craft, session.PhysicalFrame()),
          "Exit acknowledges immediately while physical crossfade starts at displayed Craft frame");
    checkpoint("exit-start");
    session.Advance(125U);
    const auto reverseFrom = session.PhysicalFrame();
    checkpoint("exit-midpoint");
    auto reenter = EnterPacket();
    reenter.bytes[6] = 3U;
    session.SendLitchcraftPacket(reenter.bytes.data(), reenter.size);
    Check(art.IsActive() && samePixels(reverseFrom, session.PhysicalFrame()),
          "mid-fade reversal starts from the currently shown blend without a jump");
    checkpoint("reverse-start");
    session.Advance(250U);
    Check(samePixels(craft, session.PhysicalFrame()),
          "mid-fade reversal reaches the current Craft owner within 250 ms");
    checkpoint("reverse-complete");
    std::puts("[MODE_CROSSFADE_JOURNEY] Enter=immediate render=0/125/250ms Exit=immediate reversal=continuous next=paint");
}

void RunTapAndHeldColorSteps() {
    for (const bool craft : {false, true}) {
        for (const uint8_t owner : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
            for (const bool up : {false, true}) {
                Fixture fixture(3U, 0U);
                auto& session = fixture.session;
                auto& art = session.Litchcraft();
                const uint8_t initial = up ? 254U : 1U;
                const char* key = owner == VIRTUAL_LITCH_OWNER_P1
                    ? modules::ui_color_keys::P1_PIECE_WHEEL
                    : modules::ui_color_keys::P2_PIECE_WHEEL;
                if (craft) {
                    auto packet = LegacyRequest(Art::Op::Tool, art.SessionId(), 95U);
                    const auto color = modules::ui_color_wheel::WheelToRgb(initial);
                    const uint8_t payload[]{0U, 15U, color.r, color.g, color.b};
                    std::copy_n(payload, 5U, packet.bytes.begin() + packet.size);
                    packet.size += 5U;
                    session.SendLitchcraftPacket(packet.bytes.data(), packet.size);
                } else {
                    art.Exit();
                    session.Advance(1U);
                    modules::ControlMutationOutcome outcome;
                    const char* otherKey = owner == VIRTUAL_LITCH_OWNER_P1
                        ? modules::ui_color_keys::P2_PIECE_WHEEL
                        : modules::ui_color_keys::P1_PIECE_WHEEL;
                    session.MutateSettingJson(94U, otherKey, "128", outcome);
                    session.MutateSettingJson(95U, key, up ? "254" : "1", outcome);
                }
                const auto checkColor = [&](const uint8_t wheel) {
                    const auto expectedColor = modules::ui_color_wheel::WheelToRgb(wheel);
                    std::printf("[COLOR_STEP_JOURNEY] craft=%u owner=%u up=%u t=%u expected=%u rgb=%u,%u,%u actual=%u rgb=%u,%u,%u\n", craft, owner, up, session.State().now_ms, wheel, expectedColor.r, expectedColor.g, expectedColor.b, session.SettingsForPersistenceTest().ReadValue(key, 999U), art.Color().r, art.Color().g, art.Color().b);
                    if (craft) {
                        const auto expected = modules::ui_color_wheel::WheelToRgb(wheel);
                        Check(art.Color().r == expected.r && art.Color().g == expected.g &&
                                  art.Color().b == expected.b,
                              "Litchcraft raw color tap/hold follows exact wheel steps and wrap");
                    } else {
                        Check(session.SettingsForPersistenceTest().ReadValue(key, 999U) == wheel,
                              "Play raw color tap/hold follows exact wheel steps and wrap");
                    }
                };
                const Button button = up ? Button::Up : Button::Down;
                const int direction = up ? 1 : -1;
                fixture.Set(owner, button, true);
                checkColor(static_cast<uint8_t>(initial + direction));
                // Set's 30 ms sample accepts the debounced edge. The production
                // repeat deadline is measured from that accepted press.
                session.Advance(hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS);
                checkColor(static_cast<uint8_t>(initial + direction * 3));
                session.Advance(hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS);
                checkColor(static_cast<uint8_t>(initial + direction * 5));
                fixture.Set(owner, button, false);
            }
        }
    }
}

void RunPaintStatusAndPaletteJourney(const uint8_t hardware, const bool rotate,
                                   const uint8_t owner) {
    Fixture fixture(hardware, 4U, false, true, rotate);
    auto& art = fixture.session.Litchcraft();
    const uint8_t physicalSwatch = owner == VIRTUAL_LITCH_OWNER_P2 ? 4U : 59U;
    const uint8_t swatch = rotate ? 63U - physicalSwatch : physicalSwatch;
    const auto documentBeforeTool = fixture.session.ArtPersistence().AdvancedDocument().Value();
    const auto revisionBeforeTool = fixture.session.ArtPersistence().AdvancedGeneration();
    auto tool = LegacyRequest(Art::Op::Tool, art.SessionId(), 90U);
    const uint8_t payload[]{0U, 15U, 200U, 80U, 24U};
    std::copy_n(payload, 5U, tool.bytes.begin() + tool.size);
    tool.size += 5U;
    fixture.session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    fixture.session.Advance(1U);
    Check(DrainLastStatus(art) == static_cast<uint8_t>(Art::Status::Ok) &&
              art.Color().r == 200U && art.Color().g == 80U && art.Color().b == 24U,
          "app tool selection is accepted and becomes exact active RGB with an advanced document");
    Check(modules::litchcraft_animation::DocumentsEqual(
              documentBeforeTool, fixture.session.ArtPersistence().AdvancedDocument().Value()) &&
              revisionBeforeTool == fixture.session.ArtPersistence().AdvancedGeneration(),
          "compact Tool selection cannot modify advanced artwork, favorites or document revision");
    auto eraseTool = LegacyRequest(Art::Op::Tool, art.SessionId(), 91U);
    const uint8_t erasePayload[]{1U, 2U, 200U, 80U, 24U};
    std::copy_n(erasePayload, 5U, eraseTool.bytes.begin() + eraseTool.size);
    eraseTool.size += 5U;
    fixture.session.SendLitchcraftPacket(eraseTool.bytes.data(), eraseTool.size);
    Art::Packet readback;
    bool exactToolReadback = false;
    while (art.PeekPacket(readback)) {
        if (readback.bytes[1] == static_cast<uint8_t>(Art::Event::Tool)) {
            exactToolReadback |= readback.size == 17U && readback.bytes[12] == 200U &&
                readback.bytes[13] == 80U && readback.bytes[14] == 24U &&
                readback.bytes[15] == 1U && readback.bytes[16] == 2U;
        }
        art.AcceptPacket();
    }
    Check(art.IsErasing() && exactToolReadback,
          "advanced compact Tool echoes exact app RGB, erase flag and target mask");
    tool.bytes[6] = 92U;
    fixture.session.SendLitchcraftPacket(tool.bytes.data(), tool.size);
    fixture.session.Advance(1U);
    const auto checkCorners = [&]() {
        const auto expected = modules::ScaleRgbSaturatingBy256(art.Color(), 48U);
        size_t lit = 0U;
        for (size_t index = 0U; index < 32U; ++index) {
            const auto actual = fixture.session.SemanticFrame().status[index];
            const bool corner = index % 8U == 0U || index % 8U == 7U;
            if (actual.r || actual.g || actual.b) ++lit;
            Check(actual.r == (corner ? expected.r : 0U) &&
                      actual.g == (corner ? expected.g : 0U) &&
                      actual.b == (corner ? expected.b : 0U),
                  "only paint-color corners remain, even with four frames");
            const size_t physicalBase = hardware == 3U ? 256U : 192U;
            Check((Energy(fixture.session.PhysicalFrame().pixels[physicalBase + index]) != 0U) == corner,
                  "physical status chain lights only the eight corner-adjacent LEDs");
        }
        std::printf("[PAINT_STATUS_JOURNEY] frames=4 lit=%zu paint=%u,%u,%u palette=%u next=paint-or-select\n",
                    lit, art.Color().r, art.Color().g, art.Color().b,
                    art.IsQuickPaletteVisible());
    };
    checkCorners();
    fixture.Tap(owner, Button::Right);
    checkCorners();
    fixture.session.Advance(1500U);
    Check(art.IsQuickPaletteVisible(), "palette remains selectable near two seconds");
    fixture.session.ObserveHallMask(UINT64_C(1) << physicalSwatch);
    fixture.session.Advance(2500U);
    Check(art.IsQuickPaletteVisible(), "held swatch magnet prevents idle expiry");
    checkCorners();
    const auto before = fixture.session.SemanticFrame().board[swatch][0U];
    fixture.session.ObserveHallMask(0U);
    fixture.session.Advance(1U);
    Check(!art.IsQuickPaletteVisible(), "magnet lift immediately ends palette input ownership");
    const auto fading = fixture.session.SemanticFrame().board[swatch][0U];
    Check(fading.r || fading.g || fading.b, "magnet lift begins visible fade instead of disappearing");
    fixture.session.Advance(150U);
    const auto middle = fixture.session.SemanticFrame().board[swatch][0U];
    Check(middle.r + middle.g + middle.b < before.r + before.g + before.b,
          "palette visibly fades toward underlying artwork");
    checkCorners();
    fixture.session.Advance(400U);
    const auto after = fixture.session.SemanticFrame().board[swatch][0U];
    Check(after.r == 0U && after.g == 0U && after.b == 0U,
          "palette fade expires to the unchanged blank artwork");
    fixture.Tap(owner, Button::Right);
    fixture.session.Advance(Art::QUICK_PALETTE_IDLE_MS - 31U);
    Check(art.IsQuickPaletteVisible(), "idle palette stays open through 1999 ms");
    fixture.session.Advance(1U);
    Check(!art.IsQuickPaletteVisible(), "idle palette closes input at exactly two seconds");
    Check(Energy(fixture.session.SemanticFrame().board[swatch][0U]) != 0U,
          "idle expiry starts a visible fade");
    fixture.session.Advance(Art::QUICK_PALETTE_FADE_MS);
    checkCorners();
    fixture.ToggleTool(owner);
    Check(art.IsErasing(), "long O toggles erase with animation documents loaded");
    fixture.ToggleTool(owner);
    Check(!art.IsErasing(), "second long O restores paint");
    fixture.Set(owner, Button::O, true);
    fixture.session.Advance(1000U);
    Check(art.IsErasing(), "holding O toggles once and does not open an inspector");
    fixture.Set(owner, Button::O, false);
    Check(art.IsErasing(), "O release after a hold does not toggle a second time");
    fixture.ToggleTool(owner);
    Check(!art.IsErasing(), "another long O recovers paint without a short Redo action");
    const auto frameCount = art.FrameCount();
    fixture.Chord(owner, Button::Up);
    Check(art.FrameCount() == frameCount, "O plus arrow cannot create animation frames");
}

Art::Status ApplyOrderedPlayback(Fixture& fixture, const uint8_t action,
                                 const uint8_t frame) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(fixture.session.State().now_ms);
    const uint8_t payload[]{action, frame};
    return fixture.session.Litchcraft().ApplyConnectionOrdered(
        2U, payload, sizeof(payload));
}

void RunPlaybackCornerFadeJourney() {
    Fixture fixture(3U, 4U);
    auto& art = fixture.session.Litchcraft();
    const auto cornerEnergy = [&]() {
        const auto color = fixture.session.SemanticFrame().status[0U];
        return static_cast<unsigned>(color.r) + color.g + color.b;
    };
    const unsigned resting = cornerEnergy();
    Check(resting != 0U, "paused fade journey starts with active paint corners");
    const uint8_t play[]{1U, art.SelectedFrame()};
    Check(ApplyOrderedPlayback(fixture, 1U, play[1U]) == Art::Status::Ok,
          "fade journey accepts Play through the production ordered domain adapter");
    fixture.session.Advance(1U);
    Check(cornerEnergy() != 0U,
          "Play starts a gentle fade instead of instantly clearing corners");
    fixture.session.Advance(249U);
    const unsigned halfOut = cornerEnergy();
    Check(halfOut != 0U && halfOut < resting,
          "Play corners fade partway after250ms");
    Checkpoint(fixture, "play-fade-250ms", "accepted-fading-out", "watch-animation");
    fixture.session.Advance(250U);
    Check(cornerEnergy() == 0U && art.IsAnimationPlaying(),
          "Play corners reach black at500ms while animation continues");
    const uint8_t pause[]{0U, art.SelectedFrame()};
    Check(ApplyOrderedPlayback(fixture, 0U, pause[1U]) == Art::Status::Ok,
          "fade journey accepts Pause");
    fixture.session.Advance(125U);
    Check(cornerEnergy() != 0U && cornerEnergy() < resting,
          "Pause corners fade partway back after125ms");
    Checkpoint(fixture, "pause-fade-125ms", "accepted-fading-in", "paint-or-resume");
    fixture.session.Advance(125U);
    Check(cornerEnergy() == resting && !art.IsAnimationPlaying(),
          "Pause restores original corners exactly at250ms");
    Check(ApplyOrderedPlayback(fixture, 1U, play[1U]) == Art::Status::Ok,
          "fade reversal accepts the next Play");
    fixture.session.Advance(200U);
    const unsigned beforeReverse = cornerEnergy();
    Check(ApplyOrderedPlayback(fixture, 0U, pause[1U]) == Art::Status::Ok,
          "rapid Pause reverses a partial Play fade");
    fixture.session.Advance(1U);
    Check(cornerEnergy() >= beforeReverse && cornerEnergy() <= beforeReverse + 2U,
          "rapid reversal begins at the current visible level without a flash");
    fixture.session.Advance(249U);
    Check(cornerEnergy() == resting, "reversed fade settles after250ms");
    const uint8_t invalid[]{1U, 255U};
    Check(ApplyOrderedPlayback(fixture, 1U, invalid[1U]) == Art::Status::Malformed,
          "invalid playback frame is rejected");
    fixture.session.Advance(250U);
    Check(cornerEnergy() == resting && !art.IsAnimationPlaying(),
          "rejected action leaves corners and paused playback unchanged");
    Checkpoint(fixture, "fade-recovery", "rejected-invalid-preserved", "paint-or-resume");
    fixture.ToggleTool(VIRTUAL_LITCH_OWNER_P1);
    Check(art.IsErasing(), "fade journey enters Erase through physical O input");
    Check(ApplyOrderedPlayback(fixture, 1U, art.SelectedFrame()) == Art::Status::Ok,
          "Erase journey accepts Play");
    fixture.session.Advance(500U);
    unsigned eraseEnergy = 0U;
    for (const auto& color : fixture.session.SemanticFrame().status) {
        eraseEnergy += Energy(color);
    }
    Check(art.IsAnimationPlaying() && art.IsErasing() && eraseEnergy != 0U,
          "Erase action cue retains its existing meaning during playback");
    Checkpoint(fixture, "playing-erase-exception", "accepted-erase-cue", "erase-or-switch-tool");
    fixture.Set(VIRTUAL_LITCH_OWNER_P1, Button::O, true);
    for (uint32_t step = 0U; step < 1000U && art.IsErasing(); ++step)
        fixture.session.Advance(1U);
    std::printf("[PLAYBACK_FADE_TOOL] erasing=%u playing=%u corner_energy=%u now=%u\n",
                art.IsErasing(), art.IsAnimationPlaying(), cornerEnergy(),
                fixture.session.State().now_ms);
    // This fixture has not remembered the current paint color. Returning to
    // paint records it through the existing palette edit, which pauses playback.
    Check(!art.IsErasing() && !art.IsAnimationPlaying() && cornerEnergy() == 0U,
          "paint palette edit pauses and starts the return fade from black");
    fixture.session.Advance(125U);
    Check(cornerEnergy() > 0U && cornerEnergy() < resting,
          "paint palette edit restores corners partway after 125 ms");
    fixture.session.Advance(125U);
    Check(cornerEnergy() == resting && !art.IsAnimationPlaying(),
          "paint palette edit restores corners after 250 ms without resuming");
    fixture.Set(VIRTUAL_LITCH_OWNER_P1, Button::O, false);
    Checkpoint(fixture, "erase-to-paint-paused", "accepted-palette-edit-fade",
               "paint-or-resume");
}

void RunPlayingArtworkStatusJourney() {
    Fixture fixture(3U, 4U);
    Check(ApplyOrderedPlayback(fixture, 1U, 0U) == Art::Status::Ok,
          "playing status journey accepts Play");
    fixture.session.Advance(500U);
    auto& art = fixture.session.Litchcraft();
    Check(art.IsAnimationPlaying() && LitBoardSquares(fixture) != 0U,
          "playing artwork journey starts with visible board animation");
    Checkpoint(fixture, "playing-artwork", "animation-visible-status-clear",
               "watch-next-frame");
    Check(StatusMask(fixture) == 0U,
          "playing animation does not overlay the current paint color on status corners");
    fixture.session.Advance(2100U);
    Check(art.IsAnimationPlaying() && StatusMask(fixture) == 0U,
          "animation frame advance keeps current-color corners clear");
    const uint8_t pause[]{0U, art.SelectedFrame()};
    Check(ApplyOrderedPlayback(fixture, 0U, pause[1U]) == Art::Status::Ok,
          "ordered Pause accepts the displayed animation frame");
    fixture.session.Advance(250U);
    Check(!art.IsAnimationPlaying() && StatusMask(fixture) == 0x81818181U,
          "paused artwork restores eight current-color corner cues");
    Checkpoint(fixture, "paused-artwork", "color-corners-restored",
               "paint-or-resume");
}

} // namespace

int main() {
    RunModeCrossfadeJourney();
    RunTapAndHeldColorSteps();
    RunPlayingArtworkStatusJourney();
    RunPlaybackCornerFadeJourney();
    for (const uint8_t hardware : {2U, 3U}) {
        for (const bool rotate : {false, true}) {
            for (const uint8_t owner : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
                RunPaintStatusAndPaletteJourney(hardware, rotate, owner);
            }
        }
    }
    Check((modules::ble_device_capabilities::CapabilityMask(
              false, false, false, false, true, true, true, true, true) &
           modules::ble_device_capabilities::Capability::LitchcraftAnimation) !=
              0U,
          "accepted physical animation controls advertise capability bit 14");
    RunSharedWarningPreemption();
    RunControlledExitFailureAndRetry();
    RunV3ShakeEraseJourney();
    std::printf("[LITCHCRAFT_ANIMATION_BOARD_CONTROLS] failures=%d\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
