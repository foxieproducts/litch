#include "modules/litchcraft_session.hpp"

#include <algorithm>
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_persistence.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules {
namespace {
bool Same(const Rgb a, const Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
uint8_t ClosestColorWheelPosition(const Rgb color) {
    uint8_t best = 0U;
    uint32_t bestDistance = UINT32_MAX;
    for (uint16_t position = 0U; position < 256U; ++position) {
        const auto candidate = ui_color_wheel::WheelToRgb(
            static_cast<uint8_t>(position));
        const int32_t dr = static_cast<int32_t>(color.r) - candidate.r;
        const int32_t dg = static_cast<int32_t>(color.g) - candidate.g;
        const int32_t db = static_cast<int32_t>(color.b) - candidate.b;
        const uint32_t distance = static_cast<uint32_t>(
            dr * dr + dg * dg + db * db);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<uint8_t>(position);
        }
    }
    return best;
}
} // namespace

void LitchcraftSession::Configure(const uint8_t hardware, const uint32_t nonce) {
    m_hardware = hardware >= 1U && hardware <= 3U ? hardware : 2U;
    m_nonce = nonce == 0U ? 1U : nonce;
    m_colorWheel = ClosestColorWheelPosition(m_color);
    m_shakeDetector.Reset();
}
void LitchcraftSession::ChangeTool(const bool erase, const uint8_t target,
                                  const Rgb color, const uint16_t request,
                                  const bool rememberColor) {
    if (m_erase == erase && m_target == target && Same(m_color, color)) {
        return;
    }
    EndStroke();
    SelectTool(erase, target, color, request, rememberColor);
    const uint64_t footprint = m_occupancy & ~m_blockedOccupancy;
    if (!StoreIsSoleArtworkOwner() && !HasStreamedDocument() &&
        footprint != 0U && !m_quickPaletteVisible) {
        StartStroke(Source::Board);
        PaintFootprint(footprint, request);
    }
}
void LitchcraftSession::SelectTool(const bool erase, const uint8_t target,
                                  const Rgb color, const uint16_t request,
                                  const bool rememberColor) {
    if (erase && !m_erase) {
        m_eraseCueStarted = true;
        m_eraseSelectedMs = fxe::Millis();
    } else if (!erase) {
        m_eraseCueStarted = false;
    }
    m_erase = erase;
    m_target = target;
    m_color = color;
    m_colorWheel = ClosestColorWheelPosition(color);
    m_palette = 255U;
    for (uint8_t i = 0U; i < LITCHCRAFT_DEFAULT_QUICK_PALETTE.size(); ++i) {
        if (Same(color, LITCHCRAFT_DEFAULT_QUICK_PALETTE[i])) {
            m_palette = i;
            break;
        }
    }
    if (!erase && rememberColor) RememberColor(color);
    ++m_revision;
    Queue(ToolPacket(request));
    m_activity = true;
}

void LitchcraftSession::ObserveOccupancy(const uint64_t occupancy) {
    if (occupancy != 0ULL) CancelBootArtwork();
    if (!m_active) {
        return;
    }
    if (m_restoreOccupancyPending || m_mutationsPaused ||
        (StoreIsSoleArtworkOwner() && m_frameStream == nullptr)) {
        m_restoreOccupancyPending = false;
        ResynchronizeOccupancy(occupancy);
        return;
    }
    if (m_quickPaletteVisible) {
        const uint64_t previous = m_occupancy;
        const uint64_t previouslyBlocked = m_blockedOccupancy;
        m_occupancy = occupancy;
        m_blockedOccupancy &= occupancy;
        if (previous != occupancy) {
            m_pickPending = false;
            m_pickConfirmed = false;
            uint64_t paletteMask = 0U;
            for (uint8_t slot = 0U; slot < 8U; ++slot) {
                paletteMask |= UINT64_C(1) << QuickPaletteSquare(slot);
            }
            const bool sampledPalette = (previous & paletteMask) != 0U;
            const bool stillOnPalette = (occupancy & paletteMask) != 0U;
            (void)SelectQuickPaletteFromOccupancy(occupancy);
            if (sampledPalette && !stillOnPalette) {
                CloseQuickPalette();
                // Re-run this transition through ordinary painting. A Hall
                // scan may coalesce the swatch lift and first destination into
                // one complete mask, so the destination is a new brush rather
                // than occupancy that the closing palette should block.
                m_occupancy = previous;
                m_blockedOccupancy = previouslyBlocked & occupancy;
            } else {
                m_activity = true;
                Ack(0U);
                return;
            }
        } else {
            return;
        }
    }
    const uint64_t previous = m_occupancy;
    const uint32_t observedAtMs = fxe::Millis();
    if (m_pickPending && (occupancy & (UINT64_C(1) << m_pickSquare)) == 0U &&
        observedAtMs - m_pickStartedMs > PICK_ARM_MS) {
        // Freeze the visible semantic blend before lift resets the gesture or
        // a coalesced destination starts painting with the selected brush.
        CompleteColorPick(ColorPickSample(observedAtMs));
    }
    const bool wasWaiting = IsWaitingForLift();
    m_occupancy = occupancy;
    m_blockedOccupancy &= occupancy;
    const bool pickWasVisible = m_pickPending || m_pickConfirmed;
    if (previous != occupancy) {
        m_pickPending = false;
        m_pickConfirmed = false;
    }
    const uint64_t newFootprint = occupancy & ~previous & ~m_blockedOccupancy;
    if (newFootprint != 0U) {
        if (m_releasePending && fxe::Millis() - m_releaseAtMs >= PHYSICAL_RELEASE_MS) {
            EndStroke();
        }
        StartStroke(Source::Board);
        m_releasePending = false;
        if (previous == 0U && (newFootprint & (newFootprint - 1U)) == 0U) {
            for (uint8_t square = 0U; square < 64U; ++square) {
                if (newFootprint == (UINT64_C(1) << square)) {
                    m_pickSquare = square;
                    bool pickReady = true;
                    if (StoreIsSoleArtworkOwner() || HasStreamedDocument()) {
                        std::array<litchcraft_frame_store::Emitter, 4U> emitters{};
                        uint16_t frame = 0U;
                        uint16_t duration = 0U;
                        const uint32_t beforeEpoch =
                            m_frameStream->DocumentEpoch();
                        pickReady = m_frameStream->CopyDisplayedSquare(
                            square, emitters, frame, duration);
                        const uint32_t afterEpoch =
                            m_frameStream->DocumentEpoch();
                        pickReady = pickReady && beforeEpoch == afterEpoch;
                        if (pickReady) {
                            m_pickDocumentEpoch = afterEpoch;
                            m_pickBeforeEmitters = emitters;
                            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                                m_pickBefore[slot] = emitters[slot].color;
                            }
                        }
                    } else {
                        std::copy_n(m_canvas.begin() + square * 4U, 4U,
                                    m_pickBefore.begin());
                    }
                    m_pickPriorStrokeChanged = m_strokeChanged;
                    m_pickStartedMs = fxe::Millis();
                    m_pickPending = pickReady;
                    break;
                }
            }
        }
        PaintFootprint(newFootprint, 0U);
    }
    if ((occupancy & ~m_blockedOccupancy) == 0U && m_strokeSource == Source::Board &&
        !m_releasePending) {
        m_releaseAtMs = fxe::Millis();
        m_releasePending = true;
    }
    if (previous != occupancy) {
        m_activity = true;
        Ack(0U);
        if (pickWasVisible || m_pickPending) {
            PublishColorPickStatus();
        }
    } else if (wasWaiting != IsWaitingForLift()) {
        Ack(0U);
    }
}
void LitchcraftSession::ResynchronizeOccupancy(const uint64_t occupancy) {
    if (!m_active) {
        return;
    }
    m_restoreOccupancyPending = false;
    const bool pickWasVisible = m_pickPending || m_pickConfirmed;
    EndStroke();
    m_pickConfirmed = false;
    m_occupancy = occupancy;
    m_blockedOccupancy = occupancy;
    Ack(0U);
    if (pickWasVisible) {
        PublishColorPickStatus();
    }
}
LitchcraftSession::ModeChordResult LitchcraftSession::ObserveEntryButton(
    const ButtonEvent& event, const uint32_t eventTimeMs,
    const bool eligible, const bool blockedByOccupancy,
    const uint64_t occupancy) {
    if (m_mutationsPaused || m_active ||
        (!IsXButton(event.button) && !IsOButton(event.button))) {
        return ModeChordResult::Ignored;
    }
    if (event.event == fxe::Button::Event::Press) {
        if (!m_modeChordPending) {
            m_modeChordEvent = event;
            m_modeChordPending = true;
            m_modeChordFormed = false;
            m_modeChordStartedMs = eventTimeMs;
            return ModeChordResult::Pending;
        }
        const bool formed = event.owner == m_modeChordEvent.owner &&
                            event.button != m_modeChordEvent.button &&
                            eventTimeMs - m_modeChordStartedMs <=
                                MODE_CHORD_PRIORITY_MS;
        if (!formed) return ModeChordResult::Replay;
        if (eligible) {
            m_modeChordFormed = true;
            return ModeChordResult::Pending;
        }
        if (blockedByOccupancy) {
            CancelEntryHold();
            return ModeChordResult::Blocked;
        }
        // One Play-mode arbiter owns the first edge. A game in progress replays
        // both captured edges into the bounded New Game hold controller.
        return ModeChordResult::Replay;
    }
    if (m_modeChordFormed && event.event == fxe::Button::Event::Release &&
        event.owner == m_modeChordEvent.owner &&
        (IsXButton(event.button) || IsOButton(event.button))) {
        const bool complete = eventTimeMs - m_modeChordStartedMs >= ENTRY_HOLD_MS;
        m_modeChordPending = false;
        m_modeChordFormed = false;
        m_modeChordReleasePending = false;
        if (complete && eligible) {
            EnterFromBoardControls(occupancy);
            return ModeChordResult::Entered;
        }
        if (complete && blockedByOccupancy) {
            return ModeChordResult::Blocked;
        }
        return ModeChordResult::Cancelled;
    }
    if (m_modeChordFormed) return ModeChordResult::Pending;
    if (event.owner == m_modeChordEvent.owner && event.button == m_modeChordEvent.button) {
        return ModeChordResult::Replay;
    }
    return ModeChordResult::Ignored;
}
bool LitchcraftSession::TryExpireEntryButton(
    const uint32_t nowMs, ButtonEvent& outEvent, uint32_t& outEventTimeMs) {
    const uint32_t elapsed = nowMs - m_modeChordStartedMs;
    if (m_active || !m_modeChordPending || m_modeChordFormed ||
        elapsed >= 0x80000000U || elapsed <= MODE_CHORD_PRIORITY_MS) {
        return false;
    }
    return TakeEntryButtonForReplay(outEvent, outEventTimeMs);
}
bool LitchcraftSession::TryGetPendingEntryButton(
    ButtonEvent& outEvent, uint32_t& outEventTimeMs) const {
    if (m_active || !m_modeChordPending || m_modeChordFormed) return false;
    outEvent = m_modeChordEvent;
    outEventTimeMs = m_modeChordStartedMs;
    return true;
}
bool LitchcraftSession::TakeEntryButtonForReplay(
    ButtonEvent& outEvent, uint32_t& outEventTimeMs) {
    if (!TryGetPendingEntryButton(outEvent, outEventTimeMs)) return false;
    m_modeChordPending = false;
    m_modeChordFormed = false;
    m_modeChordReleasePending = false;
    return true;
}
void LitchcraftSession::CancelEntryHold() {
    m_modeChordPending = false;
    m_modeChordFormed = false;
    m_modeChordReleasePending = false;
    ResetPhysicalButtons();
}
void LitchcraftSession::EnterFromBoardControls(const uint64_t occupancy) {
    AdoptStreamedModeAuthority();
    ResetPhysicalStoreBatch();
    m_active = true;
    if (HasStreamedDocument()) ResetStreamedPlayback();
    else if (HasAdvancedDocument()) ResetAdvancedPlayback();
    MarkModeChanged();
    m_session = ++m_nonce;
    if (m_session == 0U) m_session = ++m_nonce;
    m_occupancy = occupancy;
    m_blockedOccupancy = occupancy;
    m_exited = false;
    m_canvasInvalidated = true;
    ++m_revision;
    m_activity = true;
    Ack(0U);
    WakeStreamedStorage();
}
void LitchcraftSession::Update() {
    if (HasStreamedDocument()) SampleStreamedPlayback();
    else SampleAdvancedPlayback();
    const bool frameReady = CanMutatePresentedFrame();
    if (StoreIsSoleArtworkOwner() && !HasStreamedDocument()) {
        m_pendingPhysicalHistoryFrame = UINT16_MAX;
        SetAnimationPlaying(false);
        m_advancedSelectedFrame = 0U;
        const bool pickWasVisible = m_pickPending || m_pickConfirmed;
        if (m_strokeSource != Source::None || pickWasVisible) EndStroke();
        m_pickConfirmed = false;
        if (pickWasVisible) PublishColorPickStatus();
    }
    if (frameReady && m_pendingStoreStructural && !m_pendingStoreHistoryOrdered &&
        m_active && !m_mutationsPaused && HasStreamedDocument()) {
        const auto status = CompleteStoreStructural();
        if (status != Status::Busy) Ack(0U, status);
    }
    if (!m_pendingStoreStructural && !m_pendingStoreHistoryOrdered &&
        m_pendingPhysicalHistoryFrame != UINT16_MAX && m_active &&
        !m_mutationsPaused && HasStreamedDocument()) {
        const uint16_t frame = m_pendingPhysicalHistoryFrame;
        const uint32_t revision = m_pendingPhysicalHistoryRevision;
        if (m_advancedSelectedFrame != frame ||
            m_frameStream->LiveRevision() != revision) {
            m_pendingPhysicalHistoryFrame = UINT16_MAX;
        } else if (frameReady && m_frameStream->DisplayedIndex() == frame) {
            m_pendingPhysicalHistoryFrame = UINT16_MAX;
            const auto status = ApplyPhysicalStoreHistory(m_pendingPhysicalHistoryRedo,
                                                          m_pendingPhysicalHistoryOwner);
            Ack(0U, status);
        } else if (m_frameStream->RequestedFrameFailed() ||
                   static_cast<uint32_t>(fxe::Millis() -
                       m_pendingPhysicalHistoryAtMs) >= 5000U) {
            m_pendingPhysicalHistoryFrame = UINT16_MAX;
            const uint16_t displayed = m_frameStream->DisplayedIndex();
            if (displayed < m_frameStream->FrameCount()) {
                m_advancedSelectedFrame = static_cast<uint8_t>(displayed);
                (void)m_frameStream->Request(displayed);
            }
            m_activity = true;
            Ack(0U);
        }
    }
    UpdateV2Transport();
    if (m_mutationsPaused) return;
    const uint32_t now = fxe::Millis();
    if (frameReady && m_active) UpdatePhysicalButtons();
    if (frameReady && m_pickPending &&
        now - m_pickStartedMs >= PICK_ARM_MS + PICK_PROGRESS_MS) {
        CompleteColorPick(ColorPickSample(now));
    }
    if (m_releasePending && now - m_releaseAtMs >= PHYSICAL_RELEASE_MS) {
        EndStroke();
        Ack(0U);
    }
    if (m_replacing && now - m_replaceAtMs >= REPLACE_TIMEOUT_MS) {
        m_replacing = false;
    }
    if (m_quickPaletteVisible &&
        now - m_quickPaletteTouchedMs >= QUICK_PALETTE_IDLE_MS) {
        uint64_t paletteMask = 0U;
        for (uint8_t slot = 0U; slot < 8U; ++slot) {
            paletteMask |= UINT64_C(1) << QuickPaletteSquare(slot);
        }
        if ((m_occupancy & paletteMask) == 0U) CloseQuickPalette();
    }
}
void LitchcraftSession::Exit() {
    CancelBootArtwork();
    RetireV2Transfer();
    if (m_mutationsPaused || !m_active) {
        return;
    }
    // Mode authority changes in this input cycle. Flash persistence owns a
    // separate flush; its receipt cannot block navigation or reverse a newer
    // mode choice. Sleep/restart/update still await their durable barrier.
    m_exitFlushRequested = true;
    MarkModeChanged();
    FinishExit();
}
void LitchcraftSession::FinishExit() {
    ResetPhysicalButtons();
    if (m_controlFeedback != nullptr) m_controlFeedback->Reset();
    m_pendingPhysicalHistoryFrame = UINT16_MAX;
    if (HasStreamedDocument()) {
        SampleStreamedPlayback();
        SetAnimationPlaying(false);
        m_frameStream->RequestImmediateSave();
    } else if (HasAdvancedDocument()) {
        PauseAdvancedPlayback();
    }
    const bool pickWasVisible = m_pickPending || m_pickConfirmed;
    EndStroke();
    ResetPhysicalStoreBatch();
    CloseQuickPalette();
    m_active = false;
    if (HasStreamedDocument()) WakeStreamedStorage();
    m_pickConfirmed = false;
    m_exited = true;
    m_replacing = false;
    ++m_revision;
    Ack(0U);
    if (pickWasVisible) {
        PublishColorPickStatus();
    }
}
bool LitchcraftSession::ConsumeExited() {
    const bool out = m_exited;
    m_exited = false;
    return out;
}
bool LitchcraftSession::ConsumeActivity() {
    const bool out = m_activity;
    m_activity = false;
    return out;
}
} // namespace modules
