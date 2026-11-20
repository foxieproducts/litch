#include <cstdint>

#include "modules/hint_move_selection.hpp"

#include <litch_max/chess_notation.hpp>

namespace modules {
namespace {

bool IsCachedLegalMove(const uint64_t* legalDestByFromSquare,
                       const uint64_t computedFromSquaresMask64,
                       const uint8_t fromSquare,
                       const uint8_t toSquare) {
    if (legalDestByFromSquare == nullptr || fromSquare >= 64U || toSquare >= 64U) {
        return false;
    }
    if (((computedFromSquaresMask64 >> fromSquare) & 1ULL) == 0ULL) {
        return false;
    }
    return ((legalDestByFromSquare[fromSquare] >> toSquare) & 1ULL) != 0ULL;
}

bool TryFindFallback(const uint64_t* legalDestByFromSquare,
                     const uint64_t computedFromSquaresMask64,
                     HintMoveSelection& outSelection) {
    for (uint8_t fromSquare = 0U; fromSquare < 64U; ++fromSquare) {
        if (((computedFromSquaresMask64 >> fromSquare) & 1ULL) == 0ULL) {
            continue;
        }
        const uint64_t destinations = legalDestByFromSquare[fromSquare];
        for (uint8_t toSquare = 0U; toSquare < 64U; ++toSquare) {
            if (((destinations >> toSquare) & 1ULL) == 0ULL) {
                continue;
            }
            outSelection.fromSquare = fromSquare;
            outSelection.toSquare = toSquare;
            outSelection.source = HintMoveSelectionSource::LegalFallback;
            return true;
        }
    }
    return false;
}

} // namespace

bool TrySelectHintMove(const UciMove* engineBest,
                       const uint64_t* legalDestByFromSquare,
                       const uint64_t legalComputedFromSquaresMask64,
                       HintMoveSelection& outSelection) {
    outSelection = HintMoveSelection{};
    if (legalDestByFromSquare == nullptr) {
        return false;
    }

    litch_max::ParsedUciMove parsed;
    const bool parsedOk = engineBest != nullptr && litch_max::TryParseUciMove(engineBest->CStr(), parsed);
    const bool engineBestLegal = parsedOk &&
                                 IsCachedLegalMove(legalDestByFromSquare,
                                                   legalComputedFromSquaresMask64,
                                                   parsed.fromSquare,
                                                   parsed.toSquare);
    if (engineBestLegal) {
        outSelection.fromSquare = parsed.fromSquare;
        outSelection.toSquare = parsed.toSquare;
        outSelection.source = HintMoveSelectionSource::EngineBest;
        return true;
    }

    return TryFindFallback(legalDestByFromSquare,
                           legalComputedFromSquaresMask64,
                           outSelection);
}

} // namespace modules
