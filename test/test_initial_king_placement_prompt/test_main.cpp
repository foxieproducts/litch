#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

#include "firmware/local_control_requests.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/square_mapping.hpp"

namespace {

constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
constexpr uint8_t BLACK_KING = 4U;
constexpr uint8_t BLACK_QUEEN = 3U;
constexpr uint8_t WHITE_QUEEN = 59U;
constexpr uint8_t WHITE_KING = 60U;
constexpr uint64_t KING_MASK =
    (1ULL << BLACK_KING) | (1ULL << WHITE_KING);
constexpr uint64_t KQ_MASK = KING_MASK |
                             (1ULL << BLACK_QUEEN) | (1ULL << WHITE_QUEEN);

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++gFailures;
}

uint16_t Energy(const modules::Rgb color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b;
}

bool IsBlack(const modules::Rgb color) {
    return color.r == 0U && color.g == 0U && color.b == 0U;
}

bool RgbEqual(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

modules::Rgb ExpectedSquareScale(const modules::Rgb ownerRgb,
                                 const uint16_t scale256) {
    return modules::ScaleRgbSaturatingBy256(
        modules::ScaleRgbSaturatingBy256(ownerRgb, scale256), 320U);
}

modules::Rgb ExpectedCenterScale(const modules::Rgb ownerRgb,
                                 const uint16_t scale256 = 96U) {
    return modules::ScaleRgbSaturatingBy256(
        modules::ScaleRgbSaturatingBy256(
            modules::ScaleRgbSaturatingBy256(ownerRgb, scale256), 304U),
        320U);
}

template <std::size_t N>
modules::Rgb LedAt(
    const std::array<modules::Rgb, N>& frame,
    const uint8_t physicalSquare,
    const hardware::SquareLedSlot slot) {
    return frame[hardware::LedIndexForSquare(physicalSquare, slot)];
}

bool AnyStatusLit(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    for (const modules::Rgb color : status) {
        if (!IsBlack(color)) {
            return true;
        }
    }
    return false;
}

bool IsP1TurnIndicatorWithoutFault(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    bool found = false;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        if (IsBlack(status[index])) {
            continue;
        }
        found = true;
        if (index < 16U || index >= 24U) {
            return false;
        }
    }
    return found;
}

runtime::InteractionModelSnapshot BuildFreshSnapshot(
    const runtime::GameStateMachine& machine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine& engine) {
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    (void)engine.GetFen(fen, sizeof(fen));
    return runtime::BuildInteractionModelSnapshot({
        .stateMachine = &machine,
        .derived = &derived,
        .authoritativeFen = fen,
        .setupLifecycle = runtime::SetupLifecycle::FreshSetup,
    });
}

void Compose(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    const bool firstPlayerIsP2,
    const bool flipSides,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.firstPlayerIsP2 = firstPlayerIsP2;
    std::array<modules::Rgb, 64> grid{};
    modules::led_frame_composer::Compose(
        inputs, board, status, grid);
}

bool InitialKingAssistVisibleInContext(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::internal::ComposeContext context;
    return modules::led_frame_composer::internal::BuildComposeContext(
               inputs, context) &&
           context.initialKingPlacementPromptActive;
}

void CheckCadenceForPresentAndMissingKings(
    const fxe::SettingsStore& settings,
    runtime::GameStateMachine& machine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine& engine) {
    struct CadenceBoundary {
        uint32_t elapsedMs;
        bool flashOn;
    };
    static constexpr std::array<CadenceBoundary, 15> BOUNDARIES{{
        {0U, true},
        {199U, true},
        {200U, false},
        {399U, false},
        {400U, true},
        {599U, true},
        {600U, false},
        {799U, false},
        {800U, true},
        {999U, true},
        {1000U, false},
        {1199U, false},
        {1200U, false},
        {1599U, false},
        {1600U, true},
    }};
    struct KingCase {
        uint8_t king;
        uint8_t neighbor;
        modules::Rgb ownerRgb;
    };
    const std::array<KingCase, 2> kings{{
        {WHITE_KING, WHITE_QUEEN,
         modules::ui_color_wheel::WheelToRgb(0U)},
        {BLACK_KING, BLACK_QUEEN,
         modules::ui_color_wheel::WheelToRgb(128U)},
    }};
    static constexpr uint8_t WHITE_PAWN_E2 = 52U;
    const uint64_t presentMask =
        INITIAL_OCCUPANCY & ~(1ULL << WHITE_PAWN_E2);
    const uint64_t missingMask = INITIAL_OCCUPANCY & ~KING_MASK;
    const uint32_t cadenceStartMs = fxe::Millis();

    machine.TriggerInitialKingPlacementPrompt();
    for (const CadenceBoundary boundary : BOUNDARIES) {
        fxe::ManualMillis::SetNowMs(cadenceStartMs + boundary.elapsedMs);
        for (const bool kingsPresent : std::array<bool, 2>{true, false}) {
            machine.DebugSetPhysicalOccupancyMask(
                kingsPresent ? presentMask : missingMask);
            const runtime::InteractionModelSnapshot snapshot =
                BuildFreshSnapshot(machine, derived, engine);
            Check(snapshot.render.initialKingPlacementPromptElapsedMs ==
                      boundary.elapsedMs,
                  "cadence snapshot uses prompt-relative elapsed time");

            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> board{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
            Compose(settings, snapshot, false, false, board, status);
            runtime::InteractionModelSnapshot ordinarySnapshot = snapshot;
            ordinarySnapshot.render.initialKingPlacementPromptActive = false;
            ordinarySnapshot.render.initialKingPlacementPromptElapsedMs = 0U;
            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ordinaryBoard{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> ordinaryStatus{};
            Compose(settings,
                    ordinarySnapshot,
                    false,
                    false,
                    ordinaryBoard,
                    ordinaryStatus);
            for (const KingCase king : kings) {
                const modules::Rgb expectedPair = ExpectedSquareScale(
                    king.ownerRgb, boundary.flashOn ? 512U : 72U);
                const modules::Rgb expectedCenter =
                    ExpectedCenterScale(king.ownerRgb, kingsPresent ? 96U : 0U);
                Check(RgbEqual(
                          LedAt(board, king.king,
                                hardware::SquareLedSlot::Square0),
                          expectedPair) &&
                          RgbEqual(
                              LedAt(board, king.king,
                                    hardware::SquareLedSlot::Square2),
                              expectedPair),
                      "both king side LEDs follow the exact flash/rest scale");
                Check(RgbEqual(
                          LedAt(board, king.king,
                                hardware::SquareLedSlot::Piece),
                          expectedCenter),
                      kingsPresent
                          ? "present king center stays steady across cadence"
                          : "missing king center stays black across cadence");
                Check(RgbEqual(
                          LedAt(board, king.neighbor,
                                hardware::SquareLedSlot::Square0),
                          LedAt(ordinaryBoard,
                                king.neighbor,
                                hardware::SquareLedSlot::Square0)) &&
                          RgbEqual(
                              LedAt(board, king.neighbor,
                                    hardware::SquareLedSlot::Square2),
                              LedAt(ordinaryBoard,
                                    king.neighbor,
                                    hardware::SquareLedSlot::Square2)) &&
                          RgbEqual(
                              LedAt(board, king.neighbor,
                                    hardware::SquareLedSlot::Piece),
                              LedAt(ordinaryBoard,
                                    king.neighbor,
                                    hardware::SquareLedSlot::Piece)),
                      "king assist does not restyle neighboring correct pieces");
            }
        }
    }
}

void CheckSidePriorityOwnership(
    modules::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& opening) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    runtime::InteractionModelSnapshot snapshot = opening;
    inputs.interactionSnapshot = &snapshot;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    std::array<modules::Rgb, 64> grid{};
    uint64_t mask = UINT64_MAX;
    const auto compose = [&]() {
        mask = UINT64_MAX; // Every compose must replace all prior ownership.
        modules::led_frame_composer::Compose(
            inputs, board, status, grid, nullptr, nullptr, &mask);
    };
    compose();
    Check(mask == KING_MASK, "opening exports only the two required king brackets");
    snapshot.board.physicalOccupiedMask &= ~(1ULL << 52U);
    compose();
    Check(mask == KING_MASK, "ordinary missing setup socket retains king guidance");
    snapshot = opening;
    snapshot.board.physicalOccupiedMask &= ~1ULL;
    snapshot.interaction.humanIllegalMask = 1ULL;
    compose();
    Check(mask == 0ULL, "wrong pickup releases shared slots to required correction");
    snapshot = opening;
    inputs.runtimeHealth.state = runtime::RuntimeHealthState::InputPaused;
    inputs.runtimeHealth.advice.boardInputAllowed = false;
    compose();
    Check(mask == 0ULL, "input-paused health owns the entire status perimeter");
    inputs.runtimeHealth.state = runtime::RuntimeHealthState::ProtectionLatched;
    compose();
    Check(mask == 0ULL, "paused advice under a latched tag owns the entire status perimeter");
    inputs.runtimeHealth.state = runtime::RuntimeHealthState::Healthy;
    inputs.runtimeHealth.advice.boardInputAllowed = true;
    inputs.higherPriorityStatusOverlayActive = true;
    compose();
    Check(mask == 0ULL, "higher-priority system overlay releases shared slots");
    inputs.higherPriorityStatusOverlayActive = false;
    snapshot.render.initialKingPlacementPromptActive = false;
    compose();
    Check(mask == 0ULL, "dismissed assist cannot retain ownership metadata");
    snapshot = opening;
    settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    compose();
    Check(mask == 0ULL, "disabled assist cannot retain ownership metadata");
    settings.Doc().remove("BOARD_SETUP_ASSISTANCE");

    // One real king square deliberately breaks pair symmetry. The literal
    // clockwise ring is independent of the production geometry helper.
    static constexpr std::array<uint8_t, 32> ring{{
        0,1,2,3,4,5,6,7, 7,15,23,31,39,47,55,63,
        63,62,61,60,59,58,57,56, 56,48,40,32,24,16,8,0,
    }};
    snapshot.render.initialKingPlacementPromptMask = 1ULL << BLACK_KING;
    for (const bool outputRotate : {false, true}) {
        for (const bool statusRotate : {false, true}) {
            compose();
            Check(mask == (1ULL << BLACK_KING),
                  "composer exports canonical king ownership in every orientation");

            std::array<modules::Rgb, modules::EDGE_LED_COUNT> noStatus{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> testStatus{};
            testStatus.fill(modules::Rgb{17U, 211U, 83U});
            const hardware::LedFrameMappingConfig mapping{
                hardware::BoardRevision::V1C3,
                outputRotate,
                statusRotate,
                mask,
            };
            const auto boardOnly =
                hardware::MapSemanticLedFrame(mapping, board, noStatus);
            const auto withStatus =
                hardware::MapSemanticLedFrame(mapping, board, testStatus);
            const uint8_t physicalSquare = outputRotate ? 59U : 4U;
            Check(Energy(withStatus[hardware::LedIndexForSquare(
                      physicalSquare, hardware::SquareLedSlot::Square0)]) > 0U,
                  "final mapper moves the canonical king bracket to physical square 4/59");

            for (uint8_t physicalIndex = 0U;
                 physicalIndex < modules::EDGE_LED_COUNT; ++physicalIndex) {
                const uint16_t target = modules::status_led_frame_adapter::
                    VirtualLedIndexForPhysicalStatusIndex(physicalIndex);
                const bool protectedByKing =
                    ring[physicalIndex] == physicalSquare;
                Check(RgbEqual(withStatus[target], boardOnly[target]) ==
                          protectedByKing,
                      "real V1 mapper protects exactly the physical king perimeter targets");
            }
        }
    }
    inputs.interactionSnapshot = nullptr;
    compose();
    Check(mask == 0ULL, "invalid compose clears prior side ownership");
}

} // namespace

int main() {
    std::puts("[INITIAL_KING_PLACEMENT_PROMPT] start");

    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    static constexpr const char* SETTINGS_PATH =
        "build/test_initial_king_placement_prompt.json";
    (void)std::remove(SETTINGS_PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = SETTINGS_PATH;
    settingsConfig.deferredSaveDelayMs = 999999U;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settings load");
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    settings.Doc()["P2_PIECE_WHEEL"] = 128U;
    settings.Doc()["BOARD_ROTATE"] = false;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    Check(!settings.Doc().containsKey("BOARD_SETUP_ASSISTANCE"),
          "absent setup-assistance setting exercises the enabled default");
    settings.SaveNow();

    runtime::GameStateMachineConfig machineConfig;
    machineConfig.placementStableRequiredMs = 10U;
    runtime::GameStateMachine machine(machineConfig);
    machine.Reset();
    machine.DebugSetPhysicalOccupancyMask(INITIAL_OCCUPANCY);

    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, machine);
    Check(coordinator.LoadOrInitialize(), "coordinator load");

    uint64_t promptMask = 0ULL;
    Check(machine.TryGetInitialKingPlacementPrompt(promptMask) &&
              promptMask == KING_MASK,
          "fresh boot arms exactly the two expected king squares");

    runtime::InteractionModelSnapshot present = BuildFreshSnapshot(
        machine, derived, engine);
    CheckSidePriorityOwnership(settings, present);
    Check(InitialKingAssistVisibleInContext(settings, present),
          "setup assistance defaults on when the setting is absent");
    Check(present.render.initialKingPlacementPromptElapsedMs == 0U,
          "fresh prompt snapshot starts at its own crest-aligned timebase");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> presentBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> presentStatus{};
    Compose(settings, present, false, false, presentBoard, presentStatus);
    Check(Energy(LedAt(presentBoard, WHITE_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(presentBoard, WHITE_KING,
                           hardware::SquareLedSlot::Piece)),
          "present white king uses a visible side-LED bracket around the piece");
    Check(Energy(LedAt(presentBoard, BLACK_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(presentBoard, BLACK_KING,
                           hardware::SquareLedSlot::Piece)),
          "present black king uses a visible side-LED bracket around the piece");
    Check(Energy(LedAt(presentBoard, WHITE_QUEEN,
                       hardware::SquareLedSlot::Square0)) <
              Energy(LedAt(presentBoard, WHITE_QUEEN,
                           hardware::SquareLedSlot::Piece)),
          "ordinary white setup pieces remain center-led instead of bracket-led");
    Check(Energy(LedAt(presentBoard, WHITE_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(presentBoard, WHITE_QUEEN,
                           hardware::SquareLedSlot::Square0)),
          "present white king square remains distinguishable from queen");
    Check(Energy(LedAt(presentBoard, BLACK_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(presentBoard, BLACK_QUEEN,
                           hardware::SquareLedSlot::Square0)),
          "present black king square remains distinguishable from queen");
    Check(AnyStatusLit(presentStatus),
          "king identity assist coexists with the active-side Ready cue");

    fxe::ManualMillis::AdvanceMs(1000U);
    runtime::InteractionModelSnapshot minimum = BuildFreshSnapshot(
        machine, derived, engine);
    Check(minimum.render.initialKingPlacementPromptElapsedMs == 1000U,
          "prompt elapsed carries through the render snapshot");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimumBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> minimumStatus{};
    Compose(settings, minimum, false, false, minimumBoard, minimumStatus);
    const modules::Rgb p1OwnerRgb =
        modules::ui_color_wheel::WheelToRgb(0U);
    const modules::Rgb p2OwnerRgb =
        modules::ui_color_wheel::WheelToRgb(128U);
    Check(RgbEqual(
              LedAt(minimumBoard, WHITE_KING,
                    hardware::SquareLedSlot::Square0),
              ExpectedSquareScale(p1OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(minimumBoard, WHITE_KING,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p1OwnerRgb, 72U)),
          "white king uses the halved 72/256 rest pair at 1000 ms");
    Check(RgbEqual(
              LedAt(minimumBoard, BLACK_KING,
                    hardware::SquareLedSlot::Square0),
              ExpectedSquareScale(p2OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(minimumBoard, BLACK_KING,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p2OwnerRgb, 72U)),
          "black king uses the halved 72/256 rest pair at 1000 ms");
    Check(RgbEqual(
              LedAt(minimumBoard, WHITE_KING,
                    hardware::SquareLedSlot::Piece),
              ExpectedCenterScale(p1OwnerRgb)) &&
              RgbEqual(
                  LedAt(minimumBoard, BLACK_KING,
                        hardware::SquareLedSlot::Piece),
                  ExpectedCenterScale(p2OwnerRgb)),
          "present king centers stay at the steady 96/256 scale");
    Check(AnyStatusLit(minimumStatus),
          "minimum-phase king assist keeps the active-side Ready cue");

    // Missing setup centers are black; actual detection lights the king center
    // at 96/256 without changing the separate location bracket cadence.
    machine.DebugSetPhysicalOccupancyMask(
        INITIAL_OCCUPANCY & ~KQ_MASK);
    runtime::InteractionModelSnapshot missing = BuildFreshSnapshot(
        machine, derived, engine);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> missingBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> missingStatus{};
    Compose(settings, missing, false, false, missingBoard, missingStatus);
    Check(RgbEqual(
              LedAt(missingBoard, WHITE_KING,
                    hardware::SquareLedSlot::Square0),
              ExpectedSquareScale(p1OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(missingBoard, WHITE_KING,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p1OwnerRgb, 72U)),
          "missing white king keeps the exact rest pair");
    Check(RgbEqual(
              LedAt(missingBoard, WHITE_KING,
                    hardware::SquareLedSlot::Piece),
              modules::RGB_BLACK) &&
              RgbEqual(
                  LedAt(missingBoard, BLACK_KING,
                        hardware::SquareLedSlot::Piece),
                  modules::RGB_BLACK) &&
              Energy(modules::RGB_BLACK) <
                  Energy(ExpectedCenterScale(p1OwnerRgb)) &&
              Energy(modules::RGB_BLACK) <
                  Energy(ExpectedCenterScale(p2OwnerRgb)),
          "missing king centers are black until detected");
    Check(Energy(LedAt(missingBoard, WHITE_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(missingBoard, WHITE_QUEEN,
                           hardware::SquareLedSlot::Square0)),
          "missing white king bracket differs from ordinary missing queen guidance");
    Check(Energy(LedAt(missingBoard, BLACK_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(missingBoard, BLACK_QUEEN,
                           hardware::SquareLedSlot::Square0)),
          "missing black king bracket differs from ordinary missing queen guidance");
    Check(IsP1TurnIndicatorWithoutFault(missingStatus),
          "missing king assist keeps the P1 Turn indicator without a status-ring fault");

    // Missing setup kings retain a black center and their bracket palette when
    // player placement or physical orientation changes.
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> p2FirstBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> p2FirstStatus{};
    Compose(settings, missing, true, false, p2FirstBoard, p2FirstStatus);
    const modules::Rgb p1White = LedAt(
        missingBoard, WHITE_KING, hardware::SquareLedSlot::Square0);
    const modules::Rgb p2White = LedAt(
        p2FirstBoard, WHITE_KING, hardware::SquareLedSlot::Square0);
    Check(p1White.r > p1White.g &&
              RgbEqual(p2White, ExpectedSquareScale(p2OwnerRgb, 72U)) &&
              RgbEqual(LedAt(missingBoard, WHITE_KING, hardware::SquareLedSlot::Piece),
                       modules::RGB_BLACK) &&
              RgbEqual(LedAt(p2FirstBoard, WHITE_KING, hardware::SquareLedSlot::Piece),
                       modules::RGB_BLACK),
          "missing white king keeps its selected physical-end palette and black center");
    Check(RgbEqual(
              LedAt(p2FirstBoard, WHITE_KING,
                    hardware::SquareLedSlot::Square0),
              ExpectedSquareScale(p2OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(p2FirstBoard, BLACK_KING,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p1OwnerRgb, 72U)),
          "both king pairs use their fixed physical-end palettes");

    // Rotation is applied once, at the physical output boundary.
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> rotatedSemanticBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> rotatedSemanticStatus{};
    Compose(settings, missing, false, false,
            rotatedSemanticBoard, rotatedSemanticStatus);
    const auto rotatedBoard = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, true, true},
        rotatedSemanticBoard, rotatedSemanticStatus);
    const uint8_t rotatedWhiteKing = runtime::MapSquareIndex(
        WHITE_KING, true);
    const modules::Rgb rotatedWhiteKingCue = LedAt(
        rotatedBoard, rotatedWhiteKing, hardware::SquareLedSlot::Square0);
    const uint8_t rotatedBlackQueen = runtime::MapSquareIndex(BLACK_QUEEN, true);
    const modules::Rgb rotatedBlackQueenCue = LedAt(
        rotatedBoard, rotatedBlackQueen, hardware::SquareLedSlot::Square0);
    Check(rotatedWhiteKingCue.r > rotatedWhiteKingCue.g &&
              rotatedWhiteKingCue.r > rotatedWhiteKingCue.b &&
              RgbEqual(LedAt(rotatedBoard, rotatedWhiteKing, hardware::SquareLedSlot::Piece),
                       modules::RGB_BLACK),
          "rotated missing white king retains its palette and black center");
    Check(rotatedBlackQueenCue.g > rotatedBlackQueenCue.r &&
              rotatedBlackQueenCue.b > rotatedBlackQueenCue.r &&
              RgbEqual(LedAt(rotatedBoard, rotatedBlackQueen, hardware::SquareLedSlot::Piece),
                       LedAt(missingBoard, BLACK_QUEEN, hardware::SquareLedSlot::Piece)),
          "rotation preserves the missing black queen's owner-color square and black center");
    const uint8_t rotatedBlackKing = runtime::MapSquareIndex(
        BLACK_KING, true);
    Check(RgbEqual(
              LedAt(rotatedBoard, rotatedWhiteKing,
                    hardware::SquareLedSlot::Square0),
              ExpectedSquareScale(p1OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(rotatedBoard, rotatedWhiteKing,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p1OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(rotatedBoard, rotatedBlackKing,
                        hardware::SquareLedSlot::Square0),
                  ExpectedSquareScale(p2OwnerRgb, 72U)) &&
              RgbEqual(
                  LedAt(rotatedBoard, rotatedBlackKing,
                        hardware::SquareLedSlot::Square2),
                  ExpectedSquareScale(p2OwnerRgb, 72U)) &&
              RgbEqual(LedAt(rotatedBoard, rotatedBlackKing, hardware::SquareLedSlot::Piece),
                       modules::RGB_BLACK),
          "rotation moves both king brackets with black empty centers to the opposite hardware ends");

    CheckCadenceForPresentAndMissingKings(
        settings, machine, derived, engine);
    machine.DebugSetPhysicalOccupancyMask(
        INITIAL_OCCUPANCY & ~KQ_MASK);
    const runtime::InteractionModelSnapshot gatedSnapshot =
        BuildFreshSnapshot(machine, derived, engine);

    // Exercise the production board-control ingress rather than the legacy
    // direct RequestUndo shortcut. X dismisses only the optional identity
    // assist; it is acknowledged as an applied cancellation, not rejected as
    // BUS/NO and not treated as invented undo history.
    modules::ControlMutationCallbacks controlCallbacks;
    controlCallbacks.context = &coordinator;
    controlCallbacks.requestUndo = [](void* context,
                                      const uint16_t requestId,
                                      const uint8_t moveCount) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->RequestControlUndo(requestId, moveCount);
    };
    controlCallbacks.pollAction = [](void* context,
                                     const uint16_t requestId) {
        return static_cast<modules::GamePersistenceCoordinator*>(context)
            ->PollControlAction(requestId);
    };
    modules::ControlMutationService controlMutation;
    controlMutation.Configure(settings, controlCallbacks);
    firmware_internal::LocalControlRequestTracker localRequests;
    settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    settings.MarkDirty();
    Check(!InitialKingAssistVisibleInContext(settings, gatedSnapshot),
          "setting off removes the armed assist from compose ownership");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> settingOffBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> settingOffStatus{};
    Compose(settings, gatedSnapshot, false, false,
            settingOffBoard, settingOffStatus);
    Check(RgbEqual(LedAt(settingOffBoard, WHITE_KING, hardware::SquareLedSlot::Square0),
                   ExpectedSquareScale(p1OwnerRgb, 64U)) &&
              RgbEqual(LedAt(settingOffBoard, WHITE_KING, hardware::SquareLedSlot::Square2),
                       ExpectedSquareScale(p1OwnerRgb, 64U)) &&
              IsBlack(LedAt(settingOffBoard, WHITE_KING, hardware::SquareLedSlot::Piece)),
          "disabled or dismissed assistance preserves ordinary king square guidance and a black center");
    bool trackingFailed = true;
    const modules::ControlMutationOutcome settingOffXOutcome =
        firmware_internal::RunBoardUndoTap(
            machine,
            InitialKingAssistVisibleInContext(settings, gatedSnapshot),
            controlMutation,
            localRequests,
            1U,
            trackingFailed);
    Check(settingOffXOutcome.status ==
                  modules::ControlMutationStatus::Rejected &&
              settingOffXOutcome.reason ==
                  modules::ControlMutationReason::NoHistory &&
              machine.TryGetInitialKingPlacementPrompt(promptMask),
          "setting-off X reaches semantic Undo and cannot consume the armed assist");

    settings.Doc()["BOARD_SETUP_ASSISTANCE"] = true;
    settings.MarkDirty();
    Check(InitialKingAssistVisibleInContext(settings, gatedSnapshot),
          "setting on reveals the still-armed assist");
    trackingFailed = true;
    const modules::ControlMutationOutcome dismissOutcome =
        firmware_internal::RunBoardUndoTap(
            machine,
            InitialKingAssistVisibleInContext(settings, gatedSnapshot),
            controlMutation,
            localRequests,
            1U,
            trackingFailed);
    Check(dismissOutcome.requestId != 0U &&
              dismissOutcome.status == modules::ControlMutationStatus::Applied &&
              dismissOutcome.reason == modules::ControlMutationReason::None,
          "board X applies king-assist dismissal through correlated control ingress");
    Check(!machine.TryGetInitialKingPlacementPrompt(promptMask),
          "X dismisses initial king identity assist");
    runtime::InteractionModelSnapshot canceled = BuildFreshSnapshot(
        machine, derived, engine);
    Check(canceled.render.initialKingPlacementPromptElapsedMs == 0U,
          "cancel clears the prompt timing state");
    Check(!InitialKingAssistVisibleInContext(settings, canceled),
          "canceled assist stays out of compose precedence");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> canceledBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> canceledStatus{};
    Compose(settings, canceled, false, false,
            canceledBoard, canceledStatus);
    Check(RgbEqual(LedAt(canceledBoard, WHITE_KING, hardware::SquareLedSlot::Square0),
                   ExpectedSquareScale(p1OwnerRgb, 64U)) &&
              RgbEqual(LedAt(canceledBoard, WHITE_KING, hardware::SquareLedSlot::Square2),
                       ExpectedSquareScale(p1OwnerRgb, 64U)) &&
              IsBlack(LedAt(canceledBoard, WHITE_KING, hardware::SquareLedSlot::Piece)),
          "disabled or dismissed assistance preserves ordinary king square guidance and a black center");
    Check(IsP1TurnIndicatorWithoutFault(canceledStatus),
          "cancel preserves the calm P1 Turn indicator without a status-ring fault");

    coordinator.ResetEngine();
    Check(machine.TryGetInitialKingPlacementPrompt(promptMask) &&
              promptMask == KING_MASK,
          "new-game reset rearms the king identity assist");
    runtime::InteractionModelSnapshot rearmed = BuildFreshSnapshot(
        machine, derived, engine);
    Check(rearmed.render.initialKingPlacementPromptElapsedMs == 0U,
          "rearmed prompt restarts from the informative crest");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> rearmedBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> rearmedStatus{};
    Compose(settings, rearmed, false, false, rearmedBoard, rearmedStatus);
    Check(Energy(LedAt(rearmedBoard, WHITE_KING,
                       hardware::SquareLedSlot::Square0)) >
              Energy(LedAt(minimumBoard, WHITE_KING,
                           hardware::SquareLedSlot::Square0)),
          "rearmed white king cue returns to a stronger crest than the minimum phase");

    // K/Q correction takes visual precedence but does not destroy the assist;
    // cancellation reveals it again.
    machine.DebugSetPhysicalOccupancyMask(INITIAL_OCCUPANCY);
    fxe::ManualMillis::AdvanceMs(machineConfig.placementStableRequiredMs);
    machine.Tick();
    Check(!machine.IsPlacementModeActive(),
          "corrected opening exits placement before K/Q prompt");
    Check(machine.StartKingQueenSwapPrompt(KQ_MASK),
          "K/Q correction prompt starts from fresh idle setup");
    runtime::InteractionModelSnapshot kq = BuildFreshSnapshot(
        machine, derived, engine);
    Check(!InitialKingAssistVisibleInContext(settings, kq),
          "K/Q prompt suppresses overlapping king assist visuals");
    Check(machine.TryGetInitialKingPlacementPrompt(promptMask),
          "K/Q prompt preserves the latent king assist");
    trackingFailed = true;
    const modules::ControlMutationOutcome kqUndoOutcome =
        firmware_internal::RunBoardUndoTap(
            machine,
            InitialKingAssistVisibleInContext(settings, kq),
            controlMutation,
            localRequests,
            1U,
            trackingFailed);
    Check(kqUndoOutcome.status == modules::ControlMutationStatus::Rejected &&
              kqUndoOutcome.reason == modules::ControlMutationReason::Busy &&
              machine.IsKingQueenSwapPromptActive() &&
              machine.TryGetInitialKingPlacementPrompt(promptMask),
          "K/Q-owned X stays semantic and preserves the latent assist");
    Check(machine.CancelKingQueenSwapPromptIfCancelable(),
          "untouched K/Q correction remains cancelable");
    runtime::InteractionModelSnapshot afterKqCancel = BuildFreshSnapshot(
        machine, derived, engine);
    Check(InitialKingAssistVisibleInContext(settings, afterKqCancel),
          "canceling K/Q correction reveals king assist again");

    Check(coordinator.LoadExternalFen(
              "8/8/8/8/8/8/8/4K2k w - - 0 1"),
          "external FEN load");
    Check(!machine.TryGetInitialKingPlacementPrompt(promptMask),
          "non-opening position clears the fresh-start assist");

    if (gFailures == 0) {
        std::puts("[INITIAL_KING_PLACEMENT_PROMPT] ok");
        return 0;
    }
    std::printf("[INITIAL_KING_PLACEMENT_PROMPT] failures=%d\n",
                gFailures);
    return 1;
}
