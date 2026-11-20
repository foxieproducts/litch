#include "runtime/virtual_litch_session.h"

#ifdef LITCH_NATIVE

#include <new>
#include <cstring>

#include "virtual_litch_session_internal.hpp"
#include "modules/control_mutation_service.hpp"

struct virtual_litch {
    explicit virtual_litch(const virtual_litch_config& config)
        : session(config) {}

    runtime::virtual_litch_internal::Session session;
};

namespace {

bool IsOptionalSquare(const uint8_t square) {
    return square < VIRTUAL_LITCH_SQUARE_COUNT || square == UINT8_MAX;
}

bool IsBooleanByte(const uint8_t value) {
    return value <= 1U;
}

bool IsValidSleepLifecycle(const virtual_litch_sleep_lifecycle& lifecycle) {
    return lifecycle.abi_version == VIRTUAL_LITCH_SLEEP_LIFECYCLE_ABI_VERSION &&
           lifecycle.struct_size >= sizeof(lifecycle);
}

bool HasCompatibleRuntimeHealthAbi(
    const virtual_litch_runtime_health_fact& fact) {
    return fact.abi_version == VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION &&
           fact.struct_size >= sizeof(fact);
}

bool HasCompatibleRuntimeHealthAbi(
    const virtual_litch_runtime_health& health) {
    return health.abi_version == VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION &&
           health.struct_size >= sizeof(health);
}

bool HasValidRuntimeHealthEnums(
    const virtual_litch_runtime_health_fact& fact) {
    return fact.kind <= VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_LED_BUDGET &&
           fact.board_scan <=
               VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN &&
           fact.source <= VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_UNSTABLE;
}

bool HasValidRuntimeHealthValues(
    const virtual_litch_runtime_health_fact& fact) {
    return fact.ambient_sensor_count <= 2U &&
           fact.power_envelope_percent <= 100U &&
           IsBooleanByte(fact.brownout_detected) &&
           fact.reserved[0] == 0U && fact.reserved[1] == 0U;
}

bool IsValidPosition(const virtual_litch_position& position) {
    return IsOptionalSquare(position.en_passant_target_square) &&
           IsOptionalSquare(position.en_passant_capture_square) &&
           IsBooleanByte(position.side_to_move_valid) &&
           IsBooleanByte(position.side_to_move_black) &&
           IsBooleanByte(position.legal_capture_targets_valid) &&
           IsBooleanByte(position.legal_castling_targets_valid) &&
           position.reserved[0] == 0U && position.reserved[1] == 0U;
}

void WriteControlOutcome(
    const modules::ControlMutationOutcome& source,
    virtual_litch_control_outcome& destination) {
    destination = virtual_litch_control_outcome{};
    destination.struct_size = sizeof(destination);
    destination.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    destination.settings_revision = source.settingsRevision;
    destination.request_id = source.requestId;
    destination.action = static_cast<uint8_t>(source.action);
    destination.status = static_cast<uint8_t>(source.status);
    destination.reason = static_cast<uint8_t>(source.reason);
    destination.persistence_tier =
        static_cast<uint8_t>(source.persistenceTier);
}

bool HasTerminator(const char* value, const size_t capacity) {
    return std::memchr(value, '\0', capacity) != nullptr;
}

template <typename T>
bool HasCompatibleRequestAbi(const T& request) {
    return request.abi_version == VIRTUAL_LITCH_CONTROL_ABI_VERSION &&
           request.struct_size >= sizeof(T);
}

} // namespace

extern "C" {

uint32_t virtual_litch_get_abi_version(void) {
    return VIRTUAL_LITCH_ABI_VERSION;
}

void virtual_litch_default_config(virtual_litch_config* out_config) {
    if (out_config == nullptr) {
        return;
    }
    *out_config = runtime::virtual_litch_internal::DefaultConfig();
}

void virtual_litch_default_position(
    virtual_litch_position* out_position) {
    if (out_position == nullptr) {
        return;
    }
    *out_position = virtual_litch_position{};
    out_position->en_passant_target_square = UINT8_MAX;
    out_position->en_passant_capture_square = UINT8_MAX;
}

virtual_litch_result virtual_litch_create(
    const virtual_litch_config* config,
    virtual_litch** out_board) {
    if (out_board == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    *out_board = nullptr;
    const virtual_litch_config resolved =
        config == nullptr ? runtime::virtual_litch_internal::DefaultConfig()
                          : *config;
    if (resolved.struct_size != sizeof(resolved) ||
        resolved.abi_version != VIRTUAL_LITCH_ABI_VERSION) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    if (!runtime::virtual_litch_internal::IsValidConfig(resolved)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }

    try {
        *out_board = new (std::nothrow) virtual_litch(resolved);
    } catch (...) {
        *out_board = nullptr;
    }
    return *out_board == nullptr ? VIRTUAL_LITCH_ALLOCATION_FAILED
                                 : VIRTUAL_LITCH_OK;
}

void virtual_litch_destroy(virtual_litch* board) {
    delete board;
}

virtual_litch_result virtual_litch_reset(virtual_litch* board) {
    return board == nullptr ? VIRTUAL_LITCH_NULL_ARGUMENT
                            : board->session.Reset();
}

virtual_litch_result virtual_litch_dispatch_position(
    virtual_litch* board,
    const virtual_litch_position* position) {
    if (board == nullptr || position == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!IsValidPosition(*position)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    return board->session.DispatchPosition(*position);
}

virtual_litch_result virtual_litch_dispatch_board_snapshot(
    virtual_litch* board,
    const uint64_t occupied_mask) {
    if (board == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    const uint64_t changed =
        board->session.PhysicalOccupancyMask() ^ occupied_mask;
    return board->session.DispatchBoardObservation(changed, occupied_mask);
}

virtual_litch_result virtual_litch_dispatch_board_observation(
    virtual_litch* board,
    const uint64_t changed_mask,
    const uint64_t occupied_mask) {
    return board == nullptr
               ? VIRTUAL_LITCH_NULL_ARGUMENT
               : board->session.DispatchBoardObservation(
                     changed_mask, occupied_mask);
}

virtual_litch_result virtual_litch_dispatch_sensor_delta(
    virtual_litch* board,
    const uint8_t square,
    const uint8_t now_occupied) {
    if (board == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!IsBooleanByte(now_occupied)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    return board->session.DispatchSensorDelta(square, now_occupied != 0U);
}

virtual_litch_result virtual_litch_dispatch_button(
    virtual_litch* board,
    const uint8_t owner,
    const uint8_t button,
    const uint8_t event) {
    return board == nullptr
               ? VIRTUAL_LITCH_NULL_ARGUMENT
               : board->session.DispatchButton(owner, button, event);
}

virtual_litch_result virtual_litch_step_until(
    virtual_litch* board,
    const uint32_t target_time_ms) {
    return board == nullptr
               ? VIRTUAL_LITCH_NULL_ARGUMENT
               : board->session.StepUntil(target_time_ms);
}

virtual_litch_result virtual_litch_advance(
    virtual_litch* board,
    const uint32_t delta_ms) {
    return board == nullptr ? VIRTUAL_LITCH_NULL_ARGUMENT
                            : board->session.Advance(delta_ms);
}

virtual_litch_result virtual_litch_get_state(
    const virtual_litch* board,
    virtual_litch_state* out_state) {
    if (board == nullptr || out_state == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    *out_state = board->session.State();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result virtual_litch_get_semantic_frame(
    const virtual_litch* board,
    virtual_litch_semantic_frame* out_frame) {
    if (board == nullptr || out_frame == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    *out_frame = board->session.SemanticFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result virtual_litch_get_physical_frame(
    const virtual_litch* board,
    virtual_litch_physical_frame* out_frame) {
    if (board == nullptr || out_frame == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    *out_frame = board->session.PhysicalFrame();
    return VIRTUAL_LITCH_OK;
}

size_t virtual_litch_get_history_count(const virtual_litch* board) {
    return board == nullptr ? 0U : board->session.HistoryCount();
}

virtual_litch_result virtual_litch_get_history_frame(
    const virtual_litch* board,
    const size_t chronological_index,
    virtual_litch_history_frame* out_frame) {
    if (board == nullptr || out_frame == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    return board->session.TryGetHistory(chronological_index, *out_frame)
               ? VIRTUAL_LITCH_OK
               : VIRTUAL_LITCH_OUT_OF_RANGE;
}

void virtual_litch_default_setting_request(
    virtual_litch_setting_request* out_request) {
    if (out_request == nullptr) {
        return;
    }
    *out_request = virtual_litch_setting_request{};
    out_request->struct_size = sizeof(*out_request);
    out_request->abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
}

void virtual_litch_default_action_request(
    virtual_litch_action_request* out_request) {
    if (out_request == nullptr) {
        return;
    }
    *out_request = virtual_litch_action_request{};
    out_request->struct_size = sizeof(*out_request);
    out_request->abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    out_request->undo_move_count = 1U;
}

virtual_litch_result virtual_litch_mutate_setting(
    virtual_litch* board,
    const virtual_litch_setting_request* request,
    virtual_litch_control_outcome* out_outcome) {
    if (board == nullptr || request == nullptr || out_outcome == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!HasCompatibleRequestAbi(*request)) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    if (request->reserved[0] != 0U || request->reserved[1] != 0U ||
        !HasTerminator(request->key, sizeof(request->key)) ||
        !HasTerminator(request->value_json, sizeof(request->value_json)) ||
        request->key[0] == '\0' || request->value_json[0] == '\0') {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    modules::ControlMutationOutcome outcome;
    const virtual_litch_result result =
        board->session.MutateSetting(*request, outcome);
    if (result == VIRTUAL_LITCH_OK) {
        WriteControlOutcome(outcome, *out_outcome);
    }
    return result;
}

virtual_litch_result virtual_litch_dispatch_control_action(
    virtual_litch* board,
    const virtual_litch_action_request* request,
    virtual_litch_control_outcome* out_outcome) {
    if (board == nullptr || request == nullptr || out_outcome == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!HasCompatibleRequestAbi(*request)) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    if (request->reserved[0] != 0U || request->reserved[1] != 0U ||
        request->action < VIRTUAL_LITCH_CONTROL_HINT ||
        request->action > VIRTUAL_LITCH_CONTROL_COACHING ||
        request->owner > VIRTUAL_LITCH_OWNER_P2 ||
        request->coaching_mutation > 2U) {
        return VIRTUAL_LITCH_INVALID_ENUM;
    }
    modules::ControlMutationOutcome outcome;
    const virtual_litch_result result =
        board->session.DispatchControlAction(*request, outcome);
    if (result == VIRTUAL_LITCH_OK) {
        WriteControlOutcome(outcome, *out_outcome);
    }
    return result;
}

virtual_litch_result virtual_litch_take_control_completion(
    virtual_litch* board,
    virtual_litch_control_outcome* out_outcome) {
    if (board == nullptr || out_outcome == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    modules::ControlMutationOutcome outcome;
    if (!board->session.TryTakeControlCompletion(outcome)) {
        return VIRTUAL_LITCH_OUT_OF_RANGE;
    }
    WriteControlOutcome(outcome, *out_outcome);
    return VIRTUAL_LITCH_OK;
}

void virtual_litch_default_runtime_health_fact(
    virtual_litch_runtime_health_fact* out_fact) {
    if (out_fact == nullptr) {
        return;
    }
    *out_fact = virtual_litch_runtime_health_fact{};
    out_fact->struct_size = sizeof(*out_fact);
    out_fact->abi_version = VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
    out_fact->ambient_sensor_count = 2U;
    out_fact->power_envelope_percent = 100U;
}

void virtual_litch_default_runtime_health(
    virtual_litch_runtime_health* out_health) {
    if (out_health == nullptr) {
        return;
    }
    *out_health = virtual_litch_runtime_health{};
    out_health->struct_size = sizeof(*out_health);
    out_health->abi_version = VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
    out_health->board_input_allowed = 1U;
    out_health->move_acceptance_allowed = 1U;
    out_health->clocks_allowed = 1U;
}

virtual_litch_result virtual_litch_dispatch_runtime_health_fact(
    virtual_litch* board,
    const virtual_litch_runtime_health_fact* fact) {
    if (board == nullptr || fact == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!HasCompatibleRuntimeHealthAbi(*fact)) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    if (!HasValidRuntimeHealthEnums(*fact)) {
        return VIRTUAL_LITCH_INVALID_ENUM;
    }
    if (!HasValidRuntimeHealthValues(*fact)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    return board->session.DispatchRuntimeHealthFact(*fact);
}

virtual_litch_result virtual_litch_get_runtime_health(
    const virtual_litch* board,
    virtual_litch_runtime_health* out_health) {
    if (board == nullptr || out_health == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!HasCompatibleRuntimeHealthAbi(*out_health)) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    *out_health = board->session.RuntimeHealth();
    return VIRTUAL_LITCH_OK;
}

void virtual_litch_default_sleep_lifecycle(
    virtual_litch_sleep_lifecycle* out_lifecycle) {
    if (out_lifecycle == nullptr) {
        return;
    }
    *out_lifecycle = virtual_litch_sleep_lifecycle{};
    out_lifecycle->struct_size = sizeof(*out_lifecycle);
    out_lifecycle->abi_version = VIRTUAL_LITCH_SLEEP_LIFECYCLE_ABI_VERSION;
    out_lifecycle->state = VIRTUAL_LITCH_SLEEP_AWAKE;
    out_lifecycle->origin = VIRTUAL_LITCH_SLEEP_ORIGIN_NONE;
}

virtual_litch_result virtual_litch_get_sleep_lifecycle(
    const virtual_litch* board,
    virtual_litch_sleep_lifecycle* out_lifecycle) {
    if (board == nullptr || out_lifecycle == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!IsValidSleepLifecycle(*out_lifecycle)) {
        return VIRTUAL_LITCH_ABI_MISMATCH;
    }
    *out_lifecycle = board->session.SleepLifecycle();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result virtual_litch_request_sleep(
    virtual_litch* board,
    const uint8_t origin) {
    return board == nullptr ? VIRTUAL_LITCH_NULL_ARGUMENT
                            : board->session.RequestSleep(origin);
}

virtual_litch_result virtual_litch_dispatch_validated_motion_or_tap(
    virtual_litch* board) {
    return board == nullptr ? VIRTUAL_LITCH_NULL_ARGUMENT
                            : board->session.DispatchValidatedMotionOrTap();
}

virtual_litch_result virtual_litch_resolve_sleep_preflights(
    virtual_litch* board,
    const uint8_t persistence_succeeded,
    const uint8_t wake_source_succeeded) {
    if (board == nullptr) {
        return VIRTUAL_LITCH_NULL_ARGUMENT;
    }
    if (!IsBooleanByte(persistence_succeeded) ||
        !IsBooleanByte(wake_source_succeeded)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    return board->session.ResolveSleepPreflights(persistence_succeeded != 0U,
                                                 wake_source_succeeded != 0U);
}

virtual_litch_result virtual_litch_simulate_sleep_wake(
    virtual_litch* board) {
    return board == nullptr ? VIRTUAL_LITCH_NULL_ARGUMENT
                            : board->session.SimulateSleepWake();
}

virtual_litch_result virtual_litch_report_wake_buttons_released(
    virtual_litch* board) {
    return board == nullptr
               ? VIRTUAL_LITCH_NULL_ARGUMENT
               : board->session.ReportWakeButtonsReleased();
}

} // extern "C"

#endif
