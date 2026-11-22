#include <cstdio>

#include "modules/hardware_test_record.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[HARDWARE_TEST_RECORD] start");

    modules::HardwareTestRecord record;
    record.revision = hardware::BoardRevision::V3S3;
    record.radioResult = modules::ManufacturingTestRadioResult::Bypassed;
    record.factoryVerifierEnabled = true;
    record.radioChannelMask = 0U;
    record.automatedDurationMs = 1780U;
    record.operatorDurationMs = 7200U;
    record.radioDurationMs = 840U;

    modules::EncodedHardwareTestRecord encoded;
    Check(modules::EncodeHardwareTestRecord(record, encoded),
          "versioned factory record encodes without allocation");

    StaticJsonDocument<256> doc;
    Check(modules::WriteHardwareTestRecord(doc, record),
          "factory record writes to the settings document");
    const char* const stored =
        doc[modules::HARDWARE_TEST_RECORD_SETTING_KEY].as<const char*>();
    Check(stored != nullptr &&
              ArduinoJson::detail::VariantAttorney::getPool(doc)->owns(
                  const_cast<char*>(stored)),
          "settings document owns the factory record after its encoder returns");
    modules::HardwareTestRecord storedDecoded;
    Check(stored != nullptr &&
              modules::DecodeHardwareTestRecord(stored, storedDecoded),
          "owned factory record remains decodable after its encoder returns");

    modules::HardwareTestRecord decoded;
    Check(modules::DecodeHardwareTestRecord(encoded.View(), decoded),
          "valid factory record decodes");
    Check(decoded.version == 2U && decoded.revision == hardware::BoardRevision::V3S3 &&
              decoded.radioResult ==
                  modules::ManufacturingTestRadioResult::Bypassed,
          "record preserves revision and explicit bypass result");
    Check(!modules::IsFactoryVerifierEligible(decoded) && !modules::CanEnableFactoryVerifier(decoded),
          "historical bypass never authorizes a saved green verifier service");

    modules::EncodedHardwareTestRecord corrupted = encoded;
    modules::EncodedHardwareTestRecord replacement(
        "1|3|2|1|0|1780|7201|840|00000000");
    corrupted = replacement;
    Check(!modules::DecodeHardwareTestRecord(corrupted.View(), decoded),
          "corrupted or edited record cannot authorize a verifier");

    record.factoryVerifierEnabled = false;
    Check(!modules::IsFactoryVerifierEligible(record),
          "operator can explicitly disable factory verifier availability");

    modules::HardwareTestRecord ordinary = record;
    ordinary.factoryVerifierEnabled = true;
    ordinary.radioResult = modules::ManufacturingTestRadioResult::Verified;
    ordinary.radioChannelMask = modules::FACTORY_RADIO_ALL_CHANNELS;
    Check(modules::IsFactoryVerifierEligible(ordinary),
          "verified passed board may serve the batch until explicit exit");

    for (const char* historical : {
             "1|3|1|1|3|1780|7200|840|66FDDA36",
             "1|3|3|1|3|1780|7200|840|1249DCC7"}) {
        modules::HardwareTestRecord legacy;
        Check(modules::DecodeHardwareTestRecord(historical, legacy) &&
                  legacy.version == 1U && legacy.radioChannelMask == 3U &&
                  modules::IsVerifiedFactoryPass(legacy) &&
                  modules::IsFactoryVerifierEligible(legacy),
              "genuine historical dual-radio proof remains readable and eligible");
        Check(modules::EncodeHardwareTestRecord(legacy, encoded) &&
                  modules::DecodeHardwareTestRecord(encoded.View(), decoded) &&
                  decoded.version == 2U && decoded.radioChannelMask == 1U &&
                  decoded.radioResult == legacy.radioResult &&
                  decoded.automatedDurationMs == 1780U && decoded.operatorDurationMs == 7200U &&
                  decoded.radioDurationMs == 840U && legacy.version == 1U,
              "next save retains historical proof and timings in the current BLE-only format");
    }

    modules::HardwareTestRecord legacyBypass;
    Check(modules::DecodeHardwareTestRecord(
              "1|3|2|1|0|1780|7200|840|D4D636E6", legacyBypass) &&
              !modules::IsVerifiedFactoryPass(legacyBypass) &&
              !modules::IsFactoryVerifierEligible(legacyBypass),
          "legacy bootstrap remains readable without becoming a verified PASS");
    Check(modules::EncodeHardwareTestRecord(legacyBypass, encoded) &&
              modules::DecodeHardwareTestRecord(encoded.View(), decoded) &&
              decoded.version == 2U && decoded.radioChannelMask == 0U &&
              decoded.radioResult == modules::ManufacturingTestRadioResult::Bypassed &&
              !modules::IsVerifiedFactoryPass(decoded),
          "saving historical bypass in current format cannot turn it into PASS");


    for (const char* invalid : {
             "1|3|1|1|1|1780|7200|840|CE7B6BA7",
             "2|3|1|1|3|1780|7200|840|1018E30B",
             "1|3|0|0|0|0|0|0|3D4450A9"}) {
        Check(!modules::DecodeHardwareTestRecord(invalid, decoded),
              "valid CRC cannot admit mismatched proof masks or an originally invalid unverified record");
    }

    for (const char* historical : {
             "1|3|4|0|0|0|0|0|8D9D232B",
             "1|3|5|0|0|0|0|0|4C13FCEB"}) {
        Check(modules::DecodeHardwareTestRecord(historical, decoded) &&
                  decoded.version == 1U && !modules::IsVerifiedFactoryPass(decoded) &&
                  !modules::IsFactoryVerifierEligible(decoded),
              "historical Forced and Override bytes stay readable without qualifying as PASS");
    }

    for (auto result : {modules::ManufacturingTestRadioResult::Forced, modules::ManufacturingTestRadioResult::Override}) {
        modules::HardwareTestRecord bypass{1U, hardware::BoardRevision::V3S3, result, false, 0U, 0U, 0U, 0U};
        Check(modules::EncodeHardwareTestRecord(bypass, encoded) && modules::DecodeHardwareTestRecord(encoded.View(), decoded) &&
              decoded.version == 2U && decoded.radioResult == result && !modules::IsVerifiedFactoryPass(decoded) && !modules::IsFactoryVerifierEligible(decoded),
              "old Forced and new Override remain readable, distinct and never verified PASS");
    }
    if (g_failures == 0) {
        std::puts("[HARDWARE_TEST_RECORD] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
