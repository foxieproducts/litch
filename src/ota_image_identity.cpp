#include "modules/ota_image_identity.hpp"

#include <algorithm>
#include <limits>

namespace modules {
namespace {

constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9U;
constexpr uint32_t ESP_APP_DESC_MAGIC = 0xABCD5432U;
constexpr size_t CHIP_ID_OFFSET = 12U;
constexpr size_t HASH_APPENDED_OFFSET = 23U;
constexpr size_t SEGMENT_LENGTH_OFFSET = 4U;
constexpr size_t CHECKSUM_ALIGNMENT = 16U;
uint16_t ReadLe16(const uint8_t* const data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t ReadLe32(const uint8_t* const data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) |
           (static_cast<uint32_t>(data[3]) << 24U);
}

OtaBootPolicy ReadBootPolicy(
    const std::array<uint8_t, OTA_BOOT_POLICY_DESCRIPTOR_BYTES>& bytes,
    const size_t length) {
    OtaBootPolicy result;
    result.status = OtaBootPolicyStatus::Absent;
    if (length < 4U || bytes[0U] != 'F' || bytes[1U] != 'X' ||
        bytes[2U] != 'B' || bytes[3U] != 'T') {
        return result;
    }
    if (length >= 8U) {
        result.schema = ReadLe16(bytes.data() + 4U);
        result.size = ReadLe16(bytes.data() + 6U);
    }
    result.status = OtaBootPolicyStatus::Malformed;
    if (length < bytes.size()) {
        return result;
    }
    if (result.schema != OTA_BOOT_POLICY_SCHEMA) {
        result.status = OtaBootPolicyStatus::Unsupported;
        return result;
    }
    if (result.size != bytes.size() || ReadLe32(bytes.data() + 12U) != 0U) {
        return result;
    }
    result.epoch = ReadLe32(bytes.data() + 8U);
    result.status = result.epoch == OTA_BOOT_POLICY_RECORD_EPOCH ||
                            result.epoch == OTA_BOOT_POLICY_SDK_EPOCH
                        ? OtaBootPolicyStatus::Known
                        : OtaBootPolicyStatus::Unsupported;
    return result;
}

template <size_t Size>
bool IsAllZero(const std::array<uint8_t, Size>& bytes) {
    for (const uint8_t byte : bytes) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace

bool OtaImageIdentityCollector::Begin(const uint32_t expectedSize,
                                      const uint16_t expectedChipId) {
    Abort();
    constexpr uint32_t MINIMUM_IMAGE_BYTES =
        ESP_IMAGE_HEADER_BYTES + ESP_SEGMENT_HEADER_BYTES +
        ESP_APP_DESCRIPTOR_BYTES + 1U + OTA_POST_BOOT_SHA256_BYTES;
    if (expectedSize < MINIMUM_IMAGE_BYTES || expectedChipId == 0U) {
        return false;
    }
    m_expectedSize = expectedSize;
    m_expectedChipId = expectedChipId;
    m_sha256.Reset();
    m_stage = Stage::ImageHeader;
    return true;
}

void OtaImageIdentityCollector::Abort() {
    m_stage = Stage::Idle;
    m_expectedSize = 0U;
    m_receivedSize = 0U;
    m_partitionBounded = false;
    m_expectedChipId = 0U;
    m_segmentCount = 0U;
    m_segmentIndex = 0U;
    m_segmentDataOffset = 0U;
    m_segmentRemaining = 0U;
    m_paddingRemaining = 0U;
    m_checksum = 0xEFU;
    m_imageHeader.fill(0U);
    m_imageHeaderLength = 0U;
    m_segmentHeader.fill(0U);
    m_segmentHeaderLength = 0U;
    m_descriptorMagic.fill(0U);
    m_bootPolicyBytes.fill(0U);
    m_bootPolicyLength = 0U;
    m_buildId.fill(0U);
    m_imageSha256.fill(0U);
    m_computedImageSha256.fill(0U);
    m_digestLength = 0U;
    m_sha256.Reset();
}

void OtaImageIdentityCollector::Fail() {
    m_stage = Stage::Failed;
}

bool OtaImageIdentityCollector::CompleteImageHeader() {
    if (m_imageHeader[0] != ESP_IMAGE_MAGIC ||
        m_imageHeader[1] == 0U ||
        m_imageHeader[1] > ESP_MAX_SEGMENTS ||
        ReadLe16(m_imageHeader.data() + CHIP_ID_OFFSET) != m_expectedChipId ||
        m_imageHeader[HASH_APPENDED_OFFSET] != 1U) {
        return false;
    }
    m_segmentCount = m_imageHeader[1];
    m_stage = Stage::SegmentHeader;
    return true;
}

bool OtaImageIdentityCollector::CompleteSegmentHeader() {
    const uint32_t segmentLength =
        ReadLe32(m_segmentHeader.data() + SEGMENT_LENGTH_OFFSET);
    const uint32_t consumedAfterHeader = m_receivedSize + 1U;
    if (segmentLength == 0U || (segmentLength & 3U) != 0U ||
        consumedAfterHeader > m_expectedSize ||
        segmentLength > m_expectedSize - consumedAfterHeader) {
        return false;
    }
    if (m_segmentIndex == 0U && segmentLength < ESP_APP_DESCRIPTOR_BYTES) {
        return false;
    }
    m_segmentRemaining = segmentLength;
    m_segmentDataOffset = 0U;
    m_stage = Stage::SegmentData;
    return true;
}

bool OtaImageIdentityCollector::Consume(const uint8_t byte) {
    switch (m_stage) {
        case Stage::ImageHeader:
            m_imageHeader[m_imageHeaderLength++] = byte;
            if (m_imageHeaderLength == m_imageHeader.size() &&
                !CompleteImageHeader()) {
                return false;
            }
            return true;

        case Stage::SegmentHeader:
            m_segmentHeader[m_segmentHeaderLength++] = byte;
            if (m_segmentHeaderLength == m_segmentHeader.size()) {
                m_segmentHeaderLength = 0U;
                if (!CompleteSegmentHeader()) {
                    return false;
                }
            }
            return true;

        case Stage::SegmentData:
            m_checksum ^= byte;
            if (m_segmentIndex == 0U) {
                if (m_segmentDataOffset < m_descriptorMagic.size()) {
                    m_descriptorMagic[m_segmentDataOffset] = byte;
                }
                if (m_segmentDataOffset >= ESP_APP_ELF_SHA_OFFSET &&
                    m_segmentDataOffset <
                        ESP_APP_ELF_SHA_OFFSET + m_buildId.size()) {
                    m_buildId[m_segmentDataOffset - ESP_APP_ELF_SHA_OFFSET] =
                        byte;
                }
                if (m_segmentDataOffset >= ESP_APP_DESCRIPTOR_BYTES &&
                    m_segmentDataOffset <
                        ESP_APP_DESCRIPTOR_BYTES + m_bootPolicyBytes.size()) {
                    m_bootPolicyBytes[m_bootPolicyLength++] = byte;
                }
            }
            ++m_segmentDataOffset;
            --m_segmentRemaining;
            if (m_segmentRemaining == 0U) {
                ++m_segmentIndex;
                if (m_segmentIndex < m_segmentCount) {
                    m_stage = Stage::SegmentHeader;
                } else {
                    const uint32_t dataEnd = m_receivedSize + 1U;
                    if (dataEnd >
                        std::numeric_limits<uint32_t>::max() -
                            CHECKSUM_ALIGNMENT) {
                        return false;
                    }
                    const uint32_t checksumEnd =
                        (dataEnd + CHECKSUM_ALIGNMENT) &
                        ~(static_cast<uint32_t>(CHECKSUM_ALIGNMENT) - 1U);
                    if (m_partitionBounded) {
                        if (checksumEnd > m_expectedSize ||
                            m_expectedSize - checksumEnd <
                                OTA_POST_BOOT_SHA256_BYTES) {
                            return false;
                        }
                        m_expectedSize =
                            checksumEnd + OTA_POST_BOOT_SHA256_BYTES;
                    }
                    m_paddingRemaining = checksumEnd - dataEnd - 1U;
                    m_stage = m_paddingRemaining == 0U
                                  ? Stage::Checksum
                                  : Stage::Padding;
                }
            }
            return true;

        case Stage::Padding:
            if (byte != 0U || m_paddingRemaining == 0U) {
                return false;
            }
            --m_paddingRemaining;
            if (m_paddingRemaining == 0U) {
                m_stage = Stage::Checksum;
            }
            return true;

        case Stage::Checksum:
            if (byte != m_checksum) {
                return false;
            }
            m_computedImageSha256 = m_sha256.Finish();
            m_stage = Stage::Digest;
            return true;

        case Stage::Digest:
            m_imageSha256[m_digestLength++] = byte;
            if (m_digestLength == m_imageSha256.size()) {
                m_stage = Stage::Complete;
            }
            return true;

        case Stage::Idle:
        case Stage::Complete:
        case Stage::Failed:
        default:
            return false;
    }
}

bool OtaImageIdentityCollector::Append(const uint8_t* const data,
                                       const size_t len) {
    if (m_stage == Stage::Idle || m_stage == Stage::Failed ||
        m_stage == Stage::Complete || data == nullptr || len == 0U ||
        m_receivedSize > m_expectedSize ||
        len > static_cast<size_t>(m_expectedSize - m_receivedSize)) {
        Fail();
        return false;
    }
    size_t offset = 0U;
    while (offset < len) {
        const size_t size = std::min(len - offset, NextReadSize());
        if (size == 0U) {
            Fail();
            return false;
        }
        // Stay within this parser stage so the checksum is hashed before
        // Consume finishes SHA, while the appended digest is excluded.
        if (m_stage != Stage::Digest &&
            !m_sha256.Update(data + offset, size)) {
            Fail();
            return false;
        }
        for (size_t index = 0U; index < size; ++index) {
            if (!Consume(data[offset + index])) {
                Fail();
                return false;
            }
            ++m_receivedSize;
        }
        offset += size;
    }
    return true;
}

bool OtaImageIdentityCollector::Finish(OtaImageIdentity& outIdentity) const {
    if (m_stage != Stage::Complete || m_receivedSize != m_expectedSize ||
        ReadLe32(m_descriptorMagic.data()) != ESP_APP_DESC_MAGIC ||
        IsAllZero(m_buildId) || IsAllZero(m_imageSha256) ||
        m_imageSha256 != m_computedImageSha256) {
        return false;
    }
    OtaImageIdentity identity;
    for (size_t index = 0U; index < m_buildId.size(); ++index) {
        identity.buildId[index] = m_buildId[index];
    }
    identity.buildIdLength =
        static_cast<uint8_t>(OTA_POST_BOOT_SHA256_BYTES);
    identity.imageSha256 = m_imageSha256;
    identity.bootPolicy = ReadBootPolicy(m_bootPolicyBytes, m_bootPolicyLength);
    outIdentity = identity;
    return true;
}

size_t OtaImageIdentityCollector::NextReadSize() const {
    switch (m_stage) {
        case Stage::ImageHeader:
            return m_imageHeader.size() - m_imageHeaderLength;
        case Stage::SegmentHeader:
            return m_segmentHeader.size() - m_segmentHeaderLength;
        case Stage::SegmentData:
            return m_segmentRemaining;
        case Stage::Padding:
            return m_paddingRemaining;
        case Stage::Checksum:
            return 1U;
        case Stage::Digest:
            return m_imageSha256.size() - m_digestLength;
        default:
            return 0U;
    }
}

bool OtaImageIdentityCollector::ReadPartition(
    const OtaImagePartitionReader& reader,
    const uint16_t expectedChipId,
    OtaImageIdentity& outIdentity) {
    if (reader.read == nullptr || !Begin(reader.capacity, expectedChipId)) {
        Abort();
        return false;
    }
    m_partitionBounded = true;
    std::array<uint8_t, 512U> buffer{};
    while (m_stage != Stage::Complete) {
        const size_t size = std::min(buffer.size(), NextReadSize());
        if (size == 0U || m_receivedSize > m_expectedSize ||
            size > m_expectedSize - m_receivedSize ||
            !reader.read(reader.context, m_receivedSize, buffer.data(), size) ||
            !Append(buffer.data(), size)) {
            Fail();
            return false;
        }
    }
    return Finish(outIdentity);
}

} // namespace modules
