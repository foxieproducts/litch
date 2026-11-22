#include <cstdio>
#include <string_view>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/game_display_icon_overlay.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/settings_state_wire_codec.hpp"

namespace {

using Button = runtime::VirtualLitchPhysicalButton;
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
int failures = 0;

void Check(const bool okay, const char* message) {
    if (okay) return;
    ++failures;
    std::printf("[FAIL] QuickSetup/ImmediateSidePressVerticalGuard: %s\n", message);
}

bool Advance(runtime::VirtualLitchSession& board, const uint32_t ms) {
    const bool okay = board.Advance(ms) == VIRTUAL_LITCH_OK;
    Check(okay, "production timed render/input poll succeeds");
    return okay;
}

bool Edge(runtime::VirtualLitchSession& board, const uint8_t bank,
          const Button button, const bool down) {
    const bool okay = board.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK;
    Check(okay, "physical button input accepted by production adapter");
    return okay && Advance(board, 20U);
}

uint16_t Energy(const virtual_litch_color color) {
    return color.r + color.g + color.b + color.w;
}

void Checkpoint(runtime::VirtualLitchSession& board, const uint8_t bank,
                const bool rotate, const bool swapped, const char* input,
                const uint32_t elapsedMs, const uint8_t selection,
                const char* result, const char* next,
                const char* journey = "ImmediateSidePressVerticalGuard") {
    Check(board.QuickSettingsActive() && board.QuickSettingsSelectedIndex() == selection,
          "authoritative selection matches the requested checkpoint");
    Check(board.State().physical_occupied_mask == OPENING && board.UndoHistoryMoveCount() == 0U,
          "Quick Setup input retains the game and physical occupancy");
    const uint8_t ring = (bank == VIRTUAL_LITCH_OWNER_P2 ? 7U : 23U) - selection;
    const uint8_t semantic = (ring + (rotate ? 16U : 0U)) % 32U;
    const uint8_t physical = hardware::V3_3PhysicalIndexForSemanticStatus(ring);
    const auto marker = board.SemanticFrame().status[semantic];
    const auto pixel = board.PhysicalFrame().pixels[256U + physical];
    Check(marker.r > 0U && marker.r == marker.g && marker.g == marker.b && Energy(pixel) > 0U,
          "neutral selected marker stays on the physical user's edge through RGBW mapping");
    const auto text = board.DisplayText();
    std::printf("[QUICK_SETUP] journey=%s "
                "bank=%u rotate=%u players_swapped=%u t=%u input=%s elapsed_ms=%u "
                "occupancy=%016llx mode=Quick-Setup owner=%u selection=%u "
                "expected_selection=%u result=%s text=\"%.*s\" marker_chain=%u semantic_rgb=%u/%u/%u "
                "physical_rgbw=%u/%u/%u/%u cpu=%u cpu_seat=%u next=%s\n",
                journey, bank, rotate, swapped, board.State().now_ms, input, elapsedMs,
                static_cast<unsigned long long>(board.State().physical_occupied_mask),
                board.QuickSettingsOwner(), board.QuickSettingsSelectedIndex(), selection, result,
                static_cast<int>(text.size()), text.empty() ? "" : text.data(), physical,
                marker.r, marker.g, marker.b, pixel.r, pixel.g, pixel.b, pixel.w,
                board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U),
                board.SettingsForPersistenceTest().ReadValue("VS_CPU_PLAYER", 2U), next);
}

void Journey(const uint8_t bank, const bool rotate, const bool swapped) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    // Fixture orientation only. All reported interaction uses raw physical
    // input, production debounce, owner mapping, controller and mutation code.
    board.SettingsForPersistenceTest().Doc()["BOARD_ROTATE"] = rotate;
    board.SettingsForPersistenceTest().Doc()["GAME_FIRST_PLAYER"] = swapped ? 2U : 1U;
    Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK && Advance(board, 200U),
          "physical opening occupancy reaches production Play");
    Edge(board, bank, Button::X, true);
    Advance(board, 1000U);
    Edge(board, bank, Button::X, false);
    Checkpoint(board, bank, rotate, swapped, "X-hold-open+release", 1000U, 0U,
               "Accepted", "tap-Right-or-adjust-Up/Down");

    Edge(board, bank, Button::Right, true);
    Checkpoint(board, bank, rotate, swapped, "Right-Press", 0U, 1U,
               "Accepted-immediately", "release-to-read-CPU-seat");
    Advance(board, 250U);
    Advance(board, 500U);
    Checkpoint(board, bank, rotate, swapped, "Right-held/repeated", 750U, 1U,
               "Rejected-all-side-repeats", "release-and-fresh-side-press");
    Edge(board, bank, Button::Right, false);
    for (unsigned frame = 0U; frame < 10U; ++frame) Advance(board, 50U);
    Check(board.DisplayText() == "2", "CPU seat icon expires into the current numeric value");
    Checkpoint(board, bank, rotate, swapped, "Right-release+icon-expiry", 0U, 1U,
               "Accepted/value-visible", "tap-Left-for-CPU-difficulty");
    Edge(board, bank, Button::Left, true);
    Edge(board, bank, Button::Left, false);
    Checkpoint(board, bank, rotate, swapped, "Left-tap", 0U, 0U,
               "Accepted-immediately", "Up-adjusts-current-CPU");

    const uint32_t cpuBefore = board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U);
    Edge(board, bank, Button::Up, true);
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U) == cpuBefore + 1U,
          "Up Press changes the current value immediately through ControlMutationService");
    modules::ControlMutationOutcome outcome;
    Check(board.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "immediate Up has authoritative applied acknowledgement");
    Edge(board, bank, Button::Up, false);
    Advance(board, 199U);
    Edge(board, bank, Button::Right, true);
    Checkpoint(board, bank, rotate, swapped, "Up-then-Right-at239ms", 239U, 0U,
               "Rejected-vertical-guard", "release-Right/guard-expires-at 250ms");
    Advance(board, 300U);
    Checkpoint(board, bank, rotate, swapped, "ignored-Right-held-beyond-guard", 539U, 0U,
               "Rejected-no-late-replay", "release-and-fresh-side-press");
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_PLAYER", 2U) == 2U,
          "ignored side Press cannot redirect value adjustment into CPU seat");
    Edge(board, bank, Button::Right, false);

    Edge(board, bank, Button::Up, true);
    Edge(board, bank, Button::Up, false);
    Advance(board, 210U);
    Edge(board, bank, Button::Right, true);
    Checkpoint(board, bank, rotate, swapped, "Up-then-fresh-Right-at 250ms", 250U, 1U,
               "Accepted-at-guard-expiry", "release-or-tap-Left");
    Edge(board, bank, Button::Right, false);
    Edge(board, bank, Button::Left, true);
    Edge(board, bank, Button::Left, false);

    const uint32_t beforeRepeat = board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U);
    Edge(board, bank, Button::Up, true);
    Advance(board, 230U);
    Edge(board, bank, Button::Right, true);
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U) == beforeRepeat + 2U,
          "Up still repeats once at 250ms while side Press is rejected");
    Checkpoint(board, bank, rotate, swapped, "Up-Repeat+Right-Press-same-timestamp", 250U, 0U,
               "Vertical-applied/side-rejected", "release-Right-or-continue-Up");
    Advance(board, hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS - 1U);
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U) == beforeRepeat + 2U,
          "Up emits no second value repeat at18ms");
    Advance(board, 1U);
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL", 0U) == beforeRepeat + 3U,
          "Up retains its19ms production repeat cadence");
    Edge(board, bank, Button::Up, false);
    Edge(board, bank, Button::Right, false);
    Advance(board, 179U);
    Edge(board, bank, Button::Right, true);
    Checkpoint(board, bank, rotate, swapped, "Right239ms-after-last-Up-Repeat", 239U, 0U,
               "Rejected-restarted-guard", "release-then-fresh-side-after250ms");
    Edge(board, bank, Button::Right, false);
    Edge(board, bank, Button::Right, true);
    Edge(board, bank, Button::Right, false);
    Checkpoint(board, bank, rotate, swapped, "fresh-Right-after-restarted-guard", 0U, 1U,
               "Accepted-recovery", "tap-Left");
    Edge(board, bank, Button::Left, true);
    Edge(board, bank, Button::Left, false);

    Check(board.SetButtonDown(bank, Button::Down, true) == VIRTUAL_LITCH_OK &&
              board.SetButtonDown(bank, Button::Left, true) == VIRTUAL_LITCH_OK,
          "same-bank raw diagonal starts in one production poll window");
    Advance(board, 20U);
    Checkpoint(board, bank, rotate, swapped, "Down+Left-Press-same-timestamp", 0U, 0U,
               "Vertical-applied/side-rejected", "release-both-or-X-exit");
    Edge(board, bank, Button::Down, false);
    Edge(board, bank, Button::Left, false);
    Edge(board, bank, Button::Right, true);
    Edge(board, bank, Button::X, true);
    Check(!board.QuickSettingsActive(), "X closes during the vertical guard and ignored side hold");
    Edge(board, bank, Button::X, false);
    const uint8_t otherBank = bank == VIRTUAL_LITCH_OWNER_P1 ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    Edge(board, otherBank, Button::X, true);
    Advance(board, 1000U);
    Edge(board, otherBank, Button::X, false);
    Advance(board, 300U);
    Checkpoint(board, otherBank, rotate, swapped, "reopen-other-bank/ignored-Right-still-held", 0U, 0U,
               "Cancelled-old-gesture/no-repeat-replay", "release-old-Right-and-tap-again");
    Edge(board, bank, Button::Right, false);
    Edge(board, otherBank, Button::Right, true);
    Edge(board, otherBank, Button::Right, false);
    Checkpoint(board, otherBank, rotate, swapped, "fresh-Right-after-reopen", 0U, 1U,
               "Accepted-recovery", "continue-or-wait-for-idle-close");
    Advance(board, 10001U);
    Check(!board.QuickSettingsActive(), "unchanged ten-second idle close returns to Play");
    std::printf("[QUICK_SETUP_VERTICAL_GUARD_EXIT] bank=%u rotate=%u players_swapped=%u "
                "t=%u mode=Play menu_active=0 result=Idle-close "
                "occupancy=%016llx history=%u expiry=10000ms next=ordinary-gameplay\n",
                bank, rotate, swapped, board.State().now_ms,
                static_cast<unsigned long long>(board.State().physical_occupied_mask),
                board.UndoHistoryMoveCount());
}

void OrientationHandoff(const uint8_t bank) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK && Advance(board, 200U),
          "orientation handoff starts from production opening occupancy");
    Edge(board, bank, Button::X, true);
    Advance(board, 1000U);
    Edge(board, bank, Button::X, false);
    for (unsigned item = 0U; item < modules::quick_settings_items::ROTATE; ++item) {
        Edge(board, bank, Button::Right, true);
        Edge(board, bank, Button::Right, false);
    }
    Check(board.QuickSettingsSelectedIndex() == modules::quick_settings_items::ROTATE,
          "raw side Presses select Rotate immediately");
    Edge(board, bank, Button::Up, true);
    Edge(board, bank, Button::Up, false);
    Check(board.BoardRotate(), "raw Up turns board rotation on");
    Edge(board, bank, Button::Up, true);
    Edge(board, bank, Button::Up, false);
    Check(!board.BoardRotate(), "same printed-bank raw Up turns rotation off");
    Edge(board, bank, Button::Left, true);
    Edge(board, bank, Button::Left, false);
    Checkpoint(board, bank, false, false, "Rotate-on/off-then-fresh-Left", 0U,
               modules::quick_settings_items::WHITE_SQUARES,
               "Accepted-owner-handoff-recovery", "fresh-side-press-remains-usable");

    Edge(board, bank, Button::Up, true);
    Edge(board, bank, Button::Up, false);
    const uint8_t before = board.QuickSettingsSelectedIndex();
    Edge(board, bank, Button::Right, true);
    Check(board.QuickSettingsSelectedIndex() == before, "vertical guard rejects side before owner remap");
    modules::ControlMutationOutcome outcome;
    Check(board.MutateSettingJson(901U, "BOARD_ROTATE", "true", outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "typed rotation mutation changes input ownership during ignored side hold");
    Advance(board, 300U);
    Check(board.QuickSettingsSelectedIndex() == before,
          "ownership reseed cannot replay the old ignored side Press or Repeat");
    Edge(board, bank, Button::Right, false);
    Edge(board, bank, Button::Left, true);
    Edge(board, bank, Button::Left, false);
    Check(board.QuickSettingsSelectedIndex() == before - 1U,
          "fresh side Press recovers immediately after ignored hold and typed owner remap");
    std::printf("[QUICK_SETUP_OWNER_HANDOFF] bank=%u t=%u mode=Quick-Setup rotate=%u "
                "selection=%u expected=%u result=ignored-hold-cancelled/fresh-Left-accepted "
                "next=continue-or-X-exit\n", bank, board.State().now_ms, board.BoardRotate(),
                board.QuickSettingsSelectedIndex(), before - 1U);
}

void CpuDifficultySyncJourney() {
    namespace wire = modules::settings_state_wire;
    constexpr uint8_t CPU_LEVEL_INDEX = 7U;
    constexpr uint8_t BANK = VIRTUAL_LITCH_OWNER_P1;
    constexpr uint8_t CPU_ITEM = modules::quick_settings_items::CPU_DIFFICULTY;
    constexpr modules::GameDisplayIconOverlayConfig ICON{};
    constexpr uint32_t ICON_MS = ICON.fadeInMs + ICON.holdMs + ICON.fadeOutMs;
    struct AppAdjustment {
        uint16_t level;
        bool applied = false;
    };

    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    const auto appAdjust = [&](uint16_t level) {
        AppAdjustment adjustment{level};
        board.RunControlIngress([](void* context, modules::SettingsStore& settings,
                                    modules::ControlMutationService& control,
                                    modules::GamePersistenceCoordinator&) {
            auto& request = *static_cast<AppAdjustment*>(context);
            StaticJsonDocument<32> document;
            const char* key = nullptr;
            JsonVariantConst value;
            if (!wire::DecodeMutation(CPU_LEVEL_INDEX, request.level, key, document, value) ||
                std::string_view(key) != "VS_CPU_LEVEL") {
                return;
            }
            const auto outcome = control.MutateSetting(key, value);
            request.applied = outcome.status == modules::ControlMutationStatus::Applied &&
                outcome.settingsRevision == settings.GetRevision();
        }, &adjustment);
        Check(adjustment.applied, "app's typed CPU difficulty change is acknowledged by the settings owner");
    };
    const auto checkTypedValue = [&](uint16_t expected) {
        wire::Payload payload{};
        const auto& settings = board.SettingsForPersistenceTest();
        Check(wire::Encode(settings, payload) &&
                  fxe::ReadU16LE(payload.data() + sizeof(uint32_t) + CPU_LEVEL_INDEX * 2U) == expected &&
                  fxe::ReadU32LE(payload.data()) == settings.GetRevision(),
              "app-bound Settings snapshot contains the current CPU level and document revision");
    };
    const auto checkpoint = [&](const char* input, uint32_t elapsedMs,
                                const char* result, const char* next) {
        Checkpoint(board, BANK, false, false, input, elapsedMs, CPU_ITEM,
                   result, next, "CpuDifficultySync");
    };
    const auto open = [&]() {
        Edge(board, BANK, Button::X, true);
        Advance(board, 1000U);
        const uint32_t openedAt = board.State().now_ms;
        Edge(board, BANK, Button::X, false);
        Check(board.QuickSettingsActive() && board.DisplayText().empty(),
              "raw X hold opens the CPU item with its selection icon");
        return openedAt;
    };
    const auto tap = [&](Button button) {
        Edge(board, BANK, button, true);
        Edge(board, BANK, button, false);
    };

    Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK && Advance(board, 3000U),
          "CPU sync journey begins with a completed raw Hall opening");
    appAdjust(4U);
    const uint32_t openedAt = open();
    Advance(board, 500U);
    appAdjust(11U);
    checkTypedValue(11U);
    Check(board.DisplayText().empty(), "app update during the icon preserves the icon");
    checkpoint("app-11-during-icon", board.State().now_ms - openedAt,
               "Applied/icon-retained", "wait-for-original-icon-expiry");
    Advance(board, openedAt + ICON_MS - board.State().now_ms - 1U);
    Check(board.DisplayText().empty(), "CPU selection icon remains through its original last millisecond");
    Advance(board, 1U);
    Check(board.DisplayText() == "11", "CPU icon expires on schedule into the latest app level");
    checkpoint("original-icon-expiry", ICON_MS, "Latest-value-visible", "press-Down-to-lower-CPU");

    const uint32_t atMaximum = board.SettingsForPersistenceTest().GetRevision();
    tap(Button::Up);
    Check(board.DisplayText() == "11" && board.SettingsForPersistenceTest().GetRevision() == atMaximum,
          "physical Up at the app-selected maximum keeps 11 without publishing a mutation");
    tap(Button::Down);
    modules::ControlMutationOutcome outcome;
    Check(board.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied && board.DisplayText() == "10",
          "physical Down decrements the app's latest value and shows its applied result");
    checkTypedValue(10U);
    checkpoint("board-Down-from-11", 0U, "Applied/State-10", "change-opponent-in-app");

    appAdjust(0U);
    checkTypedValue(0U);
    Check(board.DisplayText() == "0", "app Off refreshes a pinned difficulty number immediately");
    checkpoint("app-Off-while-number-pinned", 0U, "Applied/State-0", "press-Up-to-enable-CPU");
    const uint32_t atOff = board.SettingsForPersistenceTest().GetRevision();
    tap(Button::Down);
    Check(board.DisplayText() == "0" && board.SettingsForPersistenceTest().GetRevision() == atOff,
          "physical Down at Off keeps 0 without publishing a mutation");
    tap(Button::Up);
    Check(board.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied && board.DisplayText() == "1",
          "physical Up from app Off applies the first enabled level");
    checkTypedValue(1U);
    checkpoint("board-Up-from-Off", 0U, "Applied/State-1", "tap-X-to-close");

    tap(Button::X);
    Check(!board.QuickSettingsActive(), "physical X closes the CPU item");
    appAdjust(6U);
    open();
    Advance(board, ICON_MS);
    Check(board.DisplayText() == "6", "reopening after a closed-menu app update shows the current level");
    checkpoint("reopen-after-app-6", ICON_MS, "Latest-value-visible", "continue-or-tap-X");
    tap(Button::X);
    Check(!board.QuickSettingsActive(), "physical X exits the reopened CPU item");
    std::puts("[QUICK_SETUP_CPU_SYNC_EXIT] mode=Play result=Closed "
              "seam=typed-setting-decode/board-mutation/state-encode/raw-buttons/mapped-frame "
              "bypass=BLE-delivery/LED-worker-mailbox/bounded-Hint-budget");
}

} // namespace

int main() {
    for (const bool rotate : {false, true}) {
        for (const bool swapped : {false, true}) {
            for (const uint8_t bank : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
                Journey(bank, rotate, swapped);
            }
        }
    }
    OrientationHandoff(VIRTUAL_LITCH_OWNER_P1);
    OrientationHandoff(VIRTUAL_LITCH_OWNER_P2);
    CpuDifficultySyncJourney();
    std::printf("[QUICK_SETUP_VERTICAL_GUARD] failures=%d seam=physical-button/debounce/dispatch/"
                "controller/selection/typed-mutation/semantic-LED/RGBW "
                "bypass=contact-voltage,fixture-initial-orientation,bounded-Hint-budget "
                "physical-feel=PENDING\n", failures);
    return failures == 0 ? 0 : 1;
}
