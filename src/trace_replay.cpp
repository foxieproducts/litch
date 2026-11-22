#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "trace/trace_replay.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/engine.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/orientation.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/event_bus.hpp"
#include "runtime/square_mapping.hpp"
#include "trace/trace_jsonl_event_reader.hpp"
#include "trace/trace_replay_adapter_guard.hpp"
#include "trace/trace_replay_interaction_checkpoint.hpp"
#include "trace/trace_replay_interaction_events.hpp"
#include "trace/trace_jsonl_parser.hpp"

#include "trace_replay_event_dispatch.hpp"
#include "trace_replay_frames.hpp"
#include "trace_replay_settings.hpp"

namespace {

void SetFailure(trace::ReplayResult& outResult, const uint32_t line, const char* msg) {
    outResult.ok = false;
    outResult.failureLine = line;
    std::snprintf(outResult.message, sizeof(outResult.message), "%s",
                  msg ? msg : "?");
}

bool ReloadPersistedReplayState(runtime::GameStateMachine& sm,
                                modules::GamePersistenceCoordinator& gamePersistence,
                                trace::ReplayResult& outResult,
                                const uint32_t lineNumber) {
    sm.Reset();
    if (!gamePersistence.LoadOrInitialize()) {
        SetFailure(outResult, lineNumber, "persist_reload_failed");
        return false;
    }
    return true;
}

bool StartsWith(const std::string_view s, const std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

const char* SkipSpaces(const char* s) {
    if (!s) {
        return nullptr;
    }
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

} // namespace

namespace trace {

bool ReplayJsonLines(const char* const* lines,
                     const size_t lineCount,
                     runtime::ManualClock& clock,
                     runtime::GameStateMachine& sm,
                     ReplayResult& outResult,
                     modules::GamePersistenceCoordinator* const gamePersistence,
                     modules::IEngine* const engine) {
    outResult = {};
    ExternalizedInteractionEventsGuard interactionEventsGuard(
        gamePersistence,
        TraceContainsExternalizedInteractionEvents(lines, lineCount));

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    bool boardRotate = false;
    bool firstPlayerIsP2 = false;
    bool squareRotate = false;
    bool explicitBoardObservation = false;
    bool legacySensorSeeded = false;
    runtime::EventBus<16> bus;
    bus.Reset();

    for (size_t i = 0; i < lineCount; ++i) {
        const char* line = lines[i];
        if (!line || std::strlen(line) == 0) {
            continue;
        }

        trace::JsonlEventRecord record;
        if (!trace::TryParseJsonlEventRecord(line, record)) {
            SetFailure(outResult, static_cast<uint32_t>(i + 1), "missing_type");
            return false;
        }

        const uint32_t tMs = record.tMs;
        clock.SetNowMs(tMs);

        if (record.hasObservationSequence &&
            !bus.AdvanceSequencePast(record.observationSequence)) {
            SetFailure(outResult, static_cast<uint32_t>(i + 1),
                       "event_sequence_exhausted");
            return false;
        }

        if (record.type == trace::JsonlEventType::Header) {
            continue;
        }

        if (record.type == trace::JsonlEventType::Settings) {
            // Accept canonical settings keys.
            if (record.hasSettingsJson) {
                const trace::jsonl::ObjectView& j = record.settingsJson;
                if (gamePersistence) {
                    auto& settings = gamePersistence->GetSettings();
                    bool settingsChanged = false;
                    settingsChanged = replay_internal::ApplyU32Setting(settings, j, "BOARD_ROTATE") || settingsChanged;
                    settingsChanged = replay_internal::ApplyU32Setting(settings, j, "GAME_FIRST_PLAYER") || settingsChanged;
                    settingsChanged = replay_internal::ApplyU32Setting(settings, j, "VS_CPU_LEVEL") || settingsChanged;
                    settingsChanged = replay_internal::ApplyU32Setting(settings, j, "VS_CPU_PLAYER") || settingsChanged;
                    settingsChanged = replay_internal::ApplyU32Setting(settings, j, "TIMER") || settingsChanged;
                    settingsChanged = replay_internal::ApplyTakebackGraceSetting(settings, j) || settingsChanged;
                    if (settingsChanged) {
                        settings.NotifyDocChanged();
                    }
                }
                uint32_t v = 0;
                if (trace::jsonl::TryGetU32(j, "BOARD_ROTATE", v)) {
                    boardRotate = v != 0U;
                }

                // Intentionally ignore legacy/removed side-assignment keys here.
                (void)trace::jsonl::TryGetU32(j, "GAME_P1_PLAYS_BLACK", v);
                (void)trace::jsonl::TryGetU32(j, "p1_plays_black", v);

                if (trace::jsonl::TryGetU32(j, "GAME_FIRST_PLAYER", v)) {
                    firstPlayerIsP2 = (v == 2U);
                }
                squareRotate = modules::ComputeSquareRotate(
                    boardRotate, firstPlayerIsP2);
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::Tick) {
            (void)bus.Push(runtime::AppEvent::MakeTick(tMs));
            runtime::AppEvent evt;
            while (bus.TryPop(evt)) {
                DispatchReplayAppEvent(sm, gamePersistence, evt);
            }

            if (gamePersistence) {
                gamePersistence->Update();
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::Sensor) {
            if (!record.hasSquare) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "sensor_missing_square");
                return false;
            }
            if (!explicitBoardObservation && !legacySensorSeeded) {
                sm.ApplyPhysicalOccupancySnapshot(
                    sm.GetExpectedOccupancyMask());
                legacySensorSeeded = true;
            }

            const uint8_t squareRaw = record.square;
            const uint8_t square = runtime::MapSquareIndex(squareRaw, squareRotate);

            const bool occ = record.hasOcc ? record.occ : false;
            (void)bus.Push(runtime::AppEvent::MakeSensorDelta(tMs, square, occ));
            (void)bus.Push(runtime::AppEvent::MakeTick(tMs));
            runtime::AppEvent evt;
            while (bus.TryPop(evt)) {
                DispatchReplayAppEvent(sm, gamePersistence, evt);
            }

            if (gamePersistence) {
                gamePersistence->Update();
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::BoardObservation) {
            if (!record.hasChangedMask || !record.hasOccupiedMask) {
                SetFailure(outResult,
                           static_cast<uint32_t>(i + 1),
                           "board_observation_parse_error");
                return false;
            }
            explicitBoardObservation = true;

            const uint64_t changedMask = replay_internal::MapOccupancyMask(
                record.changedMask,
                squareRotate);
            const uint64_t occupiedMask = replay_internal::MapOccupancyMask(
                record.occupiedMask,
                squareRotate);
            runtime::AppEvent observation = runtime::AppEvent::MakeBoardObservation(
                tMs,
                changedMask,
                occupiedMask,
                record.deferredDuckDestinationSquare < 64U
                    ? runtime::MapSquareIndex(
                          record.deferredDuckDestinationSquare, squareRotate)
                    : 255U);
            if (record.hasObservationSequence) {
                observation.seq = record.observationSequence;
                DispatchReplayAppEvent(sm, gamePersistence, observation);
            } else {
                (void)bus.Push(observation);
            }
            (void)bus.Push(runtime::AppEvent::MakeTick(tMs));
            runtime::AppEvent evt;
            while (bus.TryPop(evt)) {
                DispatchReplayAppEvent(sm, gamePersistence, evt);
            }

            if (gamePersistence) {
                gamePersistence->Update();
            }
            continue;
        }

        if (trace::IsInteractionAdapterEvent(record.type)) {
            if (!trace::ReplayInteractionAdapterEvent(
                    record, sm, gamePersistence,
                    static_cast<uint32_t>(i + 1),
                    outResult)) {
                return false;
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::Button) {
            if (!record.hasButton || !record.hasButtonEvent) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "button_parse_error");
                return false;
            }

            const modules::UserButton b = record.button;
            const fxe::Button::Event e = record.buttonEvent;

            (void)bus.Push(runtime::AppEvent::MakeButton(tMs, b, e));
            (void)bus.Push(runtime::AppEvent::MakeTick(tMs));
            runtime::AppEvent evt;
            while (bus.TryPop(evt)) {
                DispatchReplayAppEvent(sm, gamePersistence, evt);
            }

            if (gamePersistence) {
                gamePersistence->Update();
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::Control) {
            if (!gamePersistence || record.control.empty()) {
                SetFailure(outResult,
                           static_cast<uint32_t>(i + 1),
                           "control_parse_error");
                return false;
            }

            if (record.control == "hint_toggle") {
                gamePersistence->RequestHintToggle();
                continue;
            }
            if (record.control == "undo") {
                if (!record.hasControlCount || record.controlCount == 0U ||
                    !gamePersistence->RequestUndo(record.controlCount)) {
                    SetFailure(outResult,
                               static_cast<uint32_t>(i + 1),
                               "control_undo_failed");
                    return false;
                }
                continue;
            }
            if (record.control == "reset") {
                if (!gamePersistence->ResetEngine()) {
                    SetFailure(outResult,
                               static_cast<uint32_t>(i + 1),
                               "control_reset_failed");
                    return false;
                }
                continue;
            }

            SetFailure(outResult,
                       static_cast<uint32_t>(i + 1),
                       "control_unknown");
            return false;
        }

        if (record.type == trace::JsonlEventType::CheckpointState) {
            if (record.gameState.empty()) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_state_missing");
                return false;
            }
            const char* actual = sm.DescribeInteractionState();
            if (record.gameState != std::string_view(actual)) {
                outResult.ok = false;
                outResult.failureLine = static_cast<uint32_t>(i + 1);
                std::snprintf(outResult.message,
                              sizeof(outResult.message),
                              "state_mismatch exp=%.*s act=%s ph=%s l=%u p=%u rf=%u occ=%llx",
                              static_cast<int>(record.gameState.size()),
                              record.gameState.data(),
                              actual,
                              runtime::ChessInteractionPhaseToString(
                                  sm.GetChessInteractionModel().phase.tag),
                              static_cast<unsigned>(sm.GetChessInteractionModel()
                                                        .phase.plan.progress
                                                        .relocationsLiftedMask),
                              static_cast<unsigned>(sm.GetChessInteractionModel()
                                                        .phase.plan.progress
                                                        .relocationsPlacedMask),
                              static_cast<unsigned>(sm.GetChessInteractionModel()
                                                        .phase.plan.relocations[1]
                                                        .from),
                              static_cast<unsigned long long>(
                                  sm.GetPhysicalOccupancyMask()));
                return false;
            }

            if (record.hasPlacement && (record.placement != sm.IsPlacementModeActive())) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_state_placement_mismatch");
                return false;
            }

            if (record.hasCpuThinking && (record.cpuThinking != sm.IsCpuThinkingActive())) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_state_cpu_mismatch");
                return false;
            }

            if (record.hasHumanIllegal && (record.humanIllegal != sm.HasHumanIllegal())) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_state_human_illegal_mismatch");
                return false;
            }

            if (record.hasCpuIllegal && (record.cpuIllegal != sm.HasCpuIllegal())) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_state_cpu_illegal_mismatch");
                return false;
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::CheckpointInteraction ||
            record.type == trace::JsonlEventType::CheckpointVariant) {
            if (!trace::ValidateInteractionCheckpoint(
                    record,
                    sm,
                    gamePersistence,
                    static_cast<uint32_t>(i + 1),
                    outResult)) {
                return false;
            }
            continue;
        }
        if (record.type == trace::JsonlEventType::CheckpointEngine) {
            if (!engine) {
                continue;
            }

            if (record.fen.empty()) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_engine_missing");
                return false;
            }
            char actualBuf[128] = {};
            if (!engine->GetFen(actualBuf, sizeof(actualBuf))) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_engine_unavailable");
                return false;
            }

            if (record.fen != std::string_view(actualBuf)) {
                outResult.ok = false;
                outResult.failureLine = static_cast<uint32_t>(i + 1);
                std::snprintf(outResult.message, sizeof(outResult.message),
                              "checkpoint_engine_mismatch got='%s' want='%.*s'",
                              actualBuf, static_cast<int>(record.fen.size()), record.fen.data());
                return false;
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::CheckpointFrame) {
            if (!record.hasFrameHash) {
                SetFailure(outResult, static_cast<uint32_t>(i + 1), "checkpoint_frame_missing_hash");
                return false;
            }

            const uint64_t want = record.frameHash;
            const replay_internal::FrameHashKind kind =
                (record.frameKind == trace::FrameHashKindToken::Board)
                                           ? replay_internal::FrameHashKind::Board
                                       : (record.frameKind == trace::FrameHashKindToken::Edge)
                                           ? replay_internal::FrameHashKind::Edge
                                           : replay_internal::FrameHashKind::Any;

            const modules::EngineDerivedState* derived =
                gamePersistence ? &gamePersistence->GetEngineDerivedState() : nullptr;

            const replay_internal::FrameHashes hashes =
                replay_internal::ComputeComposedFrameHashes(
                    sm, derived, gamePersistence, boardRotate,
                    firstPlayerIsP2, squareRotate);
            const uint64_t got =
                (kind == replay_internal::FrameHashKind::Board) ? hashes.board
                : (kind == replay_internal::FrameHashKind::Edge) ? hashes.edge
                                                                 : hashes.all;
            if (got != want) {
                outResult.ok = false;
                outResult.failureLine = static_cast<uint32_t>(i + 1);
                const unsigned long long gotU = static_cast<unsigned long long>(got);
                const unsigned long long wantU = static_cast<unsigned long long>(want);

                const char* kindName = "all";
                if (kind == replay_internal::FrameHashKind::Board) {
                    kindName = "board";
                } else if (kind == replay_internal::FrameHashKind::Edge) {
                    kindName = "edge";
                }
                std::snprintf(outResult.message, sizeof(outResult.message),
                              "checkpoint_frame_mismatch kind=%s got=0x%016llx want=0x%016llx",
                              kindName, gotU, wantU);
                return false;
            }
            continue;
        }

        if (record.type == trace::JsonlEventType::End) {
            continue;
        }

        if (record.type == trace::JsonlEventType::Cmd) {
            if (!gamePersistence) {
                continue;
            }
            if (record.cmd.empty()) {
                continue;
            }
            char cmdBuf[256] = {};
            trace::jsonl::UnescapeJsonString(record.cmd, cmdBuf, sizeof(cmdBuf));
            const char* cmdRaw = cmdBuf;
            const std::string_view cmd(cmdRaw);
            if (StartsWith(cmd, "engine fen")) {
                // Payload starts after the subcommand; allow any amount of spacing.
                const char* fen = SkipSpaces(cmdRaw + std::string_view("engine fen").size());
                fen = SkipSpaces(fen);
                if (!fen || fen[0] == '\0') {
                    SetFailure(outResult, static_cast<uint32_t>(i + 1), "engine_fen_missing");
                    return false;
                }
                if (!gamePersistence->LoadExternalFen(fen)) {
                    SetFailure(outResult, static_cast<uint32_t>(i + 1), "engine_fen_failed");
                    return false;
                }
                continue;
            }
            if (cmd == "engine reset") {
                if (!gamePersistence->ResetEngine()) {
                    SetFailure(outResult,
                               static_cast<uint32_t>(i + 1),
                               "engine_reset_failed");
                    return false;
                }
                continue;
            }
            if (cmd == "game undo") {
                if (!gamePersistence->RequestUndo(1U)) {
                    SetFailure(outResult,
                               static_cast<uint32_t>(i + 1),
                               "game_undo_failed");
                    return false;
                }
                continue;
            }
            if (cmd == "persist reload") {
                if (!ReloadPersistedReplayState(sm, *gamePersistence, outResult,
                                                static_cast<uint32_t>(i + 1))) {
                    return false;
                }
                continue;
            }

            continue;
        }

        // Unknown record types are currently ignored to keep replay tolerant while
        // the trace schema evolves.
    }

    return true;
}

} // namespace trace
