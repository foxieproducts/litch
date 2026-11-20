#include "modules/game_history_capture.hpp"

#include <array>
#include <cstring>

namespace modules::game_history {
namespace {

bool IsPositionRecord(const RecordType type) {
    return type == RecordType::LineBase ||
           type == RecordType::CommittedPly;
}

struct PositionAuthority {
    std::array<char, 64U> board{};
    unsigned whiteKings = 0U;
    unsigned blackKings = 0U;
    char nextSide = 0;
};

bool ReadAuthority(const Position& position, PositionAuthority& out) {
    out = {};
    const auto variant = position.bytes[POSITION_BYTES - 1U] == INVALID_SQUARE
                             ? Variant::Classic : Variant::Duck;
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> fen;
    uint8_t duck = INVALID_SQUARE;
    if (!DecodePosition(position, variant, fen, duck)) return false;
    const auto text = fen.View();
    size_t square = 0U;
    for (size_t index = 0U; index < text.size(); ++index) {
        const char token = text[index];
        if (token == ' ') { out.nextSide = text[index + 1U]; break; }
        if (token == '/') continue;
        if (token >= '1' && token <= '8') { square += token - '0'; continue; }
        if (square >= out.board.size()) return false;
        out.board[square++] = token;
        if (token == 'K') ++out.whiteKings;
        if (token == 'k') ++out.blackKings;
    }
    return square == 64U;
}

} // namespace

bool GameHistoryCapture::BuildPriorSeal(const Record& latest, Record& seal) const {
    seal = {};
    seal.type = RecordType::Seal;
    seal.gameId = latest.gameId;
    seal.lineRevision = latest.lineRevision;
    PositionAuthority position;
    if (!ReadAuthority(latest.position, position)) return false;
    if (position.whiteKings == 1U && position.blackKings == 1U) return true;
    // A durable terminal transition can recover its missing result. A terminal
    // base alone cannot prove a capture and must remain diagnostic evidence.
    const auto& move = latest.transition;
    const bool white = move.side == Side::White;
    if (latest.type != RecordType::CommittedPly ||
        latest.position.bytes[POSITION_BYTES - 1U] == INVALID_SQUARE ||
        (white ? position.whiteKings != 1U || position.blackKings != 0U
               : position.blackKings != 1U || position.whiteKings != 0U) ||
        move.from >= 64U || move.to >= 64U || move.captureSquare != move.to ||
        move.duckFrom != INVALID_SQUARE || move.duckTo != INVALID_SQUARE ||
        position.board[move.from] != 0 ||
        position.nextSide != (white ? 'b' : 'w')) return false;
    const char piece = position.board[move.to];
    if (white ? piece < 'A' || piece > 'Z' : piece < 'a' || piece > 'z') return false;
    seal.status = ArchiveStatus::Completed;
    seal.outcome = Outcome::KingCaptured;
    seal.winner = white ? Winner::White : Winner::Black;
    return true;
}

GameHistoryCaptureResult GameHistoryCapture::MapMutation(
    const JournalMutationResult result) const {
    switch (result) {
        case JournalMutationResult::Applied:
            return GameHistoryCaptureResult::Applied;
        case JournalMutationResult::Bounds:
            return GameHistoryCaptureResult::Bounds;
        case JournalMutationResult::Capacity:
            return GameHistoryCaptureResult::Capacity;
        case JournalMutationResult::Corrupt:
            return GameHistoryCaptureResult::Corrupt;
        case JournalMutationResult::IoFailure:
            return GameHistoryCaptureResult::IoFailure;
        case JournalMutationResult::Exhausted:
            return GameHistoryCaptureResult::Exhausted;
    }
    return GameHistoryCaptureResult::Corrupt;
}

bool GameHistoryCapture::ValidateSnapshot(
    const GameHistoryCaptureSnapshot& snapshot) const {
    if (!IsValidGameId(snapshot.gameId) ||
        snapshot.lineRevision > MAX_LINE_REVISION ||
        !IsValidVariant(snapshot.variant) ||
        snapshot.basePly > 4096U ||
        snapshot.transitionCount > 4096U - snapshot.basePly ||
        (snapshot.transitionCount != 0U && snapshot.readStep == nullptr) ||
        (snapshot.shouldSeal &&
         !IsValidSeal(snapshot.status, snapshot.outcome,
                      snapshot.winner))) {
        return false;
    }
    PositionAuthority base;
    if (!ReadAuthority(snapshot.basePosition, base) ||
        base.whiteKings != 1U || base.blackKings != 1U) return false;

    Record checked;
    checked.type = RecordType::LineBase;
    checked.outboxSequence = 1U;
    checked.gameId = snapshot.gameId;
    checked.lineRevision = snapshot.lineRevision;
    checked.ply = snapshot.basePly;
    checked.variant = snapshot.variant;
    checked.position = snapshot.basePosition;
    std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
    size_t encodedLength = 0U;
    if (!EncodeRecord(checked, bytes.data(), bytes.size(), encodedLength)) {
        return false;
    }
    Record decoded;
    if (!DecodeRecord(bytes.data(), encodedLength, decoded)) {
        return false;
    }

    for (uint16_t index = 0U; index < snapshot.transitionCount; ++index) {
        GameHistoryCaptureStep step;
        if (!snapshot.readStep(snapshot.stepContext, index, step)) {
            return false;
        }
        checked = {};
        checked.type = RecordType::CommittedPly;
        checked.outboxSequence = 1U;
        checked.gameId = snapshot.gameId;
        checked.lineRevision = snapshot.lineRevision;
        checked.ply = static_cast<uint16_t>(snapshot.basePly + index + 1U);
        checked.position = step.afterPosition;
        checked.transition = step.transition;
        if (!EncodeRecord(checked, bytes.data(), bytes.size(), encodedLength) ||
            !DecodeRecord(bytes.data(), encodedLength, decoded)) {
            return false;
        }
        const bool isDuck =
            step.afterPosition.bytes[POSITION_BYTES - 1U] != INVALID_SQUARE;
        if ((snapshot.variant == Variant::Duck) != isDuck) {
            return false;
        }
    }
    return true;
}

bool GameHistoryCapture::ReadPositionAt(
    const GameHistoryCaptureSnapshot& snapshot,
    const uint16_t ply,
    Position& outPosition) const {
    if (ply < snapshot.basePly || ply > snapshot.CurrentPly()) {
        return false;
    }
    if (ply == snapshot.basePly) {
        outPosition = snapshot.basePosition;
        return true;
    }
    GameHistoryCaptureStep step;
    if (snapshot.readStep == nullptr ||
        !snapshot.readStep(snapshot.stepContext,
                           static_cast<uint16_t>(ply - snapshot.basePly - 1U),
                           step)) {
        return false;
    }
    outPosition = step.afterPosition;
    return true;
}

GameHistoryCaptureResult GameHistoryCapture::AppendLineBase(
    const GameHistoryCaptureSnapshot& snapshot) {
    const JournalSummary summary = m_journal.Summary();
    if (summary.sequenceExhausted || summary.newestAllocated == UINT32_MAX) {
        return GameHistoryCaptureResult::Exhausted;
    }
    Record record;
    record.type = RecordType::LineBase;
    record.outboxSequence = summary.newestAllocated + 1U;
    record.gameId = snapshot.gameId;
    record.lineRevision = snapshot.lineRevision;
    record.ply = snapshot.basePly;
    record.variant = snapshot.variant;
    record.position = snapshot.basePosition;
    return MapMutation(m_journal.Append(record));
}

GameHistoryCaptureResult GameHistoryCapture::AppendRebasedLine(
    GameHistoryCaptureSnapshot& snapshot) {
    Record anchor;
    const auto read = m_journal.ReadCurrentLinePosition(
        snapshot.gameId, snapshot.basePly, snapshot.lineRevision, anchor);
    if (read == JournalSegmentRead::Corrupt) return GameHistoryCaptureResult::Corrupt;
    if (read != JournalSegmentRead::Ready && read != JournalSegmentRead::Unavailable) {
        return GameHistoryCaptureResult::IoFailure;
    }
    if (read == JournalSegmentRead::Ready && anchor.variant == snapshot.variant &&
        SamePositionAuthority(anchor.position, snapshot.basePosition)) {
        // Preserve the exact archived overlap and seed all replacement counters
        // from it. An absent/mismatched anchor remains an explicit partial base.
        snapshot.basePosition = anchor.position;
    }
    return AppendLineBase(snapshot);
}

GameHistoryCaptureResult GameHistoryCapture::AppendSuffix(
    const GameHistoryCaptureSnapshot& snapshot,
    const uint16_t firstPly, const Position* continuation) {
    if (firstPly > snapshot.CurrentPly()) {
        return GameHistoryCaptureResult::NoChange;
    }
    Position previous;
    if (continuation != nullptr) previous = *continuation;
    else if (!ReadPositionAt(snapshot, static_cast<uint16_t>(firstPly - 1U), previous)) {
        return GameHistoryCaptureResult::Bounds;
    }
    for (uint16_t ply = firstPly; ply <= snapshot.CurrentPly(); ++ply) {
        GameHistoryCaptureStep step;
        if (snapshot.readStep == nullptr ||
            !snapshot.readStep(snapshot.stepContext,
                               static_cast<uint16_t>(
                                   ply - snapshot.basePly - 1U),
                               step)) {
            return GameHistoryCaptureResult::Bounds;
        }
        const JournalSummary summary = m_journal.Summary();
        if (summary.sequenceExhausted ||
            summary.newestAllocated == UINT32_MAX) {
            return GameHistoryCaptureResult::Exhausted;
        }
        Record record;
        record.type = RecordType::CommittedPly;
        record.outboxSequence = summary.newestAllocated + 1U;
        record.gameId = snapshot.gameId;
        record.lineRevision = snapshot.lineRevision;
        record.ply = ply;
        record.position = step.afterPosition;
        record.transition = step.transition;
        if (!AdvancePositionCounters(previous, record.transition, record.position)) {
            return GameHistoryCaptureResult::Bounds;
        }
        const GameHistoryCaptureResult appended =
            MapMutation(m_journal.Append(record));
        if (appended != GameHistoryCaptureResult::Applied) {
            return appended;
        }
        previous = record.position;
        if (ply == UINT16_MAX) {
            return GameHistoryCaptureResult::Exhausted;
        }
    }
    return GameHistoryCaptureResult::Applied;
}

GameHistoryCaptureResult GameHistoryCapture::AppendSeal(
    const GameId& gameId,
    const uint64_t lineRevision,
    const ArchiveStatus status,
    const Outcome outcome,
    const Winner winner) {
    const JournalSummary summary = m_journal.Summary();
    if (summary.sequenceExhausted || summary.newestAllocated == UINT32_MAX) {
        return GameHistoryCaptureResult::Exhausted;
    }
    Record record;
    record.type = RecordType::Seal;
    record.outboxSequence = summary.newestAllocated + 1U;
    record.gameId = gameId;
    record.lineRevision = lineRevision;
    record.status = status;
    record.outcome = outcome;
    record.winner = winner;
    return MapMutation(m_journal.Append(record));
}

GameHistoryCaptureResult GameHistoryCapture::Reconcile(
    const GameHistoryCaptureSnapshot& input) {
    GameHistoryCaptureSnapshot snapshot = input;
    if (!ValidateSnapshot(snapshot)) {
        return GameHistoryCaptureResult::Bounds;
    }

    Record latest;
    const JournalSegmentRead latestRead =
        m_journal.ReadLatestRecord(latest);
    if (latestRead == JournalSegmentRead::Corrupt) {
        return GameHistoryCaptureResult::Corrupt;
    }
    if (latestRead != JournalSegmentRead::Ready &&
        latestRead != JournalSegmentRead::Empty) {
        return GameHistoryCaptureResult::IoFailure;
    }

    bool changed = false;
    const Position* continuation = nullptr;
    uint16_t firstMissingPly = snapshot.basePly;
    if (latestRead == JournalSegmentRead::Empty) {
        const GameHistoryCaptureResult base = AppendLineBase(snapshot);
        if (base != GameHistoryCaptureResult::Applied) return base;
        changed = true;
        firstMissingPly = static_cast<uint16_t>(snapshot.basePly + 1U);
    } else if (latest.gameId != snapshot.gameId) {
        if (latest.type != RecordType::Seal) {
            Record recoveredSeal;
            if (!BuildPriorSeal(latest, recoveredSeal)) return GameHistoryCaptureResult::Corrupt;
            const GameHistoryCaptureResult priorSeal = AppendSeal(
                recoveredSeal.gameId, recoveredSeal.lineRevision,
                recoveredSeal.status, recoveredSeal.outcome, recoveredSeal.winner);
            if (priorSeal != GameHistoryCaptureResult::Applied) {
                return priorSeal;
            }
            changed = true;
        }
        const GameHistoryCaptureResult base = AppendLineBase(snapshot);
        if (base != GameHistoryCaptureResult::Applied) return base;
        changed = true;
        firstMissingPly = static_cast<uint16_t>(snapshot.basePly + 1U);
    } else if (latest.type == RecordType::Seal) {
        if (snapshot.lineRevision > latest.lineRevision) {
            const auto base = AppendRebasedLine(snapshot);
            if (base != GameHistoryCaptureResult::Applied) return base;
            changed = true;
            firstMissingPly = static_cast<uint16_t>(snapshot.basePly + 1U);
        } else {
            return snapshot.shouldSeal &&
                       latest.lineRevision == snapshot.lineRevision &&
                       latest.status == snapshot.status &&
                       latest.outcome == snapshot.outcome &&
                       latest.winner == snapshot.winner
                   ? GameHistoryCaptureResult::NoChange
                   : GameHistoryCaptureResult::Bounds;
        }
    } else {
        if (latest.lineRevision > snapshot.lineRevision ||
            !IsPositionRecord(latest.type)) {
            return GameHistoryCaptureResult::Bounds;
        }
        Position snapshotAtLatest;
        const bool latestPositionRetained = ReadPositionAt(
            snapshot, latest.ply, snapshotAtLatest);
        // Every append and Undo advances the durable revision. A newer
        // revision can extend this prefix only when all mutations were appends.
        // Equal revision is the retry of a partially captured immutable batch.
        const bool appendOnly = snapshot.lineRevision == latest.lineRevision ||
            (snapshot.CurrentPly() > latest.ply &&
             snapshot.lineRevision - latest.lineRevision ==
                 static_cast<uint64_t>(snapshot.CurrentPly() - latest.ply));
        if (appendOnly && latestPositionRetained &&
            SamePositionAuthority(latest.position, snapshotAtLatest)) {
            firstMissingPly = static_cast<uint16_t>(latest.ply + 1U);
            continuation = &latest.position;
        } else {
            if (snapshot.lineRevision <= latest.lineRevision) {
                return GameHistoryCaptureResult::Bounds;
            }
            const GameHistoryCaptureResult base = AppendRebasedLine(snapshot);
            if (base != GameHistoryCaptureResult::Applied) return base;
            changed = true;
            firstMissingPly = static_cast<uint16_t>(snapshot.basePly + 1U);
        }
    }

    const GameHistoryCaptureResult suffix =
        AppendSuffix(snapshot, firstMissingPly, continuation);
    if (suffix != GameHistoryCaptureResult::Applied &&
        suffix != GameHistoryCaptureResult::NoChange) {
        return suffix;
    }
    changed = changed || suffix == GameHistoryCaptureResult::Applied;

    if (snapshot.shouldSeal) {
        Record afterSuffix;
        const JournalSegmentRead afterRead =
            m_journal.ReadLatestRecord(afterSuffix);
        if (afterRead != JournalSegmentRead::Ready) {
            return afterRead == JournalSegmentRead::Corrupt
                       ? GameHistoryCaptureResult::Corrupt
                       : GameHistoryCaptureResult::IoFailure;
        }
        if (afterSuffix.type != RecordType::Seal) {
            const GameHistoryCaptureResult sealed = AppendSeal(
                snapshot.gameId, snapshot.lineRevision, snapshot.status,
                snapshot.outcome, snapshot.winner);
            if (sealed != GameHistoryCaptureResult::Applied) return sealed;
            changed = true;
        }
    }
    return changed ? GameHistoryCaptureResult::Applied
                   : GameHistoryCaptureResult::NoChange;
}

} // namespace modules::game_history
