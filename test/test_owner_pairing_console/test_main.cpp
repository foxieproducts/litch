#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/owner_pairing_console_commands.hpp"
#include "owner_pairing_console_test_transport.hpp"

namespace {
using Source = fxe::ConsoleInputSource;
using Runtime = modules::BoardOwnerPairingRuntime;
using Controller = Runtime::Controller;
using Record = modules::BoardOwnerRecord;
using Action = modules::owner_pairing_session_wire::Action;
constexpr Runtime::Connection LINK{4U, 5U, 6U};
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::printf("[OWNERPAIR_CONSOLE_FAIL] %s\n", message); }
}
struct Persistence final : modules::BoardOwnerPersistence {
    fxe::SettingsStore settings{{}};
    unsigned saves = 0U;
    Receipt active = 0U, next = 0U;
    Record requested = Record::Invalid;
    Outcome outcome = Outcome::Pending;
    bool releaseAllowed = true;
    bool SaveOwnerRecord(Record record, Receipt& receipt) override {
        if (active != 0U || !modules::WriteBoardOwner(settings.Doc(), record)) return false;
        ++saves; requested = record; outcome = Outcome::Pending;
        receipt = active = ++next;
        return true;
    }
    Outcome GetOutcome(Receipt receipt) const override {
        return receipt != 0U && receipt == active ? outcome : Outcome::Unknown;
    }
    bool Release(Receipt receipt) override {
        if (!releaseAllowed || receipt == 0U || receipt != active || outcome == Outcome::Pending) return false;
        active = 0U; return true;
    }
};
struct Fixture {
    Persistence persistence;
    Runtime runtime{persistence};
    test_support::OwnerPairingConsoleTransport transport;
    Console console{transport.hybrid, {false}};
    Fixture(Record record = Record::Absent, uint8_t saved = 3U) {
        fxe::ManualMillis::SetNowMs(100U);
        auto& doc = persistence.settings.Doc();
        doc["GAME_FEN"] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        doc["BRIGHTNESS"] = 7U;
        doc["ARTWORK_SENTINEL"] = "retained artwork bytes";
        doc["LED_BROWNOUT_LIMIT_LATCH"] = true;
        modules::BoardIdentity identity; identity.bytes.fill(0x42U);
        Check(modules::WriteBoardIdentity(doc, identity) && modules::WriteBoardOwner(doc, record) &&
                  modules::PrepareBoardOwnerJournal(doc), "fixture prepares real identity and ownership JSON");
        runtime.Boot(record, {true, uint8_t(record == Record::Active ? saved : 0U)}, true);
        console.SetSettingsStore(&persistence.settings);
    }
    void Usb(const std::string& line) { transport.usb.input += line; console.Update(); }
    void Ble(const std::string& line) { transport.ble.input += line; console.Update(); }
    void Arm() { Check(runtime.Begin({Action::Begin, 31U, 1U}, LINK), "exact live shared-session claim arms"); }
    void Save() {
        persistence.outcome = Persistence::Outcome::Saved;
        runtime.UpdateDurableState(persistence.requested, true); runtime.Poll();
    }
    std::string RetainedJson() const {
        StaticJsonDocument<4096U> retained;
        Check(retained.set(persistence.settings.Doc()), "retained JSON copy fits");
        retained.remove(modules::KEY_BOARD_OWNER_V1);
        std::string encoded; serializeJson(retained, encoded); return encoded;
    }
};

void TestPrivateUsbPairingsOnlyClear() {
    Fixture f(Record::Active);
    modules::RegisterOwnerPairingConsoleCommands(f.console, f.runtime);
    const auto retained = f.RetainedJson();
    f.Usb("ownerpair clear\n");
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) {
        Check(f.persistence.saves == 0U && f.runtime.Custody().SavedPhoneCount() == 3U &&
                  f.RetainedJson() == retained, "normal builds omit the private pairing-clear route");
        return;
    }
    Check(f.persistence.saves == 1U && f.persistence.requested == Record::Clearing &&
              !f.runtime.Custody().Pairable() &&
              f.runtime.Custody().PendingEffect().type == Controller::EffectType::None,
          "fresh USB clear admits only the Clearing receipt, never premature native work or Absent");
    if (f.persistence.saves != 1U) return;
    f.Usb("ownerpair clear\n");
    fxe::ManualMillis::AdvanceMs(2000U); f.runtime.Poll();
    Check(f.persistence.saves == 1U && f.runtime.Presentation().cue == Runtime::PhysicalCue::Waiting,
          "duplicate clear cannot repeat work; blue/amber expires while persistence remains pending");
    f.persistence.releaseAllowed = false; f.Save();
    Check(f.runtime.Custody().PendingEffect().type == Controller::EffectType::None,
          "saved Clearing still requires exact receipt release before native cleanup");
    f.persistence.releaseAllowed = true; f.runtime.Poll();
    auto effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Controller::EffectType::QuiesceAndRejectWork, "durable Clearing requests native quiescence");
    f.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Controller::EffectType::DeleteAllBonds && !f.runtime.Custody().IsAdding(),
          "private clear selects existing all-peer cleanup, never targeted Add rollback");
    f.runtime.Custody().AcknowledgeBondDelete(effect.token, true);
    Check(f.persistence.requested == Record::Absent && !f.runtime.Custody().Pairable(),
          "checked native-empty acknowledgement admits Absent without claiming durability");
    f.Save(); effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Controller::EffectType::UnpairSucceeded, "verified Absent awaits existing completion acknowledgement");
    f.runtime.Custody().AcknowledgeSuccess(effect.token); f.runtime.Poll();
    f.Usb("ownerpair status\n");
    Check(f.runtime.Status().state == Runtime::State::Unpaired && f.runtime.Custody().Pairable() &&
              f.runtime.Custody().SavedPhoneCount() == 0U && f.persistence.saves == 2U &&
              !f.runtime.Presentation().active && f.RetainedJson() == retained,
          "only pairing is cleared after both receipts; game, preferences, artwork, Board ID and safety remain exact");
    Check(f.transport.usb.output.find("pairing clear accepted; cleanup pending") != std::string::npos &&
              f.transport.usb.output.find("state=1 reason=0") != std::string::npos,
          "Console distinguishes request admission from status-proven completion");
    std::printf("[PRIVATE_USB_PAIRING_CLEAR] native=DeleteAllBonds receipts=%u state=%u saved_phones=%u retained_json=%u\n",
                f.persistence.saves, unsigned(f.runtime.Status().state),
                unsigned(f.runtime.Custody().SavedPhoneCount()), unsigned(f.RetainedJson() == retained));
}

void TestPrivateClearSourceAndBusyBoundaries() {
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) return;
    Fixture sources(Record::Active);
    modules::RegisterOwnerPairingConsoleCommands(sources.console, sources.runtime);
    sources.Ble("ownerpair clear\n");
    sources.Ble("ownerpair "); sources.Usb("clear\n");
    sources.Usb("ownerpair "); sources.Ble("clear\n");
    sources.Usb("\x1b[A\n"); // Recall the previous rejected clear from shared history.
    MemoryConsoleTransport unknown;
    Console programmatic(unknown, {false});
    modules::RegisterOwnerPairingConsoleCommands(programmatic, sources.runtime);
    unknown.PushBytes("ownerpair clear\n", 16U); programmatic.Update();
    Check(sources.persistence.saves == 0U && sources.runtime.Custody().SavedPhoneCount() == 3U,
          "BLE, both mixed directions, history and unknown/programmatic input cannot clear pairings");
    for (unsigned mode = 0U; mode < 7U; ++mode) {
        Fixture f(Record::Active);
        modules::RegisterOwnerPairingConsoleCommands(f.console, f.runtime);
        if (mode == 0U) {
            f.runtime.Boot(Record::Absent, {true,0U}, true); f.Arm();
        } else if (mode == 1U) {
            f.runtime.Boot(Record::Active, {true,1U}, true);
            modules::BoardOwnerAddBaseline baseline{};
            baseline.saved.count = 1U; baseline.saved.peers[0U].address[0U] = 1U;
            Check(f.runtime.Begin({Action::Begin,32U,2U},LINK,baseline), "busy Add prompt begins");
        } else if (mode == 2U) f.runtime.Boot(Record::Active, {false,0U}, true);
        else if (mode == 3U) f.runtime.Boot(Record::Active, {true,3U}, false);
        else if (mode == 4U) {
            Check(f.runtime.RegisterOwnerConnection(LINK,{{true,3U},true,true,true,true}) &&
                      f.runtime.RequestOwnerClear(LINK), "whole reset already owns its receipt");
        } else if (mode == 5U) f.runtime.CompleteOwnerSettingsReset(false);
        else Check(f.runtime.SelectLocalClear(33U,0U), "existing physical clear prompt is busy");
        const auto before = f.persistence.saves;
        f.Usb("ownerpair clear\n");
        Check(f.persistence.saves == before, "private clear cannot steal claim, Add, unknown, reset or physical-clear ownership");
    }
    Fixture failed(Record::Active);
    modules::RegisterOwnerPairingConsoleCommands(failed.console, failed.runtime);
    failed.Usb("ownerpair clear\n"); failed.Save();
    auto effect = failed.runtime.Custody().PendingEffect();
    failed.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = failed.runtime.Custody().PendingEffect();
    failed.runtime.Custody().AcknowledgeBondDelete(effect.token, false); failed.runtime.Poll();
    failed.Usb("ownerpair clear\n");
    Check(failed.persistence.saves == 1U && failed.persistence.requested == Record::Clearing &&
              failed.runtime.Status().state == Runtime::State::Failed &&
              failed.runtime.Presentation().cue == Runtime::PhysicalCue::Restart &&
              !failed.runtime.Custody().Pairable(),
          "failed native cleanup remains restart-required; private clear cannot widen recovery admission");
}

void TestWholeEditedLineProvenance() {
    Fixture f;
    Source observed = Source::Unknown;
    unsigned calls = 0U;
    f.console.RegisterCommand("probe", "", [&](const Console::Args&) {
        observed = f.console.CommandInputSource(); ++calls; return true;
    });
    f.Usb("probe\r\n");
    Check(calls == 1U && observed == Source::Local && f.console.CommandInputSource() == Source::Unknown,
          "USB CRLF executes once; source exists only inside handler");
    f.Ble("probe\n");
    Check(observed == Source::Remote, "ignored USB LF does not contaminate next BLE line");
    f.Ble("pr"); f.Usb("obe\n");
    Check(observed == Source::Mixed, "BLE prefix plus USB enter cannot become local consent");
    f.Usb("pr"); f.Ble("obe\n");
    Check(observed == Source::Mixed, "USB prefix plus BLE enter cannot become local consent");
    f.Ble("x"); f.Usb("\bprobe\n");
    Check(observed == Source::Mixed, "backspace does not erase remote provenance");
    f.Ble("x"); f.Usb("\x15probe\n");
    Check(observed == Source::Mixed, "Ctrl-U does not erase remote provenance");
    f.Ble("\x1b["); f.Usb("A\n");
    Check(observed == Source::Mixed, "history recall and enter retain entire escape provenance");
    f.Ble("probe\n"); f.Usb("\x1b[A\n");
    Check(observed != Source::Local, "shared remote history cannot gain Local authority on USB recall");
    f.Ble("pr"); f.Usb("\x1b[A\x1b[B\b\bprobe\n");
    Check(observed != Source::Local, "mixed scratch recall and backspace cannot gain Local authority");
    f.Usb("probe\n");
    Check(observed == Source::Local, "fresh submitted line recovers local provenance");

    MemoryConsoleTransport unknown;
    Console unknownConsole(unknown, {false});
    Source unidentified = Source::Local;
    unknownConsole.RegisterCommand("probe", "", [&](const Console::Args&) {
        unidentified = unknownConsole.CommandInputSource(); return true;
    });
    unknown.PushBytes("probe\n", 6U); unknownConsole.Update();
    Check(unidentified == Source::Unknown, "unidentified transport never inherits Local");
}

void TestBuildProfile() {
#if (defined(LITCH_OWNER_PAIRING) && LITCH_OWNER_PAIRING) || \
    (defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW)
    Check(modules::OWNER_PAIRING_ENABLED, "explicit public or preview profile enables the existing runtime");
#else
    Check(!modules::OWNER_PAIRING_ENABLED, "unselected profile leaves owner pairing disabled");
#endif
#if defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW
    Check(modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED, "private preview retains developer USB consent");
#else
    Check(!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED, "public and default profiles exclude developer USB consent");
#endif
}

void TestEitherEndUsbExample() {
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) return;
    Fixture f;
    modules::RegisterOwnerPairingConsoleCommands(f.console, f.runtime);
    Check(f.runtime.Begin({Action::Begin, 31U, 2U}, LINK), "either-end USB prompt opens");
    f.Usb("ownerpair status\n");
    Check(f.transport.usb.output.find("ownerpair consent 31 0 4 5 6 1") != std::string::npos,
          "unselected prompt prints a valid actual-bank example");
    f.Usb("ownerpair consent 31 2 4 5 6 1\n");
    Check(f.runtime.Custody().GetPhase() == Controller::Phase::Armed,
          "USB consent cannot remain at an unselected bank");
    f.Usb("ownerpair consent 31 1 4 5 6 1\n");
    Check(f.runtime.Custody().PendingEffect().physicalEnd == 1U &&
              f.runtime.Custody().GetPhase() == Controller::Phase::AwaitSecurity,
          "developer may choose the other bank on the exact either-end lease");
}

void TestConsentRegistrationAndBoundaries() {
    Fixture f;
    const bool registered = modules::RegisterOwnerPairingConsoleCommands(f.console, f.runtime);
    Check(registered == modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED,
          "only private preview registers ownerpair command");
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) {
        f.Arm(); f.Usb("ownerpair consent 31 1 4 5 6 1\n");
        Check(f.runtime.Custody().GetPhase() == Controller::Phase::Armed && f.persistence.saves == 0U,
              "public and default profiles cannot confirm even an injected armed runtime");
        return;
    }
    f.Usb("ownerpair consent 31 1 4 5 6 1\n");
    Check(f.runtime.Custody().Pairable(), "absent prompt cannot invent consent");
    f.Arm(); f.Usb("ownerpair status\n");
    Check(f.transport.usb.output.find("attempt=31 end=1") != std::string::npos,
          "USB status exposes only bounded state and exact attempt/end");
    f.Ble("ownerpair consent 31 1 4 5 6 1\n");
    f.Ble("ownerpair consent "); f.Usb("31 1 4 5 6 1\n");
    for (const char* malformed : {"0 1 4 5 6 1", "65536 1 4 5 6 1", "-1 1 4 5 6 1", "31 2 4 5 6 1", "31 1 4 5 6 1 extra", "30 1 4 5 6 1", "31 0 4 5 6 1", "31 1 4 4294967296 6 1", "31 1 4 5 6 0"})
        f.Usb(std::string("ownerpair consent ") + malformed + "\n");
    Check(f.runtime.Custody().GetPhase() == Controller::Phase::Armed && f.persistence.saves == 0U,
          "remote mixed malformed stale and wrong-end commands leave claim untouched");
    f.Usb("ownerpair consent 31 1 4 5 6 1\n");
    const auto effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Controller::EffectType::RequestSecurity && f.runtime.ClaimLeaseMatches(LINK, effect) &&
              f.persistence.saves == 0U && !f.runtime.OwnerReady(LINK),
          "exact USB consent requests ordinary native security without native/save acknowledgement");
    f.Usb("ownerpair consent 31 1 4 5 6 1\n");
    Check(f.runtime.Custody().PendingEffect().token == effect.token && f.persistence.saves == 0U,
          "duplicate consent cannot repeat security or create an owner save");
    for (const bool disconnect : {false, true}) {
        Fixture retired;
        modules::RegisterOwnerPairingConsoleCommands(retired.console, retired.runtime);
        retired.Arm();
        if (disconnect) retired.runtime.Disconnected(LINK);
        else fxe::ManualMillis::AdvanceMs(Runtime::CLAIM_WINDOW_MS);
        retired.Usb("ownerpair consent 31 1 4 5 6 1\n");
        Check(retired.runtime.Custody().Pairable() && retired.persistence.saves == 0U,
              "expired or disconnected prompt cannot consent");
    }
}

void TestProgrammaticCommandIsUnknown() {
    test_support::OwnerPairingConsoleTransport transport;
    Console console(transport.hybrid);
    fxe::SettingsStore settings({});
    console.SetSettingsStore(&settings);
    Source observed = Source::Local;
    unsigned calls = 0U;
    console.RegisterCommand("reboot", "", [&](const Console::Args&) {
        observed = console.CommandInputSource(); ++calls; return true;
    });
    // Use the Console's real delayed programmatic dispatch, without invoking
    // settings.Update (no filesystem write) or a physical reboot.
    transport.usb.input += "settings paste\n";
    console.Update();
    transport.usb.input += "TEST_LOCAL_INPUT=1\n\x04";
    console.Update();
    fxe::ManualMillis::AdvanceMs(10000U);
    console.Update();
    Check(calls == 1U && observed == Source::Unknown,
          "deferred programmatic dispatch cannot inherit prior USB source");
}

void TestReusedAttemptCannotAuthorizeAnotherPrompt() {
    if (!modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED) return;
    for (const bool replacement : {false, true}) {
        Fixture f;
        modules::RegisterOwnerPairingConsoleCommands(f.console, f.runtime);
        f.Arm();
        if (replacement) f.runtime.Disconnected(LINK);
        else Check(f.runtime.Cancel({Action::Cancel, 31U, 1U}, LINK), "first prompt cancels");
        auto next = LINK;
        if (replacement) { ++next.epoch; ++next.sessionToken; }
        Check(f.runtime.Begin({Action::Begin, 31U, 1U}, next), "client reuses attempt on another prompt");
        f.Usb("ownerpair consent 31 1 4 5 6 1\n");
        Check(f.runtime.Custody().GetPhase() == Controller::Phase::Armed && f.persistence.saves == 0U,
              "old USB command cannot authorize replacement session or reused same-session attempt");
        const auto pending = f.runtime.ReadPendingConsent();
        const auto line = std::string("ownerpair consent 31 1 4 ") +
            std::to_string(next.epoch) + " " + std::to_string(next.sessionToken) + " " +
            std::to_string(pending.prompt) + "\n";
        f.Usb(line);
        Check(f.runtime.Custody().PendingEffect().type == Controller::EffectType::RequestSecurity &&
                  f.runtime.ClaimLeaseMatches(next, f.runtime.Custody().PendingEffect()),
              "fresh exact complete tuple admits only current prompt's existing native lease");
    }
}
void TestRemoteEditorGenerationAndLocalContinuity() {
    Fixture f;
    unsigned calls = 0U;
    f.console.RegisterCommand("reboot", "", [&](const Console::Args&) { ++calls; return true; });
    f.Ble("reboot");
    f.transport.ble.generation = 0U; f.console.Update();
    f.transport.ble.generation = (uint64_t{55U} << 32U) | 91U;
    f.Ble("\n");
    Check(calls == 0U, "retired remote partial cannot execute any old command on replacement newline");
    f.Ble("reboot\n");
    Check(calls == 1U, "fresh current remote line keeps ordinary parser semantics");
    f.Usb("reboot");
    ++f.transport.ble.generation; f.Usb("\n");
    Check(calls == 2U, "remote lease changes leave local-only editing intact");
    f.Usb("re"); f.Ble("boot");
    ++f.transport.ble.generation; f.Usb("\n");
    Check(calls == 2U, "retired mixed-source partial disappears before a local newline");
    f.console.RegisterCommand("paste", "", [&](const Console::Args&) {
        return f.console.StartPasteMode([](Console::PasteEvent,char*,size_t) { return true; });
    });
    f.Ble("paste\n"); f.Ble("one\n\x04");
    f.Usb("reboot"); ++f.transport.ble.generation; f.Usb("\n");
    Check(calls == 3U, "completed remote paste leaves no stale lease on later local-only editing");
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock{&fxe::ManualMillisProvider};
    TestPrivateUsbPairingsOnlyClear();
    TestPrivateClearSourceAndBusyBoundaries();
    TestRemoteEditorGenerationAndLocalContinuity();
    TestBuildProfile();
    TestEitherEndUsbExample();
    TestWholeEditedLineProvenance();
    TestConsentRegistrationAndBoundaries();
    TestProgrammaticCommandIsUnknown();
    TestReusedAttemptCannotAuthorizeAnotherPrompt();
    std::printf("[OWNERPAIR_CONSOLE] runtime=%u developer_consent=%u failures=%d\n",
                modules::OWNER_PAIRING_ENABLED ? 1U : 0U,
                modules::OWNER_PAIRING_DEVELOPER_CONSENT_ENABLED ? 1U : 0U, failures);
    return failures == 0 ? 0 : 1;
}
