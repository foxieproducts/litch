#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules {

constexpr size_t OTA_BOOT_POLICY_DESCRIPTOR_BYTES = 16U;
constexpr uint16_t OTA_BOOT_POLICY_SCHEMA = 1U;
constexpr uint32_t OTA_BOOT_POLICY_RECORD_EPOCH = 1U;
constexpr uint32_t OTA_BOOT_POLICY_SDK_EPOCH = 2U;
// SDK metadata is now the sole boot authority; record-dependent NEW images are rejected.
constexpr uint32_t OTA_BOOT_POLICY_CURRENT_EPOCH = OTA_BOOT_POLICY_SDK_EPOCH;

enum class OtaBootPolicyStatus : uint8_t {
    Unavailable = 0U,
    Absent,
    Known,
    Unsupported,
    Malformed,
};

/** Descriptor evidence from a verified complete image, not upload permission. */
struct OtaBootPolicy {
    OtaBootPolicyStatus status = OtaBootPolicyStatus::Unavailable;
    uint16_t schema = 0U;
    uint16_t size = 0U;
    uint32_t epoch = 0U;
};

/** Fixed little-endian ABI immediately after the first segment's SDK appdesc. */
constexpr std::array<uint8_t, OTA_BOOT_POLICY_DESCRIPTOR_BYTES>
EncodeOtaBootPolicyDescriptor(const uint32_t epoch) {
    return {{'F', 'X', 'B', 'T',
             static_cast<uint8_t>(OTA_BOOT_POLICY_SCHEMA), 0U,
             static_cast<uint8_t>(OTA_BOOT_POLICY_DESCRIPTOR_BYTES), 0U,
             static_cast<uint8_t>(epoch),
             static_cast<uint8_t>(epoch >> 8U),
             static_cast<uint8_t>(epoch >> 16U),
             static_cast<uint8_t>(epoch >> 24U),
             0U, 0U, 0U, 0U}};
}

} // namespace modules
