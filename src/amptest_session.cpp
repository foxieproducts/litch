#include "firmware/amptest_session.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>

namespace firmware::amptest {

namespace {

std::string_view TrimWhitespace(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(start, end - start);
}

bool EndsWithInsensitive(const std::string_view text, const std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }

    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(text[offset + i]);
        const unsigned char rhs = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

bool TryParseScaledUnsignedDecimal(const std::string_view text,
                                   const uint32_t unitScaleMilli,
                                   uint32_t& outMilliAmps) {
    const std::string_view trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        return false;
    }

    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint32_t fractionDigits = 0;
    bool seenDigit = false;
    bool seenDot = false;

    for (const char c : trimmed) {
        if (c == '.') {
            if (seenDot) {
                return false;
            }
            seenDot = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }

        seenDigit = true;
        const uint8_t digit = static_cast<uint8_t>(c - '0');
        if (!seenDot) {
            whole = (whole * 10ULL) + digit;
        } else if (fractionDigits < 4U) {
            fraction = (fraction * 10ULL) + digit;
            ++fractionDigits;
        }
    }

    if (!seenDigit) {
        return false;
    }

    uint64_t milliAmps = whole * unitScaleMilli;
    if (fractionDigits != 0U) {
        uint64_t divisor = 1ULL;
        for (uint32_t i = 0; i < fractionDigits; ++i) {
            divisor *= 10ULL;
        }
        milliAmps += ((fraction * unitScaleMilli) + (divisor / 2ULL)) / divisor;
    }

    outMilliAmps = static_cast<uint32_t>(milliAmps);
    return true;
}

} // namespace

const char* LedModeToString(const LedMode mode) {
    switch (mode) {
        case LedMode::Red:
            return "red";
        case LedMode::Green:
            return "green";
        case LedMode::Blue:
            return "blue";
        case LedMode::White:
            return "white";
        case LedMode::Rgb:
            return "rgb";
        case LedMode::Rgbw:
            return "rgbw";
        case LedMode::Count:
        default:
            return "unknown";
    }
}

uint8_t LedModeActiveChannelCount(const LedMode mode) {
    switch (mode) {
        case LedMode::Rgb:
            return 3U;
        case LedMode::Rgbw:
            return 4U;
        case LedMode::Red:
        case LedMode::Green:
        case LedMode::Blue:
        case LedMode::White:
        case LedMode::Count:
        default:
            return 1U;
    }
}

uint8_t LedLevelToPwm(uint8_t level1To30) {
    if (level1To30 < 1U) {
        level1To30 = 1U;
    }
    if (level1To30 > LED_LEVEL_COUNT) {
        level1To30 = LED_LEVEL_COUNT;
    }

    return static_cast<uint8_t>(((static_cast<uint32_t>(level1To30) * 255U) + (LED_LEVEL_COUNT / 2U)) /
                                LED_LEVEL_COUNT);
}

bool TryParseCurrentInput(std::string_view text, uint32_t& outMilliAmps) {
    std::string_view trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        return false;
    }

    uint32_t unitScaleMilli = 1U;
    if (EndsWithInsensitive(trimmed, "ma")) {
        trimmed = TrimWhitespace(trimmed.substr(0, trimmed.size() - 2U));
        unitScaleMilli = 1U;
    } else if (EndsWithInsensitive(trimmed, "a")) {
        trimmed = TrimWhitespace(trimmed.substr(0, trimmed.size() - 1U));
        unitScaleMilli = 1000U;
    } else if (trimmed.find('.') != std::string_view::npos) {
        unitScaleMilli = 1000U;
    }

    return TryParseScaledUnsignedDecimal(trimmed, unitScaleMilli, outMilliAmps);
}

InterpolatedEstimate InterpolateEstimate(const Measurement& baseline,
                                         const SampleSeries& samples,
                                         const uint16_t ledCount,
                                         const uint8_t activeChannelsPerLed) {
    InterpolatedEstimate estimate;
    estimate.baselineMilliAmps = baseline.milliAmps;

    if (!baseline.recorded || ledCount == 0U || activeChannelsPerLed == 0U) {
        return estimate;
    }

    uint64_t sumXPwmTimesDelta = 0ULL;
    uint64_t sumXPwmSquared = 0ULL;
    for (uint8_t level = 0; level < LED_LEVEL_COUNT; ++level) {
        const Measurement& sample = samples[level];
        if (!sample.recorded || sample.milliAmps < baseline.milliAmps) {
            continue;
        }

        const uint32_t pwm = LedLevelToPwm(static_cast<uint8_t>(level + 1U));
        const uint32_t delta = sample.milliAmps - baseline.milliAmps;
        sumXPwmTimesDelta += static_cast<uint64_t>(pwm) * static_cast<uint64_t>(delta);
        sumXPwmSquared += static_cast<uint64_t>(pwm) * static_cast<uint64_t>(pwm);
        ++estimate.sampleCount;
    }

    if (estimate.sampleCount == 0U || sumXPwmSquared == 0ULL) {
        return estimate;
    }

    estimate.valid = true;
    estimate.estimatedFullScaleDeltaMilliAmps =
        static_cast<int32_t>((255ULL * sumXPwmTimesDelta + (sumXPwmSquared / 2ULL)) / sumXPwmSquared);
    estimate.estimatedFullScaleMilliAmps =
        baseline.milliAmps + static_cast<uint32_t>(estimate.estimatedFullScaleDeltaMilliAmps);

    estimate.milliAmpsPerLedTimes100 =
        ((static_cast<uint64_t>(estimate.estimatedFullScaleDeltaMilliAmps) * 100ULL) + (ledCount / 2U)) / ledCount;
    const uint32_t channelCount = static_cast<uint32_t>(ledCount) * static_cast<uint32_t>(activeChannelsPerLed);
    estimate.milliAmpsPerChannelTimes100 =
        ((static_cast<uint64_t>(estimate.estimatedFullScaleDeltaMilliAmps) * 100ULL) + (channelCount / 2U)) /
        channelCount;

    for (uint8_t level = 0; level < LED_LEVEL_COUNT; ++level) {
        const Measurement& sample = samples[level];
        if (!sample.recorded || sample.milliAmps < baseline.milliAmps) {
            continue;
        }

        const uint32_t pwm = LedLevelToPwm(static_cast<uint8_t>(level + 1U));
        const uint32_t expectedDelta =
            (static_cast<uint64_t>(estimate.estimatedFullScaleDeltaMilliAmps) * static_cast<uint64_t>(pwm) + 127ULL) /
            255ULL;
        const uint32_t actualDelta = sample.milliAmps - baseline.milliAmps;
        const uint32_t residual = (expectedDelta > actualDelta) ? (expectedDelta - actualDelta)
                                                                : (actualDelta - expectedDelta);
        if (residual > estimate.maxResidualMilliAmps) {
            estimate.maxResidualMilliAmps = residual;
        }
    }

    return estimate;
}

} // namespace firmware::amptest
