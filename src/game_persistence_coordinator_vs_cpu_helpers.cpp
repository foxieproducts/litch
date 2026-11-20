#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"

#include "modules/chess_strings.hpp"

namespace modules::game_persistence_vs_cpu_internal {

bool IsParsedMoveLegalInCurrentPosition(IEngine& engine,
                                        const uint8_t fromSquare,
                                        const uint8_t toSquare) {
    uint64_t destMask64 = 0;
    const bool ok = engine.GetLegalDestinationsMask(fromSquare, destMask64);
    return ok && (((destMask64 >> toSquare) & 1ULL) != 0ULL);
}

bool TryChooseClassicCpuMove(IEngine& engine, const UciMove& searched, litch_max::ParsedUciMove& outParsed,
                             UciMove& outUci) {
    UciMove analyzed = searched;
    outParsed = litch_max::ParsedUciMove{};
    outUci.Clear();
    litch_max::ParsedUciMove parsed;
    if (!litch_max::TryParseUciMove(analyzed.CStr(), parsed) ||
        !IsParsedMoveLegalInCurrentPosition(engine, parsed.fromSquare, parsed.toSquare)) {
        ThinkCandidate seed;
        if (!engine.GetImmediateThinkCandidate(seed) || !seed.IsAvailable() ||
            !litch_max::TryFormatUciMove(seed.fromSquare, seed.toSquare, seed.promotion, analyzed) ||
            !litch_max::TryParseUciMove(analyzed.CStr(), parsed) ||
            !IsParsedMoveLegalInCurrentPosition(engine, parsed.fromSquare, parsed.toSquare)) return false;
    }
    outParsed = parsed;
    outUci = analyzed;
    return true;
}

} // namespace modules::game_persistence_vs_cpu_internal
