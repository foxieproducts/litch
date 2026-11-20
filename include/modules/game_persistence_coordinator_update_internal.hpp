#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/engine.hpp"

namespace modules::game_persistence_update_internal {

uint8_t FileFromSquare(uint8_t sq);
uint8_t RankFromTopFromSquare(uint8_t sq);

bool TryGetFenPieceAtSquare(const char* fen, uint8_t square, char& outPieceChar);

bool TryComputeCpuCaptureSquare(const char* currentFen,
                                uint8_t fromSquare,
                                uint8_t toSquare,
                                uint8_t& outCaptureSquare);

bool TryComputeCpuCaptureSquareAndIsCapture(const char* currentFen,
                                            uint8_t fromSquare,
                                            uint8_t toSquare,
                                            uint8_t& outCaptureSquare,
                                            bool& outIsCapture);

uint8_t TryGetPromotionSuffixFromUci(const char* uci);

void BuildUndoPersistFieldsFromFen(const char* fenBefore,
                                   SideToMove sideToMove,
                                   uint8_t fromSquare,
                                   uint8_t toSquare,
                                   bool captureSquareValid,
                                   uint8_t captureSquare,
                                   uint8_t promotedPiece,
                                   uint8_t* outFields);

} // namespace modules::game_persistence_update_internal
