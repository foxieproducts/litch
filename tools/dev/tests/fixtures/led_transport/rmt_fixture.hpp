#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "hardware/rmt_flash_arbiter.hpp"

namespace fixture {
// SDK-only timing model. Actual renderer, buffer dirtiness, lease acquisition,
// start/completion checks and retry behavior are compiled from production.
// S3 has 48 RMT symbols/channel: each 24-symbol half needs a refill in 30 us.
enum class EventKind { Low, High, DataLow, Route, Submit };
struct Event { EventKind kind; unsigned pin; };
inline std::vector<Event> events;
inline unsigned criticalDepth = 0U;
inline unsigned schedulerDepth = 0U;
inline unsigned schedulerSuspensions = 0U;
inline uint64_t nowUs = 0U;
inline unsigned submissionDelayUs = 0U;
inline unsigned corruptions = 0U;
inline unsigned flashOverlaps = 0U;
inline unsigned starts = 0U;
inline bool forceTimeout = false;
inline bool timeoutAfterSubmission = false;
inline hardware::RmtFlashArbiter* arbiter = nullptr;
struct Channel {
    uint64_t endUs = 0U;
    unsigned maskedUs = 0U;
    bool corrupted = false;
    unsigned resetUs = 0U;
};
inline std::array<Channel, 2> channels{};

inline void Advance(unsigned us) {
    for (auto& channel : channels) {
        if (channel.endUs <= nowUs) continue;
        if (criticalDepth != 0U) {
            channel.maskedUs += us;
            if (channel.maskedUs >= 30U && !channel.corrupted) {
                channel.corrupted = true;
                ++corruptions;
            }
        } else {
            channel.maskedUs = 0U;
        }
        if (arbiter != nullptr &&
            arbiter->TryBeginFlash(arbiter->CompletedRenderGeneration())) {
            ++flashOverlaps;
            arbiter->EndFlash();
        }
    }
    nowUs += us;
}
}
