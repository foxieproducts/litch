#include "modules/control_mutation_service.hpp"

#include <cstdio>

namespace modules {

const char* ControlMutationStatusToString(
    const ControlMutationStatus status) {
    switch (status) {
        case ControlMutationStatus::Accepted:
            return "accepted";
        case ControlMutationStatus::Applied:
            return "applied";
        case ControlMutationStatus::Rejected:
            return "rejected";
        case ControlMutationStatus::Noop:
            return "noop";
    }
    return "rejected";
}

const char* ControlMutationReasonToString(
    const ControlMutationReason reason) {
    switch (reason) {
        case ControlMutationReason::None:
            return "none";
        case ControlMutationReason::BadRequest:
            return "bad_request";
        case ControlMutationReason::UnknownAction:
            return "unknown_action";
        case ControlMutationReason::QueueFull:
            return "queue_full";
        case ControlMutationReason::UnknownKey:
            return "unknown_key";
        case ControlMutationReason::TypeMismatch:
            return "type_mismatch";
        case ControlMutationReason::OutOfRange:
            return "out_of_range";
        case ControlMutationReason::InvalidEnumValue:
            return "invalid_enum_value";
        case ControlMutationReason::NotAtStart:
            return "not_at_start";
        case ControlMutationReason::Busy:
            return "busy";
        case ControlMutationReason::NoHistory:
            return "no_history";
        case ControlMutationReason::GameOver:
            return "game_over";
        case ControlMutationReason::Unsupported:
            return "unsupported";
        case ControlMutationReason::EngineRejected:
            return "engine_rejected";
        case ControlMutationReason::StorageUnavailable:
            return "storage_unavailable";
        case ControlMutationReason::ArchiveRequired:
            return "archive_required";
    }
    return "bad_request";
}

const char* ControlActionStateToString(const ControlActionState state) {
    switch (state) {
        case ControlActionState::Idle:
            return "idle";
        case ControlActionState::UndoPending:
            return "undo_pending";
    }
    return "idle";
}

ControlMutationReason ControlMutationReasonFromCoordinatorResult(
    const ControlCoordinatorResult result) {
    switch (result) {
        case ControlCoordinatorResult::Applied:
        case ControlCoordinatorResult::Pending:
            return ControlMutationReason::None;
        case ControlCoordinatorResult::QueueFull:
            return ControlMutationReason::QueueFull;
        case ControlCoordinatorResult::NotAtStart:
            return ControlMutationReason::NotAtStart;
        case ControlCoordinatorResult::Busy:
            return ControlMutationReason::Busy;
        case ControlCoordinatorResult::NoHistory:
            return ControlMutationReason::NoHistory;
        case ControlCoordinatorResult::GameOver:
            return ControlMutationReason::GameOver;
        case ControlCoordinatorResult::Unsupported:
            return ControlMutationReason::Unsupported;
        case ControlCoordinatorResult::EngineRejected:
            return ControlMutationReason::EngineRejected;
        case ControlCoordinatorResult::StorageUnavailable:
            return ControlMutationReason::StorageUnavailable;
        case ControlCoordinatorResult::ArchiveRequired:
            return ControlMutationReason::ArchiveRequired;
    }
    return ControlMutationReason::BadRequest;
}

char ControlMutationStatusToCompactCode(
    const ControlMutationStatus status) {
    switch (status) {
        case ControlMutationStatus::Accepted:
            return 'a';
        case ControlMutationStatus::Applied:
            return 'p';
        case ControlMutationStatus::Rejected:
            return 'r';
        case ControlMutationStatus::Noop:
            return 'n';
    }
    return 'r';
}

char ControlMutationReasonToCompactCode(
    const ControlMutationReason reason) {
    switch (reason) {
        case ControlMutationReason::None:
            return '0';
        case ControlMutationReason::BadRequest:
            return 'b';
        case ControlMutationReason::UnknownAction:
            return 'a';
        case ControlMutationReason::QueueFull:
            return 'q';
        case ControlMutationReason::UnknownKey:
            return 'k';
        case ControlMutationReason::TypeMismatch:
            return 't';
        case ControlMutationReason::OutOfRange:
            return 'o';
        case ControlMutationReason::InvalidEnumValue:
            return 'v';
        case ControlMutationReason::NotAtStart:
            return 's';
        case ControlMutationReason::Busy:
            return 'y';
        case ControlMutationReason::NoHistory:
            return 'h';
        case ControlMutationReason::GameOver:
            return 'g';
        case ControlMutationReason::Unsupported:
            return 'u';
        case ControlMutationReason::EngineRejected:
            return 'e';
        case ControlMutationReason::StorageUnavailable:
            return 'p';
        case ControlMutationReason::ArchiveRequired:
            return 'r';
    }
    return 'b';
}

size_t FormatControlMutationCompactResponse(
    const ControlMutationOutcome& outcome,
    char* const outBuffer,
    const size_t outCapacity) {
    if (outBuffer == nullptr || outCapacity == 0U) {
        return 0U;
    }
    const int written = std::snprintf(
        outBuffer, outCapacity, "{\"x\":[%u,%u,%u]}",
        static_cast<unsigned>(outcome.requestId),
        static_cast<unsigned>(outcome.status),
        static_cast<unsigned>(outcome.reason));
    if (written <= 0 || static_cast<size_t>(written) >= outCapacity) {
        outBuffer[0] = '\0';
        return 0U;
    }
    return static_cast<size_t>(written);
}

} // namespace modules
