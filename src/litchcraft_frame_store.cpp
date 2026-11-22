#include "modules/litchcraft_frame_store.hpp"

#include <algorithm>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_fixed_slot_wire_view.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace modules::litchcraft_frame_store {
namespace {
namespace fixed = litchcraft_frame_store_layout;
namespace view = litchcraft_fixed_slot_wire_view;
constexpr size_t IO_PAGE_BYTES = 512U;

Status Map(const fixed::Status value) {
    switch (value) {
    case fixed::Status::Ok: return Status::Ok;
    case fixed::Status::Blank: return Status::NoArtwork;
    case fixed::Status::Corrupt: return Status::Corrupt;
    case fixed::Status::PrelaunchLC5: return Status::PrelaunchLC5;
    case fixed::Status::IoFailure: return Status::IoFailure;
    case fixed::Status::RevisionConflict: return Status::RevisionConflict;
    case fixed::Status::StageInUse: return Status::Incomplete;
    case fixed::Status::InvalidArgument: return Status::InvalidArgument;
    }
    return Status::Corrupt;
}

bool ValidManifest(const Manifest& value) {
    return value.frameCount >= 1U && value.frameCount <= FRAME_COUNT &&
           value.topology >= 1U && value.topology <= 3U &&
           value.sparkleSeed != 0U;
}

uint32_t UpdateCrc32(uint32_t state, const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        state ^= bytes[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (state & 1U) != 0U ? 0xedb88320U : 0U;
            state = (state >> 1U) ^ mask;
        }
    }
    return state;
}
}

bool FrameStore::IsConfigured() const {
    return m_ops.read != nullptr && m_ops.write != nullptr &&
           m_ops.sync != nullptr;
}

bool FrameStore::Newer(uint32_t left, uint32_t right) {
    return static_cast<int32_t>(left - right) > 0;
}

bool FrameStore::SameToken(const TransactionToken& left,
                           const TransactionToken& right) {
    return left.generation == right.generation && left.nonce == right.nonce &&
           left.ownerEpoch == right.ownerEpoch;
}

bool FrameStore::Owns(const TransactionToken& token) const {
    return m_transactionActive && SameToken(token, m_activeToken);
}

void FrameStore::ReportTiming(TimingPhase phase, uint32_t elapsedMs) const {
    if (m_timingObserver != nullptr) {
        m_timingObserver(m_timingContext, phase, elapsedMs);
    }
}

void FrameStore::ResetParser() {
    m_bodyCursor = 0U;
    m_bodyCrcState = 0xffffffffU;
    m_frameCrcState = 0xffffffffU;
    m_blockCrcState = 0xffffffffU;
    m_recordHeaderRead = 0U;
    m_recordLength = 0U;
    m_recordPayloadRead = 0U;
    m_parsedFrames = 0U;
}

void FrameStore::ResetWireCursor() {
    ++m_pageEpoch;
    m_wireGeneration = 0U;
    m_wireOffset = 0U;
    m_wireRecordSize = 0U;
    m_wireRecordRead = 0U;
    m_wireNextFrame = 0U;
    m_wireFrameIndex = FRAME_COUNT;
}

Status FrameStore::Recover(const RecoveryPurpose purpose) {
    if (!IsConfigured()) return Status::NotConfigured;
    if (m_transactionActive) m_lastAbortedToken = m_activeToken;
    m_transactionActive = false;
    m_commitPrepared = false;
    m_transactionKind = TransactionKind::None;
    m_wireBodyMode = WireBodyMode::Legacy;
    m_activeToken = {};
    m_hasCurrent = false;
    m_hasAlternate = false;
    m_wireSummariesReady = false;
    m_currentRoot = {};
    m_alternateRoot = {};
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    FixedRoot roots[2]{};
    uint8_t copies[2]{};
    const fixed::Status states[2]{storage.LatestRoot(0U, roots[0], copies[0]),
                                  storage.LatestRoot(1U, roots[1], copies[1])};
    if (states[0] == fixed::Status::IoFailure ||
        states[1] == fixed::Status::IoFailure) return Status::IoFailure;
    const bool present[2]{states[0] == fixed::Status::Ok,
                          states[1] == fixed::Status::Ok};
    if (present[0] || present[1]) {
        const uint8_t newest = present[0] && present[1]
            ? (Newer(roots[1].generation, roots[0].generation) ? 1U : 0U)
            : (present[1] ? 1U : 0U);
        if (roots[newest].retired) {
            m_currentRoot = roots[newest];
            m_currentCopy = copies[newest];
            return Status::NoArtwork;
        }
    }
    const bool valid[2]{states[0] == fixed::Status::Ok && !roots[0].retired,
                        states[1] == fixed::Status::Ok && !roots[1].retired};
    if (!valid[0] && !valid[1]) {
        if ((states[0] == fixed::Status::Blank || states[0] == fixed::Status::PrelaunchLC5) &&
            (states[1] == fixed::Status::Blank || states[1] == fixed::Status::PrelaunchLC5) &&
            (states[0] == fixed::Status::PrelaunchLC5 || states[1] == fixed::Status::PrelaunchLC5)) {
            return Status::PrelaunchLC5;
        }
        return states[0] == fixed::Status::Blank &&
                       states[1] == fixed::Status::Blank
            ? Status::NoArtwork : Status::Corrupt;
    }
    const uint8_t selected = valid[0] && valid[1]
        ? (Newer(roots[1].generation, roots[0].generation) ? 1U : 0U)
        : (valid[1] ? 1U : 0U);
    m_currentRoot = roots[selected];
    m_currentCopy = copies[selected];
    const uint8_t other = selected ^ 1U;
    if (valid[other]) {
        m_alternateRoot = roots[other];
        m_alternateCopy = copies[other];
        m_hasAlternate = true;
    }
    m_hasCurrent = true;
    // Prepare derived evidence on the storage worker before edit readiness;
    // cache failure cannot change which valid artwork root was recovered.
    if (purpose == RecoveryPurpose::EditReady) (void)EnsureWireSummaries();
    ++m_pageEpoch;
    ResetWireCursor();
    return Status::Ok;
}

uint8_t FrameStore::ResidentCount() const {
    return static_cast<uint8_t>((m_hasCurrent ? 1U : 0U) +
                                (m_hasAlternate ? 1U : 0U));
}

bool FrameStore::CurrentResident(ResidentDescriptor& out) const {
    out = {};
    if (!m_hasCurrent) return false;
    out = {m_currentRoot.manifest, m_currentRoot.generation,
           m_currentRoot.bodyBytes, m_currentRoot.bodyCrc32,
           m_currentRoot.resident};
    return true;
}

bool FrameStore::Resident(uint8_t rootSlot, ResidentDescriptor& out) const {
    out = {};
    if (m_hasCurrent && rootSlot == m_currentRoot.resident) {
        return CurrentResident(out);
    }
    if (!m_hasAlternate || rootSlot != m_alternateRoot.resident) return false;
    out = {m_alternateRoot.manifest, m_alternateRoot.generation,
           m_alternateRoot.bodyBytes, m_alternateRoot.bodyCrc32,
           m_alternateRoot.resident};
    return true;
}

void FrameStore::SwapResidentAuthority() {
    std::swap(m_currentRoot, m_alternateRoot);
    std::swap(m_currentCopy, m_alternateCopy);
    m_wireSummariesReady = false;
    ++m_pageEpoch;
    ResetWireCursor();
}

Status FrameStore::SelectResident(uint8_t rootSlot,
                                  uint32_t expectedGeneration,
                                  uint32_t expectedBodyCrc32) {
    if (rootSlot > 1U || expectedGeneration == 0U) {
        return Status::InvalidArgument;
    }
    if (m_transactionActive) return Status::Incomplete;
    if (!m_hasCurrent) return Status::NoArtwork;
    const FixedRoot* target = rootSlot == m_currentRoot.resident
        ? &m_currentRoot : (m_hasAlternate &&
          rootSlot == m_alternateRoot.resident ? &m_alternateRoot : nullptr);
    if (target == nullptr) return Status::NoArtwork;
    if (target->generation != expectedGeneration ||
        target->bodyCrc32 != expectedBodyCrc32) return Status::RevisionConflict;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    const Status first = Map(storage.ReadFrame(*target, 0U,
                                               m_decodeCachedFrame));
    if (first != Status::Ok) return first;
    if (target == &m_alternateRoot) SwapResidentAuthority();
    return Status::Ok;
}

bool FrameStore::UndoResidentSelection(const ResidentDescriptor& previous) {
    if (!m_hasAlternate || m_transactionActive ||
        previous.rootSlot != m_alternateRoot.resident ||
        previous.generation != m_alternateRoot.generation ||
        previous.bodyCrc32 != m_alternateRoot.bodyCrc32) return false;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    if (storage.ReadFrame(m_alternateRoot, 0U,
                          m_decodeCachedFrame) != fixed::Status::Ok) return false;
    SwapResidentAuthority();
    return true;
}

Status FrameStore::Retire(const Ops& ops, uint32_t retiredGeneration,
                          uint32_t* resolvedGeneration) {
    if (resolvedGeneration != nullptr) *resolvedGeneration = retiredGeneration;
    if (ops.read == nullptr || ops.write == nullptr || ops.sync == nullptr) {
        return Status::NotConfigured;
    }
    std::array<uint8_t, fixed::FRAME_SLOT_BYTES> scratch{};
    fixed::Layout storage(ops, scratch.data(), scratch.size());
    FixedRoot roots[2]{};
    uint8_t copies[2]{};
    const fixed::Status states[2]{storage.LatestRoot(0U, roots[0], copies[0]),
                                  storage.LatestRoot(1U, roots[1], copies[1])};
    if (states[0] == fixed::Status::IoFailure ||
        states[1] == fixed::Status::IoFailure) return Status::IoFailure;
    const bool valid[2]{states[0] == fixed::Status::Ok,
                        states[1] == fixed::Status::Ok};
    if (!valid[0] && !valid[1]) {
        return states[0] == fixed::Status::Blank &&
                       states[1] == fixed::Status::Blank
            ? Status::Ok : Status::Corrupt;
    }
    const uint8_t selected = valid[0] && valid[1]
        ? (Newer(roots[1].generation, roots[0].generation) ? 1U : 0U)
        : (valid[1] ? 1U : 0U);
    FixedRoot root = roots[selected];
    if (retiredGeneration == 0U && resolvedGeneration != nullptr) {
        *resolvedGeneration = root.generation;
    }
    if (root.retired || (retiredGeneration != 0U &&
                         Newer(root.generation, retiredGeneration))) {
        return Status::Ok;
    }
    root.retired = true;
    ++root.generation;
    ++root.serial;
    if (root.generation == 0U) ++root.generation;
    if (root.serial == 0U) ++root.serial;
    return Map(storage.PublishRoot(root, copies[selected] ^ 1U));
}

Status FrameStore::Clear(uint32_t minimumGeneration) {
    if (!IsConfigured()) return Status::NotConfigured;
    if (m_transactionActive || m_commitPrepared) return Status::Incomplete;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    uint32_t latestGeneration = minimumGeneration;
    const uint32_t memoryGenerations[2]{m_currentRoot.generation,
                                       m_alternateRoot.generation};
    for (const uint32_t generation : memoryGenerations) {
        if (generation != 0U &&
            (latestGeneration == 0U || Newer(generation, latestGeneration))) {
            latestGeneration = generation;
        }
    }
    uint32_t diskGenerations[2]{};
    uint32_t selectedGeneration = 0U;
    uint8_t selectedResident = 0U;
    uint8_t selectedCopy = 0U;
    bool hasRoot = false;
    for (uint8_t resident = 0U; resident < 2U; ++resident) {
        uint8_t copy = 0U;
        const fixed::Status state =
            storage.LatestRoot(resident, m_incomingRoot, copy);
        if (state == fixed::Status::IoFailure) return Status::IoFailure;
        if (state != fixed::Status::Ok) continue;
        const uint32_t generation = m_incomingRoot.generation;
        diskGenerations[resident] = generation;
        if (latestGeneration == 0U || Newer(generation, latestGeneration)) {
            latestGeneration = generation;
        }
        if (!hasRoot || Newer(generation, selectedGeneration)) {
            hasRoot = true;
            selectedGeneration = generation;
            selectedResident = resident;
            selectedCopy = copy;
        }
    }
    uint32_t nextGeneration = latestGeneration + 1U;
    if (nextGeneration == 0U) ++nextGeneration;
    // Serial-number ordering cannot satisfy mutually ambiguous fences.
    for (const uint32_t generation : memoryGenerations) {
        if (generation != 0U && !Newer(nextGeneration, generation)) {
            return Status::RevisionConflict;
        }
    }
    for (const uint32_t generation : diskGenerations) {
        if (generation != 0U && !Newer(nextGeneration, generation)) {
            return Status::RevisionConflict;
        }
    }
    if (minimumGeneration != 0U &&
        !Newer(nextGeneration, minimumGeneration)) return Status::RevisionConflict;
    if (hasRoot) {
        const Status read = Map(storage.ReadRoot(selectedResident, selectedCopy,
                                                 m_incomingRoot));
        if (read != Status::Ok) return read;
        selectedCopy ^= 1U;
    } else {
        m_incomingRoot = {};
        m_incomingRoot.bodyBytes = WIRE_RECORD_HEADER_BYTES;
    }
    m_incomingRoot.retired = true;
    m_incomingRoot.generation = nextGeneration;
    ++m_incomingRoot.serial;
    if (m_incomingRoot.serial == 0U) ++m_incomingRoot.serial;
    const uint32_t startedMs = fxe::Millis();
    const Status published = Map(storage.PublishRoot(m_incomingRoot, selectedCopy));
    ReportTiming(TimingPhase::RootPublish, fxe::Millis() - startedMs);
    if (published != Status::Ok) return published;
    m_currentRoot = m_incomingRoot;
    m_currentCopy = selectedCopy;
    m_hasCurrent = false;
    m_alternateRoot = {};
    m_alternateCopy = 0U;
    m_hasAlternate = false;
    m_incomingRoot = {};
    m_incomingCopy = 0U;
    m_incomingRootNeedsInvalidation = false;
    m_incomingRootTouched = false;
    m_editHasFrame = false;
    m_editStructural = false;
    m_editIndex = FRAME_COUNT;
    m_expectedBodyBytes = 0U;
    m_expectedBodyCrc = 0U;
    m_decodeCachedFrame.durationMs = 1000U;
    m_decodeCachedFrame.emitters.fill({});
    ResetParser();
    ++m_pageEpoch;
    ResetWireCursor();
    return Status::Ok;
}

FrameStore::Ops FrameStore::EditStorageOps() {
    return {this,
        [](void* context, size_t offset, uint8_t* bytes, size_t size) {
            auto& self = *static_cast<FrameStore*>(context);
            const bool ok = self.m_ops.read(self.m_ops.context, offset, bytes, size);
            if (ok) self.ServiceEditPaging();
            return ok;
        },
        [](void* context, size_t offset, const uint8_t* bytes, size_t size) {
            auto& self = *static_cast<FrameStore*>(context);
            const bool ok = self.m_ops.write(self.m_ops.context, offset, bytes, size);
            if (ok) self.ServiceEditPaging();
            return ok;
        },
        [](void* context) {
            auto& self = *static_cast<FrameStore*>(context);
            const bool ok = self.m_ops.sync(self.m_ops.context);
            if (ok) self.ServiceEditPaging();
            return ok;
        }};
}

void FrameStore::ServiceEditPaging() {
    if (m_editPagingCallback == nullptr || m_editPaging || !m_hasCurrent) return;
    // The file adapter has released its flash lease. The edit retains slot
    // scratch; the same pager temporarily borrows the otherwise idle wire
    // record and decode frame. Paging uses original Ops and cannot recurse.
    m_editPaging = true;
    m_editPagingCallback(m_editPagingContext, *this);
    m_editPaging = false;
}

Status FrameStore::StartTransaction(
    const Manifest& manifest, uint32_t expectedGeneration,
    uint32_t ownerEpoch, TransactionKind kind, uint32_t bodyBytes,
    uint32_t bodyCrc, TransactionToken& outToken) {
    outToken = {};
    if (!IsConfigured()) return Status::NotConfigured;
    if (!ValidManifest(manifest) || ownerEpoch == 0U ||
        kind == TransactionKind::None ||
        (kind == TransactionKind::Edit && !m_hasCurrent) ||
        (kind == TransactionKind::Wire &&
         (bodyBytes < WIRE_RECORD_HEADER_BYTES ||
          bodyBytes > WIRE_BODY_MAX_BYTES))) return Status::InvalidArgument;
    if (m_transactionActive) return Status::Incomplete;
    if (expectedGeneration != Generation()) return Status::RevisionConflict;
    fixed::Layout storage(kind == TransactionKind::Edit ? EditStorageOps() : m_ops,
                          m_slotScratch.data(), m_slotScratch.size());
    if (kind == TransactionKind::Edit) {
        const Status ready = EnsureWireSummaries();
        if (ready != Status::Ok) return ready;
    }
    if (kind == TransactionKind::Edit &&
        m_currentRoot.stageIndex != fixed::NO_STAGE) {
        FixedRoot folded{};
        const Status fold = Map(storage.FoldStage(
            m_currentRoot, m_currentCopy, m_editFrame, folded));
        if (fold != Status::Ok) return fold;
        m_currentRoot = folded;
        m_currentCopy ^= 1U;
        ResetWireCursor();
    }
    if (kind == TransactionKind::Edit && m_hasAlternate &&
        m_alternateRoot.stageIndex != fixed::NO_STAGE) {
        // A replaced document may leave its older resident on the shared
        // stage slot. Fold that one frame before the new document edits it.
        FixedRoot folded{};
        const Status fold = Map(storage.FoldStage(
            m_alternateRoot, m_alternateCopy, m_editFrame, folded));
        if (fold != Status::Ok) return fold;
        m_alternateRoot = folded;
        m_alternateCopy ^= 1U;
        ResetWireCursor();
    }
    uint32_t latest = m_currentRoot.generation;
    if (m_hasAlternate &&
        Newer(m_alternateRoot.generation, latest)) {
        latest = m_alternateRoot.generation;
    }
    uint32_t next = latest + 1U;
    if (next == 0U) ++next;
    m_incomingRoot = kind == TransactionKind::Edit ? m_currentRoot : FixedRoot{};
    m_incomingRoot.manifest = manifest;
    if (kind == TransactionKind::Edit) {
        m_incomingRoot.manifest.predecessorDeltas = true;
        m_summaryEdits.fill(false);
    }
    m_incomingRoot.resident = kind == TransactionKind::Edit
        ? m_currentRoot.resident :
          (m_hasCurrent || m_currentRoot.retired
               ? uint8_t(m_currentRoot.resident ^ 1U) : 0U);
    m_incomingRoot.generation = next;
    m_incomingRoot.serial = kind == TransactionKind::Edit
        ? m_currentRoot.serial + 1U : 1U;
    if (m_incomingRoot.serial == 0U) return Status::InvalidArgument;
    m_incomingRoot.stageIndex = fixed::NO_STAGE;
    m_incomingRoot.retired = false;
    m_incomingCopy = kind == TransactionKind::Edit
        ? uint8_t(m_currentCopy ^ 1U) : 0U;
    m_expectedBodyBytes = bodyBytes;
    m_expectedBodyCrc = bodyCrc;
    m_incomingRootNeedsInvalidation = kind != TransactionKind::Edit;
    m_incomingRootTouched = false;
    m_editHasFrame = false;
    m_editStructural = false;
    m_editIndex = FRAME_COUNT;
    ResetParser();
    ++m_pageEpoch;
    ResetWireCursor();
    if (++m_nextNonce == 0U) ++m_nextNonce;
    m_activeToken = {next, m_nextNonce, ownerEpoch};
    m_wireSummariesReady = false;
    m_transactionKind = kind;
    m_wireBodyMode = WireBodyMode::Legacy;
    m_transactionActive = true;
    m_commitPrepared = false;
    outToken = m_activeToken;
    return Status::Ok;
}

Status FrameStore::PrepareIncomingBodyWrite() {
    if (!m_incomingRootNeedsInvalidation) return Status::Ok;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    m_incomingRootTouched = true;
    for (uint8_t copy = 0U; copy < 2U; ++copy) {
        const Status invalidated = Map(storage.InvalidateRoot(
            m_incomingRoot.resident, copy));
        if (invalidated != Status::Ok) return invalidated;
    }
    if (m_hasAlternate &&
        m_alternateRoot.resident == m_incomingRoot.resident) {
        m_hasAlternate = false;
        m_alternateRoot = {};
    }
    if (m_ops.prepareReplacementBank != nullptr &&
        !m_ops.prepareReplacementBank(m_ops.context, m_incomingRoot.resident)) {
        return Status::IoFailure;
    }
    m_incomingRootNeedsInvalidation = false;
    return Status::Ok;
}

Status FrameStore::Begin(const Manifest& manifest,
                         uint32_t expectedGeneration, uint32_t ownerEpoch,
                         TransactionToken& outToken) {
    return StartTransaction(manifest, expectedGeneration, ownerEpoch,
                            TransactionKind::Generated, 0U, 0U, outToken);
}

Status FrameStore::BeginWire(const Manifest& manifest,
                             uint32_t expectedGeneration, uint32_t ownerEpoch,
                             uint32_t bodyBytes, uint32_t bodyCrc32,
                             TransactionToken& outToken,
                             const WireBodyMode mode) {
    const Status status = StartTransaction(
        manifest, expectedGeneration, ownerEpoch, TransactionKind::Wire,
        bodyBytes, bodyCrc32, outToken);
    if (status == Status::Ok) m_wireBodyMode = mode;
    return status;
}

Status FrameStore::BeginEdit(uint32_t expectedGeneration, uint32_t ownerEpoch,
                             TransactionToken& outToken) {
    const uint32_t startedMs = fxe::Millis();
    const Status status = StartTransaction(CurrentManifest(), expectedGeneration,
        ownerEpoch, TransactionKind::Edit, CurrentBodyBytes(),
        CurrentBodyCrc32(), outToken);
    ReportTiming(TimingPhase::EditBegin, fxe::Millis() - startedMs);
    return status;
}

Status FrameStore::UpdateModeActive(const bool active,
                                    const uint32_t expectedGeneration) {
    if (!IsConfigured() || !m_hasCurrent) return Status::NoArtwork;
    if (m_transactionActive) return Status::Incomplete;
    if (expectedGeneration != Generation()) return Status::RevisionConflict;
    if (m_currentRoot.manifest.active == active) return Status::Ok;
    FixedRoot next = m_currentRoot;
    uint32_t latest = m_currentRoot.generation;
    if (m_hasAlternate && Newer(m_alternateRoot.generation, latest)) {
        latest = m_alternateRoot.generation;
    }
    next.generation = latest + 1U;
    if (next.generation == 0U) ++next.generation;
    ++next.serial;
    if (next.serial == 0U) return Status::InvalidArgument;
    next.manifest.active = active;
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    const uint8_t copy = m_currentCopy ^ 1U;
    const Status published = Map(storage.PublishRoot(next, copy));
    if (published != Status::Ok) return published;
    m_currentRoot = next;
    m_currentCopy = copy;
    ++m_pageEpoch;
    ResetWireCursor();
    return Status::Ok;
}

Status FrameStore::FinishRecord() {
    if (m_parsedFrames >= m_incomingRoot.manifest.frameCount ||
        (~m_frameCrcState) != fxe::ReadU32LE(m_recordHeader.data() + 2U)) {
        return Status::Corrupt;
    }
    // Selection/read scratch is transient during a transaction. Its readers
    // decode the durable root again; keep predecessor and frame-zero preview
    // owners separate so this does not put another Frame on the worker stack.
    Frame& decoded = m_decodeCachedFrame;
    const Frame* previous = m_parsedFrames == 0U
        ? nullptr : &m_incomingPreviousFrame;
    const auto codecMode = m_wireBodyMode == WireBodyMode::Rgb
        ? litchcraft_wire_codec::RgbOnlyMode::Enabled
        : litchcraft_wire_codec::RgbOnlyMode::Disabled;
    if (!litchcraft_wire_codec::FrameCodec::Decode(
            previous, m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES,
            m_recordLength, decoded, codecMode)) {
        return Status::Corrupt;
    }
    // Canonical bytes are consumed before WriteSlot reuses this owned scratch.
    auto& canonical = m_slotScratch;
    size_t canonicalSize = 0U;
    const Frame* canonicalPrevious = m_parsedFrames == 0U ||
            (m_incomingRoot.manifest.predecessorDeltas &&
             m_parsedFrames % KEYFRAME_INTERVAL == 0U)
        ? nullptr : &m_incomingPreviousFrame;
    if (!litchcraft_wire_codec::FrameCodec::Encode(
            canonicalPrevious, decoded, canonical.data(), canonical.size(),
            canonicalSize, codecMode) || canonicalSize != m_recordLength ||
        std::memcmp(canonical.data(),
                    m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES,
                    canonicalSize) != 0) {
        return Status::Corrupt;
    }
    if (m_wireBodyMode == WireBodyMode::Rgb) {
        if (!litchcraft_wire_codec::FrameCodec::Encode(
                canonicalPrevious, decoded, canonical.data(),
                canonical.size(), canonicalSize)) return Status::Corrupt;
        std::array<uint8_t, WIRE_RECORD_HEADER_BYTES> legacyHeader{};
        fxe::WriteU16LE(static_cast<uint16_t>(canonicalSize),
                        legacyHeader.data());
        fxe::WriteU32LE(fxe::Crc32(canonical.data(), canonicalSize),
                        legacyHeader.data() + 2U);
        m_blockCrcState = UpdateCrc32(
            m_blockCrcState, legacyHeader.data(), legacyHeader.size());
        m_blockCrcState = UpdateCrc32(
            m_blockCrcState, canonical.data(), canonicalSize);
        m_incomingRoot.blockBytes[m_parsedFrames / KEYFRAME_INTERVAL] +=
            static_cast<uint32_t>(legacyHeader.size() + canonicalSize);
    }
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    const uint32_t startedMs = fxe::Millis();
    const uint8_t slot = static_cast<uint8_t>(m_parsedFrames);
    const Status written = Map(m_transactionKind == TransactionKind::Wire
        ? storage.StageIncomingSlot(m_incomingRoot.resident, slot, decoded)
        : storage.WriteSlot(m_incomingRoot.resident, slot, decoded));
    ReportTiming(TimingPhase::BodyWrite, fxe::Millis() - startedMs);
    if (written != Status::Ok) return written;
    m_incomingRoot.slots[m_parsedFrames] =
        static_cast<uint8_t>(m_parsedFrames);
    if (m_parsedFrames == 0U) m_editFrame = decoded;
    if (m_parsedFrames == 0U || m_incomingRoot.manifest.predecessorDeltas) {
        m_incomingPreviousFrame = decoded;
    }
    const uint16_t completed = ++m_parsedFrames;
    if (completed % KEYFRAME_INTERVAL == 0U ||
        completed == m_incomingRoot.manifest.frameCount) {
        m_incomingRoot.blockCrc32[(completed - 1U) / KEYFRAME_INTERVAL] =
            ~m_blockCrcState;
        m_blockCrcState = 0xffffffffU;
    }
    m_recordHeaderRead = 0U;
    m_recordLength = 0U;
    m_recordPayloadRead = 0U;
    m_frameCrcState = 0xffffffffU;
    return Status::Ok;
}

Status FrameStore::ParseWireBytes(const uint8_t* bytes, size_t size) {
    size_t consumed = 0U;
    uint16_t frameBudget = FRAME_COUNT;
    const Status status = ParseWireBytes(bytes, size, consumed, frameBudget);
    return status == Status::Ok && consumed != size ? Status::Corrupt : status;
}

Status FrameStore::ParseWireBytes(const uint8_t* bytes, size_t size,
                                  size_t& consumed, uint16_t& frameBudget) {
    consumed = 0U;
    if (bytes == nullptr || size == 0U) return Status::InvalidArgument;
    size_t& source = consumed;
    while (source < size && frameBudget != 0U) {
        if (m_parsedFrames >= m_incomingRoot.manifest.frameCount) {
            return Status::Corrupt;
        }
        if (m_recordHeaderRead < WIRE_RECORD_HEADER_BYTES) {
            const size_t count = std::min(
                WIRE_RECORD_HEADER_BYTES - m_recordHeaderRead, size - source);
            m_bodyCrcState = UpdateCrc32(m_bodyCrcState, bytes + source, count);
            std::memcpy(m_recordHeader.data() + m_recordHeaderRead,
                        bytes + source, count);
            const size_t block = m_parsedFrames / KEYFRAME_INTERVAL;
            if (m_wireBodyMode == WireBodyMode::Legacy) {
                m_blockCrcState = UpdateCrc32(m_blockCrcState,
                                              bytes + source, count);
                m_incomingRoot.blockBytes[block] +=
                    static_cast<uint32_t>(count);
            }
            source += count;
            m_bodyCursor += static_cast<uint32_t>(count);
            m_recordHeaderRead += static_cast<uint16_t>(count);
            if (m_recordHeaderRead != WIRE_RECORD_HEADER_BYTES) continue;
            m_recordLength = fxe::ReadU16LE(m_recordHeader.data());
            if (m_recordLength < 3U ||
                m_recordLength > WIRE_FRAME_MAX_BYTES ||
                m_parsedFrames >= m_incomingRoot.manifest.frameCount) {
                return Status::Corrupt;
            }
        }
        const size_t count = std::min<size_t>(
            m_recordLength - m_recordPayloadRead, size - source);
        m_bodyCrcState = UpdateCrc32(m_bodyCrcState, bytes + source, count);
        std::memcpy(m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES +
                        m_recordPayloadRead,
                    bytes + source, count);
        m_frameCrcState = UpdateCrc32(m_frameCrcState,
                                      bytes + source, count);
        const size_t block = m_parsedFrames / KEYFRAME_INTERVAL;
        if (m_wireBodyMode == WireBodyMode::Legacy) {
            m_blockCrcState = UpdateCrc32(m_blockCrcState,
                                          bytes + source, count);
            m_incomingRoot.blockBytes[block] += static_cast<uint32_t>(count);
        }
        source += count;
        m_bodyCursor += static_cast<uint32_t>(count);
        m_recordPayloadRead += static_cast<uint16_t>(count);
        if (m_recordPayloadRead == m_recordLength) {
            const Status complete = FinishRecord();
            if (complete != Status::Ok) return complete;
            --frameBudget;
        }
    }
    return Status::Ok;
}

Status FrameStore::WriteBodyChunk(const TransactionToken& token,
                                  uint32_t bodyOffset, const uint8_t* bytes,
                                  size_t size) {
    size_t consumed = 0U;
    uint16_t frameBudget = FRAME_COUNT;
    const Status status = WriteBodyChunk(token, bodyOffset, bytes, size, consumed, frameBudget);
    return status == Status::Ok && consumed != size ? Status::Corrupt : status;
}

Status FrameStore::WriteBodyChunk(const TransactionToken& token,
                                  uint32_t bodyOffset, const uint8_t* bytes,
                                  size_t size, size_t& consumed,
                                  uint16_t& frameBudget) {
    consumed = 0U;
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Wire ||
        bodyOffset != m_bodyCursor || bytes == nullptr || size == 0U ||
        m_bodyCursor > m_expectedBodyBytes ||
        size > m_expectedBodyBytes - m_bodyCursor) {
        return Status::InvalidArgument;
    }
    if (frameBudget == 0U) return Status::Ok;
    const Status ready = PrepareIncomingBodyWrite();
    return ready == Status::Ok ? ParseWireBytes(bytes, size, consumed, frameBudget) : ready;
}

Status FrameStore::Checkpoint(const TransactionToken& token,
                              uint32_t bodyOffset) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (bodyOffset != m_bodyCursor) return Status::InvalidArgument;
    return m_ops.sync(m_ops.context) ? Status::Ok : Status::IoFailure;
}

Status FrameStore::WriteFrame(const TransactionToken& token,
                              uint16_t index, const Frame& frame) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Generated ||
        index != m_parsedFrames || index >= m_incomingRoot.manifest.frameCount) {
        return Status::InvalidArgument;
    }
    const Status ready = PrepareIncomingBodyWrite();
    if (ready != Status::Ok) return ready;
    std::array<uint8_t, WIRE_FRAME_MAX_BYTES> encoded{};
    size_t encodedSize = 0U;
    const Frame* previous = index == 0U ||
            (m_incomingRoot.manifest.predecessorDeltas &&
             index % KEYFRAME_INTERVAL == 0U)
        ? nullptr : &m_incomingPreviousFrame;
    if (!litchcraft_wire_codec::FrameCodec::Encode(
            previous, frame, encoded.data(), encoded.size(), encodedSize)) {
        return Status::InvalidArgument;
    }
    std::array<uint8_t, WIRE_RECORD_HEADER_BYTES> header{};
    fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), header.data());
    fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize),
                    header.data() + 2U);
    Status status = ParseWireBytes(header.data(), header.size());
    if (status == Status::Ok) status = ParseWireBytes(encoded.data(), encodedSize);
    return status;
}

Status FrameStore::WriteBlankFirstFrame(const TransactionToken& token) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Generated ||
        m_incomingRoot.manifest.frameCount != 1U || m_parsedFrames != 0U ||
        m_bodyCursor != 0U) {
        return Status::InvalidArgument;
    }
    // At frame zero encoding finishes before parsing replaces this owner;
    // neither encoder nor decoder reads a previous frame at that index.
    m_incomingPreviousFrame.durationMs = 1000U;
    m_incomingPreviousFrame.emitters.fill({});
    return WriteFrame(token, 0U, m_incomingPreviousFrame);
}

bool FrameStore::IncomingPreviewReady() const {
    return m_transactionActive && m_transactionKind == TransactionKind::Wire &&
           m_parsedFrames != 0U;
}

Status FrameStore::ReplaceFrame(const TransactionToken& token,
                                uint16_t index, const Frame& frame) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Edit || m_editHasFrame ||
        index >= m_incomingRoot.manifest.frameCount) {
        return Status::InvalidArgument;
    }
    m_editFrame = frame;
    m_editHasFrame = true;
    m_editIndex = index;
    m_summaryEdits[index] = true;
    if (index + 1U < m_incomingRoot.manifest.frameCount) {
        m_summaryEdits[index + 1U] = true;
    }
    return Status::Ok;
}

Status FrameStore::InsertFrame(const TransactionToken& token,
                               uint16_t index, const Frame& frame) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Edit || m_editHasFrame ||
        index > m_incomingRoot.manifest.frameCount ||
        m_incomingRoot.manifest.frameCount >= FRAME_COUNT) {
        return Status::InvalidArgument;
    }
    for (uint16_t destination = m_incomingRoot.manifest.frameCount;
         destination > index; --destination) {
        m_incomingRoot.slots[destination] =
            m_incomingRoot.slots[destination - 1U];
        m_workspace.summaries[destination] = m_workspace.summaries[destination - 1U];
        m_summaryEdits[destination] = m_summaryEdits[destination - 1U];
    }
    m_editFrame = frame;
    m_editHasFrame = true;
    m_editStructural = true;
    m_editIndex = index;
    ++m_incomingRoot.manifest.frameCount;
    m_workspace.summaries[index] = {};
    m_summaryEdits[index] = true;
    if (index + 1U < m_incomingRoot.manifest.frameCount) {
        m_summaryEdits[index + 1U] = true;
    }
    return Status::Ok;
}

Status FrameStore::AppendFrame(const TransactionToken& token,
                               const Frame& frame) {
    return InsertFrame(token, m_incomingRoot.manifest.frameCount, frame);
}

Status FrameStore::CopyFrameAfter(const TransactionToken& token,
                                  uint16_t index) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Edit || m_editHasFrame ||
        index >= m_incomingRoot.manifest.frameCount ||
        m_incomingRoot.manifest.frameCount >= FRAME_COUNT) {
        return Status::InvalidArgument;
    }
    const uint16_t insertion = index + 1U;
    for (uint16_t destination = m_incomingRoot.manifest.frameCount;
         destination > insertion; --destination) {
        m_incomingRoot.slots[destination] =
            m_incomingRoot.slots[destination - 1U];
        m_workspace.summaries[destination] = m_workspace.summaries[destination - 1U];
        m_summaryEdits[destination] = m_summaryEdits[destination - 1U];
    }
    m_incomingRoot.slots[insertion] = m_incomingRoot.slots[index];
    m_workspace.summaries[insertion] = m_workspace.summaries[index];
    m_summaryEdits[insertion] = true;
    ++m_incomingRoot.manifest.frameCount;
    if (insertion + 1U < m_incomingRoot.manifest.frameCount) {
        m_summaryEdits[insertion + 1U] = true;
    }
    m_editStructural = true;
    return Status::Ok;
}

Status FrameStore::DeleteFrame(const TransactionToken& token,
                               uint16_t index) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_transactionKind != TransactionKind::Edit || m_editHasFrame ||
        index >= m_incomingRoot.manifest.frameCount ||
        m_incomingRoot.manifest.frameCount == 1U) {
        return Status::InvalidArgument;
    }
    for (uint16_t destination = index;
         destination + 1U < m_incomingRoot.manifest.frameCount;
         ++destination) {
        m_incomingRoot.slots[destination] =
            m_incomingRoot.slots[destination + 1U];
        m_workspace.summaries[destination] = m_workspace.summaries[destination + 1U];
        m_summaryEdits[destination] = m_summaryEdits[destination + 1U];
    }
    --m_incomingRoot.manifest.frameCount;
    m_incomingRoot.slots[m_incomingRoot.manifest.frameCount] = 0U;
    m_workspace.summaries[m_incomingRoot.manifest.frameCount] = {};
    m_summaryEdits[m_incomingRoot.manifest.frameCount] = false;
    if (index < m_incomingRoot.manifest.frameCount) m_summaryEdits[index] = true;
    m_editStructural = true;
    return Status::Ok;
}

Status FrameStore::PrepareEdit() {
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    // Measurement is serialized on the storage worker. Reuse decode scratch
    // while the captured edit and incoming predecessor retain separate owners.
    view::WireView wire(storage, m_incomingRoot,
                        m_wireRecord.data(), m_wireRecord.size(),
                        m_incomingPreviousFrame, m_decodeCachedFrame,
                        m_editIndex, m_editHasFrame ? &m_editFrame : nullptr);
    FixedRoot identity{};
    const fixed::Status measured = wire.MeasureEditedSummaries(
        m_workspace.summaries, m_summaryEdits, identity);
    if (measured != fixed::Status::Ok) return Map(measured);
    m_incomingRoot.bodyBytes = identity.bodyBytes;
    m_incomingRoot.bodyCrc32 = identity.bodyCrc32;
    m_incomingRoot.blockBytes = identity.blockBytes;
    m_incomingRoot.blockCrc32 = identity.blockCrc32;
    ResetWireCursor();
    return Status::Ok;
}

// Keep readback's root descriptor off the filesystem sync/compaction stack
// and off Generated recovery/blank commits entirely.
__attribute__((noinline))
Status FrameStore::VerifyIncomingWireBody() {
    // Mandatory persisted-body readback also builds the derived summaries.
    // Ingress bytes alone never become trusted cached frame identity.
    return MeasureWireSummaries(m_incomingRoot, SummaryWorkspace());
}

Status FrameStore::PrepareCommit(const TransactionToken& token) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (m_commitPrepared) return Status::Ok;
    const uint32_t startedMs = fxe::Millis();
    if (m_transactionKind == TransactionKind::Edit) {
        const Status edit = PrepareEdit();
        if (edit != Status::Ok) {
            ReportTiming(TimingPhase::BodyPrepare, fxe::Millis() - startedMs);
            return edit;
        }
    } else {
        if (m_recordHeaderRead != 0U ||
            m_parsedFrames != m_incomingRoot.manifest.frameCount) {
            return Status::Incomplete;
        }
        if (m_transactionKind == TransactionKind::Wire &&
            (m_bodyCursor != m_expectedBodyBytes ||
             (~m_bodyCrcState) != m_expectedBodyCrc)) return Status::Corrupt;
        uint32_t combinedCrc = 0U;
        uint32_t combinedBytes = 0U;
        for (size_t block = 0U; block < fixed::WIRE_BLOCK_COUNT; ++block) {
            combinedCrc = fxe::CombineCrc32(
                combinedCrc, m_incomingRoot.blockCrc32[block],
                m_incomingRoot.blockBytes[block]);
            combinedBytes += m_incomingRoot.blockBytes[block];
        }
        if (m_wireBodyMode == WireBodyMode::Legacy &&
            (combinedBytes != m_bodyCursor ||
             combinedCrc != ~m_bodyCrcState)) return Status::Corrupt;
        if (combinedBytes > WIRE_BODY_MAX_BYTES) return Status::Corrupt;
        m_incomingRoot.bodyBytes = combinedBytes;
        m_incomingRoot.bodyCrc32 = combinedCrc;
    }
    Status prepared = m_ops.sync(m_ops.context) ? Status::Ok : Status::IoFailure;
    if (prepared == Status::Ok && m_transactionKind != TransactionKind::Edit) {
        prepared = VerifyIncomingWireBody();
    }
    ReportTiming(TimingPhase::BodyPrepare, fxe::Millis() - startedMs);
    if (prepared != Status::Ok) return prepared;
    m_commitPrepared = true;
    return Status::Ok;
}

Status FrameStore::PublishPreparedCommit(const TransactionToken& token) {
    if (!Owns(token)) return Status::RevisionConflict;
    if (!m_commitPrepared) return Status::Incomplete;
    fixed::Layout storage(m_transactionKind == TransactionKind::Edit
                              ? EditStorageOps() : m_ops,
                          m_slotScratch.data(), m_slotScratch.size());
    const uint32_t startedMs = fxe::Millis();
    Status published = Status::Ok;
    if (m_transactionKind == TransactionKind::Edit && m_editHasFrame &&
        !m_editStructural) {
        FixedRoot committed{};
        published = Map(storage.CommitFrameEdit(
            m_currentRoot, m_currentCopy, m_editIndex, m_editFrame,
            m_incomingRoot.generation, m_incomingRoot, committed,
            &m_workspace.summaries));
        if (published == Status::Ok) m_incomingRoot = committed;
    } else {
        if (m_transactionKind == TransactionKind::Edit && m_editHasFrame) {
            published = Map(storage.StageFrame(m_editFrame));
            if (published == Status::Ok) {
                m_incomingRoot.stageIndex = m_editIndex;
            }
        }
        if (published == Status::Ok) {
            published = Map(storage.PublishRoot(m_incomingRoot,
                m_incomingCopy, &m_workspace.summaries));
        }
    }
    ReportTiming(TimingPhase::RootPublish, fxe::Millis() - startedMs);
    if (published != Status::Ok) return published;
    if (m_transactionKind != TransactionKind::Edit) {
        if (m_hasCurrent) {
            m_alternateRoot = m_currentRoot;
            m_alternateCopy = m_currentCopy;
            m_hasAlternate = true;
        }
        m_decodeCachedFrame = m_editFrame;
    } else if (m_editHasFrame && m_editIndex == 0U) {
        m_decodeCachedFrame = m_editFrame;
    }
    m_currentRoot = m_incomingRoot;
    m_currentCopy = m_incomingCopy;
    m_wireSummariesReady = true;
    m_hasCurrent = true;
    m_transactionActive = false;
    m_commitPrepared = false;
    m_transactionKind = TransactionKind::None;
    m_incomingRootNeedsInvalidation = false;
    m_activeToken = {};
    ++m_pageEpoch;
    ResetWireCursor();
    return Status::Ok;
}

Status FrameStore::Commit(const TransactionToken& token) {
    const Status prepared = PrepareCommit(token);
    return prepared == Status::Ok ? PublishPreparedCommit(token) : prepared;
}

Status FrameStore::Abort(const TransactionToken& token) {
    if (SameToken(token, m_lastAbortedToken)) return Status::Ok;
    if (!Owns(token)) return Status::RevisionConflict;
    m_lastAbortedToken = token;
    const bool reconcile = m_incomingRootTouched || m_commitPrepared;
    m_transactionActive = false;
    m_commitPrepared = false;
    m_transactionKind = TransactionKind::None;
    m_activeToken = {};
    if (!reconcile) return Status::Ok;
    const Status recovered = Recover();
    return recovered == Status::NoArtwork ? Status::Ok : recovered;
}

Status FrameStore::BeginReadFrame(uint16_t index, ReadCursor& cursor) {
    cursor = {};
    if (!IsConfigured()) return Status::NotConfigured;
    if (!m_hasCurrent) return Status::NoArtwork;
    if (index >= m_currentRoot.manifest.frameCount) {
        return Status::InvalidArgument;
    }
    cursor.generation = m_currentRoot.generation;
    cursor.pageEpoch = ++m_pageEpoch;
    cursor.targetIndex = index;
    cursor.active = true;
    return Status::Ok;
}

Status FrameStore::ContinueReadFrame(ReadCursor& cursor,
                                     size_t maximumBytes) {
    if (!cursor.active || maximumBytes == 0U) {
        return Status::InvalidArgument;
    }
    if (!m_hasCurrent || cursor.generation != m_currentRoot.generation ||
        cursor.pageEpoch != m_pageEpoch ||
        cursor.targetIndex >= m_currentRoot.manifest.frameCount) {
        cursor.active = false;
        return Status::RevisionConflict;
    }
    cursor.encodedRead = static_cast<uint16_t>(std::min<size_t>(
        STAGE_SLOT_BYTES, cursor.encodedRead + maximumBytes));
    if (cursor.encodedRead < STAGE_SLOT_BYTES) return Status::Incomplete;
    // Edit publication owns slot scratch across its write/readback calls.
    // Its paging callback can reuse the idle wire buffer without disturbing
    // those bytes or allocating another frame/cache. Other reads retain the
    // normal scratch owner, including wire ingress and checksum measurement.
    fixed::Layout storage(m_ops,
        m_editPaging ? m_wireRecord.data() : m_slotScratch.data(),
        m_editPaging ? m_wireRecord.size() : m_slotScratch.size());
    const Status read = Map(storage.ReadFrame(m_currentRoot,
                                              cursor.targetIndex,
                                              m_decodeCachedFrame));
    cursor.active = false;
    return read;
}

void FrameStore::CancelReadFrame(ReadCursor& cursor) {
    cursor.active = false;
}

Status FrameStore::ReadFrame(uint16_t index, Frame& out) {
    ReadCursor cursor{};
    Status status = BeginReadFrame(index, cursor);
    while (status == Status::Ok && cursor.active) {
        status = ContinueReadFrame(cursor, IO_PAGE_BYTES);
        if (status == Status::Incomplete) status = Status::Ok;
    }
    if (status == Status::Ok) out = m_decodeCachedFrame;
    return status;
}

Status FrameStore::ReadWireBody(uint32_t expectedGeneration,
                                uint32_t bodyOffset, uint8_t* out,
                                size_t capacity, size_t& outSize) {
    outSize = 0U;
    if (!IsConfigured()) return Status::NotConfigured;
    if (m_transactionActive) return Status::Incomplete;
    if (!m_hasCurrent) return Status::NoArtwork;
    if (expectedGeneration == 0U ||
        expectedGeneration != m_currentRoot.generation) {
        return Status::RevisionConflict;
    }
    if (out == nullptr || capacity == 0U ||
        bodyOffset >= m_currentRoot.bodyBytes) {
        return Status::InvalidArgument;
    }
    if (m_wireFrameIndex != FRAME_COUNT) ResetWireCursor();
    fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
    view::WireView wire(storage, m_currentRoot, m_wireRecord.data(),
                        m_wireRecord.size(), m_incomingPreviousFrame,
                        m_editFrame);
    if (m_wireGeneration == expectedGeneration &&
        bodyOffset >= m_wireOffset) {
        const view::WireView::Cursor saved{m_wireOffset, m_wireRecordSize,
                                           m_wireRecordRead, m_wireNextFrame};
        if (!wire.RestoreCursor(saved)) return Status::Corrupt;
    }
    const Status read = Map(wire.Read(bodyOffset, out,
        std::min<size_t>(capacity, m_currentRoot.bodyBytes - bodyOffset),
        outSize));
    if (read != Status::Ok) {
        ResetWireCursor();
        return read;
    }
    const auto next = wire.CaptureCursor();
    m_wireGeneration = expectedGeneration;
    m_wireOffset = next.streamOffset;
    m_wireRecordSize = next.recordSize;
    m_wireRecordRead = next.recordRead;
    m_wireNextFrame = next.nextFrame;
    return Status::Ok;
}

Status FrameStore::ReadWireFrame(
    uint32_t expectedGeneration, uint16_t frameIndex, uint32_t cursor,
    uint8_t* out, size_t capacity, size_t& outSize, uint32_t& totalBytes,
    uint32_t& bodyCrc32) {
    outSize = 0U;
    totalBytes = 0U;
    bodyCrc32 = 0U;
    if (!IsConfigured()) return Status::NotConfigured;
    if (m_transactionActive) return Status::Incomplete;
    if (!m_hasCurrent) return Status::NoArtwork;
    if (expectedGeneration == 0U ||
        expectedGeneration != m_currentRoot.generation) {
        return Status::RevisionConflict;
    }
    if (frameIndex >= m_currentRoot.manifest.frameCount ||
        out == nullptr || capacity == 0U || cursor >= m_wireRecord.size()) {
        return Status::InvalidArgument;
    }
    if (m_wireGeneration != expectedGeneration ||
        m_wireFrameIndex != frameIndex) {
        // The same record page also owns full-body streaming. Retire that
        // cursor before lending its page and decode scratch to this one slot.
        ResetWireCursor();
        ++m_pageEpoch;
        fixed::Layout storage(m_ops, m_slotScratch.data(), m_slotScratch.size());
        const Status read = Map(storage.ReadFrame(
            m_currentRoot, frameIndex, m_decodeCachedFrame));
        if (read != Status::Ok) return read;
        size_t encoded = 0U;
        if (!litchcraft_wire_codec::FrameCodec::Encode(
                nullptr, m_decodeCachedFrame,
                m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES,
                WIRE_FRAME_MAX_BYTES, encoded)) return Status::Corrupt;
        fxe::WriteU16LE(static_cast<uint16_t>(encoded), m_wireRecord.data());
        fxe::WriteU32LE(fxe::Crc32(
            m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES, encoded),
            m_wireRecord.data() + 2U);
        m_wireRecordSize = WIRE_RECORD_HEADER_BYTES + encoded;
        m_wireGeneration = expectedGeneration;
        m_wireFrameIndex = frameIndex;
    }
    if (cursor >= m_wireRecordSize) return Status::InvalidArgument;
    totalBytes = static_cast<uint32_t>(m_wireRecordSize);
    bodyCrc32 = fxe::Crc32(m_wireRecord.data(), m_wireRecordSize);
    outSize = std::min<size_t>(capacity, m_wireRecordSize - cursor);
    std::memcpy(out, m_wireRecord.data() + cursor, outSize);
    return Status::Ok;
}

} // namespace modules::litchcraft_frame_store
