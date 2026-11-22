#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/home_state_snapshot.hpp"
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
    uint32_t s = 1;

    explicit Rng(const uint32_t seed) : s(seed == 0 ? 1U : seed) {}

    uint32_t NextU32() {
        // xorshift32
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }

    uint32_t Uniform(const uint32_t n) {
        if (n == 0) {
            return 0;
        }
        return NextU32() % n;
    }

    bool OneIn(const uint32_t n) {
        if (n <= 1) {
            return true;
        }
        return Uniform(n) == 0;
    }

    bool Chance(const uint32_t num, const uint32_t den) {
        if (den == 0) {
            return false;
        }
        return Uniform(den) < num;
    }
};

uint32_t ReadEnvU32(const char* name, const uint32_t defaultValue) {
    if (name == nullptr) {
        return defaultValue;
    }

    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return defaultValue;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == v) {
        return defaultValue;
    }

    return static_cast<uint32_t>(parsed);
}

bool ReadEnvBool(const char* name, const bool defaultValue) {
    if (name == nullptr) {
        return defaultValue;
    }

    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return defaultValue;
    }

    if (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0 ||
        std::strcmp(v, "yes") == 0 || std::strcmp(v, "YES") == 0 || std::strcmp(v, "on") == 0 ||
        std::strcmp(v, "ON") == 0) {
        return true;
    }

    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 || std::strcmp(v, "FALSE") == 0 ||
        std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0 || std::strcmp(v, "off") == 0 ||
        std::strcmp(v, "OFF") == 0) {
        return false;
    }

    return defaultValue;
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
        if (!isPiece) {
            return false;
        }

        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>(rank * 8 + file);
        outOcc64 |= (1ULL << sq);
        ++file;
    }

    return true;
}

char FileCharFromSquare(const uint8_t sq) {
    return static_cast<char>('a' + (sq % 8U));
}

char RankCharFromSquare(const uint8_t sq) {
    const uint8_t rankFromTop = static_cast<uint8_t>(sq / 8U);
    return static_cast<char>('8' - rankFromTop);
}

void FormatSquare(const uint8_t sq, char out[3]) {
    if (sq >= 64) {
        out[0] = '?';
        out[1] = '?';
        out[2] = '\0';
        return;
    }
    out[0] = FileCharFromSquare(sq);
    out[1] = RankCharFromSquare(sq);
    out[2] = '\0';
}

bool TryGetFenPieceAtSquare(const char* fen, const uint8_t square, char& outPieceChar) {
    outPieceChar = '\0';
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }
    if (square >= 64) {
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
        if (!isPiece) {
            return false;
        }

        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>(rank * 8 + file);
        if (sq == square) {
            outPieceChar = c;
            return true;
        }

        ++file;
    }

    return false;
}

uint8_t FileFromSquare(const uint8_t sq) {
    return static_cast<uint8_t>(sq % 8U);
}

uint8_t RankFromTopFromSquare(const uint8_t sq) {
    return static_cast<uint8_t>(sq / 8U);
}

bool TryComputeCaptureSquareAndIsCapture(const char* fenBefore,
                                         const uint8_t fromSquare,
                                         const uint8_t toSquare,
                                         uint8_t& outCaptureSquare,
                                         bool& outIsCapture) {
    outCaptureSquare = toSquare;
    outIsCapture = false;

    if (fromSquare >= 64 || toSquare >= 64) {
        return false;
    }

    char fromPiece = '\0';
    if (!TryGetFenPieceAtSquare(fenBefore, fromSquare, fromPiece)) {
        return false;
    }

    char toPiece = '\0';
    const bool toOccupied = TryGetFenPieceAtSquare(fenBefore, toSquare, toPiece);
    if (toOccupied) {
        outCaptureSquare = toSquare;
        outIsCapture = true;
        return true;
    }

    const bool isWhitePawn = (fromPiece == 'P');
    const bool isBlackPawn = (fromPiece == 'p');
    if (!isWhitePawn && !isBlackPawn) {
        return true;
    }

    const int fromFile = static_cast<int>(FileFromSquare(fromSquare));
    const int fromRank = static_cast<int>(RankFromTopFromSquare(fromSquare));
    const int toFile = static_cast<int>(FileFromSquare(toSquare));
    const int toRank = static_cast<int>(RankFromTopFromSquare(toSquare));

    const int df = toFile - fromFile;
    const int dr = toRank - fromRank;

    const bool isDiagonal = (df == 1) || (df == -1);
    if (!isDiagonal) {
        return true;
    }

    const bool isWhiteEp = isWhitePawn && (dr == -1);
    const bool isBlackEp = isBlackPawn && (dr == 1);
    if (!isWhiteEp && !isBlackEp) {
        return true;
    }

    const uint8_t capSq = static_cast<uint8_t>(fromRank * 8 + toFile);
    if (capSq >= 64) {
        return false;
    }

    outCaptureSquare = capSq;
    outIsCapture = true;
    return true;
}

bool TryPickRandomSetBitSquare(Rng& rng, const uint64_t mask64, uint8_t& outSquare) {
    if (mask64 == 0) {
        return false;
    }

    uint8_t squares[64] = {};
    uint8_t count = 0;
    for (uint8_t i = 0; i < 64; ++i) {
        if (((mask64 >> i) & 1ULL) == 0ULL) {
            continue;
        }
        squares[count] = i;
        ++count;
    }
    if (count == 0) {
        return false;
    }

    outSquare = squares[rng.Uniform(count)];
    return true;
}

bool IsCastlingKingMoveFromFen(const char* fen,
                              const uint8_t from,
                              const uint8_t to) {
    // Square mapping: 0=a8, 56=a1.
    // White:
    //  - King-side:  e1(60)->g1(62)
    //  - Queen-side: e1(60)->c1(58)
    // Black:
    //  - King-side:  e8(4)->g8(6)
    //  - Queen-side: e8(4)->c8(2)
    char movingPiece = '\0';
    if (!TryGetFenPieceAtSquare(fen, from, movingPiece)) {
        return false;
    }
    if (movingPiece == 'K' && from == 60 && (to == 62 || to == 58)) {
        return true;
    }
    if (movingPiece == 'k' && from == 4 && (to == 6 || to == 2)) {
        return true;
    }
    return false;
}

bool TryGetCastlingRookMoveForKingMove(const uint8_t kingFrom,
                                       const uint8_t kingTo,
                                       uint8_t& outRookFrom,
                                       uint8_t& outRookTo) {
    // Square mapping: 0=a8, 56=a1.
    // White:
    //  - King-side:  e1(60)->g1(62), rook h1(63)->f1(61)
    //  - Queen-side: e1(60)->c1(58), rook a1(56)->d1(59)
    // Black:
    //  - King-side:  e8(4)->g8(6), rook h8(7)->f8(5)
    //  - Queen-side: e8(4)->c8(2), rook a8(0)->d8(3)
    struct Pattern {
        uint8_t kingFrom;
        uint8_t kingTo;
        uint8_t rFrom;
        uint8_t rTo;
    };
    static constexpr Pattern PAT[] = {
        Pattern{60, 62, 63, 61},
        Pattern{60, 58, 56, 59},
        Pattern{4, 6, 7, 5},
        Pattern{4, 2, 0, 3},
    };

    for (const auto& p : PAT) {
        if (kingFrom == p.kingFrom && kingTo == p.kingTo) {
            outRookFrom = p.rFrom;
            outRookTo = p.rTo;
            return true;
        }
    }

    return false;
}

uint8_t CastlingCoverageIndex(const uint8_t kingFrom,
                              const uint8_t kingTo) {
    if (kingFrom == 60U && kingTo == 62U) return 0U;
    if (kingFrom == 60U && kingTo == 58U) return 1U;
    if (kingFrom == 4U && kingTo == 6U) return 2U;
    if (kingFrom == 4U && kingTo == 2U) return 3U;
    return 255U;
}

struct SeedScenario {
    const char* name = "initial";
    const char* fen = nullptr;
    uint8_t forcedFrom = 0U;
    uint8_t forcedTo = 0U;
    uint8_t promotionChoice = 255U;
};

SeedScenario ScenarioForGame(const uint32_t game) {
    // Every 16-game block forces the highest-risk rule branches once before
    // continuing with generated play. The game index and base seed fully
    // reproduce both the fixture and all later random choices.
    static constexpr SeedScenario SCENARIOS[14] = {
        {"castle-white-kingside",
         "4k3/8/8/8/8/8/8/4K2R w K - 0 1", 60U, 62U, 255U},
        {"castle-white-queenside",
         "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1", 60U, 58U, 255U},
        {"castle-black-kingside",
         "4k2r/8/8/8/8/8/8/4K3 b k - 0 1", 4U, 6U, 255U},
        {"castle-black-queenside",
         "r3k3/8/8/8/8/8/8/4K3 b q - 0 1", 4U, 2U, 255U},
        {"en-passant-white",
         "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", 28U, 19U, 255U},
        {"en-passant-black",
         "4k3/8/8/8/3pP3/8/8/4K3 b - e3 0 1", 35U, 44U, 255U},
        {"promotion-white-queen",
         "7k/P7/8/8/8/8/8/7K w - - 0 1", 8U, 0U, 0U},
        {"promotion-white-rook",
         "7k/P7/8/8/8/8/8/7K w - - 0 1", 8U, 0U, 1U},
        {"promotion-white-bishop",
         "7k/P7/8/8/8/8/8/7K w - - 0 1", 8U, 0U, 2U},
        {"promotion-white-knight",
         "7k/P7/8/8/8/8/8/7K w - - 0 1", 8U, 0U, 3U},
        {"promotion-black-queen",
         "7k/8/8/8/8/8/p7/7K b - - 0 1", 48U, 56U, 0U},
        {"promotion-black-rook",
         "7k/8/8/8/8/8/p7/7K b - - 0 1", 48U, 56U, 1U},
        {"promotion-black-bishop",
         "7k/8/8/8/8/8/p7/7K b - - 0 1", 48U, 56U, 2U},
        {"promotion-black-knight",
         "7k/8/8/8/8/8/p7/7K b - - 0 1", 48U, 56U, 3U},
    };
    const uint8_t index = static_cast<uint8_t>(game % 16U);
    return index < 14U ? SCENARIOS[index] : SeedScenario{};
}

uint64_t FilterAmbiguousCastlingRookSubmoves(const uint8_t fromSquare, const uint64_t destMask64) {
    // The state machine may interpret rook moves that match the castling rook
    // path as a partial castling composite, deferring the engine commit until
    // the king move is also observed. For fuzzing engine/expected parity per
    // ply, avoid selecting these ambiguous rook-only submoves.
    struct Pattern {
        uint8_t rFrom;
        uint8_t rTo;
    };
    static constexpr Pattern PAT[] = {
        Pattern{63, 61},
        Pattern{56, 59},
        Pattern{7, 5},
        Pattern{0, 3},
    };

    uint64_t filtered = destMask64;
    for (const auto& p : PAT) {
        if (fromSquare == p.rFrom) {
            filtered &= ~(1ULL << p.rTo);
        }
    }
    return filtered;
}

bool TryPickRandomLegalMove(modules::IEngine& engine,
                            Rng& rng,
                            uint8_t& outFrom,
                            uint8_t& outTo,
                            modules::UciMove& outUci) {
    uint8_t fromSquares[64] = {};
    uint8_t fromCount = 0;

    for (uint8_t sq = 0; sq < 64; ++sq) {
        uint64_t destMask64 = 0;
        if (!engine.GetLegalDestinationsMask(sq, destMask64)) {
            // Engine may be busy (should not happen here), treat as no move.
            continue;
        }
        destMask64 = FilterAmbiguousCastlingRookSubmoves(sq, destMask64);
        if (destMask64 == 0) {
            continue;
        }
        fromSquares[fromCount] = sq;
        ++fromCount;
    }

    if (fromCount == 0) {
        return false;
    }

    outFrom = fromSquares[rng.Uniform(fromCount)];

    uint64_t destMask64 = 0;
    if (!engine.GetLegalDestinationsMask(outFrom, destMask64) || destMask64 == 0) {
        return false;
    }

    destMask64 = FilterAmbiguousCastlingRookSubmoves(outFrom, destMask64);
    if (destMask64 == 0) {
        return false;
    }

    if (!TryPickRandomSetBitSquare(rng, destMask64, outTo)) {
        return false;
    }

    // UCI: a2a3. Promotion identity is selected later through the production
    // modal, so the generated physical move needs only its endpoints here.
    char buf[8] = {0};
    char fromSq[3] = {0};
    char toSq[3] = {0};
    FormatSquare(outFrom, fromSq);
    FormatSquare(outTo, toSq);
    std::snprintf(buf, sizeof(buf), "%s%s", fromSq, toSq);

    outUci.Clear();
    return outUci.Append(buf);
}

bool IsPromotionMoveFromFen(const char* fenBefore, const uint8_t from, const uint8_t to) {
    char fromPiece = '\0';
    if (!TryGetFenPieceAtSquare(fenBefore, from, fromPiece)) {
        return false;
    }

    const uint8_t toRank = static_cast<uint8_t>(to / 8U);
    const bool isWhitePawnPromo = (fromPiece == 'P') && (toRank == 0);
    const bool isBlackPawnPromo = (fromPiece == 'p') && (toRank == 7);
    return isWhitePawnPromo || isBlackPawnPromo;
}

struct Op {
    enum class Kind : uint8_t {
        Move = 0,
        Undo = 1,
        CancelUndo = 2,
        Reset = 3,
    };

    Kind kind = Kind::Move;
    uint8_t from = 0;
    uint8_t to = 0;
    uint8_t promotionChoice = 255U;
};

static constexpr uint16_t OP_LOG_CAP = 256;

struct OpLog {
    Op ops[OP_LOG_CAP] = {};
    uint16_t size = 0;

    void Push(const Op op) {
        if (size < OP_LOG_CAP) {
            ops[size] = op;
            ++size;
            return;
        }

        // Drop oldest.
        for (uint16_t i = 1; i < OP_LOG_CAP; ++i) {
            ops[i - 1] = ops[i];
        }
        ops[OP_LOG_CAP - 1] = op;
    }

    void Dump() const {
        for (uint16_t i = 0; i < size; ++i) {
            const Op& op = ops[i];
            if (op.kind == Op::Kind::Move) {
                char a[3] = {0};
                char b[3] = {0};
                FormatSquare(op.from, a);
                FormatSquare(op.to, b);
                static constexpr char PROMOTION_SUFFIXES[4] = {
                    'q', 'r', 'b', 'n'};
                const char suffix = op.promotionChoice < 4U
                                        ? PROMOTION_SUFFIXES[
                                              op.promotionChoice]
                                        : '-';
                std::printf("  op[%u]: move %s->%s promotion=%c\n",
                            static_cast<unsigned>(i), a, b, suffix);
            } else if (op.kind == Op::Kind::Undo) {
                std::printf("  op[%u]: undo\n", static_cast<unsigned>(i));
            } else if (op.kind == Op::Kind::CancelUndo) {
                std::printf("  op[%u]: cancel_undo\n", static_cast<unsigned>(i));
            } else if (op.kind == Op::Kind::Reset) {
                std::printf("  op[%u]: reset\n", static_cast<unsigned>(i));
            }
        }
    }
};

void ApplySensorDelta(runtime::GameStateMachine& sm,
                      uint64_t& physicalModel,
                      const uint8_t square,
                      const bool nowOccupied) {
    if (square < 64) {
        const uint64_t mask = (1ULL << square);
        if (nowOccupied) {
            physicalModel |= mask;
        } else {
            physicalModel &= ~mask;
        }
    }
    sm.HandleSensorChange(square, nowOccupied);
}

void Step(runtime::GameStateMachine& sm,
          runtime::ManualClock& clock,
          modules::GamePersistenceCoordinator& coord,
          const uint32_t advanceMs = 1) {
    clock.AdvanceMs(advanceMs);
    sm.Tick();
    coord.Update();
}

void ApplyPhysicalSnapshot(runtime::GameStateMachine& sm,
                           runtime::ManualClock& clock,
                           modules::GamePersistenceCoordinator& coord,
                           uint64_t& physicalModel,
                           const uint64_t targetOcc64) {
    sm.ApplyPhysicalOccupancySnapshot(targetOcc64);
    physicalModel = targetOcc64;

    // ApplyPhysicalOccupancySnapshot bypasses HandleSensorChange, so placement
    // mode won't auto-exit via UpdatePlacementMode(). If we have resolved the
    // mismatch, exit explicitly so moves are treated as moves.
    if (sm.IsPlacementModeActive() && sm.IsExpectedOccupancyMatched()) {
        sm.ExitPlacementMode();
    }
    Step(sm, clock, coord, 1);
}

void DrivePhysicalToMask(runtime::GameStateMachine& sm,
                         runtime::ManualClock& clock,
                         modules::GamePersistenceCoordinator& coord,
                         uint64_t& physicalModel,
                         const uint64_t targetOcc64) {
    // If we are mid-move, don't apply snapshots: they bypass sensor handlers
    // and can leave the state machine stuck in PickedUp/Settling.
    if (!(sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle) && !sm.IsPlacementModeActive() && !sm.IsUndoPromptActive() &&
        !sm.IsPromotionActive()) {
        sm.ForceEnterPlacementMode();
    }

    // Only use per-square deltas in corrective-only modes. Outside these modes,
    // flipping squares is interpreted as real pickups/placements and can
    // fabricate bogus moves.
    const bool correctiveMode = sm.IsUndoPromptActive() || sm.IsPlacementModeActive() ||
                                sm.IsCpuThinkingActive() || !(sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle);
    if (!correctiveMode) {
        ApplyPhysicalSnapshot(sm, clock, coord, physicalModel, targetOcc64);
        return;
    }

    const uint64_t cur = physicalModel;
    const uint64_t toSet = targetOcc64 & ~cur;
    const uint64_t toClear = cur & ~targetOcc64;

    for (uint8_t sq = 0; sq < 64; ++sq) {
        const uint64_t mask = (1ULL << sq);
        if ((toClear & mask) != 0) {
            ApplySensorDelta(sm, physicalModel, sq, false);
        }
    }

    for (uint8_t sq = 0; sq < 64; ++sq) {
        const uint64_t mask = (1ULL << sq);
        if ((toSet & mask) != 0) {
            ApplySensorDelta(sm, physicalModel, sq, true);
        }
    }

    Step(sm, clock, coord, 1);
}

bool SyncAndAssertExpectedFromEngine(
    modules::IEngine& engine,
    runtime::GameStateMachine& sm,
    modules::GamePersistenceCoordinator& coordinator,
    const modules::SettingsStore& settings,
    const uint64_t physicalModel,
    const char* failTag) {
    char fenBuf[litch_max::FEN_BUFFER_CAPACITY] = {};
    if (!engine.GetFen(fenBuf, sizeof(fenBuf))) {
        std::printf("[FAIL] %s: engine GetFen failed\n", failTag);
        return false;
    }

    uint64_t engineOcc64 = 0;
    if (!FenToOcc64(fenBuf, engineOcc64)) {
        std::printf("[FAIL] %s: FenToOcc64 failed\n", failTag);
        return false;
    }

    if (engineOcc64 != sm.GetExpectedOccupancyMask()) {
        std::printf("[FAIL] %s: expectedOcc mismatch (engine vs SM)\n", failTag);
        std::printf("  engineOcc=0x%016llx smExpected=0x%016llx\n",
                    static_cast<unsigned long long>(engineOcc64),
                    static_cast<unsigned long long>(sm.GetExpectedOccupancyMask()));
        std::printf("  fen=%s\n", fenBuf);
        return false;
    }

    if (std::strcmp(fenBuf, coordinator.GetAuthoritativeFen()) != 0) {
        std::printf("[FAIL] %s: coordinator FEN differs from engine FEN\n",
                    failTag);
        std::printf("  engine=%s\n  coordinator=%s\n",
                    fenBuf, coordinator.GetAuthoritativeFen());
        return false;
    }

    if (sm.GetPhysicalOccupancyMask() != physicalModel) {
        std::printf("[FAIL] %s: physical model differs from reducer observation\n",
                    failTag);
        std::printf("  model=0x%016llx reducer=0x%016llx\n",
                    static_cast<unsigned long long>(physicalModel),
                    static_cast<unsigned long long>(
                        sm.GetPhysicalOccupancyMask()));
        return false;
    }

    const modules::BeginnerModeController beginnerMode;
    modules::HomeStateNotification home =
        modules::BuildHomeStateSnapshot(
            sm, coordinator, settings,
            beginnerMode,
            modules::PersistenceDegradation::None);
    home.sequence = 1U;
    modules::HomeStateNotificationLine line;
    // The engine normalizes the fullmove to 1. Home keeps the first five
    // fields and projects the committed line's move number (checked below).
    const std::string_view rawFen(fenBuf);
    const std::string_view homeFen(home.fen != nullptr ? home.fen : "");
    if (homeFen.substr(0U, homeFen.rfind(' ')) != rawFen.substr(0U, rawFen.rfind(' ')) ||
        home.occupiedMask != physicalModel ||
        !modules::TryFormatHomeStateNotification(home, line) ||
        std::strstr(line.CStr(), home.fen) == nullptr) {
        std::printf("[FAIL] %s: Home projection is not atomic with game authorities\n",
                    failTag);
        std::printf("  fen=%s physical=0x%016llx home=%s\n",
                    fenBuf,
                    static_cast<unsigned long long>(physicalModel),
                    line.CStr());
        return false;
    }

    if (coordinator.GetEngineDerivedState().sideToMove !=
        engine.GetSideToMove()) {
        std::printf("[FAIL] %s: derived side-to-move differs from engine\n",
                    failTag);
        return false;
    }

    return true;
}

constexpr uint16_t UNDO_LIMIT = modules::UNDO_HISTORY_MOVE_CAPACITY;
constexpr uint64_t OPENING_MASK = UINT64_C(0xFFFF00000000FFFF);

struct UndoCapacitySettingsFile {
    static constexpr const char* PATH = "build/test_fuzz_undo_capacity.json";

    UndoCapacitySettingsFile() { RemoveFiles(); }
    ~UndoCapacitySettingsFile() { RemoveFiles(); }

    static void RemoveFiles() {
        std::remove(PATH);
        std::remove("build/test_fuzz_undo_capacity.json.bak");
        std::remove("build/test_fuzz_undo_capacity.json.tmp");
    }
};

struct RecordedPly {
    uint8_t from = 64U;
    uint8_t to = 64U;
    uint64_t occupancy = OPENING_MASK;
    uint16_t fullmove = 1U;
    modules::FenString fen;
};

uint64_t SquareBit(const uint8_t square) { return UINT64_C(1) << square; }

bool ObserveCapacityBoard(runtime::VirtualLitchSession& session, const uint64_t occupancy) {
    // This fixture uses the default orientation and player ends.
    return session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == occupancy;
}

bool CapacityPositionIs(runtime::VirtualLitchSession& session, const RecordedPly& ply,
                        const uint16_t depth) {
    const auto home = session.HomeStateSnapshot();
    const char* move = home.fen != nullptr ? std::strrchr(home.fen, ' ') : nullptr;
    return move != nullptr && std::strtoul(move + 1, nullptr, 10) == ply.fullmove &&
           session.UndoHistoryMoveCount() == depth &&
           session.State().expected_occupied_mask == ply.occupancy &&
           session.State().physical_occupied_mask == ply.occupancy &&
           ply.fen.Equals(home.fen) && !home.undoPending &&
           session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
}

unsigned UndoLandingEnergy(const runtime::VirtualLitchSession& session, const uint8_t target) {
    if (target >= 64U) return 0U;
    const auto color = session.SemanticFrame().board[target][1U];
    return color.r + color.g + color.b + color.w;
}

void CapacityCheckpoint(const runtime::VirtualLitchSession& session, const char* stage,
                        const uint16_t ply, const char* next, const uint8_t target = 64U) {
    const auto& state = session.State();
    const auto home = session.HomeStateSnapshot();
    const auto display = session.DisplayText();
    char square[3];
    FormatSquare(target, square);
    std::printf("[UNDO_CAPACITY] stage=%s ply=%u depth=%u phase=%u action=%u pending=%u "
                "physical=%016llx expected=%016llx target=%s target_energy=%u "
                "display=\"%.*s\" fen=\"%s\" next=%s\n",
                stage, unsigned(ply), unsigned(session.UndoHistoryMoveCount()),
                unsigned(state.interaction_phase), unsigned(state.required_action),
                unsigned(home.undoPending), static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                square, UndoLandingEnergy(session, target), static_cast<int>(display.size()),
                display.empty() ? "" : display.data(), home.fen != nullptr ? home.fen : "", next);
}

bool UndoCapacityTap(runtime::VirtualLitchSession& session) {
    const auto bank = session.State().side_to_move_black != 0U ? VIRTUAL_LITCH_OWNER_P2
                                                            : VIRTUAL_LITCH_OWNER_P1;
    const auto button = runtime::VirtualLitchPhysicalButton::X;
    return session.SetButtonDown(bank, button, true) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK &&
           session.SetButtonDown(bank, button, false) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK &&
           session.Advance(1U) == VIRTUAL_LITCH_OK;
}

bool SubmitCapacityUndo(runtime::VirtualLitchSession& session, const uint16_t id) {
    virtual_litch_action_request request{};
    virtual_litch_default_action_request(&request);
    request.request_id = id;
    request.action = VIRTUAL_LITCH_CONTROL_UNDO;
    request.undo_move_count = 255U;
    modules::ControlMutationOutcome outcome;
    return session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
           outcome.status == modules::ControlMutationStatus::Accepted &&
           session.Advance(21U) == VIRTUAL_LITCH_OK &&
           session.TryTakeControlCompletion(outcome) && outcome.requestId == id &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           session.Advance(1U) == VIRTUAL_LITCH_OK;
}

bool PlayCapacityPly(runtime::VirtualLitchSession& session, const RecordedPly& before,
                     RecordedPly& after, const uint8_t from, const uint8_t to,
                     const uint16_t depth) {
    after.from = from;
    after.to = to;
    after.fullmove = before.fullmove + (session.State().side_to_move_black != 0U ? 1U : 0U);
    const uint64_t lifted = before.occupancy & ~SquareBit(from);
    after.occupancy = lifted | SquareBit(to);
    return ObserveCapacityBoard(session, lifted) && ObserveCapacityBoard(session, after.occupancy) &&
           session.Advance(2100U) == VIRTUAL_LITCH_OK &&
           after.fen.Assign(session.HomeStateSnapshot().fen) &&
           CapacityPositionIs(session, after, depth);
}

bool FinishCapacityInverse(runtime::VirtualLitchSession& session, const RecordedPly& current,
                           const RecordedPly& prior, const uint16_t depth, const bool sample) {
    using Action = runtime::RequiredPhysicalAction;
    if (!session.IsUndoActionPending() ||
        session.State().required_action != static_cast<uint8_t>(Action::LiftPrimary) ||
        UndoLandingEnergy(session, current.from) == 0U) {
        return false;
    }
    if (sample) CapacityCheckpoint(session, "inverse-ready", depth + 1U,
                                   "lift-mover-and-return-to-lit-square", current.from);
    return ObserveCapacityBoard(session, current.occupancy & ~SquareBit(current.to)) &&
           session.State().required_action == static_cast<uint8_t>(Action::PlacePrimary) &&
           ObserveCapacityBoard(session, prior.occupancy) &&
           session.Advance(21U) == VIRTUAL_LITCH_OK && CapacityPositionIs(session, prior, depth);
}

bool RunClassicUndoCapacityJourney() {
    const UndoCapacitySettingsFile files;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config, files.PATH);
    auto& session = fixture.session;
    uint16_t settingId = 1U;
    for (const auto& setting : {std::pair{"VS_CPU_LEVEL", "0"}, std::pair{"TIMER", "0"},
                               std::pair{"BOARD_SETUP_ASSISTANCE", "false"},
                               std::pair{"WHITE_SQUARES_ON", "false"}}) {
        modules::ControlMutationOutcome outcome;
        if (session.MutateSettingJson(settingId++, setting.first, setting.second, outcome) != VIRTUAL_LITCH_OK ||
            (outcome.status != modules::ControlMutationStatus::Applied &&
             outcome.status != modules::ControlMutationStatus::Noop)) return false;
    }
    std::array<RecordedPly, UNDO_LIMIT + 3U> plies{};
    if (!ObserveCapacityBoard(session, OPENING_MASK) || session.Advance(1000U) != VIRTUAL_LITCH_OK ||
        !plies[0].fen.Assign(session.HomeStateSnapshot().fen) ||
        !CapacityPositionIs(session, plies[0], 0U)) return false;
    CapacityCheckpoint(session, "opening", 0U, "play-the-first-knight-move");

    // White b1-a3-b1-c3-b1 and Black b8-a6-c5-a6-b8 form an eight-ply
    // circuit with only two visits to any position. Separate each circuit
    // with one pawn advance per side to keep the actual game out of draw.
    constexpr uint8_t FROM[]{57U, 1U, 40U, 16U, 57U, 26U, 42U, 16U};
    constexpr uint8_t TO[]{40U, 16U, 57U, 26U, 42U, 16U, 57U, 1U};
    constexpr uint8_t WHITE_FILES[]{1U, 3U, 4U};
    constexpr uint8_t BLACK_FILES[]{5U, 6U, 7U};
    for (uint16_t index = 0U; index < UNDO_LIMIT; ++index) {
        const uint16_t block = index / 10U;
        const uint8_t step = index % 10U;
        uint8_t from = 0U, to = 0U;
        if (step < 8U) {
            from = FROM[step];
            to = TO[step];
        } else if (step == 8U) {
            from = 48U + WHITE_FILES[block / 4U] - 8U * (block % 4U);
            to = from - 8U;
        } else {
            from = 8U + BLACK_FILES[block / 4U] + 8U * (block % 4U);
            to = from + 8U;
        }
        if (!PlayCapacityPly(session, plies[index], plies[index + 1U], from, to, index + 1U)) {
            CapacityCheckpoint(session, "forward-move-failed", index + 1U, "inspect-accepted-move", to);
            return false;
        }
    }
    CapacityCheckpoint(session, "maximum-depth", UNDO_LIMIT, "save-and-reload");
    bool saved = false;
    session.RunControlIngress([](void* context, modules::SettingsStore& settings,
                                 modules::ControlMutationService&, modules::GamePersistenceCoordinator&) {
        *static_cast<bool*>(context) = settings.SaveNow();
    }, &saved);
    const auto& full = plies[UNDO_LIMIT];
    if (!saved || session.Reset() != VIRTUAL_LITCH_OK ||
        session.PersistenceDegradation() != modules::PersistenceDegradation::None ||
        !ObserveCapacityBoard(session, full.occupancy) || session.Advance(500U) != VIRTUAL_LITCH_OK ||
        !CapacityPositionIs(session, full, UNDO_LIMIT)) {
        CapacityCheckpoint(session, "reload-failed", UNDO_LIMIT, "inspect-durable-undo-history");
        return false;
    }
    CapacityCheckpoint(session, "reloaded-128", UNDO_LIMIT, "request-undo-then-cancel");
    if (!SubmitCapacityUndo(session, 10U) || !session.IsUndoActionPending() ||
        session.State().required_action != static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) ||
        !UndoCapacityTap(session) || session.Advance(30U) != VIRTUAL_LITCH_OK ||
        !CapacityPositionIs(session, full, UNDO_LIMIT)) {
        CapacityCheckpoint(session, "cancel-failed", UNDO_LIMIT, "inspect-cancel-undo");
        return false;
    }
    CapacityCheckpoint(session, "oversized-request-canceled", UNDO_LIMIT, "play-the-rollover-ply");

    // Evict one move of each color. Dropping Black must advance the base FEN.
    if (!PlayCapacityPly(session, full, plies[UNDO_LIMIT + 1U], 48U, 40U, UNDO_LIMIT) ||
        !PlayCapacityPly(session, plies[UNDO_LIMIT + 1U], plies.back(), 8U, 16U, UNDO_LIMIT)) {
        CapacityCheckpoint(session, "rollover-failed", UNDO_LIMIT + 2U, "inspect-the-rolled-position");
        return false;
    }
    CapacityCheckpoint(session, "rollover", UNDO_LIMIT + 2U, "undo-the-128-retained-plies");
    for (uint16_t ply = UNDO_LIMIT + 2U; ply > 2U; --ply) {
        const bool sample = ply == UNDO_LIMIT + 2U || ply == 3U;
        if (!UndoCapacityTap(session) ||
            !FinishCapacityInverse(session, plies[ply], plies[ply - 1U], ply - 3U, sample)) {
            CapacityCheckpoint(session, "inverse-failed", ply, "inspect-current-return", plies[ply].from);
            return false;
        }
    }
    if (!UndoCapacityTap(session) || !CapacityPositionIs(session, plies[2U], 0U)) {
        CapacityCheckpoint(session, "expired-history-replayed", 2U, "inspect-oldest-retained-position");
        return false;
    }
    CapacityCheckpoint(session, "128-inverses-exhausted", 2U, "choose-a-new-White-move");

    RecordedPly branch;
    if (!PlayCapacityPly(session, plies[2U], branch, 52U, 44U, 1U) ||
        !SubmitCapacityUndo(session, 11U) ||
        !FinishCapacityInverse(session, branch, plies[2U], 0U, false)) {
        CapacityCheckpoint(session, "rebranch-failed", 3U, "inspect-the-new-branch");
        return false;
    }
    CapacityCheckpoint(session, "rebranch-complete", 2U, "only-the-new-branch-was-undoable");
    std::printf("[UNDO_CAPACITY] max_depth=%u placed=%u physical_inverses=%u "
                "expired_inverse_rejected=1 typed_255_clamped_to_branch=1\n",
                unsigned(UNDO_LIMIT), unsigned(UNDO_LIMIT + 2U), unsigned(UNDO_LIMIT));
    return true;
}

} // namespace

int main() {
    std::puts("[FUZZ_UNDO_REDO_LONG] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    // Keep the fuzz output small so failures (and repro logs) are readable.
    // If you need the full state-machine logs, opt in via LITCH_FUZZ_SHOW_LOGS=1.
    const bool showLogs = ReadEnvBool("LITCH_FUZZ_SHOW_LOGS", false);
    logger::SetMinLevel(showLogs ? logger::Level::INFO : logger::Level::WARN);
    if (!showLogs) {
        logger::SetOutputLineCallback([](const char*) {});
    } else {
        logger::SetOutputLineCallback(nullptr);
    }

    Check(RunClassicUndoCapacityJourney(), "V3.3 raw X returns every retained ply after a saved maximum and rollover");
    if (g_failures != 0) return 1;

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_fuzz_undo_redo_long.json";
    settingsCfg.deferredSaveDelayMs = 1000000;

    (void)std::remove(settingsCfg.path);

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 1;
    smCfg.settleFlashMs = 1;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::ManualClock clock(1000);
    clock.SetNowMs(1000);

    modules::SettingsStore settings(settingsCfg);
    Check(settings.Load(), "settings load");

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Generate both sides' moves and exercise all four promotion choices.
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    settings.Doc()["PROMOTION_MODE"] = "choose";
    (void)settings.SaveNow();

    uint64_t physicalModel = sm.GetPhysicalOccupancyMask();

    // Long-run parameters: tune for ~3-5 min on a dev machine.
    static constexpr uint32_t DEFAULT_BASE_SEED = 0xC0FFEE42u;
    static constexpr uint32_t DEFAULT_GAME_COUNT = 2500;
    static constexpr uint16_t DEFAULT_MAX_PLIES_PER_GAME = 24;

    const uint32_t gameCount = ReadEnvU32("LITCH_FUZZ_GAMES", DEFAULT_GAME_COUNT);
    const uint32_t startGame = ReadEnvU32("LITCH_FUZZ_START_GAME", 0U);
    const uint32_t baseSeed = ReadEnvU32(
        "LITCH_FUZZ_SEED", DEFAULT_BASE_SEED);
    const uint16_t maxPliesPerGame =
        static_cast<uint16_t>(ReadEnvU32("LITCH_FUZZ_MAX_PLIES", DEFAULT_MAX_PLIES_PER_GAME));

    const uint32_t progressEveryGames = ReadEnvU32("LITCH_FUZZ_PROGRESS_GAMES", 50);

    std::printf(
        "[FUZZ_UNDO_REDO_LONG] config seed=%u start_game=%u games=%u max_plies=%u\n",
        static_cast<unsigned>(baseSeed),
        static_cast<unsigned>(startGame),
        static_cast<unsigned>(gameCount),
        static_cast<unsigned>(maxPliesPerGame));

    OpLog opLog;
    uint32_t currentGame = startGame;
    uint16_t currentPly = 0U;
    uint64_t moveCount = 0U;
    uint16_t maximumUndoDepth = 0U;
    uint64_t captureCount = 0U;
    uint64_t enPassantCount = 0U;
    uint64_t castleCount = 0U;
    uint64_t castleCounts[4] = {};
    uint64_t enPassantBySide[2] = {};
    uint64_t coveredUndoCount = 0U;
    uint64_t coveredCancelUndoCount = 0U;
    uint64_t promotionCounts[4] = {};
    uint64_t externalFenCount = 0U;
    uint64_t correctionRecoveryCount = 0U;
    const char* currentScenario = "initial";

    for (uint32_t offset = 0; offset < gameCount && g_failures == 0; ++offset) {
        const uint32_t game = startGame + offset;
        currentGame = game;
        if (progressEveryGames != 0 && (offset % progressEveryGames) == 0) {
            std::printf("[FUZZ_UNDO_REDO_LONG] progress game=%u/%u\n",
                        static_cast<unsigned>(game),
                        static_cast<unsigned>(startGame + gameCount));
            std::fflush(stdout);
        }

        Rng rng(baseSeed ^ (game * 0x9E3779B9u));
        const SeedScenario scenario = ScenarioForGame(game);
        currentScenario = scenario.name;

        coord.ResetEngine();
        coord.Update();
        opLog.Push(Op{Op::Kind::Reset, 0, 0});
        if (scenario.fen != nullptr) {
            if (!coord.LoadExternalFen(scenario.fen)) {
                std::printf("[FAIL] scenario load failed: %s\n",
                            scenario.name);
                ++g_failures;
                break;
            }
            ++externalFenCount;
        }

        // Bring physical occupancy to match the new expected position.
        const uint64_t target = sm.GetExpectedOccupancyMask();
        ApplyPhysicalSnapshot(sm, clock, coord, physicalModel, target);

        if (!SyncAndAssertExpectedFromEngine(
                engine, sm, coord, settings, physicalModel,
                "after reset")) {
            ++g_failures;
            break;
        }
        Check(sm.GetPhysicalOccupancyMask() == physicalModel, "physicalModel matches SM after reset");

        for (uint16_t ply = 0; ply < maxPliesPerGame && g_failures == 0; ++ply) {
            currentPly = ply;
            if (progressEveryGames == 0 && (ply % 8) == 0) {
                // In per-game progress mode, avoid spam; otherwise emit a small heartbeat.
                std::printf(".");
                std::fflush(stdout);
            }
            // Occasionally stress undo/cancel-undo/branching.
            if (coord.GetUndoHistoryMoveCount() != 0 && rng.Chance(1, 5)) {
                const bool requestMulti = rng.Chance(1, 3);
                const uint8_t undoCount = requestMulti ? static_cast<uint8_t>(1 + rng.Uniform(3)) : 1;

                const bool ok = coord.RequestUndo(requestMulti ? undoCount : 1U);
                if (ok) {
                    ++coveredUndoCount;
                    opLog.Push(Op{Op::Kind::Undo, 0, 0});
                    coord.Update();

                    // Satisfy undo prompt(s) by syncing physical to expected.
                    // Repeat in case chained undos fire.
                    uint8_t guard = 0;
                    while ((sm.IsUndoPromptActive() || sm.HasCommittedMovePending()) && guard < 8) {
                        const uint64_t want = sm.GetExpectedOccupancyMask();
                        DrivePhysicalToMask(sm, clock, coord, physicalModel, want);
                        ++guard;
                    }

                    if (!SyncAndAssertExpectedFromEngine(
                            engine, sm, coord, settings, physicalModel,
                            "after undo")) {
                        ++g_failures;
                        break;
                    }

                    if (coord.IsCancelUndoAvailable() && rng.OneIn(2)) {
                        // Cancel-undo uses the same button path.
                        const bool ok2 = coord.RequestUndo(1U);
                        if (ok2) {
                            ++coveredCancelUndoCount;
                            opLog.Push(Op{Op::Kind::CancelUndo, 0, 0});
                            coord.Update();

                            if (!SyncAndAssertExpectedFromEngine(
                                    engine, sm, coord, settings,
                                    physicalModel, "after cancel-undo")) {
                                ++g_failures;
                                break;
                            }
                        }
                    } else if (rng.OneIn(2)) {
                        // Branch after undo: start a different move, which must invalidate cancel-undo.
                        // Even a single sensor delta should clear cancel-undo.
                        const uint64_t curOcc = physicalModel;
                        uint8_t anySq = 0;
                        if (TryPickRandomSetBitSquare(rng, curOcc, anySq)) {
                            ApplySensorDelta(sm, physicalModel, anySq, false);
                            ApplySensorDelta(sm, physicalModel, anySq, true);

                            Check(!coord.IsCancelUndoAvailable(), "completed undo keeps cancel-undo closed after physical delta");
                        }
                    }

                    // Ensure we're playable again.
                    DrivePhysicalToMask(sm, clock, coord, physicalModel, sm.GetExpectedOccupancyMask());
                    continue;
                }
            }

            // Exercise a reversible user-visible mistake without changing the
            // generated legal line. This validates that Home reports the real
            // physical extra while engine/coordinator authority stays fixed,
            // then that removing it returns to the exact prior game.
            if (coord.GetGameVariant() == modules::GameVariant::Classic &&
                coord.GetUndoHistoryMoveCount() != 0U &&
                sm.GetChessInteractionModel().phase.tag ==
                    runtime::ChessInteractionPhase::Idle &&
                sm.IsExpectedOccupancyMatched() && rng.Chance(1U, 17U)) {
                uint8_t extraSquare = 0U;
                if (TryPickRandomSetBitSquare(
                        rng, ~physicalModel, extraSquare)) {
                    char beforeFault[litch_max::FEN_BUFFER_CAPACITY] = {};
                    const uint8_t historyBeforeFault =
                        coord.GetUndoHistoryMoveCount();
                    Check(engine.GetFen(
                              beforeFault, sizeof(beforeFault)),
                          "correction fixture reads pre-fault FEN");
                    ApplySensorDelta(
                        sm, physicalModel, extraSquare, true);
                    Step(sm, clock, coord, 1U);
                    char duringFault[litch_max::FEN_BUFFER_CAPACITY] = {};
                    const bool faultVisible =
                        sm.HasHumanIllegal() || sm.IsPlacementModeActive() ||
                        sm.GetChessInteractionModel().phase.tag ==
                            runtime::ChessInteractionPhase::Correction;
                    Check(faultVisible &&
                              engine.GetFen(duringFault,
                                            sizeof(duringFault)) &&
                              std::strcmp(beforeFault, duringFault) == 0 &&
                              coord.GetUndoHistoryMoveCount() ==
                                  historyBeforeFault &&
                              SyncAndAssertExpectedFromEngine(
                                  engine, sm, coord, settings,
                                  physicalModel, "during correction"),
                          "unexpected extra is visible without mutating game authority");

                    ApplySensorDelta(
                        sm, physicalModel, extraSquare, false);
                    for (uint8_t settle = 0U; settle < 8U; ++settle) {
                        Step(sm, clock, coord, 1U);
                        if (sm.GetChessInteractionModel().phase.tag ==
                                runtime::ChessInteractionPhase::Idle &&
                            sm.IsExpectedOccupancyMatched() &&
                            !sm.HasHumanIllegal()) {
                            break;
                        }
                    }
                    Check(
                        sm.GetChessInteractionModel().phase.tag ==
                                runtime::ChessInteractionPhase::Idle &&
                            sm.IsExpectedOccupancyMatched() &&
                            !sm.HasHumanIllegal() &&
                            coord.GetUndoHistoryMoveCount() ==
                                historyBeforeFault &&
                            SyncAndAssertExpectedFromEngine(
                                engine, sm, coord, settings,
                                physicalModel, "after correction"),
                        "removing unexpected extra restores the exact generated game");
                    ++correctionRecoveryCount;
                    if (g_failures != 0) {
                        break;
                    }
                }
            }

            // Generate a random legal move from the engine.
            char fenBefore[litch_max::FEN_BUFFER_CAPACITY] = {};
            Check(engine.GetFen(fenBefore, sizeof(fenBefore)), "engine GetFen before move");

            uint8_t from = 0;
            uint8_t to = 0;
            modules::UciMove uci;
            const bool forcedScenarioMove =
                ply == 0U && scenario.fen != nullptr;
            if (forcedScenarioMove) {
                from = scenario.forcedFrom;
                to = scenario.forcedTo;
                uint64_t forcedDestinations = 0ULL;
                if (!engine.GetLegalDestinationsMask(
                        from, forcedDestinations) ||
                    (forcedDestinations & (UINT64_C(1) << to)) == 0ULL) {
                    std::printf(
                        "[FAIL] forced scenario move is not engine-legal: %s\n",
                        scenario.name);
                    ++g_failures;
                    break;
                }
            } else if (!TryPickRandomLegalMove(
                           engine, rng, from, to, uci)) {
                // No legal moves: game over.
                break;
            }

            const bool isPromotion =
                IsPromotionMoveFromFen(fenBefore, from, to);
            const bool isCastling =
                IsCastlingKingMoveFromFen(fenBefore, from, to);

            uint8_t capSq = to;
            bool isCapture = false;
            if (!isCastling) {
                (void)TryComputeCaptureSquareAndIsCapture(fenBefore, from, to, capSq, isCapture);
            }

            // Ensure we're in a sane, playable state before starting a move.
            if (!sm.IsExpectedOccupancyMatched()) {
                std::printf("[FAIL] pre-move: expected occupancy mismatch\n");
                std::printf("  smPhysical=0x%016llx smExpected=0x%016llx\n",
                            static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()),
                            static_cast<unsigned long long>(sm.GetExpectedOccupancyMask()));
                ++g_failures;
                break;
            }
            if (sm.GetPhysicalOccupancyMask() != physicalModel) {
                std::printf("[FAIL] pre-move: physical occupancy mismatch (reset/loss?)\n");
                std::printf("  physicalModel=0x%016llx smPhysical=0x%016llx\n",
                            static_cast<unsigned long long>(physicalModel),
                            static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()));
                ++g_failures;
                break;
            }
            if (!(sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle)) {
                if (sm.IsHumanPieceInHand()) {
                    uint8_t moveFrom = runtime::CHESS_INTERACTION_INVALID_SQUARE;
                    (void)sm.TryGetHumanPieceInHandSquare(moveFrom);
                    if (moveFrom < 64 && ((physicalModel >> moveFrom) & 1ULL) == 0ULL) {
                        ApplySensorDelta(sm, physicalModel, moveFrom, true);
                        Step(sm, clock, coord, 1);
                    }
                }

                sm.ForceEnterPlacementMode();
                DrivePhysicalToMask(sm, clock, coord, physicalModel, sm.GetExpectedOccupancyMask());

                // Give the state machine a moment to unwind back to Idle.
                for (uint8_t i = 0; i < 32; ++i) {
                    Step(sm, clock, coord, 1);
                    if ((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle) && sm.IsExpectedOccupancyMatched() &&
                        !sm.HasCommittedMovePending() && !sm.IsUndoPromptActive()) {
                        break;
                    }
                }

                if (!(sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle)) {
                    std::printf("[FAIL] pre-move: state not idle (state=%s)\n",
                                sm.DescribeInteractionState());
                    const runtime::PhysicalTransitionPlan& activePlan =
                        sm.GetChessInteractionModel().phase.plan;
                    std::printf("  moveFrom=%u moveTo=%u lastSensor=%u\n",
                                static_cast<unsigned>(activePlan.moveFrom),
                                static_cast<unsigned>(activePlan.moveTo),
                                static_cast<unsigned>(sm.GetLastSensorSquare()));
                    std::printf("  placement=%u undoPrompt=%u cancelUndo=%u committedPending=%u cpuThinking=%u\n",
                                static_cast<unsigned>(sm.IsPlacementModeActive()),
                                static_cast<unsigned>(sm.IsUndoPromptActive()),
                                static_cast<unsigned>(coord.IsCancelUndoAvailable()),
                                static_cast<unsigned>(sm.HasCommittedMovePending()),
                                static_cast<unsigned>(sm.IsCpuThinkingActive()));
                    ++g_failures;
                    break;
                }

                // Now that we recovered, retry this ply with a fresh move pick.
                continue;
            }

            const uint8_t historyBeforeGesture = coord.GetUndoHistoryMoveCount();
            const auto drawBeforeGesture = coord.GetEngineDerivedState().drawReason;
            bool promotionVictimStillOnTarget = false;
            if (isCastling) {
                uint8_t rookFrom = 0;
                uint8_t rookTo = 0;
                if (!TryGetCastlingRookMoveForKingMove(from, to, rookFrom, rookTo)) {
                    std::printf("[FAIL] castling: rook mapping failed\n");
                    ++g_failures;
                    break;
                }

                // Both supported sequences are king-first. Randomize whether
                // the rook is completed while the king remains lifted or after
                // the king has been placed.
                const bool rookWhileKingLifted = rng.OneIn(2);
                ApplySensorDelta(sm, physicalModel, from, false);
                Step(sm, clock, coord, 1);

                if (rookWhileKingLifted) {
                    ApplySensorDelta(sm, physicalModel, rookFrom, false);
                    Step(sm, clock, coord, 1);
                    ApplySensorDelta(sm, physicalModel, rookTo, true);
                    Step(sm, clock, coord, 1);
                }

                ApplySensorDelta(sm, physicalModel, to, true);
                Step(sm, clock, coord, 1);

                if (!rookWhileKingLifted) {
                    ApplySensorDelta(sm, physicalModel, rookFrom, false);
                    Step(sm, clock, coord, 1);
                    ApplySensorDelta(sm, physicalModel, rookTo, true);
                    Step(sm, clock, coord, 1);
                }
            } else {
                // Capture sequencing variants.
                const uint8_t mode = static_cast<uint8_t>(rng.Uniform(3));
                promotionVictimStillOnTarget =
                    isPromotion && isCapture && capSq == to && mode == 2U;

                if (isCapture) {
                    if (mode == 0) {
                        // target-first: remove captured piece first (idle).
                        ApplySensorDelta(sm, physicalModel, capSq, false);
                        Step(sm, clock, coord, 1);
                    }
                }

                // Lift mover.
                ApplySensorDelta(sm, physicalModel, from, false);
                Step(sm, clock, coord, 1);

                if (isCapture && mode == 1) {
                    // mover-first capture: remove captured piece while picked up.
                    ApplySensorDelta(sm, physicalModel, capSq, false);
                    Step(sm, clock, coord, 1);
                }

                // Place mover.
                ApplySensorDelta(sm, physicalModel, to, true);
                Step(sm, clock, coord, 1);

                // If en-passant capture, captured pawn is not on destination; ensure it is removed.
                if (isCapture && capSq != to) {
                    ApplySensorDelta(sm, physicalModel, capSq, false);
                    Step(sm, clock, coord, 1);
                }
            }

            // Give the state machine time to settle/finalize.
            for (uint8_t i = 0; i < 8; ++i) {
                Step(sm, clock, coord, 1);
                if ((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle) && sm.IsExpectedOccupancyMatched() &&
                    !sm.HasCommittedMovePending() && !sm.IsUndoPromptActive()) {
                    break;
                }
            }

            uint8_t promotionChoice = 255U;
            if (isPromotion && drawBeforeGesture == modules::DrawReason::None) {
                if (promotionVictimStillOnTarget) {
                    // The observed target stayed occupied. Only the pawn pickup
                    // is known until the captured piece is lifted and the pawn lands.
                    const auto& pickup = sm.GetChessInteractionModel().phase;
                    const uint64_t missing = sm.GetPhysicalOccupancyMask() ^ sm.GetExpectedOccupancyMask();
                    const bool onlyPawnLifted =
                        pickup.tag == runtime::ChessInteractionPhase::PieceLifted &&
                        pickup.selectedSquare == from && missing == (UINT64_C(1) << from);
                    char unchangedFen[litch_max::FEN_BUFFER_CAPACITY] = {};
                    if (!onlyPawnLifted || sm.IsPromotionActive() || sm.HasCommittedMovePending() ||
                        !engine.GetFen(unchangedFen, sizeof(unchangedFen)) ||
                        std::strcmp(unchangedFen, fenBefore) != 0 ||
                        std::strcmp(coord.GetAuthoritativeFen(), fenBefore) != 0 ||
                        coord.GetUndoHistoryMoveCount() != historyBeforeGesture) {
                        std::printf("[FAIL] promotion capture %s advanced before the victim was lifted\n", uci.CStr());
                        ++g_failures;
                        break;
                    }
                    ApplySensorDelta(sm, physicalModel, capSq, false);
                    Step(sm, clock, coord, 1U);
                    ApplySensorDelta(sm, physicalModel, to, true);
                    for (uint8_t i = 0U; i < 8U && !sm.IsPromotionActive(); ++i) {
                        Step(sm, clock, coord, 1U);
                    }
                }
                if (!sm.IsPromotionActive()) {
                    std::printf("[FAIL] promotion: physical last-rank move did not open selection\n");
                    ++g_failures;
                    break;
                }
                promotionChoice = forcedScenarioMove
                                      ? scenario.promotionChoice
                                      : static_cast<uint8_t>(
                                            rng.Uniform(4U));
                if (promotionChoice >= 4U) {
                    std::printf(
                        "[FAIL] promotion scenario has no valid choice: %s\n",
                        scenario.name);
                    ++g_failures;
                    break;
                }
                for (uint8_t step = 0U; step < promotionChoice; ++step) {
                    sm.HandleButtonEvent(
                        modules::UserButton::Right,
                        fxe::Button::Event::Press);
                }
                sm.HandleButtonEvent(
                    modules::UserButton::P1O,
                    fxe::Button::Event::Press);
                coord.Update();
            }

            // If the coordinator never committed the move to the engine, do not
            // carry forward a physical/expected mismatch into the next ply.
            // This can happen if we accidentally tripped a corrective-only
            // latch (e.g. capture-return), which blocks committing.
            char fenAfter[litch_max::FEN_BUFFER_CAPACITY] = {};
            Check(engine.GetFen(fenAfter, sizeof(fenAfter)), "engine GetFen after move");
            const bool positionUnchanged = std::strcmp(fenBefore, fenAfter) == 0;
            // A legal last-rank move in the FEN cannot reopen an adjudicated draw.
            if (isPromotion && drawBeforeGesture != modules::DrawReason::None &&
                (!positionUnchanged || std::strcmp(coord.GetAuthoritativeFen(), fenBefore) != 0 ||
                 sm.IsPromotionActive() || sm.HasCommittedMovePending() ||
                 coord.GetUndoHistoryMoveCount() != historyBeforeGesture ||
                 coord.GetEngineDerivedState().drawReason != drawBeforeGesture)) {
                std::printf("[FAIL] promotion attempt %s changed an adjudicated draw\n", uci.CStr());
                ++g_failures;
                break;
            }
            if (positionUnchanged) {
                sm.ForceEnterPlacementMode();
                DrivePhysicalToMask(sm, clock, coord, physicalModel, sm.GetExpectedOccupancyMask());
                continue;
            }

            opLog.Push(Op{Op::Kind::Move, from, to,
                          promotionChoice});
            ++moveCount;
            const uint16_t undoDepth = coord.GetUndoHistoryMoveCount();
            if (undoDepth > maximumUndoDepth) maximumUndoDepth = undoDepth;
            if (isCapture) {
                ++captureCount;
            }
            if (isCapture && capSq != to) {
                ++enPassantCount;
                char mover = '\0';
                if (TryGetFenPieceAtSquare(fenBefore, from, mover)) {
                    ++enPassantBySide[mover == 'p' ? 1U : 0U];
                }
            }
            if (isCastling) {
                ++castleCount;
                const uint8_t castleIndex =
                    CastlingCoverageIndex(from, to);
                if (castleIndex < 4U) {
                    ++castleCounts[castleIndex];
                }
            }
            if (promotionChoice < 4U) {
                ++promotionCounts[promotionChoice];
            }

            if (!SyncAndAssertExpectedFromEngine(
                    engine, sm, coord, settings, physicalModel,
                    "after move")) {
                ++g_failures;
                break;
            }

            // Invariant: we should never spontaneously lose/reset physical occupancy.
            if (sm.GetPhysicalOccupancyMask() != physicalModel) {
                std::printf("[FAIL] physical occupancy mismatch after move\n");
                std::printf("  physicalModel=0x%016llx smPhysical=0x%016llx\n",
                            static_cast<unsigned long long>(physicalModel),
                            static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()));
                ++g_failures;
                break;
            }

            // Keep runtime bounded: if the engine indicates game over, start a new game.
            const modules::SideToMove stm = engine.GetSideToMove();
            if (engine.IsInCheckmate(stm) || engine.IsInStalemate(stm)) {
                break;
            }
        }
    }

    if (g_failures != 0) {
        std::printf("[FUZZ_UNDO_REDO_LONG] failures=%d\n", g_failures);
        std::printf(
            "[FUZZ_UNDO_REDO_LONG] reproduce LITCH_FUZZ_SEED=%u "
            "LITCH_FUZZ_START_GAME=%u LITCH_FUZZ_GAMES=1 "
            "LITCH_FUZZ_MAX_PLIES=%u failure_ply=%u fen=%s "
            "physical=0x%016llx expected=0x%016llx phase=%s\n",
            static_cast<unsigned>(baseSeed),
            static_cast<unsigned>(currentGame),
            static_cast<unsigned>(maxPliesPerGame),
            static_cast<unsigned>(currentPly),
            coord.GetAuthoritativeFen(),
            static_cast<unsigned long long>(sm.GetPhysicalOccupancyMask()),
            static_cast<unsigned long long>(sm.GetExpectedOccupancyMask()),
            sm.DescribeInteractionState());
        std::printf("[FUZZ_UNDO_REDO_LONG] scenario=%s\n",
                    currentScenario);
        std::printf("[FUZZ_UNDO_REDO_LONG] recent operations:\n");
        opLog.Dump();
        return 1;
    }

    if (gameCount >= 16U) {
        Check(castleCounts[0] != 0U && castleCounts[1] != 0U &&
                  castleCounts[2] != 0U && castleCounts[3] != 0U,
              "adversarial block covers all four castles");
        Check(enPassantBySide[0] != 0U && enPassantBySide[1] != 0U,
              "adversarial block covers en-passant for both colors");
        Check(promotionCounts[0] != 0U && promotionCounts[1] != 0U &&
                  promotionCounts[2] != 0U && promotionCounts[3] != 0U,
              "adversarial block covers all four promotion choices");
        if (g_failures != 0) {
            std::printf(
                "[FUZZ_UNDO_REDO_LONG] required adversarial coverage missing\n");
            return 1;
        }
    }

    std::printf(
        "[FUZZ_UNDO_REDO_LONG] coverage moves=%llu captures=%llu "
        "en_passant=%llu castles=%llu undo=%llu cancel_undo=%llu "
        "promotion_q=%llu promotion_r=%llu promotion_b=%llu promotion_n=%llu "
        "external_fen=%llu castle_wk=%llu castle_wq=%llu castle_bk=%llu "
        "castle_bq=%llu ep_white=%llu ep_black=%llu corrections=%llu max_undo_depth=%u\n",
        static_cast<unsigned long long>(moveCount),
        static_cast<unsigned long long>(captureCount),
        static_cast<unsigned long long>(enPassantCount),
        static_cast<unsigned long long>(castleCount),
        static_cast<unsigned long long>(coveredUndoCount),
        static_cast<unsigned long long>(coveredCancelUndoCount),
        static_cast<unsigned long long>(promotionCounts[0]),
        static_cast<unsigned long long>(promotionCounts[1]),
        static_cast<unsigned long long>(promotionCounts[2]),
        static_cast<unsigned long long>(promotionCounts[3]),
        static_cast<unsigned long long>(externalFenCount),
        static_cast<unsigned long long>(castleCounts[0]),
        static_cast<unsigned long long>(castleCounts[1]),
        static_cast<unsigned long long>(castleCounts[2]),
        static_cast<unsigned long long>(castleCounts[3]),
        static_cast<unsigned long long>(enPassantBySide[0]),
        static_cast<unsigned long long>(enPassantBySide[1]),
        static_cast<unsigned long long>(correctionRecoveryCount),
        static_cast<unsigned>(maximumUndoDepth));
    std::puts("[FUZZ_UNDO_REDO_LONG] ok");
    return 0;
}
