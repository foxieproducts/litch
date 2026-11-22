#pragma once

#include <cstddef>
#include <cstring>

#include "modules/engine.hpp"
#include "modules/game_persistence.hpp"

namespace modules::game_persistence_engine_helpers {

inline bool SafeGetFen(IEngine& engine,
                       FenString& outFen,
                       const bool retryAfterCancelThinkOnFailure) {
    char buf[litch_max::FEN_BUFFER_CAPACITY] = {};
    if (!engine.GetFen(buf, sizeof(buf))) {
        if (!retryAfterCancelThinkOnFailure) {
            outFen.Clear();
            return false;
        }

        engine.CancelThink();
        std::memset(buf, 0, sizeof(buf));
        if (!engine.GetFen(buf, sizeof(buf))) {
            return false;
        }
    }

    (void)outFen.Assign(buf);
    return !outFen.Empty();
}

} // namespace modules::game_persistence_engine_helpers
