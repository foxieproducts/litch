#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIRTUAL_LITCH_ABI_VERSION 1U
#define VIRTUAL_LITCH_CONTROL_ABI_VERSION 1U
#define VIRTUAL_LITCH_SLEEP_LIFECYCLE_ABI_VERSION 1U
#define VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION 1U
#define VIRTUAL_LITCH_SQUARE_COUNT 64U
#define VIRTUAL_LITCH_MAX_LEDS_PER_SQUARE 4U
#define VIRTUAL_LITCH_STATUS_LED_COUNT 32U
#define VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT 320U

typedef struct virtual_litch virtual_litch;

typedef enum virtual_litch_result {
    VIRTUAL_LITCH_OK = 0,
    VIRTUAL_LITCH_NULL_ARGUMENT = 1,
    VIRTUAL_LITCH_ABI_MISMATCH = 2,
    VIRTUAL_LITCH_INVALID_CONFIG = 3,
    VIRTUAL_LITCH_INVALID_ENUM = 4,
    VIRTUAL_LITCH_OUT_OF_RANGE = 5,
    VIRTUAL_LITCH_TIME_REVERSED = 6,
    VIRTUAL_LITCH_TIME_OVERFLOW = 7,
    VIRTUAL_LITCH_ALLOCATION_FAILED = 8,
    // The inputs were structurally valid, but the requested lifecycle
    // transition is disallowed in the current state or by current policy.
    VIRTUAL_LITCH_POLICY_REJECTED = 9,
} virtual_litch_result;

typedef enum virtual_litch_hardware {
    VIRTUAL_LITCH_HARDWARE_C3 = 1,
    VIRTUAL_LITCH_HARDWARE_S3 = 2,
    // V3 physical topology; semantic v1 remains Left/Piece/Right.
    VIRTUAL_LITCH_HARDWARE_V3 = 3,
    // V3.3 RGB topology, including twelve inserted control emitters.
    VIRTUAL_LITCH_HARDWARE_V3_3 = 4,
} virtual_litch_hardware;

typedef enum virtual_litch_owner {
    VIRTUAL_LITCH_OWNER_NONE = 0,
    VIRTUAL_LITCH_OWNER_P1 = 1,
    VIRTUAL_LITCH_OWNER_P2 = 2,
} virtual_litch_owner;

typedef enum virtual_litch_button {
    VIRTUAL_LITCH_BUTTON_UP = 0,
    VIRTUAL_LITCH_BUTTON_DOWN = 1,
    VIRTUAL_LITCH_BUTTON_LEFT = 2,
    VIRTUAL_LITCH_BUTTON_RIGHT = 3,
    VIRTUAL_LITCH_BUTTON_PRESS = 4, // Reserved ABI identity; input is ignored.
    VIRTUAL_LITCH_BUTTON_P1_X = 5,
    VIRTUAL_LITCH_BUTTON_P2_X = 6,
    VIRTUAL_LITCH_BUTTON_P1_O = 7,
    VIRTUAL_LITCH_BUTTON_P2_O = 8,
} virtual_litch_button;

typedef enum virtual_litch_button_event {
    VIRTUAL_LITCH_BUTTON_PRESS_EVENT = 0,
    VIRTUAL_LITCH_BUTTON_REPEAT_EVENT = 1,
    VIRTUAL_LITCH_BUTTON_RELEASE_EVENT = 2,
    VIRTUAL_LITCH_BUTTON_LONG_HOLD_EVENT = 3,
} virtual_litch_button_event;

typedef struct virtual_litch_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;
} virtual_litch_color;

typedef struct virtual_litch_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t initial_time_ms;
    uint16_t history_capacity;
    uint16_t brightness_scale_256;
    uint16_t max_led_current_ma;
    uint8_t hardware;
    uint8_t board_rotate;
    uint8_t square_rotate_for_output;
    uint8_t first_player_is_p2;
    uint8_t flip_sides;
    uint8_t strong_power_source; // Retained ABI field; normal budget is independent of CC.
    uint8_t recovery_power_limit;
    uint8_t reserved;
} virtual_litch_config;

typedef struct virtual_litch_position {
    uint64_t occupied_mask;
    uint64_t king_mask;
    uint64_t rook_mask;
    uint64_t pawn_mask;
    uint64_t mover_occupied_mask;
    uint64_t opponent_occupied_mask;
    uint64_t legal_capture_targets_mask;
    uint64_t legal_castling_king_targets_mask;
    uint8_t en_passant_target_square;
    uint8_t en_passant_capture_square;
    uint8_t side_to_move_valid;
    uint8_t side_to_move_black;
    uint8_t legal_capture_targets_valid;
    uint8_t legal_castling_targets_valid;
    uint8_t reserved[2];
} virtual_litch_position;

typedef struct virtual_litch_state {
    uint32_t now_ms;
    uint32_t frame_sequence;
    uint32_t board_observation_sequence;
    uint32_t pending_action_token;
    uint32_t feedback_epoch;
    uint64_t physical_occupied_mask;
    uint64_t expected_occupied_mask;
    uint64_t valid_destination_mask;
    uint64_t castling_king_destination_mask;
    uint64_t castling_rook_source_mask;
    uint64_t correction_mask;
    uint64_t human_illegal_mask;
    uint64_t cpu_illegal_mask;
    uint8_t interaction_phase;
    uint8_t required_action;
    uint8_t selected_square;
    uint8_t side_to_move_black;
} virtual_litch_state;

typedef struct virtual_litch_semantic_frame {
    uint32_t now_ms;
    uint32_t frame_sequence;
    uint64_t board_hash;
    uint64_t status_hash;
    uint64_t combined_hash;
    uint8_t leds_per_square;
    uint8_t reserved[7];
    virtual_litch_color board[VIRTUAL_LITCH_SQUARE_COUNT]
                                [VIRTUAL_LITCH_MAX_LEDS_PER_SQUARE];
    virtual_litch_color status[VIRTUAL_LITCH_STATUS_LED_COUNT];
} virtual_litch_semantic_frame;

typedef struct virtual_litch_physical_frame {
    uint32_t now_ms;
    uint32_t frame_sequence;
    uint64_t frame_hash;
    uint16_t led_count;
    uint16_t estimated_led_current_ma;
    uint16_t applied_led_current_ma;
    uint16_t applied_brightness_scale_65535;
    uint8_t channels_per_led;
    uint8_t power_limited;
    uint8_t reserved[2];
    virtual_litch_color pixels[VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT];
} virtual_litch_physical_frame;

typedef struct virtual_litch_history_frame {
    virtual_litch_state state;
    virtual_litch_semantic_frame semantic;
    virtual_litch_physical_frame physical;
} virtual_litch_history_frame;

typedef enum virtual_litch_control_action {
    VIRTUAL_LITCH_CONTROL_HINT = 1,
    VIRTUAL_LITCH_CONTROL_UNDO = 2,
    VIRTUAL_LITCH_CONTROL_NEW_GAME = 3,
    VIRTUAL_LITCH_CONTROL_COACHING = 4,
} virtual_litch_control_action;

typedef struct virtual_litch_setting_request {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t request_id;
    uint8_t reserved[2];
    char key[32];
    char value_json[64];
} virtual_litch_setting_request;

typedef struct virtual_litch_action_request {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t request_id;
    uint8_t action;
    uint8_t owner;
    uint8_t coaching_mutation;
    uint8_t undo_move_count;
    uint8_t reserved[2];
} virtual_litch_action_request;

typedef struct virtual_litch_control_outcome {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t settings_revision;
    uint16_t request_id;
    uint8_t action;
    uint8_t status;
    uint8_t reason;
    uint8_t persistence_tier;
    uint8_t reserved[2];
} virtual_litch_control_outcome;

typedef enum virtual_litch_runtime_health_state {
    VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY = 0,
    VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE = 1,
    VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED = 2,
    VIRTUAL_LITCH_RUNTIME_HEALTH_PROTECTION_LATCHED = 3,
} virtual_litch_runtime_health_state;

typedef enum virtual_litch_runtime_health_cause {
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_NONE = 0,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR = 1U << 0U,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_NO_AMBIENT_SENSORS = 1U << 1U,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BOARD_SCAN_UNCERTAIN = 1U << 2U,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_SOURCE_UNSTABLE = 1U << 3U,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BROWNOUT_LATCHED = 1U << 4U,
    VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_SEVERE_PROTECTION_LATCHED = 1U << 5U,
} virtual_litch_runtime_health_cause;

typedef enum virtual_litch_runtime_health_fact_kind {
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN = 0,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_AMBIENT_SENSOR_COUNT = 1,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_POWER_SOURCE = 2,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_POWER_ENVELOPE = 3,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BROWNOUT = 4,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_RESET_LATCHED_PROTECTION = 5,
    VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_LED_BUDGET = 6,
} virtual_litch_runtime_health_fact_kind;

typedef enum virtual_litch_runtime_health_board_scan {
    VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE = 0,
    VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN = 1,
} virtual_litch_runtime_health_board_scan;

typedef enum virtual_litch_runtime_health_source {
    VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_STABLE = 0,
    VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_UNSTABLE = 1,
} virtual_litch_runtime_health_source;

// This is a separate append-only ABI. Session time is authoritative; callers
// step the session before dispatching a fact instead of supplying a timestamp.
typedef struct virtual_litch_runtime_health_fact {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t kind;
    uint8_t board_scan;
    uint8_t ambient_sensor_count;
    uint8_t source;
    uint8_t power_envelope_percent;
    uint8_t brownout_detected;
    uint8_t reserved[2];
} virtual_litch_runtime_health_fact;

// Current health is correlated to a captured frame without resizing the
// deployed state, frame, or history layouts. No separate health history is
// retained.
typedef struct virtual_litch_runtime_health {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t now_ms;
    uint32_t frame_sequence;
    uint16_t revision;
    uint16_t causes;
    uint8_t state;
    uint8_t board_input_allowed;
    uint8_t move_acceptance_allowed;
    uint8_t clocks_allowed;
    uint8_t reserved[4];
} virtual_litch_runtime_health;

typedef enum virtual_litch_sleep_state {
    VIRTUAL_LITCH_SLEEP_AWAKE = 0,
    VIRTUAL_LITCH_SLEEP_COUNTDOWN = 1,
    VIRTUAL_LITCH_SLEEP_PREPARING = 2,
    VIRTUAL_LITCH_SLEEP_SLEEPING = 3,
    VIRTUAL_LITCH_SLEEP_WAKE_GUARD = 4,
    VIRTUAL_LITCH_SLEEP_FADING = 5,
} virtual_litch_sleep_state;

typedef enum virtual_litch_sleep_request_origin {
    VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE = 0,
    VIRTUAL_LITCH_SLEEP_ORIGIN_MENU = 1,
    VIRTUAL_LITCH_SLEEP_ORIGIN_CONSOLE = 2,
    VIRTUAL_LITCH_SLEEP_ORIGIN_APP = 3,
    VIRTUAL_LITCH_SLEEP_ORIGIN_NONE = 255,
} virtual_litch_sleep_request_origin;

typedef enum virtual_litch_sleep_cancel_cause {
    VIRTUAL_LITCH_SLEEP_CANCEL_NONE = 0,
    VIRTUAL_LITCH_SLEEP_CANCEL_BUTTON_PRESS = 1,
    VIRTUAL_LITCH_SLEEP_CANCEL_BOARD_OBSERVATION_CHANGED = 2,
    VIRTUAL_LITCH_SLEEP_CANCEL_VALIDATED_MOTION_OR_TAP = 3,
    VIRTUAL_LITCH_SLEEP_CANCEL_PREFLIGHT_FAILURE = 4,
    VIRTUAL_LITCH_SLEEP_CANCEL_HIGHER_PRIORITY_STATE = 5,
} virtual_litch_sleep_cancel_cause;

// This is a separate append-only ABI. The legacy state/frame/history structs
// intentionally remain byte-for-byte unchanged.
typedef struct virtual_litch_sleep_lifecycle {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t now_ms;
    uint32_t frame_sequence;
    uint32_t remaining_countdown_ms;
    uint16_t transition_revision;
    uint8_t state;
    uint8_t origin;
    uint8_t cancel_cause;
    uint8_t persistence_preflight_complete;
    uint8_t wake_source_preflight_complete;
    uint8_t sleep_authorization_pending;
    uint8_t reserved[4];
} virtual_litch_sleep_lifecycle;

uint32_t virtual_litch_get_abi_version(void);

void virtual_litch_default_config(virtual_litch_config* out_config);

void virtual_litch_default_position(
    virtual_litch_position* out_position);

virtual_litch_result virtual_litch_create(
    const virtual_litch_config* config,
    virtual_litch** out_board);

void virtual_litch_destroy(virtual_litch* board);

virtual_litch_result virtual_litch_reset(virtual_litch* board);

virtual_litch_result virtual_litch_dispatch_position(
    virtual_litch* board,
    const virtual_litch_position* position);

virtual_litch_result virtual_litch_dispatch_board_snapshot(
    virtual_litch* board,
    uint64_t occupied_mask);

virtual_litch_result virtual_litch_dispatch_board_observation(
    virtual_litch* board,
    uint64_t changed_mask,
    uint64_t occupied_mask);

virtual_litch_result virtual_litch_dispatch_sensor_delta(
    virtual_litch* board,
    uint8_t square,
    uint8_t now_occupied);

virtual_litch_result virtual_litch_dispatch_button(
    virtual_litch* board,
    uint8_t owner,
    uint8_t button,
    uint8_t event);

virtual_litch_result virtual_litch_step_until(
    virtual_litch* board,
    uint32_t target_time_ms);

virtual_litch_result virtual_litch_advance(
    virtual_litch* board,
    uint32_t delta_ms);

virtual_litch_result virtual_litch_get_state(
    const virtual_litch* board,
    virtual_litch_state* out_state);

virtual_litch_result virtual_litch_get_semantic_frame(
    const virtual_litch* board,
    virtual_litch_semantic_frame* out_frame);

virtual_litch_result virtual_litch_get_physical_frame(
    const virtual_litch* board,
    virtual_litch_physical_frame* out_frame);

size_t virtual_litch_get_history_count(const virtual_litch* board);

virtual_litch_result virtual_litch_get_history_frame(
    const virtual_litch* board,
    size_t chronological_index,
    virtual_litch_history_frame* out_frame);

void virtual_litch_default_setting_request(
    virtual_litch_setting_request* out_request);
void virtual_litch_default_action_request(
    virtual_litch_action_request* out_request);
virtual_litch_result virtual_litch_mutate_setting(
    virtual_litch* board,
    const virtual_litch_setting_request* request,
    virtual_litch_control_outcome* out_outcome);
virtual_litch_result virtual_litch_dispatch_control_action(
    virtual_litch* board,
    const virtual_litch_action_request* request,
    virtual_litch_control_outcome* out_outcome);
virtual_litch_result virtual_litch_take_control_completion(
    virtual_litch* board,
    virtual_litch_control_outcome* out_outcome);

void virtual_litch_default_runtime_health_fact(
    virtual_litch_runtime_health_fact* out_fact);
void virtual_litch_default_runtime_health(
    virtual_litch_runtime_health* out_health);
virtual_litch_result virtual_litch_dispatch_runtime_health_fact(
    virtual_litch* board,
    const virtual_litch_runtime_health_fact* fact);
virtual_litch_result virtual_litch_get_runtime_health(
    const virtual_litch* board,
    virtual_litch_runtime_health* out_health);

void virtual_litch_default_sleep_lifecycle(
    virtual_litch_sleep_lifecycle* out_lifecycle);
virtual_litch_result virtual_litch_get_sleep_lifecycle(
    const virtual_litch* board,
    virtual_litch_sleep_lifecycle* out_lifecycle);
virtual_litch_result virtual_litch_request_sleep(
    virtual_litch* board,
    uint8_t origin);
virtual_litch_result virtual_litch_dispatch_validated_motion_or_tap(
    virtual_litch* board);
virtual_litch_result virtual_litch_resolve_sleep_preflights(
    virtual_litch* board,
    uint8_t persistence_succeeded,
    uint8_t wake_source_succeeded);
virtual_litch_result virtual_litch_simulate_sleep_wake(
    virtual_litch* board);
virtual_litch_result virtual_litch_report_wake_buttons_released(
    virtual_litch* board);

#ifdef __cplusplus
}
#endif
