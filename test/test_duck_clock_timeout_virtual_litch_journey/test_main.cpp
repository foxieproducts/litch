#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "virtual_litch_session.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/game_persistence.hpp"
#include "modules/orientation.hpp"
#include "modules/settings_store.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr const char* TERMINAL_KEY = "GAME_CLOCK_TERMINAL_V1";
constexpr uint8_t DUCK = 24U; // a5, outside both pawn gestures.
int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] DUCK_CLOCK_TIMEOUT_JOURNEY: %s\n", message);
        ++g_failures;
    }
}

void RemoveStore(const char* path) {
    (void)std::remove(path);
    char sibling[192] = {};
    for (const char* suffix : {".bak", ".tmp"}) {
        std::snprintf(sibling, sizeof(sibling), "%s%s", path, suffix);
        (void)std::remove(sibling);
    }
}

bool Observe(runtime::VirtualLitchSession& session, const uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == logical;
}

bool ReadHome(const runtime::VirtualLitchSession& session, JsonDocument& doc) {
    const DeserializationError error =
        deserializeJson(doc, session.HomeStateLine().CStr());
    if (error != DeserializationError::Ok) {
        std::printf("[HOME_PARSE] error=%s capacity=%zu bytes=%zu\n",
                    error.c_str(), doc.capacity(), session.HomeStateLine().Size());
    }
    return error == DeserializationError::Ok;
}

bool Set(runtime::VirtualLitchSession& session, const uint16_t id,
         const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

void StagePawn(runtime::VirtualLitchSession& session, uint64_t& occupancy,
               const uint8_t from, const uint8_t to) {
    occupancy &= ~(UINT64_C(1) << from);
    Check(Observe(session, occupancy), "raw pawn pickup admitted");
    occupancy |= UINT64_C(1) << to;
    Check(Observe(session, occupancy), "raw pawn destination observed");
    // Real settle, dispatcher, coordinator action and acknowledgement owners.
    Check(session.Advance(2600U) == VIRTUAL_LITCH_OK &&
              session.Advance(1U) == VIRTUAL_LITCH_OK,
          "physical pawn move reaches its normal settle boundary");
    StaticJsonDocument<1536> home;
    Check(ReadHome(session, home) &&
              std::strcmp(home["a"] | "", "duck") == 0,
          "completed chess half requests mandatory Duck relocation");
}

char PieceAt(const char* fen, const uint8_t square) {
    uint8_t index = 0U;
    for (const char* p = fen; p != nullptr && *p != '\0' && *p != ' '; ++p) {
        if (*p == '/') continue;
        if (*p >= '1' && *p <= '8') {
            index = static_cast<uint8_t>(index + (*p - '0'));
        } else {
            if (index == square) return *p;
            ++index;
        }
    }
    return '\0';
}

void TerminalCheckpoint(runtime::VirtualLitchSession& session,
                         const bool loserBlack, const uint64_t terminalOccupancy,
                         const uint8_t movedPawn, const char* stage,
                         const bool retainPawn = true) {
    StaticJsonDocument<1536> home;
    Check(ReadHome(session, home), "terminal Home wire JSON parses");
    const char* winner = loserBlack ? "w" : "b";
    Check(std::strcmp(home["tw"] | "", winner) == 0 &&
              home["cl"][loserBlack ? 2U : 1U].as<uint32_t>() == 0U &&
              home["cl"][4U].as<uint32_t>() == 0U,
          "correct timeout winner remains published with a stopped zero clock");
    Check(std::strcmp(home["a"] | "", "idle") == 0 &&
              !(home["fs"] | false) && !(home["pm"] | false) &&
              std::strcmp(home["cl"][7U] | "", "n") == 0,
          "terminal Home never asks for a move, setup or correction");
    if (retainPawn) {
        Check(PieceAt(home["fen"] | "", movedPawn) == (loserBlack ? 'p' : 'P') &&
                  session.State().expected_occupied_mask == terminalOccupancy,
              "terminal presentation retains acknowledged advanced pawn and Duck");
    }

    const bool winnerIsP2 = !loserBlack;
    const uint8_t winnerBase = winnerIsP2 ? 0U : 16U;
    const uint8_t loserBase = winnerIsP2 ? 16U : 0U;
    const bool effectiveRotate = modules::ComputeControlBankSwap(
        session.BoardRotate(), session.FirstPlayerIsP2(), false);
    uint16_t winnerAccent = 0U;
    uint16_t loserAccent = 0U;
    for (uint8_t offset = 0U; offset < 8U; ++offset) {
        const auto winning = session.SemanticFrame().status[winnerBase + offset];
        const auto losing = session.SemanticFrame().status[loserBase + offset];
        winnerAccent += winning.g + winning.b;
        loserAccent += losing.g + losing.b;
    }
    Check(winnerAccent > loserAccent + 80U,
          "winner's assigned home edge carries the victory color");
    const uint8_t physicalWinnerBase =
        modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
            winnerBase, effectiveRotate);
    uint16_t mappedAccent = 0U;
    for (uint8_t offset = 0U; offset < 8U; ++offset) {
        const auto pixel = session.PhysicalFrame().pixels[
            hardware::BOARD_LED_COUNT + physicalWinnerBase + offset];
        mappedAccent += pixel.g + pixel.b + pixel.w;
    }
    Check(mappedAccent != 0U, "S3 mapped physical winner edge remains illuminated");
    const bool winnerGreen = loserBlack != session.FirstPlayerIsP2();
    for (uint8_t index = 0U; index < 32U; ++index) {
        const bool winningEdge = index >= winnerBase && index < winnerBase + 8U;
        const auto semantic = session.SemanticFrame().status[index];
        const uint8_t physicalIndex =
            modules::status_led_frame_adapter::PhysicalIndexForSemanticIndex(
                index, effectiveRotate);
        const auto physical = session.PhysicalFrame().pixels[
            hardware::BOARD_LED_COUNT + physicalIndex];
        if (winningEdge) {
            Check((winnerGreen ? semantic.g > semantic.b : semantic.b > semantic.g) &&
                      physical.g + physical.b + physical.w > 0U,
                  "all eight winner status LEDs retain the victory color and mapped light");
        } else {
            Check(semantic.r == 0U && semantic.g == 0U && semantic.b == 0U &&
                      physical.r == 0U && physical.g == 0U &&
                      physical.b == 0U && physical.w == 0U,
                  "actual timeout leaves every non-winning status LED semantically and physically dark");
        }
    }
    if (retainPawn &&
        (session.State().physical_occupied_mask & (UINT64_C(1) << movedPawn)) != 0ULL) {
        const auto center = session.SemanticFrame().board[movedPawn][
            static_cast<uint8_t>(hardware::SquareLedSlot::Piece)];
        const bool pawnGreen = !loserBlack != session.FirstPlayerIsP2();
        Check(center.r == 0U && (pawnGreen ? center.g > 0U && center.b == 0U
                                         : center.b > 0U && center.g == 0U),
              "advanced pawn retains player color instead of correction rainbow");
    }
    std::printf(
        "[DUCK_CLOCK_TIMEOUT_JOURNEY] stage=%s loser=%s rotate=%u first_p2=%u "
        "t=%u physical=%016llx expected=%016llx winner_edge=%u home=%s\n",
        stage, loserBlack ? "Black" : "White", session.BoardRotate(),
        session.FirstPlayerIsP2(), session.State().now_ms,
        static_cast<unsigned long long>(session.State().physical_occupied_mask),
        static_cast<unsigned long long>(session.State().expected_occupied_mask),
        physicalWinnerBase, session.HomeStateLine().CStr());
    std::printf("[VICTORY_STATUS_FRAME] stage=%s t=%u loser=%s rotate=%u first_p2=%u "
                "board_hash=%016llx semantic_rgb=", stage, session.State().now_ms,
                loserBlack ? "Black" : "White", session.BoardRotate(), session.FirstPlayerIsP2(),
                static_cast<unsigned long long>(session.SemanticFrame().board_hash));
    for (const auto color : session.SemanticFrame().status) {
        std::printf("%02x%02x%02x,", color.r, color.g, color.b);
    }
    std::printf(" physical_rgbw=");
    for (uint8_t index = 0U; index < 32U; ++index) {
        const auto color = session.PhysicalFrame().pixels[hardware::BOARD_LED_COUNT + index];
        std::printf("%02x%02x%02x%02x,", color.r, color.g, color.b, color.w);
    }
    std::printf(" next=hold-either-X+O-2s;early-release-keeps-result\n");
}

void Save(runtime::VirtualLitchSession& session) {
    bool saved = false;
    session.RunControlIngress(
        [](void* context, modules::SettingsStore& settings,
           modules::ControlMutationService&,
           modules::GamePersistenceCoordinator&) {
            *static_cast<bool*>(context) = settings.SaveNow();
        }, &saved);
    Check(saved, "native file save writes the coordinator-prepared terminal document");
}

void DuckClockTimeoutAfterPawnBeforeDuck(const bool rotate,
                                        const bool firstP2,
                                        const bool loserBlack) {
    char path[128] = {};
    std::snprintf(path, sizeof(path), "build/duck_timeout_%u_%u_%u.json",
                  rotate, firstP2, loserBlack);
    RemoveStore(path);
    {
        // Owner/orientation preferences exist before boot, avoiding an
        // unrelated in-progress K/Q reorientation gesture in this journey.
        modules::SettingsStoreConfig storeConfig;
        storeConfig.path = path;
        modules::SettingsStore settings(storeConfig);
        Check(settings.Load(), "fresh native preferences load");
        settings.Doc()["BOARD_ROTATE"] = rotate;
        settings.Doc()["GAME_FIRST_PLAYER"] = firstP2 ? 2U : 1U;
        Check(settings.SaveNow(), "persist only pre-boot player-end preferences");
    }
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession sessionFixture(config, path);
    auto& session = sessionFixture.session;
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
              Set(session, 100U, "BOARD_ROTATE", rotate ? "true" : "false") &&
              Set(session, 101U, "GAME_FIRST_PLAYER", firstP2 ? "2" : "1") &&
              Set(session, 102U, "TIMER", "1") &&
              Set(session, 103U, "BOARD_SETUP_ASSISTANCE", "false") &&
              Set(session, 104U, "P1_END_PIECE_WHEEL", "85") &&
              Set(session, 105U, "P2_END_PIECE_WHEEL", "170"),
          "normal acknowledged configuration precedes real Duck setup");
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK);
    Check(Observe(session, occupancy) && session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              session.GameVariant() == modules::GameVariant::Duck,
          "raw initial Hall position selects Duck and arms clock stability");
    if (loserBlack) {
        StagePawn(session, occupancy, 52U, 36U);
        occupancy &= ~(UINT64_C(1) << DUCK);
        Check(Observe(session, occupancy), "first completed turn lifts Duck");
        occupancy |= UINT64_C(1) << 25U;
        Check(Observe(session, occupancy) && session.UndoHistoryMoveCount() == 0U &&
                  session.Advance(2000U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U,
              "first compound turn settles before commit and Black gesture");
    }
    const uint8_t pawnFrom = loserBlack ? 12U : 52U;
    const uint8_t pawnTo = loserBlack ? 28U : 36U;
    StagePawn(session, occupancy, pawnFrom, pawnTo);
    StaticJsonDocument<1536> before;
    Check(ReadHome(session, before), "pending-turn Home snapshot reads");
    const uint32_t seconds = before["cl"][loserBlack ? 2U : 1U] | 0U;
    Check(seconds > 1U && before["cl"][4U].as<uint32_t>() == 1U,
          "moving side's real clock continues through mandatory Duck placement");
    const modules::FenString canonical(
        session.SettingsForPersistenceTest().Doc()[modules::KEY_GAME_FEN] | "");
    modules::GamePersistenceState beforeState;
    Check(modules::ReadGamePersistenceState(
              session.SettingsForPersistenceTest().Doc(), beforeState),
          "capture last committed compound-turn authority");
    Check(session.Advance((seconds - 1U) * 1000U) == VIRTUAL_LITCH_OK &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "actual clock reaches zero while pawn is placed and Duck stays put");
    TerminalCheckpoint(session, loserBlack, occupancy, pawnTo, "expiry");
    const uint64_t firstFrame = session.SemanticFrame().combined_hash;
    for (const uint32_t delta : std::array<uint32_t, 4>{180U, 370U, 650U, 1100U}) {
        Check(session.Advance(delta) == VIRTUAL_LITCH_OK, "advance terminal animation");
        TerminalCheckpoint(session, loserBlack, occupancy, pawnTo, "winner-animation");
    }
    Check(firstFrame != session.SemanticFrame().combined_hash,
          "existing victory animation advances through repeated timed frames");
    modules::GamePersistenceState terminal;
    Check(modules::ReadGamePersistenceState(
              session.SettingsForPersistenceTest().Doc(), terminal) &&
              terminal.fen.Equals(canonical.View()) &&
              terminal.undoHistoryCount == beforeState.undoHistoryCount &&
              terminal.interactionState.Empty() && !terminal.cpuMoveValid &&
              terminal.clockState.clock.timedOut,
          "timeout saves no incomplete history, executable plan or CPU token");
    Check(terminal.clockState.clock.activeSide ==
              beforeState.clockState.clock.activeSide &&
              (loserBlack ? terminal.clockState.clock.remainingMsWhite
                          : terminal.clockState.clock.remainingMsBlack) ==
              (loserBlack ? beforeState.clockState.clock.remainingMsWhite
                          : beforeState.clockState.clock.remainingMsBlack),
          "timeout switches no side and adds no increment or opponent time");

    Save(session);
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, occupancy),
          "native persisted reboot restores without resuming a pending turn");
    TerminalCheckpoint(session, loserBlack, occupancy, pawnTo, "restored");
    const uint64_t terminalOccupancy = occupancy;
    const uint8_t duckFrom = loserBlack ? 25U : DUCK;
    occupancy &= ~(UINT64_C(1) << duckFrom);
    Check(Observe(session, occupancy), "late Duck pickup remains observable");
    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine publication;
    Check(session.TryPrepareHomePublication(publisher, true, publication),
          "late Duck pickup cannot suppress terminal publication as reversible setup");
    occupancy |= UINT64_C(1) << 26U;
    Check(Observe(session, occupancy) && session.Advance(2600U) == VIRTUAL_LITCH_OK,
          "late Duck placement cannot complete timed-out turn");
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_ABI_VERSION;
    request.request_id = 900U;
    request.action = VIRTUAL_LITCH_CONTROL_UNDO;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    request.undo_move_count = 1U;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.requestId == request.request_id &&
              outcome.status == modules::ControlMutationStatus::Rejected,
          "actual control owner completes deferred Undo with terminal rejection");
    TerminalCheckpoint(session, loserBlack, terminalOccupancy, pawnTo, "late-placement-undo");

    // Reproduce the supported legacy separate TIMER write in the saved
    // document after the real gesture. Current admission rejects that write;
    // an older candidate could accept it without replacing the clock record.
    Save(session);
    std::array<char, 4096> terminalDocument{};
    Check(serializeJson(session.SettingsForPersistenceTest().Doc(),
                        terminalDocument.data(), terminalDocument.size()) <
              terminalDocument.size() - 1U,
          "retain exact terminal document for independent compatibility cases");
    constexpr std::array<const char*, 4> extensionNames{
        "absent-extension", "stale-extension", "malformed-extension", "valid-extension"};
    for (uint8_t extensionKind = 0U; extensionKind < extensionNames.size();
         ++extensionKind) {
        auto& doc = session.SettingsForPersistenceTest().Doc();
        doc.clear();
        Check(deserializeJson(doc, static_cast<const char*>(terminalDocument.data())) ==
                  DeserializationError::Ok,
              "each compatibility case starts from the real terminal record");
        doc["TIMER"] = 2U;
        if (extensionKind == 0U) {
            (void)doc.remove(TERMINAL_KEY);
        } else {
            JsonArray extension = doc[TERMINAL_KEY].to<JsonArray>();
            const JsonArrayConst clock =
                doc[modules::KEY_GAME_CLOCK_V1].as<JsonArrayConst>();
            extension.add(1U);
            extension.add(clock[1U].as<uint32_t>());
            extension.add(clock[2U].as<uint32_t>() + (extensionKind == 1U ? 1U : 0U));
            extension.add(const_cast<char*>(canonical.CStr()));
            extension.add(extensionKind == 2U ? "invalid"
                                              : (loserBlack ? "e7e5" : "e2e4"));
        }
        Save(session);
        Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, terminalOccupancy),
              "newer TIMER and optional display never reject valid terminal restore");
        TerminalCheckpoint(session, loserBlack, terminalOccupancy, pawnTo,
                           extensionNames[extensionKind], extensionKind == 3U);
        StaticJsonDocument<1536> restored;
        Check(ReadHome(session, restored) &&
                  restored["cl"][6U].as<uint32_t>() == 1U &&
                  session.SettingsForPersistenceTest().ReadValue("TIMER", 0U) == 2U,
              "Home uses terminal episode preset and retains next-game preference");
        if (extensionKind != 3U) {
            Check(std::strcmp(restored["fen"] | "", canonical.CStr()) == 0,
                  "invalid optional display falls back to canonical terminal position");
        }
        modules::ControlMutationOutcome timerOutcome;
        Check(session.MutateSettingJson(950U, "TIMER", "3", timerOutcome) ==
                  VIRTUAL_LITCH_OK &&
                  timerOutcome.status == modules::ControlMutationStatus::Rejected,
              "current command owner rejects a new terminal TIMER mutation");
        Check(session.Advance(2300U) == VIRTUAL_LITCH_OK,
              "ordinary updates cannot apply next-game preference to terminal episode");
        Save(session);
        Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, terminalOccupancy),
              "second native save/reboot preserves the same terminal authority");
        TerminalCheckpoint(session, loserBlack, terminalOccupancy, pawnTo,
                           "second-restore", extensionKind == 3U);
    }

    // The raw adapter does not bind firmware's ResetHoldGestureController.
    // Reuse the actual New Game command owner; physical X+O remains HIL.
    request.request_id = 901U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.requestId == request.request_id &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "explicit New Game completes through the actual deferred command owner");
    Check(Observe(session, OPENING) &&
              session.Advance(1000U) == VIRTUAL_LITCH_OK,
          "restore ordinary fresh setup only after explicit New Game");
    StaticJsonDocument<1536> reset;
    Check(ReadHome(session, reset) && !reset.containsKey("tw") &&
              reset["cl"][1U].as<uint32_t>() == 180U &&
              reset["cl"][2U].as<uint32_t>() == 180U &&
              reset["cl"][6U].as<uint32_t>() == 2U &&
              !session.SettingsForPersistenceTest().Doc().containsKey(TERMINAL_KEY),
          "only explicit New Game clears timeout/presentation and uses newer TIMER");
    RemoveStore(path);
}

} // namespace

int main() {
    for (const bool rotate : {false, true}) {
        for (const bool firstP2 : {false, true}) {
            for (const bool loserBlack : {false, true}) {
                DuckClockTimeoutAfterPawnBeforeDuck(rotate, firstP2, loserBlack);
            }
        }
    }
    std::printf("DUCK_CLOCK_TIMEOUT_JOURNEY failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
