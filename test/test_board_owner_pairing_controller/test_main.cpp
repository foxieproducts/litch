#include <cstdio>

#include "modules/board_owner_pairing_controller.hpp"

namespace {

int failures = 0;
void Check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}

struct Persistence final : modules::BoardOwnerPersistence {
    bool admit = true;
    bool release = true;
    Outcome outcome = Outcome::Pending;
    Receipt nextReceipt = 1;
    Receipt activeReceipt = 0;
    modules::BoardOwnerRecord record = modules::BoardOwnerRecord::Invalid;
    unsigned saves = 0;
    unsigned releases = 0;
    modules::BoardOwnerAddBaseline baseline{};

    bool SaveAddingJournal(const modules::BoardOwnerAddBaseline& value, Receipt& receipt) override {
        if (!value.Valid() || !SaveOwnerRecord(modules::BoardOwnerRecord::Adding, receipt)) return false;
        baseline = value;
        return true;
    }
    modules::BoardOwnerAddBaseline AddingBaseline() const override { return baseline; }

    bool SaveOwnerRecord(modules::BoardOwnerRecord value,
                         Receipt& receipt) override {
        if (!admit || activeReceipt != 0) return false;
        record = value;
        receipt = nextReceipt++;
        activeReceipt = receipt;
        outcome = Outcome::Pending;
        ++saves;
        return true;
    }
    Outcome GetOutcome(Receipt receipt) const override {
        return receipt == activeReceipt ? outcome : Outcome::Unknown;
    }
    bool Release(Receipt receipt) override {
        ++releases;
        if (!release || receipt != activeReceipt) return false;
        activeReceipt = 0;
        return true;
    }
};

using Controller = modules::BoardOwnerPairingController;
using Effect = Controller::EffectType;
using Phase = Controller::Phase;
using Ownership = Controller::Ownership;
using Reason = Controller::Reason;

Controller::ConnectionSecurity Security(bool encrypted = true,
                                       bool bonded = true, uint8_t bonds = 1U) {
    return {{true, bonds}, true, encrypted, bonded, true};
}

bool AckSecurity(Controller& controller, uint16_t session, uint16_t connection,
                 uint32_t epoch, uint8_t end, bool encrypted, bool bonded,
                 uint8_t bonds) {
    const auto expected = encrypted && bonded && bonds == 1U
                              ? Controller::AckResult::AcceptedSuccess
                              : Controller::AckResult::AcceptedFailure;
    return controller.AcknowledgeSecurity(
               controller.PendingEffect().token, session, connection, epoch,
               end, Security(encrypted, bonded, bonds)) == expected;
}
bool AckDelete(Controller& controller, bool success) {
    return controller.AcknowledgeBondDelete(controller.PendingEffect().token,
                                             success) ==
           (success ? Controller::AckResult::AcceptedSuccess
                    : Controller::AckResult::AcceptedFailure);
}
bool AckQuiesce(Controller& controller, bool success) {
    return controller.AcknowledgeQuiesce(controller.PendingEffect().token,
                                          success) ==
           (success ? Controller::AckResult::AcceptedSuccess
                    : Controller::AckResult::AcceptedFailure);
}
bool AckSuccess(Controller& controller) {
    return controller.AcknowledgeSuccess(controller.PendingEffect().token) ==
           Controller::AckResult::AcceptedSuccess;
}
bool AckDisconnect(Controller& controller, bool success) {
    return controller.AcknowledgeDisconnect(controller.PendingEffect().token,
                                             success) ==
           (success ? Controller::AckResult::AcceptedSuccess
                    : Controller::AckResult::AcceptedFailure);
}

void Save(Persistence& persistence, Controller& controller,
          Persistence::Outcome outcome = Persistence::Outcome::Saved) {
    persistence.outcome = outcome;
    controller.PollPersistence();
}

void CompleteCleanup(Persistence& p, Controller& c) {
    Check(c.GetPhase() == Phase::SavingTombstone &&
              p.record == modules::BoardOwnerRecord::Clearing &&
              c.PendingEffect().type == Effect::None,
          "cleanup first admits Clearing without destructive effect");
    Save(p, c);
    Check(c.PendingEffect().type == Effect::QuiesceAndRejectWork,
          "durable Clearing precedes quiesce");
    Check(AckQuiesce(c, true), "cleanup quiesce acknowledged");
    Check(AckDelete(c, true) && !c.Pairable() &&
              p.record == modules::BoardOwnerRecord::Absent,
          "delete starts absent save without early pairability");
    Save(p, c);
}

modules::BoardOwnerAddBaseline AddBaseline(uint8_t count) {
    modules::BoardOwnerAddBaseline value{};
    value.saved.count = count;
    for (uint8_t i = 0U; i < count && i < modules::MAX_SAVED_PHONES; ++i)
        value.saved.peers[i].address[0U] = i + 1U;
    value.rpaCount = 1U;
    value.rpas[0U].rpa.address[0U] = 91U;
    value.rpas[0U].identity.address[0U] = 99U;
    return value;
}

Controller::ConnectionSecurity AddSecurity(uint8_t count) {
    auto value = Security(true, true, count);
    value.bonds.identities = AddBaseline(count).saved;
    return value;
}

void TestThreeSavedPhonesAndJournalBeforeSecurity() {
    Persistence p; Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
    for (uint8_t saved = 1U; saved < 3U; ++saved) {
        Check(c.ArmPair(40U + saved, 20U, 30U + saved, 2U, AddBaseline(saved)) && c.IsAdding(),
              "saved phone custody can arm an addressed Add below capacity");
        Check(c.ConfirmPair(40U + saved, 20U, 30U + saved, 1U) &&
                  c.GetPhase() == Phase::SavingAddJournal &&
                  p.record == modules::BoardOwnerRecord::Adding && c.PendingEffect().type == Effect::None,
              "fresh physical Add consent journals the complete baseline before native security");
        p.outcome = Persistence::Outcome::Saved; p.release = false; c.PollPersistence();
        Check(c.GetPhase() == Phase::AwaitReceiptRelease && c.PendingEffect().type == Effect::None,
              "a saved but unreleased journal cannot authorize security");
        p.release = true; c.PollPersistence();
        const auto effect = c.PendingEffect();
        Check(effect.type == Effect::RequestSecurity && effect.physicalEnd == 1U,
              "exact journal completion requests security once at the winning bank");
        Check(c.AcknowledgeSecurity(effect.token, effect.session, effect.connection, effect.epoch,
                  effect.physicalEnd, AddSecurity(saved + 1U)) == Controller::AckResult::AcceptedSuccess &&
                  c.GetPhase() == Phase::SavingOwner,
              "native proof must retain every baseline identity and the new matched peer");
        Save(p, c); Check(AckSuccess(c), "saved Add acknowledges exact completion");
        Check(c.GetOwnership() == Ownership::Owned && c.SavedPhoneCount() == saved + 1U &&
                  !c.IsAdding() && c.HasAuthenticatedOwnerConnection(),
              "completed Add increases saved capacity while admitting only its current connection");
        c.Disconnect(20U, 30U + saved);
    }
    const auto saves = p.saves;
    Check(!c.ArmPair(50U, 21U, 50U, 2U, AddBaseline(3U)) && p.saves == saves &&
              c.PendingEffect().type == Effect::None && c.SavedPhoneCount() == 3U,
          "fourth phone refuses before journal, security, eviction or native work");
    Check(!c.ArmPair(51U, 21U, 51U, 2U, AddBaseline(2U)),
          "stale pre-third-phone inventory cannot reopen capacity");
    Check(c.RegisterAuthenticatedOwnerConnection(22U, 52U, AddSecurity(3U)),
          "one of three retained native peers can quietly prove a current session");
    Check(!c.RegisterAuthenticatedOwnerConnection(23U, 53U, AddSecurity(3U)),
          "another connection cannot take the one live owner slot");
}

void TestAddRollbackAndRecoveryNeverClearAll() {
    Persistence p; Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 2U});
    Check(c.ArmPair(60U, 20U, 60U, 2U, AddBaseline(2U)) && c.ConfirmPair(60U, 20U, 60U, 0U),
          "Add rollback fixture opens physical consent");
    Save(p, c);
    auto effect = c.PendingEffect();
    auto wrong = AddSecurity(3U);
    wrong.bonds.identities.peers[0U].address[0U] = 88U;
    Check(c.AcknowledgeSecurity(effect.token, effect.session, effect.connection, effect.epoch,
              effect.physicalEnd, wrong) == Controller::AckResult::AcceptedFailure &&
              c.GetPhase() == Phase::SavingAddJournal && p.record == modules::BoardOwnerRecord::Adding,
          "same native count with a lost baseline identity starts only targeted recovery");
    Save(p, c); Check(AckQuiesce(c, true), "durable Add journal precedes native quiescence");
    Check(c.PendingEffect().type == Effect::RollbackAddedPeers && !c.HasAuthenticatedOwnerConnection(),
          "Add failure never emits the all-bond deletion effect");
    Check(AckDelete(c, false) && c.RetryBondDelete() &&
              c.PendingEffect().type == Effect::RollbackAddedPeers,
          "failed targeted native proof retries targeted rollback without widening custody");
    Check(AckDelete(c, true) && c.GetPhase() == Phase::SavingAddRestored &&
              p.record == modules::BoardOwnerRecord::Active,
          "checked targeted rollback alone can request restored Active");
    Save(p, c);
    Check(c.GetOwnership() == Ownership::Owned && c.SavedPhoneCount() == 2U &&
              !c.HasAuthenticatedOwnerConnection() && c.PendingEffect().type == Effect::None,
          "rollback retains two saved phones and grants no stale session readiness");
    Check(!c.RegisterAuthenticatedOwnerConnection(20U, 61U, AddSecurity(3U)),
          "unexplained native inventory growth cannot grant readiness after rollback");
    p.baseline = AddBaseline(2U);
    c.Boot(modules::BoardOwnerRecord::Adding, {true, 3U});
    Check(c.IsAdding() && c.PendingEffect().type == Effect::QuiesceAndRejectWork &&
              AckQuiesce(c, true) && c.PendingEffect().type == Effect::RollbackAddedPeers,
          "restart with Adding resumes only the targeted baseline recovery");
    c.Boot(modules::BoardOwnerRecord::Adding, {false, 0U});
    Check(c.GetOwnership() == Ownership::RecoveryRequired && c.PendingEffect().type == Effect::None,
          "unavailable startup never converts an Add journal into empty custody or all-bond clear");
}

void TestAddRetirementWhileJournalReceiptPending() {
    Persistence p; Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
    Check(c.ArmPair(70U, 20U, 70U, 2U, AddBaseline(1U)) && c.ConfirmPair(70U, 20U, 70U, 1U),
          "pending Add receipt fixture starts");
    c.Disconnect(20U, 70U); Save(p, c);
    Check(c.PendingEffect().type == Effect::QuiesceAndRejectWork &&
              c.GetPhase() == Phase::AwaitQuiesce,
          "disconnect during journal persistence cannot start native security after completion");
    Check(AckQuiesce(c, true) && c.PendingEffect().type == Effect::RollbackAddedPeers,
          "retired journal retains the same bounded targeted recovery");
}

void TestBootReconciliation() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1});
    Check(c.GetOwnership() == Ownership::Owned &&
              !c.HasAuthenticatedOwnerConnection() && !c.Pairable(),
          "durable owner boots unauthenticated");

    c.Boot(modules::BoardOwnerRecord::Absent, {true, 2});
    Check(c.GetOperation() == Controller::Operation::OrphanCleanup &&
              c.GetPhase() == Phase::SavingTombstone,
          "absent owner journals orphan cleanup");
    CompleteCleanup(p, c);
    Check(c.Pairable(),
          "orphan cleanup acknowledgement permits pairing");

    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U, {}, true});
    Check(c.GetOperation() == Controller::Operation::OrphanCleanup && !c.Pairable(),
          "zero-key legacy remote metadata must be cleared before first pairing");
    CompleteCleanup(p, c);
    Check(c.Pairable(), "metadata-only cleanup reaches first-phone admission after durable absence");

    // A power cut after factory reset or between native table writes leaves
    // Clearing authoritative even if no security key survives that cut.
    c.Boot(modules::BoardOwnerRecord::Clearing, {true, 0U, {}, true});
    Check(c.PendingEffect().type == Effect::QuiesceAndRejectWork && !c.Pairable(),
          "interrupted zero-key factory cleanup resumes from Clearing");
    Check(AckQuiesce(c, true) && AckDelete(c, true), "resumed remote cleanup verifies deletion");
    Save(p, c); AckSuccess(c);
    Check(c.Pairable(), "resumed factory cleanup permits first phone only after final save");

    {
        Persistence pending; Controller finishing(pending);
        finishing.Boot(modules::BoardOwnerRecord::Clearing, {true, 0U, {}, true});
        AckQuiesce(finishing, true); AckDelete(finishing, true);
        finishing.Boot(modules::BoardOwnerRecord::Absent, {true, 0U, {}, true});
        Save(pending, finishing);
        Check(!finishing.Pairable() && finishing.GetOwnership() == Ownership::RecoveryRequired &&
                  finishing.PendingEffect().type == Effect::None,
              "metadata observed during final absence receipt prevents stale success from admitting phones");
    }

    c.Boot(modules::BoardOwnerRecord::Clearing, {true, 1});
    Check(c.PendingEffect().type == Effect::QuiesceAndRejectWork,
          "clearing boot resumes quiesce before bond deletion");
    AckQuiesce(c, true);
    Check(AckDelete(c, true) &&
              p.record == modules::BoardOwnerRecord::Absent,
          "clearing boot starts final absent save");
    Save(p, c);
    Check(c.PendingEffect().type == Effect::UnpairSucceeded,
          "clearing recovery reports success after durable absent");
    AckSuccess(c);
    Check(c.Pairable(), "clearing recovery becomes pairable after success ack");

    c.Boot(modules::BoardOwnerRecord::Invalid, {true, 0});
    Check(c.GetOwnership() == Ownership::RecoveryRequired && !c.Pairable(),
          "invalid owner record fails closed");
    c.Boot(modules::BoardOwnerRecord::Active, {true, 0});
    Check(c.GetOwnership() == Ownership::RecoveryRequired,
          "active owner without sole bond fails closed");
}

void TestPairAddressingAndRollback() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    Check(!c.ArmPair(10, 20, 0, 1), "pair rejects zero connection epoch");
    Check(c.ArmPair(10, 20, 30, 1), "pair arms exact address");
    Check(!c.ConfirmUnpair(10, 1), "unpair confirmation cannot cross pair op");
    Check(!c.ConfirmPair(11, 20, 30, 1) &&
              !c.ConfirmPair(10, 21, 30, 1) &&
              !c.ConfirmPair(10, 20, 31, 1) &&
              !c.ConfirmPair(10, 20, 30, 0),
          "stale session handle epoch and end rejected");
    Check(c.ConfirmPair(10, 20, 30, 1), "matching physical consent accepted");
    const auto security = c.PendingEffect();
    Check(security.type == Effect::RequestSecurity && security.session == 10 &&
              security.connection == 20 && security.epoch == 30 &&
              security.physicalEnd == 1,
          "security effect carries complete address");
    Check(!AckSecurity(c, 10, 20, 31, 1, true, true, 1),
          "stale auth epoch cannot complete pair");
    Check(AckSecurity(c, 10, 20, 30, 1, true, true, 1),
          "exact secure sole bond starts owner save");
    Check(c.GetOwnership() == Ownership::Unpaired &&
              c.GetPhase() == Phase::SavingOwner,
          "bond alone does not activate durable ownership");
    c.Disconnect(20, 30);
    Save(p, c);
    Check(c.GetPhase() == Phase::SavingTombstone &&
              !c.HasAuthenticatedOwnerConnection(),
          "disconnect during owner save journals rollback without success");
    CompleteCleanup(p, c);
    Check(c.Pairable() && c.GetReason() == Reason::SecurityFailed,
          "retired owner save returns unpaired after complete cleanup");

    Persistence failed;
    Controller rollback(failed);
    rollback.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    rollback.ArmPair(1, 2, 3, 0);
    rollback.ConfirmPair(1, 2, 3, 0);
    AckSecurity(rollback, 1, 2, 3, 0, true, true, 1);
    Save(failed, rollback, Persistence::Outcome::Failed);
    Check(rollback.GetPhase() == Phase::SavingTombstone &&
              rollback.PendingEffect().type == Effect::None,
          "owner save failure journals new-bond rollback");
    CompleteCleanup(failed, rollback);
    Check(rollback.Pairable() &&
              rollback.GetReason() == Reason::StorageFailed,
          "successful rollback returns unpaired with storage error");

    Persistence releaseFailed;
    Controller unknown(releaseFailed);
    unknown.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    unknown.ArmPair(1, 2, 3, 0);
    unknown.ConfirmPair(1, 2, 3, 0);
    AckSecurity(unknown, 1, 2, 3, 0, true, true, 1);
    releaseFailed.release = false;
    Save(releaseFailed, unknown);
    Check(unknown.GetPhase() == Phase::AwaitReceiptRelease &&
              releaseFailed.activeReceipt != 0,
          "release failure retains exact outstanding receipt");
    unknown.Boot(modules::BoardOwnerRecord::Absent, {true, 1});
    Check(unknown.GetPhase() == Phase::AwaitReceiptRelease &&
              releaseFailed.activeReceipt != 0,
          "boot cannot silently abandon outstanding receipt");
    unknown.PollPersistence();
    Check(unknown.GetPhase() == Phase::AwaitReceiptRelease &&
              releaseFailed.activeReceipt != 0,
          "repeated poll preserves owner-save release context");
    releaseFailed.release = true;
    Check(unknown.RetryReceiptRelease() &&
              unknown.PendingEffect().type == Effect::PairSucceeded,
          "release retry resumes exact saved outcome");

    Persistence rollbackDelete;
    Controller terminal(rollbackDelete);
    terminal.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    terminal.ArmPair(1, 2, 3, 0);
    terminal.ConfirmPair(1, 2, 3, 0);
    AckSecurity(terminal, 1, 2, 3, 0, true, true, 1);
    Save(rollbackDelete, terminal, Persistence::Outcome::Unknown);
    Save(rollbackDelete, terminal);
    AckQuiesce(terminal, true);
    Check(AckDelete(terminal, false) &&
              terminal.GetReason() == Reason::BondDeleteFailed &&
              !terminal.Pairable(),
          "unknown save plus rollback-delete failure is terminal");
}

void TestOfflineUnpairOrderingAndRetries() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1});
    Check(c.ArmUnpair(44, 0), "offline unpair arms by session and end");
    Check(!c.ConfirmPair(44, 0, 0, 0),
          "pair confirmation cannot cross unpair op");
    Check(!c.ConfirmUnpair(45, 0) && !c.ConfirmUnpair(44, 1),
          "stale offline unpair confirmation rejected");
    Check(c.ConfirmUnpair(44, 0) &&
              p.record == modules::BoardOwnerRecord::Clearing,
          "unpair first saves durable tombstone");
    p.release = false;
    Save(p, c);
    Check(c.GetPhase() == Phase::AwaitReceiptRelease &&
              p.activeReceipt != 0,
          "tombstone save retains receipt when release fails");
    c.PollPersistence();
    Check(c.GetPhase() == Phase::AwaitReceiptRelease &&
              p.activeReceipt != 0,
          "repeated poll preserves tombstone-save release context");
    p.release = true;
    Check(c.RetryReceiptRelease() && c.GetReason() == Reason::None,
          "tombstone release retry resumes quiesce without stale error");
    Check(c.PendingEffect().type == Effect::QuiesceAndRejectWork,
          "saved tombstone precedes quiesce");
    Check(AckQuiesce(c, true) &&
              c.PendingEffect().type == Effect::DeleteAllBonds,
          "quiesce ack precedes bond deletion");
    Check(AckDelete(c, true) &&
              p.record == modules::BoardOwnerRecord::Absent,
          "bond deletion ack precedes final absent save");
    p.release = false;
    Save(p, c);
    Check(c.GetPhase() == Phase::AwaitReceiptRelease &&
              p.activeReceipt != 0,
          "absent save retains receipt when release fails");
    c.PollPersistence();
    Check(c.GetPhase() == Phase::AwaitReceiptRelease &&
              p.activeReceipt != 0,
          "repeated poll preserves absent-save release context");
    p.release = true;
    Check(c.RetryReceiptRelease() && c.GetReason() == Reason::None,
          "absent release retry resumes success without stale error");
    Check(c.PendingEffect().type == Effect::UnpairSucceeded && !c.Pairable(),
          "durable absent precedes acknowledged success and pairability");
    Check(AckSuccess(c) && c.Pairable(),
          "offline unpair becomes pairable after success ack");

    Persistence retry;
    Controller r(retry);
    r.Boot(modules::BoardOwnerRecord::Active, {true, 1});
    r.ArmUnpair(1, 1);
    r.ConfirmUnpair(1, 1);
    Save(retry, r, Persistence::Outcome::Failed);
    Check(r.RetryClearingSave(), "failed tombstone save has explicit retry");
    Save(retry, r);
    const uint32_t firstQuiesce = r.PendingEffect().token;
    Check(AckQuiesce(r, false) && r.RetryQuiesce(),
          "quiesce failure has explicit retry");
    Check(r.PendingEffect().token != firstQuiesce &&
              r.AcknowledgeQuiesce(firstQuiesce, true) ==
                  Controller::AckResult::Stale,
          "delayed quiesce ack cannot consume retry effect");
    AckQuiesce(r, true);
    const uint32_t firstDelete = r.PendingEffect().token;
    AckDelete(r, false);
    Check(r.RetryBondDelete(), "failed bond deletion has explicit retry");
    Check(r.PendingEffect().token != firstDelete &&
              r.AcknowledgeBondDelete(firstDelete, true) ==
                  Controller::AckResult::Stale,
          "delayed delete ack cannot consume retry effect");
    AckDelete(r, true);
    Save(retry, r, Persistence::Outcome::Unknown);
    Check(r.RetryFinalSave(), "unknown final save has explicit retry");
    Save(retry, r);
    AckSuccess(r);
    Check(r.Pairable() && retry.releases == 4,
          "all save receipts released across retries");
}

void TestDisconnectAndBootClearTransients() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1});
    Check(!c.RegisterAuthenticatedOwnerConnection(6, 0, Security(true, true, 1)) &&
              !c.RegisterAuthenticatedOwnerConnection(6, 7, Security(false, true, 1)) &&
              !c.RegisterAuthenticatedOwnerConnection(6, 7, Security(true, false, 1)) &&
              !c.RegisterAuthenticatedOwnerConnection(6, 7, Security(true, true, 2)),
          "owner reconnect rejects stale or insufficient authentication");
    Check(c.RegisterAuthenticatedOwnerConnection(6, 7, Security(true, true, 1)) &&
              c.HasAuthenticatedOwnerConnection() &&
              c.PendingEffect().type == Effect::None,
          "quiet owner reconnect records addressed authenticated link");
    Check(!c.RegisterAuthenticatedOwnerConnection(8, 9, Security(true, true, 1)),
          "different link cannot replace authenticated owner address");
    c.ArmUnpair(10, 1);
    c.Disconnect(6, 7);
    Check(c.GetOwnership() == Ownership::Owned &&
              c.GetOperation() == Controller::Operation::Unpair &&
              c.GetPhase() == Phase::Armed && !c.HasAuthenticatedOwnerConnection(),
          "reconnected owner disconnect preserves board-local clear confirmation");

    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    c.ArmPair(1, 2, 3, 0);
    c.Disconnect(2, 3);
    Check(c.Pairable(), "disconnect during armed pair cancels operation");

    c.ArmPair(11, 12, 13, 1);
    c.ConfirmPair(11, 12, 13, 1);
    const uint32_t disconnectedSecurityToken = c.PendingEffect().token;
    c.Disconnect(12, 13);
    Check(c.GetPhase() == Phase::SavingTombstone &&
              c.PendingEffect().type == Effect::None &&
              !c.Pairable(),
          "disconnect during pending security conservatively rolls back bond");
    Check(c.AcknowledgeSecurity(disconnectedSecurityToken, 11, 12, 13, 1,
                                Security()) ==
              Controller::AckResult::Stale,
          "late security callback after disconnect cannot revive pairing");
    CompleteCleanup(p, c);
    Check(c.Pairable() && c.GetReason() == Reason::SecurityFailed,
          "security-disconnect rollback restores pairability after durable absent");

    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    c.ArmPair(5, 6, 7, 1);
    c.ConfirmPair(5, 6, 7, 1);
    AckSecurity(c, 5, 6, 7, 1, true, true, 1);
    Save(p, c);
    AckSuccess(c);
    Check(c.HasAuthenticatedOwnerConnection(),
          "successful pair retains authenticated live owner link");
    c.ArmUnpair(8, 1);
    c.Disconnect(6, 7);
    Check(c.GetOwnership() == Ownership::Owned &&
              c.GetOperation() == Controller::Operation::Unpair &&
              c.GetPhase() == Phase::Armed &&
              !c.HasAuthenticatedOwnerConnection(),
          "newly paired owner disconnect preserves board-local clear confirmation");

    Check(!c.ArmUnpair(9, 1), "disconnect cannot replace the selected local clear attempt");
    c.Disconnect(0, 0);
    Check(c.GetOperation() == Controller::Operation::Unpair &&
              c.GetPhase() == Phase::Armed,
          "offline armed unpair ignores accidental default address");
    Check(!c.ConfirmUnpair(9, 1) && c.ConfirmUnpair(8, 1),
          "offline confirmation retains the original local clear attempt and physical end");
    Save(p, c);
    AckQuiesce(c, true);
    AckDelete(c, true);
    Save(p, c);
    AckSuccess(c);
    Check(c.Pairable(), "unauthenticated unpair needs no disconnect effect");

    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    Check(c.Pairable() && c.PendingEffect().type == Effect::None,
          "boot clears transient operation effect and session state");
}

void TestAcknowledgementAdversaries() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0});
    c.ArmPair(1, 2, 3, 0);
    c.ConfirmPair(1, 2, 3, 0);
    Check(!AckSecurity(c, 1, 2, 4, 0, true, true, 1) &&
              c.GetPhase() == Phase::AwaitSecurity,
          "stale authentication cannot consume security effect");
    Check(AckSecurity(c, 1, 2, 3, 0, false, true, 1) &&
              c.GetPhase() == Phase::SavingTombstone,
          "failed security journals created-bond rollback");
    Check(!AckSecurity(c, 1, 2, 3, 0, true, true, 1),
          "duplicate security acknowledgement rejected");
    CompleteCleanup(p, c);
    Check(c.Pairable() && c.GetReason() == Reason::SecurityFailed,
          "security rollback returns unpaired with error");
    Check(!AckDelete(c, true),
          "duplicate bond-delete acknowledgement rejected");

    c.ArmPair(4, 5, 6, 1);
    c.ConfirmPair(4, 5, 6, 1);
    AckSecurity(c, 4, 5, 6, 1, true, true, 1);
    Save(p, c);
    AckSuccess(c);
    c.ArmUnpair(7, 1);
    c.ConfirmUnpair(7, 1);
    Save(p, c);
    AckQuiesce(c, true);
    AckDelete(c, true);
    Save(p, c);
    Check(AckSuccess(c) &&
              c.PendingEffect().type == Effect::Disconnect && !c.Pairable(),
          "authenticated unpair requests optional addressed disconnect");
    const auto disconnect = c.PendingEffect();
    Check(disconnect.connection == 5 && disconnect.epoch == 6,
          "disconnect targets authenticated owner link");
    Check(AckDisconnect(c, true) && c.Pairable(),
          "disconnect acknowledgement completes unpair");
    Check(!AckDisconnect(c, true) && !AckSuccess(c),
          "duplicate terminal acknowledgements rejected");
}

void TestBootDuringPendingReceipt() {
    for (const auto outcome : {Persistence::Outcome::Saved,
                               Persistence::Outcome::Failed,
                               Persistence::Outcome::Unknown}) {
        Persistence p;
        Controller c(p);
        c.Boot(modules::BoardOwnerRecord::Active, {true, 1});
        c.ArmUnpair(12, 0);
        c.ConfirmUnpair(12, 0);
        const auto receipt = p.activeReceipt;
        c.Boot(modules::BoardOwnerRecord::Active, {true, 1});
        Check(c.GetPhase() == Phase::AwaitReceiptRelease &&
                  p.activeReceipt == receipt && p.releases == 0,
              "boot retains pending receipt without releasing it");
        c.PollPersistence();
        Check(p.activeReceipt == receipt && p.releases == 0,
              "poll does not release pending reboot receipt");
        p.outcome = outcome;
        c.PollPersistence();
        Check(p.activeReceipt == 0 && p.releases == 1,
              "terminal reboot receipt releases exactly once");
        if (outcome == Persistence::Outcome::Saved) {
            Check(c.GetPhase() == Phase::AwaitQuiesce &&
                      c.PendingEffect().type == Effect::QuiesceAndRejectWork &&
                      c.GetReason() == Reason::None,
                  "saved reboot receipt resumes original tombstone phase");
        } else {
            Check(c.GetPhase() == Phase::Failed &&
                      c.GetReason() == Reason::StorageFailed &&
                      c.RetryClearingSave(),
                  "failed or unknown reboot receipt exposes clearing retry");
        }
    }
}

void TestUnavailableInventoryNeverGrantsCustody() {
    Persistence p;
    Controller c(p);
    for (const auto record : {modules::BoardOwnerRecord::Absent,
                              modules::BoardOwnerRecord::Active,
                              modules::BoardOwnerRecord::Clearing}) {
        c.Boot(record, {});
        Check(c.GetOwnership() == Ownership::RecoveryRequired &&
                  c.GetReason() == Reason::StorageFailed && !c.Pairable() &&
                  c.PendingEffect().type == Effect::None && p.saves == 0U,
              "failed inventory cannot mean empty, owner ready, or permission to delete keys");
    }
    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U});
    Check(c.Pairable(), "a complete successful inventory can reconcile after startup failure");
}

void TestNativeFailureAndWrongPeerCannotSaveOwner() {
    for (unsigned failure = 0U; failure < 4U; ++failure) {
        Persistence p;
        Controller c(p);
        c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U});
        c.ArmPair(1U, 2U, 3U, 0U);
        c.ConfirmPair(1U, 2U, 3U, 0U);
        auto security = Security();
        if (failure == 0U) security.bonds.available = false;
        else if (failure == 1U) security.nativeSucceeded = false;
        else if (failure == 2U) security.peerMatches = false;
        else {
            security.nativeSucceeded = false;
            security.bonded = false;
            security.bonds.count = 0U;
        }
        Check(c.AcknowledgeSecurity(c.PendingEffect().token, 1U, 2U, 3U,
                                    0U, security) == Controller::AckResult::AcceptedFailure &&
                  p.saves == 1U && p.record == modules::BoardOwnerRecord::Clearing &&
                  c.GetPhase() == Phase::SavingTombstone &&
                  !c.HasAuthenticatedOwnerConnection(),
              "encrypted bonded flag cannot hide inventory, native persistence, or peer failure");
        Check(c.GetReason() == (failure == 0U ? Reason::StorageFailed : Reason::SecurityFailed),
              "unavailable custody remains a storage failure");
        CompleteCleanup(p, c);
        Check(c.Pairable(),
              "possible partial native keys require durable cleanup before another claim");

        c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
        Check(!c.RegisterAuthenticatedOwnerConnection(2U, 3U, security) &&
                  !c.HasAuthenticatedOwnerConnection(),
              "quiet owner reconnect requires complete successful native and peer evidence too");
    }
}

void TestPromptCancellationPreservesLiveLink() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U});
    Check(c.ArmPair(10U, 20U, 30U, 1U), "prompt opens on the addressed live link");
    Check(!c.CancelPair(11U, 20U, 30U) &&
              !c.CancelPair(10U, 21U, 30U) &&
              !c.CancelPair(10U, 20U, 31U) && c.GetPhase() == Phase::Armed,
          "old attempt, handle, or connection epoch cannot cancel a live prompt");
    Check(c.CancelPair(10U, 20U, 30U) && c.Pairable() &&
              c.PendingEffect().type == Effect::None && p.saves == 0U &&
              !c.ConfirmPair(10U, 20U, 30U, 1U),
          "cancel retires unconfirmed consent without security or owner persistence");
    Check(!c.ArmPair(11U, 21U, 30U, 1U) &&
              !c.ArmPair(11U, 20U, 31U, 1U) &&
              c.ArmPair(11U, 20U, 30U, 1U),
          "cancel preserves the native link fence while permitting a new prompt there");
    Check(c.CancelPair(11U, 20U, 30U, Reason::Timeout) &&
              c.GetReason() == Reason::Timeout,
          "prompt expiry uses the same addressed cancellation boundary");
    c.Disconnect(20U, 30U);
    Check(c.ArmPair(12U, 21U, 31U, 0U) && c.ConfirmPair(12U, 21U, 31U, 0U) &&
              !c.CancelPair(12U, 21U, 31U) && c.GetPhase() == Phase::AwaitSecurity,
          "a new connection can claim after disconnect; pending native security needs real teardown");
}

void TestCustodyLossDuringOwnerReceiptCannotReviveAuthority() {
    for (const auto bonds : {Controller::BondInventory{},
                             Controller::BondInventory{true, 0U}}) {
        Persistence p;
        Controller c(p);
        c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U});
        c.ArmPair(1U, 2U, 3U, 0U);
        c.ConfirmPair(1U, 2U, 3U, 0U);
        AckSecurity(c, 1U, 2U, 3U, 0U, true, true, 1U);
        const auto receipt = p.activeReceipt;
        c.Boot(modules::BoardOwnerRecord::Absent, bonds);
        Check(p.activeReceipt == receipt && p.releases == 0U &&
                  c.GetPhase() == Phase::AwaitReceiptRelease &&
                  c.GetOwnership() == Ownership::RecoveryRequired,
              "custody loss preserves the accepted owner receipt while fencing authority");
        Save(p, c);
        Check(p.activeReceipt == 0U && p.releases == 1U &&
                  c.GetOwnership() == Ownership::RecoveryRequired &&
                  !c.HasAuthenticatedOwnerConnection() && !c.Pairable() &&
                  c.GetReason() == Reason::StorageFailed &&
                  c.PendingEffect().type == Effect::None,
              "successful receipt cannot revive ownership or emit success with unavailable or missing custody");
        c.PollPersistence();
        Check(p.releases == 1U, "custody failure does not release the receipt twice");
        c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
        Check(c.GetOwnership() == Ownership::Owned &&
                  !c.HasAuthenticatedOwnerConnection(),
              "fresh durable-record and native inventory reconciliation permits quiet owner reauthentication");
    }
}

void TestCustodyRecheckBlocksStaleClearingRetries() {
    for (const bool finalSave : {false, true}) {
        Persistence p;
        Controller c(p);
        c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
        c.ArmUnpair(1U, 0U);
        c.ConfirmUnpair(1U, 0U);
        if (finalSave) {
            Save(p, c);
            AckQuiesce(c, true);
            AckDelete(c, true);
        }
        Save(p, c, Persistence::Outcome::Failed);
        Check(finalSave ? c.RetryFinalSave() : c.RetryClearingSave(),
              "ordinary failed clearing save retains its explicit retry");
        c.Boot(finalSave ? modules::BoardOwnerRecord::Clearing : modules::BoardOwnerRecord::Active,
               finalSave ? Controller::BondInventory{true, 1U} : Controller::BondInventory{});
        Save(p, c, Persistence::Outcome::Failed);
        const auto saves = p.saves;
        Check(!c.RetryFinalSave() && !c.RetryClearingSave() &&
                  !c.RetryBondDelete() && !c.RetryQuiesce() &&
                  p.saves == saves && c.PendingEffect().type == Effect::None,
              "lost custody fences stale clearing retries until fresh durable reconciliation");
    }
}

void TestLocalClearCancellationPreservesOwner() {
    Persistence p;
    Controller c(p);
    c.Boot(modules::BoardOwnerRecord::Active, {true, 1U});
    c.RegisterAuthenticatedOwnerConnection(2U, 3U, Security());
    Check(c.ArmUnpair(8U, 1U), "owner can arm local clear");
    Check(!c.CancelUnpair(7U, 1U) && !c.CancelUnpair(8U, 0U),
          "wrong attempt or physical end cannot cancel clear");
    Check(c.CancelUnpair(8U, 1U) && c.HasAuthenticatedOwnerConnection() &&
              c.GetOwnership() == Ownership::Owned && p.saves == 0U,
          "leaving unconfirmed clear preserves exact live owner authority");
    Check(!c.RegisterAuthenticatedOwnerConnection(4U, 5U, Security()),
          "clear cancellation cannot silently replace original native link");
    c.ArmUnpair(9U, 1U); c.ConfirmUnpair(9U, 1U);
    Check(!c.CancelUnpair(9U, 1U), "confirmed clear cannot cancel its durable journal");
}

void TestRollbackJournalAdmissionAndRetry() {
    for (bool orphan : {false, true}) {
        Persistence p;
        Controller c(p);
        if (orphan) {
            p.admit = false;
            c.Boot(modules::BoardOwnerRecord::Absent, {true, 1U});
        } else {
            c.Boot(modules::BoardOwnerRecord::Absent, {true, 0U});
            c.ArmPair(1U, 2U, 3U, 0U); c.ConfirmPair(1U, 2U, 3U, 0U);
            p.admit = false;
            auto security = Security(); security.nativeSucceeded = false;
            c.AcknowledgeSecurity(c.PendingEffect().token, 1U, 2U, 3U, 0U, security);
        }
        Check(c.GetPhase() == Phase::Failed && !c.Pairable() &&
                  c.PendingEffect().type == Effect::None && p.saves == 0U,
              "rejected Clearing journal cannot delete native keys or admit another claim");
        p.admit = true;
        Check(c.RetryClearingSave(), "pair/orphan cleanup retries original Clearing save");
        Save(p, c); AckQuiesce(c, true);
        AckDelete(c, false);
        Check(c.RetryBondDelete(), "journaled pair/orphan cleanup can retry native deletion");
        AckDelete(c, true);
        Save(p, c, Persistence::Outcome::Failed);
        Check(!c.Pairable() && c.RetryFinalSave(), "failed absent save keeps cleanup unavailable until retry");
        Save(p, c);
        Check(c.Pairable() && c.PendingEffect().type == Effect::None &&
                  (orphan || c.GetReason() == Reason::SecurityFailed),
              "complete durable cleanup restores unpaired and original pair error without success effect");
    }
}

} // namespace

int main() {
    TestBootReconciliation();
    TestThreeSavedPhonesAndJournalBeforeSecurity();
    TestAddRollbackAndRecoveryNeverClearAll();
    TestAddRetirementWhileJournalReceiptPending();
    TestLocalClearCancellationPreservesOwner();
    TestRollbackJournalAdmissionAndRetry();
    TestPairAddressingAndRollback();
    TestOfflineUnpairOrderingAndRetries();
    TestDisconnectAndBootClearTransients();
    TestAcknowledgementAdversaries();
    TestBootDuringPendingReceipt();
    TestUnavailableInventoryNeverGrantsCustody();
    TestNativeFailureAndWrongPeerCannotSaveOwner();
    TestPromptCancellationPreservesLiveLink();
    TestCustodyLossDuringOwnerReceiptCannotReviveAuthority();
    TestCustodyRecheckBlocksStaleClearingRetries();
    if (failures == 0) std::printf("[BOARD_OWNER_PAIRING_CONTROLLER] ok\n");
    return failures == 0 ? 0 : 1;
}
