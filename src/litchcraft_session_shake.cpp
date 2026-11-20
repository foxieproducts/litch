#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {

void LitchcraftSession::ConfigureShakeErase(const bool supported) {
    m_shakeEraseSupported = supported && m_hardware == 3U;
    m_shakeDetector.Reset();
    if (!m_shakeEraseSupported) {
        m_shakeEraseAnimationActive = false;
    }
}

void LitchcraftSession::ObserveMotionSample(const int32_t xMg,
                                            const int32_t yMg,
                                            const int32_t zMg) {
    if (!m_shakeDetector.Observe(
            xMg, yMg, zMg, fxe::Millis(), ShakeEraseEligible())) {
        return;
    }
    (void)ApplyShakeErase();
}

bool LitchcraftSession::ShakeEraseEligible() const {
    return m_shakeEraseSupported && FrameEraseEligible() &&
        m_physicalFaceDown[0] == 0U && m_physicalFaceDown[1] == 0U;
}

bool LitchcraftSession::FrameEraseEligible() const {
    return m_active &&
        (!StoreIsSoleArtworkOwner() || HasStreamedDocument()) &&
        !m_mutationsPaused && !m_modeChordPending &&
        m_occupancy == 0U && m_blockedOccupancy == 0U &&
        m_strokeSource == Source::None && !m_releasePending &&
        !m_pickPending && !m_pickConfirmed && !m_quickPaletteVisible;
}

bool LitchcraftSession::ApplyShakeErase() {
    return ShakeEraseEligible() && ApplyFrameErase(true);
}

bool LitchcraftSession::ApplyFrameErase(const bool shake) {
    if (!FrameEraseEligible()) return false;
    EndStroke();
    bool changed = false;
    if (HasStreamedDocument()) {
        PinStreamedFrameForEdit();
        const uint16_t frame = m_advancedSelectedFrame;
        const uint32_t revision = m_frameStream->LiveRevision();
        using Edit = litchcraft_frame_stream::FrameStream::GuardedEditResult;
        const Edit result = m_frameStream->ClearDisplayedIfRevision(
            revision, frame);
        changed = result == Edit::Changed;
        if (!changed) return false;
        m_advancedStrokeFrame = static_cast<uint8_t>(frame);
        RecordPhysicalStoreEdit(UINT64_MAX, RGB_BLACK, 15U, true);
        WakeStreamedStorage();
    } else if (HasAdvancedDocument()) {
        InitializeAdvancedRuntime();
        if (!m_advancedRuntime) {
            return false;
        }
        auto& storage = *m_advancedPersistence->m_advanced;
        SampleAdvancedPlayback();
        const auto& displayed = storage.document.Value().frames[m_advancedSelectedFrame];
        changed = std::any_of(displayed.emitters.begin(), displayed.emitters.end(),
            [](const litchcraft_animation::Emitter& emitter) {
                return emitter.color.r != 0U || emitter.color.g != 0U ||
                    emitter.color.b != 0U ||
                    emitter.effect != litchcraft_animation::Effect::Off ||
                    emitter.durationMs != 0U;
            });
        if (!changed) return false;
        PauseAdvancedPlayback();
        storage.staging = storage.document.Value();
        auto& frame = storage.staging.frames[m_advancedSelectedFrame];
        for (auto& emitter : frame.emitters) {
            emitter = {};
        }
        if (!CommitAdvancedEdit(m_advancedSelectedFrame, false)) {
            return false;
        }
        EndStroke();
    } else {
        changed = std::any_of(m_canvas.begin(), m_canvas.end(), [](const Rgb color) {
            return color.r != 0U || color.g != 0U || color.b != 0U;
        });
        if (!changed) {
            return false;
        }
        StartStroke(Source::Board);
        m_canvas.fill(RGB_BLACK);
        MarkCanvasChanged();
        ++m_revision;
        m_strokeChanged = true;
        EndStroke();
        m_canvasInvalidated = true;
    }

    CloseQuickPalette();
    m_pickPending = false;
    m_pickConfirmed = false;
    m_activity = true;
    if (!shake) return true;
    if (++m_shakeEraseToken == 0U) {
        ++m_shakeEraseToken;
    }
    m_shakeEraseAnimationAtMs = fxe::Millis();
    m_shakeEraseAnimationActive = true;
    Queue(GesturePacket(1U, m_shakeEraseToken));
    return true;
}

} // namespace modules
