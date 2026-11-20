#include <cstdio>
#include <cstring>

#include "modules/ble_device_capabilities.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    using namespace modules::ble_device_capabilities;
    std::puts("[BLE_DEVICE_CAPABILITIES] start");

    const uint8_t v1Mask = CapabilityMask(false, false);
    Check((v1Mask & Capability::Console) != 0U,
          "V1 keeps the console capability");
    Check((v1Mask & Capability::Settings) != 0U,
          "V1 keeps the settings capability");
    Check((v1Mask & Capability::Identify) != 0U,
          "V1 keeps the identify capability");
    Check((v1Mask & Capability::FirmwareDigest) != 0U,
          "V1 keeps firmware verification");
    Check((v1Mask & Capability::FirmwareUpdate) == 0U,
          "V1 omits app firmware update");
    Check((v1Mask & Capability::RuntimeHealth) != 0U && v1Mask == 47U,
          "V1 includes the additive runtime-health capability");
    Check((v1Mask & Capability::RuntimeHealthRecheck) == 0U,
          "V1 omits the S3-only health recheck action");

    const uint8_t v2LegacyMask = CapabilityMask(true, false);
    Check(v2LegacyMask == 63U,
          "new firmware can omit recheck without changing prior capabilities");
    const uint8_t v2Mask = CapabilityMask(true, true);
    Check((v2Mask & Capability::FirmwareUpdate) != 0U,
          "V2 keeps app firmware update");
    Check((v2Mask & Capability::RuntimeHealth) != 0U &&
              (v2Mask & Capability::RuntimeHealthRecheck) != 0U &&
              v2Mask == 127U,
          "V2 adds the capability-gated health recheck action");

    const uint16_t historyMask = CapabilityMask(true, true, true, false);
    Check(historyMask == 255U &&
              (historyMask & Capability::GameHistory) != 0U &&
              (historyMask & Capability::CoachInsight) == 0U,
          "contract v1 reserves bit 7 for Game History");
    const uint16_t coachMask = CapabilityMask(true, true, false, true);
    Check(coachMask == 383U &&
              (coachMask & Capability::GameHistory) == 0U &&
              (coachMask & Capability::CoachInsight) != 0U,
          "contract v2 reserves bit 8 for Coach Insight");
    const uint16_t combinedMask = CapabilityMask(true, true, true, true);
    Check(combinedMask == 511U,
          "Game History and Coach Insight coexist without collision");
    Check(static_cast<uint16_t>(Capability::OwnerPairing) == 0x0200U,
          "Owner Pairing reserves bit 9");
    Check((combinedMask & Capability::OwnerPairing) == 0U,
          "Owner Pairing remains unadvertised");
    const uint16_t litchcraftMask = CapabilityMask(
        true, true, false, false, true, true);
    Check((litchcraftMask & Capability::Litchcraft) != 0U &&
              (litchcraftMask & Capability::LitchcraftBulk) != 0U,
          "Litchcraft advertises its capability-gated bulk transfer seam");
    const uint16_t efficientLitchcraftMask = CapabilityMask(
        true, true, false, false, true, true, true);
    Check((efficientLitchcraftMask & Capability::LitchcraftInspect) != 0U &&
              efficientLitchcraftMask == 0x1c7fU,
          "efficient Litchcraft synchronization is additive and explicit");
    const uint16_t paletteLitchcraftMask = CapabilityMask(
        true, true, false, false, true, true, true, true);
    Check((paletteLitchcraftMask & Capability::LitchcraftPalette) != 0U &&
              paletteLitchcraftMask == 0x3c7fU,
          "Litchcraft favorite-palette synchronization has an additive capability bit");
    const uint16_t animatedLitchcraftMask = CapabilityMask(
        true, true, false, false, true, true, true, true, true);
    Check((animatedLitchcraftMask & Capability::LitchcraftAnimation) != 0U &&
              animatedLitchcraftMask == 0x7c7fU,
          "accepted Litchcraft animation has an additive capability bit");
    const uint16_t animationWithoutInspect = CapabilityMask(
        true, true, false, false, true, true, false, true, true);
    Check((animationWithoutInspect & Capability::LitchcraftAnimation) == 0U,
          "Litchcraft animation cannot advertise without efficient inspection");
    const uint16_t shakeEraseMask = CapabilityMask(
        true, true, false, false, true, true, true, true, true, true);
    Check((shakeEraseMask & Capability::LitchcraftShakeErase) != 0U &&
              shakeEraseMask == 0xfc7fU,
          "V3-supported Litchcraft shake erase uses the final extended bit");
    const uint16_t noShakeEraseMask = CapabilityMask(
        true, true, false, false, true, true, true, true, true, false);
    Check((noShakeEraseMask & Capability::LitchcraftShakeErase) == 0U,
          "older or unavailable motion hardware omits shake erase");
    const uint16_t litchcraftWithoutDataLane = CapabilityMask(
        false, true, false, false, true, false);
    Check((litchcraftWithoutDataLane & Capability::Litchcraft) != 0U &&
              (litchcraftWithoutDataLane & Capability::LitchcraftBulk) == 0U,
          "legacy Litchcraft does not advertise an unavailable high-MTU lane");
    Check(HardwareRevisionFromId(1U) == HardwareRevision::V1 &&
              HardwareRevisionFromId(2U) == HardwareRevision::V2 &&
              HardwareRevisionFromId(3U) == HardwareRevision::V3 &&
              HardwareRevisionFromId(4U) ==
                  static_cast<HardwareRevision>(4U),
          "runtime hardware revision IDs map without compile-time assumptions");
    Check(HardwareRevisionFromId(5U) == HardwareRevision::Unknown,
          "unsupported runtime hardware revision remains unknown");

    char v3_3Payload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevisionFromId(4U),
              true, true, false, false,
              v3_3Payload, sizeof(v3_3Payload),
              true, true, true, true, true, true) &&
              std::strcmp(v3_3Payload, "{\"i\":[3,4,64639]}") == 0 &&
              std::strlen(v3_3Payload) <= 20U,
          "sampled V3.3 revision and shake erase survive the serialized BLE capability envelope");

    char v1Payload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V1,
              false,
              false,
              v1Payload,
              sizeof(v1Payload)),
          "V1 capability payload fits the minimum ATT payload");
    Check(std::strcmp(v1Payload, "{\"i\":[3,1,47]}") == 0,
          "new V1-targeted images advertise the session contract epoch");
    Check(std::strlen(v1Payload) <= 20U,
          "V1 compact payload is at most 20 bytes");

    char v2Payload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V2,
              true,
              true,
              v2Payload,
              sizeof(v2Payload)),
          "V2 capability payload fits the minimum ATT payload");
    Check(std::strcmp(v2Payload, "{\"i\":[3,2,127]}") == 0,
          "new V2-targeted images advertise the session contract epoch");
    Check(std::strlen(v2Payload) <= 20U,
          "V2 compact payload is at most 20 bytes");

    char v3Payload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              v3Payload,
              sizeof(v3Payload)),
          "V3 capability payload fits the minimum ATT payload");
    Check(std::strcmp(v3Payload, "{\"i\":[3,3,127]}") == 0,
          "V3 compact payload uses the sampled runtime revision");

    char historyPayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              true,
              false,
              historyPayload,
              sizeof(historyPayload)) &&
              std::strcmp(historyPayload, "{\"i\":[3,3,255]}") == 0,
          "bit 7 remains representable in the session contract epoch");

    char coachPayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              false,
              true,
              coachPayload,
              sizeof(coachPayload)) &&
              std::strcmp(coachPayload, "{\"i\":[3,3,383]}") == 0,
          "bit 8 remains distinct without claiming history");

    char extendedPayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              true,
              true,
              extendedPayload,
              sizeof(extendedPayload)),
          "extended capability payload fits the minimum ATT payload");
    Check(std::strcmp(extendedPayload, "{\"i\":[3,3,511]}") == 0,
          "session epoch advertises distinct history and coach bits");
    Check(std::strlen(extendedPayload) <= 20U,
          "extended compact payload is at most 20 bytes");

    char litchcraftPayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              false,
              false,
              litchcraftPayload,
              sizeof(litchcraftPayload),
              true,
              true) &&
              std::strcmp(litchcraftPayload, "{\"i\":[3,3,3199]}") == 0,
          "bulk Litchcraft capability remains inside minimum ATT info payload");

    char animatedLitchcraftPayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              false,
              false,
              animatedLitchcraftPayload,
              sizeof(animatedLitchcraftPayload),
              true,
              true,
              true,
              true,
              true) &&
              std::strcmp(animatedLitchcraftPayload,
                          "{\"i\":[3,3,31871]}") == 0,
          "animated Litchcraft capability payload remains stable");

    char shakeErasePayload[21] = {};
    Check(FormatCompactInfo(
              HardwareRevision::V3,
              true,
              true,
              false,
              false,
              shakeErasePayload,
              sizeof(shakeErasePayload),
              true,
              true,
              true,
              true,
              true,
              true) &&
              std::strcmp(shakeErasePayload,
                          "{\"i\":[3,3,64639]}") == 0,
          "V3 shake erase capability fits the minimum ATT info payload");

    if (g_failures == 0) {
        std::puts("[BLE_DEVICE_CAPABILITIES] ok");
        return 0;
    }
    std::printf("[BLE_DEVICE_CAPABILITIES] failures=%d\n", g_failures);
    return 1;
}
