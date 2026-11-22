#pragma once

#include <cstdint>

#include "foxie_embedded/fixed_string.hpp"
#include "modules/game_variant.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace modules {

enum class HomeAssistancePresentation : uint8_t {
    Hidden = 0,
    Off,
    Pending,
    Thinking,
    HintReady,
    HintRefreshing,
    DuckCompound,
    DuckRelocation,
    LegalGuide,
};

// Foreground facts which are owned outside GameStateMachine. These affect only
// the exported presentation; they never rewrite Hint or Coaching policy.
struct HomeAssistanceRuntimeInputs {
    bool menuOverlayActive = false;
    bool quickSettingsOverlayActive = false;
    bool gameResetHoldActive = false;
    bool coachingForcedHintDisplay = false;
};

// Typed, engine-read-free Home projection. Endpoint fields are interpreted by
// presentation; LegalGuide may also carry a matching Hint in from/to.
// Other unused fields remain invalid/zero.
struct HomeAssistanceProjection {
    bool valid = false;
    bool hintRequested = false;
    bool manualAffordanceUseful = false;
    bool coachingForced = false;
    HomeAssistancePresentation presentation =
        HomeAssistancePresentation::Hidden;
    uint8_t fromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t toSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t duckFromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t duckToSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t legalSourceSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint64_t legalDestinationMask = 0ULL;

    constexpr bool operator==(
        const HomeAssistanceProjection& other) const {
        return valid == other.valid &&
               hintRequested == other.hintRequested &&
               manualAffordanceUseful == other.manualAffordanceUseful &&
               coachingForced == other.coachingForced &&
               presentation == other.presentation &&
               fromSquare == other.fromSquare &&
               toSquare == other.toSquare &&
               duckFromSquare == other.duckFromSquare &&
               duckToSquare == other.duckToSquare &&
               legalSourceSquare == other.legalSourceSquare &&
               legalDestinationMask == other.legalDestinationMask;
    }
};

using HomeAssistanceExtra = fxe::FixedString<32>;

bool TryFormatHomeAssistanceProjection(
    const HomeAssistanceProjection& assistance,
    GameVariant variant,
    HomeAssistanceExtra& outExtra);

} // namespace modules
