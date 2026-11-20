#include <array>
#include <cstdio>
#include <cstdlib>

#include "hardware/led_layout.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/settings_store.hpp"

namespace {

constexpr uint64_t INITIAL_CHESS_OCCUPANCY =
    0xFFFF00000000FFFFULL;
constexpr uint8_t WHITE_CORRECT = 56U;
constexpr uint8_t BLACK_CORRECT = 1U;
constexpr uint8_t WHITE_MISSING = 52U;
constexpr uint8_t EXTRA = 33U;
constexpr uint8_t EXTRA_TWO = 34U;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool RgbEqual(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

bool IsBlack(const modules::Rgb color) {
    return RgbEqual(color, modules::RGB_BLACK);
}

uint16_t Energy(const modules::Rgb color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b;
}

modules::Rgb LedAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint8_t square,
    const hardware::SquareLedSlot slot) {
    return frame[hardware::LedIndexForSquare(square, slot)];
}

bool AnyStatusLit(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    for (const modules::Rgb color : frame) {
        if (!IsBlack(color)) {
            return true;
        }
    }
    return false;
}

bool StatusFramesDiffer(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& left,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& right) {
    for (size_t index = 0U; index < left.size(); ++index) {
        if (!RgbEqual(left[index], right[index])) {
            return true;
        }
    }
    return false;
}

uint32_t StatusRangeEnergy(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame,
    const size_t start,
    const size_t count) {
    uint32_t total = 0U;
    for (size_t index = start; index < start + count; ++index) {
        total += Energy(frame[index]);
    }
    return total;
}

bool IsFullRedStatus(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    for (const modules::Rgb color : frame) {
        if (IsBlack(color) || color.r < color.g || color.r < color.b) {
            return false;
        }
    }
    return true;
}

size_t CountColor(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const modules::Rgb target) {
    size_t count = 0U;
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        count += RgbEqual(frame[index], target) ? 1U : 0U;
    }
    return count;
}

runtime::InteractionModelSnapshot MakeFreshSetupSnapshot() {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.board.expectedOccupiedMask = INITIAL_CHESS_OCCUPANCY;
    snapshot.board.physicalOccupiedMask =
        INITIAL_CHESS_OCCUPANCY & ~(1ULL << WHITE_MISSING);
    snapshot.board.moverOccupiedMask = 0xFFFF000000000000ULL;
    snapshot.board.opponentOccupiedMask = 0x000000000000FFFFULL;
    snapshot.board.enginePiecesValid = true;

    static constexpr char BLACK_BACK_RANK[8] = {
        'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'};
    static constexpr char WHITE_BACK_RANK[8] = {
        'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'};
    for (uint8_t file = 0U; file < 8U; ++file) {
        snapshot.board.enginePieces[file] = BLACK_BACK_RANK[file];
        snapshot.board.enginePieces[static_cast<uint8_t>(8U + file)] = 'p';
        snapshot.board.enginePieces[static_cast<uint8_t>(48U + file)] = 'P';
        snapshot.board.enginePieces[static_cast<uint8_t>(56U + file)] =
            WHITE_BACK_RANK[file];
    }

    snapshot.interaction.placementModeActive = true;
    snapshot.interaction.placementMissingMask = 1ULL << WHITE_MISSING;
    snapshot.interaction.placementExtraMask = 0ULL;
    snapshot.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::Correction;
    snapshot.interaction.taggedState.correctionMask =
        1ULL << WHITE_MISSING;
    snapshot.game.variant = modules::GameVariant::Undecided;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::FreshSetup;
    snapshot.game.sideToMove = modules::SideToMove::White;
    snapshot.system.nowMs = 500U;
    // These color/ownership fixtures exercise the sustained strong stage;
    // the production accidental-input journey owns the 300/1500ms boundaries.
    snapshot.interaction.illegalElapsedMs = 1500U;
    snapshot.interaction.lastInteractionElapsedMs = 1500U;
    return snapshot;
}

void Compose(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status,
    const bool firstPlayerIsP2 = false) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.firstPlayerIsP2 = firstPlayerIsP2;
    std::array<modules::Rgb, 64> grid{};
    modules::led_frame_composer::Compose(inputs, board, status, grid);
}

} // namespace

int main() {
    std::puts("[FRESH_SETUP_LED_UX] start");

    static constexpr const char* SETTINGS_PATH =
        "build/test_fresh_setup_led_ux.json";
    (void)std::remove(SETTINGS_PATH);
    modules::SettingsStoreConfig config;
    config.path = SETTINGS_PATH;
    modules::SettingsStore settings(config);
    Check(settings.Load(), "settings load");
    settings.Doc()["WHITE_SQUARES_ON"] = false;
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    settings.Doc()["P2_PIECE_WHEEL"] = 128U;

    runtime::InteractionModelSnapshot fresh = MakeFreshSetupSnapshot();
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> freshBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> freshStatus{};
    Compose(settings, fresh, freshBoard, freshStatus);

    runtime::InteractionModelSnapshot freshLater = fresh;
    freshLater.system.nowMs += 80U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> freshLaterBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> freshLaterStatus{};
    Compose(settings,
            freshLater,
            freshLaterBoard,
            freshLaterStatus);

    const modules::Rgb whitePiece = LedAt(
        freshBoard, WHITE_CORRECT, hardware::SquareLedSlot::Piece);
    const modules::Rgb whiteSquare = LedAt(
        freshBoard, WHITE_CORRECT, hardware::SquareLedSlot::Square0);
    Check(whitePiece.r > whitePiece.g && whitePiece.r > whitePiece.b,
          "correct P1 piece keeps the ordinary owning-player center");
    Check(IsBlack(whiteSquare) &&
              IsBlack(LedAt(freshBoard,
                            WHITE_CORRECT,
                            hardware::SquareLedSlot::Square2)),
          "correct setup pieces do not gain an owner-colored surround");

    const modules::Rgb blackPiece = LedAt(
        freshBoard, BLACK_CORRECT, hardware::SquareLedSlot::Piece);
    Check(blackPiece.g > blackPiece.r && blackPiece.b > blackPiece.r,
          "correct P2 piece uses the owning player color");

    const modules::Rgb missingPiece = LedAt(
        freshBoard, WHITE_MISSING, hardware::SquareLedSlot::Piece);
    const modules::Rgb missingSquare = LedAt(
        freshBoard, WHITE_MISSING, hardware::SquareLedSlot::Square0);
    Check(IsBlack(missingPiece) && !IsBlack(missingSquare) &&
              missingSquare.r > missingSquare.g && missingSquare.r > missingSquare.b &&
              RgbEqual(missingSquare, LedAt(freshBoard, WHITE_MISSING,
                                            hardware::SquareLedSlot::Square2)),
          "missing setup square guides placement in owner color while its center stays black");

    Check(AnyStatusLit(freshStatus) && AnyStatusLit(freshLaterStatus) &&
              StatusFramesDiffer(freshStatus, freshLaterStatus),
          "ordinary Fresh Setup keeps an animated Turn indicator");

    runtime::InteractionModelSnapshot handoff = fresh;
    handoff.board.physicalOccupiedMask = INITIAL_CHESS_OCCUPANCY;
    handoff.interaction.placementMissingMask = 0ULL;
    handoff.interaction.taggedState.correctionMask = 0ULL;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> handoffBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> handoffStatus{};
    Compose(settings, handoff, handoffBoard, handoffStatus);
    runtime::InteractionModelSnapshot handoffLater = handoff;
    handoffLater.system.nowMs += 80U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> handoffLaterBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> handoffLaterStatus{};
    Compose(settings,
            handoffLater,
            handoffLaterBoard,
            handoffLaterStatus);
    Check(AnyStatusLit(handoffStatus) &&
              AnyStatusLit(handoffLaterStatus) &&
              StatusFramesDiffer(handoffStatus, handoffLaterStatus),
          "exact-board Fresh Setup handoff keeps the animated Turn indicator");

    runtime::InteractionModelSnapshot ready = handoff;
    ready.interaction.placementModeActive = false;
    ready.interaction.taggedState = runtime::TaggedInteractionState{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> readyBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> readyStatus{};
    Compose(settings, ready, readyBoard, readyStatus);
    Check(AnyStatusLit(readyStatus),
          "exact opening occupancy shows the Turn indicator");
    Check(RgbEqual(
              LedAt(readyBoard, WHITE_CORRECT,
                    hardware::SquareLedSlot::Square0),
              whiteSquare) &&
              RgbEqual(
                  LedAt(readyBoard, WHITE_CORRECT,
                        hardware::SquareLedSlot::Piece),
                  whitePiece),
          "placing the last piece does not restyle already-correct pieces");

    runtime::InteractionModelSnapshot oneOrdinaryExtra = ready;
    oneOrdinaryExtra.board.physicalOccupiedMask =
        INITIAL_CHESS_OCCUPANCY | (1ULL << EXTRA);
    oneOrdinaryExtra.interaction.placementModeActive = true;
    oneOrdinaryExtra.interaction.placementExtraMask = 1ULL << EXTRA;
    oneOrdinaryExtra.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::Correction;
    oneOrdinaryExtra.interaction.taggedState.correctionMask =
        1ULL << EXTRA;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> oneExtraBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> oneExtraStatus{};
    Compose(settings, oneOrdinaryExtra, oneExtraBoard, oneExtraStatus);
    const modules::Rgb oneExtraSquare0 = LedAt(
        oneExtraBoard, EXTRA, hardware::SquareLedSlot::Square0);
    const modules::Rgb oneExtraPiece = LedAt(
        oneExtraBoard, EXTRA, hardware::SquareLedSlot::Piece);
    const modules::Rgb oneExtraSquare2 = LedAt(
        oneExtraBoard, EXTRA, hardware::SquareLedSlot::Square2);
    Check(!RgbEqual(oneExtraSquare0, oneExtraPiece) &&
              !RgbEqual(oneExtraPiece, oneExtraSquare2) &&
              !RgbEqual(oneExtraSquare0, oneExtraSquare2),
          "one ineligible ordinary setup extra uses the shared mismatch rainbow");
    Check(IsFullRedStatus(oneExtraStatus),
          "one ineligible ordinary setup extra uses the shared red correction ring");

    runtime::InteractionModelSnapshot multipleExtras = oneOrdinaryExtra;
    multipleExtras.board.physicalOccupiedMask =
        INITIAL_CHESS_OCCUPANCY | (1ULL << EXTRA) |
        (1ULL << EXTRA_TWO);
    multipleExtras.interaction.placementModeActive = true;
    multipleExtras.interaction.placementExtraMask =
        (1ULL << EXTRA) | (1ULL << EXTRA_TWO);
    multipleExtras.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::Correction;
    multipleExtras.interaction.taggedState.correctionMask =
        multipleExtras.interaction.placementExtraMask;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> multipleBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> multipleStatus{};
    Compose(settings, multipleExtras, multipleBoard, multipleStatus);
    Check(!RgbEqual(
              LedAt(multipleBoard, EXTRA,
                    hardware::SquareLedSlot::Square0),
              LedAt(multipleBoard, EXTRA,
                    hardware::SquareLedSlot::Square2)),
          "multiple extras use the ordinary mismatch rainbow");
    Check(IsFullRedStatus(multipleStatus),
          "multiple extras use the full red hard-correction ring");

    runtime::InteractionModelSnapshot stagedSetup = fresh;
    stagedSetup.board.physicalOccupiedMask =
        (INITIAL_CHESS_OCCUPANCY & ~(1ULL << WHITE_MISSING)) |
        (1ULL << EXTRA) | (1ULL << EXTRA_TWO);
    stagedSetup.interaction.placementMissingMask = 1ULL << WHITE_MISSING;
    stagedSetup.interaction.placementExtraMask =
        (1ULL << EXTRA) | (1ULL << EXTRA_TWO);
    stagedSetup.interaction.taggedState.correctionMask =
        stagedSetup.interaction.placementMissingMask |
        stagedSetup.interaction.placementExtraMask;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> stagedBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> stagedStatus{};
    Compose(settings, stagedSetup, stagedBoard, stagedStatus);
    Check(RgbEqual(LedAt(stagedBoard, WHITE_MISSING,
                         hardware::SquareLedSlot::Square0), missingSquare) &&
              RgbEqual(LedAt(stagedBoard, WHITE_MISSING,
                            hardware::SquareLedSlot::Square2), missingSquare) &&
              IsBlack(LedAt(stagedBoard,
                            WHITE_MISSING,
                            hardware::SquareLedSlot::Piece)),
          "Fresh Setup retains owner-color placement pairs and black empty centers while removing leftovers");
    Check(!RgbEqual(
              LedAt(stagedBoard, EXTRA,
                    hardware::SquareLedSlot::Square0),
              LedAt(stagedBoard, EXTRA,
                    hardware::SquareLedSlot::Square2)) &&
              !RgbEqual(
                  LedAt(stagedBoard, EXTRA_TWO,
                        hardware::SquareLedSlot::Square0),
                  LedAt(stagedBoard, EXTRA_TWO,
                        hardware::SquareLedSlot::Square2)),
          "Fresh Setup keeps every leftover piece actionable");
    Check(IsFullRedStatus(stagedStatus),
          "leftover-piece stage retains the established correction ring");

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> swappedOwnerBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> swappedOwnerStatus{};
    Compose(settings,
            fresh,
            swappedOwnerBoard,
            swappedOwnerStatus,
            true);
    const modules::Rgb swappedWhite = LedAt(
        swappedOwnerBoard, WHITE_CORRECT, hardware::SquareLedSlot::Piece);
    const modules::Rgb swappedBlack = LedAt(
        swappedOwnerBoard, BLACK_CORRECT, hardware::SquareLedSlot::Piece);
    Check(swappedWhite.g > swappedWhite.r &&
              swappedWhite.b > swappedWhite.r,
          "logical P1 selects the unchanged far-end palette");
    Check(swappedBlack.r > swappedBlack.g &&
              swappedBlack.r > swappedBlack.b,
          "logical P2 selects the unchanged near-end palette");
    Check(StatusRangeEnergy(swappedOwnerStatus, 16U, 8U) >
              StatusRangeEnergy(swappedOwnerStatus, 0U, 8U),
          "Fresh Setup Turn remains canonical before physical mapping");

    runtime::InteractionModelSnapshot gameplay = multipleExtras;
    gameplay.game.setupLifecycle = runtime::SetupLifecycle::Gameplay;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> gameplayBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> gameplayStatus{};
    Compose(settings, gameplay, gameplayBoard, gameplayStatus);
    Check(!RgbEqual(
              LedAt(gameplayBoard, EXTRA, hardware::SquareLedSlot::Square0),
              LedAt(gameplayBoard, EXTRA, hardware::SquareLedSlot::Square2)),
          "midgame correction retains the mismatch rainbow");
    Check(IsFullRedStatus(gameplayStatus),
          "midgame correction retains the full-ring fault cue");

    static constexpr uint8_t DUCK_CANDIDATE = 18U;
    runtime::InteractionModelSnapshot duck = MakeFreshSetupSnapshot();
    duck.board.physicalOccupiedMask =
        INITIAL_CHESS_OCCUPANCY | (1ULL << DUCK_CANDIDATE);
    duck.interaction.placementModeActive = false;
    duck.interaction.placementMissingMask = 0ULL;
    duck.interaction.placementExtraMask = 0ULL;
    duck.interaction.taggedState = runtime::TaggedInteractionState{};
    duck.game.initialVariantValidationActive = true;
    duck.game.initialDuckCandidateSquare = DUCK_CANDIDATE;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> duckBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> duckStatus{};
    Compose(settings, duck, duckBoard, duckStatus);
    Check(RgbEqual(
              LedAt(duckBoard, DUCK_CANDIDATE,
                    hardware::SquareLedSlot::Piece),
              modules::RGB_YELLOW) &&
              CountColor(duckBoard, modules::RGB_YELLOW) == 1U,
          "valid logical Duck candidate is the sole yellow cue");
    Check(AnyStatusLit(duckStatus),
          "Duck candidate validation keeps the Turn indicator");

    runtime::InteractionModelSnapshot latchedDuck =
        MakeFreshSetupSnapshot();
    latchedDuck.board.expectedOccupiedMask =
        INITIAL_CHESS_OCCUPANCY | (1ULL << DUCK_CANDIDATE);
    latchedDuck.board.physicalOccupiedMask =
        latchedDuck.board.expectedOccupiedMask;
    latchedDuck.interaction.placementModeActive = false;
    latchedDuck.interaction.placementMissingMask = 0ULL;
    latchedDuck.interaction.placementExtraMask = 0ULL;
    latchedDuck.interaction.taggedState =
        runtime::TaggedInteractionState{};
    latchedDuck.game.variant = modules::GameVariant::Duck;
    latchedDuck.game.duckSquare = DUCK_CANDIDATE;
    latchedDuck.game.initialVariantValidationActive = false;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> latchedBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> latchedStatus{};
    Compose(settings, latchedDuck, latchedBoard, latchedStatus);
    Check(RgbEqual(
              LedAt(latchedBoard, DUCK_CANDIDATE,
                    hardware::SquareLedSlot::Piece),
              modules::RGB_YELLOW) &&
              CountColor(latchedBoard, modules::RGB_YELLOW) == 1U,
          "latched untouched Duck remains the sole yellow token cue");
    Check(AnyStatusLit(latchedStatus),
          "latched untouched Duck shows the Turn indicator");

    if (g_failures == 0) {
        std::puts("[FRESH_SETUP_LED_UX] ok");
        return 0;
    }

    std::printf("[FRESH_SETUP_LED_UX] failures=%d\n", g_failures);
    return 1;
}
