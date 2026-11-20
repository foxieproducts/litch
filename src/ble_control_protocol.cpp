#include "modules/ble_control_protocol.hpp"

#include <cstdio>
#include <cstring>

namespace modules {

ControlAction ParseBleControlActionToken(const char* token) {
    if (token == nullptr) {
        return ControlAction::Unknown;
    }
    if (std::strcmp(token, "hint") == 0 || std::strcmp(token, "h") == 0) {
        return ControlAction::Hint;
    }
    if (std::strcmp(token, "undo") == 0 || std::strcmp(token, "u") == 0) {
        return ControlAction::Undo;
    }
    if (std::strcmp(token, "new_game") == 0 || std::strcmp(token, "n") == 0) {
        return ControlAction::NewGame;
    }
    if (std::strcmp(token, "coaching") == 0 || std::strcmp(token, "c") == 0) {
        return ControlAction::Coaching;
    }
    return ControlAction::Unknown;
}

const char* BleControlActionToString(const ControlAction action) {
    switch (action) {
        case ControlAction::Hint:
            return "hint";
        case ControlAction::Undo:
            return "undo";
        case ControlAction::NewGame:
            return "new_game";
        case ControlAction::Coaching:
            return "coaching";
        case ControlAction::ReformatStorage:
            return "reformat_storage";
        case ControlAction::Sleep:
            return "sleep";
        case ControlAction::Unknown:
            return "unknown";
    }
    return "unknown";
}

bool TryParseBleControlActionRequest(
    const JsonVariantConst root,
    ParsedBleControlActionRequest& outRequest) {
    outRequest = ParsedBleControlActionRequest{};

    const JsonVariantConst actionValue = root["a"];
    if (!actionValue.is<const char*>()) {
        return false;
    }
    const char* actionToken = actionValue.as<const char*>();
    if (actionToken == nullptr || actionToken[0] == '\0') {
        return false;
    }
    outRequest.action = ParseBleControlActionToken(actionToken);

    if (root.containsKey("o")) {
        const JsonVariantConst ownerValue = root["o"];
        if (!ownerValue.is<const char*>()) {
            return false;
        }
        const char* ownerToken = ownerValue.as<const char*>();
        if (ownerToken == nullptr || ownerToken[0] == '\0') {
            return false;
        }
        if (std::strcmp(ownerToken, "p1") == 0) {
            outRequest.owner = UiOwner::P1;
        } else if (std::strcmp(ownerToken, "p2") == 0) {
            outRequest.owner = UiOwner::P2;
        } else {
            return false;
        }
    }

    if (root.containsKey("e")) {
        if (!root["e"].is<bool>()) {
            return false;
        }
        outRequest.coachingMutation = root["e"].as<bool>()
                                          ? CoachingMutation::Enable
                                          : CoachingMutation::Disable;
    }

    if (root.containsKey("n")) {
        if (!root["n"].is<uint32_t>()) {
            return false;
        }
        const uint32_t undoCount = root["n"].as<uint32_t>();
        if (undoCount == 0U || undoCount > UINT8_MAX) {
            return false;
        }
        outRequest.undoMoveCount = static_cast<uint8_t>(undoCount);
    }

    return true;
}

bool TryDecodeConnectionControlAction(
    const uint8_t* const body,
    const size_t size,
    ParsedBleControlActionRequest& outRequest) {
    outRequest = ParsedBleControlActionRequest{};
    if (body == nullptr || size != ble_control_wire::ACTION_BODY_BYTES ||
        body[0U] != ble_control_wire::VERSION) {
        return false;
    }

    const auto action = static_cast<ControlAction>(body[1U]);
    const auto owner = static_cast<UiOwner>(body[2U]);
    const auto coachingMutation = static_cast<CoachingMutation>(body[3U]);
    const uint8_t undoMoveCount = body[4U];
    const bool canonical =
        ((action == ControlAction::Hint || action == ControlAction::NewGame ||
          action == ControlAction::ReformatStorage ||
          action == ControlAction::Sleep) &&
         owner == UiOwner::None &&
         coachingMutation == CoachingMutation::Toggle &&
         undoMoveCount == 1U) ||
        (action == ControlAction::Undo && owner == UiOwner::None &&
         coachingMutation == CoachingMutation::Toggle &&
         undoMoveCount != 0U) ||
        (action == ControlAction::Coaching &&
         (owner == UiOwner::P1 || owner == UiOwner::P2) &&
         (coachingMutation == CoachingMutation::Toggle ||
          coachingMutation == CoachingMutation::Disable ||
          coachingMutation == CoachingMutation::Enable) &&
         undoMoveCount == 1U);
    if (!canonical) {
        return false;
    }

    outRequest.action = action;
    outRequest.owner = owner;
    outRequest.coachingMutation = coachingMutation;
    outRequest.undoMoveCount = undoMoveCount;
    return true;
}

ControlMutationReason BleSettingReasonFromErrorToken(
    const char* const errorToken) {
    if (errorToken == nullptr || errorToken[0] == '\0' ||
        std::strcmp(errorToken, "none") == 0) {
        return ControlMutationReason::None;
    }
    if (std::strcmp(errorToken, "bad_request") == 0 ||
        std::strcmp(errorToken, "value_too_large") == 0 ||
        std::strcmp(errorToken, "response_too_large") == 0) {
        return ControlMutationReason::BadRequest;
    }
    if (std::strcmp(errorToken, "queue_full") == 0) {
        return ControlMutationReason::QueueFull;
    }
    if (std::strcmp(errorToken, "unknown_key") == 0) {
        return ControlMutationReason::UnknownKey;
    }
    if (std::strcmp(errorToken, "type_mismatch") == 0) {
        return ControlMutationReason::TypeMismatch;
    }
    if (std::strcmp(errorToken, "out_of_range") == 0) {
        return ControlMutationReason::OutOfRange;
    }
    if (std::strcmp(errorToken, "invalid_enum_value") == 0) {
        return ControlMutationReason::InvalidEnumValue;
    }
    if (std::strcmp(errorToken, "not_at_start") == 0) {
        return ControlMutationReason::NotAtStart;
    }
    if (std::strcmp(errorToken, "busy") == 0) {
        return ControlMutationReason::Busy;
    }
    if (std::strcmp(errorToken, "unsupported") == 0) {
        return ControlMutationReason::Unsupported;
    }
    if (std::strcmp(errorToken, "storage_unavailable") == 0) {
        return ControlMutationReason::StorageUnavailable;
    }
    return ControlMutationReason::BadRequest;
}

size_t FormatBleSettingCompactResponse(const uint16_t requestId,
                                       const bool ok,
                                       const ControlMutationReason reason,
                                       char* const outBuffer,
                                       const size_t outCapacity) {
    if (outBuffer == nullptr || outCapacity == 0U || requestId == 0U) {
        return 0U;
    }

    const int len = std::snprintf(
        outBuffer, outCapacity, "{\"y\":[%u,%u,%u]}",
        static_cast<unsigned>(requestId), ok ? 1U : 0U,
        static_cast<unsigned>(reason));
    if (len <= 0 || static_cast<size_t>(len) >= outCapacity) {
        if (outCapacity != 0U) {
            outBuffer[0] = '\0';
        }
        return 0U;
    }
    return static_cast<size_t>(len);
}

} // namespace modules
