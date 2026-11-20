#include <cstdio>
#include <cstring>

#include "trace/trace_event_tokens.hpp"
#include "trace/trace_jsonl_event_reader.hpp"
#include "trace/trace_jsonl_parser.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckBoolFromLineAndObject(const char* line,
                                const trace::jsonl::ObjectView& object,
                                const char* key,
                                const bool expectedParsed,
                                const bool expectedValue) {
    bool lineValue = false;
    const bool parsedFromLine = trace::jsonl::TryGetBool(line, key, lineValue);
    Check(parsedFromLine == expectedParsed, "line parse status");
    if (parsedFromLine) {
        Check(lineValue == expectedValue, "line parse value");
    }

    bool objectValue = false;
    const bool parsedFromObject = trace::jsonl::TryGetBool(object, key, objectValue);
    Check(parsedFromObject == expectedParsed, "object parse status");
    if (parsedFromObject) {
        Check(objectValue == expectedValue, "object parse value");
    }
}

} // namespace

int main() {
    std::puts("[TRACE_JSONL_PARSER] start");

    const char* line =
        "{\"bool_true\":true,\"bool_false\":false,\"num_true\":1,\"num_false\":0,"
        "\"str_true\":\"true\",\"str_false\":\"false\",\"str_num_true\":\"1\","
        "\"str_num_false\":\"0\",\"invalid\":\"x\"}";

    trace::jsonl::ObjectView object;
    object.begin = line;
    object.end = line + std::strlen(line);
    Check(object.IsValid(), "root object view valid");

    CheckBoolFromLineAndObject(line, object, "bool_true", true, true);
    CheckBoolFromLineAndObject(line, object, "bool_false", true, false);
    CheckBoolFromLineAndObject(line, object, "num_true", true, true);
    CheckBoolFromLineAndObject(line, object, "num_false", true, false);
    CheckBoolFromLineAndObject(line, object, "str_true", true, true);
    CheckBoolFromLineAndObject(line, object, "str_false", true, false);
    CheckBoolFromLineAndObject(line, object, "str_num_true", true, true);
    CheckBoolFromLineAndObject(line, object, "str_num_false", true, false);
    CheckBoolFromLineAndObject(line, object, "invalid", false, false);
    CheckBoolFromLineAndObject(line, object, "missing", false, false);

    {
        modules::UserButton button = modules::UserButton::None;
        Check(trace::TryParseTraceButtonId("P1O", button), "parse P1O id");
        Check(button == modules::UserButton::P1O, "P1O maps to P1O");
        Check(trace::TryParseTraceButtonId("Right", button), "parse Right id");
        Check(button == modules::UserButton::Right, "Right maps to Right");
        Check(!trace::TryParseTraceButtonId("Unknown", button), "unknown id rejected");
    }

    {
        fxe::Button::Event event = fxe::Button::Event::Press;
        Check(trace::TryParseTraceButtonEvent("Released", event), "parse Released event");
        Check(event == fxe::Button::Event::Release, "Released maps to Release");
        Check(trace::TryParseTraceButtonEvent("LongHold", event), "parse LongHold event");
        Check(event == fxe::Button::Event::LongHold, "LongHold maps to LongHold");
        Check(!trace::TryParseTraceButtonEvent("noop", event), "unknown event rejected");
    }

    Check(trace::ParseFrameHashKindToken("board") == trace::FrameHashKindToken::Board,
          "parse board frame kind");
    Check(trace::ParseFrameHashKindToken("edge") == trace::FrameHashKindToken::Edge,
          "parse edge frame kind");
    Check(trace::ParseFrameHashKindToken("all") == trace::FrameHashKindToken::Any,
          "unknown frame kind defaults to any");

    {
        const char* buttonLine =
            "{\"type\":\"button\",\"t_ms\":1100,\"id\":\"P1O\",\"evt\":\"LongHold\"}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(buttonLine, rec), "event reader parses button line");
        Check(rec.type == trace::JsonlEventType::Button, "event reader button type");
        Check(rec.tMs == 1100U, "event reader button t_ms");
        Check(rec.hasButton, "event reader has button");
        Check(rec.button == modules::UserButton::P1O, "event reader button id");
        Check(rec.hasButtonEvent, "event reader has button event");
        Check(rec.buttonEvent == fxe::Button::Event::LongHold, "event reader button event");
    }

    {
        const char* controlLine =
            "{\"type\":\"control\",\"t_ms\":1200,\"seq\":7,\"control\":\"undo\",\"count\":2}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(controlLine, rec),
              "event reader parses control line");
        Check(rec.type == trace::JsonlEventType::Control,
              "event reader control type");
        Check(rec.hasObservationSequence && rec.observationSequence == 7U,
              "event reader control sequence");
        Check(rec.control == "undo", "event reader control name");
        Check(rec.hasControlCount && rec.controlCount == 2U,
              "event reader control count");
    }

    {
        const char* frameLine =
            "{\"type\":\"checkpoint_frame\",\"t_ms\":2050,\"frame_hash\":\"0x10\",\"kind\":\"edge\"}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(frameLine, rec), "event reader parses checkpoint_frame");
        Check(rec.type == trace::JsonlEventType::CheckpointFrame, "event reader checkpoint_frame type");
        Check(rec.hasFrameHash, "event reader has frame hash");
        Check(rec.frameHash == 0x10ULL, "event reader frame hash value");
        Check(rec.frameKind == trace::FrameHashKindToken::Edge, "event reader frame kind");
    }

    {
        const char* compactPositionAck =
            "{\"type\":\"position_ack\",\"t\":21,\"s\":8,"
            "\"o\":\"0x1\",\"k\":\"0x2\",\"r\":\"0x4\","
            "\"m\":\"0x8\",\"n\":\"0x10\",\"p\":\"0x20\","
            "\"e\":18,\"c\":26,\"f\":255,\"l\":\"0x40\"}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(compactPositionAck, rec),
              "event reader parses compact position acknowledgement");
        Check(rec.type == trace::JsonlEventType::PositionAcknowledged &&
                  rec.tMs == 21U && rec.hasObservationSequence &&
                  rec.observationSequence == 8U,
              "compact position acknowledgement parses time and sequence");
        Check(rec.hasOccupiedMask && rec.occupiedMask == 1ULL &&
                  rec.hasKingMask && rec.kingMask == 2ULL &&
                  rec.hasRookMask && rec.rookMask == 4ULL &&
                  rec.hasMoverMask && rec.moverMask == 8ULL &&
                  rec.hasOpponentMask && rec.opponentMask == 16ULL &&
                  rec.hasPawnMask && rec.pawnMask == 32ULL,
              "compact position acknowledgement parses piece masks");
        Check(rec.hasEnPassantTargetSquare &&
                  rec.enPassantTargetSquare == 18U &&
                  rec.hasEnPassantCaptureSquare &&
                  rec.enPassantCaptureSquare == 26U &&
                  rec.hasSideToMoveValid && rec.sideToMoveValid &&
                  rec.hasSideToMoveBlack && rec.sideToMoveBlack &&
                  rec.hasLegalCaptureTargetsValid &&
                  rec.legalCaptureTargetsValid &&
                  rec.hasLegalCaptureTargetsMask &&
                  rec.legalCaptureTargetsMask == 64ULL &&
                  rec.hasLegalCastlingTargetsValid &&
                  rec.legalCastlingTargetsValid &&
                  rec.hasLegalCastlingKingTargetsMask &&
                  rec.legalCastlingKingTargetsMask ==
                      ((1ULL << 62U) | (1ULL << 58U) |
                       (1ULL << 6U) | (1ULL << 2U)),
              "compact position acknowledgement parses context and flags");
    }

    {
        const char* legacyPositionAck =
            "{\"type\":\"position_ack\",\"t_ms\":22,\"seq\":9,"
            "\"occupied_mask\":\"0x1\",\"king_mask\":\"0x2\","
            "\"rook_mask\":\"0x4\",\"mover_mask\":\"0x8\","
            "\"opponent_mask\":\"0x10\",\"pawn_mask\":\"0x20\","
            "\"ep_t\":18,\"ep_c\":26,\"stm_v\":1,\"stm_b\":0,"
            "\"legal_v\":1,\"legal_mask\":\"0x40\","
            "\"castle_v\":1,\"castle_mask\":\"0x4000000000000000\"}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(legacyPositionAck, rec) &&
                  rec.tMs == 22U && rec.hasObservationSequence &&
                  rec.observationSequence == 9U &&
                  rec.hasOccupiedMask && rec.occupiedMask == 1ULL &&
                  rec.hasOpponentMask && rec.opponentMask == 16ULL &&
                  rec.hasSideToMoveValid && rec.sideToMoveValid &&
                  rec.hasSideToMoveBlack && !rec.sideToMoveBlack &&
                  rec.hasLegalCaptureTargetsMask &&
                  rec.legalCaptureTargetsMask == 64ULL &&
                  rec.hasLegalCastlingTargetsValid &&
                  rec.legalCastlingTargetsValid &&
                  rec.hasLegalCastlingKingTargetsMask &&
                  rec.legalCastlingKingTargetsMask == (1ULL << 62U),
              "event reader retains legacy long position acknowledgement keys");
    }

    {
        const char* variantLine =
            "{\"type\":\"checkpoint_variant\",\"v\":2,"
            "\"ds\":18,\"dp\":1,\"pd\":1,\"pf\":52,\"pt\":36,"
            "\"pp\":113,\"or\":2,\"win\":2,\"vh\":\"0x5678\","
            "\"ah\":\"0x1234\"}";
        trace::JsonlEventRecord rec;
        Check(trace::TryParseJsonlEventRecord(variantLine, rec),
              "event reader parses Duck checkpoint fields");
        Check(rec.type == trace::JsonlEventType::CheckpointVariant,
              "event reader recognizes variant checkpoint type");
        Check(rec.hasGameVariant && rec.gameVariant == 2U,
              "event reader parses variant");
        Check(rec.hasDuckSquare && rec.duckSquare == 18U,
              "event reader parses duck square");
        Check(rec.hasDuckTurnPhase && rec.duckTurnPhase == 1U,
              "event reader parses duck phase");
        Check(rec.hasPendingChessMove && rec.pendingChessMove,
              "event reader parses pending flag");
        Check(rec.hasPendingMoveFrom && rec.pendingMoveFrom == 52U &&
                  rec.hasPendingMoveTo && rec.pendingMoveTo == 36U &&
                  rec.hasPendingPromotion && rec.pendingPromotion == 113U,
              "event reader parses pending move");
        Check(rec.hasVariantOutcomeReason && rec.variantOutcomeReason == 2U &&
                  rec.hasVariantWinner && rec.variantWinner == 2U,
              "event reader parses Duck outcome");
        Check(rec.hasVariantStateHash && rec.variantStateHash == 0x5678ULL,
              "event reader parses variant hash");
        Check(rec.hasAppStateHash && rec.appStateHash == 0x1234ULL,
              "event reader parses app hash");
    }

    if (g_failures == 0) {
        std::puts("[TRACE_JSONL_PARSER] ok");
        return 0;
    }

    std::printf("[TRACE_JSONL_PARSER] failures=%d\n", g_failures);
    return 1;
}
