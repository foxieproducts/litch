#include "bounded_hint_engine.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

struct Rng {
    uint32_t state = 1;

    explicit Rng(const uint32_t seed) : state(seed == 0U ? 1U : seed) {}

    uint32_t NextU32() {
        uint32_t value = state;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        state = value;
        return value;
    }

    uint32_t Uniform(const uint32_t limit) {
        return limit == 0U ? 0U : (NextU32() % limit);
    }

    bool Chance(const uint32_t numerator, const uint32_t denominator) {
        if (denominator == 0U) {
            return false;
        }
        return Uniform(denominator) < numerator;
    }
};

uint32_t ReadEnvU32(const char* name, const uint32_t defaultValue) {
    if (name == nullptr) {
        return defaultValue;
    }

    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end == value ? defaultValue : static_cast<uint32_t>(parsed);
}

bool FenToOcc64(const char* fen, uint64_t& outOcc64) {
    outOcc64 = 0;
    if (fen == nullptr) {
        return false;
    }

    int rank = 0;
    int file = 0;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        const char c = *p;
        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            file += static_cast<int>(c - '0');
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (!isPiece || rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        outOcc64 |= (1ULL << static_cast<uint8_t>(rank * 8 + file));
        ++file;
    }

    return true;
}

bool TryGetFenPieceAtSquare(const char* fen, const uint8_t square, char& outPiece) {
    outPiece = '\0';
    if (fen == nullptr || square >= 64U) {
        return false;
    }

    int rank = 0;
    int file = 0;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        const char c = *p;
        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }
        if (c >= '1' && c <= '8') {
            file += static_cast<int>(c - '0');
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (!isPiece || rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>(rank * 8 + file);
        if (sq == square) {
            outPiece = c;
            return true;
        }
        ++file;
    }

    return false;
}

char FileCharFromSquare(const uint8_t square) {
    return static_cast<char>('a' + (square % 8U));
}

char RankCharFromSquare(const uint8_t square) {
    return static_cast<char>('8' - (square / 8U));
}

void FormatSquare(const uint8_t square, char out[3]) {
    out[0] = FileCharFromSquare(square);
    out[1] = RankCharFromSquare(square);
    out[2] = '\0';
}

bool IsPromotionMoveFromFen(const char* fenBefore, const uint8_t from, const uint8_t to) {
    char fromPiece = '\0';
    if (!TryGetFenPieceAtSquare(fenBefore, from, fromPiece)) {
        return false;
    }

    const uint8_t toRank = static_cast<uint8_t>(to / 8U);
    return (fromPiece == 'P' && toRank == 0U) || (fromPiece == 'p' && toRank == 7U);
}

bool IsCastlingKingMove(const uint8_t from, const uint8_t to) {
    return (from == 60U && (to == 62U || to == 58U)) ||
           (from == 4U && (to == 6U || to == 2U));
}

bool TryGetCastlingRookMove(const uint8_t kingFrom,
                            const uint8_t kingTo,
                            uint8_t& outRookFrom,
                            uint8_t& outRookTo) {
    struct Pattern {
        uint8_t kingFrom;
        uint8_t kingTo;
        uint8_t rookFrom;
        uint8_t rookTo;
    };

    static constexpr Pattern kPatterns[] = {
        {60, 62, 63, 61},
        {60, 58, 56, 59},
        {4, 6, 7, 5},
        {4, 2, 0, 3},
    };

    for (const auto& pattern : kPatterns) {
        if (pattern.kingFrom == kingFrom && pattern.kingTo == kingTo) {
            outRookFrom = pattern.rookFrom;
            outRookTo = pattern.rookTo;
            return true;
        }
    }

    return false;
}

bool TryComputeCaptureSquareAndIsCapture(const char* fenBefore,
                                         const uint8_t fromSquare,
                                         const uint8_t toSquare,
                                         uint8_t& outCaptureSquare,
                                         bool& outIsCapture) {
    outCaptureSquare = toSquare;
    outIsCapture = false;

    if (fromSquare >= 64U || toSquare >= 64U) {
        return false;
    }

    char fromPiece = '\0';
    if (!TryGetFenPieceAtSquare(fenBefore, fromSquare, fromPiece)) {
        return false;
    }

    char toPiece = '\0';
    if (TryGetFenPieceAtSquare(fenBefore, toSquare, toPiece)) {
        outCaptureSquare = toSquare;
        outIsCapture = true;
        return true;
    }

    if (fromPiece != 'P' && fromPiece != 'p') {
        return true;
    }

    const int fromFile = static_cast<int>(fromSquare % 8U);
    const int fromRank = static_cast<int>(fromSquare / 8U);
    const int toFile = static_cast<int>(toSquare % 8U);
    const int toRank = static_cast<int>(toSquare / 8U);

    const int df = toFile - fromFile;
    const int dr = toRank - fromRank;
    if (df != 1 && df != -1) {
        return true;
    }

    const bool isWhiteEp = (fromPiece == 'P') && (dr == -1);
    const bool isBlackEp = (fromPiece == 'p') && (dr == 1);
    if (!isWhiteEp && !isBlackEp) {
        return true;
    }

    outCaptureSquare = static_cast<uint8_t>(fromRank * 8 + toFile);
    outIsCapture = true;
    return true;
}

uint64_t FilterAmbiguousCastlingRookSubmoves(const uint8_t fromSquare, const uint64_t destMask64) {
    struct Pattern {
        uint8_t rookFrom;
        uint8_t rookTo;
    };

    static constexpr Pattern kPatterns[] = {
        {63, 61},
        {56, 59},
        {7, 5},
        {0, 3},
    };

    uint64_t filteredMask64 = destMask64;
    for (const auto& pattern : kPatterns) {
        if (pattern.rookFrom == fromSquare) {
            filteredMask64 &= ~(1ULL << pattern.rookTo);
        }
    }
    return filteredMask64;
}

struct CandidateMove {
    uint8_t from = 0;
    uint8_t to = 0;
    uint8_t captureSquare = 0;
    bool isCapture = false;
    bool isCastling = false;
    bool isFifthRankPawnCapture = false;
};

bool TryPickRandomLegalMove(modules::IEngine& engine,
                            Rng& rng,
                            const char* fenBefore,
                            CandidateMove& outMove,
                            modules::UciMove& outUci) {
    CandidateMove moves[256] = {};
    uint16_t moveCount = 0;

    for (uint8_t from = 0; from < 64U; ++from) {
        uint64_t destMask64 = 0;
        if (!engine.GetLegalDestinationsMask(from, destMask64)) {
            continue;
        }

        destMask64 = FilterAmbiguousCastlingRookSubmoves(from, destMask64);
        for (uint8_t to = 0; to < 64U; ++to) {
            if (((destMask64 >> to) & 1ULL) == 0ULL) {
                continue;
            }

            if (IsPromotionMoveFromFen(fenBefore, from, to)) {
                continue;
            }

            if (moveCount >= 256U) {
                break;
            }

            CandidateMove& move = moves[moveCount++];
            move.from = from;
            move.to = to;
            move.isCastling = IsCastlingKingMove(from, to);
            move.captureSquare = to;
            move.isCapture = false;
            if (!move.isCastling) {
                (void)TryComputeCaptureSquareAndIsCapture(fenBefore,
                                                          from,
                                                          to,
                                                          move.captureSquare,
                                                          move.isCapture);
            }

            char fromPiece = '\0';
            (void)TryGetFenPieceAtSquare(fenBefore, from, fromPiece);
            const uint8_t fromRank = static_cast<uint8_t>(from / 8U);
            move.isFifthRankPawnCapture = move.isCapture &&
                                          ((fromPiece == 'P' && fromRank == 3U) ||
                                           (fromPiece == 'p' && fromRank == 4U));
        }
    }

    if (moveCount == 0U) {
        return false;
    }

    uint16_t preferred[256] = {};
    uint16_t preferredCount = 0;
    if (rng.Chance(1, 4)) {
        for (uint16_t i = 0; i < moveCount; ++i) {
            if (moves[i].isFifthRankPawnCapture) {
                preferred[preferredCount++] = i;
            }
        }
    }
    if (preferredCount == 0U && rng.Chance(1, 2)) {
        for (uint16_t i = 0; i < moveCount; ++i) {
            if (moves[i].isCapture) {
                preferred[preferredCount++] = i;
            }
        }
    }

    const uint16_t chosenIndex = (preferredCount != 0U)
                                     ? preferred[rng.Uniform(preferredCount)]
                                     : static_cast<uint16_t>(rng.Uniform(moveCount));
    outMove = moves[chosenIndex];

    char fromSq[3] = {0};
    char toSq[3] = {0};
    FormatSquare(outMove.from, fromSq);
    FormatSquare(outMove.to, toSq);

    char uciBuf[8] = {0};
    std::snprintf(uciBuf, sizeof(uciBuf), "%s%s", fromSq, toSq);
    outUci.Clear();
    return outUci.Append(uciBuf);
}

void AdvanceTime(runtime::ManualClock& clock, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
}

void PostSensor(runtime::GameStateMachine& sm,
                modules::GamePersistenceCoordinator& coord,
                const uint8_t square,
                const bool occupiedNow) {
    sm.HandleSensorChange(square, occupiedNow);
    coord.UpdateLegalDestinationsMask();
}

void TickSystem(runtime::GameStateMachine& sm,
                runtime::ManualClock& clock,
                modules::GamePersistenceCoordinator& coord,
                const uint32_t deltaMs) {
    AdvanceTime(clock, deltaMs);
    sm.Tick();
    coord.Update();
    coord.UpdateLegalDestinationsMask();
}

void ApplyPhysicalSnapshot(runtime::GameStateMachine& sm,
                           runtime::ManualClock& clock,
                           modules::GamePersistenceCoordinator& coord,
                           uint64_t& physicalModel,
                           const uint64_t targetOcc64) {
    sm.ApplyPhysicalOccupancySnapshot(targetOcc64);
    physicalModel = targetOcc64;
    if (sm.IsPlacementModeActive() && sm.IsExpectedOccupancyMatched()) {
        sm.ExitPlacementMode();
    }
    TickSystem(sm, clock, coord, 1);
}

bool SyncExpectedFromEngine(modules::IEngine& engine,
                            runtime::GameStateMachine& sm,
                            uint64_t& outEngineOcc64,
                            char* outFen,
                            const size_t outFenSize) {
    if (!engine.GetFen(outFen, outFenSize)) {
        return false;
    }
    return FenToOcc64(outFen, outEngineOcc64);
}

void PrintFailureContext(const uint32_t game,
                         const uint32_t ply,
                         const char* stage,
                         const char* fen,
                         const CandidateMove& move,
                         const runtime::GameStateMachine& sm) {
    char fromSq[3] = {0};
    char toSq[3] = {0};
    uint8_t activeSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    const bool activeSquareValid =
        sm.TryGetHumanPieceInHandSquare(activeSquare);
    FormatSquare(move.from, fromSq);
    FormatSquare(move.to, toSq);

    std::printf("  game=%u ply=%u stage=%s move=%s%s capture=%u castling=%u state=%s\n",
                static_cast<unsigned>(game),
                static_cast<unsigned>(ply),
                stage,
                fromSq,
                toSq,
                static_cast<unsigned>(move.isCapture),
                static_cast<unsigned>(move.isCastling),
                sm.DescribeInteractionState());
    std::printf("  humanIllegal=%u cpuIllegal=%u placement=%u validDest=0x%016llx\n",
                static_cast<unsigned>(sm.HasHumanIllegal()),
                static_cast<unsigned>(sm.HasCpuIllegal()),
                static_cast<unsigned>(sm.IsPlacementModeActive()),
                static_cast<unsigned long long>(sm.GetValidDestMask()));
    std::printf("  phase=%u selected=%u hand_square=%u hand_valid=%u\n",
                static_cast<unsigned>(sm.GetChessInteractionModel().phase.tag),
                static_cast<unsigned>(sm.GetChessInteractionModel().phase.selectedSquare),
                static_cast<unsigned>(activeSquare),
                static_cast<unsigned>(activeSquareValid));
    std::printf("  fen=%s\n", fen);
}

} // namespace

int main() {
    std::puts("[RANDOM_FALSE_INVALID_STATES] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    logger::SetMinLevel(logger::Level::WARN);
    logger::SetOutputLineCallback([](const char*) {});

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_random_false_invalid_states.json";
    settingsCfg.deferredSaveDelayMs = 1000000;

    (void)std::remove(settingsCfg.path);
    (void)std::remove("build/test_random_false_invalid_states.json.bak");
    (void)std::remove("build/test_random_false_invalid_states.json.tmp");

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 1;
    smCfg.settleFlashMs = 1;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::ManualClock clock(1000);
    fxe::ManualMillis::SetNowMs(clock.NowMs());

    modules::SettingsStore settings(settingsCfg);
    Check(settings.Load(), "settings load");

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    (void)settings.SaveNow();

    static constexpr uint32_t kBaseSeed = 0x5EED1234U;
    static constexpr uint32_t kDefaultGames = 500U;
    static constexpr uint32_t kDefaultMaxPlies = 8U;

    const uint32_t gameCount = ReadEnvU32("LITCH_INVALID_RANDOM_GAMES", kDefaultGames);
    const uint32_t maxPliesPerGame = ReadEnvU32("LITCH_INVALID_RANDOM_MAX_PLIES", kDefaultMaxPlies);

    uint64_t physicalModel = sm.GetPhysicalOccupancyMask();

    for (uint32_t game = 0; game < gameCount && g_failures == 0; ++game) {
        if ((game % 50U) == 0U) {
            std::printf("[RANDOM_FALSE_INVALID_STATES] progress game=%u/%u\n",
                        static_cast<unsigned>(game),
                        static_cast<unsigned>(gameCount));
            std::fflush(stdout);
        }

        Rng rng(kBaseSeed ^ (game * 0x9E3779B9U));

        coord.ResetEngine();
        coord.Update();
        ApplyPhysicalSnapshot(sm, clock, coord, physicalModel, sm.GetExpectedOccupancyMask());

        for (uint32_t ply = 0; ply < maxPliesPerGame && g_failures == 0; ++ply) {
            char fenBefore[litch_max::FEN_BUFFER_CAPACITY] = {0};
            Check(engine.GetFen(fenBefore, sizeof(fenBefore)), "GetFen before move");

            CandidateMove move;
            modules::UciMove uci;
            if (!TryPickRandomLegalMove(engine, rng, fenBefore, move, uci)) {
                break;
            }

            test_support::BoundedHintEngine oracle;
            oracle.Reset();
            Check(oracle.SetFen(fenBefore), "oracle SetFen before move");
            Check(oracle.ApplyUciMove(uci.CStr()), "oracle ApplyUciMove");

            if (move.isCastling) {
                uint8_t rookFrom = 0;
                uint8_t rookTo = 0;
                Check(TryGetCastlingRookMove(move.from, move.to, rookFrom, rookTo), "castling rook mapping");

                PostSensor(sm, coord, move.from, false);
                if (!sm.IsHumanPieceInHand() || sm.HasHumanIllegal() ||
                    ((sm.GetValidDestMask() >> move.to) & 1ULL) == 0ULL) {
                    Check(false, "false invalid on legal castling king pickup");
                    PrintFailureContext(game, ply, "castle-king-pickup", fenBefore, move, sm);
                    break;
                }
                physicalModel &= ~(1ULL << move.from);

                PostSensor(sm, coord, move.to, true);
                physicalModel |= (1ULL << move.to);

                for (uint8_t i = 0; i < 8U; ++i) {
                    TickSystem(sm, clock, coord, 1);
                    if (sm.IsCastlingRookPromptActive() || sm.IsHumanPieceInHand()) {
                        break;
                    }
                }

                PostSensor(sm, coord, rookFrom, false);
                if (!sm.IsHumanPieceInHand() || sm.HasHumanIllegal() ||
                    ((sm.GetValidDestMask() >> rookTo) & 1ULL) == 0ULL) {
                    Check(false, "false invalid on legal castling rook pickup");
                    PrintFailureContext(game, ply, "castle-rook-pickup", fenBefore, move, sm);
                    break;
                }
                physicalModel &= ~(1ULL << rookFrom);

                PostSensor(sm, coord, rookTo, true);
                physicalModel |= (1ULL << rookTo);
            } else {
                // Occupancy-only sensors cannot observe a captured piece being
                // replaced by the mover between scans because the destination
                // bit never changes. Exercise only observable target-first and
                // mover-first-then-target-removal choreography.
                const uint8_t captureMode =
                    move.isCapture ? static_cast<uint8_t>(rng.Uniform(2U))
                                   : 255U;

                if (move.isCapture && captureMode == 0U) {
                    PostSensor(sm, coord, move.captureSquare, false);
                    physicalModel &= ~(1ULL << move.captureSquare);
                    if (sm.HasHumanIllegal()) {
                        Check(false, "false invalid on legal target-first capture removal");
                        PrintFailureContext(game, ply, "target-first", fenBefore, move, sm);
                        break;
                    }
                }

                PostSensor(sm, coord, move.from, false);
                physicalModel &= ~(1ULL << move.from);
                if (!sm.IsHumanPieceInHand() || sm.HasHumanIllegal() ||
                    ((sm.GetValidDestMask() >> move.to) & 1ULL) == 0ULL) {
                    Check(false, "false invalid on legal pickup");
                    PrintFailureContext(game, ply, "pickup", fenBefore, move, sm);
                    break;
                }

                if (move.isCapture && captureMode == 1U) {
                    PostSensor(sm, coord, move.captureSquare, false);
                    physicalModel &= ~(1ULL << move.captureSquare);
                    if (sm.HasHumanIllegal()) {
                        Check(false, "false invalid on legal capture-first removal");
                        PrintFailureContext(game, ply, "capture-first", fenBefore, move, sm);
                        break;
                    }
                }

                PostSensor(sm, coord, move.to, true);
                physicalModel |= (1ULL << move.to);
                if (sm.HasHumanIllegal()) {
                    Check(false, "false invalid on legal placement");
                    PrintFailureContext(game, ply, "placement", fenBefore, move, sm);
                    break;
                }
            }

            for (uint8_t i = 0; i < 16U; ++i) {
                TickSystem(sm, clock, coord, 1);
                if ((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle) &&
                    sm.IsExpectedOccupancyMatched() &&
                    !sm.HasCommittedMovePending() &&
                    !sm.HasHumanIllegal() &&
                    !sm.HasCpuIllegal()) {
                    break;
                }
            }

            char fenAfter[litch_max::FEN_BUFFER_CAPACITY] = {0};
            char oracleFen[litch_max::FEN_BUFFER_CAPACITY] = {0};
            uint64_t engineOcc64 = 0;
            Check(SyncExpectedFromEngine(engine, sm, engineOcc64, fenAfter, sizeof(fenAfter)),
                  "sync expected from engine after move");
            Check(oracle.GetFen(oracleFen, sizeof(oracleFen)), "oracle GetFen after move");

            if (std::strcmp(fenAfter, oracleFen) != 0) {
                Check(false, "engine/oracle FEN mismatch after legal move");
                PrintFailureContext(game, ply, "fen-mismatch", fenBefore, move, sm);
                std::printf("  actual=%s\n", fenAfter);
                std::printf("  oracle=%s\n", oracleFen);
                break;
            }

            if (engineOcc64 != sm.GetExpectedOccupancyMask()) {
                Check(false, "expected occupancy mismatch after legal move");
                PrintFailureContext(game, ply, "expected-occ", fenBefore, move, sm);
                std::printf("  engineOcc=0x%016llx expected=0x%016llx\n",
                            static_cast<unsigned long long>(engineOcc64),
                            static_cast<unsigned long long>(sm.GetExpectedOccupancyMask()));
                break;
            }

            if (sm.GetPhysicalOccupancyMask() != physicalModel) {
                Check(false, "physical occupancy model mismatch after legal move");
                PrintFailureContext(game, ply, "physical-occ", fenBefore, move, sm);
                std::printf("  model=0x%016llx physical=0x%016llx\n",
                            static_cast<unsigned long long>(physicalModel),
                            static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()));
                break;
            }

            const modules::SideToMove stm = engine.GetSideToMove();
            if (engine.IsInCheckmate(stm) || engine.IsInStalemate(stm)) {
                break;
            }
        }
    }

    if (g_failures != 0) {
        std::printf("[RANDOM_FALSE_INVALID_STATES] failures=%d\n", g_failures);
        return 1;
    }

    std::puts("[RANDOM_FALSE_INVALID_STATES] ok");
    return 0;
}
