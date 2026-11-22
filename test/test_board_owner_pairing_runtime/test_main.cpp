#include <cstdio>
#include <string_view>

#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/board_owner_pairing_runtime.hpp"

namespace {
using Runtime = modules::BoardOwnerPairingRuntime;
using Controller = Runtime::Controller;
using Record = modules::BoardOwnerRecord;
using State = Runtime::State;
using Phase = Controller::Phase;
using Effect = Controller::EffectType;
using Action = modules::owner_pairing_session_wire::Action;
constexpr Runtime::Connection LINK{12U, 31U, 71U};
int failures = 0;
void Check(bool okay, const char* message) {
    if (!okay) { ++failures; std::printf("[PAIRING_RUNTIME_FAIL] %s\n", message); }
}
struct Persistence final : modules::BoardOwnerPersistence {
    Receipt active = 0U;
    Receipt next = 1U;
    Outcome outcome = Outcome::Pending;
    Record requested = Record::Invalid;
    unsigned saves = 0U;
    modules::BoardOwnerAddBaseline baseline{};
    bool SaveAddingJournal(const modules::BoardOwnerAddBaseline& value, Receipt& receipt) override {
        if (!value.Valid() || !SaveOwnerRecord(Record::Adding, receipt)) return false;
        baseline = value;
        return true;
    }
    modules::BoardOwnerAddBaseline AddingBaseline() const override { return baseline; }
    bool SaveOwnerRecord(Record record, Receipt& receipt) override {
        if (active != 0U) return false;
        requested = record;
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
struct Fixture {
    Persistence persistence;
    Runtime runtime{persistence};
    Fixture(Record record = Record::Absent) {
        fxe::ManualMillis::SetNowMs(100U);
        runtime.Boot(record, {true, uint8_t(record == Record::Active ? 1U : 0U)}, true);
    }
    void Advance(uint32_t ms) { fxe::ManualMillis::AdvanceMs(ms); runtime.Poll(); }
    bool Edge(uint8_t end, bool isX, bool down) {
        return runtime.ObserveButton({end, isX, down, fxe::Millis()});
    }
    void Save() {
        persistence.outcome = Persistence::Outcome::Saved;
        runtime.UpdateDurableState(persistence.requested, true);
        runtime.Poll();
    }
};

void TestClaimFreshnessAndWindow() {
    Fixture f;
    Check(!f.Edge(0U, true, true), "ordinary held X initially routes to game");
    Check(f.runtime.Begin({Action::Begin, 1U, 0U}, LINK), "Begin accepts held button and opens prompt");
    Check(f.Edge(0U, false, true) && f.runtime.Custody().GetPhase() == Phase::Armed,
          "held X cannot combine with a fresh O for consent");
    f.Edge(0U, true, false);
    f.Edge(0U, true, true);
    Check(f.runtime.Custody().PendingEffect().type == Effect::RequestSecurity,
          "release then fresh X confirms with fresh held O without a timed hold");
    const auto effect = f.runtime.Custody().PendingEffect();
    Check(f.runtime.ClaimLeaseMatches(LINK, effect), "live exact claim lease accepts security effect");
    auto stale = LINK;
    ++stale.sessionToken;
    Check(!f.runtime.ClaimLeaseMatches(stale, effect), "retired shared-session token cannot complete claim");
    stale = LINK; ++stale.epoch;
    Check(!f.runtime.ClaimLeaseMatches(stale, effect), "reused handle with new epoch cannot complete claim");
    Check(f.Edge(0U, true, false) && f.Edge(0U, false, false),
          "confirmed chord release tail is consumed");

    Fixture wrong;
    Check(wrong.runtime.Begin({Action::Begin, 2U, 1U}, LINK), "other physical end prompt opens");
    Check(wrong.Edge(0U, true, true) && wrong.Edge(0U, false, true),
          "wrong-end X/O are consumed without reaching game");
    Check(wrong.runtime.Custody().GetPhase() == Phase::Armed, "wrong-end chord cannot confirm");
    wrong.Advance(5000U);
    Check(wrong.runtime.Status().state == State::AwaitPhysical, "claim remains open at the old five-second deadline");
    wrong.Advance(4999U);
    Check(wrong.runtime.Status().state == State::AwaitPhysical, "claim remains open just before ten seconds");
    wrong.Advance(1U);
    Check(wrong.runtime.Status().state == State::Unpaired &&
              wrong.runtime.Status().reason == Runtime::Reason::Timeout && wrong.persistence.saves == 0U,
          "ten-second expiry cancels before ownership/native keys");
    Check(wrong.Edge(0U, true, false) && wrong.Edge(0U, false, false), "timeout consumes held wrong-end tail");
    Check(wrong.runtime.Begin({Action::Begin, 3U, 1U}, LINK), "fresh retry recovers after timeout");
    Check(!wrong.runtime.Cancel({Action::Cancel, 2U, 1U}, LINK) &&
              !wrong.runtime.Cancel({Action::Cancel, 3U, 0U}, LINK),
          "old attempt or wrong end cannot cancel retry");
    Check(wrong.runtime.Cancel({Action::Cancel, 3U, 1U}, LINK), "addressed prompt cancellation accepted");
    Check(wrong.runtime.Begin({Action::Begin, 4U, 1U}, LINK), "cancellation preserves live link for retry");

    Fixture queued;
    queued.runtime.Begin({Action::Begin, 5U, 0U}, LINK);
    queued.runtime.ObserveButton({0U, true, true, 99U});
    queued.Edge(0U, false, true);
    Check(queued.runtime.Custody().GetPhase() == Phase::Armed,
          "queued pre-Begin Press cannot supply consent at the other fresh Press");
}

void TestIdentityAndRetirement() {
    Fixture f;
    f.runtime.UpdateDurableState(Record::Absent, false);
    Check(!f.runtime.Begin({Action::Begin, 1U, 0U}, LINK), "identity must be durable before Begin");
    f.runtime.UpdateDurableState(Record::Absent, true);
    Check(!f.runtime.Begin({Action::Begin, 0U, 0U}, LINK) &&
              !f.runtime.Begin({Action::Begin, 1U, 3U}, LINK), "malformed attempt/end rejected");
    f.runtime.Begin({Action::Begin, 2U, 0U}, LINK);
    f.Edge(0U, false, true); f.Edge(0U, true, true);
    const auto security = f.runtime.Custody().PendingEffect();
    Check(f.runtime.Cancel({Action::Cancel, 2U, 0U}, LINK) && f.runtime.NeedsDisconnect(),
          "post-consent cancellation requires actual native disconnect");
    Check(f.runtime.Custody().GetPhase() == Phase::AwaitSecurity &&
              !f.runtime.ClaimLeaseMatches(LINK, security) && f.persistence.saves == 0U,
          "retirement invalidates lease without pretending native disconnect occurred");
    auto stale = LINK; ++stale.epoch;
    f.runtime.Disconnected(stale);
    Check(f.runtime.NeedsDisconnect(), "wrong native epoch cannot discharge disconnect obligation");
    f.runtime.Disconnected(LINK);
    Check(!f.runtime.NeedsDisconnect() && f.runtime.Custody().GetPhase() == Phase::SavingTombstone &&
              f.persistence.requested == Record::Clearing,
          "actual addressed disconnect begins durable rollback journal");
    Check(f.runtime.Custody().AcknowledgeSecurity(security.token, security.session,
              security.connection, security.epoch, security.physicalEnd,
              {{true, 1U}, true, true, true, true}) == Controller::AckResult::Stale,
          "late native callback cannot revive retired claim");
}

void TestEitherEndConsentLease() {
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        Fixture f;
        f.Edge(end, true, true);
        Check(f.runtime.Begin({Action::Begin, 40U, 2U}, LINK), "either-end Begin opens one lease");
        const auto oldConsent = f.runtime.ReadPendingConsent();
        f.Edge(end, false, true);
        Check(f.runtime.Custody().GetPhase() == Phase::Armed,
              "either-end prompt refuses a preheld X plus fresh O");
        f.Edge(end, true, false); f.Edge(end, false, false);
        f.Edge(end, true, true); f.Edge(1U - end, false, true);
        Check(f.runtime.Custody().GetPhase() == Phase::Armed,
              "X and O from different banks cannot combine");
        f.Edge(end, false, true);
        const auto effect = f.runtime.Custody().PendingEffect();
        Check(effect.type == Effect::RequestSecurity && effect.physicalEnd == end &&
                  f.runtime.Status().physicalEnd == end && f.runtime.ClaimLeaseMatches(LINK, effect),
              "first complete fresh same-bank chord latches that exact native lease end");
        f.Edge(1U - end, true, true);
        Check(f.runtime.Custody().PendingEffect().token == effect.token &&
                  !f.runtime.ConfirmPendingConsent(oldConsent),
              "other-bank chord and old unselected consent cannot replace the winner");
        Check(f.runtime.Cancel({Action::Cancel, 40U, 2U}, LINK) &&
                  f.runtime.NeedsDisconnect() && !f.runtime.ClaimLeaseMatches(LINK, effect),
              "Cancel queued at the either-end prompt retires a just-confirmed winner");
    }
    Fixture f;
    Check(f.runtime.Begin({Action::Begin, 41U, 2U}, LINK), "either-end retry opens");
    auto stale = f.runtime.ReadPendingConsent();
    stale.physicalEnd = 1U;
    Check(!f.runtime.Cancel({Action::Cancel, 41U, 0U}, LINK), "cancel must address either-end lease");
    Check(f.runtime.Cancel({Action::Cancel, 41U, 2U}, LINK), "either-end cancel addresses exact prompt");
    Check(f.runtime.Begin({Action::Begin, 41U, 2U}, LINK) &&
              !f.runtime.ConfirmPendingConsent(stale), "same attempt cannot revive a retired prompt");
    f.runtime.ObserveButton({1U, true, true, 99U}); f.Edge(1U, false, true);
    Check(f.runtime.Custody().GetPhase() == Phase::Armed, "queued old press cannot confirm either-end retry");
    f.Advance(10000U);
    uint8_t bytes[8]{};
    Check(f.runtime.Status().reason == Runtime::Reason::Timeout &&
              modules::owner_pairing_session_wire::EncodeStatus(f.runtime.Status(), bytes, sizeof(bytes)),
          "either-end expiry returns a valid unpaired status without native mutation");
}

void TestRetirementDuringOwnerReceipt() {
    Fixture f;
    f.runtime.Begin({Action::Begin, 1U, 0U}, LINK);
    f.Edge(0U, true, true); f.Edge(0U, false, true);
    const auto security = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeSecurity(security.token, security.session,
        security.connection, security.epoch, security.physicalEnd,
        {{true, 1U}, true, true, true, true});
    f.runtime.Retire(LINK);
    f.Save();
    Check(f.runtime.NeedsDisconnect() &&
              f.runtime.Custody().GetPhase() == Phase::SavingOwner &&
              f.runtime.Custody().PendingEffect().type == Effect::None &&
              !f.runtime.OwnerReady(LINK),
          "saved receipt cannot activate a retired claim while real disconnect is in flight");
    f.runtime.Disconnected(LINK); f.runtime.Poll();
    Check(f.runtime.Custody().GetPhase() == Phase::SavingTombstone &&
              f.persistence.requested == Record::Clearing &&
              f.runtime.Custody().PendingEffect().type == Effect::None,
          "actual disconnect turns late owner-save success into journaled rollback");
}

void TestDurableClaimAndQuietOwner() {
    Fixture f;
    f.runtime.Begin({Action::Begin, 1U, 0U}, LINK);
    f.Edge(0U, true, true); f.Edge(0U, false, true);
    const auto security = f.runtime.Custody().PendingEffect();
    Check(f.runtime.Custody().AcknowledgeSecurity(security.token, security.session,
              security.connection, security.epoch, security.physicalEnd,
              {{true, 1U}, true, true, true, true}) == Controller::AckResult::AcceptedSuccess,
          "checked security starts exact owner receipt");
    Check(!f.runtime.OwnerReady(LINK) && f.runtime.Status(LINK).state == State::PersistingOwner,
          "bond flags alone do not report owner ready");
    f.Save();
    Check(!f.runtime.OwnerReady(LINK), "unacknowledged persistence success does not admit traffic");
    const auto effect = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeSuccess(effect.token); f.runtime.Poll();
    Check(f.runtime.OwnerReady(LINK) && f.runtime.Status(LINK).state == State::OwnerReady,
          "native bond plus durable owner and receipt completion admits requesting connection");
    auto other = LINK; ++other.sessionToken;
    Check(!f.runtime.OwnerReady(other) && f.runtime.Status(other).state == State::OwnedOther,
          "same native address without exact shared session has no owner authority");
    f.Edge(0U, true, false); f.Edge(0U, false, false);
    Check(!f.Edge(0U, true, true), "normal X outside explicit context retains game owner");
    f.Edge(0U, true, false);
    Check(f.runtime.SelectLocalClear(8U, 0U), "local clear opens on owned board");
    f.runtime.LeaveLocalClear();
    Check(f.runtime.OwnerReady(LINK), "leaving unconfirmed clear preserves owner link and authority");
}

void TestOfflineClearEitherOrderAndCue() {
    for (uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        for (bool xFirst : {false, true}) {
            Fixture f(Record::Active);
            Check(f.runtime.SelectLocalClear(10U, end), "offline clear selection arms same physical end");
            Check(f.Edge(end, xFirst, true), "first clear button belongs to explicit context");
            f.Advance(900U);
            Check(f.runtime.Presentation().progress256 == 0U, "one button cannot start clear progress");
            f.Edge(end, !xFirst, true);
            f.Advance(1000U);
            Check(f.runtime.Presentation().progress256 == 128U && f.persistence.saves == 0U,
                  "continuous both-button hold shows proportional one-second progress");
            for (bool swapped : {false, true}) {
                modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay;
                f.runtime.ComposePhysicalCue(overlay, swapped);
                hardware::PhysicalLedFrame pixels{};
                hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,
                                                           overlay, pixels, swapped);
                const uint8_t o = hardware::V3_3PhysicalIndexForLogicalControl(end == 1U,
                    hardware::ControlLedRole::O, false);
                const uint8_t x = hardware::V3_3PhysicalIndexForLogicalControl(end == 1U,
                    hardware::ControlLedRole::X, false);
                Check(pixels[256U + o].b > 0U && pixels[256U + x].b > 0U &&
                          pixels[256U + o].r == 0U && pixels[256U + x].r == 0U,
                      "both blue X/O stay at selected printed end through bank swap");
            }
            f.Edge(end, xFirst, false);
            Check(f.runtime.Presentation().progress256 == 0U && f.persistence.saves == 0U,
                  "release either button cancels hold without changing durable owner");
            f.Edge(end, xFirst, true);
            f.Advance(1999U);
            Check(f.persistence.saves == 0U, "retry must hold fresh continuous two seconds");
            f.Advance(1U);
            Check(f.persistence.requested == Record::Clearing && f.persistence.saves == 1U &&
                      f.runtime.Custody().PendingEffect().type == Effect::None,
                  "two-second hold admits Clearing before any destructive effect");
            Check(f.Edge(end, true, false) && f.Edge(end, false, false),
                  "clear confirmation release tail cannot reach game");
        }
    }
}

void TestLocalClearSurvivesOwnerDisconnect() {
    for (const bool newlyPaired : {false, true}) {
        for (const uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
            for (const bool xFirst : {false, true}) {
                Fixture f(newlyPaired ? Record::Absent : Record::Active);
                if (newlyPaired) {
                    Check(f.runtime.Begin({Action::Begin, 50U, end}, LINK), "new owner claim opens");
                    f.Edge(end, true, true); f.Edge(end, false, true);
                    const auto security = f.runtime.Custody().PendingEffect();
                    Check(f.runtime.Custody().AcknowledgeSecurity(security.token, security.session,
                        security.connection, security.epoch, security.physicalEnd,
                        {{true, 1U}, true, true, true, true}) == Controller::AckResult::AcceptedSuccess,
                        "checked native security retains successful claim context");
                    f.Save();
                    f.runtime.Custody().AcknowledgeSuccess(f.runtime.Custody().PendingEffect().token);
                    f.runtime.Poll();
                    f.Edge(end, true, false); f.Edge(end, false, false);
                } else {
                    Check(f.runtime.RegisterOwnerConnection(LINK, {{true, 1U}, true, true, true, true}),
                        "quiet reconnect records a live saved owner");
                }
                const unsigned savedBeforeClear = f.persistence.saves;
                f.runtime.SyncLocalClearContext(true, end);
                Check(f.runtime.Status().state == State::ClearPrompt, "Bluetooth selection arms local clear");
                f.Edge(end, xFirst, true); f.Edge(end, !xFirst, true); f.Advance(800U);
                f.runtime.Disconnected(LINK);
                f.runtime.SyncLocalClearContext(true, end);
                Check(f.runtime.Status().state == State::ClearPrompt &&
                          f.runtime.Presentation().cue == Runtime::PhysicalCue::Clear,
                      "phone disconnect cannot retire the selected board-local clear prompt");
                Check(f.Edge(end, xFirst, false), "early release remains owned by local clear after disconnect");
                f.Advance(2000U);
                Check(f.persistence.saves == savedBeforeClear && f.runtime.Presentation().progress256 == 0U,
                      "disconnect held-tail handling cannot hide an early release or complete old clear consent");
                if (newlyPaired) {
                    f.Edge(end, xFirst, true); f.Advance(2000U);
                    Check(f.persistence.saves == savedBeforeClear && f.runtime.Presentation().progress256 == 0U,
                          "a retired successful claim consumes held input until all X/O release");
                    f.Edge(end, xFirst, false);
                }
                f.Edge(end, !xFirst, false);
                f.runtime.SyncLocalClearContext(true, end);
                f.Edge(end, !xFirst, true); f.Edge(end, xFirst, true); f.Advance(1999U);
                Check(f.persistence.saves == savedBeforeClear, "fresh offline clear still requires two continuous seconds");
                f.Advance(1U);
                Check(f.persistence.saves == savedBeforeClear + 1U && f.persistence.requested == Record::Clearing,
                      "fresh same-bank hold clears without leaving and reselecting Bluetooth");
            }
        }
    }
}

void TestClearFeedbackReceiptsAndExpiry() {
    Fixture f(Record::Active);
    f.runtime.SelectLocalClear(20U, 0U);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay first;
    f.runtime.ComposePhysicalCue(first);
    f.Advance(600U);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay trough;
    f.runtime.ComposePhysicalCue(trough);
    Check(first.controlPixels[0].b > trough.controlPixels[0].b &&
              trough.controlPixels[0].b > 0U &&
              trough.controlPixels[0].b == trough.controlPixels[5].b,
          "clear prompt starts bright and pulses both blue buttons without going dark");
    f.Edge(0U, true, true); f.Edge(0U, false, true); f.Advance(2000U);
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay handoff;
    f.runtime.ComposePhysicalCue(handoff);
    unsigned blue = 0U;
    for (const auto color : handoff.pixels) {
        if (color.b > 0U) ++blue;
        Check(color.r == 0U, "hold completion hands its blue tail into pending without introducing success hues early");
    }
    Check(blue == 4U && handoff.perimeterAlpha255 == 255U,
          "accepted hold retains one fully visible four-pixel blue tail at the pending handoff");
    // Sample the established chase after its 200 ms entry, on a 16 ms boundary.
    f.Advance(208U);
    uint32_t visited = 0U;
    for (unsigned step = 0U; step < 32U; ++step) {
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay chase;
        f.runtime.ComposePhysicalCue(chase);
        Check(chase.mask == UINT32_MAX,
              "pending clear owns all perimeter pixels including USB status");
        unsigned lit = 0U;
        for (uint8_t pixel = 0U; pixel < 32U; ++pixel) {
            const auto color = chase.pixels[pixel];
            if (color.r + color.g + color.b != 0U) {
                visited |= UINT32_C(1) << pixel;
                ++lit;
            }
        }
        Check(lit == 8U, "pending clear has two moving four-pixel tails");
        Check(chase.controlPixels[0].b == 0U && chase.controlPixels[5].b == 0U,
              "accepted hold stops requesting either confirming button");
        f.Advance(16U);
    }
    Check(visited == UINT32_MAX, "clear chase reaches every perimeter LED");
    f.Save();
    auto effect = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeBondDelete(effect.token, true);
    f.Save();
    Check(!f.runtime.Custody().Pairable() &&
              f.runtime.Presentation().nextAction == std::string_view("wait"),
          "durable absent alone cannot start clear success before completion acknowledgement");
    effect = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeSuccess(effect.token); f.Advance(0U);
    Check(f.runtime.Custody().Pairable() &&
              f.runtime.Presentation().nextAction == std::string_view("pair-from-app"),
          "only checked completed clear starts success feedback");
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay blink;
    f.runtime.ComposePhysicalCue(blink);
    Check(blink.mask == UINT32_MAX, "verified success starts its own whole-ring feedback budget");
    f.Advance(1999U);
    Check(f.runtime.Presentation().active, "success feedback retains a full episode after verified completion");
    f.Advance(1U);
    Check(!f.runtime.Presentation().active,
          "success expires two seconds after verified completion");
}
void TestSavedPhonesConsentAndRollback() {
    for (uint8_t saved = 1U; saved <= 3U; ++saved) {
        Fixture f(Record::Active);
        f.runtime.Boot(Record::Active, {true, saved}, true);
        modules::BoardOwnerAddBaseline baseline{};
        baseline.saved.count = saved;
        for (uint8_t i = 0U; i < saved; ++i) baseline.saved.peers[i].address[0U] = i + 1U;
        const bool accepted = f.runtime.Begin({Action::Begin, 51U, 2U}, LINK, baseline);
        Check(accepted == (saved < 3U), "fourth phone is refused before physical prompt or native security");
        if (!accepted) {
            Check(f.persistence.saves == 0U && f.runtime.Custody().PendingEffect().type == Effect::None,
                  "capacity refusal has no durable or native mutation");
            continue;
        }
        Check((f.runtime.Status(LINK).flags & modules::owner_pairing_session_wire::OwnerRetained) != 0U,
              "adding prompt retains prior saved phones");
        f.Edge(1U, true, true); f.Edge(1U, false, true);
        Check(f.runtime.Status(LINK).state == State::Securing &&
                  f.runtime.Custody().PendingEffect().type == Effect::None,
              "adding journal remains pending without starting security or reporting clearing");
        f.Save();
        auto effect = f.runtime.Custody().PendingEffect();
        Check(effect.type == Effect::RequestSecurity && f.runtime.ClaimLeaseMatches(LINK, effect),
              "only durable released adding journal grants exact consent lease");
        auto identities = baseline.saved;
        identities.peers[saved].address[0U] = 9U; ++identities.count;
        Controller::ConnectionSecurity security{{true, identities.count, identities}, true, true, true, true};
        Check(f.runtime.Custody().AcknowledgeSecurity(effect.token, effect.session, effect.connection,
                  effect.epoch, effect.physicalEnd, security) == Controller::AckResult::AcceptedSuccess,
              "native completion proves prior identities plus one new phone");
        f.Save(); effect = f.runtime.Custody().PendingEffect();
        f.runtime.Custody().AcknowledgeSuccess(effect.token); f.runtime.Poll();
        Check(f.runtime.OwnerReady(LINK) && f.runtime.Custody().SavedPhoneCount() == saved + 1U,
              "second and third saved phone become ready after durable owner proof");
    }
    Fixture failed(Record::Active);
    modules::BoardOwnerAddBaseline baseline{};
    baseline.saved.count = 1U; baseline.saved.peers[0U].address[0U] = 1U;
    Check(failed.runtime.Begin({Action::Begin, 52U, 2U}, LINK, baseline), "failed-add journey begins");
    failed.Edge(0U, true, true); failed.Edge(0U, false, true); failed.Save();
    failed.runtime.Retire(LINK); failed.runtime.Disconnected(LINK); failed.Save();
    auto effect = failed.runtime.Custody().PendingEffect();
    Check(effect.type == Effect::QuiesceAndRejectWork, "retired add quiesces before targeted rollback");
    failed.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = failed.runtime.Custody().PendingEffect();
    Check(effect.type == Effect::RollbackAddedPeers, "failed add never selects delete-all-bonds");
    failed.runtime.Custody().AcknowledgeBondDelete(effect.token, true); failed.Save();
    Check(failed.runtime.Status().state == State::OwnedOther &&
              failed.runtime.Custody().SavedPhoneCount() == 1U &&
              (failed.runtime.Status().flags & modules::owner_pairing_session_wire::OwnerRetained) != 0U,
          "failed new-phone attempt restores prior saved-phone custody and retry availability");
}
void TestUnconfirmedAddDisconnectThenExplicitAllPhoneClear() {
    Fixture f(Record::Active);
    f.runtime.Boot(Record::Active, {true, 2U}, true);
    modules::BoardOwnerAddBaseline baseline{};
    baseline.saved.count = 2U;
    baseline.saved.peers[0U].address[0U] = 11U;
    baseline.saved.peers[1U].address[0U] = 12U;
    baseline.rpaCount = 1U;
    baseline.rpas[0U].rpa.address[0U] = 91U;
    baseline.rpas[0U].identity.address[0U] = 99U;
    Check(f.runtime.Begin({Action::Begin, 61U, 2U}, LINK, baseline),
          "unconfirmed Add begins with the complete prior identity/RPA baseline");
    f.runtime.Disconnected(LINK); // Actual native disconnect arrives before Retire.
    f.runtime.Retire(LINK);
    Check(f.persistence.saves == 0U && f.runtime.Custody().SavedPhoneCount() == 2U &&
              f.runtime.Custody().PendingEffect().type == Effect::None,
          "unconfirmed native disconnect preserves saved phones without native or durable work");
    Check(!f.runtime.Custody().IsAdding() && !f.runtime.Custody().AddBaseline().Valid(),
          "unconfirmed native disconnect retires the entire Add-only transient state");
    Check(f.runtime.SelectLocalClear(62U, 0U), "board-local all-phone clear can follow disconnected Add");
    f.Edge(0U, true, true); f.Edge(0U, false, true); f.Advance(Runtime::CLEAR_HOLD_MS);
    Check(f.persistence.requested == Record::Clearing &&
              f.runtime.Custody().GetPhase() == Phase::SavingTombstone,
          "explicit physical clear saves Clearing rather than an Add rollback journal");
    f.Save();
    auto effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Effect::QuiesceAndRejectWork, "all-phone clear requires native traffic quiescence");
    f.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
    effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Effect::DeleteAllBonds,
          "explicit all-phone clear requests checked deletion of every peer, never targeted Add rollback");
    f.runtime.Custody().AcknowledgeBondDelete(effect.token, false);
    Check(f.runtime.Custody().GetPhase() == Phase::Failed && f.persistence.requested == Record::Clearing,
          "missing checked native-empty proof cannot admit Absent even after the disconnect");
    Check(f.runtime.Custody().RetryBondDelete(), "all-phone clear retries its failed native proof");
    effect = f.runtime.Custody().PendingEffect();
    Check(effect.type == Effect::DeleteAllBonds, "retry still requires the all-peer native proof");
    f.runtime.Custody().AcknowledgeBondDelete(effect.token, true);
    Check(f.persistence.requested == Record::Absent, "only checked all-peer deletion admits durable Absent");
    f.Save(); effect = f.runtime.Custody().PendingEffect();
    f.runtime.Custody().AcknowledgeSuccess(effect.token); f.runtime.Poll();
    Check(f.runtime.Custody().Pairable() && f.runtime.Custody().SavedPhoneCount() == 0U &&
              f.runtime.Status().state == State::Unpaired,
          "completed explicit clear reaches Unpaired instead of restoring the disconnected Add baseline");
}
void TestAlreadyClearAcknowledgementAndAuthority() {
    for (const uint8_t end : {uint8_t{0U}, uint8_t{1U}}) {
        for (const bool xFirst : {false, true}) {
            Fixture f;
            f.runtime.SyncLocalClearContext(true, end);
            Check(f.runtime.Presentation().cue == Runtime::PhysicalCue::Clear,
                  "checked already-absent Bluetooth selection owns its local confirmation");
            f.Edge(end, xFirst, true);
            Check(!f.runtime.Begin({Action::Begin, 70U, end}, LINK),
                  "even a partially held local acknowledgement cannot become phone consent");
            f.Edge(end, !xFirst, true); f.Advance(1000U);
            Check(!f.runtime.Begin({Action::Begin, 71U, end}, LINK),
                  "held local already-clear consent cannot be transferred into a phone claim");
            Check(f.runtime.Presentation().progress256 == 128U && f.persistence.saves == 0U,
                  "already-clear acknowledgement shows the existing two-second hold progress");
            f.Edge(end, xFirst, false); f.Advance(2000U);
            Check(f.runtime.Presentation().progress256 == 0U && f.persistence.saves == 0U,
                  "already-clear early release cancels without a native or durable operation");
            f.Edge(end, xFirst, true); f.Advance(2000U);
            Check(f.runtime.Presentation().cue == Runtime::PhysicalCue::Cleared &&
                      f.runtime.Custody().Pairable() && f.persistence.saves == 0U &&
                      f.runtime.Custody().PendingEffect().type == Effect::None,
                  "already-clear confirmation acknowledges checked absence without deletion or fabricated persistence");
            f.Edge(end, xFirst, false); f.Edge(end, !xFirst, false); f.Advance(2000U);
            f.Edge(end, !xFirst, true); f.Edge(end, xFirst, true); f.Advance(2000U);
            Check(f.runtime.Presentation().cue == Runtime::PhysicalCue::Cleared && f.persistence.saves == 0U,
                  "fresh repeated already-clear hold remains a local acknowledgement");
        }
    }
    Fixture changed;
    changed.runtime.SyncLocalClearContext(true, 0U);
    changed.Edge(0U, true, true); changed.Edge(0U, false, true); changed.Advance(1000U);
    changed.runtime.UpdateDurableState(Record::Active, true);
    changed.runtime.UpdateDurableState(Record::Absent, true);
    changed.Advance(2000U);
    Check(changed.runtime.Presentation().cue != Runtime::PhysicalCue::Cleared &&
              changed.persistence.saves == 0U,
          "an intervening owner change retires already-clear hold consent even when absence returns");
    changed.Edge(0U, true, false); changed.Edge(0U, false, false);
    changed.Edge(0U, true, true); changed.Edge(0U, false, true); changed.Advance(2000U);
    Check(changed.runtime.Presentation().cue == Runtime::PhysicalCue::Cleared && changed.persistence.saves == 0U,
          "fresh released consent can acknowledge rechecked absence after authority changes");
    Fixture lost;
    lost.runtime.SyncLocalClearContext(true, 0U);
    lost.Edge(0U, true, true); lost.Edge(0U, false, true); lost.Advance(1000U);
    lost.runtime.Custody().Boot(Record::Absent, {false, 0U}); lost.Advance(2000U);
    Check(lost.runtime.Presentation().cue != Runtime::PhysicalCue::Cleared && lost.persistence.saves == 0U,
          "inventory loss during an already-clear hold fails closed without acknowledging absence");
    Fixture superseded;
    superseded.runtime.SyncLocalClearContext(true, 0U);
    superseded.Edge(0U, true, true); superseded.Edge(0U, false, true); superseded.Advance(2000U);
    superseded.Edge(0U, true, false); superseded.Edge(0U, false, false);
    Check(superseded.runtime.Begin({Action::Begin, 72U, 0U}, LINK) &&
              superseded.runtime.Presentation().cue == Runtime::PhysicalCue::Claim,
          "a new explicit phone ceremony can supersede completed already-clear cosmetic feedback");
    Fixture idle;
    idle.runtime.SyncLocalClearContext(true, 0U);
    Check(idle.runtime.Status().state == State::Unpaired &&
              idle.runtime.Presentation().cue == Runtime::PhysicalCue::Clear,
          "idle already-clear physical feedback preserves the app-visible Unpaired admission state");
    Check(idle.runtime.Begin({Action::Begin, 73U, 1U}, LINK) &&
              idle.runtime.Presentation().cue == Runtime::PhysicalCue::Claim && idle.persistence.saves == 0U,
          "idle checked-absent Bluetooth selection yields to a fresh explicit phone ceremony");
    idle.Edge(0U, true, true); idle.Edge(0U, false, true);
    Check(idle.runtime.Custody().PendingEffect().type == Effect::None,
          "the retired local clear bank cannot supply consent to the new phone prompt");
    idle.Edge(0U, true, false); idle.Edge(0U, false, false);
    idle.Edge(1U, true, true); idle.Edge(1U, false, true);
    Check(idle.runtime.Custody().PendingEffect().type == Effect::RequestSecurity,
          "the new phone ceremony retains its ordinary fresh same-bank consent guard");
    Fixture owned(Record::Active);
    owned.runtime.SyncLocalClearContext(true, 0U);
    Check(!owned.runtime.Begin({Action::Begin, 74U, 0U}, LINK) &&
              owned.runtime.Custody().GetOperation() == Controller::Operation::Unpair,
          "an owned board's destructive clear prompt remains exclusive even before buttons are held");
    for (const auto record : {Record::Absent, Record::Active, Record::Clearing}) {
        Fixture unavailable(record);
        unavailable.runtime.Boot(record, {false, 0U}, true);
        unavailable.runtime.SyncLocalClearContext(true, 0U);
        unavailable.Edge(0U, true, true); unavailable.Edge(0U, false, true); unavailable.Advance(2200U);
        Check(unavailable.runtime.Presentation().cue != Runtime::PhysicalCue::Cleared &&
                  unavailable.persistence.saves == 0U,
              "unavailable native inventory cannot acknowledge absence or start a clear from Bluetooth");
    }
}

void TestClearCosmeticDeadlineWhilePersistenceStalls() {
    for (const uint32_t start : {100U, UINT32_MAX - 1000U}) {
        Fixture f(Record::Active);
        fxe::ManualMillis::SetNowMs(start);
        f.runtime.SelectLocalClear(21U, 0U);
        f.Edge(0U, true, true); f.Edge(0U, false, true); f.Advance(2000U);
        f.Advance(1999U);
        Check(f.runtime.Presentation().cue == Runtime::PhysicalCue::Clearing,
              "pending clear still animates immediately before two-second deadline");
        f.Advance(1U);
        Check(f.runtime.Presentation().cue == Runtime::PhysicalCue::Clearing &&
                  f.runtime.Custody().GetPhase() == Phase::SavingTombstone &&
                  !f.runtime.Custody().Pairable(),
              "pending clear keeps its chase while native and durable completion remain unresolved");
        f.Save();
        auto effect = f.runtime.Custody().PendingEffect();
        f.runtime.Custody().AcknowledgeQuiesce(effect.token, true);
        effect = f.runtime.Custody().PendingEffect();
        f.runtime.Custody().AcknowledgeBondDelete(effect.token, true); f.Save();
        effect = f.runtime.Custody().PendingEffect();
        f.runtime.Custody().AcknowledgeSuccess(effect.token); f.Advance(0U);
        Check(f.runtime.Custody().Pairable() && f.runtime.Presentation().cue == Runtime::PhysicalCue::Cleared,
              "late verified completion starts its own visible success episode");
        f.Advance(1999U);
        Check(f.runtime.Presentation().active, "late success remains visible for its complete two-second budget");
        f.Advance(1U);
        Check(!f.runtime.Presentation().active, "late success returns to ordinary output on its own deadline");
    }
}

} // namespace
int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    TestClaimFreshnessAndWindow();
    TestIdentityAndRetirement();
    TestEitherEndConsentLease();
    TestRetirementDuringOwnerReceipt();
    TestDurableClaimAndQuietOwner();
    TestOfflineClearEitherOrderAndCue();
    TestLocalClearSurvivesOwnerDisconnect();
    TestAlreadyClearAcknowledgementAndAuthority();
    TestClearFeedbackReceiptsAndExpiry();
    TestSavedPhonesConsentAndRollback();
    TestUnconfirmedAddDisconnectThenExplicitAllPhoneClear();
    TestClearCosmeticDeadlineWhilePersistenceStalls();
    std::printf("[PAIRING_RUNTIME] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
