#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace modules::ble_device_capabilities {

inline constexpr uint8_t CONTRACT_VERSION = 3U;

enum class HardwareRevision : uint8_t {
    Unknown = 0U,
    V1 = 1U,
    V2 = 2U,
    V3 = 3U,
    V3_3 = 4U,
};

constexpr HardwareRevision HardwareRevisionFromId(const uint8_t revisionId) {
    switch (revisionId) {
        case static_cast<uint8_t>(HardwareRevision::V1):
            return HardwareRevision::V1;
        case static_cast<uint8_t>(HardwareRevision::V2):
            return HardwareRevision::V2;
        case static_cast<uint8_t>(HardwareRevision::V3):
            return HardwareRevision::V3;
        case static_cast<uint8_t>(HardwareRevision::V3_3):
            return HardwareRevision::V3_3;
        default:
            return HardwareRevision::Unknown;
    }
}

using CapabilityMaskValue = uint16_t;

enum Capability : CapabilityMaskValue {
    Console = 1U << 0U,
    Settings = 1U << 1U,
    Identify = 1U << 2U,
    FirmwareDigest = 1U << 3U,
    FirmwareUpdate = 1U << 4U,
    RuntimeHealth = 1U << 5U,
    RuntimeHealthRecheck = 1U << 6U,
    GameHistory = 1U << 7U,
    CoachInsight = 1U << 8U,
    OwnerPairing = 1U << 9U,
    Litchcraft = 1U << 10U,
    LitchcraftBulk = 1U << 11U,
    LitchcraftInspect = 1U << 12U,
    LitchcraftPalette = 1U << 13U,
    LitchcraftAnimation = 1U << 14U,
    LitchcraftShakeErase = 1U << 15U,
};

constexpr CapabilityMaskValue CapabilityMask(
    const bool supportsFirmwareUpdate,
    const bool supportsRuntimeHealthRecheck,
    const bool supportsGameHistory = false,
    const bool supportsCoachInsight = false,
    const bool supportsLitchcraft = false,
    const bool supportsLitchcraftBulk = false,
    const bool supportsLitchcraftInspect = false,
    const bool supportsLitchcraftPalette = false,
    const bool supportsLitchcraftAnimation = false,
    const bool supportsLitchcraftShakeErase = false,
    const bool supportsOwnerPairing = false) {
    const CapabilityMaskValue common = static_cast<CapabilityMaskValue>(
        Capability::Console | Capability::Settings | Capability::Identify |
        Capability::FirmwareDigest | Capability::RuntimeHealth);
    CapabilityMaskValue mask =
        supportsFirmwareUpdate
            ? static_cast<CapabilityMaskValue>(common |
                                               Capability::FirmwareUpdate)
            : common;
    if (supportsRuntimeHealthRecheck) {
        mask = static_cast<CapabilityMaskValue>(
            mask | Capability::RuntimeHealthRecheck);
    }
    if (supportsGameHistory) {
        mask = static_cast<CapabilityMaskValue>(
            mask | Capability::GameHistory);
    }
    if (supportsCoachInsight) {
        mask = static_cast<CapabilityMaskValue>(
            mask | Capability::CoachInsight);
    }
    if (supportsLitchcraft) {
        mask = static_cast<CapabilityMaskValue>(mask | Capability::Litchcraft);
        if (supportsLitchcraftBulk) {
            mask = static_cast<CapabilityMaskValue>(
                mask | Capability::LitchcraftBulk);
        }
        if (supportsLitchcraftInspect) {
            mask = static_cast<CapabilityMaskValue>(
                mask | Capability::LitchcraftInspect);
        }
        if (supportsLitchcraftPalette) {
            mask = static_cast<CapabilityMaskValue>(
                mask | Capability::LitchcraftPalette);
        }
        if (supportsLitchcraftAnimation && supportsLitchcraftInspect) {
            mask = static_cast<CapabilityMaskValue>(
                mask | Capability::LitchcraftAnimation);
        }
        if (supportsLitchcraftShakeErase) {
            mask = static_cast<CapabilityMaskValue>(
                mask | Capability::LitchcraftShakeErase);
        }
    }
    if (supportsOwnerPairing) mask = static_cast<CapabilityMaskValue>(mask | Capability::OwnerPairing);
    return mask;
}

// The compact envelope must fit in the 20-byte ATT payload available at the
// minimum BLE MTU. Shape: {"i":[contract,hardware,capability_mask]}.
inline bool FormatCompactInfo(const HardwareRevision hardwareRevision,
                              const bool supportsFirmwareUpdate,
                              const bool supportsRuntimeHealthRecheck,
                              const bool supportsGameHistory,
                              const bool supportsCoachInsight,
                              char* const out,
                              const size_t outCapacity,
                              const bool supportsLitchcraft = false,
                              const bool supportsLitchcraftBulk = false,
                              const bool supportsLitchcraftInspect = false,
                              const bool supportsLitchcraftPalette = false,
                              const bool supportsLitchcraftAnimation = false,
                              const bool supportsLitchcraftShakeErase = false,
                              const bool supportsOwnerPairing = false) {
    if (out == nullptr || outCapacity == 0U) {
        return false;
    }

    const CapabilityMaskValue capabilityMask = CapabilityMask(
        supportsFirmwareUpdate,
        supportsRuntimeHealthRecheck,
        supportsGameHistory,
        supportsCoachInsight,
        supportsLitchcraft,
        supportsLitchcraftBulk,
        supportsLitchcraftInspect,
        supportsLitchcraftPalette,
        supportsLitchcraftAnimation,
        supportsLitchcraftShakeErase, supportsOwnerPairing);
    // Every image built with the connection-owned transport advertises the
    // new contract epoch. Capability bits remain domain features; the epoch
    // promises the mandatory 0108/0109 session endpoints themselves.
    const uint8_t contractVersion = CONTRACT_VERSION;
    const int written = std::snprintf(
        out,
        outCapacity,
        "{\"i\":[%u,%u,%u]}",
        static_cast<unsigned>(contractVersion),
        static_cast<unsigned>(hardwareRevision),
        static_cast<unsigned>(capabilityMask));
    return written > 0 && static_cast<size_t>(written) < outCapacity &&
           static_cast<size_t>(written) <= 20U;
}

inline bool FormatCompactInfo(const HardwareRevision hardwareRevision,
                              const bool supportsFirmwareUpdate,
                              const bool supportsRuntimeHealthRecheck,
                              char* const out,
                              const size_t outCapacity) {
    return FormatCompactInfo(
        hardwareRevision,
        supportsFirmwareUpdate,
        supportsRuntimeHealthRecheck,
        false,
        false,
        out,
        outCapacity);
}

// Recovery keeps the same updater/session endpoints, without domain features.
inline bool FormatRecoveryInfo(const HardwareRevision hardwareRevision,
                               const bool supportsFirmwareUpdate,
                               char* const out, const size_t outCapacity) {
    if (out == nullptr || outCapacity == 0U) return false;
    const auto mask = static_cast<CapabilityMaskValue>(Capability::FirmwareDigest |
        (supportsFirmwareUpdate ? Capability::FirmwareUpdate : 0U));
    const int written = std::snprintf(out, outCapacity, "{\"i\":[3,%u,%u,1]}",
        static_cast<unsigned>(hardwareRevision), static_cast<unsigned>(mask));
    return written > 0 && static_cast<size_t>(written) < outCapacity && written <= 20;
}

} // namespace modules::ble_device_capabilities
