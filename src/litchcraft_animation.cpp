#include "modules/litchcraft_animation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_animation {
namespace {

constexpr uint32_t PHASE_SCALE = 65536U;
constexpr uint32_t CHANNEL_SCALE = 65535U;

uint32_t Phase(const uint64_t elapsedMs, const uint16_t durationMs) {
    return static_cast<uint32_t>((elapsedMs % durationMs) * PHASE_SCALE /
                                 durationMs);
}

uint8_t ScaleChannel(const uint8_t channel, const uint32_t scale) {
    const uint32_t product = static_cast<uint32_t>(channel) * scale;
    return static_cast<uint8_t>((product + CHANNEL_SCALE / 2U) /
                                CHANNEL_SCALE);
}

Rgb ScaleColor(const Rgb color, const uint32_t scale) {
    return {ScaleChannel(color.r, scale), ScaleChannel(color.g, scale),
            ScaleChannel(color.b, scale)};
}

uint32_t RoundedDivide(const uint32_t numerator, const uint32_t denominator) {
    return (numerator + denominator / 2U) / denominator;
}

Rgb RotateHue(const Rgb color, const uint32_t phase) {
    const uint8_t maximum = std::max({color.r, color.g, color.b});
    const uint8_t minimum = std::min({color.r, color.g, color.b});
    const uint32_t chroma = static_cast<uint32_t>(maximum - minimum);
    if (chroma == 0U) {
        return color;
    }

    int32_t hue = 0;
    if (maximum == color.r) {
        hue = (color.g >= color.b)
                  ? static_cast<int32_t>(RoundedDivide(
                        256U * static_cast<uint32_t>(color.g - color.b), chroma))
                  : 1536 - static_cast<int32_t>(RoundedDivide(
                               256U * static_cast<uint32_t>(color.b - color.g),
                               chroma));
    } else if (maximum == color.g) {
        if (color.b >= color.r) {
            hue = 512 + static_cast<int32_t>(RoundedDivide(
                            256U * static_cast<uint32_t>(color.b - color.r),
                            chroma));
        } else {
            hue = 512 - static_cast<int32_t>(RoundedDivide(
                            256U * static_cast<uint32_t>(color.r - color.b),
                            chroma));
        }
    } else if (color.r >= color.g) {
        hue = 1024 + static_cast<int32_t>(RoundedDivide(
                         256U * static_cast<uint32_t>(color.r - color.g), chroma));
    } else {
        hue = 1024 - static_cast<int32_t>(RoundedDivide(
                         256U * static_cast<uint32_t>(color.g - color.r), chroma));
    }

    const uint32_t rotation = phase * 1536U / PHASE_SCALE;
    const uint32_t rotated =
        (static_cast<uint32_t>((hue % 1536 + 1536) % 1536) + rotation) % 1536U;
    const uint32_t sector = rotated / 256U;
    const uint32_t fraction = rotated % 256U;
    const uint8_t rising = static_cast<uint8_t>(
        minimum + RoundedDivide(chroma * fraction, 256U));
    const uint8_t falling = static_cast<uint8_t>(
        minimum + chroma - RoundedDivide(chroma * fraction, 256U));

    switch (sector) {
    case 0U:
        return {maximum, rising, minimum};
    case 1U:
        return {falling, maximum, minimum};
    case 2U:
        return {minimum, maximum, rising};
    case 3U:
        return {minimum, falling, maximum};
    case 4U:
        return {rising, minimum, maximum};
    default:
        return {maximum, minimum, falling};
    }
}

uint32_t SparkleSlot(const uint32_t seed, const uint8_t frameIndex,
                     const uint16_t emitterIndex, const uint64_t cycle) {
    uint32_t hash = seed ^ (static_cast<uint32_t>(frameIndex) * 0x9e3779b9U) ^
                    (static_cast<uint32_t>(emitterIndex) * 0x85ebca6bU) ^
                    static_cast<uint32_t>(cycle);
    hash ^= hash << 13U;
    hash ^= hash >> 17U;
    hash ^= hash << 5U;
    return hash & 3U;
}

bool SameGroup(const Emitter& left, const Emitter& right) {
    return left.effect == right.effect && left.durationMs == right.durationMs;
}

Rgb ComposeIndependent(const Emitter& emitter, const uint64_t elapsedMs,
                       const uint32_t sparkleSeed, const uint8_t frameIndex,
                       const uint16_t emitterIndex) {
    if (emitter.color.r == 0U && emitter.color.g == 0U && emitter.color.b == 0U) {
        return RGB_BLACK;
    }
    if (emitter.effect == Effect::Off) {
        return emitter.color;
    }
    const uint32_t phase = Phase(elapsedMs, emitter.durationMs);
    const uint32_t triangle =
        65535U - static_cast<uint32_t>(
                     std::abs(static_cast<int32_t>(2U * phase) - 65535));
    switch (emitter.effect) {
    case Effect::Breathe:
        return ScaleColor(
            emitter.color,
            16384U + static_cast<uint32_t>(49151ULL * triangle / 65535U));
    case Effect::Flash:
        return phase < 32768U ? emitter.color : RGB_BLACK;
    case Effect::FadeToBlack:
        return ScaleColor(emitter.color, triangle);
    case Effect::Rainbow:
        return RotateHue(emitter.color, phase);
    case Effect::Sparkle: {
        const uint32_t slot = phase * 4U / PHASE_SCALE;
        const uint64_t cycle = elapsedMs / emitter.durationMs;
        return slot == SparkleSlot(sparkleSeed, frameIndex, emitterIndex, cycle)
                   ? emitter.color
                   : ScaleColor(emitter.color, 16384U);
    }
    case Effect::OrbitClockwise:
    case Effect::OrbitCounterclockwise:
    case Effect::Bounce:
        return emitter.color;
    case Effect::Off:
        return emitter.color;
    }
    return RGB_BLACK;
}

uint8_t FrameIndexAtValidated(const Document& document,
                              const uint64_t elapsedMs) {
    uint64_t totalDuration = 0U;
    for (uint8_t index = 0U; index < document.frameCount; ++index) {
        totalDuration += document.frames[index].durationMs;
    }
    const uint64_t loopElapsed = elapsedMs % totalDuration;
    uint64_t boundary = 0U;
    for (uint8_t index = 0U; index < document.frameCount; ++index) {
        boundary += document.frames[index].durationMs;
        if (loopElapsed < boundary) {
            return index;
        }
    }
    return 0U;
}

void ComposeSquareUnchecked(const Emitter* const square, Rgb* const output,
                            const uint64_t elapsedMs,
                            const uint32_t sparkleSeed,
                            const uint8_t frameIndex,
                            const uint16_t emitterBaseIndex,
                            uint16_t* const groupCandidatesVisited) {
    for (size_t index = 0U; index < EMITTERS_PER_SQUARE; ++index) {
        const Emitter& emitter = square[index];
        output[index] = ComposeIndependent(
            emitter, elapsedMs, sparkleSeed, frameIndex,
            static_cast<uint16_t>(emitterBaseIndex + index));
        if (emitter.effect != Effect::OrbitClockwise &&
            emitter.effect != Effect::OrbitCounterclockwise &&
            emitter.effect != Effect::Bounce) {
            continue;
        }

        size_t memberCount = 0U;
        size_t memberPosition = 0U;
        for (size_t candidate = 0U; candidate < EMITTERS_PER_SQUARE;
             ++candidate) {
            if (groupCandidatesVisited != nullptr) {
                ++*groupCandidatesVisited;
            }
            if (!SameGroup(emitter, square[candidate])) {
                continue;
            }
            if (candidate == index) {
                memberPosition = memberCount;
            }
            ++memberCount;
        }
        const uint32_t phase = Phase(elapsedMs, emitter.durationMs);
        size_t activePosition = 0U;
        if (emitter.effect == Effect::Bounce && memberCount > 1U) {
            const size_t pathLength = 2U * memberCount - 2U;
            const size_t step = phase * pathLength / PHASE_SCALE;
            activePosition = step < memberCount ? step : pathLength - step;
        } else {
            activePosition = phase * memberCount / PHASE_SCALE;
            if (emitter.effect == Effect::OrbitCounterclockwise) {
                activePosition = memberCount - 1U - activePosition;
            }
        }
        output[index] = memberPosition == activePosition ? emitter.color : RGB_BLACK;
    }
}

} // namespace

bool IsValidEffect(const uint8_t effect) {
    return effect <= static_cast<uint8_t>(Effect::Sparkle);
}

bool IsValidFrameDuration(const uint16_t durationMs) {
    return durationMs >= MIN_FRAME_DURATION_MS &&
           durationMs <= MAX_FRAME_DURATION_MS;
}

bool IsValidEffectDuration(const Effect effect, const uint16_t durationMs) {
    if (effect == Effect::Off) {
        return durationMs == 0U;
    }
    return IsValidEffect(static_cast<uint8_t>(effect)) &&
           durationMs >= MIN_EFFECT_DURATION_MS &&
           durationMs <= MAX_EFFECT_DURATION_MS && durationMs % 100U == 0U;
}

bool IsValidDocumentHeader(const uint8_t schema, const uint8_t frameCount,
                           const uint8_t topology, const uint8_t flags,
                           const uint32_t sparkleSeed) {
    return schema == SCHEMA && frameCount >= MIN_FRAME_COUNT &&
           frameCount <= MAX_FRAME_COUNT && topology >= 1U && topology <= 3U &&
           (flags & 0xfeU) == 0U && sparkleSeed != 0U;
}

bool IsValidEmitter(const Emitter& emitter) {
    return IsValidEffect(static_cast<uint8_t>(emitter.effect)) &&
           IsValidEffectDuration(emitter.effect, emitter.durationMs);
}

bool TryFrameStartElapsed(const ValidatedDocument& adopted,
                          const uint8_t frameIndex,
                          uint64_t& outElapsedMs) {
    if (!adopted.IsAdopted()) {
        return false;
    }
    const Document& document = adopted.Value();
    if (frameIndex >= document.frameCount) {
        return false;
    }
    uint64_t start = 0U;
    for (uint8_t index = 0U; index < frameIndex; ++index) {
        start += document.frames[index].durationMs;
    }
    outElapsedMs = start;
    return true;
}

uint8_t FrameIndexAt(const ValidatedDocument& adopted,
                     const uint64_t elapsedMs) {
    if (!adopted.IsAdopted()) {
        return 0U;
    }
    const Document& document = adopted.Value();
    return FrameIndexAtValidated(document, elapsedMs);
}

ComposedSquare ComposeSquare(const Square& square, const uint64_t elapsedMs,
                             const uint32_t sparkleSeed,
                             const uint8_t frameIndex,
                             const uint16_t emitterBaseIndex) {
    ComposedSquare output{};
    if (sparkleSeed == 0U || frameIndex >= MAX_FRAME_COUNT ||
        emitterBaseIndex > EMITTER_COUNT - EMITTERS_PER_SQUARE ||
        emitterBaseIndex % EMITTERS_PER_SQUARE != 0U ||
        std::any_of(square.begin(), square.end(),
                    [](const Emitter& emitter) { return !IsValidEmitter(emitter); })) {
        return output;
    }
    ComposeSquareUnchecked(square.data(), output.data(), elapsedMs, sparkleSeed,
                           frameIndex, emitterBaseIndex, nullptr);
    return output;
}

ComposedSquare ComposeStoredSquare(
    const std::array<litchcraft_frame_store::Emitter,
                     EMITTERS_PER_SQUARE>& square,
    const uint64_t elapsedMs, const uint32_t sparkleSeed,
    const uint16_t frameIndex, const uint16_t emitterBaseIndex) {
    ComposedSquare output{};
    if (sparkleSeed == 0U ||
        frameIndex >= litchcraft_frame_store::FRAME_COUNT ||
        emitterBaseIndex > EMITTER_COUNT - EMITTERS_PER_SQUARE ||
        emitterBaseIndex % EMITTERS_PER_SQUARE != 0U) {
        return output;
    }
    Square translated{};
    for (size_t index = 0U; index < square.size(); ++index) {
        const auto& source = square[index];
        if (!IsValidEffect(source.effect) ||
            !IsValidEffectDuration(static_cast<Effect>(source.effect),
                                   source.effectDurationMs)) {
            return output;
        }
        translated[index] = {
            source.color, static_cast<Effect>(source.effect),
            EffectDuration(source.effectDurationMs)};
    }
    ComposeSquareUnchecked(translated.data(), output.data(), elapsedMs,
                           sparkleSeed, static_cast<uint8_t>(frameIndex),
                           emitterBaseIndex, nullptr);
    return output;
}

ComposedFrame ComposeDocument(const ValidatedDocument& adopted,
                              const uint64_t elapsedMs) {
    return ComposeDocument(adopted, elapsedMs, elapsedMs);
}

ComposedFrame ComposeDocument(const ValidatedDocument& adopted,
                              const uint64_t frameElapsedMs,
                              const uint64_t effectElapsedMs) {
    ComposedFrame output{};
    if (!adopted.IsAdopted()) {
        return output;
    }
    const Document& document = adopted.Value();
    output.frameIndex = FrameIndexAtValidated(document, frameElapsedMs);
    const Frame& frame = document.frames[output.frameIndex];
    for (size_t squareIndex = 0U; squareIndex < 64U; ++squareIndex) {
        const size_t base = squareIndex * EMITTERS_PER_SQUARE;
        ComposeSquareUnchecked(frame.emitters.data() + base,
                               output.emitters.data() + base, effectElapsedMs,
                               document.sparkleSeed, output.frameIndex,
                               static_cast<uint16_t>(base),
                               &output.groupCandidatesVisited);
    }
    output.emittersEvaluated = EMITTER_COUNT;
    return output;
}

uint64_t PlaybackCursor::Sample(const uint64_t monotonicMs) const {
    return m_playing && monotonicMs >= m_anchorMs
               ? m_elapsedMs + (monotonicMs - m_anchorMs)
               : m_elapsedMs;
}

uint64_t PlaybackCursor::Pause(const uint64_t monotonicMs) {
    m_elapsedMs = Sample(monotonicMs);
    m_anchorMs = monotonicMs;
    m_playing = false;
    return m_elapsedMs;
}

void PlaybackCursor::Resume(const uint64_t monotonicMs) {
    if (m_playing) {
        return;
    }
    m_anchorMs = monotonicMs;
    m_playing = true;
}

void PlaybackCursor::Seek(const uint64_t elapsedMs, const uint64_t monotonicMs) {
    m_elapsedMs = elapsedMs;
    m_anchorMs = monotonicMs;
}

bool PlaybackCursor::SelectFrame(const ValidatedDocument& document,
                                 const uint8_t frameIndex,
                                 const uint64_t monotonicMs) {
    uint64_t frameStart = 0U;
    if (!TryFrameStartElapsed(document, frameIndex, frameStart)) {
        return false;
    }
    Pause(monotonicMs);
    Seek(frameStart, monotonicMs);
    return true;
}

} // namespace modules::litchcraft_animation
