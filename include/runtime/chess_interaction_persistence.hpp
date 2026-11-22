#pragma once

#include <cstddef>

#include "runtime/chess_interaction_model.hpp"

namespace runtime {

struct ChessInteractionDurableState {
    uint8_t version = 3U;
    TaggedInteractionState phase{};
    uint32_t nextActionToken = 1U;
};

bool HasDurableChessInteractionState(const ChessInteractionModel& model);
bool CanSafelyPersistChessInteractionState(const ChessInteractionModel& model);
bool ExportChessInteractionDurableState(const ChessInteractionModel& model,
                                        ChessInteractionDurableState& outState);
bool RestoreChessInteractionDurableState(ChessInteractionModel& model,
                                         const ChessInteractionDurableState& state);
bool EncodeChessInteractionDurableState(const ChessInteractionModel& model,
                                        char* out,
                                        size_t capacity);
bool DecodeChessInteractionDurableState(const char* encoded,
                                        ChessInteractionDurableState& outState);

} // namespace runtime
