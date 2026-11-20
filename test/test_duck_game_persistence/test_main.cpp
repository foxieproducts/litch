#include <cstring>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/duck_game_persistence.hpp"

namespace {

void TestVariantRoundTrips(fxe::test_support::TestResults& results) {
    modules::DuckGameDurableState undecided;
    char encoded[32] = {};
    results.Check(modules::EncodeDuckGameDurableState(
                      undecided, encoded, sizeof(encoded)),
                  "undecided state encodes");

    modules::DuckGameDurableState decoded;
    results.Check(modules::DecodeDuckGameDurableState(encoded, decoded) &&
                      decoded.variant == modules::GameVariant::Undecided &&
                      decoded.duckSquare == modules::INVALID_DUCK_SQUARE,
                  "undecided state round-trips");

    modules::DuckGameDurableState pending;
    pending.variant = modules::GameVariant::Duck;
    pending.duckSquare = 20U;
    pending.phase = modules::DuckTurnPhase::DuckRelocation;
    pending.pendingChessMoveValid = true;
    pending.pendingMoveFrom = 52U;
    pending.pendingMoveTo = 36U;
    pending.pendingPromotionSuffix = 'n';
    results.Check(modules::EncodeDuckGameDurableState(
                      pending, encoded, sizeof(encoded)) &&
                      modules::DecodeDuckGameDurableState(encoded, decoded) &&
                      decoded.variant == modules::GameVariant::Duck &&
                      decoded.duckSquare == 20U &&
                      decoded.phase == modules::DuckTurnPhase::DuckRelocation &&
                      decoded.pendingChessMoveValid &&
                      decoded.pendingMoveFrom == 52U &&
                      decoded.pendingMoveTo == 36U &&
                      decoded.pendingPromotionSuffix == 'n',
                  "pending combined turn round-trips");

    modules::DuckGameDurableState terminal;
    terminal.variant = modules::GameVariant::Duck;
    terminal.duckSquare = 31U;
    terminal.outcome = {
        modules::VariantOutcomeReason::KingCaptured,
        modules::GameWinner::White};
    results.Check(modules::EncodeDuckGameDurableState(
                      terminal, encoded, sizeof(encoded)) &&
                      modules::DecodeDuckGameDurableState(encoded, decoded) &&
                      decoded.outcome.IsTerminal(),
                  "terminal Duck outcome round-trips");
}

void TestVariantRejectsMalformedState(
    fxe::test_support::TestResults& results) {
    modules::DuckGameDurableState state;
    char encoded[32] = {};
    state.variant = modules::GameVariant::Duck;
    state.duckSquare = 18U;
    results.Check(modules::EncodeDuckGameDurableState(
                      state, encoded, sizeof(encoded)),
                  "baseline Duck state encodes");

    modules::DuckGameDurableState decoded;
    results.Check(!modules::DecodeDuckGameDurableState(
                      "01021200000000ffff", decoded),
                  "truncated state is rejected");
    encoded[0] = 'f';
    encoded[1] = 'f';
    results.Check(!modules::DecodeDuckGameDurableState(encoded, decoded),
                  "unknown durable version is rejected");

    state.phase = modules::DuckTurnPhase::DuckRelocation;
    results.Check(!modules::EncodeDuckGameDurableState(
                      state, encoded, sizeof(encoded)),
                  "relocation phase requires pending chess move");
    state.phase = modules::DuckTurnPhase::ChessMove;
    state.pendingPromotionSuffix = 'x';
    results.Check(!modules::EncodeDuckGameDurableState(
                      state, encoded, sizeof(encoded)),
                  "unknown promotion suffix is rejected");
    state.pendingPromotionSuffix = '\0';
    state.outcome.reason = modules::VariantOutcomeReason::Fowled;
    results.Check(!modules::EncodeDuckGameDurableState(
                      state, encoded, sizeof(encoded)),
                  "terminal reason requires winner");
}

void TestUndoHistoryCodec(fxe::test_support::TestResults& results) {
    const uint8_t from[] = {18U, modules::INVALID_DUCK_SQUARE, 44U};
    const uint8_t to[] = {27U, modules::INVALID_DUCK_SQUARE, 7U};
    char encoded[32] = {};
    results.Check(modules::EncodeDuckUndoHistory(
                      from, to, 3U, encoded, sizeof(encoded)),
                  "aligned Duck undo history encodes");

    uint8_t decodedFrom[3] = {};
    uint8_t decodedTo[3] = {};
    results.Check(modules::DecodeDuckUndoHistory(
                      encoded, decodedFrom, decodedTo, 3U, 3U) &&
                      std::memcmp(from, decodedFrom, sizeof(from)) == 0 &&
                      std::memcmp(to, decodedTo, sizeof(to)) == 0,
                  "aligned Duck undo history round-trips");
    results.Check(!modules::DecodeDuckUndoHistory(
                      encoded, decodedFrom, decodedTo, 3U, 2U),
                  "history length must match ordinary undo count");

    const uint8_t invalidFrom[] = {18U};
    const uint8_t invalidTo[] = {18U};
    results.Check(!modules::EncodeDuckUndoHistory(
                      invalidFrom, invalidTo, 1U,
                      encoded, sizeof(encoded)),
                  "same-square relocation is rejected");
    const uint8_t halfAbsentFrom[] = {modules::INVALID_DUCK_SQUARE};
    const uint8_t halfAbsentTo[] = {7U};
    results.Check(!modules::EncodeDuckUndoHistory(
                      halfAbsentFrom, halfAbsentTo, 1U,
                      encoded, sizeof(encoded)),
                  "half-absent relocation pair is rejected");
}

} // namespace

int main() {
    fxe::test_support::TestResults results("DUCK_GAME_PERSISTENCE");
    results.Start();
    TestVariantRoundTrips(results);
    TestVariantRejectsMalformedState(results);
    TestUndoHistoryCodec(results);
    return results.Finish();
}
