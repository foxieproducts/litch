#include "firmware/current_game_log_service.hpp"

#ifdef ARDUINO

#include <array>
#include <new>

#include "current_game_log_service_internal.hpp"
#include "modules/current_game_log_persistence.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_history_snapshot_reader.hpp"

namespace firmware {
namespace {

using modules::current_game_log::CommitPointer;
using modules::current_game_log::LineSource;
using modules::current_game_log::Result;
using modules::game_history::Record;
using modules::game_history::RecordType;
using current_game_log_service_internal::RequiresArchive;
using current_game_log_service_internal::SameGame;
using current_game_log_service_internal::StorageBatch;

} // namespace

struct CurrentGameLogService::LineContext {
    CurrentGameLogService* service = nullptr;
    const modules::game_history::GameHistoryCaptureSnapshot* snapshot = nullptr;
    const CommitPointer* authority = nullptr;
    uint64_t baseRevision = 0U;
    uint32_t storedOffset = 0U;
    uint16_t storedNextIndex = 0U;
    uint16_t recordCount = 0U;
    bool useStoredPrefix = false;
};

CurrentGameLogService::CurrentGameLogService(
    const modules::current_game_log::StorageOps storage)
    : m_storage(storage),
      m_settingsOps{this, &EnqueuePointerThunk},
      m_log(m_storage, m_settingsOps) {}

bool CurrentGameLogService::Configure(
    SettingsPersistenceWorker& worker,
    const JsonDocument& initialDocument) {
    CommitPointer initialPointer;
    if (m_configured ||
        !modules::current_game_log::ReadCommitPointer(
            initialDocument, initialPointer) ||
        !worker.SetSnapshotTransactionCallbacks(
            &PrepareThunk, &ResultThunk, this)) {
        return false;
    }
    m_committedPointer = initialPointer;
    m_worker = &worker;
    m_configured = true;
    return true;
}

bool CurrentGameLogService::PrepareThunk(
    void* const context,
    JsonDocument& document) {
    return context != nullptr &&
        static_cast<CurrentGameLogService*>(context)->Prepare(document);
}

void CurrentGameLogService::ResultThunk(
    void* const context,
    const SettingsPersistenceWorker::SnapshotTransactionOutcome outcome,
    const JsonDocument&) {
    if (context != nullptr) {
        static_cast<CurrentGameLogService*>(context)->Complete(outcome);
    }
}

bool CurrentGameLogService::EnqueuePointerThunk(
    void* const context,
    const CommitPointer& pointer,
    modules::current_game_log::SettingsOps::Receipt& outReceipt) {
    return context != nullptr &&
        static_cast<CurrentGameLogService*>(context)->EnqueuePointer(
            pointer, outReceipt);
}

bool CurrentGameLogService::ReadLineRecordThunk(
    void* const context,
    const uint16_t index,
    Record& out) {
    if (context == nullptr) return false;
    auto& line = *static_cast<LineContext*>(context);
    return line.service != nullptr &&
        line.service->BuildLineRecord(line, index, out);
}

bool CurrentGameLogService::BuildSnapshotRecord(
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
    const uint16_t ply,
    const uint64_t revision,
    Record& out) const {
    out = {};
    out.outboxSequence = static_cast<uint32_t>(ply) + 1U;
    out.gameId = snapshot.gameId;
    out.lineRevision = revision;
    out.ply = ply;
    if (ply == 0U) {
        if (snapshot.basePly != 0U) return false;
        out.type = RecordType::LineBase;
        out.variant = snapshot.variant;
        out.position = snapshot.basePosition;
        return true;
    }
    if (ply <= snapshot.basePly || snapshot.readStep == nullptr) {
        return false;
    }
    modules::game_history::GameHistoryCaptureStep step;
    if (!snapshot.readStep(
            snapshot.stepContext,
            static_cast<uint16_t>(ply - snapshot.basePly - 1U), step)) {
        return false;
    }
    out.type = RecordType::CommittedPly;
    out.position = step.afterPosition;
    out.transition = step.transition;
    return true;
}

bool CurrentGameLogService::ReadStoredRecord(
    LineContext& context,
    const uint16_t index,
    Record& out) const {
    if (context.authority == nullptr || index > context.authority->currentPly) {
        return false;
    }
    if (index < context.storedNextIndex) {
        context.storedOffset = 0U;
        context.storedNextIndex = 0U;
    }
    while (context.storedNextIndex <= index) {
        if (context.storedOffset >= context.authority->byteLength) return false;
        uint8_t header[modules::game_history::RECORD_HEADER_BYTES] = {};
        if (!m_storage.read(
                m_storage.context, context.authority->slot,
                context.storedOffset, header, sizeof(header))) {
            return false;
        }
        const size_t length = modules::game_history::RECORD_HEADER_BYTES +
            static_cast<size_t>(header[2U]) +
            modules::game_history::RECORD_CRC_BYTES;
        if (length > modules::game_history::MAX_RECORD_BYTES ||
            length > context.authority->byteLength - context.storedOffset) {
            return false;
        }
        std::array<uint8_t, modules::game_history::MAX_RECORD_BYTES> encoded{};
        if (!m_storage.read(
                m_storage.context, context.authority->slot,
                context.storedOffset, encoded.data(), length) ||
            !modules::game_history::DecodeRecord(
                encoded.data(), length, out)) {
            return false;
        }
        context.storedOffset += static_cast<uint32_t>(length);
        if (context.storedNextIndex == index) {
            ++context.storedNextIndex;
            return out.type != RecordType::Seal;
        }
        ++context.storedNextIndex;
    }
    return false;
}

bool CurrentGameLogService::BuildLineRecord(
    LineContext& context,
    const uint16_t index,
    Record& out) {
    if (context.snapshot == nullptr || index >= context.recordCount) {
        return false;
    }
    const auto& snapshot = *context.snapshot;
    const uint16_t positionRecords =
        static_cast<uint16_t>(snapshot.CurrentPly() + 1U);
    if (index == positionRecords) {
        if (!snapshot.shouldSeal || index + 1U != context.recordCount) {
            return false;
        }
        out = {};
        out.type = RecordType::Seal;
        out.outboxSequence = static_cast<uint32_t>(index) + 1U;
        out.gameId = snapshot.gameId;
        out.lineRevision = snapshot.lineRevision;
        out.status = snapshot.status;
        out.outcome = snapshot.outcome;
        out.winner = snapshot.winner;
        return true;
    }

    const uint16_t ply = index;
    const uint64_t revision = context.baseRevision + ply;
    if (context.useStoredPrefix && ply <= snapshot.basePly) {
        if (!ReadStoredRecord(context, index, out) ||
            !SameGame(out.gameId, snapshot.gameId) || out.ply != ply) {
            return false;
        }
        out.outboxSequence = static_cast<uint32_t>(index) + 1U;
        out.lineRevision = revision;
        if (ply == snapshot.basePly &&
            !modules::game_history::SamePositionAuthority(
                out.position, snapshot.basePosition)) {
            return false;
        }
        return true;
    }
    return BuildSnapshotRecord(snapshot, ply, revision, out);
}

bool CurrentGameLogService::RewriteSnapshot(
    const CommitPointer& authority,
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot) {
    if (snapshot.basePly > authority.currentPly ||
        snapshot.lineRevision <
            static_cast<uint64_t>(snapshot.CurrentPly()) + 1U) {
        return Fail(Result::Bounds);
    }
    LineContext context;
    context.service = this;
    context.snapshot = &snapshot;
    context.authority = &authority;
    context.baseRevision = snapshot.lineRevision - snapshot.CurrentPly();
    context.recordCount = static_cast<uint16_t>(
        snapshot.CurrentPly() + 1U + (snapshot.shouldSeal ? 1U : 0U));
    context.useStoredPrefix = snapshot.basePly != 0U;
    const LineSource source{
        &context, context.recordCount, &ReadLineRecordThunk};
    CommitPointer candidate;
    modules::current_game_log::SettingsOps::Receipt receipt = 0U;
    const Result result = m_log.Rewrite(
        authority, source, candidate, receipt);
    if (result != Result::Applied) return Fail(result);
    m_lastResult = result;
    return true;
}

bool CurrentGameLogService::ReplaceSnapshot(
    JsonDocument& document,
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
    const CommitPointer& authority) {
    if (snapshot.basePly != 0U ||
        snapshot.lineRevision <
            static_cast<uint64_t>(snapshot.CurrentPly()) + 1U) {
        return Fail(Result::Bounds);
    }
    const Result opened = EnsureOpen(authority);
    if (opened != Result::Ready) return Fail(opened);
    m_preparedDocument = &document;
    LineContext context;
    context.service = this;
    context.snapshot = &snapshot;
    context.baseRevision = snapshot.lineRevision - snapshot.CurrentPly();
    context.recordCount = static_cast<uint16_t>(
        snapshot.CurrentPly() + 1U + (snapshot.shouldSeal ? 1U : 0U));
    const LineSource source{
        &context, context.recordCount, &ReadLineRecordThunk};
    CommitPointer candidate;
    modules::current_game_log::SettingsOps::Receipt receipt = 0U;
    const Result result = m_log.ReplaceLine(
        authority, source,
        RequiresArchive(snapshot),
        candidate, receipt);
    m_preparedDocument = nullptr;
    if (result != Result::Applied) return Fail(result);
    m_lastResult = result;
    return true;
}

bool CurrentGameLogService::ReconcileSnapshot(
    JsonDocument& document,
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
    const CommitPointer& authority) {
    m_preparedDocument = &document;
    if (!authority.present) {
        if (snapshot.basePly != 0U ||
            snapshot.lineRevision <
                static_cast<uint64_t>(snapshot.CurrentPly()) + 1U) {
            m_preparedDocument = nullptr;
            return Fail(Result::Bounds);
        }
        ResetLog();
        LineContext context;
        context.service = this;
        context.snapshot = &snapshot;
        context.baseRevision = snapshot.lineRevision - snapshot.CurrentPly();
        context.recordCount = static_cast<uint16_t>(
            snapshot.CurrentPly() + 1U + (snapshot.shouldSeal ? 1U : 0U));
        const LineSource source{
            &context, context.recordCount, &ReadLineRecordThunk};
        CommitPointer candidate;
        modules::current_game_log::SettingsOps::Receipt receipt = 0U;
        const Result result = m_log.InitializeLine(
            source, RequiresArchive(snapshot),
            candidate, receipt);
        m_preparedDocument = nullptr;
        if (result != Result::Applied) return Fail(result);
        m_lastResult = result;
        return true;
    }
    const Result opened = EnsureOpen(authority);
    if (opened != Result::Ready) {
        m_preparedDocument = nullptr;
        return Fail(opened);
    }

    Record latest;
    const Result latestResult = m_log.ReadLatest(authority, latest);
    if (latestResult != Result::Ready) {
        m_preparedDocument = nullptr;
        return Fail(latestResult);
    }
    // Free setup relocation can be saved separately or coalesce into turn one.
    // Replace only an unplayed Duck base, preserving the old bytes until promotion.
    if (current_game_log_service_internal::CanRebaseUnplayedDuckOpening(authority, snapshot, latest))
        return ReplaceSnapshot(document, snapshot, authority);
    const bool sameRevision =
        snapshot.lineRevision == authority.lineRevision;
    const bool samePly = snapshot.CurrentPly() == authority.currentPly;
    if (sameRevision && samePly) {
        if (snapshot.shouldSeal && latest.type != RecordType::Seal) {
            Record seal;
            seal.type = RecordType::Seal;
            seal.outboxSequence = latest.outboxSequence + 1U;
            seal.gameId = snapshot.gameId;
            seal.lineRevision = snapshot.lineRevision;
            seal.status = snapshot.status;
            seal.outcome = snapshot.outcome;
            seal.winner = snapshot.winner;
            CommitPointer candidate;
            modules::current_game_log::SettingsOps::Receipt receipt = 0U;
            const Result result = m_log.Append(
                authority, seal, candidate, receipt);
            m_preparedDocument = nullptr;
            if (result != Result::Applied) return Fail(result);
            m_lastResult = result;
            return true;
        }
        m_preparedDocument = nullptr;
        const bool exactSeal = snapshot.shouldSeal
            ? latest.type == RecordType::Seal &&
                latest.status == snapshot.status &&
                latest.outcome == snapshot.outcome &&
                latest.winner == snapshot.winner
            : latest.type != RecordType::Seal;
        return exactSeal || Fail(Result::Bounds);
    }
    if (snapshot.lineRevision <= authority.lineRevision) {
        m_preparedDocument = nullptr;
        return Fail(Result::Bounds);
    }

    const uint16_t missing = snapshot.CurrentPly() > authority.currentPly
        ? static_cast<uint16_t>(snapshot.CurrentPly() - authority.currentPly)
        : 0U;
    const bool singleAppend = missing == 1U && !snapshot.shouldSeal &&
        snapshot.lineRevision == authority.lineRevision + 1U &&
        snapshot.basePly <= authority.currentPly;
    if (singleAppend) {
        Record record;
        const bool built = BuildSnapshotRecord(
            snapshot, snapshot.CurrentPly(), snapshot.lineRevision, record);
        modules::game_history::Position anchor = snapshot.basePosition;
        bool anchorRead = true;
        if (snapshot.basePly != authority.currentPly) {
            modules::game_history::GameHistoryCaptureStep step;
            anchorRead = snapshot.readStep != nullptr && snapshot.readStep(
                snapshot.stepContext,
                static_cast<uint16_t>(
                    authority.currentPly - snapshot.basePly - 1U), step);
            if (anchorRead) anchor = step.afterPosition;
        }
        if (!built || !anchorRead ||
            !modules::game_history::SamePositionAuthority(
                latest.position, anchor)) {
            m_preparedDocument = nullptr;
            return Fail(Result::Bounds);
        }
        CommitPointer candidate;
        modules::current_game_log::SettingsOps::Receipt receipt = 0U;
        const Result result = m_log.Append(
            authority, record, candidate, receipt);
        m_preparedDocument = nullptr;
        if (result != Result::Applied) return Fail(result);
        m_lastResult = result;
        return true;
    }
    const bool rewritten = RewriteSnapshot(authority, snapshot);
    m_preparedDocument = nullptr;
    return rewritten;
}

bool CurrentGameLogService::Prepare(JsonDocument& document) {
    if (m_pendingReceipt != 0U) return Fail(Result::BadState);
    const StorageBatch batch(m_storage);
    if (!batch.IsReady()) return Fail(Result::IoFailure);
    m_pendingRemoval = false;
    CommitPointer authority = m_committedPointer;
    if (!modules::current_game_log::WriteCommitPointer(
            document, authority)) {
        return Fail(Result::SettingsRejected);
    }
    if (m_worker == nullptr) return Fail(Result::BadState);
    void* const scratch = m_worker->ActiveSnapshotTransactionScratch(
        sizeof(modules::GamePersistenceState),
        alignof(modules::GamePersistenceState));
    if (scratch == nullptr) return Fail(Result::BadState);
    auto* const state = new (scratch) modules::GamePersistenceState{};
    struct StateLifetime final {
        modules::GamePersistenceState* state;
        ~StateLifetime() { state->~GamePersistenceState(); }
    } stateLifetime{state};
    if (m_archiveRequestPending) {
        const bool snapshotMatches =
            modules::ReadGamePersistenceState(document, *state) &&
            state->gameHistory.valid &&
            state->gameHistory.present &&
            !state->gameHistory.captureIncomplete &&
            SameGame(state->gameHistory.gameId,
                     m_archiveRequestAuthority.gameId) &&
            state->gameHistory.lineRevision ==
                m_archiveRequestAuthority.lineRevision &&
            state->gameHistory.currentPly ==
                m_archiveRequestAuthority.currentPly;
        if (!SameGame(authority.gameId, m_archiveRequestAuthority.gameId) ||
            authority.slot != m_archiveRequestAuthority.slot ||
            authority.generation != m_archiveRequestAuthority.generation ||
            authority.byteLength != m_archiveRequestAuthority.byteLength ||
            authority.prefixCrc32 != m_archiveRequestAuthority.prefixCrc32 ||
            authority.lineRevision != m_archiveRequestAuthority.lineRevision ||
            authority.currentPly != m_archiveRequestAuthority.currentPly ||
            !authority.present || !authority.archiveRequired ||
            !snapshotMatches) {
            m_archiveRequestPending = false;
            m_archiveSaveRequested.store(false, std::memory_order_release);
            m_archiveCompletion.store(2U, std::memory_order_release);
            // Retire only the stale acknowledgement. The newer game snapshot
            // still owns its ordinary validation and atomic save below.
        } else {
            return PrepareArchiveAcknowledgement(document, authority);
        }
    }
    if (!document.containsKey(modules::KEY_GAME_HISTORY_V1) ||
        !document.containsKey(modules::KEY_GAME_FEN)) {
        m_lastResult = Result::Ready;
        return true;
    }
    if (!modules::ReadGamePersistenceState(document, *state)) {
        return authority.present ? Fail(Result::Corrupt) : true;
    }
    if (!state->gameHistory.valid ||
        !state->gameHistory.present ||
        state->gameHistory.captureIncomplete) {
        return authority.present ? Fail(Result::Bounds) : true;
    }
    const bool replacing = authority.present &&
        !SameGame(authority.gameId, state->gameHistory.gameId);
    modules::game_history::GameHistorySnapshotReader reader(*state);
    modules::game_history::GameHistoryCaptureSnapshot snapshot;
    if (!reader.Build(snapshot)) {
        modules::DuckGameDurableState variantState;
        const bool variantUndecided = modules::DecodeDuckGameDurableState(
            state->variantState.CStr(), variantState) &&
            variantState.variant == modules::GameVariant::Undecided;
        // Explicit New Game may retire a different identity without archival.
        // Same-game Undo to zero retains the stricter empty-opening guard.
        const bool freshNewGame = replacing && variantUndecided &&
            state->gameHistory.lineRevision == 1U &&
            state->gameHistory.currentPly == 0U;
        if (freshNewGame ||
            modules::current_game_log::CanRetireUndecidedOpening(
                authority, state->gameHistory.currentPly,
                variantUndecided)) {
            if (authority.present) return PrepareReplacement(document);
            m_lastResult = Result::Ready;
            return true;
        }
        return Fail(Result::Bounds);
    }
    if (replacing) return ReplaceSnapshot(document, snapshot, authority);
    return ReconcileSnapshot(document, snapshot, authority);
}

} // namespace firmware

#endif
