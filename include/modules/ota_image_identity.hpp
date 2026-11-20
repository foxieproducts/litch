#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "modules/ota_boot_policy.hpp"
#include "modules/ota_post_boot_verifier.hpp"
#include "modules/sha256.hpp"

namespace modules {

/** Exact identities collected from one complete ESP application image. */
struct OtaImageIdentity {
    std::array<uint8_t, OTA_POST_BOOT_BUILD_ID_BYTES> buildId{};
    uint8_t buildIdLength = 0U;
    std::array<uint8_t, OTA_POST_BOOT_SHA256_BYTES> imageSha256{};
    OtaBootPolicy bootPolicy{};

    [[nodiscard]] std::string_view BuildIdView() const {
        return std::string_view(
            reinterpret_cast<const char*>(buildId.data()), buildIdLength);
    }
};

/** Bounded read-only partition view; a failed read supplies no image evidence. */
struct OtaImagePartitionReader {
    uint32_t capacity = 0U;
    void* context = nullptr;
    bool (*read)(void* context, uint32_t offset, uint8_t* out, size_t size) =
        nullptr;
};

/**
 * Bounded parser for an unsigned ESP image stream.
 *
 * It validates image/segment framing, checksum padding, the XOR checksum, and
 * the exact appended-SHA boundary and digest. The build identity is the raw 32-byte
 * esp_app_desc.app_elf_sha256 field; esp_app_desc.version belongs to the
 * framework build in the pinned Arduino core and is not a Litch identity.
 * Optional boot-policy evidence is published only after complete integrity
 * verification; absent or unrecognized policy never rejects a legacy image.
 */
class OtaImageIdentityCollector final {
  public:
    static constexpr size_t ESP_IMAGE_HEADER_BYTES = 24U;
    static constexpr size_t ESP_SEGMENT_HEADER_BYTES = 8U;
    static constexpr size_t ESP_APP_DESCRIPTOR_BYTES = 256U;
    static constexpr size_t ESP_APP_ELF_SHA_OFFSET = 144U;
    static constexpr uint8_t ESP_MAX_SEGMENTS = 16U;

    [[nodiscard]] bool Begin(uint32_t expectedSize, uint16_t expectedChipId);
    void Abort();
    [[nodiscard]] bool Append(const uint8_t* data, size_t len);
    [[nodiscard]] bool Finish(OtaImageIdentity& outIdentity) const;
    /** Verify the complete image, excluding unused partition tail bytes. */
    [[nodiscard]] bool ReadPartition(const OtaImagePartitionReader& reader,
                                     uint16_t expectedChipId,
                                     OtaImageIdentity& outIdentity);

  private:
    enum class Stage : uint8_t {
        Idle = 0U,
        ImageHeader,
        SegmentHeader,
        SegmentData,
        Padding,
        Checksum,
        Digest,
        Complete,
        Failed,
    };

    [[nodiscard]] bool Consume(uint8_t byte);
    [[nodiscard]] bool CompleteImageHeader();
    [[nodiscard]] bool CompleteSegmentHeader();
    [[nodiscard]] size_t NextReadSize() const;
    void Fail();

    Stage m_stage = Stage::Idle;
    uint32_t m_expectedSize = 0U;
    uint32_t m_receivedSize = 0U;
    bool m_partitionBounded = false;
    uint16_t m_expectedChipId = 0U;
    uint8_t m_segmentCount = 0U;
    uint8_t m_segmentIndex = 0U;
    uint32_t m_segmentDataOffset = 0U;
    uint32_t m_segmentRemaining = 0U;
    uint32_t m_paddingRemaining = 0U;
    uint8_t m_checksum = 0xEFU;
    std::array<uint8_t, ESP_IMAGE_HEADER_BYTES> m_imageHeader{};
    size_t m_imageHeaderLength = 0U;
    std::array<uint8_t, ESP_SEGMENT_HEADER_BYTES> m_segmentHeader{};
    size_t m_segmentHeaderLength = 0U;
    std::array<uint8_t, 4U> m_descriptorMagic{};
    std::array<uint8_t, OTA_BOOT_POLICY_DESCRIPTOR_BYTES> m_bootPolicyBytes{};
    size_t m_bootPolicyLength = 0U;
    std::array<uint8_t, OTA_POST_BOOT_SHA256_BYTES> m_buildId{};
    std::array<uint8_t, OTA_POST_BOOT_SHA256_BYTES> m_imageSha256{};
    std::array<uint8_t, OTA_POST_BOOT_SHA256_BYTES> m_computedImageSha256{};
    size_t m_digestLength = 0U;
    Sha256 m_sha256;
};

} // namespace modules
