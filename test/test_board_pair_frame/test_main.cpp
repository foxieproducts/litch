#include <cstdio>
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "virtual_litch_session_internal.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); }
}
bool Equal(modules::Rgb a, modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
void CheckAverage() {
    for (unsigned top = 0; top < 256U; ++top) {
        for (unsigned bottom = 0; bottom < 256U; ++bottom) {
            modules::BoardSquareLeds square{};
            square.top = {uint8_t(top), uint8_t(255U-top), uint8_t(top)};
            square.bottom = {uint8_t(bottom), uint8_t(255U-bottom), uint8_t(bottom)};
            const auto avg = modules::RoundedPieceAverage(square);
            Check(avg.r == (top + bottom + 1U) / 2U &&
                  avg.g == (511U - top - bottom) / 2U && avg.b == avg.r,
                  "all 65536 widened rounded channel pairs");
        }
    }
}
void CheckMapping() {
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    for (uint8_t i=0; i<64U; ++i) {
        board[i] = {{i,1,2}, {uint8_t(i+64U),3,4},
                    {uint8_t(i+128U),5,6}, {uint8_t(i+192U),7,8}};
    }
    for (uint8_t i=0; i<32U; ++i) { status[i] = {i,9,10}; }
    for (const auto revision : {hardware::BoardRevision::V1C3,
         hardware::BoardRevision::V2S3, hardware::BoardRevision::V3S3}) {
        const auto topology = hardware::BoardLedTopologyFor(revision);
        for (const bool rotate : {false,true}) {
            for (const bool statusRotate : {false,true}) {
                const auto physical = hardware::MapSemanticLedFrame(
                    {revision,rotate,statusRotate}, board, status);
                for (uint8_t sq=0; sq<64U; ++sq) {
                    const unsigned mapped = rotate ? 63U-sq : sq;
                    const unsigned base = mapped * topology.physicalLedsPerSquare;
                    if (revision == hardware::BoardRevision::V3S3) {
                        Check(Equal(physical[base], rotate ? board[sq].right : board[sq].left) &&
                              Equal(physical[base+1U], rotate ? board[sq].bottom : board[sq].top) &&
                              Equal(physical[base+2U], rotate ? board[sq].left : board[sq].right) &&
                              Equal(physical[base+3U], rotate ? board[sq].top : board[sq].bottom),
                              "all V3 four-color squares rotate both opposing pairs exactly once");
                    } else {
                        Check(Equal(physical[base+1U], modules::RoundedPieceAverage(board[sq])),
                              "V1/V2 inward pair averages before output");
                    }
                }
                if (topology.physicalStatusLedCount != 0U) {
                    for (uint8_t i=0; i<32U; ++i) {
                        const auto mapped = modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(i,statusRotate);
                        Check(Equal(physical[topology.physicalBoardLedCount+mapped],status[i]),
                              "status orientation and offset independent of pair mapping");
                    }
                }
                // Legacy ingress must be exactly equal-pair and retain V1 status blending once.
                const auto legacy = modules::ProjectLegacyBoardFrame(board);
                const auto equalPairs = modules::ExpandLegacyBoardFrame(legacy);
                const auto a = hardware::MapSemanticLedFrame({revision,rotate,statusRotate,1ULL},legacy,status);
                const auto b = hardware::MapSemanticLedFrame({revision,rotate,statusRotate,1ULL},equalPairs,status);
                for (unsigned i=0;i<hardware::MAX_PHYSICAL_LED_COUNT;++i) {
                    Check(Equal(a[i],b[i]),"legacy adapter parity including protected V1 status");
                }
            }
        }
    }
}
void CheckShutdownOrientationInvariant() {
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    board[0U].top = {240U, 12U, 3U};
    board[63U].top = {4U, 20U, 230U};
    status[0U] = {3U, 210U, 40U};
    status[16U] = {180U, 80U, 2U};
    for (const bool boardRotate : {false, true}) {
        for (const bool firstPlayerIsP2 : {false, true}) {
            const auto normal = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V3S3,
                 boardRotate != firstPlayerIsP2,
                 boardRotate != firstPlayerIsP2},
                board, status);
            const auto shutdown = hardware::MapRuntimeSemanticLedFrame(
                hardware::BoardRevision::V3S3,
                boardRotate, firstPlayerIsP2, board, status);
            for (size_t index = 0U; index < normal.size(); ++index) {
                Check(Equal(normal[index], shutdown[index]),
                      "shutdown starts from the exact normal player placement");
            }
        }
    }
}
void CheckShutdownAcceptedFrameInvariant() {
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    board[0U].top = {240U, 12U, 3U};
    board[63U].bottom = {4U, 20U, 230U};
    status[3U] = {3U, 210U, 40U};
    status[19U] = {180U, 80U, 2U};

    // Effective rotation=true models P2 at the flipped end, while the fixed
    // overlay uses literal board orientation=false.
    auto presented = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V3S3, true, false}, board, status);
    modules::litchcraft_physical_presentation::ApplyCanvasScale(
        presented,
        hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3)
            .physicalBoardLedCount);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay health{};
    health.Set(27U, {255U, 255U, 0U});
    health.Set(28U, modules::RGB_BLACK);
    hardware::ApplyFixedPhysicalStatusOverlay(
        hardware::BoardRevision::V3S3, health, presented);

    hardware::AcceptedPhysicalLedFrame accepted{};
    hardware::RecordAcceptedPhysicalLedFrame(
        true, presented, 173U, &accepted);
    Check(accepted.valid && accepted.brightnessScale256 == 173U,
          "completed pre-sleep render publishes its exact scale to shutdown");
    for (size_t index = 0U; index < presented.size(); ++index) {
        Check(Equal(accepted.frame[index], presented[index]),
              "first fade preserves flipped-P2 art, overlay, and health pixels");
    }

    hardware::PhysicalLedFrame denied{};
    denied.fill({99U, 88U, 77U});
    hardware::RecordAcceptedPhysicalLedFrame(false, denied, 256U, &accepted);
    Check(Equal(accepted.frame[0U], presented[0U]) &&
              accepted.brightnessScale256 == 173U,
          "denied pre-sleep render cannot replace the accepted fade source");
}
void CheckRotatedProductionHealthOverlay() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_board_pair_health.json";
    modules::SettingsStore settings(settingsConfig);
    (void)settings.Load();

    runtime::InteractionModelSnapshot snapshot{};
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.firstPlayerIsP2 = true;
    inputs.litchcraftActive = true;
    std::array<modules::Rgb, 64> grid{};

    modules::BoardLedFrame healthyBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> healthyStatus{};
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay healthyOverlay{};
    modules::led_frame_composer::Compose(
        inputs, healthyBoard, healthyStatus, grid, nullptr, nullptr, nullptr,
        nullptr, &healthyOverlay);
    auto healthy = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, true, true},
        healthyBoard, healthyStatus);
    hardware::ApplyFixedPhysicalStatusOverlay(
        hardware::BoardRevision::V2S3, healthyOverlay, healthy);

    inputs.runtimeHealth.state = runtime::RuntimeHealthState::DegradedUsable;
    modules::BoardLedFrame degradedBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> degradedStatus{};
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay degradedOverlay{};
    modules::led_frame_composer::Compose(
        inputs, degradedBoard, degradedStatus, grid, nullptr, nullptr, nullptr,
        nullptr, &degradedOverlay);
    auto degraded = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, true, true},
        degradedBoard, degradedStatus);
    hardware::ApplyFixedPhysicalStatusOverlay(
        hardware::BoardRevision::V2S3, degradedOverlay, degraded);

    const uint16_t usbFirst = hardware::BOARD_LED_COUNT + 27U;
    const uint16_t usbSecond = hardware::BOARD_LED_COUNT + 28U;
    bool onlyUsbPairChanged = true;
    for (uint16_t index = 0U; index < hardware::TOTAL_LED_COUNT; ++index) {
        if (index == usbFirst || index == usbSecond) continue;
        onlyUsbPairChanged = onlyUsbPairChanged &&
            Equal(healthy[index], degraded[index]);
    }
    Check(degradedOverlay.mask != 0U && onlyUsbPairChanged &&
              !Equal(healthy[usbFirst], degraded[usbFirst]) &&
              Equal(degraded[hardware::BOARD_LED_COUNT + 11U],
                    healthy[hardware::BOARD_LED_COUNT + 11U]) &&
              Equal(degraded[hardware::BOARD_LED_COUNT + 12U],
                    healthy[hardware::BOARD_LED_COUNT + 12U]),
          "rotated P2 production frames change only the physical USB health pair");
}
void CheckOverlaysAndPower() {
    modules::BoardLedFrame board{};
    board[0] = {{40,0,0},{100,20,0},{0,40,0},{20,0,80}};
    std::array<modules::Rgb,32> edge{};
    std::array<modules::Rgb,64> grid{};
    modules::led_frame_composer::ComposeInputs in;
    in.menuActive=true; in.menuVisualAlpha255=255U; in.menuBackgroundDimScale256=128U;
    modules::led_frame_composer::internal::ComposeContext ctx;
    ctx.in=&in; ctx.renderIntent.menuBackgroundDimVisible=true;
    modules::led_frame_composer::internal::ApplyMenuBackgroundDim(ctx,board,grid);
    Check(Equal(board[0].top,{50,10,0}) && Equal(board[0].bottom,{10,0,40}),
          "production menu dim scales distinct inward emitters once");
    modules::led_frame_composer::internal::ApplyGlobalBrightnessBoost(board,edge,grid);
    Check(Equal(board[0].top,{63,13,0}) && Equal(board[0].bottom,{13,0,50}),
          "production global boost preserves independent inward colors");
    modules::ApplyToBoardSquare(board[0],modules::BoardLedSlots::PiecePair, {11,22,33});
    Check(Equal(board[0].top,{11,22,33}) && Equal(board[0].bottom,{11,22,33}),
          "required piece owner replaces both inward emitters");
    for (const bool recovery : {false,true}) {
        modules::BoardLedFrame load{};
        for (auto& square : load) { square={{180,100,70},{200,40,10},{70,180,100},{10,60,220}}; }
        const auto physical = hardware::MapSemanticLedFrame({hardware::BoardRevision::V3S3,true,false},load,edge);
        const auto config=modules::led_render_policy::MakePowerBudgetPolicyConfig({true,recovery});
        modules::PowerBudgetPolicy policy(config);
        runtime::virtual_litch_internal::CaptureLedRenderer renderer(288U,true);
        modules::BudgetAwareLedPipeline pipeline(renderer,policy,
            modules::led_render_policy::MakePipelineConfig(true,32U));
        pipeline.SetBrightnessScale01(1.0f);
        pipeline.RenderFrame(physical.data(),288U);
        const auto stats=pipeline.GetLastStats();
        const auto top=renderer.Pixels()[1U],bottom=renderer.Pixels()[3U];
        Check(top.r!=bottom.r || top.g!=bottom.g || top.b!=bottom.b,
              "real RGBW pipeline retains independent inward output under cap");
        Check(uint32_t(stats.appliedLedCurrentMa)+config.fixedSystemCurrentMa<=config.maxTotalCurrentMa,
              "all independent emitters remain within unchanged power policy");
    }
}
}
int main() {
    std::printf("[PAIR_MEMORY] Session=%zu ComposeContext=%zu BoardLedFrame=%zu legacyBuffer=%zu\n", sizeof(runtime::virtual_litch_internal::Session), sizeof(modules::led_frame_composer::internal::ComposeContext), sizeof(modules::BoardLedFrame), sizeof(std::array<modules::Rgb,hardware::TOTAL_LED_COUNT>));
    static_assert(sizeof(modules::BoardSquareLeds)==12U);
    static_assert(sizeof(modules::BoardLedFrame)==768U);
    CheckAverage(); CheckMapping(); CheckShutdownOrientationInvariant();
    CheckShutdownAcceptedFrameInvariant();
    CheckRotatedProductionHealthOverlay();
    CheckOverlaysAndPower();
    std::printf("[BOARD_PAIR_FRAME] failures=%d frame_bytes=%zu delta_from_3slot=192\n",failures,sizeof(modules::BoardLedFrame));
    return failures ? 1 : 0;
}
