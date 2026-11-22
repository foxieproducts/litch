#include <cstdio>
#include <array>
#include <cstdlib>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/control_led_thinking_feedback.hpp"
#include "modules/physical_palette_shortcut.hpp"
#include "modules/animation_timing.hpp"
#include "modules/control_led_feedback.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/ui_color_wheel.hpp"

namespace {
using Role = hardware::ControlLedRole;
using Overlay = modules::status_led_frame_adapter::FixedPhysicalStatusOverlay;
bool Same(modules::Rgb a, modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
bool Lit(modules::Rgb a) { return a.r != 0U || a.g != 0U || a.b != 0U; }
}
void CheckBeachball(fxe::test_support::TestResults& results) {
    using Style = modules::hint_thinking_style_setting::Style;
    constexpr Role dpad[]{Role::Up, Role::Right, Role::Down, Role::Left};
    constexpr modules::Rgb sentinel{7U, 11U, 19U};
    const auto energy = [](modules::Rgb color) { return uint16_t(color.r + color.g + color.b); };
    for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2}) {
        const uint8_t base = owner == modules::UiOwner::P2 ? 6U : 0U;
        const auto checkPattern = [&](const Overlay& frame) {
            unsigned total=0U; uint16_t peak=0U;
            std::array<modules::Rgb,4U> colors{};
            for(uint8_t position=0U;position<4U;++position){
                colors[position]=frame.controlPixels[base+static_cast<uint8_t>(dpad[position])];
                const auto e=energy(colors[position]); total+=e; if(e>peak)peak=e;
                results.Check(e>=31U,"every Beachball position stays above its one-eighth-strength floor");
            }
            results.Check(total>=348U && total<=356U && peak>=143U,
                          "Beachball continuously shares one bright head between adjacent positions with the reduced one-eighth tail envelope");
            for(uint8_t x=0U;x<4U;++x)for(uint8_t y=x+1U;y<4U;++y){
                const auto a=colors[x],b=colors[y];const int ea=energy(a),eb=energy(b);
                const int distance=std::abs(int(a.r)*255/ea-int(b.r)*255/eb)+
                    std::abs(int(a.g)*255/ea-int(b.g)*255/eb)+std::abs(int(a.b)*255/ea-int(b.b)*255/eb);
                results.Check(distance>100,"four simultaneous Beachball positions have distinct quarter-offset hues");
            }
        };
        for (const uint32_t elapsed : {0U,1U,137U,249U,250U,499U,500U,501U,999U,1000U,
                                      1499U,1500U,1999U,2000U,2001U,3999U}) {
            Overlay frame,next;
            modules::ComposeControlLedThinkingFeedback({owner,elapsed,Style::Beachball},frame);
            modules::ComposeControlLedThinkingFeedback({owner,elapsed+1U,Style::Beachball},next);
            checkPattern(frame);
            for(uint8_t position=0U;position<4U;++position){
                const auto a=frame.controlPixels[base+static_cast<uint8_t>(dpad[position])];
                const auto b=next.controlPixels[base+static_cast<uint8_t>(dpad[position])];
                const auto gentle=[](uint8_t x,uint8_t y){return (x>y?x-y:y-x)<=6U;};
                results.Check(gentle(a.r,b.r)&&gentle(a.g,b.g)&&gentle(a.b,b.b),
                              "each fixed physical LED changes continuously through head handoffs and the two-second wrap");
            }
        }
        Overlay initial,quarter;
        modules::ComposeControlLedThinkingFeedback({owner,0U,Style::Beachball},initial);
        modules::ComposeControlLedThinkingFeedback({owner,500U,Style::Beachball},quarter);
        results.Check(!Same(initial.controlPixels[base+static_cast<uint8_t>(Role::Up)],
                            quarter.controlPixels[base+static_cast<uint8_t>(Role::Right)]),
                      "the traveling bright head changes hue rather than phase-cancelling to a fixed color");
        Overlay start, repeat, minimal, minimalLater;
        start.controlPixels.fill(sentinel);
        modules::ComposeControlLedThinkingFeedback({owner, 0U, Style::Beachball}, start);
        modules::ComposeControlLedThinkingFeedback({owner, 2000U, Style::Beachball}, repeat);
        modules::ComposeControlLedThinkingFeedback({owner, 777U, Style::Beachball, {}, true}, minimal);
        modules::ComposeControlLedThinkingFeedback({owner, 1777U, Style::Beachball, {}, true}, minimalLater);
        uint16_t mask = 0U;
        for (uint8_t position = 0U; position < 4U; ++position) {
            const uint8_t index = base + static_cast<uint8_t>(dpad[position]);
            mask |= static_cast<uint16_t>(1U << index);
            results.Check(Same(start.controlPixels[index], repeat.controlPixels[index]),
                          "Beachball completes exactly one clockwise trip and spectrum cycle in two seconds");
            results.Check(Same(minimal.controlPixels[index], modules::ScaleRgbSaturatingBy256(
                              modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(position * 64U)), 128U)) &&
                              Same(minimal.controlPixels[index], minimalLater.controlPixels[index]),
                          "Minimal Beachball preserves the static dim four-hue rainbow without color motion");
        }
        results.Check(start.controlMask == mask, "Beachball owns exactly the mover D-pad, never X/O or the opposite bank");
        for (uint8_t index = 0U; index < 12U; ++index)
            if ((mask & (1U << index)) == 0U)
                results.Check(Same(start.controlPixels[index], sentinel), "Beachball leaves existing X/O and opponent pixels untouched");
        for (const bool swapped : {false, true}) {
            const bool physicalP2 = (owner == modules::UiOwner::P2) != swapped;
            const uint8_t fitted[]{static_cast<uint8_t>(physicalP2 ? 5U : 27U),
                static_cast<uint8_t>(physicalP2 ? 6U : 28U), static_cast<uint8_t>(physicalP2 ? 7U : 29U),
                static_cast<uint8_t>(physicalP2 ? 8U : 30U)};
            const uint8_t emulated[]{static_cast<uint8_t>(physicalP2 ? 5U : 21U),
                static_cast<uint8_t>(physicalP2 ? 4U : 20U), static_cast<uint8_t>(physicalP2 ? 3U : 19U),
                static_cast<uint8_t>(physicalP2 ? 2U : 18U)};
            for (const bool v3 : {false, true}) {
                hardware::PhysicalLedFrame frame; frame.fill(sentinel);
                hardware::ApplyFixedPhysicalStatusOverlay(v3 ? hardware::BoardRevision::V3S3
                    : hardware::BoardRevision::V3_3S3, start, frame, swapped);
                for (uint16_t led = 0U; led < frame.size(); ++led) {
                    auto expected = sentinel;
                    for (uint8_t position = 0U; position < 4U; ++position)
                        if (led == 256U + (v3 ? emulated[position] : fitted[position]))
                            expected = start.controlPixels[base + static_cast<uint8_t>(dpad[position])];
                    results.Check(Same(frame[led], expected),
                                  "Beachball maps four hues and its moving head through V3.3/V3 bank swaps, preserving every other LED");
                }
            }
        }
    }
}

void CheckReusableBeachball(fxe::test_support::TestResults& results) {
    for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2}) {
        for (const bool minimal : {false, true}) {
            for (const uint16_t scale : {0U, 128U, 256U}) {
                modules::ControlLedThinkingFeedback operation;
                operation.owner = owner;
                operation.elapsedMs = 1377U;
                operation.minimalAnimations = minimal;
                operation.scale256 = scale;
                Overlay arbitrary, explicitHint;
                modules::ComposeControlLedThinkingFeedback(operation, arbitrary);
                modules::ComposeControlLedThinkingFeedback(
                    {owner, 1377U, modules::hint_thinking_style_setting::Style::Beachball, {}, minimal, scale}, explicitHint);
                bool same = arbitrary.controlMask == explicitHint.controlMask &&
                    arbitrary.emulatedControlMask == explicitHint.emulatedControlMask &&
                    arbitrary.controlAlpha255 == explicitHint.controlAlpha255;
                for (size_t i = 0U; i < arbitrary.controlPixels.size(); ++i)
                    same = same && Same(arbitrary.controlPixels[i], explicitHint.controlPixels[i]);
                for (size_t i = 0U; i < arbitrary.emulatedControlPixels.size(); ++i)
                    same = same && Same(arbitrary.emulatedControlPixels[i], explicitHint.emulatedControlPixels[i]);
                results.Check(same && operation.style == modules::hint_thinking_style_setting::Style::Beachball &&
                                  arbitrary.controlAlpha255[owner == modules::UiOwner::P2 ? 1U : 0U] == (scale * 255U + 128U) / 256U,
                              "an arbitrary operation renders default Beachball from caller-owned elapsed/owner/Minimal/fade exactly like Hint");
                operation.owner = modules::UiOwner::None;
                Overlay completed;
                modules::ComposeControlLedThinkingFeedback(operation, completed);
                results.Check(completed.controlMask == 0U && completed.emulatedControlMask == 0U,
                              "completed work contributes no stale rainbow to the next fresh overlay");
                hardware::PhysicalLedFrame frame; constexpr modules::Rgb ordinary{13U, 17U, 23U}; frame.fill(ordinary);
                hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3, completed, frame);
                bool unchanged = true;
                for (const auto pixel : frame) unchanged = unchanged && Same(pixel, ordinary);
                results.Check(unchanged, "ending an arbitrary operation preserves every ordinary physical LED in the next frame");
            }
        }
    }
}

void CheckCauldron(fxe::test_support::TestResults& results) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    namespace anim=modules::animation;
    const auto& token=anim::LED_FRAME_TOKENS.hintCauldronBreath;
    results.Check(token.phase.periodMs==1600U&&token.curve.minValue==10U&&token.curve.maxValue==255U,
        "Cauldron period/range use the existing animation-token configuration");
    results.Check(anim::SampleValue({0U},token)==10U&&anim::SampleValue({800U},token)==255U&&
        anim::SampleValue({1600U},token)==10U&&anim::SampleValue({400U},token)<70U&&
        std::abs(int(anim::SampleValue({400U},token))-int(anim::SampleValue({1200U},token)))<=3,
        "one temporal gamma correction preserves exact bounds and cycle symmetry within8-bit phase quantization");
    auto bounded=token;bounded.phase.periodMs=0U;
    results.Check(anim::SampleValue({500U},bounded)==10U,"zero period follows existing safe one-tick convention");
    bounded=token;bounded.curve.minValue=255U;bounded.curve.maxValue=10U;
    results.Check(anim::SampleValue({800U},bounded)==255U&&anim::SampleValue({0U},bounded)==10U,
        "reversed configured limits remain bounded instead of wrapping brightness");
    for(uint32_t elapsed=0U;elapsed<=1600U;elapsed+=20U) {
        const auto sample = anim::SamplePhase({elapsed}, token.phase);
        const double phase = double(sample.progress255) / 255.0;
        const double ideal=10.0+245.0*std::pow((1.0-std::cos(6.283185307179586*phase))*0.5,2.2);
        results.Check(std::abs(double(anim::SampleValue({elapsed},token))-ideal)<=1.5,
            "every renderer-cadence sample follows the intended gamma2.2 cosine within rounding tolerance at the actual quantized phase");
    }
    unsigned previous=10U;
    for(uint32_t time=0U;time<=1600U;time+=20U) {
        const auto value=anim::SampleValue({time},token);
        results.Check(value>=10U&&value<=255U&&(value>previous?value-previous:previous-value)<=17U,
            "every render step stays bounded and gentle through the full cycle");
        previous=value;
    }
    for(const auto owner:{modules::UiOwner::P1,modules::UiOwner::P2}) {
        const uint8_t bank=owner==modules::UiOwner::P2?1U:0U;
        for(const bool swapped:{false,true}) {
            Overlay brew;
            modules::ComposeControlLedThinkingFeedback({owner,800U,
                modules::hint_thinking_style_setting::Style::CauldronBrewing,{0U,255U,0U},false,256U},brew);
            results.Check(brew.controlMask==(1U<<(bank*6U))&&brew.emulatedControlMask==0U,
                "Cauldron owns only the requesting player's fitted O and no emulated or other controls");
            hardware::PhysicalLedFrame frame{};
            hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,brew,frame,swapped);
            const auto target=256U+hardware::V3_3PhysicalIndexForLogicalControl(bank==1U,Role::O,swapped);
            for(size_t i=0U;i<frame.size();++i)
                results.Check(i==target?frame[i].g==255U&&frame[i].r==0U&&frame[i].b==0U:!Lit(frame[i]),
                    "mapped Cauldron preserves current selected green and leaves every other physical LED untouched");
            Overlay stillA,stillB;
            modules::ComposeControlLedThinkingFeedback({owner,0U,modules::hint_thinking_style_setting::Style::CauldronBrewing,{0U,255U,0U},true,256U},stillA);
            modules::ComposeControlLedThinkingFeedback({owner,800U,modules::hint_thinking_style_setting::Style::CauldronBrewing,{0U,255U,0U},true,256U},stillB);
            results.Check(Same(stillA.controlPixels[bank*6U],stillB.controlPixels[bank*6U])&&stillA.controlPixels[bank*6U].g==10U,
                "Minimal Animations retains a steady dim current-color O");
        }
    }
    fxe::ManualMillis::SetNowMs(4000U);
    for(const auto style:{modules::hint_thinking_style_setting::Style::None,
        modules::hint_thinking_style_setting::Style::PlayerClockwise,
        modules::hint_thinking_style_setting::Style::FoxieSweep,
        modules::hint_thinking_style_setting::Style::Beachball}) {
        Overlay overlay;
        modules::ComposeControlLedThinkingFeedback({modules::UiOwner::P1,237U,style,{0U,255U,0U},false,128U},overlay);
        hardware::PhysicalLedFrame before{};
        hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,overlay,before);
        modules::ControlLedFeedback feedback;modules::ControlLedFeedbackInputs inputs;
        inputs.playerColors[0]={0U,255U,0U};
        feedback.Trigger(modules::ControlFeedbackAction::Hint,modules::UiOwner::P1,Role::O);
        fxe::ManualMillis::SetNowMs(4075U);feedback.Compose(inputs,overlay);
        hardware::PhysicalLedFrame after{};
        hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,overlay,after);
        for(size_t i=0U;i<after.size();++i)
            results.Check(i==282U?after[i].g>0U:Same(after[i],before[i]),
                "quickbrew works with every existing style including None without changing any other mapped LED");
        fxe::ManualMillis::SetNowMs(4000U);
    }
}

int main() {
    fxe::test_support::TestResults results("CONTROL_LED_THINKING_FEEDBACK");
    namespace style = modules::hint_thinking_style_setting;
    results.Check(std::strcmp(style::DEFAULT_VALUE, "cauldron_brewing") == 0 &&
                      style::Parse(nullptr) == style::Style::CauldronBrewing &&
                      style::Parse("") == style::Style::CauldronBrewing &&
                      style::Parse("unknown") == style::Style::CauldronBrewing &&
                      style::Parse("player_clockwise") == style::Style::PlayerClockwise &&
                      style::Parse("foxie_sweep") == style::Style::FoxieSweep &&
                      style::Parse("none") == style::Style::None &&
                      style::Parse("beachball") == style::Style::Beachball &&
                      static_cast<uint8_t>(style::Style::Beachball) == 3U,
                  "fresh/invalid Hint thinking defaults to Cauldron while every explicitly saved style keeps its meaning");
    CheckCauldron(results);
    CheckBeachball(results);
    CheckReusableBeachball(results);
    constexpr Role sweep[]{Role::X, Role::Left, Role::Right, Role::O, Role::Right, Role::Left, Role::X};
    constexpr Role clockwise[]{Role::Up, Role::Right, Role::Down, Role::Left, Role::Up};
    constexpr modules::Rgb player{20U, 190U, 90U};
    for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2}) {
        const uint8_t base = owner == modules::UiOwner::P2 ? 6U : 0U;
        for (uint8_t step = 0U; step < 7U; ++step) {
            Overlay out;
            modules::ComposeControlLedThinkingFeedback({owner, step * 200U,
                modules::hint_thinking_style_setting::Style::FoxieSweep, player, false}, out);
            const modules::Rgb expected = (step + step / 6U) % 2U == 0U ? modules::Rgb{128U, 0U, 255U} : modules::Rgb{255U, 128U, 0U};
            for (uint8_t i = 0U; i < 12U; ++i) {
                results.Check(Same(out.controlPixels[i], i == base + static_cast<uint8_t>(sweep[step]) ? expected : modules::Rgb{}),
                    "Foxie sequence visits X Left Right O Right Left X with traveling established colors on only its owner bank");
            }
        }
        for (uint8_t step = 0U; step < 5U; ++step) {
            Overlay out;
            modules::ComposeControlLedThinkingFeedback({owner, step * 200U,
                modules::hint_thinking_style_setting::Style::PlayerClockwise, player, false}, out);
            for (uint8_t i = 0U; i < 12U; ++i) {
                results.Check(Same(out.controlPixels[i], i == base + static_cast<uint8_t>(clockwise[step]) ? player : modules::Rgb{}),
                    "player style moves Up Right Down Left clockwise in the exact chosen color, leaving X O and opponent off");
            }
        }
        for (uint32_t boundary : {200U, 400U, 600U, 800U, 1000U, 1200U, 2400U, 3600U}) {
            Overlay a, b;
            modules::ComposeControlLedThinkingFeedback({owner, boundary - 1U, {}, player, false}, a);
            modules::ComposeControlLedThinkingFeedback({owner, boundary + 1U, {}, player, false}, b);
            for (uint8_t i = 0U; i < 12U; ++i) {
                const auto close = [](uint8_t x, uint8_t y) { return (x > y ? x - y : y - x) <= 4U; };
                results.Check(close(a.controlPixels[i].r,b.controlPixels[i].r) && close(a.controlPixels[i].g,b.controlPixels[i].g) && close(a.controlPixels[i].b,b.controlPixels[i].b),
                    "crossfade remains continuous at every reversal and repeat boundary");
            }
        }
        for (auto style : {modules::hint_thinking_style_setting::Style::FoxieSweep,
                           modules::hint_thinking_style_setting::Style::PlayerClockwise,
                           modules::hint_thinking_style_setting::Style::Beachball}) {
            Overlay a,b;
            modules::ComposeControlLedThinkingFeedback({owner, 0U, style, player, true}, a);
            modules::ComposeControlLedThinkingFeedback({owner, 777U, style, player, true}, b);
            for (uint8_t i = 0U; i < 12U; ++i)
                results.Check(Same(a.controlPixels[i],b.controlPixels[i]), "Minimal Animations keeps a stable bank cue");
        }
        for (bool swapped : {false,true}) {
            const bool physicalP2 = (owner == modules::UiOwner::P2) != swapped;
            const uint8_t expectedV3[]{static_cast<uint8_t>(physicalP2 ? 5U : 21U),
                static_cast<uint8_t>(physicalP2 ? 4U : 20U),
                static_cast<uint8_t>(physicalP2 ? 3U : 19U),
                static_cast<uint8_t>(physicalP2 ? 2U : 18U)};
            for (bool clockwiseStyle : {false, true}) {
                const uint8_t sweepPositions[]{0U,1U,2U,3U,2U,1U,0U};
                const uint8_t count = clockwiseStyle ? 5U : 7U;
                for (uint8_t step = 0U; step < count; ++step) {
                    Overlay feedback;
                    modules::ComposeControlLedThinkingFeedback({owner, step * 200U,
                        clockwiseStyle ? modules::hint_thinking_style_setting::Style::PlayerClockwise
                                       : modules::hint_thinking_style_setting::Style::FoxieSweep, player, false}, feedback);
                    hardware::PhysicalLedFrame frame;
                    constexpr modules::Rgb sentinel{1U,2U,3U};
                    frame.fill(sentinel);
                    hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3S3, feedback, frame, swapped);
                    const auto color = clockwiseStyle ? player : (step + step / 6U) % 2U == 0U
                        ? modules::Rgb{128U,0U,255U} : modules::Rgb{255U,128U,0U};
                    for (uint16_t led = 0U; led < frame.size(); ++led) {
                        auto expectedColor = sentinel;
                        for (uint8_t position = 0U; position < 4U; ++position)
                            if (led == 256U + expectedV3[position]) expectedColor = position ==
                                (clockwiseStyle ? step % 4U : sweepPositions[step]) ? color : modules::Rgb{};
                        results.Check(Same(frame[led],expectedColor),
                            "V3 middle-four emulation visits player-left-to-right positions through shared home mapping and touches only the owner's four pixels");
                    }
                }
            }
            Overlay emulatedPreview;
            modules::ComposeControlLedColorPreview(owner, player, emulatedPreview);
            hardware::PhysicalLedFrame v3{};
            hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3S3, emulatedPreview, v3, swapped);
            for (auto index : expectedV3) results.Check(Same(v3[256U+index],player),
                "V3 color edit takes all four middle LEDs, preserving physical end under bank swaps");
            Overlay out;
            modules::ComposeControlLedColorPreview(owner, player, out);
            hardware::PhysicalLedFrame physical{};
            hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3, out, physical, swapped);
            const bool p2 = (owner == modules::UiOwner::P2) != swapped;
            const uint8_t expected[]{static_cast<uint8_t>(p2 ? 9U : 26U), static_cast<uint8_t>(p2 ? 5U : 27U), static_cast<uint8_t>(p2 ? 6U : 28U), static_cast<uint8_t>(p2 ? 7U : 29U), static_cast<uint8_t>(p2 ? 8U : 30U), static_cast<uint8_t>(p2 ? 4U : 31U)};
            for (auto index : expected) {
                const auto c = physical[256U + index];
                results.Check(c.r == player.r && c.g == player.g && c.b == player.b,
                    "final V3.3 mapper uses corrected P2 O4/X9 and P1 O26/X31 status indices with shared button bank swapping");
            }
        }
    }

    for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2}) {
        for (const bool minimal : {false, true}) {
            Overlay untouched;
            untouched.controlMask = 0x5a5U;
            untouched.emulatedControlMask = 0xa5U;
            untouched.controlPixels.fill(player);
            untouched.emulatedControlPixels.fill(player);
            untouched.controlAlpha255 = {37U, 91U};
            const auto before = untouched;
            modules::ComposeControlLedThinkingFeedback(
                {owner, 777U, style::Style::None, player, minimal, 128U}, untouched);
            bool same = untouched.controlMask == before.controlMask &&
                untouched.emulatedControlMask == before.emulatedControlMask &&
                untouched.controlAlpha255 == before.controlAlpha255;
            for (size_t i = 0U; i < untouched.controlPixels.size(); ++i)
                same = same && Same(untouched.controlPixels[i], before.controlPixels[i]);
            for (size_t i = 0U; i < untouched.emulatedControlPixels.size(); ++i)
                same = same && Same(untouched.emulatedControlPixels[i], before.emulatedControlPixels[i]);
            results.Check(same, "None leaves existing pixels, ownership masks and alpha untouched in every animation mode");
            Overlay preview;
            modules::ComposeControlLedThinkingFeedback(
                {owner, 777U, style::Style::None, player, minimal}, preview);
            modules::ComposeControlLedColorPreview(owner, player, preview);
            results.Check(preview.controlMask != 0U && preview.emulatedControlMask != 0U &&
                              preview.controlAlpha255[owner == modules::UiOwner::P2 ? 1U : 0U] == 255U,
                          "None suppresses thinking only and leaves physical palette preview available");
        }
    }

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 100U);
    modules::ControlLedThinkingPresentation presentation;
    presentation.SetRequested(modules::UiOwner::P1);
    results.Check(presentation.Scale256() == 0U, "request begins dark without a brightness step");
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 50U);
    const auto quarter = presentation.Scale256();
    fxe::ManualMillis::SetNowMs(UINT32_MAX);
    const auto half = presentation.Scale256();
    fxe::ManualMillis::SetNowMs(99U);
    results.Check(quarter > 0U && quarter < half && half <= 132U && presentation.Scale256() == 256U,
        "eased fade reaches full intensity200ms later across millis wrap");
    presentation.SetRequested(modules::UiOwner::None);
    results.Check(presentation.Scale256() == 256U, "stop starts from its actual brightness");
    fxe::ManualMillis::SetNowMs(199U);
    results.Check(presentation.Scale256() == 128U, "stop fades rather than clearing the bank");
    presentation.SetRequested(modules::UiOwner::P1);
    results.Check(presentation.Scale256() == 128U && presentation.ElapsedMs() == 300U,
        "same-player restart preserves intensity and motion during outgoing fade");
    fxe::ManualMillis::SetNowMs(399U);
    results.Check(presentation.Scale256() == 256U, "restart completes without per-frame accumulation");
    presentation.SetRequested(modules::UiOwner::P2);
    results.Check(presentation.Scale256() == 0U && presentation.ElapsedMs() == 0U,
        "a different player gets a fresh episode without an old player's tail");
    Overlay faded;
    modules::ComposeControlLedThinkingFeedback({modules::UiOwner::P1, 200U, {}, player, false, 128U}, faded);
    constexpr modules::Rgb baseline{20U,30U,40U};
    hardware::PhysicalLedFrame blended; blended.fill(baseline);
    hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3S3, faded, blended);
    results.Check(Same(blended[256U+20U], modules::AlphaBlendRgb(baseline, {255U,128U,0U},128U)) &&
                  Same(blended[256U+21U], modules::AlphaBlendRgb(baseline, {},128U)),
        "V3 bank crossfades against ordinary Turn instead of dimming to black before an abrupt handoff");
    modules::ComposeControlLedColorPreview(modules::UiOwner::P2,player,faded);
    results.Check(faded.controlAlpha255[0U] == 128U && faded.controlAlpha255[1U] == 255U,
        "a thinking fade cannot dim the opposite player's palette preview");

    modules::physical_palette_shortcut::EditPreview preview;
    modules::ButtonEvent event; event.owner = modules::UiOwner::P1; event.button = modules::UserButton::Up; event.event = fxe::Button::Event::Press;
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 200U); preview.Observe(event,true);
    fxe::ManualMillis::SetNowMs(100U);
    results.Check(preview.VisibleOwners() == 1U,"palette edit preview survives millis wrap while held");
    event.event = fxe::Button::Event::Repeat; preview.Observe(event,true);
    fxe::ManualMillis::SetNowMs(999U);
    results.Check(preview.VisibleOwners() == 1U,"real repeat refreshes the bounded cosmetic edit lease");
    fxe::ManualMillis::SetNowMs(1000U);
    results.Check(preview.VisibleOwners() == 0U,"missed release expires without stale ownership");
    event.event = fxe::Button::Event::Press;preview.Observe(event,true);
    event.event = fxe::Button::Event::Release;preview.Observe(event,false);
    results.Check(preview.VisibleOwners() == 0U,"release restores thinking immediately");
    // Supporting compositor evidence for the transient search/fade seam: the
    // raw VirtualLitch journey uses synchronous CPU polling and reaches its
    // directed move before it can observe an in-flight search frame.
    {
        namespace composer = modules::led_frame_composer;
        composer::ComposeSettings settings;
        settings.cpuOwner = modules::UiOwner::P2;
        settings.farEndPieceWheel = 255U;
        settings.thinkingStyle = style::Style::CauldronBrewing;
        runtime::InteractionModelSnapshot snapshot;
        snapshot.game.sideToMove = modules::SideToMove::Black;
        snapshot.game.cpuThinking = true;
        snapshot.render.thinkingControlCpu = true;
        snapshot.render.thinkingControlOwner = modules::UiOwner::P2;
        snapshot.render.thinkingControlScale256 = 128U;
        snapshot.render.thinkingControlElapsedMs = 800U;
        fxe::ManualMillis::SetNowMs(2048U);
        const auto compose = [&]() {
            composer::ComposeInputs inputs;
            inputs.resolvedSettings = &settings;
            inputs.interactionSnapshot = &snapshot;
            modules::BoardLedFrame board{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
            std::array<modules::Rgb, 64> grid{};
            Overlay overlay;
            composer::Compose(inputs, board, edge, grid, nullptr, nullptr,
                nullptr, nullptr, &overlay);
            return overlay;
        };
        constexpr uint16_t cpuPair = (1U << (6U + static_cast<uint8_t>(Role::Up))) |
                                     (1U << (6U + static_cast<uint8_t>(Role::Down)));
        const auto thinking = compose();
        results.Check((thinking.controlMask & cpuPair) == 0U &&
            Lit(thinking.controlPixels[6U]) && thinking.controlAlpha255[1] == 128U,
            "CPU thinking owns its bank without a beacon sharing its fade");
        snapshot.game.cpuThinking = false;
        const auto retiring = compose();
        results.Check((retiring.controlMask & cpuPair) == 0U &&
            Lit(retiring.controlPixels[6U]) && retiring.controlAlpha255[1] == 128U,
            "retiring CPU thinking still owns the bank after work completes");
        snapshot.game.cpuThinking = true;
        settings.thinkingStyle = style::Style::None;
        const auto none = compose();
        results.Check((none.controlMask & cpuPair) == cpuPair &&
            none.controlAlpha255[1] == 255U &&
            Same(none.controlPixels[7U], none.controlPixels[9U]),
            "None leaves the paired marker visible on the CPU turn without a thinking fade");
        settings.thinkingStyle = style::Style::CauldronBrewing;
        snapshot.game.cpuThinking = false;
        snapshot.render.thinkingControlScale256 = 0U;
        const auto complete = compose();
        results.Check((complete.controlMask & cpuPair) == cpuPair,
            "completed CPU thinking releases the pair at the existing fade boundary");
    }
    return results.Finish();
}
