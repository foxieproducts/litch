#pragma once

#include "runtime/chess_interaction_model.hpp"

namespace runtime::chess_interaction_internal {

void ClearPhase(TaggedInteractionState& phase);
RequiredPhysicalAction GetNextPlanAction(const PhysicalTransitionPlan& plan);
void EnterPlanCorrection(ChessInteractionModel& model,
                         uint64_t correctionMask);
void EnterPlanComplete(ChessInteractionModel& model);
void ReduceIdleObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event);
void ReducePieceLiftedObservation(ChessInteractionModel& model,
                                  const ChessInteractionEvent& event);
bool TryReduceSettlingProgressRegression(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event);
void ReplayIdleAfterConsumedChanges(ChessInteractionModel& model,
                                    const ChessInteractionEvent& event,
                                    uint64_t consumedMask);
void ReducePlanObservation(ChessInteractionModel& model,
                           const ChessInteractionEvent& event);
void UpdatePlanProgress(PhysicalTransitionPlan& plan,
                        uint64_t occupiedMask);
bool ReopenHumanPlanForCurrentOccupancy(ChessInteractionModel& model,
                                        uint64_t occupiedMask);

} // namespace runtime::chess_interaction_internal
