#pragma once

#include <cstdint>
#include <string_view>

#include "modules/user_input.hpp"
#include "trace/trace_event_tokens.hpp"
#include "trace/trace_jsonl_parser.hpp"

namespace trace {

enum class JsonlEventType : uint8_t {
    Unknown = 0,
    Header,
    Settings,
    Tick,
    Sensor,
    BoardObservation,
    Button,
    Control,
    PositionAcknowledged,
    ActionResult,
    AdapterResult,
    Cmd,
    CheckpointState,
    CheckpointInteraction,
    CheckpointVariant,
    CheckpointEngine,
    CheckpointFrame,
    CheckpointInvariant,
    End,
};

struct JsonlEventRecord {
    JsonlEventType type = JsonlEventType::Unknown;
    uint32_t tMs = 0;

    // settings
    bool hasSettingsJson = false;
    jsonl::ObjectView settingsJson = {};

    // sensor
    bool hasSquare = false;
    uint8_t square = 0;
    bool hasOcc = false;
    bool occ = false;

    // atomic board observation
    bool hasChangedMask = false;
    uint64_t changedMask = 0;
    bool hasOccupiedMask = false;
    uint64_t occupiedMask = 0;
    bool hasObservationSequence = false;
    uint32_t observationSequence = 0;
    uint8_t deferredDuckDestinationSquare = 255U;
    bool hasKingMask = false;
    uint64_t kingMask = 0;
    bool hasRookMask = false;
    uint64_t rookMask = 0;
    bool hasPawnMask = false;
    uint64_t pawnMask = 0;
    bool hasMoverMask = false;
    uint64_t moverMask = 0;
    bool hasOpponentMask = false;
    uint64_t opponentMask = 0;
    bool hasEnPassantTargetSquare = false;
    uint8_t enPassantTargetSquare = 255U;
    bool hasEnPassantCaptureSquare = false;
    uint8_t enPassantCaptureSquare = 255U;
    bool hasSideToMoveValid = false;
    bool sideToMoveValid = false;
    bool hasSideToMoveBlack = false;
    bool sideToMoveBlack = false;
    bool hasLegalCaptureTargetsValid = false;
    bool legalCaptureTargetsValid = false;
    bool hasLegalCaptureTargetsMask = false;
    uint64_t legalCaptureTargetsMask = 0;
    std::string_view captureRows = {};
    bool hasLegalCastlingTargetsValid = false;
    bool legalCastlingTargetsValid = false;
    bool hasLegalCastlingKingTargetsMask = false;
    uint64_t legalCastlingKingTargetsMask = 0;
    bool hasSucceeded = false;
    bool succeeded = false;
    std::string_view adapter = {};

    // button
    bool hasButton = false;
    modules::UserButton button = modules::UserButton::None;
    bool hasButtonEvent = false;
    fxe::Button::Event buttonEvent = fxe::Button::Event::Press;

    // semantic control
    std::string_view control = {};
    bool hasControlCount = false;
    uint8_t controlCount = 0;

    // cmd/checkpoint payloads
    std::string_view cmd = {};
    std::string_view fen = {};
    std::string_view gameState = {};

    // checkpoint_state flags
    bool hasPlacement = false;
    bool placement = false;
    bool hasCpuThinking = false;
    bool cpuThinking = false;
    bool hasHumanIllegal = false;
    bool humanIllegal = false;
    bool hasCpuIllegal = false;
    bool cpuIllegal = false;

    // checkpoint_interaction
    std::string_view interactionPhase = {};
    std::string_view transitionKind = {};
    std::string_view requiredAction = {};
    std::string_view shadowDivergence = {};
    bool hasExpectedMask = false;
    uint64_t expectedMask = 0;
    bool hasPhysicalMask = false;
    uint64_t physicalMask = 0;
    bool hasRelocationsLifted = false;
    uint32_t relocationsLifted = 0;
    bool hasRelocationsPlaced = false;
    uint32_t relocationsPlaced = 0;
    bool hasRemovalComplete = false;
    bool removalComplete = false;
    bool hasRestorationComplete = false;
    bool restorationComplete = false;
    bool hasActionAcknowledged = false;
    bool actionAcknowledged = false;
    bool hasActionToken = false;
    uint32_t actionToken = 0;
    bool hasModelHash = false;
    uint64_t modelHash = 0;
    bool hasGameVariant = false;
    uint8_t gameVariant = 0;
    bool hasDuckSquare = false;
    uint8_t duckSquare = 255U;
    bool hasDuckTurnPhase = false;
    uint8_t duckTurnPhase = 0;
    bool hasPendingChessMove = false;
    bool pendingChessMove = false;
    bool hasPendingMoveFrom = false;
    uint8_t pendingMoveFrom = 255U;
    bool hasPendingMoveTo = false;
    uint8_t pendingMoveTo = 255U;
    bool hasPendingPromotion = false;
    uint8_t pendingPromotion = 0U;
    bool hasVariantOutcomeReason = false;
    uint8_t variantOutcomeReason = 0U;
    bool hasVariantWinner = false;
    uint8_t variantWinner = 0U;
    bool hasVariantStateHash = false;
    uint64_t variantStateHash = 0ULL;
    bool hasAppStateHash = false;
    uint64_t appStateHash = 0ULL;

    // checkpoint_invariant flags
    bool hasCpuEverStarted = false;
    bool cpuEverStarted = false;
    bool hasCpuMoveQueued = false;
    bool cpuMoveQueued = false;

    // checkpoint_frame
    bool hasFrameHash = false;
    uint64_t frameHash = 0;
    FrameHashKindToken frameKind = FrameHashKindToken::Any;
};

inline JsonlEventType ParseJsonlEventType(const std::string_view type) {
    if (type == "header") {
        return JsonlEventType::Header;
    }
    if (type == "settings") {
        return JsonlEventType::Settings;
    }
    if (type == "tick") {
        return JsonlEventType::Tick;
    }
    if (type == "sensor") {
        return JsonlEventType::Sensor;
    }
    if (type == "board_observation") {
        return JsonlEventType::BoardObservation;
    }
    if (type == "button") {
        return JsonlEventType::Button;
    }
    if (type == "control") {
        return JsonlEventType::Control;
    }
    if (type == "position_ack") {
        return JsonlEventType::PositionAcknowledged;
    }
    if (type == "action_result") {
        return JsonlEventType::ActionResult;
    }
    if (type == "adapter_result") {
        return JsonlEventType::AdapterResult;
    }
    if (type == "cmd") {
        return JsonlEventType::Cmd;
    }
    if (type == "checkpoint_state") {
        return JsonlEventType::CheckpointState;
    }
    if (type == "checkpoint_interaction") {
        return JsonlEventType::CheckpointInteraction;
    }
    if (type == "checkpoint_variant") {
        return JsonlEventType::CheckpointVariant;
    }
    if (type == "checkpoint_engine") {
        return JsonlEventType::CheckpointEngine;
    }
    if (type == "checkpoint_frame") {
        return JsonlEventType::CheckpointFrame;
    }
    if (type == "checkpoint_invariant") {
        return JsonlEventType::CheckpointInvariant;
    }
    if (type == "end") {
        return JsonlEventType::End;
    }
    return JsonlEventType::Unknown;
}

inline void ParseVariantCheckpointFields(
    const char* line,
    JsonlEventRecord& out) {
    uint32_t value = 0U;
    if ((jsonl::TryGetU32(line, "v", value) ||
         jsonl::TryGetU32(line, "variant", value)) && value <= UINT8_MAX) {
        out.hasGameVariant = true;
        out.gameVariant = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "ds", value) ||
         jsonl::TryGetU32(line, "duck_square", value)) && value <= UINT8_MAX) {
        out.hasDuckSquare = true;
        out.duckSquare = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "dp", value) ||
         jsonl::TryGetU32(line, "duck_phase", value)) && value <= UINT8_MAX) {
        out.hasDuckTurnPhase = true;
        out.duckTurnPhase = static_cast<uint8_t>(value);
    }
    out.hasPendingChessMove =
        jsonl::TryGetBool(line, "pd", out.pendingChessMove) ||
        jsonl::TryGetBool(line, "pending", out.pendingChessMove);
    if ((jsonl::TryGetU32(line, "pf", value) ||
         jsonl::TryGetU32(line, "pending_from", value)) && value <= UINT8_MAX) {
        out.hasPendingMoveFrom = true;
        out.pendingMoveFrom = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "pt", value) ||
         jsonl::TryGetU32(line, "pending_to", value)) && value <= UINT8_MAX) {
        out.hasPendingMoveTo = true;
        out.pendingMoveTo = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "pp", value) ||
         jsonl::TryGetU32(line, "pending_promotion", value)) &&
        value <= UINT8_MAX) {
        out.hasPendingPromotion = true;
        out.pendingPromotion = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "or", value) ||
         jsonl::TryGetU32(line, "outcome_reason", value)) &&
        value <= UINT8_MAX) {
        out.hasVariantOutcomeReason = true;
        out.variantOutcomeReason = static_cast<uint8_t>(value);
    }
    if ((jsonl::TryGetU32(line, "win", value) ||
         jsonl::TryGetU32(line, "winner", value)) && value <= UINT8_MAX) {
        out.hasVariantWinner = true;
        out.variantWinner = static_cast<uint8_t>(value);
    }
    out.hasVariantStateHash = jsonl::TryGetU64HexOrDec(
        line, "vh", out.variantStateHash) ||
                              jsonl::TryGetU64HexOrDec(
                                  line, "variant_hash", out.variantStateHash);
    out.hasAppStateHash = jsonl::TryGetU64HexOrDec(
        line, "ah", out.appStateHash) ||
                          jsonl::TryGetU64HexOrDec(
                              line, "app_hash", out.appStateHash);
}

inline bool TryParseJsonlEventRecord(const char* line, JsonlEventRecord& out) {
    out = {};

    std::string_view typeView;
    if (!jsonl::TryGetStringView(line, "type", typeView) || typeView.empty()) {
        return false;
    }

    out.type = ParseJsonlEventType(typeView);
    if (!jsonl::TryGetU32(line, "t_ms", out.tMs)) {
        (void)jsonl::TryGetU32(line, "t", out.tMs);
    }

    if (out.type == JsonlEventType::Settings) {
        out.hasSettingsJson = jsonl::TryGetObject(line, "json", out.settingsJson);
        return true;
    }

    if (out.type == JsonlEventType::Sensor) {
        uint32_t square = 0;
        if (jsonl::TryGetU32(line, "square", square)) {
            out.hasSquare = true;
            out.square = static_cast<uint8_t>(square);
        }

        uint32_t occRaw = 0;
        if (jsonl::TryGetU32(line, "occ", occRaw) ||
            jsonl::TryGetU32(line, "occupied", occRaw)) {
            out.hasOcc = true;
            out.occ = occRaw != 0U;
        }
        return true;
    }

    if (out.type == JsonlEventType::BoardObservation) {
        uint64_t deferredDuck = 255U;
        if (jsonl::TryGetU64HexOrDec(line, "dd", deferredDuck)) {
            if (deferredDuck >= 64U) return false;
            out.deferredDuckDestinationSquare = static_cast<uint8_t>(deferredDuck);
        }
        out.hasChangedMask = jsonl::TryGetU64HexOrDec(
            line,
            "changed_mask",
            out.changedMask);
        out.hasOccupiedMask = jsonl::TryGetU64HexOrDec(
            line,
            "occupied_mask",
            out.occupiedMask);
        out.hasObservationSequence = jsonl::TryGetU32(
            line,
            "seq",
            out.observationSequence);
        return true;
    }

    if (out.type == JsonlEventType::Button) {
        std::string_view id;
        if (jsonl::TryGetStringView(line, "id", id) && !id.empty()) {
            modules::UserButton b = modules::UserButton::None;
            if (TryParseTraceButtonId(id, b)) {
                out.hasButton = true;
                out.button = b;
            }
        }

        std::string_view event;
        if (jsonl::TryGetStringView(line, "evt", event) && !event.empty()) {
            fxe::Button::Event e = fxe::Button::Event::Press;
            if (TryParseTraceButtonEvent(event, e)) {
                out.hasButtonEvent = true;
                out.buttonEvent = e;
            }
        }
        return true;
    }

    if (out.type == JsonlEventType::Control) {
        out.hasObservationSequence = jsonl::TryGetU32(
            line, "seq", out.observationSequence);
        (void)jsonl::TryGetStringView(line, "control", out.control);
        uint32_t count = 0;
        if (jsonl::TryGetU32(line, "count", count) && count <= UINT8_MAX) {
            out.hasControlCount = true;
            out.controlCount = static_cast<uint8_t>(count);
        }
        return true;
    }

    if (out.type == JsonlEventType::PositionAcknowledged) {
        out.hasObservationSequence = jsonl::TryGetU32(
            line, "s", out.observationSequence) ||
                                     jsonl::TryGetU32(
                                         line, "seq", out.observationSequence);
        out.hasOccupiedMask = jsonl::TryGetU64HexOrDec(
            line, "o", out.occupiedMask) ||
                              jsonl::TryGetU64HexOrDec(
                                  line, "occupied_mask", out.occupiedMask);
        out.hasKingMask = jsonl::TryGetU64HexOrDec(
            line, "k", out.kingMask) ||
                          jsonl::TryGetU64HexOrDec(
                              line, "king_mask", out.kingMask);
        out.hasRookMask = jsonl::TryGetU64HexOrDec(
            line, "r", out.rookMask) ||
                          jsonl::TryGetU64HexOrDec(
                              line, "rook_mask", out.rookMask);
        out.hasMoverMask = jsonl::TryGetU64HexOrDec(
            line, "m", out.moverMask) ||
                           jsonl::TryGetU64HexOrDec(
                               line, "mover_mask", out.moverMask);
        out.hasOpponentMask = jsonl::TryGetU64HexOrDec(
            line, "n", out.opponentMask) ||
                              jsonl::TryGetU64HexOrDec(
                                  line, "opponent_mask", out.opponentMask);
        out.hasPawnMask = jsonl::TryGetU64HexOrDec(
            line, "p", out.pawnMask) ||
                          jsonl::TryGetU64HexOrDec(
                              line, "pawn_mask", out.pawnMask);
        uint32_t epSquare = 0;
        if ((jsonl::TryGetU32(line, "e", epSquare) ||
             jsonl::TryGetU32(line, "ep_t", epSquare)) &&
            epSquare <= 255U) {
            out.hasEnPassantTargetSquare = true;
            out.enPassantTargetSquare = static_cast<uint8_t>(epSquare);
        }
        if ((jsonl::TryGetU32(line, "c", epSquare) ||
             jsonl::TryGetU32(line, "ep_c", epSquare)) &&
            epSquare <= 255U) {
            out.hasEnPassantCaptureSquare = true;
            out.enPassantCaptureSquare = static_cast<uint8_t>(epSquare);
        }
        uint32_t flags = 0U;
        if (jsonl::TryGetU32(line, "f", flags)) {
            out.hasSideToMoveValid = true;
            out.sideToMoveValid = (flags & 1U) != 0U;
            out.hasSideToMoveBlack = true;
            out.sideToMoveBlack = (flags & 2U) != 0U;
            out.hasLegalCaptureTargetsValid = true;
            out.legalCaptureTargetsValid = (flags & 4U) != 0U;
            out.hasLegalCastlingTargetsValid = true;
            out.legalCastlingTargetsValid = (flags & 8U) != 0U;
            out.hasLegalCastlingKingTargetsMask = true;
            out.legalCastlingKingTargetsMask =
                ((flags & 16U) != 0U ? (1ULL << 62U) : 0ULL) |
                ((flags & 32U) != 0U ? (1ULL << 58U) : 0ULL) |
                ((flags & 64U) != 0U ? (1ULL << 6U) : 0ULL) |
                ((flags & 128U) != 0U ? (1ULL << 2U) : 0ULL);
        } else {
            out.hasSideToMoveValid = jsonl::TryGetBool(
                line, "stm_v", out.sideToMoveValid);
            out.hasSideToMoveBlack = jsonl::TryGetBool(
                line, "stm_b", out.sideToMoveBlack);
            out.hasLegalCaptureTargetsValid = jsonl::TryGetBool(
                line, "legal_v", out.legalCaptureTargetsValid);
            out.hasLegalCastlingTargetsValid = jsonl::TryGetBool(
                line, "castle_v", out.legalCastlingTargetsValid);
        }
        out.hasLegalCaptureTargetsMask = jsonl::TryGetU64HexOrDec(
            line, "l", out.legalCaptureTargetsMask) ||
                                              jsonl::TryGetU64HexOrDec(
                                                  line, "legal_mask",
                                                  out.legalCaptureTargetsMask);
        const char* captureBegin = nullptr;
        const char* captureEnd = nullptr;
        if (jsonl::detail::TryGetValueRange(jsonl::detail::RootObject(line),
                "capture_rows", captureBegin, captureEnd) &&
            (!jsonl::TryGetStringView(line, "capture_rows", out.captureRows) || out.captureRows.empty())) {
            return false;
        }
        uint64_t longCastlingMask = 0ULL;
        if (jsonl::TryGetU64HexOrDec(
                line, "castle_mask", longCastlingMask)) {
            out.hasLegalCastlingKingTargetsMask = true;
            out.legalCastlingKingTargetsMask = longCastlingMask;
        }
        return true;
    }

    if (out.type == JsonlEventType::ActionResult) {
        out.hasObservationSequence = jsonl::TryGetU32(
            line, "seq", out.observationSequence);
        out.hasActionToken = jsonl::TryGetU32(
            line, "token", out.actionToken);
        out.hasSucceeded = jsonl::TryGetBool(
            line, "ok", out.succeeded);
        return true;
    }

    if (out.type == JsonlEventType::AdapterResult) {
        out.hasObservationSequence = jsonl::TryGetU32(
            line, "seq", out.observationSequence);
        (void)jsonl::TryGetStringView(line, "adapter", out.adapter);
        out.hasSucceeded = jsonl::TryGetBool(
            line, "ok", out.succeeded);
        return true;
    }

    if (out.type == JsonlEventType::Cmd) {
        (void)jsonl::TryGetStringView(line, "cmd", out.cmd);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointEngine) {
        (void)jsonl::TryGetStringView(line, "fen", out.fen);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointState) {
        (void)jsonl::TryGetStringView(line, "game_state", out.gameState);
        out.hasPlacement = jsonl::TryGetBool(line, "placement", out.placement);
        out.hasCpuThinking = jsonl::TryGetBool(line, "cpu", out.cpuThinking);
        out.hasHumanIllegal = jsonl::TryGetBool(line, "human_illegal", out.humanIllegal);
        out.hasCpuIllegal = jsonl::TryGetBool(line, "cpu_illegal", out.cpuIllegal);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointInteraction) {
        if (!jsonl::TryGetStringView(line, "ph", out.interactionPhase)) {
            (void)jsonl::TryGetStringView(line, "phase", out.interactionPhase);
        }
        if (!jsonl::TryGetStringView(line, "pl", out.transitionKind)) {
            (void)jsonl::TryGetStringView(line, "plan", out.transitionKind);
        }
        if (!jsonl::TryGetStringView(line, "req", out.requiredAction)) {
            (void)jsonl::TryGetStringView(line, "required", out.requiredAction);
        }
        if (!jsonl::TryGetStringView(line, "d", out.shadowDivergence) &&
            !jsonl::TryGetStringView(line, "div", out.shadowDivergence)) {
            (void)jsonl::TryGetStringView(line, "divergence", out.shadowDivergence);
        }
        out.hasExpectedMask = jsonl::TryGetU64HexOrDec(
            line, "exp", out.expectedMask) ||
                              jsonl::TryGetU64HexOrDec(
                                  line, "expected", out.expectedMask);
        out.hasPhysicalMask = jsonl::TryGetU64HexOrDec(
            line, "physical", out.physicalMask);
        out.hasRelocationsLifted = jsonl::TryGetU32(
            line, "l", out.relocationsLifted) ||
                                   jsonl::TryGetU32(
                                       line, "lifted", out.relocationsLifted);
        out.hasRelocationsPlaced = jsonl::TryGetU32(
            line, "p", out.relocationsPlaced) ||
                                  jsonl::TryGetU32(
                                      line, "placed", out.relocationsPlaced);
        out.hasRemovalComplete = jsonl::TryGetBool(
            line, "r", out.removalComplete) ||
                                 jsonl::TryGetBool(
                                     line, "removed", out.removalComplete);
        out.hasRestorationComplete = jsonl::TryGetBool(
            line, "s", out.restorationComplete) ||
                                     jsonl::TryGetBool(
                                         line, "restored", out.restorationComplete);
        out.hasActionAcknowledged = jsonl::TryGetBool(
            line, "a", out.actionAcknowledged) ||
                                    jsonl::TryGetBool(
                                        line, "acked", out.actionAcknowledged);
        out.hasActionToken = jsonl::TryGetU32(
            line, "tok", out.actionToken) ||
                             jsonl::TryGetU32(
                                 line, "token", out.actionToken);
        out.hasModelHash = jsonl::TryGetU64HexOrDec(
            line, "h", out.modelHash) ||
                           jsonl::TryGetU64HexOrDec(
                               line, "hash", out.modelHash) ||
                           jsonl::TryGetU64HexOrDec(
                               line, "model_hash", out.modelHash);
        ParseVariantCheckpointFields(line, out);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointVariant) {
        ParseVariantCheckpointFields(line, out);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointInvariant) {
        out.hasCpuEverStarted = jsonl::TryGetBool(line, "cpu_started", out.cpuEverStarted);
        out.hasCpuMoveQueued = jsonl::TryGetBool(line, "cpu_move_queued", out.cpuMoveQueued);
        return true;
    }

    if (out.type == JsonlEventType::CheckpointFrame) {
        out.hasFrameHash = jsonl::TryGetU64HexOrDec(line, "frame_hash", out.frameHash);
        std::string_view kind;
        if (jsonl::TryGetStringView(line, "kind", kind)) {
            out.frameKind = ParseFrameHashKindToken(kind);
        }
        return true;
    }

    return true;
}

} // namespace trace
