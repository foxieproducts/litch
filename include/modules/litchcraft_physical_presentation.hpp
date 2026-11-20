#pragma once

#include <cstddef>
#include <array>

#include "modules/rgb.hpp"

namespace modules::litchcraft_physical_presentation {

// One painted V3 square can illuminate four emitters. Bound only the physical
// canvas pixels so dense artwork does not bloom toward white; status previews
// retain their separately bounded scale.
constexpr uint16_t CANVAS_SCALE256 = 128U;

// Presentation alone owns this transition. Mode admission/acknowledgement never
// waits for it, and interrupted transitions start from the last shown frame.
template <size_t LedCount>
class ModeCrossfade {
  public:
    static constexpr uint32_t DURATION_MS = 250U;
    using Frame = std::array<Rgb, LedCount>;

    void Reset() { m_hasPresented = false; m_active = false; }

    // Reuse the last accepted output for a cosmetic transition whose target
    // mode stays the same (for example, stored boot artwork returning to Craft).
    void TransitionFromPresented(const bool litchcraft, const uint32_t nowMs) {
        if (!m_hasPresented) return;
        m_from = m_presented;
        m_fromMix = m_presentedMix;
        m_startedMs = nowMs;
        m_active = true;
        m_litchcraft = litchcraft;
    }

    void Apply(Frame& frame, const bool litchcraft, const uint32_t nowMs,
               const bool urgent = false) {
        const uint16_t targetMix = litchcraft ? 0U : 256U;
        if (!m_hasPresented || urgent) {
            m_active = false;
            m_litchcraft = litchcraft;
            m_whiteMix = targetMix;
            return;
        }
        if (m_litchcraft != litchcraft) {
            m_from = m_presented;
            m_fromMix = m_presentedMix;
            m_startedMs = nowMs;
            m_active = true;
            m_litchcraft = litchcraft;
        }
        const uint32_t elapsed = nowMs - m_startedMs;
        if (!m_active || elapsed >= DURATION_MS) {
            m_active = false;
            m_whiteMix = targetMix;
            return;
        }
        const auto blend = [elapsed](const uint16_t from, const uint16_t to) {
            return static_cast<uint16_t>((from * (DURATION_MS - elapsed) +
                                          to * elapsed + DURATION_MS / 2U) / DURATION_MS);
        };
        for (size_t index = 0U; index < LedCount; ++index) {
            frame[index] = {
                static_cast<uint8_t>(blend(m_from[index].r, frame[index].r)),
                static_cast<uint8_t>(blend(m_from[index].g, frame[index].g)),
                static_cast<uint8_t>(blend(m_from[index].b, frame[index].b))};
        }
        m_whiteMix = blend(m_fromMix, targetMix);
    }

    void Presented(const Frame& frame) {
        m_presented = frame;
        m_presentedMix = m_whiteMix;
        m_hasPresented = true;
    }
    uint16_t WhiteMix(const bool litchcraft) const {
        return m_hasPresented && m_litchcraft == litchcraft
            ? m_whiteMix : (litchcraft ? 0U : 256U);
    }

  private:
    Frame m_from{};
    Frame m_presented{};
    uint32_t m_startedMs = 0U;
    uint16_t m_whiteMix = 256U;
    uint16_t m_fromMix = 256U;
    uint16_t m_presentedMix = 256U;
    bool m_hasPresented = false;
    bool m_litchcraft = false;
    bool m_active = false;
};

template <typename Frame>
void ApplyCanvasScale(Frame& physicalFrame, const size_t physicalBoardLedCount) {
    const size_t count = physicalBoardLedCount < physicalFrame.size()
        ? physicalBoardLedCount : physicalFrame.size();
    for (size_t index = 0U; index < count; ++index) {
        physicalFrame[index] = ScaleRgbSaturatingBy256(
            physicalFrame[index], CANVAS_SCALE256);
    }
}

} // namespace modules::litchcraft_physical_presentation
