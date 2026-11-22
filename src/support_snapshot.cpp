#include "modules/support_snapshot.hpp"
#include <cstring>

namespace modules::support_snapshot {
namespace {
class Writer {
  public:
    Writer(uint8_t* out, size_t capacity) : m_out(out), m_capacity(capacity) {}
    void Put(uint64_t value, size_t bytes) {
        for (size_t i = 0U; i < bytes; ++i) {
            if (m_out != nullptr && m_size < m_capacity) m_out[m_size] = value & 0xffU;
            ++m_size;
            value >>= 8U;
        }
    }
    void Header(uint8_t kind, uint32_t request) { Put(1U, 1U); Put(kind, 1U); Put(request, 4U); }
    size_t Size() const { return m_out != nullptr && m_size <= m_capacity ? m_size : 0U; }
  private:
    uint8_t* m_out;
    size_t m_capacity;
    size_t m_size = 0U;
};
void Progress(Writer& w, const runtime::ProgressRecord& record) {
    const auto& c = record.context;
    w.Put(c.uptimeMs, 4U); w.Put(c.expectedOccupancy, 8U); w.Put(c.observedOccupancy, 8U);
    w.Put(c.healthCauses, 2U); w.Put(c.interactionPhase, 1U); w.Put(c.variant, 1U);
    w.Put(c.storageError, 1U); w.Put(c.flags, 1U);
    for (auto stage : record.stages) w.Put(stage, 1U);
    for (auto at : record.stageAtMs) w.Put(at, 4U);
    const unsigned start = (unsigned(record.nextStage) + 8U - record.recentStageCount) % 8U;
    for (unsigned i = 0U; i < 8U; ++i) {
        w.Put(i < record.recentStageCount ? record.recentStages[(start + i) % 8U]
            : unsigned(runtime::ProgressStage::Idle), 1U);
    }
}
}
size_t EncodePublic(uint8_t* out, size_t capacity, uint32_t request,
                    const PublicHealth& h) {
    if (capacity < PUBLIC_BYTES) return 0U;
    Writer w(out, capacity);
    w.Put(2U, 1U); w.Put(h.validity, 1U); w.Put(h.healthRevision, 2U);
    w.Put(h.healthState, 1U); w.Put(h.healthCauses, 2U);
    w.Put(h.resetReason, 1U); w.Put(h.storageError, 1U); w.Put(h.bootLoadError, 1U);
    w.Put(h.uptimeMs, 8U); w.Put(h.hardwareMajor, 1U); w.Put(h.hardwareMinor, 1U);
    w.Put(h.digestAvailability, 1U);
    for (auto byte : h.digest) w.Put(byte, 1U);
    const size_t labelSize = h.releaseLabel != nullptr ? std::strlen(h.releaseLabel) : 0U;
    const size_t count = labelSize < 28U ? labelSize : 28U;
    w.Put(count, 1U);
    for (size_t i = 0U; i < 28U; ++i) w.Put(i < count ? static_cast<uint8_t>(h.releaseLabel[i]) : 0U, 1U);
    w.Put(request, 4U);
    w.Put(h.recoveryMode, 1U); w.Put(h.recoveryCause, 1U);
    w.Put(h.recoveryState, 1U); w.Put(h.recoveryFailure, 1U);
    w.Put(h.boots, 4U); w.Put(h.earlyCrashes, 4U);
    w.Put(h.updater, 1U); w.Put(h.storageAccess, 1U);
    w.Put(h.priorStage, 1U); w.Put(h.priorStageAtMs, 4U);
    w.Put(h.priorStageValid, 1U);
    return w.Size();
}
size_t EncodeHistory(uint8_t* out, size_t capacity, uint32_t request,
    const SupportRuntimeSnapshot& s, uint32_t total, uint32_t saved,
    uint32_t failed, const trace::SupportActionHistory& actions) {
    Writer w(out, capacity);
    w.Header(0U, request); w.Put(s.available, 1U); w.Put(actions.Size(), 1U);
    w.Put(actions.Omitted(), 4U);
    w.Put(s.boots, 8U); w.Put(s.priorRuntimeMs, 8U); w.Put(s.uptimeMs, 8U);
    w.Put(s.savedCheckpointMs, 8U); w.Put(s.automaticSleeps, 8U);
    w.Put(s.manualSleeps, 8U); w.Put(s.otherSleeps, 8U);
    w.Put(total, 4U); w.Put(saved, 4U); w.Put(failed, 4U);
    for (size_t i = 0U; i < actions.Size(); ++i) {
        const auto& a = actions.At(i);
        w.Put(a.uptimeMs, 8U); w.Put(a.sequence, 4U); w.Put(a.durationMs, 4U);
        w.Put(static_cast<uint8_t>(a.kind), 1U);
        w.Put((static_cast<uint8_t>(a.origin) << 4U) | static_cast<uint8_t>(a.outcome), 1U);
        w.Put(a.count, 1U); w.Put(a.secondCount, 1U);
    }
    return w.Size();
}
size_t EncodeIncident(uint8_t* out, size_t capacity, uint32_t request,
    const runtime::ProgressSnapshot& p, uint32_t saved, uint32_t failed,
    uint8_t bootError) {
    Writer w(out, capacity);
    w.Header(1U, request); w.Put(p.resetReason, 4U); w.Put(p.bootCount, 4U);
    w.Put(p.watchdogRestarts, 4U); w.Put(p.previousValid, 1U);
    Progress(w, p.current); Progress(w, p.previous);
    w.Put(saved, 4U); w.Put(failed, 4U); w.Put(bootError, 1U);
    return w.Size();
}
} // namespace modules::support_snapshot
