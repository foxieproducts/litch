#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "virtual_litch_session.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/white_squares_setting.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] near dark: %s\n", message); }
}

bool IsColor(const virtual_litch_color& color,
             const uint8_t r,
             const uint8_t g,
             const uint8_t b,
             const uint8_t w) {
    return color.r == r && color.g == g && color.b == b && color.w == w;
}

const virtual_litch_color& PhysicalSquareLed(
    const runtime::VirtualLitchSession& session,
    const uint8_t square,
    const hardware::SquareLedSlot slot) {
    return session.PhysicalFrame().pixels[
        hardware::LedIndexForSquare(square, slot)];
}

bool MutateSetting(runtime::VirtualLitchSession& session,
                   const uint16_t requestId,
                   const char* key,
                   const char* valueJson) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(requestId, key, valueJson, outcome) ==
               VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           outcome.reason == modules::ControlMutationReason::None;
}

void PrintBackgroundCheckpoint(const runtime::VirtualLitchSession& session,
                               const char* stage,
                               const uint8_t level,
                               const char* input,
                               const char* acknowledgement,
                               const char* nextAction,
                               const uint8_t backgroundSquare,
                               const uint8_t p1PieceSquare,
                               const uint8_t p2PieceSquare) {
    const auto& state = session.State();
    const auto& physical = session.PhysicalFrame();
    const auto& background = PhysicalSquareLed(
        session, backgroundSquare, hardware::SquareLedSlot::Square0);
    const auto& p1Piece = PhysicalSquareLed(
        session, p1PieceSquare, hardware::SquareLedSlot::Piece);
    const auto& p2Piece = PhysicalSquareLed(
        session, p2PieceSquare, hardware::SquareLedSlot::Piece);
    std::printf(
        "[ORDINARY_LIGHT_BACKGROUND] stage=%s level=%u input=%s ack=%s "
        "phase=%u physical=%016llX expected=%016llX correction=%016llX "
        "limited=%u bg=%u/%u/%u/%u p1=%u/%u/%u/%u "
        "p2=%u/%u/%u/%u next=%s\n",
        stage, static_cast<unsigned>(level), input, acknowledgement,
        static_cast<unsigned>(state.interaction_phase),
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.correction_mask),
        static_cast<unsigned>(physical.power_limited),
        static_cast<unsigned>(background.r),
        static_cast<unsigned>(background.g),
        static_cast<unsigned>(background.b),
        static_cast<unsigned>(background.w),
        static_cast<unsigned>(p1Piece.r),
        static_cast<unsigned>(p1Piece.g),
        static_cast<unsigned>(p1Piece.b),
        static_cast<unsigned>(p1Piece.w),
        static_cast<unsigned>(p2Piece.r),
        static_cast<unsigned>(p2Piece.g),
        static_cast<unsigned>(p2Piece.b),
        static_cast<unsigned>(p2Piece.w), nextAction);
}
}
void RunJourney(const bool disabled) {
    char directory[] = "/tmp/litch-near-dark-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "fixture created");
    const std::string path = std::string(directory) + "/settings.json";
    std::ofstream(path) << (disabled ? "{invalid" : "{}");
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession sessionFixture(config, path.c_str());
    auto& session = sessionFixture.session;
    const uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    session.ObserveHallMask(opening); session.Advance(200); session.Advance(2000);
    constexpr uint8_t ordinaryLightSquare = 34U;
    constexpr uint8_t p1PawnSquare = 52U;
    constexpr uint8_t p2PawnSquare = 12U;
    constexpr std::array<uint8_t, 9> expectedOrdinaryCodes{
        0U, 1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U};

    if (!disabled) {
        Check(MutateSetting(session, 500U, "P1_END_PIECE_WHEEL", "147") &&
                  MutateSetting(session, 501U, "P2_END_PIECE_WHEEL", "213") &&
                  MutateSetting(session, 502U, "BRIGHTNESS", "4") &&
                  MutateSetting(session, 503U, "BRIGHTNESS", "5"),
              "representative palette and brightness changes are acknowledged");
        Check(session.PhysicalFrame().power_limited == 0U &&
                  IsColor(PhysicalSquareLed(session, p1PawnSquare,
                                            hardware::SquareLedSlot::Piece),
                          0U, 35U, 48U, 0U) &&
                  IsColor(PhysicalSquareLed(session, p2PawnSquare,
                                            hardware::SquareLedSlot::Piece),
                          64U, 0U, 35U, 0U),
              "unthrottled 147/213 piece hues retain their calibrated level-5 physical codes");
        Check(IsColor(PhysicalSquareLed(session, ordinaryLightSquare,
                                        hardware::SquareLedSlot::Square0),
                      5U, 5U, 5U, 5U),
              "ordinary level-5 light-square background is reduced to code 5");

        Check(MutateSetting(session, 504U, "P1_END_PIECE_WHEEL", "140") &&
                  MutateSetting(session, 505U, "P2_END_PIECE_WHEEL", "101"),
              "currently observed 140/101 palette changes are acknowledged");
        Check(session.PhysicalFrame().power_limited == 0U &&
                  IsColor(PhysicalSquareLed(session, p1PawnSquare,
                                            hardware::SquareLedSlot::Piece),
                          0U, 45U, 46U, 0U) &&
                  IsColor(PhysicalSquareLed(session, p2PawnSquare,
                                            hardware::SquareLedSlot::Piece),
                          0U, 86U, 14U, 0U),
              "unthrottled 140/101 piece hues retain their calibrated level-5 physical codes");

        for (uint8_t level = 0U; level <= 8U; ++level) {
            char levelJson[2] = {static_cast<char>('0' + level), '\0'};
            const bool levelApplied = MutateSetting(
                session, static_cast<uint16_t>(510U + level),
                "BRIGHTNESS", levelJson);
            Check(levelApplied,
                  "level sweep mutation is acknowledged");
            const uint8_t expected = expectedOrdinaryCodes[level];
            const auto& square0 = PhysicalSquareLed(
                session, ordinaryLightSquare, hardware::SquareLedSlot::Square0);
            const auto& square2 = PhysicalSquareLed(
                session, ordinaryLightSquare, hardware::SquareLedSlot::Square2);
            Check(session.PhysicalFrame().power_limited == 0U &&
                      IsColor(square0, expected, expected, expected, expected) &&
                      IsColor(square2, expected, expected, expected, expected),
                  "unthrottled ordinary light-square code follows the 0-8 sweep");
            Check(PhysicalSquareLed(session, p1PawnSquare,
                                   hardware::SquareLedSlot::Piece).b != 0U &&
                      PhysicalSquareLed(session, p2PawnSquare,
                                        hardware::SquareLedSlot::Piece).g != 0U,
                  "both representative piece hues survive the 0-8 sweep");
            if (level == 0U || level == 5U || level == 8U) {
                PrintBackgroundCheckpoint(
                    session, "enabled", level, "BRIGHTNESS-setting",
                    levelApplied ? "Applied" : "Missing", "continue-play",
                    ordinaryLightSquare,
                    p1PawnSquare, p2PawnSquare);
            }
        }
        Check(MutateSetting(session, 518U, "BRIGHTNESS", "0"),
              "minimum-piece inspection selects level zero");
        session.Advance(2000U);
        const auto& minimumP1 = PhysicalSquareLed(session, p1PawnSquare, hardware::SquareLedSlot::Piece);
        const auto& minimumP2 = PhysicalSquareLed(session, p2PawnSquare, hardware::SquareLedSlot::Piece);
        Check(minimumP1.b == 1U && minimumP2.g == 1U &&
                  minimumP1.r <= 1U && minimumP1.g <= 1U && minimumP1.w <= 1U &&
                  minimumP2.r <= 1U && minimumP2.b <= 1U && minimumP2.w <= 1U,
              "ordinary pieces reach minimum nonzero channel codes without dimming required guidance");
        const bool levelFiveRestored =
            MutateSetting(session, 519U, "BRIGHTNESS", "5");
        Check(levelFiveRestored,
              "journey restores level 5 before physical Quick Setup");
        PrintBackgroundCheckpoint(
            session, "enabled-restored", 5U, "BRIGHTNESS-setting",
            levelFiveRestored ? "Applied" : "Missing", "lift-e2",
            ordinaryLightSquare, p1PawnSquare, p2PawnSquare);

        const auto destinationBackground = PhysicalSquareLed(
            session, 36U, hardware::SquareLedSlot::Square0);
        const uint64_t liftedAtLevelFive =
            opening & ~(UINT64_C(1) << p1PawnSquare);
        const bool liftAccepted =
            session.ObserveHallMask(liftedAtLevelFive) == VIRTUAL_LITCH_OK;
        const virtual_litch_color destination0 = PhysicalSquareLed(
            session, 36U, hardware::SquareLedSlot::Square0);
        const virtual_litch_color destination2 = PhysicalSquareLed(
            session, 36U, hardware::SquareLedSlot::Square2);
        const auto matchesBackground = [&](const virtual_litch_color cue) {
            return IsColor(cue, destinationBackground.r, destinationBackground.g,
                           destinationBackground.b, destinationBackground.w);
        };
        Check(liftAccepted &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::PieceLifted) &&
                  session.State().physical_occupied_mask == liftedAtLevelFive &&
                  session.State().expected_occupied_mask == opening &&
                  session.State().correction_mask == 0ULL &&
                  (session.State().valid_destination_mask &
                   (UINT64_C(1) << 36U)) != 0ULL &&
                  IsColor(destinationBackground, 5U, 5U, 5U, 5U) &&
                  matchesBackground(destination0) && matchesBackground(destination2),
              "level-5 pickup keeps the light-square background until the wave reaches e4");
        const bool revealComplete = session.Advance(220U) == VIRTUAL_LITCH_OK;
        const virtual_litch_color steadyDestination = PhysicalSquareLed(
            session, 36U, hardware::SquareLedSlot::Square0);
        Check(revealComplete && IsColor(steadyDestination, 0U, 18U, 48U, 0U) &&
                  IsColor(PhysicalSquareLed(session, 36U, hardware::SquareLedSlot::Square2),
                          0U, 18U, 48U, 0U),
              "both mapped level-5 destination rails reach the calibrated P1 color after the reveal");
        const bool returnAccepted =
            session.ObserveHallMask(opening) == VIRTUAL_LITCH_OK;
        const auto returnAcknowledgement = PhysicalSquareLed(
            session, p1PawnSquare, hardware::SquareLedSlot::Square0);
        Check(returnAccepted &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::Idle) &&
                  session.State().physical_occupied_mask == opening &&
                  session.State().expected_occupied_mask == opening &&
                  session.State().correction_mask == 0ULL &&
                  !IsColor(returnAcknowledgement, 5U, 5U, 5U, 5U),
              "returning e2 restores the authoritative opening with a local acknowledgement");
        Check(session.Advance(560U) == VIRTUAL_LITCH_OK &&
                  IsColor(PhysicalSquareLed(
                              session, p1PawnSquare,
                              hardware::SquareLedSlot::Square0),
                          5U, 5U, 5U, 5U),
              "returned-piece acknowledgement expires to the ordinary background at 560 ms");
        std::printf(
            "[ORDINARY_LIGHT_GUIDANCE] level=5 input=lift-e2/return-e2 "
            "lift=%s return=%s phase=%u physical=%016llX expected=%016llX "
            "correction=%016llX prompt=%u/%u/%u/%u steady_prompt=%u/%u/%u/%u background=5/5/5/5 "
            "expiry=560ms next=play\n",
            liftAccepted ? "Accepted" : "Rejected",
            returnAccepted ? "Accepted" : "Rejected",
            static_cast<unsigned>(session.State().interaction_phase),
            static_cast<unsigned long long>(
                session.State().physical_occupied_mask),
            static_cast<unsigned long long>(
                session.State().expected_occupied_mask),
            static_cast<unsigned long long>(session.State().correction_mask),
            static_cast<unsigned>(destination0.r),
            static_cast<unsigned>(destination0.g),
            static_cast<unsigned>(destination0.b),
            static_cast<unsigned>(destination0.w),
            static_cast<unsigned>(steadyDestination.r),
            static_cast<unsigned>(steadyDestination.g),
            static_cast<unsigned>(steadyDestination.b),
            static_cast<unsigned>(steadyDestination.w));
    }
    const auto down = [&](runtime::VirtualLitchPhysicalButton button, bool held) {
        return session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, held) == VIRTUAL_LITCH_OK &&
               session.Advance(20) == VIRTUAL_LITCH_OK;
    };
    const auto tap = [&](runtime::VirtualLitchPhysicalButton button) {
        if (session.QuickSettingsActive() &&
            (button == runtime::VirtualLitchPhysicalButton::Left ||
             button == runtime::VirtualLitchPhysicalButton::Right) &&
            session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
        return down(button, true) && down(button, false);
    };
    Check(down(runtime::VirtualLitchPhysicalButton::X,true) && session.Advance(1000)==VIRTUAL_LITCH_OK &&
          down(runtime::VirtualLitchPhysicalButton::X,false) && session.QuickSettingsActive(), "raw X hold opens Quick Setup");
    for(unsigned i=0;i<3;++i) Check(tap(runtime::VirtualLitchPhysicalButton::Right), "navigate brightness");
    Check(session.QuickSettingsSelectedIndex()==modules::quick_settings_items::BRIGHTNESS, "brightness selected");
    for(unsigned i=0;i<5;++i) Check(tap(runtime::VirtualLitchPhysicalButton::Down), "step brightness down");
    modules::ControlMutationOutcome outcome;
    Check(session.TryGetLastBoardControlOutcome(outcome) && outcome.status==modules::ControlMutationStatus::Applied,
          "zero has production mutation acknowledgement");
    auto& settings=session.SettingsForPersistenceTest();
    Check(settings.ReadValue("BRIGHTNESS",99U)==0U && session.DisplayText()=="0", "new zero selected and displayed");
    Check(settings.Doc().containsKey("WHITE_SQUARES_ON"), "new zero records independent white-square choice");
    modules::white_squares_setting::MigrateLegacyWhiteSquaresSetting(settings);
    Check(settings.ReadValue("BRIGHTNESS",99U)==0U && modules::white_squares_setting::ReadWhiteSquaresOn(settings),
          "new zero cannot be mistaken for legacy off sentinel");
    Check(tap(runtime::VirtualLitchPhysicalButton::Down) && settings.ReadValue("BRIGHTNESS",99U)==0U,
          "down at zero is bounded");
    Check(session.PhysicalFrame().applied_brightness_scale_65535>0U &&
          session.PhysicalFrame().applied_brightness_scale_65535<=2048U, "guidance retains 8/256 while ordinary pieces use their minimum layer");
    const virtual_litch_color nearDarkPiece = PhysicalSquareLed(
        session, p1PawnSquare, hardware::SquareLedSlot::Piece);
    const bool whiteOffInput =
        tap(runtime::VirtualLitchPhysicalButton::Right) &&
        tap(runtime::VirtualLitchPhysicalButton::Down);
    modules::ControlMutationOutcome whiteOffOutcome;
    const bool whiteOffAcknowledged =
        session.TryGetLastBoardControlOutcome(whiteOffOutcome) &&
        whiteOffOutcome.status == modules::ControlMutationStatus::Applied &&
        whiteOffOutcome.reason == modules::ControlMutationReason::None;
    Check(whiteOffInput && whiteOffAcknowledged &&
          !modules::white_squares_setting::ReadWhiteSquaresOn(settings) &&
          settings.ReadValue("BRIGHTNESS",99U)==0U,
          "white-square switch remains independent");
    Check(IsColor(PhysicalSquareLed(session, ordinaryLightSquare,
                                    hardware::SquareLedSlot::Square0),
                  0U, 0U, 0U, 0U) &&
              IsColor(PhysicalSquareLed(session, p1PawnSquare,
                                        hardware::SquareLedSlot::Piece),
                      nearDarkPiece.r, nearDarkPiece.g, nearDarkPiece.b,
                      nearDarkPiece.w),
          "turning light squares off blacks only the ordinary background");
    PrintBackgroundCheckpoint(
        session, "off", 0U, "QuickSetup-LightSquares-Down",
        whiteOffAcknowledged ? "Applied" : "Missing", "exit-Quick-Setup",
        ordinaryLightSquare, p1PawnSquare, p2PawnSquare);
    Check(tap(runtime::VirtualLitchPhysicalButton::X) && !session.QuickSettingsActive(), "raw X exits Quick Setup");
    session.Advance(2000);
    unsigned minimumBlueTurn=32;
    for(unsigned tick=0;tick<30;++tick) {
        session.Advance(50);
        unsigned blueTurn=0;
        for(unsigned i=hardware::BOARD_LED_COUNT;i<session.PhysicalFrame().led_count;++i) {
            const auto& p=session.PhysicalFrame().pixels[i]; if(p.b>p.r) ++blueTurn;
        }
        if(blueTurn<minimumBlueTurn) minimumBlueTurn=blueTurn;
    }
    Check(minimumBlueTurn>0, "P1 Turn survives physical quantization throughout sampled cycle");
    const uint64_t lifted=opening & ~(UINT64_C(1)<<52);
    Check(session.ObserveHallMask(lifted)==VIRTUAL_LITCH_OK, "near-dark accepts e2 lift");
    Check(!IsColor(PhysicalSquareLed(session, 36U,
                                     hardware::SquareLedSlot::Square0),
                   0U, 0U, 0U, 0U) &&
              !IsColor(PhysicalSquareLed(session, 36U,
                                         hardware::SquareLedSlot::Square2),
                       0U, 0U, 0U, 0U),
          "legal e2 lift preserves the physical e4 destination prompt");
    const uint64_t moved=lifted | (UINT64_C(1)<<36);
    session.ObserveHallMask(moved); session.Advance(2100);
    Check(session.State().expected_occupied_mask==moved && session.State().correction_mask==0,
          "near-dark move settles normally");
    Check(!IsColor(PhysicalSquareLed(session, 36U,
                                     hardware::SquareLedSlot::Piece),
                   0U, 0U, 0U, 0U),
          "settled move preserves its representative physical piece cue");
    // Extra piece is a real invalid Hall observation, then ordinary recovery.
    session.ObserveHallMask(moved | (UINT64_C(1)<<40));
    session.Advance(200);
    bool correctionVisible=false;
    unsigned maxR=0,maxG=0,maxB=0;
    for(unsigned tick=0;tick<20;++tick) {
        session.Advance(50);
        for(unsigned slot=0;slot<hardware::LEDS_PER_SQUARE;++slot) {
            const auto& p=session.PhysicalFrame().pixels[hardware::LedIndexForSquare(40,static_cast<hardware::SquareLedSlot>(slot))];
            correctionVisible |= p.r>p.g && p.r>p.b;
            if(p.r>maxR) maxR=p.r; if(p.g>maxG) maxG=p.g; if(p.b>maxB) maxB=p.b;
        }
    }
    std::printf("[PLAYBACK] extra-piece mismatch=%016llX physical_red=%u maxima=%u/%u/%u home=%s\n",static_cast<unsigned long long>(session.State().physical_occupied_mask ^ session.State().expected_occupied_mask),unsigned(correctionVisible),maxR,maxG,maxB,session.HomeStateLine().CStr());
    Check((session.State().physical_occupied_mask ^ session.State().expected_occupied_mask)==(UINT64_C(1)<<40) && correctionVisible, "a3 extra-piece correction produces physical red cue");
    session.ObserveHallMask(moved); session.Advance(200); session.Advance(2000);
    Check(session.State().physical_occupied_mask==session.State().expected_occupied_mask, "remove extra piece restores normal turn");
    const bool backgroundRecoveryApplied =
        MutateSetting(session, 520U, "WHITE_SQUARES_ON", "true");
    Check(backgroundRecoveryApplied &&
              IsColor(PhysicalSquareLed(session, ordinaryLightSquare,
                                        hardware::SquareLedSlot::Square0),
                      0U, 0U, 0U, 0U),
          "level zero intentionally suppresses dim background even when enabled");
    PrintBackgroundCheckpoint(
        session, "recovered", 0U, "WHITE_SQUARES_ON-setting",
        backgroundRecoveryApplied ? "Applied" : "Missing", "continue-play",
        ordinaryLightSquare, 36U, p2PawnSquare);
    unsigned visible=0;
    for(unsigned i=0;i<session.PhysicalFrame().led_count;++i) {
        const auto& p=session.PhysicalFrame().pixels[i]; if(p.r||p.g||p.b||p.w) ++visible;
    }
    Check(visible>0, "near-dark retains nonblack physical output");
    std::printf("[PLAYBACK] near-dark session_only=%u input=raw-X-hold/right3/down5 ack=applied display=0 scale=%u white=independent exit=X visible=%u next=e2e4-accepted\n",
                unsigned(disabled), session.PhysicalFrame().applied_brightness_scale_65535,visible);
    std::filesystem::remove_all(directory);
}
void RunEmptySetupQuickSettingsJourney() {
    char directory[] = "/tmp/litch-empty-setup-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "empty setup fixture created");
    const std::string path = std::string(directory) + "/settings.json";
    std::ofstream(path) << "{}";
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession sessionFixture(config, path.c_str());
    auto& session = sessionFixture.session;
    session.ObserveHallMask(0U);
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = 800U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    request.owner = VIRTUAL_LITCH_OWNER_P1;
    modules::ControlMutationOutcome outcome;
    Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Accepted &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              session.TryTakeControlCompletion(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "empty setup begins through acknowledged New Game");
    session.Advance(2200);
    Check(std::string_view(session.HomeStateLine().CStr()).find("\"pm\":1") !=
              std::string_view::npos,
          "empty setup is authoritatively in placement mode");
    const auto down = [&](runtime::VirtualLitchPhysicalButton button, bool held) {
        return session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, button, held) ==
                   VIRTUAL_LITCH_OK &&
               session.Advance(20) == VIRTUAL_LITCH_OK;
    };
    const auto button = runtime::VirtualLitchPhysicalButton::X;
    Check(down(button, true), "empty setup entry press delivered");
    session.Advance(1000);
    Check(down(button, false), "empty setup entry release delivered");
    const bool opened = session.QuickSettingsActive();
    std::printf("[EMPTY_SETUP_QUICK_SETTINGS] input=%s physical=%016llX "
                "expected=%016llX accepted=%u next=adjust-brightness\n",
                "X-hold-1s",
                static_cast<unsigned long long>(session.State().physical_occupied_mask),
                static_cast<unsigned long long>(session.State().expected_occupied_mask),
                static_cast<unsigned>(opened));
    Check(opened, "empty setup permits Quick Setup before placing any pieces");
    if (opened) {
        const auto tap = [&](runtime::VirtualLitchPhysicalButton key) {
            if (session.QuickSettingsActive() &&
                (key == runtime::VirtualLitchPhysicalButton::Left ||
                 key == runtime::VirtualLitchPhysicalButton::Right) &&
                session.Advance(250U) != VIRTUAL_LITCH_OK) return false;
            return down(key, true) && down(key, false);
        };
        for (unsigned step = 0U; step < 3U; ++step) {
            Check(tap(runtime::VirtualLitchPhysicalButton::Right),
                  "empty setup navigates to brightness");
        }
        Check(session.QuickSettingsSelectedIndex() ==
                  modules::quick_settings_items::BRIGHTNESS,
              "empty setup selects brightness");
        for (unsigned step = 0U; step < 5U; ++step) {
            Check(tap(runtime::VirtualLitchPhysicalButton::Down),
                  "empty setup reduces brightness");
        }
        Check(session.TryGetLastBoardControlOutcome(outcome) &&
                  outcome.status == modules::ControlMutationStatus::Applied &&
                  session.SettingsForPersistenceTest().ReadValue("BRIGHTNESS", 99U) == 0U &&
                  session.DisplayText() == "0",
              "empty setup acknowledges and displays level zero");
        Check(tap(runtime::VirtualLitchPhysicalButton::X) &&
                  !session.QuickSettingsActive(),
              "empty setup exits Quick Setup with X");
        session.Advance(200U);
        Check(session.State().physical_occupied_mask == 0U &&
                  std::string_view(session.HomeStateLine().CStr()).find("\"pm\":1") !=
                      std::string_view::npos,
              "Quick Setup exit preserves empty-board placement");
        constexpr std::array<uint8_t, 2U> kingSquares{60U, 4U};
        std::array<unsigned, 2U> minimumBracket{1024U, 1024U};
        std::array<unsigned, 2U> maximumBracket{};
        const std::array<virtual_litch_color, 2U> centers{
            PhysicalSquareLed(session, kingSquares[0], hardware::SquareLedSlot::Piece),
            PhysicalSquareLed(session, kingSquares[1], hardware::SquareLedSlot::Piece)};
        for (unsigned tick = 0U; tick < 32U; ++tick) {
            session.Advance(50U);
            for (size_t king = 0U; king < kingSquares.size(); ++king) {
                const auto& left = PhysicalSquareLed(
                    session, kingSquares[king], hardware::SquareLedSlot::Square0);
                const auto& right = PhysicalSquareLed(
                    session, kingSquares[king], hardware::SquareLedSlot::Square2);
                const unsigned intensity = unsigned(left.r) + left.g + left.b + left.w;
                if (intensity < minimumBracket[king]) minimumBracket[king] = intensity;
                if (intensity > maximumBracket[king]) maximumBracket[king] = intensity;
                Check(IsColor(right, left.r, left.g, left.b, left.w),
                      "both sides of each king bracket share its cadence");
                Check(IsColor(PhysicalSquareLed(session, kingSquares[king],
                                              hardware::SquareLedSlot::Piece),
                              centers[king].r, centers[king].g,
                              centers[king].b, centers[king].w),
                      "missing king centers remain black while location brackets flash");
            }
        }
        for (size_t king = 0U; king < kingSquares.size(); ++king) {
            Check(IsColor(centers[king], 0U, 0U, 0U, 0U),
                  "near-dark missing king center is fully black before detection");
            Check(minimumBracket[king] > 0U &&
                      maximumBracket[king] > minimumBracket[king],
                  "both king brackets retain visible rest and flash at level zero");
        }
        std::printf("[EMPTY_SETUP_QUICK_SETTINGS] input=right3/down5/X "
                    "ack=Applied brightness=0 menu=closed placement=1 "
                    "e1_bracket=%u..%u e8_bracket=%u..%u center=black next=place-kings\n",
                    minimumBracket[0], maximumBracket[0],
                    minimumBracket[1], maximumBracket[1]);
    }
    std::filesystem::remove_all(directory);
}

int main() {
    RunEmptySetupQuickSettingsJourney();
    RunJourney(false);
    RunJourney(true);
    // Only the old missing-flag encoding migrates to level 1 with squares off.
    modules::SettingsStoreConfig legacyConfig; modules::SettingsStore legacy(legacyConfig); legacy.Doc()["BRIGHTNESS"]=0U;
    modules::white_squares_setting::MigrateLegacyWhiteSquaresSetting(legacy);
    Check(legacy.ReadValue("BRIGHTNESS",99U)==1U && !modules::white_squares_setting::ReadWhiteSquaresOn(legacy),
          "legacy zero migration preserved");
    return failures?1:0;
}
