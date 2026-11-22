#pragma once

#include <cstddef>
#include <cstdint>

#include <litch_max/types.hpp>

namespace modules {

using litch_max::SideToMove;
using litch_max::UciMove;
using litch_max::ThinkProgressObserver;
using litch_max::ThinkCandidateSource;
using litch_max::ThinkRootKind;
using litch_max::ThinkScoreUnits;
using litch_max::ThinkProfile;
using litch_max::ThinkCandidate;
using litch_max::ThinkCandidateObserver;

enum class ThinkState : uint8_t {
    Idle = 0,
    Thinking = 1,
    Done = 2,
    Canceled = 3,
    Error = 4,
};

enum class ThinkRequestKind : uint8_t { Unavailable, Cpu, Hint, Benchmark, Ponder };
enum class ThinkStopReason : uint8_t { Unavailable, Deadline, NodeCap, Exhausted, Canceled, Error };

constexpr uint8_t THINK_OPENING_MAX_PLIES = 16U;
struct ThinkOpeningMove {
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    char promotion = '\0';
};
struct ThinkOpeningPrefix {
    ThinkOpeningMove moves[THINK_OPENING_MAX_PLIES]{};
    uint8_t moveCount = 0U;
    bool fromCanonicalStart = false;
};

struct ThinkConfig {
    // Difficulty is intentionally engine-defined. Higher means stronger.
    uint8_t difficulty = 0;

    // Optional limits; engines may ignore unsupported fields.
    uint32_t maxThinkMs = 0;
    uint32_t maxNodes = 0;
    uint32_t maxDepth = 0;

    // Determinism hook for engines that use randomness.
    uint32_t rngSeed = 0;

    // Immutable authoritative game metadata, copied before worker ownership.
    // UINT16_MAX means an unknown reversible clock; it cannot establish KPK wins.
    uint16_t reversibleHalfmoves = UINT16_MAX;
    ThinkOpeningPrefix openingPrefix{};
    // Exact-root completed work retained across speculative cancellation.
    ThinkCandidate completedRootFloor{};

    ThinkProgressObserver progressObserver{};
    ThinkCandidateObserver candidateObserver{};
    // Hint-only sustained analysis; ordinary opponent difficulty is unchanged.
    bool continuousAnalysis = false;
    // Actual calculation before the first scored Hint publication. Admission,
    // waiting after completion and visibility changes do not spend this budget.
    uint16_t minimumCalculationMs = 0U;
    // Enables a reply-aware preliminary Classic root. Its bounded allowance
    // ends when that root completes; later Full work obeys maxNodes.
    uint16_t firstRootNodeReserve = 0U;
};

// One bounded request snapshot. Absolute millis may be zero or wrap; each
// timestamp has explicit validity and elapsed intervals use unsigned subtraction.
// First accepted Hint and configured CPU level are coordinator-owned; worker fields are cached,
// never assembled by consulting an active engine from the Console or renderer.
struct ThinkDiagnostics {
    uint32_t requestId = 0U;
    uint32_t requestAtMs = 0U;
    uint32_t preflightAtMs = 0U;
    uint32_t startAtMs = 0U;
    uint32_t pollAtMs = 0U;
    uint32_t firstPublicationAtMs = 0U;
    uint32_t firstFullRootAtMs = 0U;
    uint32_t firstFull3RootAtMs = 0U; // Actual completed Full depth 3.
    uint32_t firstAcceptedHintAtMs = 0U;
    uint32_t finalAtMs = 0U;
    uint32_t quiescentAtMs = 0U;
    uint32_t publicationCount = 0U;
    uint32_t completedRootCount = 0U;
    uint32_t finalNodes = 0U;
    ThinkProfile requested{};
    ThinkProfile effective{};
    ThinkCandidate firstPublication{};
    ThinkCandidate latestPublication{};
    ThinkCandidate lastCompletedRoot{};
    ThinkRequestKind kind = ThinkRequestKind::Unavailable;
    ThinkState state = ThinkState::Idle;
    SideToMove side = SideToMove::White;
    uint8_t duckSquare = 255U;
    uint8_t fullDepth = 0U;
    uint8_t preliminaryDepth = 0U;
    uint8_t duckDepth = 0U;
    bool isDuck = false;
    bool preflightValid = false;
    bool startValid = false;
    bool pollValid = false;
    bool effectiveValid = false;
    bool rootProgressValid = false;
    bool firstPublicationValid = false;
    bool firstFullRootValid = false;
    bool firstFull3RootValid = false;
    bool firstAcceptedHintValid = false;
    bool finalValid = false;
    bool quiescentValid = false;
    bool finalNodesValid = false;
    bool active = false;
    bool retired = false;
    uint8_t configuredCpuLevel = 0U; // Zero is unavailable, not a Hint level.
    ThinkStopReason stopReason = ThinkStopReason::Unavailable;
};

struct ThinkStats {
    uint32_t nodeCount = 0;
    // Search work only; an approved early-result release hold is excluded.
    uint32_t calculationMs = 0U;
    bool calculationMsValid = false;
    ThinkStopReason stopReason = ThinkStopReason::Unavailable;
};

constexpr uint8_t CLASSIC_THINK_CANDIDATE_CAPACITY = 3U;

enum ClassicThinkBestFact : uint8_t {
    ClassicThinkOutcomeKnown = 1U << 0U,
    ClassicThinkGivesCheck = 1U << 1U,
    ClassicThinkGivesMate = 1U << 2U,
};

struct ClassicThinkCandidate {
    // Logical squares use the board-wide 0=a8 .. 63=h1 convention.
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    // Zero for an ordinary move; lowercase UCI promotion suffix otherwise.
    char promotion = '\0';
};

// Fixed-size summary of the last completed Classic analysis. Source and offline
// depth distinguish a retained Book result from completed live search. The first
// candidate is the move returned by PollThinkProgress; remaining candidates
// preserve engine order but do not claim comparable evaluations: alpha-beta
// searches may return bounds for non-best root moves. bestScore is valid only
// for the selected move from completed live/offline work and is an
// engine-relative signal, not user-facing evaluation text.
struct ClassicThinkResult {
    ClassicThinkCandidate candidates[CLASSIC_THINK_CANDIDATE_CAPACITY];
    int16_t bestScore = 0;
    uint8_t candidateCount = 0U;
    uint8_t bestMoveFacts = 0U;
    bool bestScoreValid = false;
    // Complete preliminary comparison; the Full search did not finish a root.
    bool preliminaryOnly = false;
    ThinkCandidateSource source = ThinkCandidateSource::Search;
    uint8_t offlineDepth = 0U;
};

struct DuckThinkResult {
    UciMove chessMove;
    uint8_t duckDestination = 255U;
    int32_t score = 0;
    bool capturedKing = false;
};

class IEngine {
  public:
    virtual ~IEngine() = default;

    // \brief Resets engine to the initial position.
    virtual void Reset() = 0;

    // \brief Sets position from FEN.
    virtual bool SetFen(const char* fen) = 0;

    // \brief Writes current FEN into a provided buffer.
    virtual bool GetFen(char* outFen, size_t outSize) const = 0;

    // \brief Applies a move in UCI notation (e.g. "e2e4", "e7e8q").
    virtual bool ApplyUciMove(const char* uciMove) = 0;

    virtual SideToMove GetSideToMove() const = 0;

    // \brief Queries for check/game-over conditions.
    //
    // Notes:
    // - These are expected to be pure queries of current engine state.
    // - For legal positions, only the side-to-move can be in check/mate.
    virtual bool IsInCheck(SideToMove side) const = 0;
    virtual bool IsInCheckmate(SideToMove side) const = 0;
    virtual bool IsInStalemate(SideToMove side) const = 0;

    // \brief Returns a bitmask of legal destination squares for a given origin.
    //
    // Squares are indexed 0..63 with 0 = a8 and 63 = h1.
    //
    // Notes:
    // - The result is computed for the current side-to-move only.
    // - Returns true for valid inputs even if there are 0 legal destinations.
    // - Returns false if fromSquare is invalid or if the engine is busy.
    virtual bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outDestMask64) = 0;

    // Quiescent-only, allocation-free ranked legal decision. A seed has no
    // completed search depth; it is available before worker/scratch admission.
    virtual bool GetImmediateThinkCandidate(ThinkCandidate& out) {
        out = ThinkCandidate{};
        return false;
    }

    // \brief Starts VS-CPU thinking for the current side-to-move.
    // \brief Returns false if thinking could not be started.
    virtual bool StartThink(const ThinkConfig& cfg) = 0;

    // Speculative next CPU root, captured while quiescent. Leaves the live game untouched.
    virtual bool StartPonder(const char*, const ThinkConfig&) { return false; }
    virtual bool PonderRootMatchesLive() const { return false; }

    // Fixed imported root, compute only. Implementations must leave live state untouched.
    // Cancel before worker dispatch must make the following Poll nonblocking.
    virtual bool StartBenchmark(uint8_t, const ThinkConfig&) { return false; }

    // \brief Starts a variant-aware search over a complete Duck turn.
    //
    // Engines without Duck support may retain the default false result.
    virtual bool StartDuckThink(const char* fen,
                                uint8_t duckSquare,
                                const ThinkConfig& cfg) {
        (void)fen;
        (void)duckSquare;
        (void)cfg;
        return false;
    }

    // \brief Requests canceling an in-progress think.
    virtual void CancelThink() = 0;

    // \brief Advances/captures progress for an async engine.
    // \brief If state == Done, outBestMoveUci must contain the chosen move.
    // \brief Progress is 0..100.
    // \brief Polls progress for an in-progress think.
    // \brief If state == Done, outBestMoveUci must contain the chosen move.
    // \brief Progress is 0..100.
    virtual ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                         UciMove& outBestMoveUci) = 0;

    // rief Reports the most recent think statistics (if available).
    virtual bool GetLastThinkStats(ThinkStats& outStats) const = 0;

    // \brief Returns a bounded summary from the most recent Classic search.
    // This is a coordinator-owned result read, not a thread-safe observation:
    // an AsyncThinkWorker caller must require IsDone() && !IsActive() first.
    // Engines without root-candidate support retain the default false result.
    virtual bool GetLastClassicThinkResult(ClassicThinkResult& outResult) const {
        outResult = ClassicThinkResult{};
        return false;
    }

    // \brief Returns the compound result from the most recent Duck search.
    virtual bool GetLastDuckThinkResult(DuckThinkResult& outResult) const {
        outResult = DuckThinkResult{};
        return false;
    }
};

} // namespace modules
