#pragma once

#include <cstdint>

#include "modules/boot_fade_in.hpp"
#include "hardware/board_revision.hpp"

namespace firmware {

// The startup loop owns this sequence. Rendering and initialization stay on
// that same task; the final accepted transfer, not elapsed time, opens services.
class StartupLedSequence final {
  public:
    // V3.3 must start Hall/services before choosing an empty-board intro.
    static constexpr bool UsesPreServiceGrid(hardware::BoardRevision revision) {
        return hardware::ResolveBoardRevision(revision) != hardware::BoardRevision::V3_3S3;
    }
    static constexpr uint32_t FADE_MS = 500U;
    static constexpr uint32_t FRAME_MS = 25U;

    void Begin(const bool gridEnabled = true) {
        m_fade = modules::BootFadeIn({gridEnabled ? FADE_MS : 0U, FRAME_MS});
        m_fade.Start();
        m_fadeStartedMs = fxe::Millis();
        m_beginMs = m_fadeStartedMs;
    }
    const modules::BootFadeIn& Fade() const { return m_fade; }
    bool IsReady() const { return m_ready; }
    bool NeedsOutputRecovery() const {
        return !m_ready && m_outputRejected &&
               fxe::Millis() - m_beginMs >= FADE_MS;
    }
    uint32_t AcceptedFrames() const { return m_acceptedFrames; }
    uint32_t RejectedFrames() const { return m_rejectedFrames; }
    uint32_t MaxFrameGapMs() const { return m_maxFrameGapMs; }
    uint32_t FadeStartedMs() const { return m_fadeStartedMs; }
    uint32_t FadeCompletedMs() const { return m_fadeCompletedMs; }

    template <typename Render, typename Initialize>
    void Update(Render&& render, Initialize&& initialize) {
        if (m_ready || !m_fade.IsStarted()) {
            return;
        }
        const uint32_t nowMs = fxe::Millis();
        if (m_attempted && nowMs - m_lastAttemptMs < FRAME_MS) {
            return;
        }
        m_attempted = true;
        m_lastAttemptMs = nowMs;
        if (m_acceptedFrames == 0U) {
            // Initial output failure cannot consume the fade invisibly. Every
            // initial attempt is black; the first accepted one starts the ramp.
            m_fade.Start();
            m_fadeStartedMs = nowMs;
        }
        const uint16_t fadeScale256 = m_fade.GetFadeScale256();
        if (!render(fadeScale256)) {
            ++m_rejectedFrames;
            m_outputRejected = true;
            return;
        }
        m_outputRejected = false;
        if (m_acceptedFrames != 0U) {
            const uint32_t gapMs = nowMs - m_lastAcceptedMs;
            if (gapMs > m_maxFrameGapMs) {
                m_maxFrameGapMs = gapMs;
            }
        }
        ++m_acceptedFrames;
        m_lastAcceptedMs = nowMs;
        if (fadeScale256 == 256U) {
            m_fadeCompletedMs = fxe::Millis();
            initialize();
            m_ready = true;
        }
    }

  private:
    modules::BootFadeIn m_fade{{FADE_MS, FRAME_MS}};
    bool m_ready = false;
    bool m_attempted = false;
    bool m_outputRejected = false;
    uint32_t m_lastAttemptMs = 0U;
    uint32_t m_lastAcceptedMs = 0U;
    uint32_t m_acceptedFrames = 0U;
    uint32_t m_rejectedFrames = 0U;
    uint32_t m_maxFrameGapMs = 0U;
    uint32_t m_fadeCompletedMs = 0U;
    uint32_t m_fadeStartedMs = 0U;
    uint32_t m_beginMs = 0U;
};

} // namespace firmware
