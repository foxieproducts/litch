#include "bounded_hint_engine.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <ArduinoJson.h>

#include "foxie_embedded/millis.hpp"
#include "modules/orientation.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include <litch_max/chess_notation.hpp>
#include "runtime/chess_interaction_model.hpp"
#include "runtime/square_mapping.hpp"

namespace {
int failures = 0;
DynamicJsonDocument homeFixture(24576);
uint64_t homeFixtureSeen = 0ULL;
void Check(bool ok, const char* run, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s: %s\n", run, message); }
}
bool Eq(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
bool Rotate(const runtime::VirtualLitchSession& session) {
    return modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2());
}
bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
    uint64_t physical = 0ULL;
    for (uint8_t sq = 0U; sq < 64U; ++sq) {
        if ((logical >> sq) & 1ULL) physical |= 1ULL << runtime::MapSquareIndex(sq, Rotate(session));
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}
bool Setting(runtime::VirtualLitchSession& s, const char* key, const char* value) {
    modules::ControlMutationOutcome out;
    return s.MutateSettingJson(1U, key, value, out) == VIRTUAL_LITCH_OK &&
        (out.status == modules::ControlMutationStatus::Applied || out.status == modules::ControlMutationStatus::Noop);
}
bool TapHint(runtime::VirtualLitchSession& s, bool black) {
    const bool swap = modules::ComputeControlBankSwap(s.BoardRotate(), s.FirstPlayerIsP2(), false);
    const auto owner = modules::MapOwnerToPhysicalHomeEdge(
        black ? modules::UiOwner::P2 : modules::UiOwner::P1, swap);
    const auto physical = owner == modules::UiOwner::P2 ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    return s.SetButtonDown(physical, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
        s.Advance(20U) == VIRTUAL_LITCH_OK &&
        s.SetButtonDown(physical, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
        s.Advance(20U) == VIRTUAL_LITCH_OK;
}
bool At(runtime::VirtualLitchSession& s, uint32_t now) {
    return now >= s.State().now_ms && s.Advance(now - s.State().now_ms) == VIRTUAL_LITCH_OK;
}
virtual_litch_color Square(const runtime::VirtualLitchSession& s, uint8_t square) {
    return s.SemanticFrame().board[square][0];
}
virtual_litch_color PhysicalSquare(const runtime::VirtualLitchSession& s, uint8_t square) {
    return s.PhysicalFrame().pixels[4U * runtime::MapSquareIndex(square, Rotate(s))];
}
virtual_litch_color RemovalSide(const runtime::VirtualLitchSession& s,uint8_t victim,hardware::SquareLedSlot slot){
    const auto revision=s.PhysicalFrame().led_count==300U?hardware::BoardRevision::V3_3S3:hardware::BoardRevision::V3S3;
    const auto targets=hardware::MapSemanticSquareLedTargets(revision,victim,slot,Rotate(s));
    return s.PhysicalFrame().pixels[targets.indices[0U]];
}
bool RemovalCue(const runtime::VirtualLitchSession& s,uint8_t victim){
    const auto left=s.SemanticFrame().board[victim][0U],right=s.SemanticFrame().board[victim][2U];
    const bool palette = Eq(left, {255U, 101U, 0U, 0U}) ||
        Eq(left, {64U, 25U, 0U, 0U}) || Eq(left, {255U, 85U, 0U, 0U});
    if (!palette || left.b != 0U || left.w != 0U || !Eq(left,right) ||
        !Eq(s.SemanticFrame().board[victim][1U],{})) return false;
    const auto revision=s.PhysicalFrame().led_count==300U?hardware::BoardRevision::V3_3S3:hardware::BoardRevision::V3S3;
    const auto pieces=hardware::MapSemanticSquareLedTargets(revision,victim,hardware::SquareLedSlot::Piece,Rotate(s));
    for(uint8_t i=0U;i<pieces.count;++i)if(!Eq(s.PhysicalFrame().pixels[pieces.indices[i]],{}))return false;
    for(const auto slot:{hardware::SquareLedSlot::Square0,hardware::SquareLedSlot::Square2}){
        const auto semantic=s.SemanticFrame().board[victim][static_cast<uint8_t>(slot)];
        const auto mapped=RemovalSide(s,victim,slot);
        if (mapped.r == 0U || mapped.b != 0U || mapped.w != 0U || mapped.g > mapped.r) return false;

    }
    return true;
}
void Checkpoint(const runtime::VirtualLitchSession& s, const char* run,
                const char* stage, uint8_t victim, const char* next) {
    const auto square = Square(s, victim), piece = s.SemanticFrame().board[victim][1];
    const auto physical = PhysicalSquare(s, victim);
    const auto display = s.DisplayText();
    const auto home = s.HomeStateLine();
    bool fixtureMatched = false;
    unsigned fixtureIndex = 0U;
    for (const auto checkpoint : homeFixture["checkpoints"].as<JsonArrayConst>()) {
        if (std::strcmp(checkpoint["run"] | "", run) == 0 &&
            std::strcmp(checkpoint["stage"] | "", stage) == 0) {
            fixtureMatched = std::strcmp(checkpoint["home"] | "", home.CStr()) == 0;
            homeFixtureSeen |= UINT64_C(1) << fixtureIndex;
            break;
        }
        ++fixtureIndex;
    }
    Check(fixtureMatched, run, "actual producer Home bytes match the shared app replay checkpoint");
    std::printf("[HINT_CAPTURE_HOME] run=%s stage=%s %s\n", run, stage, home.CStr());
    std::printf("[HINT_CAPTURE] run=%s stage=%s t=%u phase=%u action=%u occupied=%016llx "
                "hint=%u correction=%016llx illegal=%016llx victim=%u square=%u,%u,%u piece=%u,%u,%u physical=%u,%u,%u,%u "
                "display=\"%.*s\" next=%s\n", run, stage, s.State().now_ms,
                s.State().interaction_phase, s.State().required_action,
                static_cast<unsigned long long>(s.State().physical_occupied_mask),
                s.HintDisplayRequested(), static_cast<unsigned long long>(s.State().correction_mask),
                static_cast<unsigned long long>(s.State().human_illegal_mask), victim, square.r, square.g, square.b,
                piece.r, piece.g, piece.b, physical.r, physical.g, physical.b, physical.w,
                static_cast<int>(display.size()), display.data(), next);
}
void CheckHeldProjection(const runtime::VirtualLitchSession& s, const char* run,
                         uint8_t from, uint8_t to, bool matchingHint) {
    StaticJsonDocument<2048> home;
    const auto wire = s.HomeStateLine();
    Check(!deserializeJson(home, wire.CStr()), run, "actual held Home snapshot parses");
    char expected[24]{};
    std::snprintf(expected, sizeof(expected), "1l%c%c%016llX",
                  'a' + from % 8U, '8' - from / 8U,
                  static_cast<unsigned long long>(s.State().valid_destination_mask));
    if (matchingHint) {
        expected[20] = static_cast<char>('a' + to % 8U);
        expected[21] = static_cast<char>('8' - to / 8U);
    }
    Check(std::strcmp(home["a"] | "", "lift") == 0 &&
              std::strcmp(home["as"] | "", expected) == 0,
          run, matchingHint ? "actual held Home preserves every legal target plus matching Hint target" :
                              "other held piece has its legal mask without the unrelated Hint target");
}
struct Position {
    const char* name;
    const char* fen;
    uint64_t occupied;
    uint8_t from, to, victim;
    bool black, swapped;
};
bool Prepare(runtime::VirtualLitchSession& s, const Position& p) {
    return s.Reset() == VIRTUAL_LITCH_OK && Observe(s, UINT64_C(0xFFFF00000000FFFF)) &&
        s.Advance(200U) == VIRTUAL_LITCH_OK && s.LoadExternalFen(p.fen) &&
        Observe(s, p.occupied) && s.Advance(200U) == VIRTUAL_LITCH_OK;
}
bool WaitCpuPlan(runtime::VirtualLitchSession& s, const Position& p) {
    bool ready = false;
    for (unsigned i = 0U; i < 80U; ++i) {
        StaticJsonDocument<2048> home;
        if (!deserializeJson(home, s.HomeStateLine().CStr()) && std::strcmp(home["a"] | "", "cpu") == 0) {
            uint8_t cpuFrom = 255U, cpuTo = 255U, cpuVictim = 255U;
            ready = litch_max::TryParseUciSquare(home["c"][0U] | "", cpuFrom) &&
                litch_max::TryParseUciSquare(home["c"][1U] | "", cpuTo) &&
                litch_max::TryParseUciSquare(home["c"][2U] | "", cpuVictim) &&
                cpuFrom == p.from && cpuTo == p.to && cpuVictim == p.victim;
            break;
        }
        s.Advance(100U);
    }
    return ready;
}
void Journey(const Position& p) {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = p.black ? VIRTUAL_LITCH_HARDWARE_V3 : VIRTUAL_LITCH_HARDWARE_V3_3;
    config.first_player_is_p2 = p.swapped;
    const uint16_t currentBudget = modules::SelectS3LedPowerEnvelope(false).maxLedCurrentMa;
    test_support::BoundedHintSession humanFixture(config), cpuFixture(config);
    auto& human = humanFixture.session; auto& cpu = cpuFixture.session;
    Check(Prepare(human, p) && Prepare(cpu, p), p.name, "external FEN plus mapped Hall occupancy enters real play");
    const auto ordinaryBoard=human.SemanticFrame();
    const auto ordinaryPhysical=human.PhysicalFrame();
    Check(Setting(cpu, "VS_CPU_PLAYER", p.black ? "2" : "1") && Setting(cpu, "VS_CPU_LEVEL", "3"),
          p.name, "production CPU policy enabled for the same side and position");
    const bool cpuReady = WaitCpuPlan(cpu, p);
    Check(cpuReady, p.name, "production CPU search supplies the comparison capture prompt");
    Check(TapHint(human, p.black), p.name, "physical O tap reveals the completed production Hint");
    uint8_t from = 255U, to = 255U;
    Check(human.TryGetHintMoveSquares(from, to) && from == p.from && to == p.to,
          p.name, "real bounded search selects the legal capture used by the physical journey");
    if (from != p.from || to != p.to || !cpuReady) return;

    const auto firstHuman = Square(human, p.victim);
    const auto firstCpu = Square(cpu, p.victim);
    Check(Eq(firstHuman, {255U, 101U, 0U, 0U}) &&
              Eq(firstCpu, {255U, 101U, 0U, 0U}),
          p.name, "the first shown Hint and CPU removal cues begin with an amber strike");
    const uint32_t humanStart = human.State().now_ms;
    const uint32_t cpuStart = cpu.State().now_ms;
    std::array<virtual_litch_color, 2> cpuColors{};
    bool varied = false, matches = true, victimDark = true, physicalMatches = true;
    bool floor = false, wasStrike = false;
    unsigned strikeStarts = 0U;
    uint8_t normalPeak = 0U;
    for (unsigned i = 0U; i < 25U; ++i) {
        At(human, humanStart + i * 50U); At(cpu, cpuStart + i * 50U);
        const auto expected = Square(cpu, p.victim);
        const auto left=human.SemanticFrame().board[p.victim][0U],right=human.SemanticFrame().board[p.victim][2U];
        const bool strike = Eq(left, {255U, 101U, 0U, 0U});
        if (strike && !wasStrike) ++strikeStarts;
        wasStrike = strike;
        floor = floor || Eq(left, {64U, 25U, 0U, 0U});
        if (strike) normalPeak = std::max(normalPeak, RemovalSide(
            human, p.victim, hardware::SquareLedSlot::Square0).r);
        if (i == 0U) cpuColors[0] = expected;
        if (!Eq(expected, cpuColors[0])) { cpuColors[1] = expected; varied = true; }
        matches = matches && Eq(Square(human, p.victim), expected);
        physicalMatches = physicalMatches && Eq(RemovalSide(human,p.victim,hardware::SquareLedSlot::Square0),RemovalSide(cpu,p.victim,hardware::SquareLedSlot::Square0)) &&
            Eq(RemovalSide(human,p.victim,hardware::SquareLedSlot::Square2),RemovalSide(cpu,p.victim,hardware::SquareLedSlot::Square2));
        victimDark = victimDark && Eq(human.SemanticFrame().board[p.victim][1], {}) &&
            Eq(cpu.SemanticFrame().board[p.victim][1], {});
        for (const auto* board : {&human, &cpu}) {
            const auto revision = board->PhysicalFrame().led_count == 300U
                ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3;
            const auto targets = hardware::MapSemanticSquareLedTargets(revision, p.victim,
                hardware::SquareLedSlot::Piece, Rotate(*board));
            for (uint8_t target = 0U; target < targets.count; ++target)
                victimDark = victimDark && Eq(board->PhysicalFrame().pixels[targets.indices[target]], {});
            Check(RemovalCue(*board,p.victim),p.name,
                  "both actual square emitters carry synchronized warm flashes and visible floor, with both Piece LEDs zero");
            Check(board->PhysicalFrame().applied_led_current_ma <= currentBudget,
                  p.name, "capture palette remains inside the configured physical current budget");

        }
        if (!p.black) for(const uint8_t neighbor:{35U,37U}){
            bool ordinary=true;
            for(uint8_t slot=0U;slot<3U;++slot){
                ordinary=ordinary && Eq(human.SemanticFrame().board[neighbor][slot],ordinaryBoard.board[neighbor][slot]);
                const auto targets=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,neighbor,
                    static_cast<hardware::SquareLedSlot>(slot),Rotate(human));
                for(uint8_t target=0U;target<targets.count;++target)
                    ordinary=ordinary && Eq(human.PhysicalFrame().pixels[targets.indices[target]],ordinaryPhysical.pixels[targets.indices[target]]);
            }
            Check(ordinary,p.name,"every nonowned neighbor rail and BOTH Piece LEDs remain byte-identical to ordinary output");
            if(i==0U || i==12U)std::printf("[CAPTURE_LOCAL_NEIGHBOR] t=%u square=%u ordinary=%u semantic_left=%u,%u,%u semantic_right=%u,%u,%u\n",
                human.State().now_ms,neighbor,ordinary,human.SemanticFrame().board[neighbor][0U].r,
                human.SemanticFrame().board[neighbor][0U].g,human.SemanticFrame().board[neighbor][0U].b,
                human.SemanticFrame().board[neighbor][2U].r,human.SemanticFrame().board[neighbor][2U].g,human.SemanticFrame().board[neighbor][2U].b);
        }
        if (!p.black) {
            const auto l = RemovalSide(human,p.victim,hardware::SquareLedSlot::Square0);
            const auto r = RemovalSide(human,p.victim,hardware::SquareLedSlot::Square2);
            std::printf("[CAPTURE_PALETTE] age=%u clock_mod1200=%u semantic=%u,%u,%u physicalL=%u,%u,%u,%u physicalR=%u,%u,%u,%u current=%u budget=%u\n",
                i*50U, human.State().now_ms%1200U, left.r,left.g,left.b,
                l.r,l.g,l.b,l.w,r.r,r.g,r.b,r.w,human.PhysicalFrame().applied_led_current_ma,currentBudget);
        }
        if (i == 0U || i == 6U) {
            Checkpoint(cpu, p.name, "CPU-reference", p.victim, "remove-victim");
            Checkpoint(human, p.name, "Hint-visible", p.victim, "remove-victim-or-lift-mover");
        }
    }
    if (!p.black) {
        auto dimConfig = config;
        dimConfig.brightness_scale_256 = modules::brightness_scale_policy::NEAR_DARK_SCALE256;
        test_support::BoundedHintSession dimFixture(dimConfig);
        auto& dim = dimFixture.session;
        Check(Prepare(dim,p) && TapHint(dim,p.black) && dim.Advance(600U)==VIRTUAL_LITCH_OK,
              p.name,"actual Hint scene uses production near-dark output scale");
        uint8_t lowPeak = 0U, lowFloor = 255U;
        for (unsigned sample=0U; sample<24U; ++sample) {
            dim.Advance(50U);
            Check(RemovalCue(dim,p.victim),p.name,"warm floor survives physical near-dark quantization");
            const auto pixel=RemovalSide(dim,p.victim,hardware::SquareLedSlot::Square0);
            lowPeak = std::max(lowPeak, pixel.r);
            lowFloor = std::min(lowFloor, pixel.r);
            Check(dim.PhysicalFrame().applied_brightness_scale_65535<=2048U &&
                  dim.PhysicalFrame().applied_led_current_ma<=currentBudget,p.name,
                  "near-dark capture is actually dimmed and obeys current budget");
            std::printf("[CAPTURE_LOW] clock_mod1200=%u square=%u,%u,%u,%u scale=%u current=%u budget=%u\n",
                dim.State().now_ms%1200U,pixel.r,pixel.g,pixel.b,pixel.w,
                dim.PhysicalFrame().applied_brightness_scale_65535,
                dim.PhysicalFrame().applied_led_current_ma,currentBudget);
        }
        Check(Setting(dim,"MINIMAL_ANIMATIONS","true"),p.name,"Minimal capture mode applies");
        dim.Advance(0U);
        const auto minimal=Square(dim,p.victim);
        const auto lowOrdinary = RemovalSide(dim,p.victim,hardware::SquareLedSlot::Square0);
        Check(lowPeak > lowOrdinary.r && lowOrdinary.r > lowFloor && lowFloor > 0U,
              p.name, "near-dark strike exceeds the steady ordinary amber reference and retains a nonzero rest");
        Check(Eq(minimal, {255U,85U,0U,0U}), p.name,
              "Minimal uses the steady orange semantic capture color");
        for(unsigned sample=0U;sample<6U;++sample){
            dim.Advance(200U);
            Check(Eq(Square(dim,p.victim),minimal)&&RemovalCue(dim,p.victim) &&
                      Eq(RemovalSide(dim,p.victim,hardware::SquareLedSlot::Square0),lowOrdinary),p.name,
                  "Minimal keeps actual occupied victim rails steady and Piece LEDs off");
        }

        auto headroomConfig = config;
        headroomConfig.brightness_scale_256 = 128U;
        test_support::BoundedHintSession headroomFixture(headroomConfig), referenceFixture(headroomConfig);
        auto& headroom = headroomFixture.session;
        auto& reference = referenceFixture.session;
        Check(Prepare(headroom,p) && TapHint(headroom,p.black) &&
                  Prepare(reference,p) && Setting(reference,"MINIMAL_ANIMATIONS","true") &&
                  TapHint(reference,p.black), p.name,
              "normal-output strike and steady amber reference follow real Hint with headroom");
        uint8_t headroomPeak = 0U;
        for (unsigned sample = 0U; sample < 25U; ++sample) {
            if (Eq(Square(headroom,p.victim), {255U,101U,0U,0U}))
                headroomPeak = std::max(headroomPeak,
                    RemovalSide(headroom,p.victim,hardware::SquareLedSlot::Square0).r);
            headroom.Advance(50U);
        }
        const auto normalOrdinary = RemovalSide(reference,p.victim,hardware::SquareLedSlot::Square0);
        Check(headroomPeak > normalOrdinary.r && normalOrdinary.r > 0U, p.name,
              "actual normal-output strike is brighter than ordinary steady amber when budget permits");
        std::printf("[CAPTURE_ACCENT] full_peak=%u normal_peak=%u normal_reference=%u low_peak=%u low_reference=%u low_floor=%u\n",
                    normalPeak, headroomPeak, normalOrdinary.r, lowPeak, lowOrdinary.r, lowFloor);

        auto cappedConfig = config;
        cappedConfig.max_led_current_ma = 120U;
        test_support::BoundedHintSession cappedFixture(cappedConfig);
        auto& capped = cappedFixture.session;
        Check(Prepare(capped,p) && TapHint(capped,p.black), p.name, "hard-capped Hint reaches capture guidance");
        bool observedStrike = false, powerLimited = false;
        uint16_t maxCappedCurrent = 0U;
        for (unsigned sample = 0U; sample < 25U; ++sample) {
            const auto semantic = Square(capped,p.victim);
            observedStrike = observedStrike || Eq(semantic, {255U,101U,0U,0U});
            powerLimited = powerLimited || capped.PhysicalFrame().power_limited != 0U;
            maxCappedCurrent = std::max(maxCappedCurrent, capped.PhysicalFrame().applied_led_current_ma);
            Check(capped.PhysicalFrame().applied_led_current_ma <= cappedConfig.max_led_current_ma,
                  p.name, "capture accent never exceeds the hard LED current cap");
            capped.Advance(50U);
        }
        Check(observedStrike && powerLimited, p.name,
              "hard-capped observation includes an actual strike while limiting is active");
        std::printf("[CAPTURE_HARD_CAP] maximum=%u limit=%u limited=%u\n",
                    maxCappedCurrent, cappedConfig.max_led_current_ma, powerLimited);
    }

    const auto removal = cpuColors[0].r > cpuColors[1].r ? cpuColors[0] : cpuColors[1];
    Check(varied && matches && strikeStarts >= 2U && floor, p.name,
          "Hint and CPU share two amber strikes followed by the sustained dim rest");
    Check(physicalMatches, p.name, "V3 physical mapping matches CPU removal feedback after end/orientation mapping");
    Check(victimDark, p.name, "BOTH actual victim Piece emitters stay zero throughout suggested and mandatory removal");
    uint8_t replayFrom = 64U, replayTo = 64U;
    Check(TapHint(human, p.black) && !human.HintDisplayRequested(),
          p.name, "second O tap hides manual capture guidance with Coaching disabled");
    Check(TapHint(human, p.black) && human.HintDisplayRequested() &&
          human.TryGetHintMoveSquares(replayFrom, replayTo) && replayFrom == p.from && replayTo == p.to,
          p.name, "third tap reveals the same current-position capture suggestion");
    human.Advance(300U);
    Check(RemovalCue(human, p.victim), p.name,
          "replayed Hint retains victim-removal guidance and keeps the actual victim Piece emitters dark");
    if (!p.black) for (const uint8_t neighbor : {35U,37U})
        Check(Eq(human.SemanticFrame().board[neighbor][0U],ordinaryBoard.board[neighbor][0U]) &&
            Eq(human.SemanticFrame().board[neighbor][2U],ordinaryBoard.board[neighbor][2U]),p.name,
              "replaying Hint keeps every nonowned neighbor rail at its ordinary color");
    Checkpoint(human, p.name, "Hint-replayed", p.victim, "remove-victim-or-lift-mover");

    const uint8_t other = p.black ? 4U : 62U;
    Check(Observe(human, p.occupied & ~(1ULL << other)), p.name, "different piece pickup keeps ordinary legal guidance");
    bool wrongPieceRemoval = false;
    for (unsigned i = 0U; i < 7U; ++i) {
        wrongPieceRemoval = wrongPieceRemoval || RemovalCue(human,p.victim);
        human.Advance(100U);
    }
    Check(!wrongPieceRemoval, p.name, "capture Hint cannot demand victim removal for a different held piece");
    const auto beforeHomeRead = human.State();
    {
        // A standalone executable's ambient clock can precede this simulated
        // king lift. The existing Home read must use its session's clock,
        // rather than wrapping the resignation timer and hiding legal moves.
        fxe::ScopedMillisProviderOverride ambient(+[]() -> uint32_t { return 1U; });
        CheckHeldProjection(human, p.name, other, p.to, false);
        const auto snapshot = human.HomeStateSnapshot();
        const auto line = human.HomeStateLine();
        Check(snapshot.assistance.presentation == modules::HomeAssistancePresentation::LegalGuide &&
                  snapshot.assistance.legalSourceSquare == other &&
                  snapshot.assistance.legalDestinationMask == beforeHomeRead.valid_destination_mask &&
                  beforeHomeRead.valid_destination_mask != 0ULL,
              p.name, "earlier ambient clock cannot arm resignation or hide the actual held king's legal guide");
        Check(fxe::Millis() == 1U &&
                  human.State().now_ms == beforeHomeRead.now_ms &&
                  human.State().frame_sequence == beforeHomeRead.frame_sequence &&
                  human.State().selected_square == beforeHomeRead.selected_square &&
                  human.State().valid_destination_mask == beforeHomeRead.valid_destination_mask,
              p.name, "Home reads restore the ambient provider without advancing session time or interaction");
        std::printf("[HOME_CLOCK_JOURNEY] run=%s ambient_ms=%u virtual_ms=%u selected=%u "
                    "legal=%016llx home=%s next=choose-legal-target-or-return-king\n",
                    p.name, fxe::Millis(), human.State().now_ms, human.State().selected_square,
                    static_cast<unsigned long long>(human.State().valid_destination_mask), line.CStr());
    }
    Checkpoint(human, p.name, "other-piece-held", p.victim, "choose-legal-target-or-return-piece");
    Check(Observe(human, p.occupied), p.name, "returning other piece restores unchanged-position guidance");
    Checkpoint(human, p.name, "other-pickup-returned", p.victim, "follow-hint-or-choose-another-move");

    if (!p.black) {
        constexpr uint8_t illegalTarget = 37U; // f4: the e2 rook cannot move diagonally.
        const uint64_t lifted = p.occupied & ~(1ULL << p.from);
        char unchangedFen[128]{};
        Check(Observe(human, lifted) &&
                  (human.State().valid_destination_mask & (1ULL << illegalTarget)) == 0ULL &&
                  Observe(human, lifted | (1ULL << illegalTarget)),
              p.name, "hinted rook placed on illegal f4 through real Hall input");
        // Illegal placement is the existing local correction owner while the
        // source remains selected; it need not change the reducer phase.
        Check(human.State().human_illegal_mask == (1ULL << illegalTarget) &&
                  human.State().selected_square == p.from &&
                  (human.State().physical_occupied_mask ^ human.State().expected_occupied_mask) ==
                      ((1ULL << p.from) | (1ULL << illegalTarget)),
              p.name, "local correction identifies extra f4 and retains the empty e2 source");
        bool removalDuringCorrection = false;
        for (unsigned i = 0U; i < 13U; ++i) {
            removalDuringCorrection = removalDuringCorrection || RemovalCue(human,p.victim);
            human.Advance(50U);
        }
        Check(!removalDuringCorrection && human.UndoHistoryMoveCount() == 0U &&
                  humanFixture.engine.GetFen(unchangedFen, sizeof(unchangedFen)) &&
                  std::strcmp(unchangedFen, p.fen) == 0,
              p.name, "full correction cadence suppresses victim cue without changing FEN or committing");
        const auto illegalCue = Square(human, illegalTarget);
        Check(illegalCue.r > illegalCue.g && illegalCue.g > 0U && illegalCue.b == 0U,
              p.name, "illegal f4 has the existing gentle amber correction cue");
        Checkpoint(human, p.name, "illegal-f4-correction", p.victim, "remove-f4-and-restore-e2");
        Checkpoint(human, p.name, "illegal-square-cue", illegalTarget, "remove-f4");
        Check(Observe(human, lifted) && human.State().human_illegal_mask == 0ULL &&
                  human.State().selected_square == p.from,
              p.name, "removing f4 clears its correction and returns to the original held source");
        Checkpoint(human, p.name, "illegal-piece-relifted", p.from, "return-rook-to-e2");
        Check(Observe(human, p.occupied) && human.State().human_illegal_mask == 0ULL &&
                  human.State().correction_mask == 0ULL &&
                  human.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
                  human.UndoHistoryMoveCount() == 0U && human.TryGetHintMoveSquares(from, to) &&
                  from == p.from && to == p.to && human.HintDisplayRequested() &&
                  humanFixture.engine.GetFen(unchangedFen, sizeof(unchangedFen)) &&
                  std::strcmp(unchangedFen, p.fen) == 0,
              p.name, "restoring e2 recovers the original visible capture Hint without a commit");
        bool restoredRed = true, restoredVaried = false;
        const auto restoredStart = Square(human,p.victim);
        for (unsigned i = 0U; i < 25U; ++i) {
            const auto color = Square(human, p.victim);
            restoredRed = restoredRed && RemovalCue(human,p.victim);
            restoredVaried = restoredVaried || !Eq(color,restoredStart);
            human.Advance(50U);
        }
        Check(restoredRed && restoredVaried,p.name,"recovery resumes the CPU-equivalent warm capture rhythm");
        Checkpoint(human, p.name, "illegal-placement-restored", p.victim, "follow-original-capture-Hint");
    }

    uint64_t occupied = p.occupied & ~(1ULL << p.from);
    Check(Observe(human, occupied), p.name, "source-first pickup follows the real human interaction reducer");
    Check((human.State().valid_destination_mask & (1ULL << p.to)) != 0ULL,
          p.name, "Hint capture remains a legal destination while source is held");
    bool heldVaried = false, heldMatches = true;
    const auto heldInitial = Square(human, p.victim);
    for (unsigned i = 0U; i < 13U; ++i) {
        human.Advance(100U);
        const auto color = Square(human, p.victim);
        heldVaried = heldVaried || !Eq(heldInitial, color);
        heldMatches = heldMatches && RemovalCue(human,p.victim);
    }
    Check(heldVaried && heldMatches, p.name, "matching held Hint retains the warm capture flashes and floor");
    CheckHeldProjection(human, p.name, p.from, p.to, true);
    Checkpoint(human, p.name, "mover-held", p.victim, "remove-captured-piece");
    if (p.to != p.victim) {
        occupied |= 1ULL << p.to;
        Check(Observe(human, occupied) && human.Advance(2500U) == VIRTUAL_LITCH_OK &&
                  human.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::RemoveCapture),
              p.name, "placed en-passant mover still requires actual pawn removal past settle deadline");
        bool pendingRed = false;
        for (unsigned i = 0U; i < 7U; ++i) {
            pendingRed = pendingRed || RemovalCue(human,p.victim);
            human.Advance(100U);
        }
        Check(pendingRed, p.name, "existing en-passant owner keeps removal cue beyond confirmation deadline");
        StaticJsonDocument<2048> pendingHome;
        const auto pendingWire = human.HomeStateLine();
        uint8_t captureSquare = 255U;
        Check(!deserializeJson(pendingHome, pendingWire.CStr()) &&
                  std::strcmp(pendingHome["a"] | "", "settle") == 0 &&
                  litch_max::TryParseUciSquare(pendingHome["c"][2U] | "", captureSquare) &&
                  captureSquare == p.victim && std::strcmp(pendingHome["as"] | "", "1x") == 0,
              p.name, "actual pending Home exposes mandatory pawn removal without stale Hint authority");
        Checkpoint(human, p.name, "en-passant-pawn-pending", p.victim, "remove-off-destination-pawn");
    }
    occupied &= ~(1ULL << p.victim);
    Check(Observe(human, occupied), p.name, "captured piece removal is acknowledged from Hall input");
    const auto emptyPiece = human.SemanticFrame().board[p.victim][1];
    Check(p.to == p.victim
              ? emptyPiece.r != 0U || emptyPiece.g != 0U || emptyPiece.b != 0U || emptyPiece.w != 0U
              : emptyPiece.r == 0U && emptyPiece.g == 0U && emptyPiece.b == 0U && emptyPiece.w == 0U,
          p.name, "removed target becomes the explicit landing cue; off-target en-passant victim stays dark");
    Check(!Eq(Square(human, p.victim), removal), p.name, "victim removal immediately retires the colored capture cue");
    if (p.to == p.victim) {
        Check(!Eq(Square(human, p.to), removal) &&
                  (PhysicalSquare(human, p.to).r != 0U ||
                   PhysicalSquare(human, p.to).g != 0U ||
                   PhysicalSquare(human, p.to).b != 0U),
              p.name, "V3.3 removal hands the now-empty destination to the strong landing socket");
        const auto target = hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,
            p.to, hardware::SquareLedSlot::Piece, Rotate(human));
        bool brighterTop = false, brighterBottom = false;
        for (uint8_t sample = 0U; sample <= 24U; ++sample) {
            const auto top = human.PhysicalFrame().pixels[target.indices[0U]];
            const auto bottom = human.PhysicalFrame().pixels[target.indices[1U]];
            const auto energy = [](virtual_litch_color c) { return c.r + c.g + c.b + c.w; };
            Check(energy(top) > 0U && energy(bottom) > 0U, p.name,
                  "cleared capture landing keeps BOTH physical piece emitters above a visible floor");
            brighterTop = brighterTop || energy(top) > energy(bottom);
            brighterBottom = brighterBottom || energy(bottom) > energy(top);
            if (sample != 24U) human.Advance(50U);
        }
        Check(brighterTop && brighterBottom, p.name, "cleared victim landing alternates its actual piece pair");
    }
    Checkpoint(human, p.name, "victim-removed", p.victim, p.to == p.victim ? "place-mover-on-target" : "finish-move");
    if (p.to == p.victim) {
        occupied |= 1ULL << p.to;
        Check(Observe(human, occupied), p.name, "ordinary capture mover placed on cleared destination");
    }
    for (unsigned sample = 0U; sample < 21U; ++sample) {
        Check(!RemovalCue(human, p.victim), p.name,
              "seated Hint capturer never reacquires victim removal during settling");
        if (sample == 0U || sample == 10U || sample == 20U) {
            const auto& slots = human.SemanticFrame().board[p.to];
            std::printf("[CAPTURE_SETTLE] run=%s age=%u phase=%u action=%u square=%u,%u,%u piece=%u,%u,%u\n",
                p.name, sample * 100U, human.State().interaction_phase, human.State().required_action,
                slots[0].r, slots[0].g, slots[0].b, slots[1].r, slots[1].g, slots[1].b);
        }
        if (sample != 20U) human.Advance(100U);
    }
    Check(human.Advance(1000U) == VIRTUAL_LITCH_OK && human.UndoHistoryMoveCount() == 1U &&
              human.State().expected_occupied_mask == occupied && human.State().correction_mask == 0ULL,
          p.name, "capture commits once and acknowledgement expires without correction");
    // Last-move/acknowledgement animations may continue; only the removal cue
    // must retire. Do not require the whole square to become static.
    bool staleRemoval = false;
    for (unsigned i = 0U; i < 13U; ++i) {
        staleRemoval = staleRemoval || RemovalCue(human,p.victim);
        human.Advance(50U);
    }
    Check(!staleRemoval, p.name, "completed move has no stale removal blink through a full cadence");
    Checkpoint(human, p.name, "complete-after-expiry", p.victim, "next-player-move");

    // These two fixtures share mcu-max's legacy rules position. The comparison
    // above observes one common FEN; physical execution must restore the CPU
    // fixture after the human fixture has advanced that shared position.
    Check(Prepare(cpu, p) && Setting(cpu, "VS_CPU_PLAYER", p.black ? "2" : "1") &&
              Setting(cpu, "VS_CPU_LEVEL", "3") && WaitCpuPlan(cpu, p),
          p.name, "original CPU capture is requeued before its independent physical execution");
    // Finish the CPU plan through the same Hall seam.
    // A newly seated capturer must never inherit its removed victim's capture cue.
    uint64_t cpuOccupied = p.occupied & ~(1ULL << p.victim);
    Check(Observe(cpu, cpuOccupied), p.name, "CPU victim-first removal uses the real directed plan");
    if (p.to == p.victim) {
        const auto socket = PhysicalSquare(cpu, p.to);
        Check(socket.r != 0U || socket.g != 0U || socket.b != 0U,
              p.name, "CPU removal exposes the same strong V3.3 empty destination");
    }
    cpuOccupied &= ~(1ULL << p.from);
    Check(Observe(cpu, cpuOccupied), p.name, "CPU mover pickup follows victim removal");
    cpuOccupied |= 1ULL << p.to;
    Check(Observe(cpu, cpuOccupied), p.name, "CPU capturer seats on its legal destination");
    bool cpuStaleRemoval = false;
    for (unsigned i = 0U; i < 13U; ++i) {
        cpuStaleRemoval = cpuStaleRemoval || RemovalCue(cpu,p.victim);
        cpu.Advance(50U);
    }
    Check(!cpuStaleRemoval, p.name, "CPU removal cue retires when victim is gone across seating and acknowledgement");
    Check(cpu.Advance(3000U) == VIRTUAL_LITCH_OK && cpu.UndoHistoryMoveCount() == 1U &&
              cpu.State().expected_occupied_mask == cpuOccupied && cpu.State().correction_mask == 0ULL,
          p.name, "CPU capture commits once with no correction after acknowledgement expiry");
    const auto cpuSeated = Square(cpu, p.to);
    const auto cpuPiece = cpu.SemanticFrame().board[p.to][1];
    std::printf("[CPU_CAPTURE_COMPLETED] run=%s t=%u phase=%u action=%u occupied=%016llx "
                "destination=%u square=%u,%u,%u piece=%u,%u,%u red_retired=%u next=next-player-move\n",
                p.name, cpu.State().now_ms, cpu.State().interaction_phase, cpu.State().required_action,
                static_cast<unsigned long long>(cpu.State().physical_occupied_mask), p.to,
                cpuSeated.r, cpuSeated.g, cpuSeated.b, cpuPiece.r, cpuPiece.g, cpuPiece.b,
                cpuStaleRemoval ? 0U : 1U);
}
}
int main() {
    std::ifstream fixtureFile("test/fixtures/hint_capture_home_journey.json");
    Check(fixtureFile.good() && !deserializeJson(homeFixture, fixtureFile),
          "HomeProducer", "shared app replay fixture loads");
    Journey({"HintCaptureMatchesCpu/ordinary-white", "4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1",
        (1ULL << 4U) | (1ULL << 36U) | (1ULL << 52U) | (1ULL << 62U), 52U, 36U, 36U, false, false});
    // The bishop supports d3 through the newly vacated e4 square. En passant
    // wins White's only pawn and leaves a protected passed pawn, rather than
    // asking the search to prefer the old undefended d3 pawn over a King move.
    Journey({"HintCaptureMatchesCpu/en-passant-black-swapped", "4k3/8/6b1/8/3Pp3/8/8/4K3 b - d3 0 1",
        (1ULL << 4U) | (1ULL << 22U) | (1ULL << 35U) | (1ULL << 36U) | (1ULL << 60U),
        36U, 43U, 35U, true, true});
    const unsigned checkpointCount = homeFixture["checkpoints"].size();
    Check(checkpointCount > 0U && checkpointCount < 64U &&
              homeFixtureSeen == ((UINT64_C(1) << checkpointCount) - 1ULL),
          "HomeProducer", "every app replay checkpoint came from this production journey");
    std::printf("[HINT_CAPTURE_VIRTUAL_LITCH] failures=%d\n", failures);
    return failures ? 1 : 0;
}
