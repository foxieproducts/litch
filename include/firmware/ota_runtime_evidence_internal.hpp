#pragma once

#include "firmware/ota_platform_adapter.hpp"

namespace firmware::ota_evidence_internal {

struct SnapshotStamp {
    uint32_t sample = 0U;
    uint32_t capturedAtMs = 0U;
};

/** Reserve immediately after a raw SDK capture, while its lease is still held. */
SnapshotStamp CaptureStamp();
/** Existing platform owners publish only after releasing their flash lease. */
void RecordSnapshot(const modules::OtaPostBootSnapshot* snapshot,
                    SnapshotStamp stamp);
void InvalidateSnapshot();
void RecordAdmission(const modules::OtaAdmissionEvidence& admission);
bool CopyAdmission(modules::OtaAdmissionEvidence& outAdmission);

} // namespace firmware::ota_evidence_internal
