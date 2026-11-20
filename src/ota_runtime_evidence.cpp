#include "firmware/ota_runtime_evidence_internal.hpp"

#if defined(ARDUINO) && !defined(LITCH_ESP32S3)

namespace firmware {
bool CopyEsp32OtaBootEvidence(OtaBootEvidence& out) { out = {}; return false; }
namespace ota_evidence_internal {
SnapshotStamp CaptureStamp() { return {}; }
void RecordSnapshot(const modules::OtaPostBootSnapshot*, SnapshotStamp) {}
void InvalidateSnapshot() {}
void RecordAdmission(const modules::OtaAdmissionEvidence&) {}
bool CopyAdmission(modules::OtaAdmissionEvidence& out) { out = {}; return false; }
} // namespace ota_evidence_internal
} // namespace firmware

#else

#include <atomic>

#include "foxie_embedded/millis.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <freertos/FreeRTOS.h>
#else
#include <mutex>
#endif

namespace firmware {
namespace {

OtaBootEvidence bootEvidence;
modules::OtaAdmissionEvidence admissionEvidence;
std::atomic<uint32_t> nextSample{0U};

// Only bounded scalar copies run under this lock. No SDK, flash, clock,
// allocation or logging work is performed while readers/writers exclude.
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
portMUX_TYPE evidenceMux = portMUX_INITIALIZER_UNLOCKED;
void Lock() { portENTER_CRITICAL(&evidenceMux); }
bool TryLock() { return portTRY_ENTER_CRITICAL(&evidenceMux, 0U); }
void Unlock() { portEXIT_CRITICAL(&evidenceMux); }
#else
std::mutex evidenceMutex;
void Lock() { evidenceMutex.lock(); }
bool TryLock() { return evidenceMutex.try_lock(); }
void Unlock() { evidenceMutex.unlock(); }
#endif

} // namespace

bool CopyEsp32OtaBootEvidence(OtaBootEvidence& out) {
    if (!TryLock()) return false;
    out = bootEvidence;
    Unlock();
    return true;
}

namespace ota_evidence_internal {

SnapshotStamp CaptureStamp() {
    return {nextSample.fetch_add(1U, std::memory_order_relaxed) + 1U, fxe::Millis()};
}

void RecordSnapshot(const modules::OtaPostBootSnapshot* const snapshot,
                    const SnapshotStamp stamp) {
    OtaBootEvidence next;
    next.sample = stamp.sample;
    next.capturedAtMs = stamp.capturedAtMs;
    next.available = snapshot != nullptr;
    if (snapshot != nullptr) {
        next.snapshot = *snapshot;
        next.runningState = modules::EvaluateOtaPostBootVerification(
            *snapshot, modules::OtaPostBootHealth::NotReported).runningState;
    }
    Lock();
    // A released publisher may be preempted by a later SDK observer. Never
    // relabel its older observation as the newest state when it resumes.
    if (static_cast<int32_t>(stamp.sample - bootEvidence.sample) > 0) {
        bootEvidence = next;
    }
    Unlock();
}

void InvalidateSnapshot() { RecordSnapshot(nullptr, CaptureStamp()); }

void RecordAdmission(const modules::OtaAdmissionEvidence& admission) {
    Lock();
    admissionEvidence = admission;
    Unlock();
}

bool CopyAdmission(modules::OtaAdmissionEvidence& out) {
    if (!TryLock()) return false;
    out = admissionEvidence;
    Unlock();
    return true;
}

} // namespace ota_evidence_internal
} // namespace firmware
#endif
