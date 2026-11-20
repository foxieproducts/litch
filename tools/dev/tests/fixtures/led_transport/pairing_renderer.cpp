#include <cstdio>

#include "firmware/led_animation_renderer.hpp"
#include "hardware/neopixel_led_renderer.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/quick_settings_items.hpp"
#include "rmt_fixture.hpp"

namespace fxe { uint32_t Millis() { return fixture::nowUs / 1000U; } }

namespace {
using Pairing = modules::BoardOwnerPairingRuntime;
using Role = hardware::ControlLedRole;
unsigned failures = 0U;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: captured pairing renderer: %s\n", message);
        ++failures;
    }
}

struct Persistence final : modules::BoardOwnerPersistence {
    Receipt receipt = 0U;
    Receipt next = 1U;
    Outcome outcome = Outcome::Pending;
    modules::BoardOwnerRecord requested = modules::BoardOwnerRecord::Invalid;
    bool SaveOwnerRecord(modules::BoardOwnerRecord record, Receipt& accepted) override {
        if (receipt != 0U) return false;
        requested = record;
        accepted = receipt = next++;
        outcome = Outcome::Pending;
        return true;
    }
    Outcome GetOutcome(Receipt accepted) const override {
        return accepted == receipt ? outcome : Outcome::Unknown;
    }
    bool Release(Receipt accepted) override {
        if (accepted == 0U || accepted != receipt) return false;
        receipt = 0U;
        return true;
    }
};

struct Fixture {
    Persistence persistence;
    Pairing pairing{persistence};
    const hardware::BoardHardwareProfile profile =
        hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3);
    hardware::NeoPixelLedRenderer output{256U, 47U, 44U, 33U, true};
    firmware::LedAnimationRenderer renderer;
    firmware::LedAnimationPresentation presentation;

    Fixture() {
        fixture::nowUs = 100000U;
        fixture::channels = {};
        pairing.Boot(modules::BoardOwnerRecord::Active, {true, 1U}, true);
        output.Begin();
        renderer.Bind(output, profile, {});
        renderer.CancelBoot();
        modules::UpwardOutputSlew source;
        source.Reset(2000U, fxe::Millis());
        const modules::AmbientLuxAverager ambient;
        renderer.InitializeBrightness(ambient, source);
        presentation.pairingAllowed = true;
        presentation.quickSettingsActive = true;
        presentation.menuSelection = modules::quick_settings_items::OWNER_PAIRING;
        presentation.menu.SetOwner(modules::UiOwner::P1);
        modules::MenuLevelOverlay::IconPixels icon{};
        Check(modules::menu_icons_6x6::ParseIcon(modules::menu_icons_6x6::BLUETOOTH_ICON, icon),
              "the actual pinned Bluetooth glyph is available");
        presentation.menu.ShowPinnedIcon(icon, {});
        presentation.output.autoBrightness = false;
        presentation.output.brightnessLevel = 8U;
        presentation.output.power = modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig({true, false});
        presentation.output.pipeline = modules::led_render_policy::MakePipelineConfig(false, 32U);
        presentation.output.pipeline.outputBrightnessScale256 = 128U;
    }

    void Capture() {
        presentation.capturedAtMs = fxe::Millis();
        presentation.pairing = pairing.CaptureCue();
    }

    void RenderAt(const uint32_t nowMs) {
        fixture::nowUs = uint64_t(nowMs) * 1000U;
        fixture::pixelWrites.clear();
        const auto suspensions = fixture::schedulerSuspensions;
        firmware::LedAnimationFrameResult result;
        renderer.Render(presentation, result);
        Check(result.attempted && result.receipt.completed, "production Render completes both SDK strips");
        Check(fixture::pixelWrites.size() == 300U, "production pipeline writes every V3.3 physical pixel");
        unsigned glyphPixels = 0U;
        for (const auto& pixel : fixture::pixelWrites)
            if (pixel.channel == 0U && pixel.r == 0U && pixel.b > pixel.g) ++glyphPixels;
        Check(glyphPixels >= 6U, "the real Quick Setup Bluetooth foreground remains visible alongside pairing feedback");
        Check(fixture::criticalDepth == 0U && fixture::schedulerDepth == 0U && fixture::corruptions == 0U,
              "S3 scheduler guard restores scheduling without starving RMT refill");
        Check(fixture::schedulerSuspensions > suspensions, "actual S3 TX completion branch is exercised for this frame");
    }

    unsigned ControlEnergy(const Role role, const bool p2 = false) const {
        const auto index = hardware::V3_3PhysicalIndexForControl(p2, role);
        for (const auto& pixel : fixture::pixelWrites)
            if (pixel.channel == 1U && pixel.index == index) return pixel.r + pixel.g + pixel.b;
        return 0U;
    }

    std::array<modules::Rgb, 32U> Ring() const {
        std::array<modules::Rgb, 32U> ring{};
        for (uint8_t index = 0U; index < 32U; ++index) {
            const auto chain = hardware::V3_3PhysicalIndexForSemanticStatus(index);
            for (const auto& pixel : fixture::pixelWrites)
                if (pixel.channel == 1U && pixel.index == chain)
                    ring[index] = {uint8_t(pixel.r), uint8_t(pixel.g), uint8_t(pixel.b)};
        }
        return ring;
    }

    void Save() {
        persistence.outcome = Persistence::Outcome::Saved;
        pairing.UpdateDurableState(persistence.requested, true);
        pairing.Poll();
    }
};

bool Same(const std::array<modules::Rgb, 32U>& left,
          const std::array<modules::Rgb, 32U>& right) {
    for (size_t index = 0U; index < left.size(); ++index)
        if (left[index].r != right[index].r || left[index].g != right[index].g ||
            left[index].b != right[index].b) return false;
    return true;
}

unsigned BluePixels(const std::array<modules::Rgb, 32U>& ring) {
    unsigned count = 0U;
    for (const auto pixel : ring)
        if (pixel.r == 0U && pixel.g > 0U && pixel.b > pixel.g) ++count;
    return count;
}

void CapturedHoldReachesOutput() {
    Fixture f;
    f.pairing.SyncLocalClearContext(true, 0U);
    f.pairing.Poll();
    f.pairing.ObserveButton({0U, true, true, fxe::Millis()});
    f.pairing.ObserveButton({0U, false, true, fxe::Millis()});
    f.Capture();
    const uint32_t start = f.presentation.capturedAtMs;
    uint32_t visited = 0U;
    bool onlyBlue = true;
    bool fourLedTail = true;
    unsigned early = 0U;
    unsigned late = 0U;
    for (uint32_t elapsed = 200U; elapsed <= 1950U; elapsed += 50U) {
        f.RenderAt(start + elapsed);
        const auto ring = f.Ring();
        const auto blue = BluePixels(ring);
        fourLedTail &= blue >= 4U && blue <= 5U;
        for (uint8_t pixel = 0U; pixel < ring.size(); ++pixel) {
            const auto color = ring[pixel];
            onlyBlue &= color.r == 0U && (color.b == 0U || color.b > color.g);
            if (color.b > color.g) visited |= UINT32_C(1) << pixel;
        }
        if (elapsed % 500U == 0U) {
            const uint8_t head = static_cast<uint8_t>(elapsed * 32U / Pairing::CLEAR_HOLD_MS);
            Check(ring[head].b > 0U && ring[head].b > ring[(head + 1U) % 32U].b,
                  "captured hold head advances one quarter of the physical perimeter per half-second");
        }
        if (elapsed == 200U) early = f.ControlEnergy(Role::X);
        if (elapsed == 1500U) late = f.ControlEnergy(Role::X);
    }
    Check(onlyBlue && fourLedTail && visited == UINT32_MAX,
          "captured hold delivers one blue four-LED tail across every status position, including USB, in two seconds");
    Check(early > 0U && late > early &&
              f.ControlEnergy(Role::X) == f.ControlEnergy(Role::O) &&
              f.ControlEnergy(Role::X, true) == 0U && f.ControlEnergy(Role::O, true) == 0U,
          "one captured hold advances physical blue X/O only at the opening end without live runtime polling");
    f.pairing.ObserveButton({0U, true, false, fxe::Millis()});
    f.Capture();
    const auto cancelledAt = fxe::Millis();
    f.RenderAt(cancelledAt);
    const auto cancelled = f.Ring();
    f.presentation.pairingAllowed = false;
    f.RenderAt(cancelledAt);
    Check(Same(cancelled, f.Ring()), "early release removes the captured hold lap and restores the underlying status frame");
    std::printf("[CAPTURED_PAIRING_RENDERER] hold_snapshot=%u sampled_ms=200..1950/50 ring=one-blue-tail visited=%08lx cancel=underlying-frame own_X_energy=%u/%u other_X=0 seam=Render/pipeline/Arduino-RGB-output\n",
                start, static_cast<unsigned long>(visited), early, late);
}

void CapturedPendingAndSuccessReachOutput() {
    Fixture f;
    f.pairing.SyncLocalClearContext(true, 0U);
    f.pairing.ObserveButton({0U, true, true, fxe::Millis()});
    f.pairing.ObserveButton({0U, false, true, fxe::Millis()});
    fixture::nowUs += 2000000U;
    f.pairing.Poll();
    f.Capture();
    const uint32_t accepted = f.presentation.capturedAtMs;
    f.RenderAt(accepted);
    Check(BluePixels(f.Ring()) == 4U,
          "captured accepted hold retains its blue tail at the pending handoff through real output");
    f.RenderAt(accepted + 5000U);
    const auto pending = f.Ring();
    f.RenderAt(accepted + 5033U);
    Check(BluePixels(pending) >= 2U && !Same(pending, f.Ring()),
          "one pending snapshot still delivers a moving blue tail after five seconds of controlled native delay");

    f.Save();
    auto effect = f.pairing.Custody().PendingEffect();
    Check(effect.type == Pairing::Controller::EffectType::QuiesceAndRejectWork,
          "durable Clearing precedes the controlled native acknowledgment");
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true);
    f.Save();
    effect = f.pairing.Custody().PendingEffect();
    Check(effect.type == Pairing::Controller::EffectType::UnpairSucceeded,
          "native deletion and durable Absent precede completion acknowledgment");
    f.pairing.Custody().AcknowledgeSuccess(effect.token);
    f.pairing.Poll();
    f.Capture();
    const uint32_t completed = f.presentation.capturedAtMs;
    f.RenderAt(completed + 400U);
    const auto success = f.Ring();
    f.RenderAt(completed + 560U);
    Check(BluePixels(success) == 16U && !Same(success, f.Ring()),
          "one captured checked-success snapshot delivers whole-ring pulses from the completion clock");
    f.RenderAt(completed + 1800U);
    Check(BluePixels(f.Ring()) == 16U,
          "the late clear retains its full success episode near the independent two-second deadline");
    f.RenderAt(completed + 2000U);
    const auto expired = f.Ring();
    f.presentation.pairingAllowed = false;
    f.RenderAt(completed + 2000U);
    Check(Same(expired, f.Ring()),
          "a stored success snapshot autonomously expires to the underlying game without live runtime polling");
    std::printf("[CAPTURED_PAIRING_RENDERER] accepted_ms=%u completed_ms=%u pending_samples=5000/5033 success_samples=400/560/1800 expiry=2000 native_and_NVS=controlled runtime_poll_between_render_samples=0 S3_scheduler_branch=exercised\n",
                accepted, completed);
}
}

int main() {
    CapturedHoldReachesOutput();
    CapturedPendingAndSuccessReachOutput();
    return failures == 0U ? 0 : 1;
}
