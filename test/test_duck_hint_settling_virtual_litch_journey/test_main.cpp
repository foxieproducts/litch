#include "bounded_hint_engine.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/orientation.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/square_mapping.hpp"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace {
constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
constexpr uint8_t DUCK = 24U;
int failures = 0;
void Check(bool ok, const char* run, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s: %s\n", run, message); }
}
std::string Home(runtime::VirtualLitchSession& board, const char* key) {
    StaticJsonDocument<2048> json;
    const auto line = board.HomeStateLine();
    if (deserializeJson(json, line.CStr())) return {};
    return json[key] | "";
}
bool Yellow(const virtual_litch_color color) {
    return color.r != 0U && color.r == color.g && color.b == 0U && color.w == 0U;
}
bool Equal(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
enum class DuckPickup { AfterChessPlacement, AfterChessPickup };
enum class DuckPlacementBatch { Separate, Unique, Ambiguous };
void Journey(const char* policy, uint32_t duration, bool firstP2, bool rotate, bool coaching,
             bool correction = false, bool minimal = false,
             DuckPickup duckPickup = DuckPickup::AfterChessPlacement,
             DuckPlacementBatch batch = DuckPlacementBatch::Separate, unsigned captureOrder = 0U) {
    char run[128]{};
    std::snprintf(run, sizeof(run), "DuckHintSettling/%s/first-p%u/rotate-%u/%s/%s/pickup-%u/batch-%u/capture-%u",
                  policy, firstP2 ? 2U : 1U, rotate,
                  correction ? "correction" : (coaching ? "Coaching" : "Hint"),
                  minimal ? "Minimal" : "Normal", unsigned(duckPickup), unsigned(batch), captureOrder);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = captureOrder ? VIRTUAL_LITCH_HARDWARE_V3_3 : VIRTUAL_LITCH_HARDWARE_S3;
    const auto revision = captureOrder ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V2S3;
    config.first_player_is_p2 = firstP2;
    config.board_rotate = rotate;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    const bool mappedRotate = modules::ComputeSquareRotate(rotate, firstP2);
    const bool banksSwapped = modules::ComputeControlBankSwap(rotate, firstP2, false);
    const uint8_t white = banksSwapped ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    const uint8_t black = banksSwapped ? VIRTUAL_LITCH_OWNER_P1 : VIRTUAL_LITCH_OWNER_P2;
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK);
    auto observe = [&]() {
        uint64_t physical = 0;
        for (uint8_t s = 0; s < 64; ++s) {
            if (occupancy & (UINT64_C(1) << s))
                physical |= UINT64_C(1) << runtime::MapSquareIndex(s, mappedRotate);
        }
        return board.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
    };
    auto button = [&](uint8_t owner, runtime::VirtualLitchPhysicalButton key, uint32_t hold = 20U) {
        bool ok = board.SetButtonDown(owner, key, true) == VIRTUAL_LITCH_OK &&
            board.Advance(20U) == VIRTUAL_LITCH_OK;
        for (uint32_t elapsed = 20U; elapsed < hold; elapsed += 20U)
            ok = ok && board.Advance(20U) == VIRTUAL_LITCH_OK;
        return ok && board.SetButtonDown(owner, key, false) == VIRTUAL_LITCH_OK &&
            board.Advance(20U) == VIRTUAL_LITCH_OK;
    };
    auto checkpoint = [&](const char* stage, const char* next) {
        const auto& state = board.State();
        const auto line = board.HomeStateLine();
        std::printf("[DUCK_HINT_SETTLING] run=%s stage=%s t=%u occupied=%016llx "
            "expected=%016llx phase=%u action=%u token=%u history=%u hint=%u home=%s next=%s\n",
            run, stage, state.now_ms, static_cast<unsigned long long>(state.physical_occupied_mask),
            static_cast<unsigned long long>(state.expected_occupied_mask), state.interaction_phase,
            state.required_action, state.pending_action_token, board.UndoHistoryMoveCount(),
            board.HintDisplayRequested(), line.CStr(), next);
    };
    Check(board.Reset() == VIRTUAL_LITCH_OK && observe() && board.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "physical opening selects Duck");
    modules::ControlMutationOutcome outcome;
    const std::string value = std::string("\"") + policy + "\"";
    Check(board.MutateSettingJson(1U, "MOVE_CONFIRM", value.c_str(), outcome) == VIRTUAL_LITCH_OK &&
          (outcome.status == modules::ControlMutationStatus::Applied ||
           outcome.status == modules::ControlMutationStatus::Noop) && board.Advance(0U) == VIRTUAL_LITCH_OK,
          run, "shared confirmation setting applied");
    virtual_litch_color captureOwner{};
    if (captureOrder != 0U) {
        bool restored = false;
        // Use the existing saved-game seam only to choose a capture position.
        // Search, buttons, Hall observations, settling and both LED mappings below are production.
        board.RunControlIngress([](void* context, modules::SettingsStore& settings,
            modules::ControlMutationService&, modules::GamePersistenceCoordinator& coordinator) {
            modules::GamePersistenceState state;
            const bool position = state.fen.Assign("4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1");
            state.undoBaseFen = state.fen;
            modules::DuckGameDurableState duck;
            duck.variant = modules::GameVariant::Duck;
            duck.duckSquare = DUCK;
            char encoded[32]{};
            *static_cast<bool*>(context) = position && modules::EncodeDuckGameDurableState(duck, encoded, sizeof(encoded)) &&
                state.variantState.Assign(encoded) && modules::WriteGamePersistenceState(settings.Doc(), state) &&
                coordinator.LoadOrInitialize();
        }, &restored);
        occupancy = (UINT64_C(1) << 4U) | (UINT64_C(1) << 36U) | (UINT64_C(1) << 52U) |
                    (UINT64_C(1) << 62U) | (UINT64_C(1) << DUCK);
        Check(restored && observe() && board.Advance(2500U) == VIRTUAL_LITCH_OK &&
              board.GameVariant() == modules::GameVariant::Duck, run, "saved Duck capture position restored");
        captureOwner = board.SemanticFrame().board[52U][1U];
        Check(!Equal(captureOwner, {}), run, "saved capturer has its player color before Hint");
    }
    Check(button(white, runtime::VirtualLitchPhysicalButton::O, coaching ? 1200U : 20U),
          run, "physical O requests guidance");
    uint8_t from = 64, to = 64, duckFrom = 64, duckTo = 64;
    for (uint8_t i = 0; i < 40 && !board.TryGetHintMoveSquares(from, to); ++i)
        Check(board.Advance(100U) == VIRTUAL_LITCH_OK, run, "wait for bounded production search");
    const bool ready = board.TryGetHintMoveSquares(from, to) &&
        board.TryGetHintDuckMoveSquares(duckFrom, duckTo) && duckFrom == DUCK && duckTo < 64;
    if (coaching) {
        const auto setupCue = board.EdgeCueIntent();
        Check(ready && setupCue.kind == modules::EdgeLedCueKind::SetupComplete &&
              !board.HintDisplayRequested(), run,
              "compound cache is ready while SetupComplete owns automatic presentation");
        checkpoint("compound-cached-during-setup", "finish-Ready-before-automatic-Hint");
        const uint32_t lastSetupAge = modules::EdgeLedCueController::SETUP_COMPLETE_DURATION_MS - 1U;
        for (uint8_t i = 0U; i < 20U && board.EdgeCueIntent().kind == modules::EdgeLedCueKind::SetupComplete &&
             board.State().now_ms - setupCue.startMs < lastSetupAge; ++i) {
            const uint32_t remaining = lastSetupAge - (board.State().now_ms - setupCue.startMs);
            Check(board.Advance(remaining < 100U ? remaining : 100U) == VIRTUAL_LITCH_OK &&
                  board.EdgeCueIntent().kind == modules::EdgeLedCueKind::SetupComplete &&
                  !board.HintDisplayRequested(), run,
                  "automatic compound Hint stays hidden through the original Ready fade");
        }
        Check(board.Advance(1U) == VIRTUAL_LITCH_OK &&
              board.EdgeCueIntent().kind == modules::EdgeLedCueKind::None &&
              board.HintDisplayRequested(), run,
              "Ready expiry reveals automatic compound guidance without restarting acknowledgement");
        Check(board.TryGetHintMoveSquares(from, to) && from < 64U && to < 64U &&
              board.TryGetHintDuckMoveSquares(duckFrom, duckTo) && duckFrom == DUCK && duckTo < 64U,
              run, "expiry reveals the current complete cached compound decision");
    }
    Check(ready && board.HintDisplayRequested(), run, "one legal compound Hint is revealed");
    if (!ready) return;
    StaticJsonDocument<2048> readyHome;
    const auto readyLine = board.HomeStateLine();
    Check(!deserializeJson(readyHome, readyLine.CStr()) &&
          readyHome["co"].as<unsigned>() == (coaching ? 1U : 0U),
          run, "White Coaching latch is enabled only in automatic cases");
    checkpoint("compound-ready", "follow-chess-then-same-Duck-destination");
    const auto duckTargetPiece = hardware::MapSemanticSquareLedTargets(
        revision, duckTo, hardware::SquareLedSlot::Piece, mappedRotate);
    uint8_t hiddenDiagnostics = 0U;
    const auto duckGuidanceHidden = [&]() {
        const auto report = [&](const char* reason, uint8_t square, uint8_t slot,
                                virtual_litch_color color) {
            if (hiddenDiagnostics++ < 5U) std::printf(
                "[DUCK_HIDE_DETAIL] run=%s time=%u home=%s reason=%s square=%u slot=%u from=%u to=%u duckFrom=%u duckTo=%u color=%u,%u,%u,%u\n",
                run, board.State().now_ms, Home(board, "a").c_str(), reason, square, slot,
                from, to, DUCK, duckTo, color.r, color.g, color.b, color.w);
            return false;
        };
        const auto landing = board.SemanticFrame().board[duckTo][1U];
        if (Yellow(landing)) return report("landing-semantic", duckTo, 1U, landing);
        for (uint8_t target = 0U; target < duckTargetPiece.count; ++target) {
            const auto color = board.PhysicalFrame().pixels[duckTargetPiece.indices[target]];
            if (Yellow(color)) return report("landing-physical", duckTo, target, color);
        }
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (square == from || square == to) continue;
            for (uint8_t slot = 0U; slot < 3U; ++slot) {
                // A present Duck keeps its ordinary yellow Piece identity;
                // this is not a destination or moving route invitation.
                if (square == DUCK && slot == 1U &&
                    (occupancy & (UINT64_C(1) << DUCK)) != 0ULL) continue;
                const auto color = board.SemanticFrame().board[square][slot];
                if (Yellow(color)) return report("yellow-scan", square, slot, color);
            }
        }
        return true;
    };
    Check(duckGuidanceHidden(), run,
          "chess Hint does not reveal the later Duck landing or path in semantic or mapped LEDs");
    const auto duckSourcePiece = hardware::MapSemanticSquareLedTargets(
        revision, DUCK, hardware::SquareLedSlot::Piece, mappedRotate);
    bool ordinaryDuckIdentity = Yellow(board.SemanticFrame().board[DUCK][1U]);
    for (uint8_t target = 0U; target < duckSourcePiece.count; ++target) {
        ordinaryDuckIdentity = ordinaryDuckIdentity &&
            Yellow(board.PhysicalFrame().pixels[duckSourcePiece.indices[target]]);
    }
    Check(ordinaryDuckIdentity, run,
          "the occupied Duck retains its ordinary yellow identity without an early route or landing");
    if (!coaching) {
        Check(button(white, runtime::VirtualLitchPhysicalButton::O) && !board.HintDisplayRequested(),
              run, "second O hides manual guidance with Coaching disabled");
        Check(button(white, runtime::VirtualLitchPhysicalButton::O) && board.HintDisplayRequested(),
              run, "third O reveals retained compound guidance with Coaching disabled");
        uint8_t cachedFrom = 64, cachedTo = 64, cachedDuckFrom = 64, cachedDuckTo = 64;
        Check(board.TryGetHintMoveSquares(cachedFrom, cachedTo) &&
              board.TryGetHintDuckMoveSquares(cachedDuckFrom, cachedDuckTo) &&
              cachedFrom == from && cachedTo == to && cachedDuckFrom == duckFrom && cachedDuckTo == duckTo,
              run, "manual hide and reveal reuse the same complete compound decision");
    } else {
        uint8_t replayFrom = 64, replayTo = 64, replayDuckFrom = 64, replayDuckTo = 64;
        Check(board.TryGetHintMoveSquares(from, to) &&
              board.TryGetHintDuckMoveSquares(duckFrom, duckTo) &&
              button(white, runtime::VirtualLitchPhysicalButton::O) && board.HintDisplayRequested() &&
              board.TryGetHintMoveSquares(replayFrom, replayTo) &&
              board.TryGetHintDuckMoveSquares(replayDuckFrom, replayDuckTo) &&
              replayFrom == from && replayTo == to && replayDuckFrom == duckFrom && replayDuckTo == duckTo,
              run, "Coaching O replays the exact currently shown chess and Duck tuple");
    }
    if (minimal) {
        Check(board.MutateSettingJson(2U, "MINIMAL_ANIMATIONS", "true", outcome) == VIRTUAL_LITCH_OK &&
              (outcome.status == modules::ControlMutationStatus::Applied ||
               outcome.status == modules::ControlMutationStatus::Noop) && board.Advance(0U) == VIRTUAL_LITCH_OK,
              run, "Minimal applies to the already-revealed Coaching decision");
        bool hidden = true;
        for (uint32_t age = 0U; age < 1200U; age += 20U) {
            Check(board.Advance(20U) == VIRTUAL_LITCH_OK, run,
                  "observe chess-only guidance throughout the prior Duck pulse period");
            hidden = hidden && duckGuidanceHidden();
        }
        Check(hidden, run,
              "Minimal keeps the later Duck landing and path hidden during chess guidance");
        checkpoint("Minimal-compound-steady", "lift-hinted-chess-piece");
    }
    const bool checkPickupOrder = duckPickup == DuckPickup::AfterChessPickup && minimal;
    if (checkPickupOrder) {
        const uint64_t start = occupancy;
        occupancy &= ~((UINT64_C(1) << from) | (UINT64_C(1) << DUCK));
        Check(observe() && board.DuckSquare() == DUCK &&
              board.State().correction_mask != 0ULL && board.UndoHistoryMoveCount() == 0U,
              run, "an opening pickup with unknown order keeps Duck and requests correction");
        checkpoint("opening-unordered-pickup", "return-both-pieces");
        occupancy = start;
        Check(observe() && board.State().correction_mask == 0ULL,
              run, "restoring both pieces permits a fresh chess-first pickup");
        const auto mover = UINT64_C(1) << from;
        const auto duck = UINT64_C(1) << DUCK;
        const auto alternative = UINT64_C(1) << (from == 52U ? 51U : 52U);
        occupancy &= ~mover;
        Check(observe(), run, "pick up a chess piece to begin");
        occupancy &= ~duck;
        Check(observe(), run, "pick up the Duck after that piece");
        occupancy = (occupancy | mover) & ~alternative;
        Check(observe() && Home(board, "a") == "corr",
              run, "returning one mover and lifting another cannot transfer Duck pickup ownership");
        checkpoint("held-Duck-mover-changed", "restore-Duck-and-new-mover");
        occupancy = start;
        Check(observe() && board.State().correction_mask == 0ULL,
              run, "restoring the starting occupancy recovers normal pickup");
    }
    if (captureOrder != 0U) {
        Check(from == 52U && to == 36U && (occupancy & (UINT64_C(1) << to)) != 0U,
              run, "real bounded Duck Hint selects the available rook capture");
        if (from != 52U || to != 36U) return;
        if (captureOrder == 2U) {
            occupancy &= ~(UINT64_C(1) << to);
            Check(observe(), run, "capture victim lifted before mover");
        }
    }
    occupancy &= ~(UINT64_C(1) << from);
    Check(observe(), run, "lift hinted chess piece");
    if (duckPickup == DuckPickup::AfterChessPickup) {
        occupancy &= ~(UINT64_C(1) << DUCK);
        Check(observe(), run, "lift Duck while the chess mover remains held");
    }
    char heldAssistance[24]{};
    std::snprintf(heldAssistance, sizeof(heldAssistance), "%ul%c%c%016llX%c%c",
                  coaching ? 5U : 1U, 'a' + from % 8U, '8' - from / 8U,
                  static_cast<unsigned long long>(board.State().valid_destination_mask),
                  'a' + to % 8U, '8' - to / 8U);
    Check(captureOrder == 2U
              ? board.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary)
              : Home(board, "as") == heldAssistance, run,
          "held Duck chess piece exposes legal targets or its acknowledged victim-first placement plan");
    checkpoint("chess-mover-held", "place-chess-piece-first");
    bool compoundVisible = true;
    for (uint8_t sample = 0U; sample < 4U; ++sample) {
        uint8_t heldDuckFrom = 64U, heldDuckTo = 64U;
        compoundVisible = compoundVisible && board.HintDisplayRequested() &&
            board.TryGetHintDuckMoveSquares(heldDuckFrom, heldDuckTo) &&
            heldDuckFrom == DUCK && heldDuckTo == duckTo &&
            duckGuidanceHidden() && board.State().correction_mask == 0ULL;
        Check(board.Advance(100U) == VIRTUAL_LITCH_OK, run,
              "observe stable compound guidance through held-piece rebuilds");
    }
    Check(compoundVisible, run,
          "the same Duck tuple stays cached without a Duck landing or path while chess is held");
    if (batch != DuckPlacementBatch::Separate) {
        const uint64_t legal = board.State().valid_destination_mask;
        const bool ambiguous = batch == DuckPlacementBatch::Ambiguous;
        uint8_t second = from;
        if (ambiguous) {
            second = 64U;
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint64_t bit = UINT64_C(1) << square;
                if (square != to && (occupancy & bit) == 0ULL && (legal & bit) != 0ULL) {
                    second = square;
                    break;
                }
            }
        }
        Check(second < 64U && (legal & (UINT64_C(1) << to)) != 0ULL, run,
              "the physical fixture supplies the named legal assignment case");
        if (second >= 64U) return;
        const auto firstLanding = UINT64_C(1) << to;
        const auto secondLanding = UINT64_C(1) << second;
        occupancy |= firstLanding | secondLanding;
        Check(observe(), run, "both physical landings arrive in one atomic Hall observation");
        if (!ambiguous) {
            Check(board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  board.State().correction_mask == 0ULL && board.UndoHistoryMoveCount() == 0U &&
                  Home(board, "a") == "settle", run,
                  "unique assignment reaches the existing final Duck confirmation");
            Check(Yellow(board.SemanticFrame().board[second][1U]), run,
                  "an accepted Duck landing stays yellow even on the vacated mover square");
            checkpoint("unique-both-placed",
                       duration == 0U ? "confirm-chess-then-Duck-with-Circle" : "wait-for-Duck-confirmation");
            if (duration == 0U) {
                Check(board.Advance(6000U) == VIRTUAL_LITCH_OK &&
                      board.UndoHistoryMoveCount() == 0U &&
                      board.State().correction_mask == 0ULL, run,
                      "coalesced landing waits for chess Circle without false correction");
                Check(button(white, runtime::VirtualLitchPhysicalButton::O) &&
                      board.Advance(1000U) == VIRTUAL_LITCH_OK &&
                      board.UndoHistoryMoveCount() == 0U &&
                      Home(board, "a") == "settle", run,
                      "chess Circle advances to separate Duck confirmation");
                Check(button(white, runtime::VirtualLitchPhysicalButton::O) &&
                      board.Advance(1000U) == VIRTUAL_LITCH_OK, run,
                      "Duck Circle completes the compound turn");
            } else {
                Check(board.Advance(duration - 1U) == VIRTUAL_LITCH_OK && board.UndoHistoryMoveCount() == 0U &&
                      board.Advance(1U) == VIRTUAL_LITCH_OK, run, "wait for the final Duck deadline");
            }
            Check(board.UndoHistoryMoveCount() == 1U && board.DuckSquare() == second, run,
                  "the unambiguous chess and Duck roles commit exactly one compound turn");
            checkpoint("unique-both-accepted", "Black-to-move");
            return;
        }
        const auto amber = [&](const uint8_t square) {
            const auto c = board.SemanticFrame().board[square][0U];
            return c.r > c.g && c.g > 0U && c.b == 0U;
        };
        Check(board.Advance(1600U) == VIRTUAL_LITCH_OK && amber(to) && amber(second) &&
              board.HomeStateSnapshot().duckLandingRemovalMask == (firstLanding | secondLanding) &&
              board.UndoHistoryMoveCount() == 0U, run,
              "ambiguous landings remain local amber without guessing either role");
        checkpoint("ambiguous-both-placed", "lift-both-amber-landings-then-place-chess-first");
        occupancy &= ~secondLanding;
        Check(observe() && amber(to) && board.UndoHistoryMoveCount() == 0U &&
              board.HomeStateSnapshot().duckLandingRemovalMask == firstLanding, run,
              "one remaining landing is not misinterpreted as the chess piece");
        occupancy &= ~firstLanding;
        Check(observe() && board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              board.HintDisplayRequested() && duckGuidanceHidden(), run,
              "lifting both placements restores the original chess guidance while Duck guidance remains hidden");
        checkpoint("ambiguous-recovered", "place-chess-piece-first");
        const uint64_t moverSource = UINT64_C(1) << from;
        const uint64_t openInvalidSquares =
            ~(occupancy | legal | moverSource | (UINT64_C(1) << DUCK));
        const uint64_t invalidLanding = openInvalidSquares & (0ULL - openInvalidSquares);
        uint8_t invalidSquare = 0U;
        while (invalidSquare < 63U && (invalidLanding & (1ULL << invalidSquare)) == 0ULL) {
            ++invalidSquare;
        }
        occupancy |= moverSource | invalidLanding;
        Check(invalidLanding != 0ULL && observe() &&
              board.HomeStateSnapshot().duckLandingRemovalMask == (moverSource | invalidLanding) &&
              amber(from) && amber(invalidSquare),
              run, "zero legal assignments mark both landings, even the original chess source");
        checkpoint("no-legal-assignment", "lift-both-marked-landings");
        occupancy &= ~(moverSource | invalidLanding);
        Check(observe() && board.HomeStateSnapshot().duckLandingRemovalMask == 0ULL &&
              Home(board, "a") == "lift", run, "removing both landings reopens chess placement");
    }
    if (captureOrder == 1U) {
        occupancy &= ~(UINT64_C(1) << to);
        Check(observe(), run, "capture victim lifted after mover");
    }
    occupancy |= UINT64_C(1) << to;
    Check(observe(), run, "place hinted chess piece");
    Check(Home(board, "a") == "duck" ? Yellow(board.SemanticFrame().board[duckTo][1U])
                                      : duckGuidanceHidden(), run,
          "seated chess piece hides Duck guidance until its stage is committed");
    if (duration == 2000U && !firstP2 && !rotate && !coaching && !correction &&
        !minimal && captureOrder == 0U && duckPickup == DuckPickup::AfterChessPlacement &&
        batch == DuckPlacementBatch::Separate) {
        Check(board.Advance(100U) == VIRTUAL_LITCH_OK && duckGuidanceHidden(), run,
              "Duck guidance stays hidden while the chess placement is settling");
        occupancy &= ~(UINT64_C(1) << to);
        Check(observe() && duckGuidanceHidden(), run,
              "relifting the settling chess piece restores chess-only guidance");
        uint8_t cachedDuckFrom = 64U, cachedDuckTo = 64U;
        Check(board.TryGetHintDuckMoveSquares(cachedDuckFrom, cachedDuckTo) &&
                  cachedDuckFrom == DUCK && cachedDuckTo == duckTo, run,
              "chess relift retains the compound Duck tuple");
        occupancy |= UINT64_C(1) << to;
        Check(observe() && duckGuidanceHidden(), run,
              "reseating the chess piece starts a fresh chess-only settling interval");
    }
    if (captureOrder != 0U) {
        for (unsigned sample = 0U; sample <= 20U; ++sample) {
            const auto& slots = board.SemanticFrame().board[to];
            if (Home(board, "a") != "duck") Check(duckGuidanceHidden(), run,
                "capture settling keeps later Duck landing and path hidden");
            Check(Equal(slots[1], captureOwner), run,
                  "seated Duck capturer owns Piece LEDs throughout settling, independent of removal palette");
            const auto pieceTargets = hardware::MapSemanticSquareLedTargets(revision,
                to, hardware::SquareLedSlot::Piece, mappedRotate);
            for (uint8_t i = 0U; i < pieceTargets.count; ++i)
                Check(!Equal(board.PhysicalFrame().pixels[pieceTargets.indices[i]], {}), run,
                      "both physical capturer Piece emitters remain lit while settling");
            if (sample == 0U || sample == 10U || sample == 20U) {
                const auto targets = hardware::MapSemanticSquareLedTargets(revision,
                    to, hardware::SquareLedSlot::Square0, mappedRotate);
                const auto pixel = board.PhysicalFrame().pixels[targets.indices[0]];
                std::printf("[DUCK_CAPTURE_SETTLE] order=%u age=%u phase=%u action=%u square=%u,%u,%u piece=%u,%u,%u physical=%u,%u,%u,%u\n",
                    captureOrder, sample * 100U, board.State().interaction_phase, board.State().required_action,
                    slots[0].r, slots[0].g, slots[0].b, slots[1].r, slots[1].g, slots[1].b,
                    pixel.r, pixel.g, pixel.b, pixel.w);
            }
            if (sample != 20U) board.Advance(100U);
        }
    }

    if (duckPickup == DuckPickup::AfterChessPlacement || duration == 0U) {
        Check(board.HintDisplayRequested() &&
              (Home(board, "a") == "duck" ? Yellow(board.SemanticFrame().board[duckTo][1U])
                                         : duckGuidanceHidden()) &&
              board.State().side_to_move_black == 0U, run,
              "Duck guidance waits for chess confirmation and appears after the handoff");
    } else {
        Check(Home(board, "a") == "duck" &&
              board.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary), run,
              "already-held Duck advances directly from chess placement to Duck placement");
    }
    if (duration == 0U) {
        Check(button(white, runtime::VirtualLitchPhysicalButton::O),
              run, "confirm chess half using shared Circle owner");
        if (duckPickup == DuckPickup::AfterChessPickup) {
            Check(board.Advance(950U) == VIRTUAL_LITCH_OK && Home(board, "a") == "settle",
                  run, "sample the hidden Duck landing just before chess confirmation");
            bool hiddenBefore = duckGuidanceHidden();
            for (uint8_t ms = 0U; ms < 60U && Home(board, "a") == "settle"; ++ms) {
                hiddenBefore = hiddenBefore && duckGuidanceHidden();
                Check(board.Advance(1U) == VIRTUAL_LITCH_OK, run, "cross the handoff boundary");
            }
            const auto& now = board.SemanticFrame().board[duckTo];
            Check(Home(board, "a") == "duck" && hiddenBefore && Yellow(now[1U]),
                  run, "Duck landing first appears when chess confirmation hands off to Duck placement");
            checkpoint("continuous-Circle-handoff", "place-held-Duck");
        } else {
            Check(board.Advance(1000U) == VIRTUAL_LITCH_OK, run, "finish chess confirmation");
        }
    }
    if (duration == 0U && duckPickup == DuckPickup::AfterChessPlacement) {
        const uint32_t toPeak = 1200U - board.State().now_ms % 1200U + 72U;
        Check(board.Advance(toPeak) == VIRTUAL_LITCH_OK, run, "sample the required Duck lift reminder before pickup");
        const auto targets = hardware::MapSemanticSquareLedTargets(
            revision, DUCK, hardware::SquareLedSlot::Piece, mappedRotate);
        const auto color = board.PhysicalFrame().pixels[targets.indices[0U]];
        std::printf("[DUCK_HINT_REMINDER] run=%s t=%u square=%u mapped=%u rgbw=%u,%u,%u,%u current=%u next=lift-Duck\n",
            run, board.State().now_ms, DUCK, targets.indices[0U], color.r, color.g, color.b, color.w,
            board.PhysicalFrame().applied_led_current_ma);
        checkpoint("Duck-reminder", "lift-Duck");
    }
    occupancy &= ~(UINT64_C(1) << DUCK);
    Check(observe() && board.Advance(0U) == VIRTUAL_LITCH_OK, run, "lift Duck through production handoff");
    uint8_t retainedFrom = 64, retainedTo = 64;
    Check(board.TryGetHintMoveSquares(retainedFrom, retainedTo) && retainedFrom == DUCK && retainedTo == duckTo,
          run, "same compound Duck decision survives chess handoff");
    bool mappedLandingYellow = true;
    for (uint8_t target = 0U; target < duckTargetPiece.count; ++target)
        mappedLandingYellow = mappedLandingYellow && Yellow(board.PhysicalFrame().pixels[duckTargetPiece.indices[target]]);
    Check(mappedLandingYellow, run,
          "after chess commits, every mapped Duck landing Piece LED shows the retained yellow destination");
    const auto initialDuckFrame = board.SemanticFrame();
    const auto yellow = initialDuckFrame.board[duckTo][0];
    const uint8_t distance = static_cast<uint8_t>(std::max(
        std::abs(int(duckTo / 8U) - int(DUCK / 8U)), std::abs(int(duckTo % 8U) - int(DUCK % 8U))));
    bool yellowPath = distance <= 1U;
    bool yellowTransit = false, steadyEndpoints = true, yellowLanding = true, identities = true;
    const uint32_t pass = std::max(900U, 480U * distance) + 240U;
    for (uint32_t age = 0U; age <= pass; age += 20U) {
        const auto& frame = board.SemanticFrame();
        yellowLanding = yellowLanding && Yellow(frame.board[duckTo][1U]);
        for (uint8_t slot = 0U; slot < frame.leds_per_square; ++slot) {
            steadyEndpoints = steadyEndpoints &&
                Equal(initialDuckFrame.board[DUCK][slot], frame.board[DUCK][slot]) &&
                Equal(initialDuckFrame.board[duckTo][slot], frame.board[duckTo][slot]);
        }
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (square == DUCK || square == duckTo) continue;
            identities = identities && ((occupancy & (UINT64_C(1) << square)) == 0ULL ||
                !Yellow(frame.board[square][1U]));
            for (uint8_t slot = 0U; slot < 3U; ++slot) {
                const auto color = frame.board[square][slot];
                if (Yellow(color)) {
                    if (!yellowPath) std::printf("[DUCK_HINT_PATH] run=%s age=%u source=%u transit=%u target=%u slot=%u yellow=%u next=place-Duck-on-yellow-target\n",
                        run, age, DUCK, square, duckTo, slot, color.r);
                    yellowPath = true;
                    yellowTransit = true;
                }
            }
        }
        if (age + 20U <= pass) Check(board.Advance(20U) == VIRTUAL_LITCH_OK, run,
            "held Duck keeps its original target through one complete yellow route pass");
    }
    Check(Yellow(yellow) && yellowLanding && identities &&
          (minimal ? steadyEndpoints && !yellowTransit : yellowPath) &&
          board.TryGetHintMoveSquares(retainedFrom, retainedTo) && retainedFrom == DUCK && retainedTo == duckTo,
          run, minimal ? "Minimal held Duck source and landing stay steady without yellow travel or false piece identities"
                       : "Duck next-action landing and route are yellow, preserve other piece identities and keep the fixed target");
    const std::string stagedFen = Home(board, "fen");
    auto pending = [&]() { return board.UndoHistoryMoveCount() == 0U &&
        board.State().side_to_move_black == 0U && Home(board, "fen") == stagedFen; };
    occupancy |= UINT64_C(1) << duckTo;
    Check(observe() && pending(), run, "placement remains one staged uncommitted turn");
    Check(board.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling),
          run, "selectable Duck uses shared Settling phase");
    checkpoint("Duck-placed", duration ? "wait-or-relift" : "press-Circle-or-relift");
    Check(Home(board, "a") == "settle", run,
          "Home reports pending confirmation instead of asking to place the already placed Duck");
    Check(board.Advance(100U) == VIRTUAL_LITCH_OK, run, "sample pending placement");
    if (correction) {
        constexpr uint8_t unrelatedMoverSquare = 51U; // White d2 pawn.
        occupancy &= ~(UINT64_C(1) << unrelatedMoverSquare);
        Check(observe(), run, "unrelated pickup reaches production correction");
        Check(Home(board, "a") == "corr" &&
                  board.UndoHistoryMoveCount() == 0U &&
                  Home(board, "fen") == stagedFen,
              run, "mover-side pickup cannot confirm the staged Duck turn");
        checkpoint("unrelated-piece-lifted", "restore-d2");
        occupancy |= UINT64_C(1) << unrelatedMoverSquare;
        Check(observe() && pending(), run,
              "restore unrelated piece through production correction");
        Check(board.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                  Home(board, "a") == "settle",
              run, "correction restoration resumes Duck settling authority");
        checkpoint("unrelated-piece-restored", "wait-for-shared-confirmation");
        if (duration != 0U) {
            Check(board.Advance(duration - 1U) == VIRTUAL_LITCH_OK && pending(),
                  run, "restored Duck stays pending through a fresh timed deadline minus 1ms");
            Check(board.Advance(1U) == VIRTUAL_LITCH_OK,
                  run, "restored Duck reaches the fresh shared deadline");
        } else {
            Check(board.Advance(6000U) == VIRTUAL_LITCH_OK && pending(),
                  run, "restored Duck remains pending under Confirm policy");
            Check(button(white, runtime::VirtualLitchPhysicalButton::O) &&
                      board.Advance(1000U) == VIRTUAL_LITCH_OK,
                  run, "Circle confirms restored Duck through the shared owner");
        }
        Check(board.UndoHistoryMoveCount() == 1U &&
                  board.State().side_to_move_black == 1U,
              run, "restored Duck placement commits exactly one compound turn");
        checkpoint("correction-deadline-observed", "expected-Black-selects-chess-move");
        return;
    }
    occupancy &= ~(UINT64_C(1) << duckTo);
    Check(observe() && pending(), run, "relift cancels pending placement");
    Check(board.Advance(100U) == VIRTUAL_LITCH_OK, run, "held Duck remains responsive");
    Check(board.TryGetHintMoveSquares(retainedFrom, retainedTo) && retainedTo == duckTo,
          run, "relift retains original compound suggestion");
    checkpoint("Duck-relifted", "replace-Duck-on-same-suggestion");
    occupancy |= UINT64_C(1) << duckTo;
    Check(observe() && pending(), run, "replacement starts fresh confirmation");
    if (duration) {
        Check(board.Advance(duration - 1U) == VIRTUAL_LITCH_OK && pending(), run, "pending at fresh deadline minus 1ms");
        Check(board.Advance(1U) == VIRTUAL_LITCH_OK, run, "reach shared deadline");
    } else {
        Check(board.Advance(6000U) == VIRTUAL_LITCH_OK && pending(), run, "Confirm never auto-commits");
        Check(button(white, runtime::VirtualLitchPhysicalButton::O) && board.Advance(1000U) == VIRTUAL_LITCH_OK,
              run, "Circle confirms through existing finalize animation");
    }
    Check(board.UndoHistoryMoveCount() == 1U && board.State().side_to_move_black == 1U,
          run, "exactly one compound turn commits");
    if (checkPickupOrder) {
        const uint64_t start = occupancy;
        constexpr uint8_t BLACK_PAWN = 12U; // e7
        constexpr uint8_t BLACK_TARGET = 20U; // e6
        occupancy &= ~(UINT64_C(1) << duckTo);
        Check(observe() && Home(board, "a") == "corr", run,
              "Duck-first pickup on an active turn asks the player to restore it");
        occupancy &= ~(UINT64_C(1) << BLACK_PAWN);
        Check(observe(), run, "attempt a chess pickup after Duck-first correction");
        occupancy |= UINT64_C(1) << BLACK_TARGET;
        Check(observe() && board.Advance(5000U) == VIRTUAL_LITCH_OK &&
              board.UndoHistoryMoveCount() == 1U && Home(board, "a") == "corr",
              run, "a later legal chess landing cannot bypass the pickup order");
        checkpoint("Duck-first-rejected", "return-Duck-and-chess-piece");
        occupancy = start;
        Check(observe() && Home(board, "a") == "idle" &&
              board.State().side_to_move_black == 1U && board.UndoHistoryMoveCount() == 1U,
              run, "restoring the board keeps the same turn and history");
    }
    checkpoint("compound-committed", "tap-X-for-real-Undo");
    Check(button(black, runtime::VirtualLitchPhysicalButton::X), run, "other bank requests real Undo");
    Check(board.IsUndoActionPending() && !board.HintDisplayRequested(), run, "Undo owns fixed return and suppresses Hint");
    checkpoint("Undo-fixed-return", "return-Duck-to-original-square");
}
}
int main() {
    for (bool firstP2 : {false, true}) for (bool rotate : {false, true})
        for (bool coaching : {false, true}) {
            Journey("settle_2s", 2000U, firstP2, rotate, coaching);
            Journey("settle_5s", 5000U, firstP2, rotate, coaching);
            Journey("press_circle_to_confirm", 0U, firstP2, rotate, coaching);
        }
    Journey("settle_2s", 2000U, false, false, false, true);
    Journey("press_circle_to_confirm", 0U, false, false, false, true);
    Journey("settle_2s", 2000U, false, false, true, false, true, DuckPickup::AfterChessPickup);
    Journey("press_circle_to_confirm", 0U, false, false, true, false, false, DuckPickup::AfterChessPickup);
    Journey("settle_2s", 2000U, false, false, true, false, false, DuckPickup::AfterChessPickup, DuckPlacementBatch::Unique);
    Journey("press_circle_to_confirm", 0U, false, false, true, false, false, DuckPickup::AfterChessPickup, DuckPlacementBatch::Unique);
    Journey("settle_2s", 2000U, false, false, true, false, false, DuckPickup::AfterChessPickup, DuckPlacementBatch::Ambiguous);
    Journey("settle_2s", 2000U, false, false, true, false, true);
    Journey("settle_2s", 2000U, false, false, false, false, false,
            DuckPickup::AfterChessPlacement, DuckPlacementBatch::Separate, 1U);
    Journey("settle_2s", 2000U, false, false, false, false, false,
            DuckPickup::AfterChessPlacement, DuckPlacementBatch::Separate, 2U);
    std::printf("DuckHintSettling failures=%d\n", failures);
    return failures ? 1 : 0;
}
