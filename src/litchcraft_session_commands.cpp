#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"

namespace modules {
LitchcraftSession::Status LitchcraftSession::Apply(const Op op, const uint8_t* payload, const size_t size, const uint16_t request, const bool eligible, const uint64_t occupancy) {
    if (op != Op::Snapshot) CancelBootArtwork();
    if (HasStreamedDocument() && (op == Op::Undo || op == Op::Redo)) {
        if (size != 0U) return Status::Malformed;
        return op == Op::Undo ? ApplyPhysicalStoreHistory() : Status::Unsupported;
    }
    if (HasAdvancedDocument() && (op == Op::Undo || op == Op::Redo)) {
        if (size != 0U) return Status::Malformed;
        return AdvancedUndoRedo(op == Op::Redo);
    }
    if (op == Op::ShakeErase) {
        if (size != 0U) return Status::Malformed;
        if (!m_shakeEraseSupported) return Status::Unsupported;
        if (!m_active) return Status::StaleSession;
        if (!ShakeEraseEligible()) return Status::Busy;
        (void)ApplyShakeErase();
        return Status::Ok;
    }
    if (op == Op::Enter) {
        if (size != 0U) {
            return Status::Malformed;
        }
        AdoptStreamedModeAuthority();
        // Enter is also the client's authority barrier after reconnect. Park
        // any prior V2 transfer immediately, including idempotent Enter while
        // this mode is already active.
        RetireV2Transfer();
        // The physical chord owns entry until release, including cancellation.
        // An app attaching during its rainbow must not commit the mode early.
        if (IsEntryHoldActive()) return Status::EntryPending;
        if (!m_active) {
            // Additive status 8 preserves every existing wire value while
            // letting new apps distinguish physical cleanup from a real game.
            if (occupancy != 0ULL) {
                m_occupiedEntryRejected = true;
                return Status::BoardNotEmpty;
            }
            if (!eligible) {
                return Status::Busy;
            }
            // A fresh mode session must not deliver queued Brush/Tool traffic
            // stamped with the retired session after its Enter response.
            m_queueCount = 0U;
            m_queueHead = 0U;
            m_priorityQueueOffset = 0xffU;
            m_snapshotActive = false;
            m_v2PendingRequest = 0U;
            ResetPhysicalStoreBatch();
            m_active = true;
            if (HasStreamedDocument()) ResetStreamedPlayback();
            else if (HasAdvancedDocument()) ResetAdvancedPlayback();
            MarkModeChanged();
            m_session = ++m_nonce;
            if (m_session == 0U) {
                m_session = ++m_nonce;
            }
            m_occupancy = occupancy;
            m_blockedOccupancy = occupancy;
            m_exited = false;
            ++m_revision;
            m_activity = true;
            WakeStreamedStorage();
        }
        m_canvasInvalidated = true;
        return Status::Ok;
    }
    if (op == Op::Snapshot) {
        if (size > 1U || (size == 1U && payload[0] != 1U)) {
            return Status::Malformed;
        }
        StartSnapshot(request, size == 1U);
        return Status::Ok;
    }
    if (op == Op::Inspect) {
        if (size != 0U) {
            return Status::Malformed;
        }
        // A reconnecting capability-aware client uses Inspect as its new
        // notification baseline. Let that authoritative State through even if
        // an older incremental queue overflowed while notifications were
        // unavailable; its revision will make the client request a snapshot.
        m_resyncRequired = false;
        m_resyncSent = false;
        m_queueCount = 0U;
        m_queueHead = 0U;
        m_priorityQueueOffset = 0xffU;
        return Status::Ok;
    }
    if (!m_active) {
        return Status::StaleSession;
    }
    if (op == Op::Tool) {
        if (size != 5U || payload[0] > 1U || payload[1] == 0U || payload[1] > 15U) {
            return Status::Malformed;
        }
        if (StoreIsSoleArtworkOwner() || HasStreamedDocument() ||
            HasAdvancedDocument()) {
            // The additive compact Tool packet selects session paint state.
            // Advanced artwork/favorites/history remain owned by v2 edits;
            // selection alone cannot repaint a resting brush or flatten art.
            EndStroke();
            SelectTool(payload[0] != 0U, payload[1],
                       {payload[2], payload[3], payload[4]}, request, false);
        } else {
            ChangeTool(payload[0] != 0U, payload[1],
                       {payload[2], payload[3], payload[4]}, request);
        }
        return Status::Ok;
    }
    if (op == Op::Paint) {
        if (size < 3U || size > 12U || payload[2] > 3U || Read16(payload) == 0U) {
            return Status::Malformed;
        }
        for (size_t i = 3U; i < size; ++i) {
            if (payload[i] >= 64U) {
                return Status::Malformed;
            }
        }
        m_activity = true;
        if ((payload[2] & 1U) != 0U) {
            EndStroke();
        }
        StartStroke(Source::App, Read16(payload));
        for (size_t i = 3U; i < size; ++i) {
            PaintSquare(payload[i], request);
        }
        if ((payload[2] & 2U) != 0U) {
            EndStroke();
        }
        return Status::Ok;
    }
    if (op == Op::PaletteChunk) {
        return ApplyPaletteChunk(payload, size);
    }
    if (op == Op::ReplaceChunk) {
        if (!m_replacing) {
            return Status::Incomplete;
        }
        if (size < 5U || size > 11U || (size - 2U) % 3U != 0U) {
            return Status::Malformed;
        }
        const uint16_t offset = Read16(payload);
        const size_t count = (size - 2U) / 3U;
        if (offset + count > ELEMENT_COUNT) {
            return Status::Malformed;
        }
        for (size_t i = 0U; i < count; ++i) {
            const auto& old = m_replacement[offset + i];
            const Rgb next{payload[2U + i * 3U], payload[3U + i * 3U], payload[4U + i * 3U]};
            if (m_received[offset + i] && (old.r != next.r || old.g != next.g || old.b != next.b)) {
                return Status::Malformed;
            }
        }
        for (size_t i = 0U; i < count; ++i) {
            m_replacement[offset + i] = {payload[2U + i * 3U], payload[3U + i * 3U], payload[4U + i * 3U]};
            m_received[offset + i] = true;
        }
        m_replaceAtMs = fxe::Millis();
        return Status::Ok;
    }
    if (size != 0U) {
        return Status::Malformed;
    }
    switch (op) {
        case Op::Exit:
            Exit();
            return Status::Ok;
        case Op::Undo:
        case Op::Redo: {
#if defined(LITCH_ESP32S3)
            return Status::Unsupported;
#else
            EndStroke();
            const Canvas previous = m_canvas;
            const bool applied = op == Op::Undo
                ? m_history.Undo(m_canvas)
                : m_history.Redo(m_canvas);
            if (!applied) return Status::NoHistory;
            QueueCanvasChanges(previous, request);
            m_activity = true;
            return Status::Ok;
#endif
        }
        case Op::Clear:
            EndStroke();
            StartStroke(Source::App);
            {
            const Canvas previous = m_canvas;
            m_canvas.fill(RGB_BLACK);
            QueueCanvasChanges(previous, request);
            m_strokeChanged = !std::equal(previous.begin(), previous.end(), m_canvas.begin(),
                [](const Rgb a, const Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; });
            EndStroke();
            }
            m_activity = true;
            return Status::Ok;
        case Op::ReplaceBegin:
            m_received.reset();
            m_replacing = true;
            m_replaceAtMs = fxe::Millis();
            return Status::Ok;
        case Op::ReplaceCommit:
            if (!m_replacing || !m_received.all()) {
                return Status::Incomplete;
            }
            EndStroke();
            StartStroke(Source::App);
            m_strokeChanged = ReplaceCanvas(m_replacement);
            EndStroke();
            ++m_revision;
            m_replacing = false;
            m_activity = true;
            return Status::Ok;
        default:
            return Status::Unsupported;
    }
}
} // namespace modules
