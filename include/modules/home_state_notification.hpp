#pragma once

#include <cstdint>

#include "foxie_embedded/fixed_string.hpp"
#include <litch_max/fen_constants.hpp>
#include "modules/cpu_thinking.hpp"
#include "modules/game_variant.hpp"
#include "modules/game_variant_state.hpp"
#include "modules/home_assistance_projection.hpp"

namespace modules {

inline constexpr uint8_t HOME_STATE_NOTIFICATION_VERSION = 1U;
inline constexpr size_t HOME_STATE_NOTIFICATION_CAPACITY = 320U;

enum class HomeStateAction : uint8_t {
    Idle = 0,
    Lift,
    Progress,
    Duck,
    Correction,
    Settle,
    Promote,
    Cpu,
};

// Additive v1 Home projection. Persistence failure is orthogonal to gameplay:
// the board may continue in volatile memory while the app communicates the
// durability limitation.
enum class PersistenceDegradation : uint8_t {
    None = 0,
    QuarantinedRecord,
    StorageIo,
};

enum class HomeClassicResult : uint8_t {
    Ongoing = 0U,
    Checkmate = 1U,
    Stalemate = 2U,
    Draw = 3U,
};

struct HomeStateNotification {
    uint32_t sequence = 0U;
    HomeStateAction action = HomeStateAction::Idle;
    const char* fen = nullptr;
    uint64_t occupiedMask = 0ULL;
    uint64_t duckLandingRemovalMask = 0ULL;
    GameVariant variant = GameVariant::Undecided;
    uint8_t duckSquare = INVALID_DUCK_SQUARE;
    VariantOutcome outcome{};
    GameWinner clockWinner = GameWinner::None;
    bool boardRotated = false;
    bool whiteOwnerIsP2 = false;
    bool placementMode = false;
    bool freshSetup = false;
    bool kingQueenSwapPromptActive = false;
    // Additive v1 authority for the one-time opening Vex obstruction journey.
    // Older apps ignore this top-level field and absence means false.
    bool vexMoveFirst = false;
    bool undoPending = false;
    bool coachingStateValid = false;
    uint8_t coachingMask = 0U;
    HomeAssistanceProjection assistance{};
    PersistenceDegradation persistenceDegradation =
        PersistenceDegradation::None;
    CpuThinkingPresentation cpuThinking{};
    // Existing c tuple: queued CPU move, outstanding human capture removal, or
    // pending human promotion (promote/corr, no outstanding capture removal).
    bool physicalMoveValid = false;
    uint8_t physicalFromSquare = 0U;
    uint8_t physicalToSquare = 0U;
    bool physicalCaptureSquareValid = false;
    uint8_t physicalCaptureSquare = 0U;
    // Active physical Duck plan: c = CPU fixed target, h = human selectable,
    // u = neutral fixed return. Zero omits the optional projection.
    char duckRelocationOwner = '\0';
    uint8_t duckRelocationFromSquare = INVALID_DUCK_SQUARE;
    uint8_t duckRelocationToSquare = INVALID_DUCK_SQUARE;
    bool whiteLastMoveValid = false;
    uint8_t whiteLastMoveFromSquare = 0U;
    uint8_t whiteLastMoveToSquare = 0U;
    bool blackLastMoveValid = false;
    uint8_t blackLastMoveFromSquare = 0U;
    uint8_t blackLastMoveToSquare = 0U;
    char lastMoveMostRecentSide = '\0';
    // Typed Home extension. The piece belongs to the most recent move.
    uint8_t confirmedPromotion = 0U; // 1–4: Queen, Rook, Bishop, Knight.
    bool promotionRevealActive = false;
    bool promotionAutomatic = false;
    uint8_t promotionSelection = 0U; // Same 1–4 mapping, for the pending move.
    bool clockStateValid = false;
    bool clockEnabled = false;
    bool clockRunning = false;
    bool clockPaused = false;
    char clockActiveSide = 'w';
    uint8_t clockTimerSetting = 0U;
    char clockSetupState = 'n';
    uint32_t clockWhiteRemainingSeconds = 0U;
    uint32_t clockBlackRemainingSeconds = 0U;
    HomeClassicResult classicResult = HomeClassicResult::Ongoing;
};

using HomeStateNotificationLine =
    fxe::FixedString<HOME_STATE_NOTIFICATION_CAPACITY>;

const char* HomeStateActionToToken(HomeStateAction action);

// Projects the Duck square that Home should treat as intentional. A valid
// opening candidate is owned immediately while the variant remains undecided,
// even though durable Duck activation still waits for its stability gate.
uint8_t HomeStateDuckSquareForPresentation(
    const GameVariantStateSnapshot& variant,
    uint8_t initialDuckCandidateSquare);

// Home state is an atomic external projection. While setup variant selection
// remains reversible, removing or replacing the tracked Duck briefly leaves
// occupancy and the durable variant on opposite sides of one transition, so
// that exact snapshot cannot escape. Coherent provisional placement and
// ordinary invalid extras remain visible.
bool ShouldPublishHomeStateNotification(
    const GameVariantStateSnapshot& variant,
    uint64_t physicalOccupancy,
    bool initialVariantSelectionReversible);

bool IsValidHomeStateNotification(const HomeStateNotification& snapshot);

bool TryFormatHomeStateNotification(
    const HomeStateNotification& snapshot,
    HomeStateNotificationLine& outLine);

class HomeStateNotificationPublisher final {
  public:
    bool TryPrepare(const HomeStateNotification& snapshot,
                    bool force,
                    HomeStateNotificationLine& outLine);

    [[nodiscard]] uint32_t Sequence() const { return m_sequence; }

  private:
    bool MatchesLast(const HomeStateNotification& snapshot) const;
    void Remember(const HomeStateNotification& snapshot);

    HomeStateNotification m_last{};
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> m_lastFen;
    uint32_t m_sequence = 0U;
    bool m_hasLast = false;
};

} // namespace modules
