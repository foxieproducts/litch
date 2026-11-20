#if defined(LITCH_ESP32S3)

#include "modules/ota_boot_policy.hpp"

// The pinned S3 linker places this section after the 256-byte SDK appdesc.
// The normal S3 link explicitly retains this symbol under --gc-sections.
extern "C" {
extern const std::array<uint8_t, modules::OTA_BOOT_POLICY_DESCRIPTOR_BYTES>
    litch_boot_policy_descriptor
        __attribute__((section(".rodata_custom_desc"), aligned(4))) =
            modules::EncodeOtaBootPolicyDescriptor(
                modules::OTA_BOOT_POLICY_CURRENT_EPOCH);
}

static_assert(sizeof(litch_boot_policy_descriptor) ==
              modules::OTA_BOOT_POLICY_DESCRIPTOR_BYTES);

#endif
