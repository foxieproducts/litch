#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/owner_pairing_console_commands.hpp"
#include "owner_pairing_console_test_transport.hpp"

namespace {
using Pairing = modules::BoardOwnerPairingRuntime;
using Core = Pairing::Controller;
using Record = modules::BoardOwnerRecord;
using Button = runtime::VirtualLitchPhysicalButton;
using Action = modules::owner_pairing_session_wire::Action;
using State = Pairing::State;
constexpr uint64_t OCCUPANCY = UINT64_C(0xFFFF00000000FFFF);
constexpr Pairing::Connection LINK{10U, 20U, 30U};
int failures = 0;
void Check(bool okay, const char* message) {
    if (!okay) { ++failures; std::printf("[PAIRING_JOURNEY_FAIL] %s\n", message); }
}
struct Persistence final : modules::BoardOwnerPersistence {
    Receipt active = 0U;
    Receipt next = 1U;
    Outcome outcome = Outcome::Pending;
    Record requested = Record::Invalid;
    Record durable = Record::Absent;
    unsigned saves = 0U;
    modules::BoardOwnerAddBaseline baseline{};
    bool SaveAddingJournal(const modules::BoardOwnerAddBaseline& value, Receipt& receipt) override {
        if (!value.Valid() || !SaveOwnerRecord(Record::Adding, receipt)) return false;
        baseline = value;
        return true;
    }
    modules::BoardOwnerAddBaseline AddingBaseline() const override { return baseline; }
    bool SaveOwnerRecord(Record value, Receipt& receipt) override {
        if (active != 0U) return false;
        requested = value;
        receipt = active = next++;
        outcome = Outcome::Pending;
        ++saves;
        return true;
    }
    Outcome GetOutcome(Receipt receipt) const override {
        return receipt == active ? outcome : Outcome::Unknown;
    }
    bool Release(Receipt receipt) override {
        if (receipt != active || receipt == 0U) return false;
        active = 0U;
        return true;
    }
};
virtual_litch_config Config(virtual_litch_hardware hardware = VIRTUAL_LITCH_HARDWARE_V3_3) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardware;
    return config;
}
struct Fixture {
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    Persistence persistence;
    Pairing pairing{persistence};
    const bool dedicatedControls;
    test_support::BoundedHintSession fixture;
    runtime::VirtualLitchSession& board = fixture.session;
    Fixture(Record record = Record::Absent, bool rotated = false,
            virtual_litch_hardware hardware = VIRTUAL_LITCH_HARDWARE_V3_3,
            bool firstPlayerIsP2 = false)
        : dedicatedControls(hardware == VIRTUAL_LITCH_HARDWARE_V3_3), fixture(Config(hardware)) {
        persistence.durable = record;
        pairing.Boot(record, {true, uint8_t(record == Record::Active ? 1U : 0U)}, true);
        board.BindOwnerPairingRuntime(pairing);
        board.SettingsForPersistenceTest().Doc()["BOARD_ROTATE"] = rotated;
        board.SettingsForPersistenceTest().Doc()["GAME_FIRST_PLAYER"] = firstPlayerIsP2 ? 2U : 1U;
        Check(board.ObserveHallMask(OCCUPANCY) == VIRTUAL_LITCH_OK, "opening Hall occupancy accepted");
        Advance(200U);
    }
    void Advance(uint32_t ms) {
        Check(board.Advance(ms) == VIRTUAL_LITCH_OK, "production timer/render advance accepted");
    }
    void Edge(uint8_t end, Button button, bool down) {
        const uint8_t bank = end == 1U ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
        Check(board.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK, "raw physical button accepted");
        Advance(20U);
    }
    void Tap(uint8_t end, Button button) { Edge(end, button, true); Edge(end, button, false); }
    void Save() {
        persistence.outcome = Persistence::Outcome::Saved;
        persistence.durable = persistence.requested;
        pairing.UpdateDurableState(persistence.durable, true);
        Advance(1U);
    }
    void OpenMenu(uint8_t end) {
        Edge(end, Button::X, true); Advance(1000U); Edge(end, Button::X, false);
        Check(board.QuickSettingsActive(), "X hold opens production Quick Settings");
    }
    void OpenClear(uint8_t end) {
        OpenMenu(end);
        for (unsigned index = 0U; index < modules::quick_settings_items::OWNER_PAIRING; ++index)
            Tap(end, Button::Right);
        const auto expectedState = persistence.durable == Record::Absent ? State::Unpaired : State::ClearPrompt;
        Check(board.QuickSettingsSelectedIndex() == modules::quick_settings_items::OWNER_PAIRING &&
                  pairing.Status().state == expectedState && pairing.Presentation().cue == Pairing::PhysicalCue::Clear,
              "actual rightmost Quick Settings selection arms production local clear");
    }
};

void PreviewStatusFrame(Fixture& f, const char* stage) {
    if (std::getenv("LITCH_PAIRING_PREVIEW") == nullptr) return;
    std::printf("[PAIRING_PIXELS] stage=%s t=%u pixels=", stage, f.board.State().now_ms);
    for (uint8_t index = 0U; index < 44U; ++index) {
        const auto c = f.board.PhysicalFrame().pixels[256U + index];
        std::printf("%u,%u,%u%s", c.r, c.g, c.b, index == 43U ? "\n" : ";");
    }
}

uint16_t Energy(virtual_litch_color color) { return color.r + color.g + color.b + color.w; }

bool BluetoothGlyphVisible(const Fixture& f) {
    constexpr const char* rows[]{"..BB..", "B.B.B.", ".BBB..", ".BBB..", "B.B.B.", "..BB.."};
    for (uint8_t y = 0U; y < 6U; ++y) {
        for (uint8_t x = 0U; x < 6U; ++x) {
            if (rows[y][x] != 'B') continue;
            const auto pixel = f.board.SemanticFrame().board[(y + 1U) * 8U + x + 1U][0U];
            if (pixel.b == 0U || pixel.r != 0U || pixel.g != 0U) return false;
        }
    }
    return true;
}

virtual_litch_color ControlPixel(Fixture& f, uint8_t end, hardware::ControlLedRole role) {
    const bool p2 = end == 1U;
    const uint8_t index = f.dedicatedControls
        ? hardware::V3_3PhysicalIndexForLogicalControl(p2, role, false)
        : modules::PlayerHomeLedIndex(p2 ? modules::UiOwner::P2 : modules::UiOwner::P1,
                                      role == hardware::ControlLedRole::O ? 2U : 5U);
    return f.board.PhysicalFrame().pixels[256U + index];
}

virtual_litch_color ControlPixel(Fixture& f, hardware::ControlLedRole role) {
    return ControlPixel(f, f.pairing.Presentation().physicalEnd, role);
}

void CheckClearAtOpeningEnd(Fixture& f, uint8_t openingEnd) {
    Check(f.pairing.Status().physicalEnd == openingEnd,
          "clear authority stays at the literal physical bank that opened Quick Settings");
    for (const auto role : {hardware::ControlLedRole::X, hardware::ControlLedRole::O}) {
        const auto own = ControlPixel(f, openingEnd, role);
        const auto other = ControlPixel(f, 1U - openingEnd, role);
        Check(own.b > 0U && own.b > own.g && own.r == 0U && Energy(other) == 0U,
              "only the opening bank's printed physical X/O pulse blue through final output");
    }
}

const char* ModeName(State state) {
    switch (state) {
        case State::AwaitPhysical: return "Phone-confirmation";
        case State::Securing: return "Securing-owner";
        case State::PersistingOwner: return "Saving-owner";
        case State::ClearPrompt: return "Local-clear-confirmation";
        case State::Clearing: return "Clearing-owner";
        case State::OwnerReady: return "Owner-ready";
        case State::OwnedOther: return "Owned";
        case State::Unpaired: return "Unpaired";
        case State::Failed: return "Recovery-required";
        default: return "Unavailable";
    }
}

void Checkpoint(Fixture& f, const char* journey, const char* input,
                const char* result, const char* instruction, const char* expectedNextAction,
                bool cue = true) {
    const auto state = f.pairing.Status(LINK);
    const auto presentation = f.pairing.Presentation();
    Check(std::string_view(presentation.nextAction) == expectedNextAction,
          "checkpoint authoritative next action matches the intended runtime action");
    const auto oPixel = ControlPixel(f, hardware::ControlLedRole::O);
    const auto xPixel = ControlPixel(f, hardware::ControlLedRole::X);
    if (cue && (presentation.cue == Pairing::PhysicalCue::Claim ||
                presentation.cue == Pairing::PhysicalCue::Clear)) {
        Check(Energy(oPixel) > 0U && Energy(xPixel) > 0U,
              "target printed physical X/O are lit through production profile/power output");
    } else if (cue && presentation.cue == Pairing::PhysicalCue::Confirmed) {
        for (uint8_t bank = 0U; bank < 2U; ++bank)
            for (const auto role : {hardware::ControlLedRole::O, hardware::ControlLedRole::X}) {
                const auto green = ControlPixel(f, bank, role);
                Check(green.g > 0U && green.r == 0U && green.b == 0U,
                      "accepted consent acknowledges all four physical X/O in green");
            }
    } else if (cue && presentation.cue == Pairing::PhysicalCue::Waiting) {
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay waiting;
        f.pairing.ComposePhysicalCue(waiting);
        Check(!presentation.active && waiting.mask == 0U && waiting.controlMask == 0U &&
                  waiting.emulatedControlMask == 0U && waiting.controlAccentMask == 0U,
              "pending native work adds no more board input prompt or endless processing bounce");
    }
    Check(f.board.State().physical_occupied_mask == OCCUPANCY &&
              f.board.UndoHistoryMoveCount() == 0U,
          "pairing input preserves production physical occupancy/game history");
    const auto text = f.board.DisplayText();
    std::printf("[OWNER_PAIRING_JOURNEY] journey=%s hardware=%s t=%u occupancy=%016llx input=%s "
                "mode=%s end=%u attempt=%u result=%s menu_active=%u selection=%u glyph=%s text=\"%.*s\" "
                "semantic_controls=%s hold256=%u physicalX=%u/%u/%u/%u "
                "physicalO=%u/%u/%u/%u next=%s expected_next=%s instruction=%s expiry=%s\n",
                journey, f.dedicatedControls ? "V3.3" : "V3", f.board.State().now_ms,
                static_cast<unsigned long long>(OCCUPANCY), input,
                ModeName(state.state), state.physicalEnd, state.attempt, result,
                unsigned(f.board.QuickSettingsActive()), unsigned(f.board.QuickSettingsSelectedIndex()),
                BluetoothGlyphVisible(f) ? "Bluetooth" : "none",
                static_cast<int>(text.size()), text.empty() ? "" : text.data(),
                !presentation.active ? "none" :
                    presentation.cue == Pairing::PhysicalCue::Confirmed ? "all-four-X/O/green-fade" :
                    presentation.cue == Pairing::PhysicalCue::Waiting ? "no-extra-board-input" :
                    presentation.cue == Pairing::PhysicalCue::Restart ? "red-X/blinking+restart-text" :
                    presentation.cue == Pairing::PhysicalCue::Clear ?
                        (presentation.progress256 > 0U ? "blue-X/O/brightening+blue-perimeter-lap" : "blue-X/O/pulsing") :
                    presentation.cue == Pairing::PhysicalCue::Clearing ? "blue+orange-purple/perimeter-chase" :
                    presentation.cue == Pairing::PhysicalCue::Cleared ? "blue+orange-purple/bounded-success-pulse" : "blue-X/O",
                presentation.progress256,
                xPixel.r, xPixel.g, xPixel.b, xPixel.w, oPixel.r, oPixel.g, oPixel.b, oPixel.w,
                presentation.nextAction, expectedNextAction, instruction, state.state == State::AwaitPhysical ? "10000ms-from-Begin" :
                    presentation.cue == Pairing::PhysicalCue::Clearing ? "until-verified-completion" :
                    presentation.cue == Pairing::PhysicalCue::Cleared ? "2000ms-from-completion" : "none");
}

void ClaimSuccess(uint8_t end, bool rotated,
                  virtual_litch_hardware hardware = VIRTUAL_LITCH_HARDWARE_V3_3) {
    Fixture f(Record::Absent, rotated, hardware);
    Pairing::Command begin;
    begin.attempt = 11U;
    Check(f.pairing.Begin(begin, LINK), "default shared-session Begin admits either-end consent");
    f.Advance(1U);
    for (uint8_t bank : {uint8_t{0U}, uint8_t{1U}}) {
        for (const auto role : {hardware::ControlLedRole::X, hardware::ControlLedRole::O}) {
            const auto pixel = ControlPixel(f, bank, role);
            Check(pixel.b > pixel.g && pixel.r == 0U,
                  "both physical X/O pairs steadily cue either-end consent through production output");
        }
    }
    Checkpoint(f, "FreshSameEndClaim", "Begin", "Await-physical", "press-both-blue-buttons", "press-both-blue-buttons");
    f.Advance(7500U);
    Check(f.pairing.Status().state == State::AwaitPhysical,
          "either-end prompt remains available after the old five-second deadline");
    Checkpoint(f, "FreshSameEndClaim", "7500ms-before-consent", "Await-physical", "press-both-blue-buttons", "press-both-blue-buttons");
    f.Edge(end, Button::O, true);
    f.Edge(end, Button::X, true);
    Check(f.pairing.Custody().PendingEffect().type == Core::EffectType::RequestSecurity,
          "two fresh same-end debounced Presses immediately request native security");
    Check(f.pairing.Status().physicalEnd == end,
          "production raw input latches the actual consenting bank before native security");
    Checkpoint(f, "FreshSameEndClaim", "O-then-X", "Accepted-no-hold", "wait-native-bond", "wait");
    auto effect = f.pairing.Custody().PendingEffect();
    Check(f.pairing.ClaimLeaseMatches(LINK, effect), "native effect matches exact requesting session");
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, true, true, true, true});
    Check(!f.pairing.OwnerReady(LINK), "native acknowledgement does not bypass durable owner save");
    f.Save();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token);
    f.Advance(1U);
    Check(f.pairing.OwnerReady(LINK), "durable owner receipt completion admits owner");
    f.Edge(end, Button::O, false); f.Edge(end, Button::X, false);
    Check(!f.board.QuickSettingsActive(), "confirmed held release does not enter a game/menu action");
    Checkpoint(f, "FreshSameEndClaim", "native-bond+owner-saved+release", "Owner-ready",
               "ordinary-gameplay", "ordinary-gameplay", false);
}

void AddSavedPhonesJourney() {
    for (uint8_t saved = 1U; saved < 3U; ++saved) {
        Fixture f(Record::Active);
        f.pairing.Boot(Record::Active, {true, saved}, true);
        modules::BoardOwnerAddBaseline baseline{};
        baseline.saved.count = saved;
        for (uint8_t i = 0U; i < saved; ++i) baseline.saved.peers[i].address[0U] = i + 1U;
        Check(f.pairing.Begin({Action::Begin, 81U, 2U}, LINK, baseline), "saved phone Add opens existing consent");
        f.Advance(Pairing::CLAIM_WINDOW_MS);
        Check(f.pairing.Status().state == State::OwnedOther && f.persistence.saves == 0U,
              "Add expiry preserves all saved phones without native or durable mutation");
        Checkpoint(f, "AddSavedPhones", "prompt-expiry", "Owned-unchanged", "retry-from-app", "ordinary-gameplay", false);
        Check(f.pairing.Begin({Action::Begin, 82U, 2U}, LINK, baseline), "Add can retry after expiry");
        f.Advance(7500U);
        Check(f.pairing.Status().state == State::AwaitPhysical,
              "saved-phone Add accepts consent after the old five-second deadline");
        f.Edge(0U, Button::X, true); f.Edge(1U, Button::O, true);
        Check(f.pairing.Status().state == State::AwaitPhysical, "cross-bank Add chord cannot consent");
        Checkpoint(f, "AddSavedPhones", "cross-bank-X/O", "Rejected-keep-prompt", "press-same-end", "press-both-blue-buttons");
        f.Edge(0U, Button::X, false); f.Edge(1U, Button::X, true);
        Check(f.pairing.Custody().GetPhase() == Core::Phase::SavingAddJournal,
              "fresh same-bank chord starts journal before security");
        Checkpoint(f, "AddSavedPhones", "fresh-same-end-X/O", "Journal-pending-prior-phones-retained", "wait", "wait");
        f.Save();
        auto effect = f.pairing.Custody().PendingEffect();
        Check(effect.type == Core::EffectType::RequestSecurity && f.pairing.ClaimLeaseMatches(LINK, effect),
              "Add native effect follows durable journal and exact physical lease");
        auto identities = baseline.saved;
        identities.peers[saved].address[0U] = 9U; ++identities.count;
        f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
            effect.epoch, effect.physicalEnd, {{true, identities.count, identities}, true, true, true, true});
        f.Advance(1U);
        Checkpoint(f, "AddSavedPhones", "native-complete-all-identities", "Owner-save-pending", "wait", "wait");
        f.Save(); effect = f.pairing.Custody().PendingEffect(); f.pairing.Custody().AcknowledgeSuccess(effect.token);
        f.Edge(1U, Button::X, false); f.Edge(1U, Button::O, false);
        Check(f.pairing.OwnerReady(LINK) && f.pairing.Custody().SavedPhoneCount() == saved + 1U,
              "second and third phone reach durable readiness with no accidental game action");
        Checkpoint(f, "AddSavedPhones", "owner-saved-and-release", "Owner-ready", "ordinary-gameplay", "ordinary-gameplay", false);
    }
    Fixture full(Record::Active);
    full.pairing.Boot(Record::Active, {true, 3U}, true);
    Check(!full.pairing.Begin({Action::Begin, 83U, 2U}, LINK) && full.persistence.saves == 0U &&
              full.pairing.Custody().PendingEffect().type == Core::EffectType::None,
          "fourth phone never starts a physical prompt, native security or key mutation");
    Checkpoint(full, "AddSavedPhones", "fourth-phone-Begin", "Rejected-capacity", "unpair-all-on-board", "ordinary-gameplay", false);
}

void PrivateUsbConsentJourney() {
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) return;
    Fixture f;
    test_support::OwnerPairingConsoleTransport transport;
    Console console(transport.hybrid, {false});
    Check(modules::RegisterOwnerPairingConsoleCommands(console, f.pairing), "private preview registers production USB adapter");
    Check(f.pairing.Begin({Action::Begin, 91U, 0U}, LINK), "USB journey starts with ordinary session Begin");
    f.Advance(1U);
    Checkpoint(f, "PrivateUsbConsentClaim", "Begin", "Await-physical", "usb-exact-consent-or-buttons", "press-both-blue-buttons");
    transport.ble.input += "ownerpair consent 91 0 10 20 30 1\n"; console.Update(); f.Advance(1U);
    Check(f.pairing.Status().state == State::AwaitPhysical && f.persistence.saves == 0U,
          "BLE Console cannot substitute for local developer consent");
    Checkpoint(f, "PrivateUsbConsentClaim", "BLE-consent-rejected", "Rejected-no-mutation", "usb-exact-consent-or-buttons", "press-both-blue-buttons");
    transport.usb.input += "ownerpair consent 91 0 10 20 30 1\n"; console.Update(); f.Advance(1U);
    auto effect = f.pairing.Custody().PendingEffect();
    Check(effect.type == Core::EffectType::RequestSecurity && f.persistence.saves == 0U &&
              f.pairing.ClaimLeaseMatches(LINK, effect), "USB consent enters same exact native lease as physical chord");
    Checkpoint(f, "PrivateUsbConsentClaim", "USB-exact-consent", "Accepted-security-pending", "wait-native-bond", "wait");
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, true, true, true, true});
    f.Advance(1U);
    Check(!f.pairing.OwnerReady(LINK), "USB native result still waits for durable owner receipt");
    Checkpoint(f, "PrivateUsbConsentClaim", "native-security-success", "Owner-save-pending", "wait-durable-owner", "wait");
    f.Save(); effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.OwnerReady(LINK) && !f.board.QuickSettingsActive(), "USB consent completes only through durable owner gate");
    Checkpoint(f, "PrivateUsbConsentClaim", "durable-owner-receipt", "Owner-ready", "ordinary-gameplay", "ordinary-gameplay", false);
}

void EitherEndInvalidInputRecovery() {
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        Fixture f;
        f.Edge(end, Button::X, true);
        Pairing::Command begin;
        begin.attempt = 92U;
        Check(f.pairing.Begin(begin, LINK), "either-end lease opens with preheld raw X");
        f.Edge(end, Button::O, true);
        Check(f.pairing.Status().state == State::AwaitPhysical,
              "preheld physical X cannot combine with fresh O at either end");
        Checkpoint(f, "EitherEndFreshChordRecovery", "preheld-X/fresh-O", "Rejected-preheld",
                   "release-and-press-same-end-pair", "press-both-blue-buttons");
        f.Edge(end, Button::X, false); f.Edge(end, Button::O, false);
        f.Edge(end, Button::X, true); f.Edge(1U - end, Button::O, true);
        Check(f.pairing.Status().state == State::AwaitPhysical && !f.board.QuickSettingsActive(),
              "cross-bank raw physical presses neither confirm nor reach game/menu");
        Checkpoint(f, "EitherEndFreshChordRecovery", "cross-bank-X/O", "Rejected-cross-bank",
                   "press-same-end-pair", "press-both-blue-buttons");
        f.Edge(end, Button::O, true);
        const auto winner = f.pairing.Custody().PendingEffect();
        f.Edge(1U - end, Button::X, true);
        Check(winner.type == Core::EffectType::RequestSecurity && winner.physicalEnd == end &&
                  f.pairing.Custody().PendingEffect().token == winner.token &&
                  f.pairing.Status().physicalEnd == end,
              "one winning raw same-bank chord starts security once and consumes the other bank");
        Checkpoint(f, "EitherEndFreshChordRecovery", "fresh-same-end-pair/second-bank", "One-winner",
                   "wait-native-bond", "wait");
    }
}

void InvalidConsentAndRecovery() {
    Fixture f;
    f.Edge(0U, Button::X, true);
    Check(f.pairing.Begin({Action::Begin, 12U, 0U}, LINK), "Begin opens with X preheld");
    const uint32_t promptStartedMs = f.board.State().now_ms;
    f.Edge(0U, Button::O, true);
    Check(f.pairing.Status().state == State::AwaitPhysical,
          "preheld X cannot satisfy fresh confirmation");
    Checkpoint(f, "HeldWrongEndTimeoutRecovery", "preheld-X+fresh-O", "Rejected-preheld", "release-both", "press-both-blue-buttons");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    f.Edge(1U, Button::X, true); f.Edge(1U, Button::O, true);
    Check(f.pairing.Status().state == State::AwaitPhysical && !f.board.QuickSettingsActive(),
          "wrong-end chord is consumed before normal game/menu owners");
    const auto expiring = f.pairing.ReadPendingConsent();
    Checkpoint(f, "HeldWrongEndTimeoutRecovery", "wrong-end-X+O", "Rejected-wrong-end", "press-end0", "press-both-blue-buttons");
    f.Advance(9999U - (f.board.State().now_ms - promptStartedMs));
    Check(f.pairing.Status().state == State::AwaitPhysical,
          "wrong-end input cannot retire the prompt before ten seconds");
    f.Advance(1U);
    Check(f.pairing.Status().reason == Pairing::Reason::Timeout && f.persistence.saves == 0U,
          "ten-second expiry admits no persistence/native effect");
    Check(!f.pairing.ConfirmPendingConsent(expiring) &&
              f.pairing.Presentation().cue != Pairing::PhysicalCue::Confirmed,
          "expired queued consent cannot show a green acknowledgement or extend the ten-second lease");
    f.Edge(1U, Button::X, false); f.Edge(1U, Button::O, false);
    Checkpoint(f, "HeldWrongEndTimeoutRecovery", "ten-second-expiry+release", "Unpaired",
               "retry-from-app", "ordinary-gameplay", false);
    Check(f.pairing.Begin({Action::Begin, 13U, 0U}, LINK), "new attempt recovers after timeout");
    Check(!f.pairing.Cancel({Action::Cancel, 12U, 0U}, LINK), "old attempt cannot cancel retry");
    Check(f.pairing.Cancel({Action::Cancel, 13U, 0U}, LINK), "current attempt cancels before native security");
    Check(f.pairing.Begin({Action::Begin, 14U, 0U}, LINK), "cancel preserves connection for another attempt");
    uint8_t malformed[]{1U, 1U, 0U, 0U, 0U};
    Pairing::Command decoded;
    Check(!modules::owner_pairing_session_wire::DecodeCommand(malformed, sizeof(malformed), decoded) &&
              f.pairing.Status().attempt == 14U,
          "malformed zero-attempt ingress cannot replace current prompt");
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true);
    const auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Retire(LINK);
    Check(f.pairing.NeedsDisconnect() && !f.pairing.ClaimLeaseMatches(LINK, effect),
          "shared-session retirement fences security and requests actual disconnect");
    // Advance production timers/rendering while the native callback is still
    // pending. Virtual time cannot acknowledge a native disconnect itself.
    f.Advance(250U);
    Check(f.pairing.Status().state == State::Clearing &&
              f.pairing.Presentation().cue == Pairing::PhysicalCue::Waiting &&
              f.board.DisplayText().empty() && f.pairing.NeedsDisconnect() &&
              !f.pairing.OwnerReady(LINK) && !f.pairing.ClaimLeaseMatches(LINK, effect),
          "normal asynchronous disconnect wait is Clearing without restart text or owner authority");
    Checkpoint(f, "HeldWrongEndTimeoutRecovery", "retire/250ms-native-disconnect-pending",
               "Cleanup-in-progress", "wait-for-native-disconnect", "wait");
    f.pairing.Disconnected(LINK);
    f.Save();
    auto cleanup = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(cleanup.token, true);
    cleanup = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(cleanup.token, true);
    f.Save();
    Check(f.pairing.Custody().Pairable() && f.persistence.durable == Record::Absent,
          "actual native teardown+journaled rollback recovers to unpaired");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::None &&
              !f.pairing.Presentation().active && f.board.DisplayText().empty(),
          "completed retired-claim rollback removes historical error cue and obsolete restart text");
    Checkpoint(f, "HeldWrongEndTimeoutRecovery", "retire/disconnect/rollback", "Unpaired",
               "retry-from-app", "ordinary-gameplay", false);
}

uint8_t LargestRingDelta(const virtual_litch_physical_frame& first,
                        const virtual_litch_physical_frame& second);

void CompleteLocalClear(Fixture& f, const char* journey) {
    static bool successPreviewTaken = false;
    const bool preview = std::getenv("LITCH_PAIRING_PREVIEW") != nullptr &&
        f.dedicatedControls && !successPreviewTaken;
    if (preview) successPreviewTaken = true;
    f.Save();
    auto effect = f.pairing.Custody().PendingEffect();
    Check(effect.type == Core::EffectType::QuiesceAndRejectWork,
          "durable Clearing precedes actual native quiesce");
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    Check(!f.pairing.Custody().Pairable() &&
              f.pairing.Presentation().cue == Pairing::PhysicalCue::Clearing,
          "native deletion and durable absent still await the actual completion acknowledgement");
    Checkpoint(f, journey, "durable-absent/completion-pending", "Pending",
               "wait-completion", "wait");
    const auto beforeCompletion = f.board.PhysicalFrame();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    if (f.dedicatedControls)
        Check(LargestRingDelta(beforeCompletion, f.board.PhysicalFrame()) <= 18U,
              "quick checked cleanup preserves the hold-to-pending blend when success starts within 200 ms");
    Check(f.pairing.Custody().Pairable() && f.persistence.durable == Record::Absent,
          "offline clear succeeds only after native deletion and durable absent");
    Checkpoint(f, journey, "journal/native-delete/absent", "Unpaired",
               "leave-item8-or-pair-app", "pair-from-app");
    // Success now eases through the same 160 ms half-cycles. Its first peak is
    // allowed to crossfade from the pending tails, never appear as a color cut.
    for (unsigned step = 0U; step < 8U; ++step) {
        f.Advance(20U);
        if (preview) PreviewStatusFrame(f, "clear-success");
    }
    for (unsigned half = 0U; half < 6U; ++half) {
        for (uint8_t pixel = 0U; pixel < 32U; ++pixel) {
            const uint8_t physical = f.dedicatedControls
                ? hardware::V3_3PhysicalIndexForSemanticStatus(pixel) : pixel;
            Check((Energy(f.board.PhysicalFrame().pixels[256U + physical]) > 0U) ==
                      (half % 2U == 0U),
                  "production output shows three whole-perimeter success pulses with dark troughs");
        }
        Checkpoint(f, journey, half % 2U == 0U ? "success-on" : "success-off",
                   "Unpaired", "pair-from-app", "pair-from-app");
        for (unsigned step = 0U; step < 8U; ++step) {
            f.Advance(20U);
            if (preview) PreviewStatusFrame(f, "clear-success");
        }
    }
    for (unsigned step = 0U; step < 44U; ++step) {
        f.Advance(20U);
        if (preview) PreviewStatusFrame(f, "clear-success-exit");
    }
    Checkpoint(f, journey, "success-two-second-expiry", "Unpaired",
               "leave-item8-or-pair-app", "ordinary-gameplay", false);
}

void ClearFeedbackDeadlineWithDelayedNativeCompletion() {
    Fixture f(Record::Active, false, VIRTUAL_LITCH_HARDWARE_V3_3);
    constexpr const char* journey = "ClearFeedbackDeadlineWithDelayedNativeCompletion";
    f.OpenClear(0U);
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true); f.Advance(2000U);
    Checkpoint(f, journey, "same-end-X+O-2000ms", "Clearing-pending",
               "release-and-wait", "wait");
    f.Advance(1999U);
    Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::Clearing,
          "pending blue/orange animation remains visible before original deadline");
    f.Advance(1U);
    Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::Clearing &&
              f.persistence.requested == Record::Clearing && !f.pairing.Custody().Pairable(),
          "pending clear keeps its visible chase until native and durable completion");
    Checkpoint(f, journey, "pending-Clearing-at-2000ms", "Blue-orange-pending/native-not-complete",
               "wait-native-and-storage", "wait");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    f.Save();
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.Custody().Pairable() && f.pairing.Presentation().cue == Pairing::PhysicalCue::Cleared,
          "late checked completion starts the success episode on its own clock");
    Checkpoint(f, journey, "late-native-clear/verified-Absent", "Unpaired/visible-success",
               "pair-from-app", "pair-from-app");
    f.Advance(1999U);
    Check(f.pairing.Presentation().active, "late success retains a complete two-second visible episode");
    f.Advance(1U);
    Checkpoint(f, journey, "two-seconds-after-completion", "Success-expired",
               "ordinary-gameplay", "ordinary-gameplay", false);
}

void AlreadyClearStaysInBluetooth(uint8_t end, bool xFirst, bool rotated) {
    Fixture f(Record::Absent, rotated);
    constexpr const char* journey = "AlreadyClearStaysInBluetooth";
    f.OpenClear(end);
    const uint8_t other = 1U - end;
    f.Edge(other, Button::X, true); f.Edge(other, Button::O, true); f.Advance(2000U);
    Check(f.board.QuickSettingsActive() && f.pairing.Presentation().progress256 == 0U,
          "already-clear wrong-bank chord stays in Bluetooth without acknowledging");
    f.Edge(other, Button::X, false); f.Edge(other, Button::O, false);
    const Button first = xFirst ? Button::X : Button::O;
    const Button second = xFirst ? Button::O : Button::X;
    f.Edge(end, first, true); f.Edge(end, second, true); f.Advance(100U);
    Check(f.board.QuickSettingsActive(), "already-clear X/O is local consent instead of CloseOverlay");
    if (!f.board.QuickSettingsActive()) return;
    const auto before = Energy(ControlPixel(f, end, hardware::ControlLedRole::X));
    f.Advance(900U);
    Check(f.pairing.Presentation().progress256 == 128U &&
              Energy(ControlPixel(f, end, hardware::ControlLedRole::X)) > before,
          "already-clear physical controls visibly brighten with the existing hold progress");
    Checkpoint(f, journey, "same-end-X+O-1000ms", "Half-progress/no-native-operation",
               "keep-holding", "keep-holding-both");
    f.Edge(end, first, false);
    Check(f.pairing.Presentation().progress256 == 0U && f.persistence.saves == 0U,
          "already-clear early release cancels the local acknowledgement");
    f.Edge(end, first, true); f.Advance(2000U);
    Check(f.board.QuickSettingsActive() && f.pairing.Presentation().cue == Pairing::PhysicalCue::Cleared &&
              f.pairing.Custody().Pairable() && f.persistence.saves == 0U &&
              f.pairing.Custody().PendingEffect().type == Core::EffectType::None,
          "already-clear hold acknowledges checked absence in the same menu without deletion or save");
    Checkpoint(f, journey, "fresh-same-end-X+O-2000ms", "Already-clear/no-native-or-persistence-effects",
               "release-both", "pair-from-app");
    f.Edge(end, first, false); f.Edge(end, second, false); f.Advance(2000U);
    f.Edge(end, second, true); f.Edge(end, first, true); f.Advance(2000U);
    Check(f.board.QuickSettingsActive() && f.pairing.Presentation().cue == Pairing::PhysicalCue::Cleared &&
              f.persistence.saves == 0U,
          "a fresh repeated already-clear hold cannot fall through into menu Close");
}

void OfflineClear(uint8_t end, bool xFirst, bool rotated, virtual_litch_hardware hardware,
                  bool firstPlayerIsP2 = false) {
    Fixture f(Record::Active, rotated, hardware, firstPlayerIsP2);
    f.OpenClear(end);
    CheckClearAtOpeningEnd(f, end);
    Check(BluetoothGlyphVisible(f) && f.board.DisplayText().empty(),
          "raw Quick Settings entry and seven Right taps show Bluetooth without a fake numeric value");
    Checkpoint(f, "OfflineLocalClearEitherOrder", "Quick-Settings/item8", "Clear-prompt", "hold-both-2-seconds", "hold-both-2-seconds");
    f.Advance(9000U);
    Check(f.board.QuickSettingsActive(), "idle clear prompt remains selectable before ten seconds");
    Check(BluetoothGlyphVisible(f), "selected Bluetooth glyph remains visible beyond transient icon duration");
    const Button first = xFirst ? Button::X : Button::O;
    const Button second = xFirst ? Button::O : Button::X;
    f.Edge(end, first, true); f.Advance(300U); f.Edge(end, second, true);
    f.Advance(1000U);
    Check(f.pairing.Presentation().progress256 == 128U && f.persistence.saves == 0U,
          "one second of both held shows half progress without clearing");
    Check(f.board.QuickSettingsActive(), "active late-start hold cannot be cancelled by prior menu idle time");
    Checkpoint(f, "OfflineLocalClearEitherOrder", "late-start/both-held-1000ms", "Half-progress", "keep-holding", "keep-holding-both");
    f.Edge(end, first, false);
    Check(f.pairing.Presentation().progress256 == 0U && f.persistence.saves == 0U,
          "early release cancels progress and retains owner");
    Checkpoint(f, "OfflineLocalClearEitherOrder", "early-release", "Cancelled-progress", "retry-hold-both", "hold-both-2-seconds");
    f.Edge(end, first, true); f.Advance(2000U);
    Check(f.persistence.requested == Record::Clearing && f.persistence.saves == 1U,
          "fresh continuous two-second hold begins durable clear");
    f.Edge(end, first, false); f.Edge(end, second, false);
    CompleteLocalClear(f, "OfflineLocalClearEitherOrder");
}

void LocalClearSurvivesOwnerDisconnect(uint8_t end, bool xFirst, bool rotated,
                                       bool duringHold) {
    Fixture f(Record::Absent, rotated, VIRTUAL_LITCH_HARDWARE_V3_3, end == 1U);
    constexpr const char* journey = "LocalClearSurvivesOwnerDisconnect";
    Check(f.pairing.Begin({Action::Begin, 61U, end}, LINK), "clear regression starts with a live new owner claim");
    f.Edge(end, Button::X, true); f.Edge(end, Button::O, true);
    auto effect = f.pairing.Custody().PendingEffect();
    Check(f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session,
        effect.connection, effect.epoch, effect.physicalEnd,
        {{true, 1U}, true, true, true, true}) == Core::AckResult::AcceptedSuccess,
        "controlled checked native security completes the exact live claim");
    f.Save(); effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    f.Edge(end, Button::X, false); f.Edge(end, Button::O, false);
    Check(f.pairing.OwnerReady(LINK), "successful claim retains the live native owner and claim tuple");
    const unsigned savedBeforeClear = f.persistence.saves;
    f.OpenClear(end);
    CheckClearAtOpeningEnd(f, end);
    Checkpoint(f, journey, "raw-X-hold/release/seven-Right-taps", "Clear-prompt-at-opener",
               "hold-opening-end-both-2-seconds", "hold-both-2-seconds");
    const Button first = xFirst ? Button::X : Button::O;
    const Button second = xFirst ? Button::O : Button::X;
    if (duringHold) {
        f.Edge(end, first, true); f.Edge(end, second, true); f.Advance(800U);
    }
    // Native radio lifetime is controlled; all physical controls and menu
    // routing above/below use the production debounce and pairing runtime.
    f.pairing.Disconnected(LINK); f.Advance(1U);
    Check(f.board.QuickSettingsActive() && BluetoothGlyphVisible(f) &&
              f.pairing.Status().state == State::ClearPrompt,
          "native owner disconnect leaves Bluetooth selected and its local confirmation armed");
    CheckClearAtOpeningEnd(f, end);
    Checkpoint(f, journey, duringHold ? "owner-disconnect-during-800ms-hold" : "owner-disconnect-before-hold",
               "Local-clear-still-armed", duringHold ? "keep-holding-or-release-to-cancel" : "hold-both-2-seconds",
               duringHold ? "keep-holding-both" : "hold-both-2-seconds");
    if (f.pairing.Status().state != State::ClearPrompt) {
        f.Edge(end, Button::X, true);
        Check(f.board.QuickSettingsActive(), "X after owner disconnect supplies local consent instead of closing Bluetooth");
        return;
    }
    if (duringHold) {
        f.Edge(end, first, false); f.Advance(2000U);
        Check(f.persistence.saves == savedBeforeClear && f.pairing.Presentation().progress256 == 0U,
              "early physical release after disconnect cancels hold without clearing ownership");
        Checkpoint(f, journey, "early-release/remaining-button-held/2000ms", "Cancelled-no-clear",
                   "release-both-before-retry", "hold-both-2-seconds");
        f.Edge(end, first, true); f.Advance(2000U);
        Check(f.persistence.saves == savedBeforeClear && f.pairing.Presentation().progress256 == 0U,
              "repressing one button cannot restore used consent before both release");
        f.Edge(end, first, false); f.Edge(end, second, false);
    }
    f.Edge(end, first, true); f.Edge(end, second, true); f.Advance(1999U);
    Check(f.board.QuickSettingsActive() && f.persistence.saves == savedBeforeClear,
          "fresh offline X/O stays in Bluetooth and cannot clear before two seconds");
    f.Advance(1U);
    Check(f.persistence.requested == Record::Clearing && f.persistence.saves == savedBeforeClear + 1U,
          "fresh two-second physical hold clears without leaving Bluetooth after phone disconnect");
    Checkpoint(f, journey, "fresh-same-end-X+O-2000ms", "Accepted-clear-pending",
               "release-and-wait-for-native-and-durable-clear", "wait");
    f.Edge(end, first, false); f.Edge(end, second, false);
    CompleteLocalClear(f, journey);
}

// The original journey checked the end supplied by Presentation itself. Anchor
// this regression to the raw physical opener and change the mapping while the
// same menu stays open, as an ordinary Quick Settings adjustment can do.
void ClearStaysAtMenuOpeningEnd(uint8_t end, bool rotated, uint8_t changedItem) {
    Fixture f(Record::Active, rotated, VIRTUAL_LITCH_HARDWARE_V3_3, end == 1U);
    f.OpenMenu(end);
    for (uint8_t step = 0U; step < modules::quick_settings_items::TOTAL_COUNT &&
            f.board.QuickSettingsSelectedIndex() < changedItem; ++step)
        f.Tap(end, Button::Right);
    f.Tap(end, Button::Up);
    modules::ControlMutationOutcome outcome;
    Check(f.board.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "raw menu adjustment applies player-end or rotation change without closing Quick Settings");
    f.Advance(modules::QUICK_SETTINGS_VERTICAL_GUARD_MS);
    for (uint8_t step = 0U; step < modules::quick_settings_items::TOTAL_COUNT &&
            f.board.QuickSettingsSelectedIndex() < modules::quick_settings_items::OWNER_PAIRING; ++step)
        f.Tap(end, Button::Right);
    Check(f.board.QuickSettingsActive() && BluetoothGlyphVisible(f) &&
              f.pairing.Status().state == State::ClearPrompt,
          "the same open menu selects the production clear context after orientation changes");
    CheckClearAtOpeningEnd(f, end);
    std::printf("[OWNER_PAIRING_OPENING_END] hardware=V3.3 raw_opening_end=%u "
                "initial_rotate=%u changed_item=%u runtime_end=%u\n",
                end, rotated, changedItem, f.pairing.Status().physicalEnd);
    Checkpoint(f, "LocalClearStaysAtQuickMenuPhysicalEnd", "raw-X-hold/setting-change/item8",
               "Clear-prompt-at-opener", "hold-opening-end-both-2-seconds", "hold-both-2-seconds");
    const uint8_t other = 1U - end;
    f.Edge(other, Button::X, true); f.Edge(other, Button::O, true); f.Advance(2000U);
    Check(f.persistence.saves == 0U && f.pairing.Presentation().progress256 == 0U,
          "opposite-end two-second X/O cannot clear the opening player's owner record");
    f.Edge(other, Button::X, false); f.Edge(other, Button::O, false);
    Checkpoint(f, "LocalClearStaysAtQuickMenuPhysicalEnd", "opposite-end-X+O-2000ms",
               "Rejected-wrong-end", "use-menu-opening-end", "hold-both-2-seconds");
    f.Edge(end, Button::O, true); f.Edge(end, Button::X, true); f.Advance(1000U);
    Check(f.pairing.Presentation().progress256 == 128U && f.persistence.saves == 0U,
          "opening-end hold reaches proportional progress after rejected opposite-end input");
    f.Edge(end, Button::O, false);
    Check(f.pairing.Presentation().progress256 == 0U && f.persistence.saves == 0U,
          "opening-end early release cancels without losing ownership");
    Checkpoint(f, "LocalClearStaysAtQuickMenuPhysicalEnd", "opening-end-release-at-1000ms",
               "Cancelled-progress", "retry-opening-end-both", "hold-both-2-seconds");
    f.Edge(end, Button::O, true); f.Advance(2000U);
    Check(f.persistence.saves == 1U && f.persistence.requested == Record::Clearing,
          "retry from the original physical end admits exactly one durable clear");
    Checkpoint(f, "LocalClearStaysAtQuickMenuPhysicalEnd", "opening-end-retry-2000ms",
               "Accepted-clear-pending", "wait-for-native-and-durable-clear", "wait");
    f.Edge(end, Button::O, false); f.Edge(end, Button::X, false);
    CompleteLocalClear(f, "LocalClearStaysAtQuickMenuPhysicalEnd");
}

void CheckFailureReleasesLocalPlay(Fixture& f, const char* journey) {
    Check(!f.pairing.Presentation().active && !f.pairing.OwnerReady(LINK),
          "optional pairing failure grants no product access and owns no board presentation");
    Check(f.board.DisplayText() != "RS" && f.board.DisplayText() != "ST",
          "actual board frame contains no permanent restart takeover");
    const auto phase = f.pairing.Custody().GetPhase();
    const auto reason = f.pairing.Custody().GetReason();
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        f.Edge(end, Button::X, false); f.Edge(end, Button::O, false);
    }
    if (!f.board.QuickSettingsActive()) f.OpenMenu(0U);
    const auto selected = f.board.QuickSettingsSelectedIndex();
    f.Tap(0U, selected == 0U ? Button::Right : Button::Left);
    Check(f.board.QuickSettingsActive() && f.board.QuickSettingsSelectedIndex() != selected,
          "actual X hold and D-pad retain local Setup/menu control despite pairing failure");
    Check(f.pairing.Custody().GetPhase() == phase && f.pairing.Custody().GetReason() == reason &&
              !f.pairing.OwnerReady(LINK),
          "local play does not clear failure evidence or weaken app security");
    Checkpoint(f, journey, "raw-X-hold+D-pad", "Local-menu-usable/app-failure-retained",
               "continue-local-play/check-app", "ordinary-gameplay", false);
    f.Tap(0U, Button::X);
    Check(!f.board.QuickSettingsActive(), "local menu exits normally after pairing failure");
}

void StartupPairingFailureLeavesLocalPlay() {
    Fixture f(Record::Active);
    f.pairing.Boot(Record::Active, {}, true);
    f.Advance(1U);
    Check(f.pairing.Status().state == State::Failed &&
              f.pairing.Status().reason == Pairing::Reason::StorageFailed &&
              f.pairing.Status().flags == 5U,
          "unavailable startup inventory reproduces captured failed retained-owner identity tuple");
    CheckFailureReleasesLocalPlay(f, "StartupPairingFailureLeavesLocalPlay");
}

void NativeFailureRollbackCanRetry() {
    Fixture f;
    f.pairing.Begin({Action::Begin, 21U, 0U}, LINK);
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true);
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, false, false, true, true});
    f.Advance(1U);
    Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::Waiting,
          "native failure still being rolled back requests wait rather than premature restart");
    Checkpoint(f, "NativeFailureRollbackRetry", "security-failed/rollback-journal-pending",
               "Cleanup-in-progress", "wait-for-rollback", "wait");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    f.Save();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    Check(f.pairing.Custody().Pairable() &&
              f.pairing.Custody().GetReason() == Pairing::Reason::SecurityFailed &&
              f.pairing.Presentation().cue == Pairing::PhysicalCue::None &&
              !f.pairing.Presentation().active && f.board.DisplayText().empty() &&
              f.pairing.Custody().PendingEffect().type == Core::EffectType::None,
          "complete rollback retains diagnostic reason while clearing obsolete restart presentation and no success");
    Checkpoint(f, "NativeFailureRollbackRetry", "rollback/native-delete/durable-Absent",
               "Unpaired/retry-available", "retry-from-app", "ordinary-gameplay", false);
    auto reconnect = LINK;
    ++reconnect.epoch;
    ++reconnect.sessionToken;
    Check(f.pairing.Begin({Action::Begin, 22U, 0U}, reconnect),
          "successful rollback accepts a fresh reconnected app pairing attempt without restart");
    f.Advance(1U);
    Checkpoint(f, "NativeFailureRollbackRetry", "fresh-Begin-after-complete-rollback",
               "Await-physical", "press-both-blue-buttons", "press-both-blue-buttons");
}

void WholeSettingsResetPresentationThroughFinalReceipt() {
    for (const bool failed : {false,true}) {
        Fixture f(Record::Active, false, VIRTUAL_LITCH_HARDWARE_V3_3);
        constexpr const char* journey = "WholeSettingsResetFinalReceipt";
        Check(f.pairing.RegisterOwnerConnection(LINK,{{true,1U},true,true,true,true}) &&
                  f.pairing.RequestOwnerClear(LINK), "current owner admits whole settings reset");
        f.Advance(2000U);
        f.Save(); auto effect = f.pairing.Custody().PendingEffect();
        f.pairing.Disconnected(LINK);
        f.pairing.Custody().AcknowledgeQuiesce(effect.token,true);
        effect = f.pairing.Custody().PendingEffect();
        f.pairing.Custody().AcknowledgeBondDelete(effect.token,true); f.Save();
        effect = f.pairing.Custody().PendingEffect();
        f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
        Check(f.pairing.Custody().Pairable() && f.pairing.Status().state == State::Clearing &&
                  (f.pairing.Status().flags & modules::owner_pairing_session_wire::CanAddPhone) == 0U &&
                  f.pairing.Presentation().cue == Pairing::PhysicalCue::Waiting,
              "Absent does not offer pairing while replacement settings are still pending");
        Checkpoint(f,journey,"verified-Absent/replacement-settings-pending","Reset-still-pending",
                   "wait-for-settings-and-safe-backup","wait");
        Check(!f.pairing.Begin({Action::Begin, 81U, 2U}, LINK),
              "pending settings reset cannot admit another phone before both replacement receipts");
        f.Advance(10000U);
        Checkpoint(f,journey,"first-settings-receipt/fallback-still-pending","No-early-success",
                   "wait-for-final-receipt","wait");
        if (failed) {
            f.pairing.CompleteOwnerSettingsReset(false); f.Advance(1U);
            Check((f.pairing.Status().flags & modules::owner_pairing_session_wire::CanAddPhone) == 0U &&
                      !f.pairing.Begin({Action::Begin, 81U, 2U}, LINK),
                  "failed settings reset cannot offer or admit another phone before restart");
            CheckFailureReleasesLocalPlay(f,journey);
            Checkpoint(f,journey,"replacement-receipt-failed/BLE-revoked","Reset-incomplete/recovery-retained",
                       "check-app/restart-for-recovery","ordinary-gameplay",false);
        } else {
            f.pairing.CompleteOwnerSettingsReset(true);
            f.pairing.Boot(Record::Absent,{true,0U},true); f.Advance(1U);
            Checkpoint(f,journey,"both-settings-receipts-verified/reboot","Fresh-unpaired-boot",
                       "pair-from-app","ordinary-gameplay",false);
        }
    }
}

void NativeFailureRequiresRestart() {
    Fixture f;
    f.pairing.Begin({Action::Begin, 20U, 1U}, LINK);
    f.Edge(1U, Button::X, true); f.Edge(1U, Button::O, true);
    Checkpoint(f, "NativeFailureRestart", "fresh-X+O", "Consent-accepted/wait", "wait-native-bond", "wait");
    const auto firstO = ControlPixel(f, hardware::ControlLedRole::O);
    Check(firstO.g > 0U && firstO.r == 0U && firstO.b == 0U,
          "valid physical consent is confirmed locally before the native result");
    f.Advance(400U);
    Check(!f.pairing.Presentation().active &&
              Energy(ControlPixel(f, hardware::ControlLedRole::O)) == 0U &&
              Energy(ControlPixel(f, hardware::ControlLedRole::X)) == 0U,
          "native bonding stays pending without an endless white X/O processing bounce");
    auto effect = f.pairing.Custody().PendingEffect();
    Check(f.pairing.ClaimLeaseMatches(LINK, effect), "native failure is injected at the actual addressed ack seam");
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, false, false, true, true});
    f.Save();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, false);
    f.Advance(1U);
    Check(f.pairing.Custody().GetPhase() == Core::Phase::Failed,
          "native key cleanup failure leaves a current recovery obligation requiring restart");
    CheckFailureReleasesLocalPlay(f, "NativeFailureRestart");
    Check(f.pairing.Custody().PendingEffect().type == Core::EffectType::None &&
              !f.pairing.Custody().Pairable(),
          "failed native cleanup offers no success or fresh claim admission");
    f.Edge(1U, Button::X, false); f.Edge(1U, Button::O, false);
    Check(!f.board.QuickSettingsActive(), "native-error held release tail cannot enter normal menu/game actions");
    // Native restart seam: the fresh durable record/checked inventory enter
    // the same Boot owner; no radio, flash voltage or physical reboot is faked.
    f.pairing.Boot(f.persistence.durable, {true, 1U}, true);
    Check(f.board.Reset() == VIRTUAL_LITCH_OK, "native board restart resets ordinary presentation owners");
    f.board.ObserveHallMask(OCCUPANCY); f.Advance(200U);
    Check(f.board.DisplayText().empty() && !f.pairing.Custody().Pairable(),
          "restart clears stale error text while resuming durable Clearing");
    Checkpoint(f, "NativeFailureRestart", "restart/durable-Clearing+native-key-remains",
               "Recovery-in-progress", "wait", "wait");
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.Custody().Pairable() && !f.pairing.OwnerReady(LINK),
          "restarted cleanup returns unpaired without ever reporting claim success");
    Checkpoint(f, "NativeFailureRestart", "recovery/native-delete/durable-Absent",
               "Unpaired", "pair-from-app", "ordinary-gameplay", false);
}

void StorageFailureResumesClearingAfterRestart() {
    Fixture f(Record::Active);
    f.OpenClear(0U);
    f.Edge(0U, Button::O, true); f.Edge(0U, Button::X, true); f.Advance(2000U);
    Checkpoint(f, "StorageFailureRestart", "two-second-clear-consent", "Consent-accepted/wait", "wait-durable-clear", "wait");
    f.Save();
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true);
    f.persistence.outcome = Persistence::Outcome::Failed;
    f.Advance(1U);
    Check(f.persistence.durable == Record::Clearing && !f.pairing.Custody().Pairable() &&
              f.pairing.Custody().PendingEffect().type == Core::EffectType::None,
          "failed final absent receipt preserves durable clearing and never emits clear success");
    CheckFailureReleasesLocalPlay(f, "StorageFailureRestart");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    Check(f.board.UndoHistoryMoveCount() == 0U, "storage-error held tail cannot reset or mutate game history");
    f.pairing.Boot(f.persistence.durable, {true, 0U}, true);
    Check(f.board.Reset() == VIRTUAL_LITCH_OK, "restart resets text and menu before durable clearing recovery");
    f.board.ObserveHallMask(OCCUPANCY); f.Advance(200U);
    Check(f.board.DisplayText().empty() && !f.pairing.Custody().Pairable() &&
              f.pairing.Custody().PendingEffect().type == Core::EffectType::QuiesceAndRejectWork,
          "restart removes old error pin and resumes existing clearing journal before success");
    Checkpoint(f, "StorageFailureRestart", "restart/durable-Clearing+empty-native-inventory", "Recovery-in-progress", "wait", "wait", true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    effect = f.pairing.Custody().PendingEffect();
    Check(effect.type == Core::EffectType::UnpairSucceeded,
          "restarted clear succeeds only after new durable absent receipt");
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.Custody().Pairable() && f.board.DisplayText().empty(),
          "recovered clear returns unpaired with no stale error message");
    Checkpoint(f, "StorageFailureRestart", "recovery/native-clear/durable-Absent", "Unpaired", "pair-from-app", "ordinary-gameplay", false);
}

void LeavingContextPreservesNormalChord() {
    Fixture f(Record::Active);
    f.OpenClear(0U);
    f.Tap(0U, Button::Left);
    Check(!BluetoothGlyphVisible(f), "navigation replaces the pinned Bluetooth glyph with the selected item");
    Check(f.pairing.Custody().GetOperation() == Core::Operation::None &&
              f.persistence.saves == 0U,
          "leaving selected clear item cancels Armed unpair");
    f.Tap(0U, Button::X);
    Check(!f.board.QuickSettingsActive(), "X exits normal Quick Settings after clear context leave");
    Check(!BluetoothGlyphVisible(f), "menu exit leaves no pinned Bluetooth glyph behind");
    f.Advance(2200U);
    const uint64_t lifted = OCCUPANCY & ~(UINT64_C(1) << 52U);
    const uint64_t moved = lifted | (UINT64_C(1) << 36U);
    Check(f.board.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK &&
              f.board.ObserveHallMask(moved) == VIRTUAL_LITCH_OK,
          "physical e2-e4 enters the existing move production owner");
    f.Advance(2200U);
    Check(f.board.UndoHistoryMoveCount() == 1U, "physical move commits before ordinary New Game chord");
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true);
    // The production New Game coordinator acknowledges on subsequent ticks.
    for (unsigned tick = 0U; tick < 2100U; ++tick) f.Advance(1U);
    Check(f.persistence.saves == 0U && f.pairing.Custody().GetOwnership() == Core::Ownership::Owned,
          "normal X+O outside explicit context cannot clear owner");
    modules::ControlMutationOutcome outcome;
    Check(f.board.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::NewGame &&
              (outcome.status == modules::ControlMutationStatus::Accepted ||
               outcome.status == modules::ControlMutationStatus::Applied),
          "normal X+O still reaches existing production New Game action owner");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    Check(f.board.ObserveHallMask(OCCUPANCY) == VIRTUAL_LITCH_OK,
          "restoring pieces completes ordinary New Game setup");
    f.Advance(2200U);
    Checkpoint(f, "LeaveClearPreservesNormalXO", "leave-item8/ordinary-X+O", "Owner-retained",
               "ordinary-gameplay", "ordinary-gameplay", false);
}
uint8_t LargestRingDelta(const virtual_litch_physical_frame& first,
                         const virtual_litch_physical_frame& second) {
    uint8_t largest = 0U;
    for (uint8_t pixel = 0U; pixel < 32U; ++pixel) {
        const auto index = 256U + hardware::V3_3PhysicalIndexForSemanticStatus(pixel);
        const auto a = first.pixels[index], b = second.pixels[index];
        for (const auto difference : {std::abs(int(a.r) - int(b.r)),
                                     std::abs(int(a.g) - int(b.g)),
                                     std::abs(int(a.b) - int(b.b))})
            largest = std::max(largest, static_cast<uint8_t>(difference));
    }
    return largest;
}

void ClearHoldBlueLap(const Record record, const uint8_t end, const bool rotated) {
    Fixture f(record, rotated);
    constexpr const char* journey = "ClearHoldBlueLap";
    f.OpenClear(end);
    const auto idle = f.board.PhysicalFrame();
    Checkpoint(f, journey, "raw-X-hold/release/seven-Right-taps", "Clear-prompt",
               "hold-opening-end-both-2-seconds", "hold-both-2-seconds");
    f.Edge(end, Button::X, true); f.Edge(end, Button::O, true);
    uint32_t visited = 0U;
    bool onlyBlue = true;
    bool fourLedTail = true;
    for (uint32_t elapsed = 50U; elapsed <= 1950U; elapsed += 50U) {
        f.Advance(50U);
        if (elapsed < Pairing::PRESENTATION_FADE_MS) continue;
        unsigned blue = 0U;
        for (uint8_t pixel = 0U; pixel < 32U; ++pixel) {
            const auto color = f.board.PhysicalFrame().pixels[
                256U + hardware::V3_3PhysicalIndexForSemanticStatus(pixel)];
            onlyBlue &= color.r == 0U && (color.b == 0U || color.b > color.g);
            if (color.b > color.g) { visited |= UINT32_C(1) << pixel; ++blue; }
        }
        fourLedTail &= blue >= 4U && blue <= 5U;
        if (elapsed % 500U == 0U) {
            const uint8_t head = static_cast<uint8_t>(elapsed * 32U / Pairing::CLEAR_HOLD_MS);
            const auto physicalHead = hardware::V3_3PhysicalIndexForSemanticStatus(head);
            const auto next = hardware::V3_3PhysicalIndexForSemanticStatus((head + 1U) % 32U);
            Check(f.board.PhysicalFrame().pixels[256U + physicalHead].b > 0U &&
                      f.board.PhysicalFrame().pixels[256U + physicalHead].b >
                          f.board.PhysicalFrame().pixels[256U + next].b,
                  "raw hold advances one blue tail a quarter-lap per half-second regardless of opening bank or rotation");
            CheckClearAtOpeningEnd(f, end);
            Checkpoint(f, journey, "raw-X+O/quarter-lap", "Blue-only/keep-holding/no-effects",
                       "keep-holding", "keep-holding-both");
        }
    }
    Check(onlyBlue && fourLedTail && visited == UINT32_MAX && f.persistence.saves == 0U &&
              BluetoothGlyphVisible(f),
          "production hold visits all 32 status positions with one blue four-LED tail while preserving Bluetooth and ownership");
    f.Edge(end, Button::X, false);
    Check(f.pairing.Presentation().progress256 == 0U && f.persistence.saves == 0U &&
              LargestRingDelta(idle, f.board.PhysicalFrame()) == 0U,
          "early raw release cancels the blue lap and restores the underlying status frame without clearing");
    Checkpoint(f, journey, "raw-X-release-before-2s", "Cancelled-blue-lap/no-effects",
               "retry-hold-both", "hold-both-2-seconds");
    f.Edge(end, Button::X, true); f.Advance(1999U);
    const auto outgoing = f.board.PhysicalFrame();
    f.Advance(1U);
    Check(LargestRingDelta(outgoing, f.board.PhysicalFrame()) <= 4U,
          "the completed hold lap blends into pending or checked success without a dark cut");
    f.Edge(end, Button::X, false); f.Edge(end, Button::O, false);
    if (record == Record::Active) {
        Check(f.persistence.saves == 1U && !f.pairing.Custody().Pairable() &&
                  f.pairing.Presentation().cue == Pairing::PhysicalCue::Clearing,
              "owned hold enters honest pending cleanup before existing checked completion");
        CompleteLocalClear(f, journey);
    } else {
        Check(f.persistence.saves == 0U && f.pairing.Custody().Pairable() &&
                  f.pairing.Custody().PendingEffect().type == Core::EffectType::None &&
                  f.pairing.Presentation().cue == Pairing::PhysicalCue::Cleared,
              "already-clear hold enters the same existing completion animation with no native or durable effect");
        Checkpoint(f, journey, "full-2s-hold/checked-Absent", "Already-clear/success",
                   "pair-from-app", "pair-from-app");
        f.Advance(2000U);
        Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::None && f.board.QuickSettingsActive(),
              "already-clear completion expires independently without closing Bluetooth");
    }
    std::printf("[UNPAIR_BLUE_HOLD] journey=%s record=%s end=%u rotated=%u visited=%08lx blue_only=%u tail=4/adjacent-blend cancel=underlying-frame completion=existing-checked-episode\n",
                journey, record == Record::Active ? "Active" : "Absent", end, rotated,
                static_cast<unsigned long>(visited), onlyBlue);
}

void PerimeterAlphaPreservesGameFrame() {
    const auto same = [](const hardware::PhysicalLedFrame& a,
                         const hardware::PhysicalLedFrame& b) {
        return std::equal(a.begin(), a.end(), b.begin(), [](modules::Rgb left, modules::Rgb right) {
            return left.r == right.r && left.g == right.g && left.b == right.b;
        });
    };
    for (const auto revision : {hardware::BoardRevision::V3S3, hardware::BoardRevision::V3_3S3}) {
        const auto topology = hardware::BoardLedTopologyFor(revision);
        const uint16_t ring = topology.physicalBoardLedCount +
            (revision == hardware::BoardRevision::V3_3S3
                ? hardware::V3_3PhysicalIndexForSemanticStatus(3U) : 3U);
        hardware::PhysicalLedFrame game;
        game.fill({31U, 67U, 109U});
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay;
        overlay.pixels[3U] = {190U, 48U, 7U};
        auto unchanged = game;
        hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, unchanged);
        Check(same(unchanged, game) && overlay.perimeterAlpha255 == 255U,
              "a zero mask preserves the whole game frame and all old producers default opaque");
        overlay.mask = 1UL << 3U;
        for (const uint8_t alpha : {uint8_t{0U}, uint8_t{127U}, uint8_t{255U}}) {
            overlay.perimeterAlpha255 = alpha;
            auto actual = game;
            hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, actual, true);
            auto expected = game;
            expected[ring] = modules::AlphaBlendRgb(game[ring], overlay.pixels[3U], alpha);
            Check(same(actual, expected),
                  "ring alpha zero/full/intermediate maps only the addressed physical pixel and blends the exact game underneath");
        }
    }
}

void ControlAccentPreservesLowerLayers() {
    for (const auto revision : {hardware::BoardRevision::V3S3, hardware::BoardRevision::V3_3S3}) {
        for (const bool swapped : {false, true}) {
            for (const bool masked : {false, true}) {
                for (const uint8_t lowerAlpha : {uint8_t{0U}, uint8_t{127U}, uint8_t{255U}}) {
                    hardware::PhysicalLedFrame game;
                    game.fill({31U, 67U, 109U});
                    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay;
                    overlay.controlMask = masked ? 0xFFFU : 0U;
                    overlay.emulatedControlMask = masked ? 0xFFU : 0U;
                    overlay.controlPixels.fill({80U, 40U, 160U});
                    overlay.emulatedControlPixels.fill({80U, 40U, 160U});
                    overlay.controlAlpha255.fill(lowerAlpha);
                    overlay.controlAccentPixels.fill(modules::RGB_GREEN);
                    auto lower = game;
                    hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, lower, swapped);
                    Check(overlay.controlAccentMask == 0U && overlay.controlAccentAlpha255 == 0U,
                          "the upper control accent defaults empty for every existing producer");
                    overlay.controlAccentAlpha255 = 255U;
                    auto emptyAccent = game;
                    hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, emptyAccent, swapped);
                    Check(std::equal(emptyAccent.begin(), emptyAccent.end(), lower.begin(),
                              [](modules::Rgb a, modules::Rgb b) {
                                  return a.r == b.r && a.g == b.g && a.b == b.b;
                              }),
                          "an empty accent mask preserves the entire lower frame byte-for-byte even with a full alpha and populated colors");
                    overlay.controlAccentMask = (1U << 0U) | (1U << 11U);
                    for (const uint8_t alpha : {uint8_t{0U}, uint8_t{127U}, uint8_t{255U}}) {
                        overlay.controlAccentAlpha255 = alpha;
                        auto actual = game, expected = lower;
                        hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, actual, swapped);
                        for (const uint8_t index : {uint8_t{0U}, uint8_t{11U}}) {
                            const bool p2 = (index >= 6U) != swapped;
                            const auto role = static_cast<hardware::ControlLedRole>(index % 6U);
                            const uint8_t target = revision == hardware::BoardRevision::V3_3S3
                                ? hardware::V3_3PhysicalIndexForControl(p2, role)
                                : modules::PlayerHomeLedIndex(p2 ? modules::UiOwner::P2 : modules::UiOwner::P1,
                                      role == hardware::ControlLedRole::O ? 2U : 5U);
                            expected[256U + target] = modules::AlphaBlendRgb(
                                lower[256U + target], modules::RGB_GREEN, alpha);
                        }
                        const bool same = std::equal(actual.begin(), actual.end(), expected.begin(),
                            [](modules::Rgb a, modules::Rgb b) {
                                return a.r == b.r && a.g == b.g && a.b == b.b;
                            });
                        Check(same,
                              "accent zero/full/intermediate blends over masked lower alpha or unmasked game base, preserving every directional and unrelated pixel in both orientations");
                    }
                }
            }
        }
    }
    std::printf("[PAIRING_MAPPING] overlay_bytes=%zu empty_mask=unchanged alpha=0/127/255 lower=masked+unmasked bank_swap=both directions=unchanged\n",
                sizeof(modules::status_led_frame_adapter::FixedPhysicalStatusOverlay));
}

void ClearSecondaryCrossfadesAndRetires(const bool rotated) {
    Fixture f(Record::Active, rotated);
    f.OpenClear(0U);
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true); f.Advance(2000U);
    const uint32_t confirmed = f.board.State().now_ms;
    if (!rotated) PreviewStatusFrame(f, "clear-pending");
    f.Edge(0U, Button::X, false); f.Edge(0U, Button::O, false);
    while (f.board.State().now_ms - confirmed < 200U) {
        f.Advance(20U);
        if (!rotated) PreviewStatusFrame(f, "clear-pending");
    }
    bool blue = false, orange = false, purple = false;
    for (unsigned sample = 0U; sample <= 80U; ++sample) {
        if (sample != 0U) f.Advance(20U);
        if (!rotated) PreviewStatusFrame(f, "clear-pending");
        for (uint8_t pixel = 0U; pixel < 32U; ++pixel) {
            const auto color = f.board.PhysicalFrame().pixels[
                256U + hardware::V3_3PhysicalIndexForSemanticStatus(pixel)];
            blue |= color.r == 0U && color.b > color.g && color.b > 0U;
            orange |= color.r > color.b * 2U && color.g > 0U;
            purple |= color.r > color.g * 2U && color.b > color.g * 2U;
        }
    }
    Check(blue && orange && purple && !f.pairing.Custody().Pairable(),
          "pending clear retains blue while its secondary tail crosses orange and purple without claiming success");
    f.Advance(1999U - (f.board.State().now_ms - confirmed));
    const auto outgoing = f.board.PhysicalFrame();
    if (!rotated) PreviewStatusFrame(f, "clear-to-wait");
    f.Advance(1U);
    if (!rotated) PreviewStatusFrame(f, "clear-to-wait");
    // The old 3-level bound described the now-retired fade into black. A live
    // chase advances one 16 ms adjacent-pixel blend, plus its secondary hue.
    constexpr uint8_t chaseStepPerMs = (255U + 15U) / 16U + 2U;
    Check(f.pairing.Presentation().cue == Pairing::PhysicalCue::Clearing &&
              !f.pairing.Custody().Pairable() &&
              LargestRingDelta(outgoing, f.board.PhysicalFrame()) <= chaseStepPerMs,
          "pending clear continues smoothly past two seconds without asking for new consent");
    f.Save();
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
    const auto waiting = f.board.PhysicalFrame();
    effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay retiring;
    f.pairing.ComposePhysicalCue(retiring);
    std::printf("[PAIRING_CLEAR_SUCCESS] active=%u pairable=%u ring_mask=%08lx alpha=%u max_pixel_step=%u next=pair-from-app\n",
                f.pairing.Presentation().active, f.pairing.Custody().Pairable(),
                static_cast<unsigned long>(retiring.mask), retiring.perimeterAlpha255,
                LargestRingDelta(waiting, f.board.PhysicalFrame()));
    Check(f.pairing.Presentation().active && f.pairing.Custody().Pairable() &&
              retiring.mask == UINT32_MAX &&
              LargestRingDelta(waiting, f.board.PhysicalFrame()) <= chaseStepPerMs,
          "checked late completion starts success with a smooth transition from the pending chase");
    for (unsigned step = 0U; step < 10U; ++step) {
        f.Advance(20U);
        if (!rotated) PreviewStatusFrame(f, "clear-late-completion");
    }
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay retired;
    f.pairing.ComposePhysicalCue(retired);
    Check(retired.mask == UINT32_MAX,
          "late completion retains its own success episode after the pending phase");
    f.Advance(1800U);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay expired;
    f.pairing.ComposePhysicalCue(expired);
    Check(expired.mask == 0U && !f.pairing.Presentation().active,
          "success releases the ordinary ring two seconds after completion");
    std::printf("[PAIRING_CLEAR_CROSSFADE] blue=%u orange=%u purple=%u success_budget_ms=2000 late_completion=checked next=ordinary-gameplay\n",
                blue, orange, purple);
}

void AcceptedConsentTurnsFourControlsGreen(const bool rotated, const uint8_t end) {
    Fixture f(Record::Absent, rotated);
    const auto green = [&] {
        bool all = true;
        for (uint8_t bank = 0U; bank < 2U; ++bank)
            for (const auto role : {hardware::ControlLedRole::O, hardware::ControlLedRole::X}) {
                const auto c = ControlPixel(f, bank, role);
                all &= c.g > 0U && c.r == 0U && c.b == 0U;
            }
        return all;
    };
    Check(f.pairing.Begin({Action::Begin, 92U, modules::owner_pairing_session_wire::EITHER_END}, LINK),
          "either-end consent needs no extra board action before the same-end X/O press");
    f.Advance(1U);
    for (uint8_t bank = 0U; bank < 2U; ++bank)
        for (const auto role : {hardware::ControlLedRole::O, hardware::ControlLedRole::X}) {
            const auto c = ControlPixel(f, bank, role);
            Check(c.r == 0U && c.g > 0U && c.b > c.g,
                  "the unchanged normal either-end claim lights all four X/O steadily blue");
        }
    if (!rotated && end == 0U) PreviewStatusFrame(f, "consent-before");
    auto stale = f.pairing.ReadPendingConsent();
    ++stale.prompt; stale.physicalEnd = end;
    Check(!f.pairing.ConfirmPendingConsent(stale) && !green() && f.persistence.saves == 0U,
          "a stale queued confirmation never turns any consent lights green");
    f.Edge(end, Button::O, true); f.Edge(end, Button::X, true);
    Check(green() && f.pairing.Custody().PendingEffect().type == Core::EffectType::RequestSecurity &&
              !f.pairing.OwnerReady(LINK) && f.persistence.saves == 0U,
          "accepted raw same-end consent immediately turns all four X/O green while security and durability are still pending");
    if (!rotated && end == 0U) PreviewStatusFrame(f, "consent-confirmed");
    uint16_t previousGreen = ControlPixel(f, 0U, hardware::ControlLedRole::O).g;
    for (unsigned sample = 0U; sample < 50U; ++sample) {
        f.Advance(20U);
        if (!rotated && end == 0U) PreviewStatusFrame(f, "consent-confirmed");
        Check(!f.pairing.OwnerReady(LINK),
              "the green fade never grants owner access or completes the native operation");
        const auto color = ControlPixel(f, 0U, hardware::ControlLedRole::O);
        if (sample < 10U) {
            Check(color.g <= previousGreen,
                  "mapped green fades monotonically toward the unchanged lower control output");
            previousGreen = color.g;
        } else {
            Check(!f.pairing.Presentation().active && !green(),
                  "pending security never restarts green or white bouncing controls after the acknowledgement expires");
        }
    }
    Check(!f.pairing.Presentation().active && f.pairing.Presentation().nextAction == std::string_view("wait"),
          "after the bounded acknowledgement native work stays pending without an endless button request");
    f.Edge(end, Button::O, false); f.Edge(end, Button::X, false);
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, true, true, true, true});
    f.Save(); effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.OwnerReady(LINK) && !f.pairing.Presentation().active,
          "only checked security and the actual owner save admit readiness without replaying green");
    std::printf("[PAIRING_CONFIRMED_GREEN] rotate=%u end=%u ack_ms=200 native_and_save=pending_during_ack next=wait-without-new-input\n", rotated, end);
}

void ConsentAcknowledgmentDoesNotDelayOrReplay() {
    Fixture f;
    Check(f.pairing.Begin({Action::Begin, 93U, modules::owner_pairing_session_wire::EITHER_END}, LINK),
          "fresh normal claim starts the rapid-completion journey");
    auto accepted = f.pairing.ReadPendingConsent();
    accepted.physicalEnd = 1U;
    f.Edge(1U, Button::X, true); f.Edge(1U, Button::O, true);
    auto effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
        effect.epoch, effect.physicalEnd, {{true, 1U}, true, true, true, true});
    f.Save(); effect = f.pairing.Custody().PendingEffect();
    f.pairing.Custody().AcknowledgeSuccess(effect.token); f.Advance(1U);
    Check(f.pairing.OwnerReady(LINK) && f.pairing.Presentation().cue == Pairing::PhysicalCue::Confirmed &&
              f.pairing.Presentation().nextAction == std::string_view("ordinary-gameplay"),
          "checked security and durable proof grant readiness during the cosmetic green fade without waiting for it");
    f.pairing.Retire(LINK); f.Advance(1U);
    Check(!f.pairing.Presentation().active && !f.pairing.ConfirmPendingConsent(accepted),
          "retiring the exact session removes green immediately and a delayed consent cannot replay it");
    std::printf("[PAIRING_CONFIRMED_RAPID] native_and_save=checked_before_fade_end ready=immediate retire=no-green-replay\n");
}

struct ConsentExpiryClock {
    inline static uint32_t boundary = 0U;
    inline static uint32_t crossing = 0U;
    inline static unsigned reads = 0U;
    static uint32_t Read() { return boundary + (reads++ < 2U ? 0U : crossing); }
};

void ConsentFadeCannotFlashWhenClockCrossesExpiry() {
    Fixture f;
    Check(f.pairing.Begin({Action::Begin, 94U, modules::owner_pairing_session_wire::EITHER_END}, LINK),
          "expiry-crossing journey starts an ordinary claim");
    f.Edge(0U, Button::X, true); f.Edge(0U, Button::O, true);
    for (const uint32_t crossing : {1U, 2U}) {
        ConsentExpiryClock::boundary = f.board.State().now_ms + Pairing::PRESENTATION_FADE_MS - 1U;
        ConsentExpiryClock::crossing = crossing;
        ConsentExpiryClock::reads = 0U;
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay;
        {
            fxe::ScopedMillisProviderOverride clock(&ConsentExpiryClock::Read);
            f.pairing.ComposePhysicalCue(overlay);
        }
        hardware::PhysicalLedFrame underlying;
        underlying.fill({17U, 41U, 83U});
        auto actual = underlying;
        hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3, overlay, actual);
        Check(overlay.controlAccentAlpha255 == 0U &&
                  std::equal(actual.begin(), actual.end(), underlying.begin(),
                      [](modules::Rgb a, modules::Rgb b) {
                          return a.r == b.r && a.g == b.g && a.b == b.b;
                      }),
              "a clock crossing from 199 to 200/201 ms during one compose cannot wrap into a full-green frame");
        Check(!f.pairing.OwnerReady(LINK) && f.persistence.saves == 0U,
              "crossing the cosmetic fade deadline grants no native or durable authority");
        std::printf("[PAIRING_EXPIRY_CLOCK] decision_age=199 compose_age=%u reads=%u alpha=%u mapped=underlying-required\n",
                    199U + crossing, ConsentExpiryClock::reads, overlay.controlAccentAlpha255);
    }
}

} // namespace
int main() {
    PerimeterAlphaPreservesGameFrame();
    ControlAccentPreservesLowerLayers();
    ConsentAcknowledgmentDoesNotDelayOrReplay();
    ConsentFadeCannotFlashWhenClockCrossesExpiry();
    for (const bool rotated : {false, true}) {
        for (uint8_t end : {uint8_t{0U}, uint8_t{1U}})
            AcceptedConsentTurnsFourControlsGreen(rotated, end);
        ClearSecondaryCrossfadesAndRetires(rotated);
    }
    StartupPairingFailureLeavesLocalPlay();
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        for (bool rotated : {false, true}) {
            for (const auto record : {Record::Active, Record::Absent})
                ClearHoldBlueLap(record, end, rotated);
            for (const auto hardware : {VIRTUAL_LITCH_HARDWARE_V3, VIRTUAL_LITCH_HARDWARE_V3_3})
                ClaimSuccess(end, rotated, hardware);
            for (bool xFirst : {false, true}) {
                AlreadyClearStaysInBluetooth(end, xFirst, rotated);
                for (const auto hardware : {VIRTUAL_LITCH_HARDWARE_V3, VIRTUAL_LITCH_HARDWARE_V3_3}) {
                    OfflineClear(end, xFirst, rotated, hardware);
                }
                for (const bool duringHold : {false, true})
                    LocalClearSurvivesOwnerDisconnect(end, xFirst, rotated, duringHold);
            }
        }
    }
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        for (bool rotated : {false, true}) {
            OfflineClear(end, end == 0U, rotated, VIRTUAL_LITCH_HARDWARE_V3_3, true);
            for (uint8_t item : {modules::quick_settings_items::FIRST_PLAYER,
                                 modules::quick_settings_items::ROTATE})
                ClearStaysAtMenuOpeningEnd(end, rotated, item);
        }
    }
    ClearFeedbackDeadlineWithDelayedNativeCompletion();
    InvalidConsentAndRecovery();
    EitherEndInvalidInputRecovery();
    AddSavedPhonesJourney();
    PrivateUsbConsentJourney();
    NativeFailureRollbackCanRetry();
    WholeSettingsResetPresentationThroughFinalReceipt();
    NativeFailureRequiresRestart();
    StorageFailureResumesClearingAfterRestart();
    LeavingContextPreservesNormalChord();
    std::printf("[OWNER_PAIRING_JOURNEYS] failures=%d seam=physical-level/debounce/production-pairing/"
                "game-arbitration/profile-control-LED/power-output bypass=native-radio,NVS-receipt-fault-adapter,"
                "contact-voltage,bounded-Hint-budget physical-timing-legibility=PENDING\n", failures);
    return failures == 0 ? 0 : 1;
}
