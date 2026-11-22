#include "bounded_hint_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/settings_registry.hpp"
#include "modules/settings_state_wire_codec.hpp"
#include "runtime/chess_interaction_model.hpp"

#include <ArduinoJson.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint8_t SOURCE = 52U, TARGET = 36U;
constexpr uint64_t LIFTED = OPENING & ~(UINT64_C(1) << SOURCE);
constexpr uint64_t PLACED = LIFTED | (UINT64_C(1) << TARGET);
constexpr uint8_t HALO_SQUARES[]{35U, 28U, 37U, 44U};
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] PLACEMENT_ORBIT %s\n", message); }
}
bool Same(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
bool SameBoard(const runtime::VirtualLitchSession& a, const runtime::VirtualLitchSession& b) {
    for (uint8_t square = 0; square < 64U; ++square)
        for (uint8_t slot = 0; slot < 3U; ++slot)
            if (!Same(a.SemanticFrame().board[square][slot], b.SemanticFrame().board[square][slot])) return false;
    return true;
}
bool Set(runtime::VirtualLitchSession& s, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return s.MutateSettingJson(711U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied || outcome.status == modules::ControlMutationStatus::Noop);
}
bool Observe(runtime::VirtualLitchSession& s, uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(s.BoardRotate(), s.FirstPlayerIsP2());
    uint64_t physical = 0U;
    for (uint8_t square = 0U; square < 64U; ++square)
        if ((logical & (UINT64_C(1) << square)) != 0U) physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
    return s.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}
uint16_t Pixel(const runtime::VirtualLitchSession& s, uint8_t square, uint8_t slot) {
    const bool rotate = modules::ComputeSquareRotate(s.BoardRotate(), s.FirstPlayerIsP2());
    return modules::MapSquareToPhysical(square, rotate) * 4U + (rotate ? (slot + 2U) % 4U : slot);
}
bool Ready(runtime::VirtualLitchSession& s, const char* policy, const char* style) {
    return s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(s, "P2_END_PIECE_WHEEL", "85") && Set(s, "P1_END_PIECE_WHEEL", "0") &&
        Set(s, "P2_END_PIECE_WHEEL", "170") && Set(s, "WHITE_SQUARES_ON", "false") &&
        Set(s, "MOVE_CONFIRM", policy) && Set(s, "SETTLE_ANIM_STYLE", style) &&
        Observe(s, LIFTED) && Observe(s, PLACED);
}
void Log(const runtime::VirtualLitchSession& s, const char* style, const char* policy, unsigned age, unsigned context) {
    std::printf("[PLACEMENT_ORBIT] style=%s policy=%s output=%u age=%u occupancy=%016llx expected=%016llx phase=%u action=%u history=%u next=wait-for-confirmation-or-relift text=%.*s halo=",
        style, policy, context, age, (unsigned long long)s.State().physical_occupied_mask,
        (unsigned long long)s.State().expected_occupied_mask, s.State().interaction_phase,
        s.State().required_action, s.UndoHistoryMoveCount(), (int)s.DisplayText().size(), s.DisplayText().data());
    for (const uint8_t square : HALO_SQUARES) {
        const auto c = s.SemanticFrame().board[square][0U];
        const auto p = s.PhysicalFrame().pixels[Pixel(s, square, 0U)];
        const auto q = s.PhysicalFrame().pixels[Pixel(s, square, 2U)];
        std::printf("%u:s%u/%u/%u,p%u/%u/%u+%u/%u/%u;", square,c.r,c.g,c.b,p.r,p.g,p.b,q.r,q.g,q.b);
    }
    std::printf(" current=%u cap=%u\n", s.PhysicalFrame().applied_led_current_ma, s.PhysicalFrame().power_limited);
}
void HumanJourney(const char* style, const char* policy, unsigned context) {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = context == 1U;
    config.brightness_scale_256 = context == 1U ? 8U : 86U;
    config.max_led_current_ma = context == 2U ? 80U : 0U;
    test_support::BoundedHintSession fixture(config), control(config);
    auto& s = fixture.session; auto& none = control.session;
    const bool ready = Ready(s, policy, style) && Ready(none, policy, "\"none\"");
    Check(ready, "real Hall pickup/placement and acknowledged setting enter paired production sessions");
    if (!ready) return;
    Check(s.BoardRotate() == (context == 1U) && none.BoardRotate() == (context == 1U) &&
          modules::ComputeSquareRotate(s.BoardRotate(), s.FirstPlayerIsP2()) == (context == 1U),
          "journey resolves the actual board-rotation setting before checking mapped emitters");
    const bool orbit = std::strstr(style, "orbit") != nullptr;
    const bool rainbow = std::strstr(style, "rainbow") != nullptr;
    const unsigned duration = rainbow ? 750U : 320U;
        unsigned fadeStartEnergy = 0U;
    for (unsigned age = 0U; age <= 800U; age += 20U) {
        if (age) Check(s.Advance(20U) == VIRTUAL_LITCH_OK && none.Advance(20U) == VIRTUAL_LITCH_OK, "20ms production render cadence advances both controls");
        Check(s.UndoHistoryMoveCount() == 0U && s.State().expected_occupied_mask == OPENING,
              "optional effect never commits the pending human move");
        if (age == 20U && !orbit) {
            const auto rail = s.SemanticFrame().board[TARGET][0U];
            const auto pixel = s.PhysicalFrame().pixels[Pixel(s, TARGET, 0U)];
            Check(rail.g > 0U && rail.b > 0U && pixel.g > 0U && pixel.b > 0U &&
                  !Same(rail, none.SemanticFrame().board[TARGET][0U]),
                  "Landing flash is an immediate white glint distinguishable from same-hue settling");
        }
        if (orbit && age < duration) {
            const unsigned quadrant = age * 4U / duration;
            const bool trailing = rainbow && quadrant > 0U && (age * 4U % duration) < 160U;
            const uint8_t trail = HALO_SQUARES[(quadrant + 3U) % 4U];
            const uint8_t active = HALO_SQUARES[quadrant];
            bool semantic = false, mapped = false;
            for (uint8_t slot : {0U, 2U}) {
                semantic |= !Same(s.SemanticFrame().board[active][slot], none.SemanticFrame().board[active][slot]);
                mapped |= !Same(s.PhysicalFrame().pixels[Pixel(s, active, slot)], none.PhysicalFrame().pixels[Pixel(none, active, slot)]);
            }
            Check(semantic && (mapped || (rainbow && context == 1U && duration - age < 64U)), "each clockwise quadrant reaches neighboring square rails at normal/low/capped output before quantized fade-off");
            for (uint8_t square = 0U; square < 64U; ++square) {
                if (square == TARGET) continue;
                for (uint8_t slot : {1U})
                    Check(Same(s.SemanticFrame().board[square][slot], none.SemanticFrame().board[square][slot]),
                          "orbit never changes neighboring piece emitters or identities");
                if (trailing && square == trail) {
                    Check(!Same(s.SemanticFrame().board[square][0U], none.SemanticFrame().board[square][0U]) ||
                          !Same(s.SemanticFrame().board[square][2U], none.SemanticFrame().board[square][2U]),
                          "Rainbow keeps a short fading previous-quadrant trail without coloring pieces");
                }
                if (square != active && !(trailing && square == trail))
                    for (uint8_t slot : {0U, 2U})
                        Check(Same(s.SemanticFrame().board[square][slot], none.SemanticFrame().board[square][slot]),
                              "only the active quadrant and bounded Rainbow trail participate; no static cross or offboard wrap");
            }
        }
        for (uint8_t slot : {1U, 3U}) {
            const auto piece = s.SemanticFrame().board[TARGET][1U];
            const auto mapped = s.PhysicalFrame().pixels[Pixel(s, TARGET, slot)];
            Check(piece.r > 0U && piece.g == 0U && piece.b == 0U && mapped.r > 0U && mapped.g == 0U && mapped.b == 0U,
                  "both central piece emitters retain nonzero actual mover hue, including Rainbow orbit");
        }
        if (rainbow && (age == 500U || age == 700U)) {
            unsigned energy = 0U;
            for (const auto square : HALO_SQUARES) for (const auto slot : {0U, 2U}) {
                const auto a = s.SemanticFrame().board[square][slot];
                const auto b = none.SemanticFrame().board[square][slot];
                energy += std::abs(int(a.r)-int(b.r)) + std::abs(int(a.g)-int(b.g)) + std::abs(int(a.b)-int(b.b));
            }
            if (age == 500U) fadeStartEnergy = energy;
            else Check(energy > 0U && energy < fadeStartEnergy,
                "Rainbow visibly fades during its final250ms instead of ending at full strength");
        }
        if (age >= (orbit ? duration : 160U)) Check(SameBoard(s, none), "effect expiry returns to identical independently timed settling");
        if (config.max_led_current_ma) Check(s.PhysicalFrame().applied_led_current_ma <= 80U, "decorative halo obeys the existing current cap");
        if (age % 100U == 20U || age == 760U || age == 800U) Log(s, style, policy, age, context);
    }
    if (std::strcmp(policy, "\"press_circle_to_confirm\"") == 0) {
        for (auto* board : {&s, &none}) {
            Check(board->Advance(3000U) == VIRTUAL_LITCH_OK && board->UndoHistoryMoveCount() == 0U &&
                board->SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
                board->Advance(20U) == VIRTUAL_LITCH_OK && board->SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
                board->Advance(20U) == VIRTUAL_LITCH_OK, "late physical Circle confirms without restarting placement clock");
        }
        Check(SameBoard(s, none), "Circle acknowledgement never replays placement or its halo");
        Check(s.Advance(1200U) == VIRTUAL_LITCH_OK, "original Circle finalize interval finishes");
    } else {
        const unsigned remaining = std::strcmp(policy, "\"settle_5s\"") == 0 ? 4560U : 1560U;
        Check(s.Advance(remaining) == VIRTUAL_LITCH_OK, "configured deadline stays authoritative");
    }
    Check(s.UndoHistoryMoveCount() == 1U, "completion commits exactly once");
    for (const uint8_t square : HALO_SQUARES)
        for (uint8_t slot : {0U, 2U}) {
            const auto c = s.SemanticFrame().board[square][slot];
            Check(c.r == 0U && c.g == 0U && c.b == 0U, "completion never replays an optional halo");
        }
}
uint8_t Square(const char* text) {
    return text && text[0] >= 'a' && text[0] <= 'h' && text[1] >= '1' && text[1] <= '8'
        ? static_cast<uint8_t>((8 - (text[1] - '0')) * 8 + text[0] - 'a') : 64U;
}

void CpuJourney(const char* style, bool nearDark) {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = nearDark ? 8U : 86U;
    test_support::BoundedHintSession fixture(config);
    auto& s = fixture.session;
    Check(Ready(s, "\"settle_2s\"", "\"none\"") && s.Advance(2600U) == VIRTUAL_LITCH_OK &&
        s.UndoHistoryMoveCount() == 1U && Set(s, "SETTLE_ANIM_STYLE", style) &&
        (!nearDark || Set(s, "BRIGHTNESS", "0")) &&
        Set(s, "VS_CPU_PLAYER", "2") && Set(s, "VS_CPU_LEVEL", "1"),
        "real human move and acknowledged CPU settings start the actual P2 CPU search");
    StaticJsonDocument<2048> home;
    bool queued = false;
    for (unsigned attempt = 0U; attempt < 120U; ++attempt) {
        if (deserializeJson(home, s.HomeStateLine().CStr()) != DeserializationError::Ok) break;
        if (std::strcmp(home["a"] | "", "cpu") == 0 && home["c"].is<JsonArray>()) { queued = true; break; }
        Check(s.Advance(100U) == VIRTUAL_LITCH_OK, "production search publishes its bounded CPU prompt");
    }
    const uint8_t from = Square(home["c"][0U] | "--"), to = Square(home["c"][1U] | "--");
    Check(queued && from < 64U && to < 64U, "actual engine selects the physical CPU relocation");
    if (!queued || from >= 64U || to >= 64U) return;
    const uint64_t lifted = PLACED & ~(UINT64_C(1) << from);
    const uint64_t placed = lifted | (UINT64_C(1) << to);
    Check(Observe(s, lifted) && Observe(s, placed), "raw Hall follows the actual CPU source/destination");
    for (unsigned frame = 0U; frame < 30U && s.UndoHistoryMoveCount() < 2U; ++frame)
        Check(s.Advance(20U) == VIRTUAL_LITCH_OK, "physical stability precedes CPU acknowledgement");
    Check(s.UndoHistoryMoveCount() == 2U && s.State().expected_occupied_mask == placed,
          "accepted CPU physical completion starts exactly one latched-mover presentation");
    static constexpr int OFFSETS[4][2]{{0,-1},{-1,0},{0,1},{1,0}};
    const unsigned duration = std::strstr(style, "rainbow") ? 750U : 320U;
    for (unsigned age = 0U; age <= duration; age += 20U) {
        if (age) Check(s.Advance(20U) == VIRTUAL_LITCH_OK, "CPU effect uses the same 20ms output cadence");
        for (uint8_t slot : {1U,3U}) {
            const auto piece = s.PhysicalFrame().pixels[Pixel(s,to,slot)];
            Check(piece.b > 0U && piece.r == 0U && piece.g == 0U,
                  "CPU P2 mover retains actual blue identity after the authoritative side flips, even near dark");
        }
        if (age < duration) {
            const unsigned quadrant = age * 4U / duration;
            const int rank = static_cast<int>(to / 8U) + OFFSETS[quadrant][0];
            const int file = static_cast<int>(to % 8U) + OFFSETS[quadrant][1];
            if (rank >= 0 && rank < 8 && file >= 0 && file < 8) {
                const uint8_t neighbor = static_cast<uint8_t>(rank*8+file);
                const auto l=s.PhysicalFrame().pixels[Pixel(s,neighbor,0U)],r=s.PhysicalFrame().pixels[Pixel(s,neighbor,2U)];
                const auto semantic = s.SemanticFrame().board[neighbor][0U];
                const auto semanticRight = s.SemanticFrame().board[neighbor][2U];
                const bool fadingBelowOneCode = nearDark && std::strstr(style, "rainbow") && duration - age < 64U;
                Check((semantic.r || semantic.g || semantic.b || semanticRight.r || semanticRight.g || semanticRight.b) &&
                      (l.r || l.g || l.b || r.r || r.g || r.b || fadingBelowOneCode),
                      "CPU orbit reaches mapped neighboring rails until its near-dark fade quantizes below one output code");
            }
        }
        Check(s.UndoHistoryMoveCount() == 2U, "optional CPU effect cannot duplicate engine/history acknowledgement");
        if (age % 80U == 0U) std::printf("[PLACEMENT_CPU] style=%s near_dark=%u age=%u from=%u to=%u phase=%u history=%u expected=%016llx next=wait-for-settle-or-next-player\n",
            style,nearDark,age,from,to,s.State().interaction_phase,s.UndoHistoryMoveCount(),(unsigned long long)s.State().expected_occupied_mask);
    }
    Check(s.Advance(2200U) == VIRTUAL_LITCH_OK && s.UndoHistoryMoveCount() == 2U && s.State().side_to_move_black == 0U,
          "CPU effect and settling expire independently with P1 ready and no replay");
}

void RecoveryJourney(const char* style) {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config),control(config);
    auto& s=fixture.session;auto& none=control.session;
    Check(Ready(s,"\"press_circle_to_confirm\"",style)&&Ready(none,"\"press_circle_to_confirm\"","\"none\""),
          "cancellation journey starts with raw paired pending placements");
    for(auto* board:{&s,&none}) Check(board->Advance(60U)==VIRTUAL_LITCH_OK&&Observe(*board,LIFTED)&&Observe(*board,OPENING)&&
        board->Advance(20U)==VIRTUAL_LITCH_OK&&Observe(*board,LIFTED)&&Observe(*board,LIFTED|(UINT64_C(1)<<44U)),
        "quick source return and fresh e2-e3 placement replace the canceled effect");
    for(uint8_t slot:{0U,2U}) {
        const auto old=s.SemanticFrame().board[SOURCE][slot];
        Check(old.r==0U&&old.g==0U&&old.b==0U,"new placement cannot retain a stale optional source-return flash");
    }
    for(auto* board:{&s,&none}) Check(board->Advance(60U)==VIRTUAL_LITCH_OK&&Observe(*board,LIFTED)&&
        Observe(*board,LIFTED|(UINT64_C(1)<<37U))&&board->Advance(320U)==VIRTUAL_LITCH_OK,
        "relift and illegal e2-f4 placement enter actual correction");
    Check(SameBoard(s,none)&&s.State().human_illegal_mask!=0U,
          "required correction exactly replaces optional placement; no orphan halo or false identity");
    const bool rainbow = std::strstr(style, "rainbow") != nullptr;
    for(auto* board:{&s,&none}) Check(Observe(*board,LIFTED)&&Observe(*board,OPENING)&&board->Advance(rainbow ? 600U : 560U)==VIRTUAL_LITCH_OK,
        "raw source return clears correction through the existing restore owner");
    if (rainbow) {
        Check(!SameBoard(s,none), "restored Rainbow retains its fading tail beyond the former560ms lifetime");
        for (auto* board : {&s,&none}) Check(board->Advance(150U)==VIRTUAL_LITCH_OK,
            "restored Rainbow reaches the exact750ms endpoint");
    }
    Check(SameBoard(s,none)&&s.UndoHistoryMoveCount()==0U&&s.State().expected_occupied_mask==OPENING&&s.State().human_illegal_mask==0U,
          "rapid cancellation and invalid placement recover without consuming a turn/history or queuing another effect");
    std::printf("[PLACEMENT_RECOVERY] style=%s history=%u phase=%u expected=%016llx actual=%016llx next=play-or-change-style\n",
        style,s.UndoHistoryMoveCount(),s.State().interaction_phase,(unsigned long long)s.State().expected_occupied_mask,(unsigned long long)s.State().physical_occupied_mask);
}

void Migration() {
    constexpr const char* path = "build/test_placement_virtual_upgrade.json";
    const auto clean = [&]() {
        (void)std::remove(path);
        (void)std::remove("build/test_placement_virtual_upgrade.json.bak");
        (void)std::remove("build/test_placement_virtual_upgrade.json.tmp");
    };
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    for (const char* previous : {"glow", "pulse"}) {
        clean();
        modules::SettingsStoreConfig savedConfig;
        savedConfig.path = path;
        {
            modules::SettingsStore older(savedConfig);
            Check(older.Load(), "old saved placement fixture loads");
            older.Doc()["SETTLE_ANIM_STYLE"] = previous;
            older.Doc()["BRIGHTNESS"] = 3U;
            older.Doc()["MOVE_CONFIRM"] = "settle_5s";
            older.Doc()["VS_CPU_LEVEL"] = 0U;
            Check(older.SaveNow(), "legacy placement label is written through the real settings owner");
        }
        const char* expected = std::strcmp(previous, "glow") == 0 ? "landing_flash" : "mini_orbit";
        for (uint8_t cycle = 0U; cycle < 2U; ++cycle) {
            test_support::BoundedHintSession fixture(config, path);
            auto& s = fixture.session;
            Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) && s.Advance(200U) == VIRTUAL_LITCH_OK,
                  "production-equivalent persisted VirtualLitch session loads and observes the actual board");
            auto& settings = s.SettingsForPersistenceTest();
            modules::settings_state_wire::Payload encoded{};
            Check(std::strcmp(settings.ReadValue("SETTLE_ANIM_STYLE", ""), expected) == 0 &&
                      settings.ReadValue("BRIGHTNESS", 0U) == 3U &&
                      std::strcmp(settings.ReadValue("MOVE_CONFIRM", ""), "settle_5s") == 0 &&
                      modules::settings_state_wire::Encode(settings, encoded) &&
                      encoded[64U] == (std::strcmp(previous, "glow") == 0 ? 0U : 1U),
                  "load migration preserves the chosen ordinal, canonical confirmation and unrelated brightness");
            Check(settings.SaveNow(), "actual migrated session saves before a fresh session reload");
        }
    }
    clean();
}
}
int main() {
    Migration();
    for (const char* style : {"\"landing_flash\"", "\"mini_orbit\"", "\"rainbow_orbit\""})
        for (const char* policy : {"\"settle_2s\"", "\"settle_5s\"", "\"press_circle_to_confirm\""})
            for (unsigned context = 0U; context < 3U; ++context) HumanJourney(style, policy, context);
    for (const char* style : {"\"landing_flash\"", "\"mini_orbit\"", "\"rainbow_orbit\""}) RecoveryJourney(style);
    for (const char* style : {"\"mini_orbit\"", "\"rainbow_orbit\""})
        for (bool nearDark : {false, true}) CpuJourney(style, nearDark);
    std::printf("[PLACEMENT_ORBIT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
