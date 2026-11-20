#include <cstdio>

#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/button_event_transform.hpp"
#include "modules/hint_button_policy.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/orientation.hpp"
#include "runtime/square_mapping.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[FLIP_SUITE] start");

    // Sanity: validation helper.
    Check(runtime::IsValidSquareIndex(0), "0 is valid square");
    Check(runtime::IsValidSquareIndex(63), "63 is valid square");
    Check(!runtime::IsValidSquareIndex(64), "64 is invalid square");

    // Non-flipped mapping is identity.
    for (uint8_t i = 0; i < 64; ++i) {
        const uint8_t mapped = runtime::MapSquareIndex(i, false);
        Check(mapped == i, "identity mapping when flipSides=false");
    }

    // Flipped mapping is stable involution: 63 - i.
    for (uint8_t i = 0; i < 64; ++i) {
        const uint8_t mapped = runtime::MapSquareIndex(i, true);
        Check(mapped == static_cast<uint8_t>(63U - i), "flip mapping is 63 - i");
        const uint8_t unflipped = runtime::MapSquareIndex(mapped, true);
        Check(unflipped == i, "flip mapping is an involution");
    }

    // Spot-check chessboard corners.
    Check(runtime::MapSquareIndex(0, true) == 63, "a8 (0) maps to h1 (63)");
    Check(runtime::MapSquareIndex(7, true) == 56, "h8 (7) maps to a1 (56)");
    Check(runtime::MapSquareIndex(56, true) == 7, "a1 (56) maps to h8 (7)");
    Check(runtime::MapSquareIndex(63, true) == 0, "h1 (63) maps to a8 (0)");

    // Full orientation matrix: sensor mapping, LED output, control ownership,
    // and active-player hint routing must agree for every setting combination.
    for (uint8_t boardRotateRaw = 0U; boardRotateRaw < 2U; ++boardRotateRaw) {
        for (uint8_t firstPlayerRaw = 0U; firstPlayerRaw < 2U; ++firstPlayerRaw) {
            const bool boardRotate = boardRotateRaw != 0U;
            const bool firstPlayerIsP2 = firstPlayerRaw != 0U;
            const bool squareRotate = modules::ComputeSquareRotate(
                boardRotate, firstPlayerIsP2);
            Check(squareRotate == (boardRotate != firstPlayerIsP2),
                  "gameplay maps P1 to its selected end of the physically oriented board");

            static constexpr uint8_t HINT_FROM = 52U;
            const uint8_t physicalHintFrom = modules::MapSquareToPhysical(
                HINT_FROM, squareRotate);
            Check(physicalHintFrom == ((boardRotate != firstPlayerIsP2) ? 11U : 52U),
                  "e2 follows logical P1 to the selected physical end");
            Check(modules::MapSquareToLogical(physicalHintFrom, squareRotate) == HINT_FROM,
                  "orientation matrix keeps sensor and hint square mapping inverse");

            const uint16_t logicalPieceLed = hardware::LedIndexForSquare(
                HINT_FROM, hardware::SquareLedSlot::Piece);
            const uint16_t physicalPieceLed = hardware::MapLedIndexForSquareAndEdgeRotate(
                logicalPieceLed, squareRotate, boardRotate);
            Check(physicalPieceLed == hardware::LedIndexForSquare(
                                            physicalHintFrom,
                                            hardware::SquareLedSlot::Piece),
                  "orientation matrix maps hinted piece LED to the sensed physical square");

            const modules::UiOwner activeOwner =
                modules::led_frame_composer::OwnerForSideToMove(
                    modules::SideToMove::White, firstPlayerIsP2);
            const modules::UiOwner activePhysicalSurface =
                modules::MapOwnerToPhysicalHomeEdge(activeOwner,
                    modules::ComputeControlBankSwap(boardRotate, firstPlayerIsP2, false));

            Check(activeOwner == modules::UiOwner::P1,
                  "orientation matrix keeps White attached to logical P1");

            modules::ButtonEvent physicalP1O{
                modules::UserButton::P1O,
                fxe::Button::Event::Press,
                modules::UiOwner::P1};
            modules::ButtonEvent physicalP2O{
                modules::UserButton::P2O,
                fxe::Button::Event::Press,
                modules::UiOwner::P2};
            const bool banksSwapped = modules::ComputeControlBankSwap(
                boardRotate, firstPlayerIsP2, false);
            Check(banksSwapped == (boardRotate != firstPlayerIsP2) &&
                      modules::ComputeControlBankSwap(boardRotate, firstPlayerIsP2, true) == boardRotate,
                  "game controls follow P1 end assignment while Litchcraft ignores game ownership");
            modules::ApplyRotateToButtonEvent(physicalP1O, banksSwapped);
            modules::ApplyRotateToButtonEvent(physicalP2O, banksSwapped);

            const modules::HintButtonRoutingDecision p1Hint =
                modules::ResolveHintButtonRouting(
                    true, true, physicalP1O.owner, activeOwner);
            const modules::HintButtonRoutingDecision p2Hint =
                modules::ResolveHintButtonRouting(
                    true, true, physicalP2O.owner, activeOwner);
            Check(p1Hint.shouldRequestHintOnRelease ==
                      (activePhysicalSurface == modules::UiOwner::P1),
                  "orientation matrix routes hint tap from the active physical surface");
            Check(p2Hint.shouldRequestHintOnRelease ==
                      (activePhysicalSurface == modules::UiOwner::P2),
                  "orientation matrix rejects the off-turn physical hint surface");
            Check(p1Hint.shouldRequestHintOnRelease != p2Hint.shouldRequestHintOnRelease,
                  "orientation matrix exposes exactly one active hint control");
        }
    }

    if (g_failures == 0) {
        std::puts("[FLIP_SUITE] ok");
        return 0;
    }

    std::printf("[FLIP_SUITE] failures=%d\n", g_failures);
    return 1;
}
