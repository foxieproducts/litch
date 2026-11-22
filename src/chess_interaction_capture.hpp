#pragma once

#include "runtime/chess_interaction_model.hpp"

namespace runtime::chess_interaction_capture {

bool IsKnownInvalidHumanCapture(const CaptureLegality& legality, const PhysicalTransitionPlan& plan);
bool IsEnPassantSource(const ChessPositionState& position, uint8_t from);
void RecordProgressDuringCorrection(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event);
bool ReduceIdleObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event);
bool ReducePieceLiftedObservation(ChessInteractionModel& model,
                                  const ChessInteractionEvent& event);
void ReduceCaptureStagedObservation(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event);

} // namespace runtime::chess_interaction_capture
