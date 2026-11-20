#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_session.hpp"

namespace modules {
bool LitchcraftSession::BeginBootArtworkOnce() {
    if (!HasStreamedDocument() || m_mutationsPaused)
        return false;
    AdoptStreamedModeAuthority();
    const bool resumePlaying = m_advancedPlaying;
    m_bootResumeFrame = m_advancedSelectedFrame;
    ResetStreamedPlayback();
    m_bootArtworkFlags = static_cast<uint8_t>(1U | 8U | (resumePlaying ? 4U : 0U));
    SetAnimationPlaying(true);
    WakeStreamedStorage();
    return true;
}
void LitchcraftSession::UpdateBootArtwork() {
    if (IsBootArtworkPlaying())
        SampleStreamedPlayback();
}
void LitchcraftSession::NoteBootArtworkPresented() {
    if (IsBootArtworkPlaying() && (m_bootArtworkFlags & 8U) != 0U &&
        m_streamFrameStarted && !m_streamAwaitingInitialFrame) {
        m_streamFrameStartedAtMs = fxe::Millis();
        m_bootArtworkFlags &= static_cast<uint8_t>(~8U);
    }
}
void LitchcraftSession::CancelBootArtwork() {
    if (!IsBootArtworkPlaying())
        return;
    const bool resumePlaying = (m_bootArtworkFlags & 4U) != 0U;
    const uint8_t resumeFrame = m_bootResumeFrame;
    ResetStreamedPlayback();
    m_advancedSelectedFrame = resumeFrame < FrameCount() ? resumeFrame : 0U;
    if (HasStreamedDocument())
        (void)m_frameStream->Request(m_advancedSelectedFrame);
    SetAnimationPlaying(resumePlaying);
    WakeStreamedStorage(); // still tagged boot-only: release/resume the same pager
    m_bootArtworkFlags = 0U;
    // The normal owner resumes on its next tick. Do not publish a temporary
    // playback preference or mode-root save for the cosmetic boot episode.
}
} // namespace modules
