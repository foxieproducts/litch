#pragma once

#include <cstdint>

#include "modules/settings_store.hpp"
#include "trace/trace_jsonl_parser.hpp"

namespace trace::replay_internal {

inline bool ApplyU32Setting(modules::SettingsStore& settings,
                            const trace::jsonl::ObjectView& json,
                            const char* key) {
    uint32_t value = 0U;
    if (!trace::jsonl::TryGetU32(json, key, value)) {
        return false;
    }
    auto& doc = settings.Doc();
    const uint32_t current = doc[key] | UINT32_MAX;
    if (current == value) {
        return false;
    }
    doc[key] = value;
    return true;
}

inline bool ApplyTakebackGraceSetting(
    modules::SettingsStore& settings,
    const trace::jsonl::ObjectView& json) {
    uint32_t value = 0U;
    // Version-1 traces predate this key and therefore recorded no grace.
    (void)trace::jsonl::TryGetU32(json, "TAKEBACK_GRACE_SECONDS", value);
    auto& doc = settings.Doc();
    const uint32_t current = doc["TAKEBACK_GRACE_SECONDS"] | UINT32_MAX;
    if (current == value) {
        return false;
    }
    doc["TAKEBACK_GRACE_SECONDS"] = value;
    return true;
}

} // namespace trace::replay_internal
