#include "bounded_hint_engine.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/orientation.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "runtime/chess_interaction_model.hpp"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint64_t LIFTED = OPENING & ~(UINT64_C(1) << 52U);
constexpr uint64_t PLACED = LIFTED | (UINT64_C(1) << 36U);
constexpr uint64_t BLACK_LIFTED = PLACED & ~(UINT64_C(1) << 11U);
constexpr uint64_t BLACK_PLACED = BLACK_LIFTED | (UINT64_C(1) << 27U);
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); }
}
bool Same(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
bool Set(runtime::VirtualLitchSession& b, unsigned id, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return b.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
}
virtual_litch_config Config() {
    virtual_litch_config c{}; virtual_litch_default_config(&c);
    c.hardware = VIRTUAL_LITCH_HARDWARE_V3_3; return c;
}
void Ready(runtime::VirtualLitchSession& b) {
    Check(b.Reset() == VIRTUAL_LITCH_OK && b.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK &&
          b.Advance(3200U) == VIRTUAL_LITCH_OK, "ready V3.3 production session");
}
void Circle(runtime::VirtualLitchSession& b, uint8_t owner) {
    b.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::O, true); b.Advance(20U);
    b.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::O, false); b.Advance(20U);
    b.Advance(1000U); // Current contract: Circle accepts the existing 1000ms window.
}
unsigned Edge(const runtime::VirtualLitchSession& b) {
    unsigned n = 0U;
    for (unsigned i = 2U; i <= 5U; ++i) {
        const auto a = b.SemanticFrame().status[8U + i];
        const auto c = b.SemanticFrame().status[24U + i];
        n += a.r + a.g + a.b + c.r + c.g + c.b;
    }
    return n;
}
virtual_litch_color Floor(const runtime::VirtualLitchSession& b) {
    const auto targets = hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,
        36U, hardware::SquareLedSlot::Square0, false);
    return b.PhysicalFrame().pixels[targets.indices[0]];
}
void Log(const runtime::VirtualLitchSession& b, const char* journey, const char* input, const char* next) {
    const auto s = b.State(); const auto c = b.SemanticFrame().board[36U][0];
    std::printf("[ANIMATION_JOURNEY] %s t=%u input=%s phase=%s side=%u expected=%016llx actual=%016llx history=%u e4=%u/%u/%u edge=%u next=%s text=%.*s\n",
        journey, s.now_ms, input, runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(s.interaction_phase)),
        s.side_to_move_black, (unsigned long long)s.expected_occupied_mask, (unsigned long long)s.physical_occupied_mask,
        b.UndoHistoryMoveCount(), c.r, c.g, c.b, Edge(b), next, (int)b.DisplayText().size(), b.DisplayText().data());
    const auto pc = Floor(b);
    std::printf("[ANIMATION_PIXELS] %s t=%u e4=%u/%u/%u scale=%u pixels=", journey, s.now_ms,
        pc.r, pc.g, pc.b, b.PhysicalFrame().applied_brightness_scale_65535);
    for (unsigned i = 0U; i < b.PhysicalFrame().led_count; ++i) {
        const auto p = b.PhysicalFrame().pixels[i]; std::printf("%02x%02x%02x", p.r, p.g, p.b);
    }
    std::printf("\n");
}
void Turn() {
    test_support::BoundedHintSession f(Config()); auto& b = f.session; Ready(b);
    Check(Set(b, 720U, "TURN_ANIM_STYLE", "\"side_middle_band\"") &&
          Set(b, 721U, "MOVE_CONFIRM", "\"press_circle_to_confirm\""), "turn settings acknowledged");
    // Enter gameplay through physical input/acknowledgement. Fresh Setup's separate
    // presentation owner is not replaced by an injected position or synthetic frame.
    b.ObserveHallMask(LIFTED); b.ObserveHallMask(PLACED); Circle(b, 1U); b.Advance(300U);
    Check(b.State().side_to_move_black == 1U && b.UndoHistoryMoveCount() == 1U, "opening acknowledgement enters ordinary gameplay");
    const auto full = Edge(b); Log(b, "turn-handoff", "idle", "lift d7");
    b.ObserveHallMask(BLACK_LIFTED); const auto start = Edge(b);
    Log(b, "turn-handoff", "lift d7", "outgoing cue fades");
    Check(start > 0U && start == full, "pickup preserves first Turn frame instead of blanking it");
    for (unsigned t = 0U; t < 80U; t += 20U) { b.Advance(20U); Log(b, "turn-handoff", "+20", "fade out continues"); }
    const auto half = Edge(b);
    for (unsigned t = 0U; t < 160U; t += 20U) { b.Advance(20U); Log(b, "turn-handoff", "+20", "finish fade out"); }
    Check(half > 0U && half < start && Edge(b) == 0U, "outgoing Turn fades monotonically to off within240ms");
    b.ObserveHallMask(BLACK_PLACED); Circle(b, 2U);
    Log(b, "turn-handoff", "O acknowledged", "new current player fades in");
    Check(b.State().side_to_move_black == 0U && b.UndoHistoryMoveCount() == 2U, "authoritative O acknowledgement commits once");
    const auto incoming = Edge(b);
    for (unsigned t = 0U; t < 80U; t += 20U) { b.Advance(20U); Log(b, "turn-handoff", "+20", "fade in continues"); }
    const auto middle = Edge(b);
    for (unsigned t = 0U; t < 200U; t += 20U) { b.Advance(20U); Log(b, "turn-handoff", "+20", "finish fade in"); }
    Check(incoming > 0U && incoming < middle && middle < Edge(b), "current-player Turn begins fading at acknowledgement with no extra delay");
    Log(b, "turn-handoff", "fade complete", "white next move");
    // Quick cancel coalesces to the existing visible level; it cannot replay a stale side.
    const uint64_t nextLifted = BLACK_PLACED & ~(UINT64_C(1) << 51U);
    b.ObserveHallMask(nextLifted); b.Advance(60U); const auto dim = Edge(b);
    b.ObserveHallMask(BLACK_PLACED); const auto restored = Edge(b);
    Check(dim > 0U && restored == dim, "source return during fade resumes smoothly at current intensity");
    b.Advance(240U); Check(Edge(b) == full && b.UndoHistoryMoveCount() == 2U, "cancelled pickup returns current-player cue without committing");
    Log(b, "turn-handoff", "cancel +240", "white move");
}
void Away() {
    modules::EdgeLedRenderer renderer;
    modules::EdgeLedState state; state.mode = modules::EdgeUiMode::IdleTurn;
    state.activeSide = modules::UiOwner::P1; state.animationTimeExplicit = true;
    state.turnAnimStyle = static_cast<modules::TurnAnimStyle>(4U);
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> frame{};
    auto position = [&](uint32_t time) { state.nowMs = time; renderer.Render(state, frame);
        unsigned weighted = 0U, sum = 0U; for (unsigned i = 0U; i < 8U; ++i) {
            const auto p = frame[8U+i]; const unsigned value = p.r+p.g+p.b;
            weighted += i*value; sum += value; }
        return sum == 0U ? 0.0 : static_cast<double>(weighted)/sum; };
    Check(position(300U) > position(700U), "append-only away style moves away from current P1 home");
    test_support::BoundedHintSession f(Config()); auto& b=f.session; Ready(b);
    const bool accepted = Set(b, 731U, "TURN_ANIM_STYLE", "\"away_from_active_end\"");
    Check(accepted, "away style admitted through production typed setting owner");
    Log(b, "away-style", accepted ? "style accepted" : "style rejected", "place opening move");
    b.ObserveHallMask(LIFTED); b.ObserveHallMask(PLACED); b.Advance(2000U);
    Check(b.State().side_to_move_black == 1U && b.UndoHistoryMoveCount() == 1U,
          "away journey commits through ordinary timed confirmation");
    b.Advance(300U); Log(b, "away-style", "P2 current +300", "pulse away from north");
    auto actualPosition = [&]() {
        unsigned weighted = 0U, sum = 0U;
        for (unsigned i = 0U; i < 8U; ++i) {
            const auto p = b.SemanticFrame().status[8U+i];
            const unsigned value = p.r+p.g+p.b;
            Check(p.g == 0U && p.b == 0U, "away cue uses current P2 red palette");
            weighted += i*value; sum += value;
        }
        Check(sum > 0U, "away pulse has actual visible side pixels");
        return sum == 0U ? 0.0 : static_cast<double>(weighted)/sum;
    };
    const auto start = actualPosition(); b.Advance(400U);
    Log(b, "away-style", "P2 current +700", "lift black piece");
    Check(actualPosition() > start, "production P2 away pulse travels south away from its north home");
}
void MappedEntry() {
    struct Scenario { bool firstP2; bool rotated; unsigned brightness; unsigned hue; };
    constexpr Scenario scenarios[] = {{false,false,1U,0U}, {false,true,8U,85U},
        {true,false,1U,170U}, {true,true,4U,21U}};
    for (const auto scenario : scenarios) {
        auto config = Config();
        config.first_player_is_p2 = scenario.firstP2 ? 1U : 0U;
        config.board_rotate = scenario.rotated ? 1U : 0U;
        const bool swapped = modules::ComputeControlBankSwap(scenario.rotated, scenario.firstP2, false);
        config.square_rotate_for_output = swapped ? 1U : 0U;
        config.brightness_scale_256 = modules::brightness_scale_policy::ComputeEffectiveScale256(
            scenario.brightness, false, 0.0f, 256U, false);
        test_support::BoundedHintSession fixture(config); auto& b=fixture.session;
        auto raw = [&](uint64_t logical) {
            uint64_t result=0ULL;
            for (unsigned square=0U;square<64U;++square)
                if ((logical & (UINT64_C(1)<<square)) !=0ULL)
                    result |= UINT64_C(1) << (swapped ? 63U-square : square);
            return result;
        };
        Check(b.Reset()==VIRTUAL_LITCH_OK && b.ObserveHallMask(raw(OPENING))==VIRTUAL_LITCH_OK &&
            b.Advance(3200U)==VIRTUAL_LITCH_OK, "mapped entry starts through production Fresh Setup");
        char brightness[8]; char hue[8];
        std::snprintf(brightness,sizeof(brightness),"%u",scenario.brightness);
        std::snprintf(hue,sizeof(hue),"%u",scenario.hue);
        Check(Set(b,810U,"BRIGHTNESS",brightness) && Set(b,811U,"P1_END_PIECE_WHEEL",hue) &&
            Set(b,812U,"P2_END_PIECE_WHEEL",hue) && Set(b,813U,"MOVE_CONFIRM","\"press_circle_to_confirm\""),
            "mapped entry settings accepted by typed owner");
        b.Advance(300U);
        auto entry = [&](const bool black) {
            const bool physicalP2 = black != swapped;
            const unsigned home = physicalP2 ? 0U : 16U;
            unsigned minimum=UINT32_MAX, energy=0U, lit=0U;
            std::printf("[TURN_ENTRY_MAPPED] first_p2=%u rotate=%u brightness=%u hue=%u owner=%s phase=%s next=lift-%s rgbw=",
                scenario.firstP2,scenario.rotated,scenario.brightness,scenario.hue,black?"P2":"P1",
                runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(b.State().interaction_phase)),black?"d7":"d2");
            for (unsigned i=2U;i<=5U;++i) {
                const unsigned index=256U+hardware::V3_3PhysicalIndexForSemanticStatus(home+i);
                const auto pixel=b.PhysicalFrame().pixels[index];
                const unsigned value=pixel.r+pixel.g+pixel.b+pixel.w;
                if (value) ++lit; energy+=value; if(value<minimum)minimum=value;
                std::printf("%u:%u/%u/%u/%u,",index,pixel.r,pixel.g,pixel.b,pixel.w);
            }
            std::printf(" lit=%u min_energy=%u total=%u power_scale=%u\n",lit,minimum,energy,
                b.PhysicalFrame().applied_brightness_scale_65535);
            Check(b.State().side_to_move_black==(black?1U:0U) && lit==4U && minimum>0U,
                "acknowledged owner has four spatial pixels after real mapping/brightness/power quantization");
        };
        b.ObserveHallMask(raw(LIFTED)); b.ObserveHallMask(raw(PLACED)); Circle(b,swapped?2U:1U); entry(true);
        b.Advance(300U); b.ObserveHallMask(raw(BLACK_LIFTED)); b.ObserveHallMask(raw(BLACK_PLACED));
        Circle(b,swapped?1U:2U); entry(false);
    }
}

void ActiveSidePcbInsertion() {
    // Named regression for the owner's comet entering O then skipping a rim LED.
    // Explicit PCB indices exclude all twelve control emitters.
    constexpr unsigned homeIndices[2][8] = {
        {22U,23U,24U,25U,32U,33U,34U,35U},
        {0U,1U,2U,3U,10U,11U,12U,13U},
    };
    for (bool rotate : {false, true}) {
        auto config = Config(); config.board_rotate = rotate ? 1U : 0U;
        config.square_rotate_for_output = rotate ? 1U : 0U;
        test_support::BoundedHintSession fixture(config); auto& b = fixture.session;
        auto raw = [rotate](uint64_t logical) {
            uint64_t result = 0ULL;
            for (unsigned i = 0U; i < 64U; ++i)
                if (logical & (UINT64_C(1) << i))
                    result |= UINT64_C(1) << (rotate ? 63U-i : i);
            return result;
        };
        Check(b.Reset() == VIRTUAL_LITCH_OK &&
              b.ObserveHallMask(raw(OPENING)) == VIRTUAL_LITCH_OK &&
              b.Advance(3200U) == VIRTUAL_LITCH_OK, "PCB comet enters through raw opening Hall scan");
        Check(Set(b,850U,"TURN_ANIM_STYLE","\"active_end_pingpong_sides\"") &&
              Set(b,851U,"MOVE_CONFIRM","\"press_circle_to_confirm\""),
              "PCB comet style and confirmation accepted");
        for (bool black : {true, false}) {
            b.ObserveHallMask(raw(black ? LIFTED : BLACK_LIFTED));
            b.ObserveHallMask(raw(black ? PLACED : BLACK_PLACED));
            const bool moverP2 = (!black) != rotate;
            Circle(b, moverP2 ? 2U : 1U); b.Advance(300U);
            Check(b.State().side_to_move_black == (black ? 1U : 0U) &&
                  b.UndoHistoryMoveCount() == (black ? 1U : 2U),
                  "PCB comet follows acknowledged authoritative turn");
            bool visited[8]{}; bool onlyRim = true;
            const auto& home = homeIndices[black != rotate ? 1U : 0U];
            for (unsigned t = 0U; t < 3000U; t += 20U) {
                b.Advance(20U);
                for (unsigned chain = 0U; chain < 44U; ++chain) {
                    const auto p = b.PhysicalFrame().pixels[256U+chain];
                    if (!(p.r || p.g || p.b || p.w)) continue;
                    bool expected = false;
                    for (unsigned i = 0U; i < 8U; ++i)
                        if (home[i] == chain) { expected = true; visited[i] = true; }
                    onlyRim &= expected;
                }
            }
            Check(onlyRim, "active-side comet never enters O/X/direction LEDs or another edge");
            for (bool seen : visited) Check(seen, "active-side comet reaches every PCB rim position across both insertions");
            std::printf("[PCB_COMET] rotate=%u black=%u only_active_rim=%u next=lift-next-piece visited=", rotate, black, onlyRim);
            for (bool seen : visited) std::printf("%u", seen);
            std::printf("\n");
        }
    }
}

}
int main() {
    Turn(); Away(); MappedEntry(); ActiveSidePcbInsertion();
    std::printf("[ANIMATION_JOURNEY] failures=%d\n", failures);
    return failures != 0 ? 1 : 0;
}
