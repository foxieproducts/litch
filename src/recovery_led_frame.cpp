#include <array>
#include <cstddef>

#include "firmware/recovery_led_frame.hpp"

namespace firmware {

namespace {

constexpr modules::Rgb DIM_WHITE{30U, 30U, 24U};
constexpr modules::Rgb DIM_AMBER{48U, 18U, 0U};
constexpr modules::Rgb AMBER{92U, 34U, 0U};
constexpr modules::Rgb DIM_RED{56U, 0U, 0U};
constexpr modules::Rgb RED{112U, 0U, 0U};
constexpr modules::Rgb MAGENTA{70U, 0U, 54U};
constexpr modules::Rgb DIM_BLUE{0U, 18U, 54U};
constexpr modules::Rgb BLUE{0U, 34U, 104U};
constexpr modules::Rgb CYAN{0U, 76U, 84U};
constexpr modules::Rgb GREEN{0U, 76U, 16U};

constexpr std::array<uint8_t, 12U> PROGRESS_INDICES = {
    1U, 3U, 5U, 7U, 9U, 12U,
    17U, 19U, 21U, 23U, 25U, 28U,
};

constexpr std::array<uint8_t, 8U> RETRY_PROGRESS_INDICES = {
    0U, 2U, 4U, 6U, 16U, 18U, 20U, 22U,
};

void Set(RecoveryLedFrame& frame,
         const uint8_t index,
         const modules::Rgb color) {
    if (index < frame.size()) {
        frame[index] = color;
    }
}

void AddCausePattern(RecoveryLedFrame& frame,
                     const RecoveryCause cause,
                     const uint32_t nowMs) {
    const bool alternate = ((nowMs / 600U) & 1U) != 0U;
    switch (cause) {
        case RecoveryCause::None:
            Set(frame, 0U, DIM_WHITE);
            Set(frame, 16U, DIM_WHITE);
            break;

        case RecoveryCause::EarlyCrash:
            Set(frame, 0U, DIM_AMBER);
            Set(frame, 16U, DIM_AMBER);
            Set(frame, alternate ? 8U : 24U, AMBER);
            break;

        case RecoveryCause::PersistedStateCorrupt:
            Set(frame, 3U, MAGENTA);
            Set(frame, 11U, DIM_RED);
            Set(frame, 19U, MAGENTA);
            Set(frame, 27U, DIM_RED);
            break;

        case RecoveryCause::UpdatePending:
            Set(frame, 0U, DIM_BLUE);
            Set(frame, 7U, BLUE);
            Set(frame, 16U, DIM_BLUE);
            Set(frame, 23U, BLUE);
            break;

        case RecoveryCause::UpdateFailed:
            Set(frame, 4U, RED);
            Set(frame, 12U, DIM_BLUE);
            Set(frame, 20U, RED);
            Set(frame, 28U, DIM_BLUE);
            break;

        case RecoveryCause::ManualFactoryReset:
            Set(frame, 4U, AMBER);
            Set(frame, 5U, DIM_AMBER);
            Set(frame, 20U, AMBER);
            Set(frame, 21U, DIM_AMBER);
            break;
    }
}

template <size_t N>
void AddProgress(RecoveryLedFrame& frame,
                 const std::array<uint8_t, N>& indices,
                 const uint16_t progressPermille,
                 const modules::Rgb color) {
    const size_t litCount =
        (static_cast<size_t>(progressPermille) * N + 999U) / 1000U;
    for (size_t index = 0U; index < litCount && index < N; ++index) {
        Set(frame, indices[index], color);
    }
}

RecoveryLedFrame ComposeReadyFrame(const RecoverySnapshot& snapshot,
                                   const uint32_t nowMs) {
    RecoveryLedFrame frame{};
    AddCausePattern(frame, snapshot.cause, nowMs);
    return frame;
}

RecoveryLedFrame ComposeRetryFrame(const RecoverySnapshot& snapshot) {
    RecoveryLedFrame frame{};
    Set(frame, 8U, DIM_BLUE);
    Set(frame, 24U, DIM_BLUE);
    AddProgress(frame, RETRY_PROGRESS_INDICES,
                snapshot.totalProgressPermille, CYAN);
    return frame;
}

RecoveryLedFrame ComposeDestructiveFrame(
    const RecoverySnapshot& snapshot,
    const bool cancelWindow) {
    RecoveryLedFrame frame{};
    const modules::Rgb guard = cancelWindow ? CYAN : RED;
    Set(frame, 0U, guard);
    Set(frame, 16U, guard);
    if (cancelWindow) {
        Set(frame, 8U, DIM_WHITE);
        Set(frame, 24U, DIM_WHITE);
    }
    AddProgress(frame, PROGRESS_INDICES,
                snapshot.totalProgressPermille, AMBER);
    return frame;
}

} // namespace

RecoveryLedFrame ComposeRecoveryLedFrame(const RecoverySnapshot& snapshot,
                                         const uint32_t nowMs) {
    const uint32_t stateElapsedMs = nowMs - snapshot.stateEnteredMs;
    switch (snapshot.state) {
        case RecoveryState::Inactive:
            return RecoveryLedFrame{};

        case RecoveryState::WaitForRelease: {
            RecoveryLedFrame frame =
                ComposeReadyFrame(snapshot, stateElapsedMs);
            Set(frame, 15U, DIM_WHITE);
            Set(frame, 31U, DIM_WHITE);
            return frame;
        }

        case RecoveryState::Ready:
            return ComposeReadyFrame(snapshot, stateElapsedMs);

        case RecoveryState::RetryHold:
            return ComposeRetryFrame(snapshot);

        case RecoveryState::DestructiveHold:
            return ComposeDestructiveFrame(snapshot, false);

        case RecoveryState::CancelWindow:
            return ComposeDestructiveFrame(snapshot, true);

        case RecoveryState::Cancelled:
            return ComposeReadyFrame(snapshot, stateElapsedMs);

        case RecoveryState::ActionPending: {
            RecoveryLedFrame frame{};
            if (snapshot.pendingAction == RecoveryAction::FactoryReset) {
                Set(frame, 0U, RED);
                Set(frame, 8U, DIM_WHITE);
                Set(frame, 16U, RED);
                Set(frame, 24U, DIM_WHITE);
            } else {
                Set(frame, 0U, CYAN);
                Set(frame, 8U, GREEN);
                Set(frame, 16U, CYAN);
                Set(frame, 24U, GREEN);
            }
            return frame;
        }

        case RecoveryState::ActionFailed: {
            RecoveryLedFrame frame{};
            const modules::Rgb failure =
                ((stateElapsedMs / 500U) & 1U) != 0U ? RED : DIM_RED;
            Set(frame, 2U, failure);
            Set(frame, 6U, failure);
            Set(frame, 10U, failure);
            Set(frame, 14U, failure);
            Set(frame, 18U, failure);
            Set(frame, 22U, failure);
            Set(frame, 26U, failure);
            Set(frame, 30U, failure);
            return frame;
        }
    }

    return RecoveryLedFrame{};
}

hardware::PhysicalLedFrame ComposeFactoryResetControlFrame(
    const hardware::BoardRevision revision, const bool hasP2Controls,
    const modules::Rgb color) {
    hardware::PhysicalLedFrame frame{};

    // The dedicated controls and the older home-edge substitutes use the
    // same physical banks as GPIO. Saved gameplay orientation does not apply.
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay;
    for (uint8_t bank = 0U; bank < (hasP2Controls ? 2U : 1U); ++bank) {
        if (revision == hardware::BoardRevision::V3_3S3) {
            for (const auto role : {hardware::ControlLedRole::O,
                                    hardware::ControlLedRole::X}) {
                const uint8_t index = bank * 6U + static_cast<uint8_t>(role);
                overlay.controlPixels[index] = color;
                overlay.controlMask |= 1U << index;
            }
        } else {
            const auto owner = bank == 0U ? modules::UiOwner::P1
                                         : modules::UiOwner::P2;
            overlay.Set(modules::PlayerHomeLedIndex(owner, 2U), color);
            overlay.Set(modules::PlayerHomeLedIndex(owner, 5U), color);
        }
    }
    hardware::ApplyFixedPhysicalStatusOverlay(revision, overlay, frame);
    return frame;
}

} // namespace firmware
