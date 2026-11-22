#pragma once

#include <cstdint>

namespace hardware::user_input_timing {

inline constexpr uint32_t BUTTON_DEBOUNCE_MS = 20U;
inline constexpr uint32_t DPAD_BEFORE_REPEAT_MS = 250U;
inline constexpr uint32_t DPAD_NAV_REPEAT_RATE_MS = 50U;
// 25 ms was visibly sluggish for held P1/P2 color adjustment on V3. A 19 ms
// interval is 31.6% more repeat events per second while preserving tap steps.
inline constexpr uint32_t DPAD_VALUE_REPEAT_RATE_MS = 19U;

} // namespace hardware::user_input_timing
