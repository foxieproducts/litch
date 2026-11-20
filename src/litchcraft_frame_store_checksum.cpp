#include "modules/litchcraft_frame_store.hpp"

#include <new>

#include "modules/litchcraft_fixed_slot_wire_view.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"

namespace modules::litchcraft_frame_store {
namespace {
namespace fixed = litchcraft_frame_store_layout;
namespace view = litchcraft_fixed_slot_wire_view;

bool SameIdentity(const FixedRoot& expected, const FixedRoot& actual) {
    return expected.bodyBytes == actual.bodyBytes &&
        expected.bodyCrc32 == actual.bodyCrc32 &&
        expected.blockBytes == actual.blockBytes &&
        expected.blockCrc32 == actual.blockCrc32;
}

Status ReadResult(const fixed::Status status) {
    return status == fixed::Status::Ok ? Status::Ok :
        (status == fixed::Status::IoFailure ? Status::IoFailure : Status::Corrupt);
}
} // namespace

FrameStore::IngressBody& FrameStore::IngressBodyBuffer() {
    if (!m_ingressWorkspace) {
        new (&m_workspace.ingress) IngressBody{};
        m_ingressWorkspace = true;
        m_wireSummariesReady = false;
    }
    return m_workspace.ingress;
}

WireSummaries& FrameStore::SummaryWorkspace() {
    if (m_ingressWorkspace) {
        new (&m_workspace.summaries) WireSummaries{};
        m_ingressWorkspace = false;
        m_wireSummariesReady = false;
    }
    return m_workspace.summaries;
}

bool FrameStore::LoadWireSummaries() {
    return LoadBoundWireSummaries() == Status::Ok;
}

Status FrameStore::LoadBoundWireSummaries() {
    SummaryWorkspace();
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    const auto read = storage.ReadWireSummaries(m_currentRoot, m_currentCopy,
                                                m_workspace.summaries);
    if (read != fixed::Status::Ok) {
        return read == fixed::Status::Blank || read == fixed::Status::IoFailure
            ? Status::IoFailure : Status::Corrupt;
    }
    if (m_currentRoot.manifest.predecessorDeltas) {
        FixedRoot identity = m_currentRoot;
        if (view::WireView::CombineSummaries(m_workspace.summaries, identity) !=
                fixed::Status::Ok || !SameIdentity(m_currentRoot, identity)) {
            return Status::Corrupt;
        }
    }
    m_wireSummariesReady = true;
    return Status::Ok;
}

Status FrameStore::EnsureWireSummaries(const bool allowRebuild,
    bool (*continueWork)(void*), void* context) {
    if (m_wireSummariesReady || LoadWireSummaries()) return Status::Ok;
    if (!allowRebuild) return Status::Incomplete;
    const Status measured = MeasureWireSummaries(m_currentRoot,
                                                 m_workspace.summaries, continueWork, context);
    if (measured != Status::Ok) return measured;
    if (continueWork != nullptr && !continueWork(context)) return Status::Incomplete;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    const Status written = ReadResult(storage.WriteWireSummaries(
        m_currentRoot, m_currentCopy, m_workspace.summaries));
    if (written == Status::Ok) m_wireSummariesReady = true;
    return written;
}

Status FrameStore::MeasureWireSummaries(const FixedRoot& root,
                                       WireSummaries& out,
                                       bool (*continueWork)(void*), void* context) {
    struct Progress {
        bool (*proceed)(void*);
        void* context;
        bool stopped = false;
    } progress{continueWork, context};
    const auto proceed = [](void* p) {
        auto& state = *static_cast<Progress*>(p);
        if (state.proceed == nullptr || state.proceed(state.context)) return true;
        state.stopped = true;
        return false;
    };
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    FixedRoot measuredRoot = root;
    FixedRoot identity{};
    if (!root.manifest.predecessorDeltas) {
        // Rescue/legacy roots keep their published identity. Validate it once
        // before deriving adjacent-predecessor summaries for the next edit.
        view::WireView legacy(storage, root, m_wireRecord.data(),
            m_wireRecord.size(), m_incomingPreviousFrame, m_decodeCachedFrame);
        const Status status = ReadResult(legacy.MeasureBlocks(identity, proceed, &progress));
        if (progress.stopped) return Status::Incomplete;
        if (status != Status::Ok) return status;
        if (!SameIdentity(root, identity)) return Status::Corrupt;
        measuredRoot.manifest.predecessorDeltas = true;
    }
    view::WireView wire(storage, measuredRoot, m_wireRecord.data(),
        m_wireRecord.size(), m_incomingPreviousFrame, m_decodeCachedFrame);
    const Status status = ReadResult(wire.MeasureSummaries(out, identity, proceed, &progress));
    if (progress.stopped) return Status::Incomplete;
    if (status != Status::Ok) return status;
    return !root.manifest.predecessorDeltas || SameIdentity(root, identity)
        ? Status::Ok : Status::Corrupt;
}

} // namespace modules::litchcraft_frame_store
