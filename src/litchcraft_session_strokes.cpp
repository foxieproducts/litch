#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/millis.hpp"

namespace modules {
namespace {
bool Same(const Rgb a, const Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
} // namespace

uint8_t LitchcraftSession::UndoCount() const {
    if (HasStreamedDocument()) return m_frameStream->UndoCount();
    if (StoreIsSoleArtworkOwner()) return 0U;
#if !defined(LITCH_ESP32S3)
    if (m_advancedRuntime) return m_advancedHistory.UndoCount();
    return static_cast<uint8_t>(m_history.UndoCount() + (m_strokeChanged ? 1U : 0U));
#else
    return 0U;
#endif
}
uint8_t LitchcraftSession::RedoCount() const {
    if (HasStreamedDocument()) return m_frameStream->RedoCount();
    if (StoreIsSoleArtworkOwner()) return 0U;
#if !defined(LITCH_ESP32S3)
    if (m_advancedRuntime) return m_advancedHistory.RedoCount();
    return m_strokeChanged ? 0U : m_history.RedoCount();
#else
    return 0U;
#endif
}
void LitchcraftSession::StartStroke(const Source source, const uint16_t stroke) {
    if (StoreIsSoleArtworkOwner() && !HasStreamedDocument()) return;
    if (HasStreamedDocument()) {
        if (m_frameStream->ReplacementPending()) return;
        if (m_strokeSource != source ||
            (source == Source::App && m_strokeId != stroke)) EndStroke();
        if (m_strokeSource == Source::None) {
            PinStreamedFrameForEdit();
            m_advancedStrokeFrame = m_advancedSelectedFrame;
            if (!m_frameStream->BeginHistoryStroke(m_advancedStrokeFrame)) {
                return;
            }
        }
        if (m_strokeSource == Source::None && m_supportActions != nullptr) {
            m_supportStroke = m_supportActions->Begin(m_erase ? trace::SupportActionKind::Erase : trace::SupportActionKind::Paint,
                source == Source::App ? trace::SupportActionOrigin::App : trace::SupportActionOrigin::Board);
            m_supportStrokeSquares = 0U;
        }
        m_strokeSource = source;
        m_strokeId = stroke;
        return;
    }
    InitializeAdvancedRuntime();
    if (m_strokeSource != source ||
        (source == Source::App && m_strokeId != stroke)) {
        EndStroke();
    }
    if (HasAdvancedDocument() && m_strokeSource == Source::None) {
        PauseAdvancedPlayback();
        m_advancedStrokeFrame = m_advancedSelectedFrame;
    } else if (!HasAdvancedDocument() && m_strokeSource == Source::None) {
        m_strokeBefore = m_canvas;
    }
    if (m_strokeSource == Source::None && m_supportActions != nullptr) {
        m_supportStroke = m_supportActions->Begin(m_erase ? trace::SupportActionKind::Erase : trace::SupportActionKind::Paint,
            source == Source::App ? trace::SupportActionOrigin::App : trace::SupportActionOrigin::Board);
        m_supportStrokeSquares = 0U;
    }
    m_strokeSource = source;
    m_strokeId = stroke;
}
void LitchcraftSession::EndStroke() {
    if (m_supportActions != nullptr && m_supportStroke != 0U) {
        m_supportActions->Finish(m_supportStroke, m_strokeChanged
            ? trace::SupportActionOutcome::Applied : trace::SupportActionOutcome::NoChange,
            static_cast<uint8_t>(__builtin_popcountll(m_supportStrokeSquares)));
        m_supportStroke = 0U;
    }
    m_pickPending = false;
    const bool streamed = HasStreamedDocument();
    if (streamed) m_frameStream->EndHistoryStroke();
#if !defined(LITCH_ESP32S3)
    if (!streamed && !StoreIsSoleArtworkOwner() &&
        m_strokeChanged && !m_advancedRuntime) {
        (void)m_history.Record(m_strokeBefore, m_canvas, false);
    }
#endif
    m_strokeChanged = false;
    m_strokeSource = Source::None;
    m_releasePending = false;
}
bool LitchcraftSession::PaintSquare(const uint8_t square, const uint16_t request) {
    const Rgb color = m_erase ? RGB_BLACK : m_color;
    bool changed = false;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        auto& pixel = m_canvas[square * 4U + slot];
        if ((m_target & (1U << slot)) != 0U && !Same(pixel, color)) {
            pixel = color;
            changed = true;
        }
    }
    if (!changed) {
        return false;
    }
    m_strokeChanged = true;
    m_supportStrokeSquares |= UINT64_C(1) << square;
    QueueCell(square, m_target, color, request);
    m_activity = true;
    return true;
}
void LitchcraftSession::QueueCell(const uint8_t square, const uint8_t target,
                                 const Rgb color, const uint16_t request) {
    MarkCanvasChanged();
    ++m_revision;
    auto packet = Header(Event::Cell, request, m_revision);
    packet.bytes[12] = square;
    packet.bytes[13] = target;
    packet.bytes[14] = color.r;
    packet.bytes[15] = color.g;
    packet.bytes[16] = color.b;
    packet.size = 17U;
    Queue(packet);
}
void LitchcraftSession::QueueCanvasChanges(const Canvas& previous, const uint16_t request) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        uint8_t pending = 0U;
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const size_t element = square * 4U + slot;
            if (!Same(previous[element], m_canvas[element])) {
                pending |= 1U << slot;
            }
        }
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if ((pending & (1U << slot)) == 0U) {
                continue;
            }
            const auto color = m_canvas[square * 4U + slot];
            uint8_t target = 0U;
            for (uint8_t other = slot; other < 4U; ++other) {
                if ((pending & (1U << other)) != 0U &&
                    Same(color, m_canvas[square * 4U + other])) {
                    target |= 1U << other;
                }
            }
            // Existing v1 clients apply each Cell as a successor revision.
            // Group matching emitters so an ordinary square costs one frame.
            QueueCell(square, target, color, request);
            pending &= ~target;
        }
    }
}
void LitchcraftSession::PaintFootprint(const uint64_t footprint, const uint16_t request) {
    if (HasStreamedDocument()) { PaintStreamedFootprint(footprint); return; }
    if (StoreIsSoleArtworkOwner()) return;
    if (HasAdvancedDocument()) { PaintAdvancedFootprint(footprint); return; }
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((footprint & (UINT64_C(1) << square)) != 0U) {
            PaintSquare(square, request);
        }
    }
}

void LitchcraftSession::PaintStreamedFootprint(const uint64_t footprint) {
    if (m_strokeSource != Source::Board) return;
    const Rgb color = m_erase ? RGB_BLACK : m_color;
    uint64_t changed = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((footprint & (UINT64_C(1) << square)) == 0U) continue;
        if (m_frameStream->PaintDisplayed(
                m_advancedStrokeFrame, square, m_target, color, m_erase)) {
            changed |= UINT64_C(1) << square;
            m_supportStrokeSquares |= UINT64_C(1) << square;
            RecordPhysicalStoreEdit(
                UINT64_C(1) << square, color, m_target, m_erase);
        }
    }
    m_strokeChanged = m_strokeChanged || changed != 0U;
    if (changed != 0U) {
        m_activity = true;
        WakeStreamedStorage();
    }
}

void LitchcraftSession::CompleteColorPick(const Rgb sample) {
    if (HasStreamedDocument()) { CompleteStreamedColorPick(sample); return; }
    if (StoreIsSoleArtworkOwner()) {
        EndStroke();
        m_pickConfirmed = false;
        PublishColorPickStatus();
        return;
    }
    if (HasAdvancedDocument()) { CompleteAdvancedColorPick(sample); return; }
    uint8_t restored = 0U;
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        const auto before = m_pickBefore[slot];
        auto& current = m_canvas[m_pickSquare * 4U + slot];
        if (Same(current, before)) {
            continue;
        }
        current = before;
        restored |= 1U << slot;
    }
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        if ((restored & (1U << slot)) == 0U) {
            continue;
        }
        uint8_t target = 0U;
        for (uint8_t other = slot; other < 4U; ++other) {
            if ((restored & (1U << other)) != 0U && Same(m_pickBefore[slot], m_pickBefore[other])) {
                target |= 1U << other;
            }
        }
        QueueCell(m_pickSquare, target, m_pickBefore[slot], 0U);
        restored &= ~target;
    }
    // The immediate mark was only a preview of this held touch. Earlier marks
    // in the same physical stroke remain one undoable operation.
    m_strokeChanged = m_pickPriorStrokeChanged;
    EndStroke();
    if (Same(sample, RGB_BLACK)) {
        SelectTool(true, m_target, m_color, 0U);
    } else {
        SelectTool(false, m_target, sample, 0U);
    }
    m_pickConfirmed = true;
    m_pickConfirmedMs = fxe::Millis();
    Ack(0U);
    PublishColorPickStatus();
}

void LitchcraftSession::CompleteStreamedColorPick(const Rgb sample) {
    if (m_frameStream->DocumentEpoch() != m_pickDocumentEpoch) {
        EndStroke();
        PublishColorPickStatus();
        return;
    }
    std::array<litchcraft_frame_store::Emitter, 4U> preview{};
    uint16_t frame = 0U;
    uint16_t duration = 0U;
    if (!m_frameStream->CopyDisplayedSquare(
            m_pickSquare, preview, frame, duration) ||
        frame != m_advancedStrokeFrame) {
        EndStroke();
        PublishColorPickStatus();
        return;
    }
    const bool restored = m_frameStream->RestoreDisplayedSquare(
        m_pickDocumentEpoch, frame, m_pickSquare, m_pickBeforeEmitters);
    if (m_frameStream->DocumentEpoch() != m_pickDocumentEpoch) {
        EndStroke();
        PublishColorPickStatus();
        return;
    }
    if (restored) {
        uint8_t pending = 0U;
        bool restoredEffects = false;
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if (!Same(preview[slot].color, m_pickBefore[slot])) {
                pending |= 1U << slot;
            }
            restoredEffects = restoredEffects ||
                preview[slot].effect != m_pickBeforeEmitters[slot].effect ||
                preview[slot].effectDurationMs !=
                    m_pickBeforeEmitters[slot].effectDurationMs;
        }
        while (pending != 0U) {
            uint8_t first = 0U;
            while ((pending & (1U << first)) == 0U) ++first;
            uint8_t mask = 0U;
            for (uint8_t slot = first; slot < 4U; ++slot) {
                if ((pending & (1U << slot)) != 0U &&
                    Same(m_pickBefore[slot], m_pickBefore[first])) {
                    mask |= 1U << slot;
                }
            }
            RecordPhysicalStoreEdit(UINT64_C(1) << m_pickSquare,
                                    m_pickBefore[first], mask, false);
            pending &= ~mask;
        }
        // The compact physical RGB event cannot restore a prior effect.
        // Its existing overflow signal asks the app for a Store snapshot.
        if (restoredEffects) m_physicalStoreBatchOverflow = true;
        WakeStreamedStorage();
    }
    m_strokeChanged = m_pickPriorStrokeChanged;
    EndStroke();
    const bool erase = Same(sample, RGB_BLACK);
    SelectTool(erase, m_target, erase ? m_color : sample, 0U);
    m_pickConfirmed = true;
    m_pickConfirmedMs = fxe::Millis();
    Ack(0U);
    PublishColorPickStatus();
}

void LitchcraftSession::RecordPhysicalStoreEdit(const uint64_t footprint,
                                                const Rgb color,
                                                const uint8_t mask,
                                                const bool clearEffects) {
    const auto authority = m_frameStream->ReadLiveAuthority();
    RecordPhysicalStoreEdit(footprint, color, mask, clearEffects, authority,
                            m_advancedStrokeFrame);
}

void LitchcraftSession::RecordPhysicalStoreEdit(
    const uint64_t footprint, const Rgb color, const uint8_t mask,
    const bool clearEffects,
    const litchcraft_frame_stream::FrameStream::LiveAuthority& authority,
    const uint8_t frame) {
    ++m_physicalStoreEdit.revision;
    if (m_physicalStoreEdit.revision == 0U) {
        m_physicalStoreEdit.revision = 1U;
    }
    m_physicalStoreEdit.generation = authority.generation;
    m_physicalStoreEdit.frame = frame;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((footprint & (UINT64_C(1) << square)) != 0U) {
            m_physicalStoreEdit.square = square;
        }
    }
    m_physicalStoreEdit.mask = mask;
    m_physicalStoreEdit.color = color;
    m_physicalStoreEdit.clearEffects = clearEffects;

    if (m_physicalStoreBatchCount == PHYSICAL_STORE_BATCH_CAPACITY) {
        m_physicalStoreBatchHead = static_cast<uint8_t>(
            (m_physicalStoreBatchHead + 1U) %
            PHYSICAL_STORE_BATCH_CAPACITY);
        --m_physicalStoreBatchCount;
        m_physicalStoreBatchOverflow = true;
    }
    const uint8_t slot = static_cast<uint8_t>(
        (m_physicalStoreBatchHead + m_physicalStoreBatchCount) %
        PHYSICAL_STORE_BATCH_CAPACITY);
    uint8_t* const record = m_physicalStoreBatch.data() +
        slot * PHYSICAL_STORE_BATCH_RECORD_BYTES;
    fxe::WriteU32LE(m_physicalStoreEdit.generation, record);
    record[4U] = m_physicalStoreEdit.frame;
    record[5U] = static_cast<uint8_t>(
        m_physicalStoreEdit.mask |
        (m_physicalStoreEdit.clearEffects ? 0x10U : 0U));
    record[6U] = color.r;
    record[7U] = color.g;
    record[8U] = color.b;
    fxe::WriteU64LE(footprint, record + 9U);
    fxe::WriteU32LE(authority.revision, record + 17U);
    ++m_physicalStoreBatchCount;
}

bool LitchcraftSession::CopyPhysicalStoreEdit(PhysicalStoreEdit& out) const {
    if (m_physicalStoreEdit.revision == 0U) return false;
    out = m_physicalStoreEdit;
    return true;
}

bool LitchcraftSession::PeekPhysicalStoreBatch(
    uint8_t* const out, const size_t capacity, size_t& outSize) {
    constexpr size_t HEADER_BYTES = 13U;
    constexpr size_t BATCH_HEADER_BYTES = 2U;
    outSize = 0U;
    if (m_physicalStoreBatchCount == 0U || out == nullptr ||
        capacity < HEADER_BYTES + BATCH_HEADER_BYTES +
            PHYSICAL_STORE_BATCH_RECORD_BYTES) {
        return false;
    }
    const size_t availableRecords =
        (capacity - HEADER_BYTES - BATCH_HEADER_BYTES) /
        PHYSICAL_STORE_BATCH_RECORD_BYTES;
    const uint8_t count = static_cast<uint8_t>(std::min<size_t>(
        m_physicalStoreBatchCount, availableRecords));
    if (count == 0U) return false;
    const uint32_t firstRevision =
        m_physicalStoreEdit.revision - m_physicalStoreBatchCount + 1U;
    const uint32_t terminalRevision = firstRevision + count - 1U;
    out[0U] = 0x4dU;
    out[1U] = 2U;
    out[2U] = PHYSICAL_STORE_BATCH_EVENT;
    fxe::WriteU32LE(m_session, out + 3U);
    fxe::WriteU32LE(terminalRevision, out + 7U);
    uint16_t request = static_cast<uint16_t>(terminalRevision);
    if (request == 0U) request = 1U;
    fxe::WriteU16LE(request, out + 11U);
    // An inverse may need several records at one live revision. A prefix
    // cannot establish that whole revision even when the ring did not overflow.
    const uint8_t lastSlot = static_cast<uint8_t>(
        (m_physicalStoreBatchHead + count - 1U) % PHYSICAL_STORE_BATCH_CAPACITY);
    const uint8_t nextSlot = static_cast<uint8_t>(
        (m_physicalStoreBatchHead + count) % PHYSICAL_STORE_BATCH_CAPACITY);
    const bool splitRevision = count < m_physicalStoreBatchCount &&
        fxe::ReadU32LE(m_physicalStoreBatch.data() +
            lastSlot * PHYSICAL_STORE_BATCH_RECORD_BYTES + 17U) ==
        fxe::ReadU32LE(m_physicalStoreBatch.data() +
            nextSlot * PHYSICAL_STORE_BATCH_RECORD_BYTES + 17U);
    out[13U] = m_physicalStoreBatchOverflow || splitRevision ? 1U : 0U;
    out[14U] = count;
    for (uint8_t index = 0U; index < count; ++index) {
        const uint8_t slot = static_cast<uint8_t>(
            (m_physicalStoreBatchHead + index) %
            PHYSICAL_STORE_BATCH_CAPACITY);
        std::copy_n(
            m_physicalStoreBatch.data() +
                slot * PHYSICAL_STORE_BATCH_RECORD_BYTES,
            PHYSICAL_STORE_BATCH_RECORD_BYTES,
            out + HEADER_BYTES + BATCH_HEADER_BYTES +
                index * PHYSICAL_STORE_BATCH_RECORD_BYTES);
    }
    m_physicalStoreBatchPeekCount = count;
    m_physicalStoreBatchPeekOverflow = m_physicalStoreBatchOverflow;
    outSize = HEADER_BYTES + BATCH_HEADER_BYTES +
        count * PHYSICAL_STORE_BATCH_RECORD_BYTES;
    return true;
}

void LitchcraftSession::AcceptPhysicalStoreBatch() {
    if (m_physicalStoreBatchPeekCount == 0U ||
        m_physicalStoreBatchPeekCount > m_physicalStoreBatchCount) {
        return;
    }
    m_physicalStoreBatchHead = static_cast<uint8_t>(
        (m_physicalStoreBatchHead + m_physicalStoreBatchPeekCount) %
        PHYSICAL_STORE_BATCH_CAPACITY);
    m_physicalStoreBatchCount = static_cast<uint8_t>(
        m_physicalStoreBatchCount - m_physicalStoreBatchPeekCount);
    if (m_physicalStoreBatchPeekOverflow) {
        m_physicalStoreBatchOverflow = false;
    }
    m_physicalStoreBatchPeekCount = 0U;
    m_physicalStoreBatchPeekOverflow = false;
}

void LitchcraftSession::ResetPhysicalStoreBatch() {
    m_physicalStoreEdit = {};
    m_physicalStoreBatchHead = 0U;
    m_physicalStoreBatchCount = 0U;
    m_physicalStoreBatchPeekCount = 0U;
    m_physicalStoreBatchOverflow = false;
    m_physicalStoreBatchPeekOverflow = false;
}
} // namespace modules
