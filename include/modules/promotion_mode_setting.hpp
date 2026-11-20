#pragma once

#include <cstring>

namespace modules::promotion_mode_setting {
inline constexpr const char* KEY = "PROMOTION_MODE";
inline constexpr const char* DEFAULT_VALUE = "queen";
inline bool RequiresChoice(const char* value) {
    return value != nullptr && std::strcmp(value, "choose") == 0;
}
} // namespace modules::promotion_mode_setting
