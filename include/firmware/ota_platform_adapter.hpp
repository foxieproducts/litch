#pragma once

#include "modules/ota_platform_adapter.hpp"
#include "modules/ota_post_boot_executor.hpp"

namespace hardware {
class RmtFlashArbiter;
}

namespace modules { class FirmwareDigestProvider; }

namespace firmware {

using OtaBootEvidence = modules::OtaBootEvidence;

/** RAM only. False means contended; available=false means no usable capture. */
bool CopyEsp32OtaBootEvidence(OtaBootEvidence& outEvidence);

/** Start the S3 boot worker without mounting or loading user storage. */
modules::OtaPlatformAdapter* InitializeEsp32OtaPlatformAdapter(
    hardware::RmtFlashArbiter& flashArbiter);

bool InitializeEsp32OtaPostBootWorker(
    hardware::RmtFlashArbiter& flashArbiter);

/** Schedule a standalone recovery digest independently of usable OTA slot metadata. */
bool QueueEsp32FirmwareDigest(modules::FirmwareDigestProvider& provider);

using OtaBootHealthTicket = uint32_t;
enum class OtaBootHealthRequestStatus : uint8_t {
    Pending, Complete, Busy, Unavailable, Stale, Unsupported,
};

/**
 * Retain one health transaction on the existing worker without waiting.
 * Busy leaves the occupied job untouched. A nonzero ticket owns its result
 * until the matching poll consumes Complete; there is no running-job cancel.
 */
OtaBootHealthRequestStatus BeginEsp32OtaBootHealth(
    modules::OtaPostBootHealth health, OtaBootHealthTicket& outTicket);

/** Resolve the retained preparation ticket after authoritative runtime health. */
OtaBootHealthRequestStatus ResolveEsp32OtaBootHealth(
    OtaBootHealthTicket ticket, modules::OtaPostBootHealth health);

/**
 * Zero-wait observation. Outputs are written only on Complete, exactly once.
 * Only a Confirmed Passed job supplies its verified SHA; other results zero it.
 */
OtaBootHealthRequestStatus PollEsp32OtaBootHealth(
    OtaBootHealthTicket ticket, modules::OtaPostBootExecutionResult& outResult,
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES>& outImageSha256);

/** Short raw SDK observation; caller must already own the flash/RMT lease. */
bool ReadEsp32OtaBootSnapshotWithFlashLease(
    modules::OtaPostBootSnapshot& outSnapshot);

/** Full source integrity/coherent VALID proof on the existing Core-0 worker. */
bool VerifyEsp32OtaUpdateSource(modules::OtaPostBootSnapshot& outSnapshot);

/** Worker-only SDK implementation; never call from the cooperative main loop. */
bool ReadEsp32OtaVerifiedSourceOnWorker(
    hardware::RmtFlashArbiter& arbiter,
    modules::OtaPostBootSnapshot& outSnapshot);

/** Execute one complete health transaction on the existing Core-0 worker. */
modules::OtaPostBootExecutionResult ProcessEsp32OtaBootHealth(
    modules::OtaPostBootHealth health);

bool ReadEsp32OtaRunningState(
    modules::OtaPostBootRunningState& outState);

} // namespace firmware
