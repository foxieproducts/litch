#include "modules/boot_display.hpp"
#include "modules/boot_fox_asset.hpp"
#include "modules/litchcraft_session.hpp"
#include "foxie_embedded/millis.hpp"
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[BOOT_DISPLAY_FAIL] %s\n", message); }
}
bool Same(modules::Rgb a, modules::Rgb b) { return a.r==b.r && a.g==b.g && a.b==b.b; }
void Begin(modules::BootDisplay& display, uint32_t now = 1000U) {
    fxe::ManualMillis::SetNowMs(now);
    display.Begin(modules::BootDisplayChoice::Fox, true, {});
    display.ObserveBoard(true, 0ULL);
    hardware::PhysicalLedFrame frame{};
    display.Apply(frame, false, true);
    display.Presented();
}
void Policy() {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    modules::BootDisplay display;
    Check(!display.IsComplete(), "startup is undecided before its owner chooses a display");
    hardware::PhysicalLedFrame frame{};
    frame.fill({12U, 34U, 56U});
    const auto live = frame;
    display.Begin(modules::BootDisplayChoice::Fox, true, {});
    display.ObserveBoard(false, 0ULL);
    display.Apply(frame, false, false, nullptr, true);
    Check(display.IsPending() && !display.IsComplete() && Same(frame[0], live[0]), "unknown occupancy never means a completed startup decision");
    for (uint8_t edge = 0U; edge < modules::EDGE_LED_COUNT; ++edge)
        Check(Same(frame[256U + hardware::V3_3PhysicalIndexForSemanticStatus(edge)], {}),
              "pending startup suppresses ordinary turn status before Fox");
    display.ObserveBoard(true, 1ULL);
    Check(display.IsActive(), "a partial first board admits the selected startup display");
    display.ObserveBoard(true, 0ULL);
    Check(!display.IsActive() && !display.IsPending(), "handling the admitted occupied board retires without rearming");
    display.Begin(modules::BootDisplayChoice::Fox,true,{});
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    display.ObserveBoard(true,opening);
    Check(display.IsActive(), "an initial board without proven ongoing-game authority admits startup");
    display.Begin(modules::BootDisplayChoice::Fox,true,{});
    display.ObserveBoard(true,opening);
    display.ObserveBoard(true,opening);
    Check(display.IsActive() && !display.IsComplete(), "unchanged complete initial board admits the selected episode");
    display.ObserveBoard(true,opening & ~(UINT64_C(1)<<52U));
    Check(display.IsComplete(), "real pickup cancels the occupied boot episode immediately");
    display.Begin(modules::BootDisplayChoice::Fox,true,{});
    display.ObserveBoard(true,opening | (UINT64_C(1)<<27U));
    display.ObserveBoard(true,opening | (UINT64_C(1)<<27U));
    Check(display.IsActive(), "unchanged complete initial Duck setup admits the selected episode");
    display.ObserveBoard(true,opening | (UINT64_C(1)<<28U));
    Check(display.IsComplete(), "moving the admitted Duck cancels rather than changing the boot baseline");
    constexpr uint64_t resumed = opening ^ (UINT64_C(1) << 12U) ^ (UINT64_C(1) << 28U);
    const modules::BootDisplay::Context game{0U, 0U, resumed, true};
    display.Begin(modules::BootDisplayChoice::Fox, true, game);
    display.ObserveBoard(false, resumed);
    Check(display.IsPending(), "unknown Hall keeps the restored-game decision pending");
    display.ObserveBoard(true, resumed);
    Check(display.IsComplete() && !display.IsActive(), "fresh exact ongoing-game occupancy skips startup");
    display.ObserveBoard(true, 0ULL);
    Check(!display.IsActive(), "clearing a skipped game cannot rearm startup");
    display.Begin(modules::BootDisplayChoice::Fox, true, game);
    display.ObserveBoard(true, resumed ^ (UINT64_C(1) << 28U));
    Check(display.IsActive(), "a moved piece prevents the saved-game skip");
    display.ObserveBoard(true, resumed);
    Check(display.IsComplete(), "later piece handling cancels without changing the admitted baseline");
    display.Begin(modules::BootDisplayChoice::Fox, true, game);
    display.ObserveBoard(true, resumed, false);
    Check(display.IsActive(), "Litchcraft mode cannot inherit a stale chess-game skip");
    Begin(display);
    for (uint16_t i=0; i<256U; ++i) Check(Same(frame[i],live[i]), "Begin helper owns no destination buffer");
    frame=live;const auto initialOpacity=display.Apply(frame,false,true,nullptr,true);
    Check(initialOpacity==0U, "episode starts with zero post-budget board opacity");
    for (uint16_t i=0; i<256U; ++i) Check(Same(frame[i],modules::boot_fox_asset::PIXELS[i]), "budgeting sees the full-demand Fox even during fade");
    for (uint8_t edge = 0U; edge < modules::EDGE_LED_COUNT; ++edge)
        Check(Same(frame[256U + hardware::V3_3PhysicalIndexForSemanticStatus(edge)], {}),
              "Fox owns ordinary turn status until its episode completes");
    for (uint8_t control = 0U; control < 12U; ++control) {
        const auto index = 256U + hardware::V3_3PhysicalIndexForLogicalControl(
            control >= 6U, static_cast<hardware::ControlLedRole>(control % 6U), false);
        Check(Same(frame[index], live[index]), "dedicated control output retains priority during Fox");
    }
    frame = live;
    display.Apply(frame, false, true, nullptr, false);
    for (uint8_t edge = 0U; edge < modules::EDGE_LED_COUNT; ++edge) {
        const auto index = 256U + hardware::V3_3PhysicalIndexForSemanticStatus(edge);
        Check(Same(frame[index], live[index]), "required semantic status retains priority during Fox");
    }
    fxe::ManualMillis::SetNowMs(1100U);frame=live;
    Check(display.Apply(frame,false,true)==0U,"100ms black lead-in precedes the unchanged fade");
    fxe::ManualMillis::SetNowMs(1225U);frame=live;
    Check(display.Apply(frame,false,true)==128U && Same(frame[102U],modules::boot_fox_asset::PIXELS[102U]), "250ms midpoint dims the already-budgeted full-demand image");
    fxe::ManualMillis::SetNowMs(1350U);frame=live;display.Apply(frame,false,true);
    for (uint16_t i=0; i<256U; ++i) Check(Same(frame[i],modules::boot_fox_asset::PIXELS[i]), "Minimal shows exact selected peak asset");
    fxe::ManualMillis::SetNowMs(1350U);frame=live;display.Apply(frame,true,true);
    for (uint16_t i=0; i<256U; ++i) {
        const uint16_t mapped=(63U-i/4U)*4U+(i%4U+2U)%4U;
        Check(Same(frame[mapped],modules::boot_fox_asset::PIXELS[i]), "all 256 positions rotate once including Top/Bottom");
    }
    unsigned yellow=0, changedYellow=0;
    hardware::PhysicalLedFrame phaseA=live,phaseB=live;
    fxe::ManualMillis::SetNowMs(1375U);display.Apply(phaseA,false,false);
    fxe::ManualMillis::SetNowMs(1625U);display.Apply(phaseB,false,false);
    for (uint16_t i=0;i<256U;++i) {
        if(modules::boot_fox_asset::IsYellow(i)) { ++yellow;if(!Same(phaseA[i],phaseB[i]))++changedYellow; }
        else Check(Same(phaseA[i],phaseB[i]), "sparkles never change orange, purple or negative space");
    }
    Check(yellow==32U && changedYellow>0U, "selected source-yellow mask sparkles deterministically");
    fxe::ManualMillis::SetNowMs(1850U);frame=live;display.Apply(frame,false,true);
    Check(display.IsActive() && Same(frame[102U], modules::boot_fox_asset::PIXELS[102U]),
          "Fox remains fully visible at the old 750ms cutoff");
    fxe::ManualMillis::SetNowMs(2350U);frame=live;display.Apply(frame,false,true);
    for(uint16_t i=0;i<256U;++i) {
        auto logo=modules::boot_fox_asset::PIXELS[i];
        Check(Same(frame[i],{static_cast<uint8_t>((logo.r+12U+1U)/2U),static_cast<uint8_t>((logo.g+34U+1U)/2U),static_cast<uint8_t>((logo.b+56U+1U)/2U)}), "crossfade uses current destination midpoint");
    }
    auto changed=live;changed.fill({90U,80U,70U});display.Apply(changed,false,true);
    Check(changed[102U].r!=frame[102U].r, "destination is recomposed while blending, never frozen");
    fxe::ManualMillis::SetNowMs(2600U);frame=live;display.Apply(frame,false,true);
    Check(!display.IsActive() && Same(frame[0],live[0]) && Same(frame[255],live[255]), "exact 1600ms expiry reveals live output");
    display.ObserveBoard(true,0ULL);Check(!display.IsActive(),"expiry never rearms");
    Begin(display);display.ObserveBoard(true,1ULL);Check(!display.IsActive(),"piece placement cancels immediately");
    Begin(display);display.UpdatePriority(true,{});Check(!display.IsActive(),"priority cancels permanently");
    Begin(display);display.UpdatePriority(false,{1U,0U});Check(!display.IsActive(),"mode generation cancels");
    Begin(display);
    Check(!display.ConsumeButton({modules::UserButton::P1O,fxe::Button::Event::Press}) && display.IsComplete(),
          "manual Hint cancels startup and retains its normal control owner");
    for(auto x:{modules::UserButton::P1X,modules::UserButton::P2X}) {
        Begin(display);
        Check(display.ConsumeButton({x,fxe::Button::Event::Press}),"either X skips");
        Check(display.ConsumeButton({x,fxe::Button::Event::LongHold}),"held skip cannot open reset/menu");
        Check(display.ConsumeButton({x,fxe::Button::Event::Release}),"cancel release cannot Undo or erase");
        Check(!display.ConsumeButton({x,fxe::Button::Event::Press}),"next separate X belongs to normal mode");
    }
    modules::LitchcraftSession noArtwork;
    display.Begin(modules::BootDisplayChoice::StoredArtwork,true,{});
    frame = live;
    display.Apply(frame, false, false, nullptr, true);
    for (uint16_t index = 0U; index < 256U; ++index)
        Check(Same(frame[index], {}), "stored intro holds the board black before Hall admission");
    for (uint8_t control = 0U; control < 12U; ++control) {
        const auto index = 256U + hardware::V3_3PhysicalIndexForLogicalControl(
            control >= 6U, static_cast<hardware::ControlLedRole>(control % 6U), false);
        Check(Same(frame[index], live[index]), "pending stored intro retains dedicated controls");
    }
    display.ObserveBoard(true,0ULL);display.UpdateArtwork(noArtwork,false);
    Check(display.NeedsArtwork(),"stored intro waits for the existing recovery owner");
    frame = live;
    display.Apply(frame, false, false, nullptr, false);
    for (uint16_t index = 0U; index < 256U; ++index)
        Check(Same(frame[index], {}), "stored intro holds the admitted board black until artwork starts");
    for (size_t index = 256U; index < frame.size(); ++index)
        Check(Same(frame[index], live[index]), "required status retains priority while stored intro waits");
    display.UpdatePriority(true, {});
    frame = live;
    display.Apply(frame, false, false, nullptr, true);
    for (size_t index = 0U; index < frame.size(); ++index)
        Check(Same(frame[index], live[index]), "urgent cancellation releases the pending stored surface");
    display.Begin(modules::BootDisplayChoice::StoredArtwork,true,{});
    display.ObserveBoard(true,0ULL);
    display.UpdateArtwork(noArtwork,true);frame=live;display.Apply(frame,false,false);
    Check(!display.IsActive() && !display.IsPending() && Same(frame[102U],live[102U]),
        "missing stored artwork skips without showing a fox or changing live output");
    display.Begin(modules::BootDisplayChoice::Off,true,{});display.ObserveBoard(true,0ULL);Check(!display.IsActive() && display.IsComplete(),"Off completes the explicit no-animation decision");
    frame = live;
    display.Apply(frame, false, false, nullptr, true);
    for (size_t index = 0U; index < frame.size(); ++index)
        Check(Same(frame[index], live[index]), "Off keeps the complete live surface");
    display.Begin(modules::BootDisplayChoice::StoredArtwork, true, game);
    display.ObserveBoard(true, resumed);
    frame = live;
    display.Apply(frame, false, false, nullptr, true);
    Check(display.IsComplete(), "exact resumed game skips the stored intro");
    for (size_t index = 0U; index < frame.size(); ++index)
        Check(Same(frame[index], live[index]), "resumed-game skip keeps the complete live surface");
    display.Begin(modules::BootDisplayChoice::Fox,false,{});display.ObserveBoard(true,0ULL);Check(!display.IsActive(),"other topology/factory admission skips");
    Begin(display,std::numeric_limits<uint32_t>::max()-50U);
    fxe::ManualMillis::SetNowMs(299U);frame=live;display.Apply(frame,false,true);
    Check(Same(frame[102U],modules::boot_fox_asset::PIXELS[102U]),"fade time remains correct across millis wrap");
    display.Begin(modules::BootDisplayChoice::Fox,true,{});display.ObserveBoard(true,0ULL);
    fxe::ManualMillis::SetNowMs(50000U);frame=live;const auto delayedOpacity=display.Apply(frame,false,true);
    Check(display.IsActive() && delayedOpacity==0U,"slow service handoff/denied output cannot consume invisible intro");
    display.Presented();fxe::ManualMillis::SetNowMs(50350U);frame=live;display.Apply(frame,false,true);
    Check(Same(frame[102U],modules::boot_fox_asset::PIXELS[102U]),"first accepted black anchors complete visible fade");
    std::printf("[BOOT_DISPLAY] state_bytes=%zu asset_bytes=%zu yellow=%u would_catch_empty_boot_trigger=yes bypass=SDK/optics\n",sizeof(display),sizeof(modules::boot_fox_asset::PIXELS)+sizeof(modules::boot_fox_asset::YELLOW_MASK),yellow);
}
}
int main() { Policy();return failures ? 1 : 0; }
