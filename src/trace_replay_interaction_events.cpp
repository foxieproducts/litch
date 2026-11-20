#include "trace/trace_replay_interaction_events.hpp"

#include <cstdio>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/string_parsing.hpp"
#include "runtime/app_event_dispatch.hpp"

namespace trace {
namespace {

bool ReadCaptureRows(std::string_view text, runtime::CaptureLegality& out) {
    if (text.empty()) return true; // Records created before exact capture authority.
    const auto readWord = [&text](uint64_t& word) {
        if (text.size() < 16U) return false;
        word = 0ULL;
        for (size_t i = 0U; i < 16U; i += 2U) {
            uint8_t byte = 0U;
            if (!modules::string_parsing::TryParseHexByte(text[i], text[i + 1U], byte)) return false;
            word = (word << 8U) | byte;
        }
        text.remove_prefix(16U);
        return true;
    };
    uint64_t nonemptySources = 0ULL;
    if (!readWord(out.knownSources) || out.knownSources == 0ULL ||
        !readWord(nonemptySources) || (nonemptySources & ~out.knownSources) != 0ULL) {
        return false;
    }
    for (uint8_t source = 0U; source < 64U; ++source) {
        if ((nonemptySources & (1ULL << source)) == 0ULL) continue;
        if (!readWord(out.landingsBySource[source]) || out.landingsBySource[source] == 0ULL) {
            return false;
        }
    }
    return text.empty();
}

bool Fail(const uint32_t lineNumber,
          const char* message,
          ReplayResult& outResult) {
    outResult.ok = false;
    outResult.failureLine = lineNumber;
    std::snprintf(outResult.message, sizeof(outResult.message), "%s", message);
    return false;
}

} // namespace

bool IsInteractionAdapterEvent(const JsonlEventType type) {
    return type == JsonlEventType::PositionAcknowledged ||
           type == JsonlEventType::ActionResult ||
           type == JsonlEventType::AdapterResult;
}

bool ReplayInteractionAdapterEvent(const JsonlEventRecord& record,
                                   runtime::GameStateMachine& stateMachine,
                                   modules::GamePersistenceCoordinator* const gamePersistence,
                                   const uint32_t lineNumber,
                                   ReplayResult& outResult) {
    runtime::AppEvent event;
    const uint32_t sequence = record.hasObservationSequence ? record.observationSequence : 0U;
    if (record.type == JsonlEventType::PositionAcknowledged) {
        if (!record.hasOccupiedMask || !record.hasKingMask ||
            !record.hasRookMask) {
            return Fail(lineNumber, "position_ack_parse_error", outResult);
        }
        event = runtime::AppEvent::MakePositionAcknowledged(
            record.tMs,
            record.occupiedMask,
            record.kingMask,
            record.rookMask,
            record.hasMoverMask ? record.moverMask : 0ULL,
            record.hasOpponentMask ? record.opponentMask : 0ULL,
            record.hasPawnMask ? record.pawnMask : 0ULL,
            record.hasEnPassantTargetSquare ? record.enPassantTargetSquare : 255U,
            record.hasEnPassantCaptureSquare ? record.enPassantCaptureSquare : 255U,
            record.hasSideToMoveValid ? record.sideToMoveValid : false,
            record.hasSideToMoveBlack ? record.sideToMoveBlack : false,
            record.hasLegalCaptureTargetsValid
                ? record.legalCaptureTargetsValid
                : false,
            record.hasLegalCaptureTargetsMask
                ? record.legalCaptureTargetsMask
                : 0ULL,
            record.hasLegalCastlingTargetsValid
                ? record.legalCastlingTargetsValid
                : false,
            record.hasLegalCastlingKingTargetsMask
                ? record.legalCastlingKingTargetsMask
                : 0ULL);
        runtime::CaptureLegality captureLegality;
        if (!ReadCaptureRows(record.captureRows, captureLegality)) {
            return Fail(lineNumber, "position_ack_capture_rows_invalid", outResult);
        }
        event.seq = sequence;
        runtime::DispatchAppEventToStateMachine(stateMachine, event, &captureLegality);
        if (gamePersistence != nullptr &&
            !gamePersistence->ResumeExternalizedDuckRedoAfterPositionAcknowledged()) {
            return Fail(lineNumber, "position_ack_duck_redo_resume_failed", outResult);
        }
        return true;
    }
    if (record.type == JsonlEventType::ActionResult) {
        if (!record.hasActionToken || !record.hasSucceeded) {
            return Fail(lineNumber, "action_result_parse_error", outResult);
        }
        event = runtime::AppEvent::MakeActionResult(
            record.tMs,
            record.actionToken,
            record.succeeded);
    } else {
        if (record.adapter.empty() || !record.hasSucceeded) {
            return Fail(lineNumber, "adapter_result_parse_error", outResult);
        }
        runtime::AppEventType type = runtime::AppEventType::PersistenceResult;
        if (record.adapter == "search") {
            type = runtime::AppEventType::SearchResult;
        } else if (record.adapter == "restore" ||
                   record.adapter == "cancel_undo") {
            type = runtime::AppEventType::RestoreResult;
        }
        event = runtime::AppEvent::MakeAdapterResult(
            type,
            record.tMs,
            0,
            record.succeeded);
    }
    event.seq = sequence;
    runtime::DispatchAppEventToStateMachine(stateMachine, event);
    return true;
}

} // namespace trace
