#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/engine.hpp"
#include <litch_max/chess_strings.hpp>

namespace modules {

using litch_max::TryGetFenPieceAtSquare;
const char* DescribeFenPieceType(char pieceChar);
const char* DescribeFenPieceSide(char pieceChar);
void FormatSquareOrUnknown(uint8_t square, char out[3]);
bool SafeGetFenToBuffer(IEngine& engine, char* outFen, size_t outSize);

} // namespace modules