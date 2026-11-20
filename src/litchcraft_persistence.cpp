#include "modules/litchcraft_persistence.hpp"

#include <algorithm>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/millis.hpp"

namespace modules {
void LitchcraftPersistence::Configure(const Ops ops) {
    m_ops = ops;
}
void LitchcraftPersistence::ConfigureStoreOnly(const Ops ops) {
    Configure(ops);
    m_storeOnly = true;
    m_settings.Doc().remove(LITCHCRAFT_ART_KEY);
}
void LitchcraftPersistence::ConfigureAdvancedStorage(AdvancedStorage& storage) {
    m_advanced = &storage;
    m_session.AttachAdvancedPersistence(this);
}
LitchcraftPersistence::AdvancedStorage*
LitchcraftPersistence::AcquireAdvancedTransportLease() {
    // Static receipts own the worker's JSON buffer, not this envelope. An
    // independent entry-mode save must not reject the immediate snapshot.
    if (!IsAdvancedConfigured() || m_advancedTransportLeased ||
        (m_receipt != 0U && m_submittedKind == SubmittedKind::Advanced)) return nullptr;
    m_advancedTransportLeased = true;
    return m_advanced;
}
LitchcraftPersistence::AdvancedStorage*
LitchcraftPersistence::AdvancedTransportStorage() {
    return m_advancedTransportLeased ? m_advanced : nullptr;
}
void LitchcraftPersistence::ReleaseAdvancedTransportLease() {
    m_advancedTransportLeased = false;
}
bool LitchcraftPersistence::AdoptAdvancedTransportDocument(
    const size_t size, const uint32_t crc, const bool active) {
    const bool previouslyAdopted = HasAdvancedDocument();
    const uint8_t beforeFrame = m_session.m_advancedSelectedFrame;
    if (!m_advancedTransportLeased || !IsAdvancedConfigured() || m_advancedRestoreBlocked ||
        !litchcraft_animation::DecodeDocument(m_advanced->envelope.bytes.data(),
                                               size, crc, m_advanced->staging,
                                               m_advanced->document)) return false;
    // A complete validated document must own rendering before its ACK. Do not
    // touch worker cursors or clear the displayed cache during this handoff.
    const bool replacingStream = m_session.m_frameStream != nullptr &&
        m_session.m_frameStream->RetireDocument();
    m_advanced->active = active;
    // Decode swaps the old live body into staging; compare those two existing
    // bodies so a semantically identical replacement creates no new revision.
    if (!replacingStream && previouslyAdopted && litchcraft_animation::DocumentsEqual(
            m_advanced->staging, m_advanced->document.Value())) {
        m_session.PauseAdvancedPlayback();
        return true;
    }
    if (!previouslyAdopted) {
        // The first advanced operation still retains the previous static art.
        // Its undo representation is the equivalent single schema-3 frame.
        m_advanced->staging.frameCount = 1U;
        m_advanced->staging.topology = m_session.m_hardware;
        m_advanced->staging.favorites = m_session.m_recentColors;
        for (size_t i = 0U; i < m_session.m_canvas.size(); ++i) {
            m_advanced->staging.frames[0].emitters[i].color = m_session.m_canvas[i];
        }
    }
    m_session.InitializeAdvancedRuntime();
    m_session.EndStroke();
    m_session.SelectAdvancedFrame(0U);
    m_session.RecordAdvancedMutation(beforeFrame, false);
    return true;
}
bool LitchcraftPersistence::IsAdvancedConfigured() const {
#if defined(LITCH_ESP32S3)
    return false;
#else
    return m_advanced != nullptr && m_ops.readAdvanced != nullptr &&
           (m_ops.enqueueAdvanced != nullptr ||
            m_ops.enqueueAdvancedDocument != nullptr) &&
           m_ops.poll != nullptr &&
           m_ops.release != nullptr;
#endif
}
bool LitchcraftPersistence::HasAdvancedDocument() const {
    return IsAdvancedConfigured() && m_advanced->document.IsAdopted();
}
const litchcraft_animation::ValidatedDocument&
LitchcraftPersistence::AdvancedDocument() const {
    return m_advanced->document;
}
LitchcraftPersistence::AdvancedRecordStatus
LitchcraftPersistence::RestoreAdvanced() {
    if (!IsAdvancedConfigured()) return AdvancedRecordStatus::BackendUnavailable;
    const auto current = ReadAdvancedSlot(AdvancedSlot::Current);
    if (current == AdvancedRecordStatus::BackendUnavailable) {
        m_advancedRestoreBlocked = true;
        m_failed = true;
        return current;
    }
    if (current == AdvancedRecordStatus::Valid) {
        m_advancedRestoreBlocked = false;
        m_failed = m_restoreBlocked;
        return current;
    }
    if (current == AdvancedRecordStatus::FutureVersion) {
        m_advancedRestoreBlocked = true;
        m_failed = true;
        return current;
    }
    const auto lastGood = ReadAdvancedSlot(AdvancedSlot::LastGood);
    if (lastGood == AdvancedRecordStatus::BackendUnavailable) {
        m_advancedRestoreBlocked = true;
        m_failed = true;
        return lastGood;
    }
    if (lastGood == AdvancedRecordStatus::Valid) {
        m_advancedRestoreBlocked = false;
        m_failed = m_restoreBlocked;
        return AdvancedRecordStatus::Recovered;
    }
    if (current == AdvancedRecordStatus::Missing &&
        lastGood == AdvancedRecordStatus::Missing) {
        m_advancedRestoreBlocked = false;
        m_failed = m_restoreBlocked;
        return AdvancedRecordStatus::Missing;
    }
    // A future-version record remains authoritative and must be preserved, but
    // corrupt bytes with no validated fallback are not an authority. Keep the
    // invalid result visible while allowing an explicit valid replacement and
    // unrelated static-mode persistence (including a physical exit).
    m_advancedRestoreBlocked = lastGood == AdvancedRecordStatus::FutureVersion;
    m_failed = true;
    return lastGood == AdvancedRecordStatus::FutureVersion
        ? AdvancedRecordStatus::FutureVersion
        : AdvancedRecordStatus::Invalid;
}
bool LitchcraftPersistence::SetAdvancedDocument(
    const litchcraft_animation::ValidatedDocument& document,
    const bool active) {
    if (!IsAdvancedConfigured() || !document.IsAdopted() ||
        m_advancedRestoreBlocked) {
        return false;
    }
    const bool unchanged = HasAdvancedDocument() && litchcraft_animation::DocumentsEqual(
        m_advanced->document.Value(), document.Value());
    m_advanced->document = document;
    m_advanced->active = active;
    if (unchanged) return true;
    ++m_advancedGeneration;
    m_advancedChangedAtMs = fxe::Millis();
    m_failed = m_restoreBlocked || m_advancedRestoreBlocked;
    m_session.InitializeAdvancedRuntime();
    m_session.SyncAdvancedCanvas();
    return true;
}
LitchcraftRecordStatus LitchcraftPersistence::Restore(
    const uint64_t occupancy, const bool bootOccupancyKnown) {
    const auto status = ReadLitchcraftRecord(m_settings.Doc(), m_durable);
    m_restoreBlocked = status == LitchcraftRecordStatus::Invalid ||
                       status == LitchcraftRecordStatus::FutureVersion;
    m_failed = m_restoreBlocked || m_advancedRestoreBlocked;
    if (status == LitchcraftRecordStatus::Valid) {
        m_session.RestoreArtwork(m_durable.canvas, m_durable.active, occupancy,
                                 &m_durable.quickPalette, bootOccupancyKnown);
    }
    m_savedCanvasGeneration = m_session.CanvasGeneration();
    m_savedModeGeneration = m_session.ModeGeneration();
    m_savedPaletteGeneration = m_session.PaletteGeneration();
    return status;
}
bool LitchcraftPersistence::IsDirty() const {
    if (m_storeOnly) return false;
    return m_savedCanvasGeneration != m_session.CanvasGeneration() ||
           m_savedModeGeneration != m_session.ModeGeneration() ||
           m_savedPaletteGeneration != m_session.PaletteGeneration() ||
           m_savedAdvancedGeneration != m_advancedGeneration;
}
void LitchcraftPersistence::RequestFlush() {
    if (m_flushRequested) return;
    m_flushRequested = true;
    m_flushState = FlushStatus::Pending;
    m_failed = false;
}
void LitchcraftPersistence::RestoreDurableDocument() {
    (void)WriteLitchcraftRecord(m_settings.Doc(), m_durable, m_encoded,
                                m_encodedPalette);
}
fxe::SettingsStore::AsyncSaveAdmission
LitchcraftPersistence::QueueAdvanced() {
    if (!HasAdvancedDocument() || m_receipt != 0U) {
        return fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    }
    AdvancedEnvelope& envelope = m_advanced->envelope;
    uint32_t receipt = 0U;
    fxe::SettingsStore::AsyncSaveAdmission admitted =
        fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    if (m_ops.enqueueAdvancedDocument != nullptr) {
        m_advanced->staging = m_advanced->document.Value();
        envelope.size = 0U;
        admitted = m_ops.enqueueAdvancedDocument(
            m_ops.context, m_advanced->staging, m_advanced->active,
            envelope, receipt);
    } else {
        if (!EncodeAdvancedEnvelope(m_advanced->document.Value(),
                                    m_advanced->active, envelope)) {
            return fxe::SettingsStore::AsyncSaveAdmission::Rejected;
        }
        admitted = m_ops.enqueueAdvanced(
            m_ops.context, envelope.bytes.data(), envelope.size, receipt);
    }
    if (admitted == fxe::SettingsStore::AsyncSaveAdmission::Accepted &&
        receipt != 0U) {
        m_receipt = receipt;
        m_submittedKind = SubmittedKind::Advanced;
        m_submittedAdvancedGeneration = m_advancedGeneration;
        m_lastAdvancedReceipt = receipt;
        m_submittedEnvelopeCrc = envelope.size == 0U ? 0U : fxe::ReadU32LE(
            envelope.bytes.data() + envelope.size - ADVANCED_ENVELOPE_TRAILER_BYTES);
        m_submittedEnvelopeFrames = m_advanced->document.Value().frameCount;
        return admitted;
    }
    return admitted == fxe::SettingsStore::AsyncSaveAdmission::Deferred
        ? admitted : fxe::SettingsStore::AsyncSaveAdmission::Rejected;
}

bool LitchcraftPersistence::EncodeAdvancedEnvelope(
    const litchcraft_animation::Document& document, const bool active,
    AdvancedEnvelope& envelope) {
    size_t bodySize = 0U;
    uint32_t bodyCrc = 0U;
    if (!litchcraft_animation::EncodeDocumentToBuffer(
            document,
            envelope.bytes.data() + ADVANCED_BODY_OFFSET,
            envelope.bytes.size() - ADVANCED_BODY_OFFSET -
                ADVANCED_ENVELOPE_TRAILER_BYTES,
            bodySize, bodyCrc)) {
        return false;
    }
    envelope.size = ADVANCED_BODY_OFFSET + bodySize +
                    ADVANCED_ENVELOPE_TRAILER_BYTES;
    std::fill_n(envelope.bytes.begin(), ADVANCED_BODY_OFFSET, 0U);
    envelope.bytes[0] = 'L';
    envelope.bytes[1] = 'C';
    envelope.bytes[2] = 'A';
    envelope.bytes[3] = '3';
    envelope.bytes[4] = 1U;
    envelope.bytes[5] = active ? 1U : 0U;
    envelope.bytes[8] = static_cast<uint8_t>(bodySize);
    envelope.bytes[9] = static_cast<uint8_t>(bodySize >> 8U);
    envelope.bytes[12] = static_cast<uint8_t>(bodyCrc);
    envelope.bytes[13] = static_cast<uint8_t>(bodyCrc >> 8U);
    envelope.bytes[14] = static_cast<uint8_t>(bodyCrc >> 16U);
    envelope.bytes[15] = static_cast<uint8_t>(bodyCrc >> 24U);
    RefreshAdvancedEnvelopeChecksum(envelope);
    return true;
}
void LitchcraftPersistence::Update() {
    if (m_storeOnly) {
        // Store mode exit is saved by the frame worker. Only an explicit
        // lifecycle/OTA flush needs a tracked settings receipt here.
        (void)m_session.ConsumeExitFlushRequested();
        if (!m_flushRequested) return;
        if (!IsConfigured()) {
            m_failed = true;
            m_flushRequested = false;
            m_flushState = FlushStatus::Failed;
            return;
        }
        if (m_receipt != 0U) {
            const auto outcome = m_ops.poll(m_ops.context, m_receipt);
            if (outcome == SaveOutcome::Pending) return;
            m_ops.release(m_ops.context, m_receipt);
            m_receipt = 0U;
            m_failed = outcome != SaveOutcome::Saved;
            m_flushRequested = false;
            m_flushState = m_failed ? FlushStatus::Failed : FlushStatus::Saved;
            return;
        }
        uint32_t receipt = 0U;
        const auto admission = m_ops.enqueue(m_ops.context, m_settings.Doc(), receipt);
        if (admission == fxe::SettingsStore::AsyncSaveAdmission::Accepted &&
            receipt != 0U) {
            m_receipt = receipt;
        } else if (admission != fxe::SettingsStore::AsyncSaveAdmission::Deferred) {
            m_failed = true;
            m_flushRequested = false;
            m_flushState = FlushStatus::Failed;
        }
        return;
    }
    if (m_session.ConsumeExitFlushRequested()) RequestFlush();
    if (!IsConfigured() && !IsAdvancedConfigured()) {
        if (m_flushRequested) m_flushState = FlushStatus::Failed;
        return;
    }
    const auto fail = [&]() {
        m_failed = true;
        m_flushRequested = false;
        m_flushState = FlushStatus::Failed;
        m_failedCanvasGeneration = m_session.CanvasGeneration();
        m_failedModeGeneration = m_session.ModeGeneration();
        m_failedPaletteGeneration = m_session.PaletteGeneration();
        m_failedAdvancedGeneration = m_advancedGeneration;
    };
    if (m_restoreBlocked || m_advancedRestoreBlocked) {
        fail();
        return;
    }
    if (m_receipt != 0U) {
        const auto outcome = m_ops.poll(m_ops.context, m_receipt);
        if (outcome == SaveOutcome::Pending) return;
        if (outcome == SaveOutcome::Saved) {
            if (m_submittedKind == SubmittedKind::Advanced) {
                // The worker verifies both Pending and promoted Current against
                // the frozen envelope before publishing Saved. Reading it again
                // here would put flash waits back on the input/render caller.
                m_savedAdvancedGeneration = m_submittedAdvancedGeneration;
                m_savedAdvancedReceipt = m_receipt;
                if (m_submittedEnvelopeCrc == 0U &&
                    m_advanced->envelope.size >=
                        ADVANCED_BODY_OFFSET + ADVANCED_ENVELOPE_TRAILER_BYTES) {
                    m_submittedEnvelopeCrc = fxe::ReadU32LE(
                        m_advanced->envelope.bytes.data() +
                        m_advanced->envelope.size -
                        ADVANCED_ENVELOPE_TRAILER_BYTES);
                }
                m_savedEnvelopeCrc = m_submittedEnvelopeCrc;
                m_savedEnvelopeFrames = m_submittedEnvelopeFrames;
                m_failed = false;
                // Retain a composite lifecycle flush until its static settings
                // snapshot also saves, even when artwork is now caught up.
            } else {
                LitchcraftRecord saved;
                if (ReadLitchcraftRecord(m_settings.Doc(), saved) !=
                    LitchcraftRecordStatus::Valid) {
                    // Only this owner changes the art field while its receipt is
                    // retained. A violated source invariant must not claim success.
                    RestoreDurableDocument();
                    fail();
                } else {
                    m_durable = saved;
                    m_savedCanvasGeneration = m_submittedCanvasGeneration;
                    m_savedModeGeneration = m_submittedModeGeneration;
                    m_savedPaletteGeneration = m_submittedPaletteGeneration;
                    m_failed = false;
                    if (m_submittedForFlush && !IsDirty()) {
                        m_flushRequested = false;
                        m_flushState = FlushStatus::Saved;
                    }
                }
            }
        } else {
            // Restore the old mode before releasing the receipt: unrelated
            // settings must never write a failed exit's inactive candidate.
            if (m_submittedKind == SubmittedKind::Static) {
                RestoreDurableDocument();
            }
            fail();
        }
        m_ops.release(m_ops.context, m_receipt);
        m_receipt = 0U;
        m_submittedKind = SubmittedKind::None;
        if (m_failed) return;
    }
    if (m_failed && !m_flushRequested &&
        m_failedCanvasGeneration == m_session.CanvasGeneration() &&
        m_failedModeGeneration == m_session.ModeGeneration() &&
        m_failedPaletteGeneration == m_session.PaletteGeneration() &&
        m_failedAdvancedGeneration == m_advancedGeneration) return;
    const uint32_t now = fxe::Millis();
    if (m_advancedTransportLeased) return;
    const bool advancedDirty =
        m_savedAdvancedGeneration != m_advancedGeneration;
    const bool advancedDue = advancedDirty &&
        now - m_advancedChangedAtMs >= CANVAS_IDLE_MS;
    if (advancedDirty && (m_flushRequested || advancedDue)) {
        const auto admitted = QueueAdvanced();
        if (admitted == fxe::SettingsStore::AsyncSaveAdmission::Accepted) {
            m_submittedForFlush = m_flushRequested;
        } else if (admitted !=
                   fxe::SettingsStore::AsyncSaveAdmission::Deferred) {
            fail();
        }
        return;
    }
    const bool canvasDirty =
        m_savedCanvasGeneration != m_session.CanvasGeneration();
    const bool paletteDirty =
        m_savedPaletteGeneration != m_session.PaletteGeneration();
    const bool artworkDue =
        (canvasDirty || paletteDirty) &&
        (!canvasDirty ||
         now - m_session.CanvasChangedAtMs() >= CANVAS_IDLE_MS) &&
        (!paletteDirty ||
         now - m_session.PaletteChangedAtMs() >= CANVAS_IDLE_MS);
    const bool modeDue = m_savedModeGeneration != m_session.ModeGeneration() &&
        now - m_session.ModeChangedAtMs() >= MODE_IDLE_MS;
    if (!m_flushRequested && !artworkDue && !modeDue) return;
    if (!IsConfigured()) {
        fail();
        return;
    }
    LitchcraftRecord candidate = m_durable;
    m_submittedCanvasGeneration = m_savedCanvasGeneration;
    m_submittedPaletteGeneration = m_savedPaletteGeneration;
    if ((artworkDue && canvasDirty) || m_flushRequested) {
        candidate.canvas = m_session.Artwork();
        m_submittedCanvasGeneration = m_session.CanvasGeneration();
    }
    if ((artworkDue && paletteDirty) || m_flushRequested) {
        candidate.quickPalette = m_session.RecentColors();
        m_submittedPaletteGeneration = m_session.PaletteGeneration();
    }
    candidate.active = m_session.IsActive();
    m_submittedModeGeneration = m_session.ModeGeneration();
    if (!WriteLitchcraftRecord(m_settings.Doc(), candidate, m_encoded,
                               m_encodedPalette)) {
        RestoreDurableDocument();
        fail();
        return;
    }
    uint32_t receipt = 0U;
    const auto admitted = m_ops.enqueue(m_ops.context, m_settings.Doc(), receipt);
    if (admitted == fxe::SettingsStore::AsyncSaveAdmission::Accepted && receipt != 0U) {
        m_receipt = receipt;
        m_submittedKind = SubmittedKind::Static;
        m_submittedForFlush = m_flushRequested;
    } else {
        RestoreDurableDocument();
        if (admitted != fxe::SettingsStore::AsyncSaveAdmission::Deferred) fail();
    }
}
} // namespace modules
