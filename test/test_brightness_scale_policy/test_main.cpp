#include <cstdio>
#include <initializer_list>

#include "modules/brightness_scale_policy.hpp"

namespace {
int failures = 0;
void Check(const bool value, const char* message) {
    if (!value) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    using namespace modules::brightness_scale_policy;
    std::puts("[BRIGHTNESS_SCALE_POLICY] start");
    Check(USER_MAX_EFFECTIVE_SCALE256 == 230U, "manual maximum is rounded 90 percent LED output");
    Check(ComputeEffectiveScale256(0U, false, 0.0f, 256U) == NEAR_DARK_SCALE256,
          "new level zero uses the near-dark nonzero envelope");
    Check(ComputeEffectiveScale256(0U, true, 1.0f, 256U) == NEAR_DARK_SCALE256,
          "explicit near-dark bypasses ambient uplift");
    Check(ComputeEffectiveScale256(9U, false, 0.0f, 256U) == 58U,
          "level above range clamps to level eight");
    Check(ComputeEffectiveScale256(1U, true, 0.0f, 256U) == 12U,
          "level one stays at the minimum visible scale in a dark room");
    Check(ComputeEffectiveScale256(1U, true, 1.0f, 256U) == 29U,
          "ambient bright endpoint reaches the chosen level ceiling");
    Check(ComputeEffectiveScale256(1U, false, 1.0f, 0U) == 0U,
          "fade may intentionally reach black");
    Check(ComputeEffectiveScale256(8U, false, 0.0f, 128U) == 29U,
          "fade uses shipped nearest rounding");
    for (uint32_t level = 0U; level <= 8U; ++level) {
        const auto manual = ComputeEffectiveScale256(level, true, 0.0f, 256U, false);
        Check(manual == ComputeEffectiveScale256(level, true, 1.0f, 256U, false),
              "auto off ignores full room-to-covered sensor change");
        Check(manual <= 230U && manual > 0U, "manual remains nonzero and below output ceiling");
        if (level > 0U) {
            Check(manual > ComputeEffectiveScale256(level - 1U, false, 0.0f, 256U, false),
                  "every manual level increases brightness");
        }
    }
    for (uint32_t level = 1U; level <= 8U; ++level) {
        const auto ceiling = ComputeEffectiveScale256(level, true, 1.0f, 256U, false);
        for (const float ambient : {-1.0f, 0.0f, 0.4f, 1.0f, 2.0f}) {
            Check(ComputeEffectiveScale256(level, true, ambient, 256U) <= ceiling,
                  "Auto stays within the same user ceiling across ambient input");
        }
    }
    Check(ComputeEffectiveScale256(8U, true, 0.0f, 256U, false) == 230U,
          "manual level eight reaches 90 percent without ambient uplift");
    Check(ComputeEffectiveScale256(8U, false, 0.0f, 256U, true) == 58U,
          "automatic startup and missing sensors use the dark-room base");
    Check(ComputeEffectiveScale256(0U, true, 1.0f, 256U, false) == 8U,
          "near-dark retains the required-guidance envelope; compositor dims ordinary pieces");
    std::printf("[BRIGHTNESS_SCALE_POLICY] %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
