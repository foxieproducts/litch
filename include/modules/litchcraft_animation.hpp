#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/rgb.hpp"
namespace modules::litchcraft_frame_store { struct Emitter; }

namespace modules::litchcraft_animation {

inline constexpr uint8_t SCHEMA = 3U;
inline constexpr size_t EMITTER_COUNT = 256U;
inline constexpr size_t EMITTERS_PER_SQUARE = 4U;
inline constexpr size_t MIN_FRAME_COUNT = 1U;
inline constexpr size_t MAX_FRAME_COUNT = 8U;
inline constexpr uint16_t MIN_FRAME_DURATION_MS = 50U;
inline constexpr uint16_t MAX_FRAME_DURATION_MS = 5000U;
inline constexpr uint16_t DEFAULT_FRAME_DURATION_MS = 1000U;
inline constexpr uint16_t MIN_EFFECT_DURATION_MS = 500U;
inline constexpr uint16_t MAX_EFFECT_DURATION_MS = 10000U;
inline constexpr uint16_t DEFAULT_EFFECT_DURATION_MS = 2000U;

enum class Effect : uint8_t {
    Off = 0U,
    Breathe = 1U,
    Flash = 2U,
    FadeToBlack = 3U,
    Rainbow = 4U,
    OrbitClockwise = 5U,
    OrbitCounterclockwise = 6U,
    Bounce = 7U,
    Sparkle = 8U,
};

// The effect contract admits only exact 100ms steps up to 10 seconds.
// Keep millisecond semantics at every caller and wire boundary, but store one
// byte per emitter. Invalid inputs retain an invalid sentinel; never round a
// malformed duration into an accepted value. Frame durations remain uint16_t.
class EffectDuration final {
  public:
    constexpr EffectDuration(uint16_t milliseconds = 0U)
        : m_ticks(milliseconds <= MAX_EFFECT_DURATION_MS && milliseconds % 100U == 0U
                      ? static_cast<uint8_t>(milliseconds / 100U) : 255U) {}
    constexpr operator uint16_t() const {
        return m_ticks == 255U ? UINT16_MAX : static_cast<uint16_t>(m_ticks * 100U);
    }
  private:
    uint8_t m_ticks;
};

struct Emitter final {
    Rgb color{};
    Effect effect = Effect::Off;
    EffectDuration durationMs{};
};
static_assert(sizeof(Emitter) == 5U, "exact RGB/effect/duration needs five bytes");

using Square = std::array<Emitter, EMITTERS_PER_SQUARE>;
using ComposedSquare = std::array<Rgb, EMITTERS_PER_SQUARE>;

struct Frame final {
    uint16_t durationMs = DEFAULT_FRAME_DURATION_MS;
    std::array<Emitter, EMITTER_COUNT> emitters{};
};

struct Document final {
    uint8_t frameCount = 1U;
    uint8_t topology = 3U;
    bool playbackOnEntry = false;
    uint32_t sparkleSeed = 1U;
    std::array<Rgb, 8U> favorites{};
    std::array<Frame, MAX_FRAME_COUNT> frames{};
};

class ValidatedDocument final {
  public:
    static bool TryAdopt(const Document& candidate, ValidatedDocument& out);
    // Exchanges an activation-owned staging body with the current authority,
    // avoiding a third temporary body during decode admission.
    static bool TryAdoptBySwap(Document& candidate, ValidatedDocument& out);
    bool IsAdopted() const { return m_adopted; }
    const Document& Value() const { return m_document; }

  private:
    Document m_document{};
    bool m_adopted = false;
};

struct ComposedFrame final {
    uint8_t frameIndex = 0U;
    uint16_t emittersEvaluated = 0U;
    uint16_t groupCandidatesVisited = 0U;
    std::array<Rgb, EMITTER_COUNT> emitters{};
};

// Runtime playback authority. The injected monotonic sample is captured once
// by the caller per rendered frame; pause/seek never mutate the document.
class PlaybackCursor final {
  public:
    uint64_t Sample(uint64_t monotonicMs) const;
    uint64_t Pause(uint64_t monotonicMs);
    void Resume(uint64_t monotonicMs);
    void Seek(uint64_t elapsedMs, uint64_t monotonicMs);
    bool SelectFrame(const ValidatedDocument& document, uint8_t frameIndex,
                     uint64_t monotonicMs);
    bool IsPlaying() const { return m_playing; }

  private:
    uint64_t m_elapsedMs = 0U;
    uint64_t m_anchorMs = 0U;
    bool m_playing = false;
};

bool IsValidEffect(uint8_t effect);
bool IsValidFrameDuration(uint16_t durationMs);
bool IsValidEffectDuration(Effect effect, uint16_t durationMs);
bool IsValidDocumentHeader(uint8_t schema, uint8_t frameCount,
                           uint8_t topology, uint8_t flags, uint32_t sparkleSeed);
bool IsValidEmitter(const Emitter& emitter);
bool IsValidDocument(const Document& document);
bool DocumentsEqual(const Document& left, const Document& right);
bool TryFrameStartElapsed(const ValidatedDocument& document, uint8_t frameIndex,
                          uint64_t& outElapsedMs);
uint8_t FrameIndexAt(const ValidatedDocument& document, uint64_t elapsedMs);

// Composes one validated semantic square before topology mapping, optical
// calibration, brightness, or power limiting. Invalid input fails dark.
// emitterBaseIndex must be squareIndex * 4.
ComposedSquare ComposeSquare(const Square& square, uint64_t elapsedMs,
                             uint32_t sparkleSeed, uint8_t frameIndex,
                             uint16_t emitterBaseIndex);
ComposedSquare ComposeStoredSquare(
    const std::array<litchcraft_frame_store::Emitter, EMITTERS_PER_SQUARE>&
        square,
    uint64_t elapsedMs, uint32_t sparkleSeed, uint16_t frameIndex,
    uint16_t emitterBaseIndex);
ComposedFrame ComposeDocument(const ValidatedDocument& document,
                              uint64_t elapsedMs);
// Frame transport and continuously running square effects have independent
// accumulated times, sampled from the same monotonic clock by their owner.
ComposedFrame ComposeDocument(const ValidatedDocument& document,
                              uint64_t frameElapsedMs, uint64_t effectElapsedMs);

} // namespace modules::litchcraft_animation
