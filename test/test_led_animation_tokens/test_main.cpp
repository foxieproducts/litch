#include <array>
#include <cstdio>

#include "modules/animation_timing.hpp"
#include "modules/board_led_frame.hpp"
#include "firmware/led_animation_task.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/neopixel_led_renderer.hpp"

namespace firmware {
struct LedAnimationTaskTestPeer {
    static bool Take(LedAnimationTask& task) { return task.TakePublication(); }
    static void Render(LedAnimationTask& task, LedAnimationFrameResult& result) {
        task.m_renderer.Render(task.m_current, result);
    }
    static uint64_t DuckReminder(const LedAnimationTask& task) {
        return task.m_renderer.m_frameOwnership.duckReminder;
    }
    static modules::UiOwner MenuOwner(const LedAnimationTask& task) {
        return task.m_renderer.m_frameOwnership.menuOwner;
    }
};
}

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

int main() {
    std::puts("[LED_ANIMATION_TOKENS] start");

    {
        fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(100U);
        const auto profile = hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3);
        hardware::NeoPixelLedRenderer output(256U, 47U, 44U, 33U, true);
        firmware::LedAnimationTask task;
        task.Bind(output, profile, {});
        task.CancelBoot();
        firmware::LedAnimationPresentation source;
        auto& game = source.scene.emplace<firmware::GameLedPresentation>();
        constexpr uint8_t duck = 18U;
        const uint64_t duckMask = UINT64_C(1) << duck;
        auto& snapshot = game.interaction.snapshot;
        snapshot.board.expectedOccupiedMask = duckMask;
        snapshot.board.physicalOccupiedMask = duckMask;
        source.physicalOccupiedMask = duckMask;
        snapshot.game.variant = modules::GameVariant::Duck;
        snapshot.game.duckSquare = duck;
        snapshot.game.duckTurnPhase = modules::DuckTurnPhase::DuckRelocation;
        auto& interaction = snapshot.interaction.taggedState;
        interaction.tag = runtime::ChessInteractionPhase::DuckRelocation;
        interaction.requiredAction = runtime::RequiredPhysicalAction::LiftPrimary;
        interaction.plan.kind = runtime::PhysicalTransitionKind::DuckRelocation;
        interaction.plan.source = runtime::PhysicalTransitionSource::Human;
        interaction.plan.beforeOccupiedMask = duckMask;
        interaction.plan.afterOccupiedMask = duckMask;
        interaction.plan.moveFrom = duck;
        interaction.plan.relocationCount = 1U;
        interaction.plan.relocations[0] = {duck, runtime::CHESS_INTERACTION_INVALID_SQUARE};
        modules::BleIdentifyPresentation owner;
        owner.Start(7U, {0U, 0U, 255U}, false, 3000U, 100U);
        source.identify = owner.Capture();
        task.Publish(source);
        Check(firmware::LedAnimationTaskTestPeer::Take(task), "Identify captured by animation task");
        firmware::LedAnimationFrameResult first;
        firmware::LedAnimationTaskTestPeer::Render(task, first);
        fxe::ManualMillis::SetNowMs(400U);
        task.Publish(source);
        Check(firmware::LedAnimationTaskTestPeer::Take(task), "unchanged owner scene reaches animation task");
        firmware::LedAnimationFrameResult during;
        firmware::LedAnimationTaskTestPeer::Render(task, during);
        const auto blue = [](const auto& grid) {
            size_t count = 0U;
            for (const auto& pixel : grid) if (pixel.b > pixel.r && pixel.b > pixel.g) ++count;
            return count;
        };
        Check(blue(during.grid) != 0U, "Identify is rendered after unchanged publication");
        fxe::ManualMillis::SetNowMs(3100U);
        task.Publish(source);
        Check(firmware::LedAnimationTaskTestPeer::Take(task), "expiry frame receives unchanged scene");
        firmware::LedAnimationFrameResult expired;
        firmware::LedAnimationTaskTestPeer::Render(task, expired);
        Check(blue(expired.grid) == 0U, "Identify disappears at wall-clock deadline after republishing");
        Check(firmware::LedAnimationTaskTestPeer::DuckReminder(task) == duckMask,
              "Duck relocation attention resumes without the expired Identify foreground");

        fxe::ManualMillis::SetNowMs(3300U);
        owner.Start(7U, {0U, 0U, 255U}, false, 3000U, 3300U);
        source.identify = owner.Capture();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult started;
        firmware::LedAnimationTaskTestPeer::Render(task, started);
        fxe::ManualMillis::SetNowMs(3600U);
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult visible;
        firmware::LedAnimationTaskTestPeer::Render(task, visible);
        Check(blue(visible.grid) != 0U, "new Identify renders before Stop");
        owner.Stop(); source.identify = owner.Capture();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult stopped;
        firmware::LedAnimationTaskTestPeer::Render(task, stopped);
        Check(blue(stopped.grid) == 0U, "Stop removes Identify after unchanged publications");

        fxe::ManualMillis::SetNowMs(4000U);
        owner.Start(7U, {0U, 0U, 255U}, false, 3000U, 4000U);
        source.identify = owner.Capture();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult beforeScene;
        firmware::LedAnimationTaskTestPeer::Render(task, beforeScene);
        source.scene.emplace<firmware::OtaLedScene>();
        fxe::ManualMillis::SetNowMs(4300U);
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult special;
        firmware::LedAnimationTaskTestPeer::Render(task, special);
        fxe::ManualMillis::SetNowMs(7000U);
        source.scene.emplace<firmware::GameLedPresentation>();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult resumed;
        firmware::LedAnimationTaskTestPeer::Render(task, resumed);
        Check(blue(resumed.grid) == 0U, "expired Identify cannot return after special scene");

        fxe::ManualMillis::SetNowMs(8000U);
        owner.Start(7U, {0U, 0U, 255U}, false, 3000U, 8000U);
        source.identify = owner.Capture();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult beforeMenu;
        firmware::LedAnimationTaskTestPeer::Render(task, beforeMenu);
        fxe::ManualMillis::SetNowMs(8100U);
        source.quickSettingsActive = true;
        source.menu.SetOwner(modules::UiOwner::P1);
        source.menu.ShowMessage("3", {255U, 0U, 0U}, {});
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult menuStart;
        firmware::LedAnimationTaskTestPeer::Render(task, menuStart);
        fxe::ManualMillis::SetNowMs(8400U);
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult menuVisible;
        firmware::LedAnimationTaskTestPeer::Render(task, menuVisible);
        const auto red = [](const auto& grid) {
            size_t count = 0U;
            for (const auto& pixel : grid) if (pixel.r > pixel.g && pixel.r > pixel.b) ++count;
            return count;
        };
        Check(red(menuVisible.grid) != 0U && blue(menuVisible.grid) == 0U,
              "Quick Settings preempts Identify without losing its own menu");
        owner.Stop(); source.identify = owner.Capture();
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult menuAfterStop;
        firmware::LedAnimationTaskTestPeer::Render(task, menuAfterStop);
        Check(red(menuAfterStop.grid) != 0U && blue(menuAfterStop.grid) == 0U,
              "Identify Stop does not cancel the concurrent menu");
        source.menu.SetOwner(modules::UiOwner::P2);
        task.Publish(source); firmware::LedAnimationTaskTestPeer::Take(task);
        firmware::LedAnimationFrameResult oppositeMenu;
        firmware::LedAnimationTaskTestPeer::Render(task, oppositeMenu);
        Check(red(oppositeMenu.grid) != 0U &&
                  firmware::LedAnimationTaskTestPeer::MenuOwner(task) == modules::UiOwner::P2,
              "opposite-end menu ownership survives unchanged-source publication");
    }

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 100U;
        token.phase.playback = modules::animation::PlaybackMode::Loop;
        token.curve.waveform = modules::animation::Waveform::Blink;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;
        token.curve.onWidth255 = 127U;

        Check(modules::animation::SampleValue({0U}, token) == 255U,
              "loop blink starts on");
        Check(modules::animation::SampleValue({60U}, token) == 0U,
              "loop blink turns off after duty window");
        Check(modules::animation::SampleValue({100U}, token) == 255U,
              "loop blink repeats");
    }

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 120U;
        token.phase.playback = modules::animation::PlaybackMode::OneShot;
        token.curve.waveform = modules::animation::Waveform::Triangle;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;

        Check(modules::animation::SampleValue({0U}, token) == 0U,
              "one-shot triangle starts at minimum");
        Check(modules::animation::SampleValue({60U}, token) >= 250U,
              "one-shot triangle peaks mid-flight");
        Check(modules::animation::SamplePhase({120U}, token.phase).finished,
              "one-shot phase reports finished at end");
        Check(modules::animation::SampleValue({120U}, token) == 0U,
              "one-shot triangle returns to minimum after completion");
    }

    {
        modules::animation::ValueToken token;
        token.phase.periodMs = 100U;
        token.phase.playback = modules::animation::PlaybackMode::PingPongLoop;
        token.curve.waveform = modules::animation::Waveform::Saw;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;

        const uint8_t start = modules::animation::SampleValue({0U}, token);
        const uint8_t peak = modules::animation::SampleValue({100U}, token);
        const uint8_t returnValue = modules::animation::SampleValue({150U}, token);

        Check(start == 0U, "pingpong saw starts at minimum");
        Check(peak >= 250U, "pingpong saw reaches maximum at turnaround");
        Check(returnValue > 100U && returnValue < 200U,
              "pingpong saw comes back down on the return leg");
    }

    {
        modules::BoardSquareLeds square;
        modules::ApplyToBoardSquare(square,
                                    modules::BoardLedSlots::Right,
                                    modules::RGB_YELLOW,
                                    255U,
                                    modules::animation::BlendMode::Replace);

        Check(RgbEq(square.left, modules::RGB_BLACK),
              "slot-targeting leaves square0 untouched");
        Check(RgbEq(square.top, modules::RGB_BLACK) && RgbEq(square.bottom, modules::RGB_BLACK),
              "slot-targeting leaves piece untouched");
        Check(RgbEq(square.right, modules::RGB_YELLOW),
              "slot-targeting updates square2 independently");
    }

    if (g_failures == 0) {
        std::puts("[LED_ANIMATION_TOKENS] ok");
        return 0;
    }

    std::printf("[LED_ANIMATION_TOKENS] failures=%d\n", g_failures);
    return 1;
}
