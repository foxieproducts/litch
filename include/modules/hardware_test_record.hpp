#pragma once

#include <cstdint>
#include <string_view>

#include <ArduinoJson.h>

#include "foxie_embedded/fixed_string.hpp"
#include "modules/manufacturing_test_session.hpp"

namespace modules {

inline constexpr uint8_t HARDWARE_TEST_RECORD_VERSION = 2U;
inline constexpr uint8_t FACTORY_RADIO_BLE_CHANNEL = 1U << 0U;
inline constexpr uint8_t FACTORY_RADIO_ALL_CHANNELS = FACTORY_RADIO_BLE_CHANNEL;
// Version 1 required both transports. These constants only validate stored
// evidence; new qualification and writes require BLE alone.
inline constexpr uint8_t HARDWARE_TEST_RECORD_LEGACY_DUAL_RADIO_VERSION = 1U;
inline constexpr uint8_t HARDWARE_TEST_RECORD_LEGACY_DUAL_RADIO_MASK = 3U;
inline constexpr char HARDWARE_TEST_RECORD_SETTING_KEY[] =
    "HARDWARE_TEST_RECORD";

struct HardwareTestRecord {
    uint8_t version = HARDWARE_TEST_RECORD_VERSION;
    hardware::BoardRevision revision = hardware::BoardRevision::Unknown;
    ManufacturingTestRadioResult radioResult =
        ManufacturingTestRadioResult::Unverified;
    bool factoryVerifierEnabled = false;
    uint8_t radioChannelMask = 0U;
    uint32_t automatedDurationMs = 0U;
    uint32_t operatorDurationMs = 0U;
    uint32_t radioDurationMs = 0U;
};

using EncodedHardwareTestRecord = fxe::FixedString<96>;

// Writes the current version, retaining valid historical results and durations.
// A historical verified dual-radio record already contains genuine BLE proof.
bool EncodeHardwareTestRecord(const HardwareTestRecord& record,
                              EncodedHardwareTestRecord& out);
bool WriteHardwareTestRecord(JsonDocument& doc,
                             const HardwareTestRecord& record);
bool DecodeHardwareTestRecord(std::string_view encoded,
                              HardwareTestRecord& out);

bool IsVerifiedFactoryPass(const HardwareTestRecord& record);

bool CanEnableFactoryVerifier(const HardwareTestRecord& record);
bool IsFactoryVerifierEligible(const HardwareTestRecord& record);

} // namespace modules
