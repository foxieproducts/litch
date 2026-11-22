#include "modules/hardware_test_record.hpp"

#include <cstddef>

namespace modules {

namespace {

uint32_t Crc32(const std::string_view bytes) {
    uint32_t crc = 0xFFFFFFFFU;
    for (const char value : bytes) {
        crc ^= static_cast<uint8_t>(value);
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool ParseUnsigned(const std::string_view token,
                   const uint8_t base,
                   uint32_t& out) {
    if (token.empty() || (base != 10U && base != 16U)) {
        return false;
    }
    uint32_t value = 0U;
    for (const char c : token) {
        uint8_t digit = 0U;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint8_t>(c - '0');
        } else if (base == 16U && c >= 'A' && c <= 'F') {
            digit = static_cast<uint8_t>(10U + c - 'A');
        } else if (base == 16U && c >= 'a' && c <= 'f') {
            digit = static_cast<uint8_t>(10U + c - 'a');
        } else {
            return false;
        }
        if (digit >= base || value > (UINT32_MAX - digit) / base) {
            return false;
        }
        value = value * base + digit;
    }
    out = value;
    return true;
}

bool SplitRecord(const std::string_view encoded,
                 std::string_view (&tokens)[9]) {
    size_t start = 0U;
    for (uint8_t index = 0U; index < 9U; ++index) {
        const size_t separator = encoded.find('|', start);
        const bool isLast = index == 8U;
        if ((separator == std::string_view::npos) != isLast) {
            return false;
        }
        const size_t end = isLast ? encoded.size() : separator;
        tokens[index] = encoded.substr(start, end - start);
        if (tokens[index].empty()) {
            return false;
        }
        start = end + 1U;
    }
    return true;
}

bool HasValidFields(const HardwareTestRecord& record) {
    const bool isLegacy =
        record.version == HARDWARE_TEST_RECORD_LEGACY_DUAL_RADIO_VERSION;
    const uint8_t requiredMask = isLegacy
        ? HARDWARE_TEST_RECORD_LEGACY_DUAL_RADIO_MASK : FACTORY_RADIO_ALL_CHANNELS;
    if ((!isLegacy && record.version != HARDWARE_TEST_RECORD_VERSION) ||
        record.revision == hardware::BoardRevision::Unknown ||
        record.radioChannelMask > requiredMask) {
        return false;
    }
    if (record.radioResult == ManufacturingTestRadioResult::Bypassed) {
        return record.radioChannelMask == 0U;
    }
    if (record.radioResult == ManufacturingTestRadioResult::Forced ||
        record.radioResult == ManufacturingTestRadioResult::Override) {
        return record.radioChannelMask == 0U &&
               !record.factoryVerifierEnabled;
    }
    if (record.radioResult == ManufacturingTestRadioResult::Verified ||
        record.radioResult == ManufacturingTestRadioResult::PeerVerified) {
        return record.radioChannelMask == requiredMask;
    }
    return false;
}

} // namespace

bool EncodeHardwareTestRecord(const HardwareTestRecord& record,
                              EncodedHardwareTestRecord& out) {
    if (!HasValidFields(record)) {
        out.Clear();
        return false;
    }
    EncodedHardwareTestRecord prefix;
    if (!prefix.Format("%u|%u|%u|%u|%u|%lu|%lu|%lu",
                       static_cast<unsigned>(HARDWARE_TEST_RECORD_VERSION),
                       static_cast<unsigned>(record.revision),
                       static_cast<unsigned>(record.radioResult),
                       static_cast<unsigned>(
                           record.factoryVerifierEnabled ? 1U : 0U),
                       static_cast<unsigned>(IsVerifiedFactoryPass(record)
                           ? FACTORY_RADIO_BLE_CHANNEL : record.radioChannelMask),
                       static_cast<unsigned long>(record.automatedDurationMs),
                       static_cast<unsigned long>(record.operatorDurationMs),
                       static_cast<unsigned long>(record.radioDurationMs))) {
        return false;
    }
    fxe::FixedString<16> suffix;
    if (!suffix.Format("|%08lX",
                       static_cast<unsigned long>(Crc32(prefix.View())))) {
        return false;
    }
    out = prefix;
    return out.Append(suffix.View());
}

bool WriteHardwareTestRecord(JsonDocument& doc,
                             const HardwareTestRecord& record) {
    EncodedHardwareTestRecord encoded;
    if (!EncodeHardwareTestRecord(record, encoded)) {
        return false;
    }
    // ArduinoJson links const char* values as if they had static lifetime.
    // FixedString is local here, so pass a mutable pointer to request an owned
    // copy in the document's pool.
    return doc[HARDWARE_TEST_RECORD_SETTING_KEY].set(
        const_cast<char*>(encoded.CStr()));
}

bool DecodeHardwareTestRecord(const std::string_view encoded,
                              HardwareTestRecord& out) {
    std::string_view tokens[9];
    if (!SplitRecord(encoded, tokens)) {
        return false;
    }
    uint32_t values[9] = {};
    for (uint8_t index = 0U; index < 8U; ++index) {
        if (!ParseUnsigned(tokens[index], 10U, values[index])) {
            return false;
        }
    }
    if (!ParseUnsigned(tokens[8], 16U, values[8])) {
        return false;
    }
    const size_t checksumSeparator = encoded.rfind('|');
    if (checksumSeparator == std::string_view::npos ||
        Crc32(encoded.substr(0U, checksumSeparator)) != values[8]) {
        return false;
    }
    if (values[0] > UINT8_MAX || values[1] > 4U || values[2] > 5U ||
        values[3] > 1U || values[4] > UINT8_MAX) return false;
    HardwareTestRecord candidate{
        static_cast<uint8_t>(values[0]),
        hardware::BoardRevisionFromId(static_cast<uint8_t>(values[1])),
        static_cast<ManufacturingTestRadioResult>(values[2]),
        values[3] == 1U,
        static_cast<uint8_t>(values[4]),
        values[5],
        values[6],
        values[7],
    };
    if (values[3] > 1U || !HasValidFields(candidate)) {
        return false;
    }
    out = candidate;
    return true;
}

bool IsVerifiedFactoryPass(const HardwareTestRecord& record) {
    return HasValidFields(record) &&
        (record.radioResult == ManufacturingTestRadioResult::Verified ||
         record.radioResult == ManufacturingTestRadioResult::PeerVerified);
}

bool CanEnableFactoryVerifier(const HardwareTestRecord& record) {
    return IsVerifiedFactoryPass(record);
}

bool IsFactoryVerifierEligible(const HardwareTestRecord& record) {
    return record.factoryVerifierEnabled && CanEnableFactoryVerifier(record);
}

} // namespace modules
