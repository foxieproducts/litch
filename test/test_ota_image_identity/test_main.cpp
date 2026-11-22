#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "modules/ota_image_identity.hpp"

namespace {

constexpr uint16_t ESP32S3_CHIP_ID = 9U;
constexpr size_t IMAGE_BYTES = 384U;
constexpr size_t APP_DESCRIPTOR_OFFSET = 32U;
constexpr size_t APP_ELF_SHA_OFFSET = APP_DESCRIPTOR_OFFSET + 144U;
constexpr size_t SECOND_SEGMENT_HEADER_OFFSET = 288U;
constexpr size_t SECOND_SEGMENT_DATA_OFFSET = 296U;
constexpr size_t CHECKSUM_OFFSET = 351U;
constexpr size_t IMAGE_SHA_OFFSET = 352U;

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

void RefreshChecksum(std::array<uint8_t, IMAGE_BYTES>& image) {
    uint8_t checksum = 0xEFU;
    for (size_t index = APP_DESCRIPTOR_OFFSET; index < 288U; ++index) {
        checksum ^= image[index];
    }
    for (size_t index = SECOND_SEGMENT_DATA_OFFSET; index < 344U; ++index) {
        checksum ^= image[index];
    }
    image[CHECKSUM_OFFSET] = checksum;
}

uint8_t HexNibble(const char digit) {
    return digit <= '9' ? static_cast<uint8_t>(digit - '0')
                        : static_cast<uint8_t>(digit - 'a' + 10);
}

const char* ExpectedDigestHex(const uint8_t identityBase) {
    if (identityBase == 0x20U) {
        return "8b9f7242252dab5a468e0eec890fd5cafa9f6c88eb6e7aba42d2ae78bb9c0c2b";
    }
    if (identityBase == 0x60U) {
        return "a8cecf91c84d6ad0fdd94eb2b9999dc1459af1724f5ff90b2b76e5035073c222";
    }
    return "003a3aaf917dab89611a1b9d381b92d79425f2e60b261326e1458cdcf07fb266";
}

void SetDigestFromHex(std::array<uint8_t, IMAGE_BYTES>& image,
                      const char* const digest) {
    for (size_t index = 0U; index < 32U; ++index) {
        image[IMAGE_SHA_OFFSET + index] = static_cast<uint8_t>(
            (HexNibble(digest[index * 2U]) << 4U) |
            HexNibble(digest[index * 2U + 1U]));
    }
}

std::array<uint8_t, IMAGE_BYTES> MakeImage(const uint8_t identityBase = 0x40U) {
    std::array<uint8_t, IMAGE_BYTES> image{};
    image[0U] = 0xE9U;
    image[1U] = 2U;
    image[12U] = static_cast<uint8_t>(ESP32S3_CHIP_ID & 0xFFU);
    image[13U] = static_cast<uint8_t>(ESP32S3_CHIP_ID >> 8U);
    image[23U] = 1U;
    image[24U] = 0x20U;
    image[27U] = 0x3CU;
    image[28U] = 0x00U;
    image[29U] = 0x01U;
    image[32U] = 0x32U;
    image[33U] = 0x54U;
    image[34U] = 0xCDU;
    image[35U] = 0xABU;
    for (size_t index = 0U; index < 32U; ++index) {
        image[APP_ELF_SHA_OFFSET + index] =
            static_cast<uint8_t>(identityBase + index);
    }
    image[SECOND_SEGMENT_HEADER_OFFSET + 1U] = 0x80U;
    image[SECOND_SEGMENT_HEADER_OFFSET + 2U] = 0xC8U;
    image[SECOND_SEGMENT_HEADER_OFFSET + 3U] = 0x3FU;
    image[SECOND_SEGMENT_HEADER_OFFSET + 4U] = 48U;
    for (size_t index = 0U; index < 48U; ++index) {
        image[SECOND_SEGMENT_DATA_OFFSET + index] =
            static_cast<uint8_t>(0xD0U + index);
    }
    RefreshChecksum(image);
    SetDigestFromHex(image, ExpectedDigestHex(identityBase));
    return image;
}

struct PartitionFixture {
    template <typename Image>
    explicit PartitionFixture(const Image& image)
        : data(image.data()), size(image.size()), failAt(image.size()) {}

    const uint8_t* data;
    size_t size;
    size_t failAt;
    size_t lastEnd = 0U;
    size_t maxRead = 0U;
    size_t reads = 0U;

    static bool Read(void* context, const uint32_t offset,
                     uint8_t* out, const size_t size) {
        auto& self = *static_cast<PartitionFixture*>(context);
        ++self.reads;
        if (offset > self.failAt || size > self.failAt - offset ||
            offset > self.size || size > self.size - offset) {
            return false;
        }
        self.lastEnd = offset + size;
        self.maxRead = size > self.maxRead ? size : self.maxRead;
        std::memcpy(out, self.data + offset, size);
        return true;
    }
};

bool TestPartitionReadsExactImageAndFailsClosed() {
    auto image = MakeImage();
    PartitionFixture fixture{image};
    modules::OtaImagePartitionReader reader{
        IMAGE_BYTES + 4096U, &fixture, PartitionFixture::Read};
    modules::OtaImageIdentityCollector collector;
    modules::OtaImageIdentity identity;
    if (!Expect(collector.ReadPartition(reader, ESP32S3_CHIP_ID, identity),
                "partition verifies full image without reading erased tail") ||
        !Expect(fixture.lastEnd == IMAGE_BYTES && fixture.maxRead <= 512U,
                "partition reads stop at exact appended hash boundary") ||
        !Expect(identity.buildId[0U] == 0x40U &&
                    identity.imageSha256[0U] == image[IMAGE_SHA_OFFSET],
                "partition retains distinct ELF and verified image identities")) {
        return false;
    }
    for (const size_t failAt : {0U, 25U, 175U, 343U, 351U, 383U}) {
        fixture.failAt = failAt;
        modules::OtaImageIdentity unpublished = identity;
        if (!Expect(!collector.ReadPartition(reader, ESP32S3_CHIP_ID,
                                             unpublished),
                    "failed partition read publishes no image evidence") ||
            !Expect(unpublished.imageSha256 == identity.imageSha256,
                    "read failure leaves output owned by caller")) {
            return false;
        }
    }
    fixture.failAt = IMAGE_BYTES;
    reader.capacity = IMAGE_BYTES - 1U;
    if (!Expect(!collector.ReadPartition(reader, ESP32S3_CHIP_ID, identity),
                "partition cannot fit truncated appended hash")) {
        return false;
    }
    reader.capacity = IMAGE_BYTES + 4096U;
    image[SECOND_SEGMENT_DATA_OFFSET] ^= 1U;
    image[SECOND_SEGMENT_DATA_OFFSET + 1U] ^= 1U;
    return Expect(!collector.ReadPartition(reader, ESP32S3_CHIP_ID, identity),
                  "same-XOR corruption still fails full image SHA") &&
           Expect(!collector.Begin(0U, ESP32S3_CHIP_ID),
                  "strict ingress size checks remain in force");
}

template <typename Image>
bool AppendPattern(modules::OtaImageIdentityCollector& collector,
                   const Image& image,
                   const size_t pattern) {
    size_t offset = 0U;
    while (offset < image.size()) {
        const size_t remaining = image.size() - offset;
        const size_t chunk = remaining < pattern ? remaining : pattern;
        if (!collector.Append(image.data() + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

bool TestCollectsExactBinaryIdentitiesAcrossFragments() {
    const auto image = MakeImage();
    constexpr std::array<size_t, 5U> PATTERNS = {1U, 7U, 20U, 181U, 512U};
    for (const size_t pattern : PATTERNS) {
        modules::OtaImageIdentityCollector collector;
        modules::OtaImageIdentity identity;
        if (!Expect(collector.Begin(IMAGE_BYTES, ESP32S3_CHIP_ID),
                    "valid declared image begins") ||
            !Expect(AppendPattern(collector, image, pattern),
                    "valid fragmented stream appends") ||
            !Expect(collector.Finish(identity),
                    "exact image framing yields identity") ||
            !Expect(identity.buildIdLength == 32U,
                    "ELF SHA build identity has exact length")) {
            return false;
        }
        for (size_t index = 0U; index < 32U; ++index) {
            if (!Expect(identity.buildId[index] ==
                            static_cast<uint8_t>(0x40U + index),
                        "descriptor ELF SHA is retained") ||
                !Expect(identity.imageSha256[index] ==
                            image[IMAGE_SHA_OFFSET + index],
                        "validated appended image SHA is retained")) {
                return false;
            }
        }
    }
    return true;
}

bool Rejects(std::array<uint8_t, IMAGE_BYTES> image,
             const char* const message) {
    modules::OtaImageIdentityCollector collector;
    modules::OtaImageIdentity identity;
    return Expect(collector.Begin(IMAGE_BYTES, ESP32S3_CHIP_ID),
                  "malformed fixture begins collection") &&
           Expect(!AppendPattern(collector, image, 13U) ||
                      !collector.Finish(identity),
                  message);
}

bool TestRejectsMalformedFramingAndIdentity() {
    auto image = MakeImage();
    image[0U] = 0U;
    if (!Rejects(image, "bad image magic is rejected")) {
        return false;
    }
    image = MakeImage();
    image[1U] = 17U;
    if (!Rejects(image, "excess segment count is rejected")) {
        return false;
    }
    image = MakeImage();
    image[SECOND_SEGMENT_HEADER_OFFSET + 4U] = 15U;
    if (!Rejects(image, "malformed second segment is rejected")) {
        return false;
    }
    image = MakeImage();
    image[12U] = 5U;
    if (!Rejects(image, "wrong chip id is rejected")) {
        return false;
    }
    image = MakeImage();
    image[23U] = 0U;
    if (!Rejects(image, "image without appended hash is rejected")) {
        return false;
    }
    image = MakeImage();
    image[28U] = 0xFCU;
    image[29U] = 0U;
    if (!Rejects(image, "undersized first descriptor segment is rejected")) {
        return false;
    }
    image = MakeImage();
    image[32U] = 0U;
    RefreshChecksum(image);
    SetDigestFromHex(
        image,
        "52c16339298e09e59ca758a76c0c2df07871f5918a7c637737c95f471d19393f");
    if (!Rejects(image, "bad app descriptor magic is rejected")) {
        return false;
    }
    image = MakeImage();
    for (size_t index = 0U; index < 32U; ++index) {
        image[APP_ELF_SHA_OFFSET + index] = 0U;
    }
    RefreshChecksum(image);
    SetDigestFromHex(
        image,
        "c336e5625f16d324e310523ca6b70da1d1b0fc37493f09add2076166208f5fc9");
    if (!Rejects(image, "zero ELF SHA build identity is rejected")) {
        return false;
    }
    image = MakeImage();
    image[344U] = 1U;
    SetDigestFromHex(
        image,
        "3036bc4ca6439154cca88eab9c53cfbb2d30dfb99c8535565de313360c21129d");
    if (!Rejects(image, "nonzero checksum padding is rejected")) {
        return false;
    }
    image = MakeImage();
    image[CHECKSUM_OFFSET] ^= 1U;
    SetDigestFromHex(
        image,
        "2fdc028d5c3ceb7203011a77170990b5e96ed8ab2f5ee32044b17e781112b8cf");
    if (!Rejects(image, "wrong ESP image checksum is rejected")) {
        return false;
    }
    image = MakeImage();
    image[IMAGE_SHA_OFFSET] ^= 1U;
    if (!Rejects(image, "non-matching appended image SHA is rejected")) {
        return false;
    }
    image = MakeImage();
    for (size_t index = IMAGE_SHA_OFFSET; index < image.size(); ++index) {
        image[index] = 0U;
    }
    return Rejects(image, "zero appended image SHA is rejected");
}

bool TestExactBoundaryAbortAndRetry() {
    const auto first = MakeImage(0x20U);
    const auto second = MakeImage(0x60U);
    modules::OtaImageIdentityCollector collector;
    modules::OtaImageIdentity identity;
    if (!Expect(collector.Begin(IMAGE_BYTES - 1U, ESP32S3_CHIP_ID),
                "truncated declaration begins") ||
        !Expect(collector.Append(first.data(), IMAGE_BYTES - 1U),
                "truncated bytes can be observed") ||
        !Expect(!collector.Finish(identity),
                "missing digest byte cannot finalize")) {
        return false;
    }
    collector.Abort();
    if (!Expect(!collector.Append(first.data(), 1U),
                "aborted collector accepts no stale bytes") ||
        !Expect(collector.Begin(IMAGE_BYTES, ESP32S3_CHIP_ID),
                "collector can retry after abort") ||
        !Expect(AppendPattern(collector, second, 23U),
                "retry receives independent stream") ||
        !Expect(collector.Finish(identity), "retry finalizes") ||
        !Expect(identity.buildId[0] == 0x60U,
                "retry does not retain prior identity")) {
        return false;
    }

    collector.Abort();
    const uint8_t junk = 0U;
    return Expect(collector.Begin(IMAGE_BYTES + 1U, ESP32S3_CHIP_ID),
                  "oversized declaration begins for exact-layout rejection") &&
           Expect(collector.Append(first.data(), first.size()),
                  "valid image prefix is observed") &&
           Expect(!collector.Append(&junk, 1U),
                  "trailing declared byte is rejected before activation");
}

const char* PolicyStatusName(const modules::OtaBootPolicyStatus status) {
    switch (status) {
        case modules::OtaBootPolicyStatus::Unavailable: return "unavailable";
        case modules::OtaBootPolicyStatus::Absent: return "absent";
        case modules::OtaBootPolicyStatus::Known: return "known";
        case modules::OtaBootPolicyStatus::Unsupported: return "unsupported";
        case modules::OtaBootPolicyStatus::Malformed: return "malformed";
    }
    return "invalid";
}

bool ExpectCorpusIdentity(const bool valid,
                         const modules::OtaImageIdentity& identity,
                         const JsonObjectConst testCase) {
    const JsonObjectConst expected = testCase["expected"];
    const auto& policy = identity.bootPolicy;
    const bool matches =
        valid == testCase["valid_integrity"].as<bool>() &&
        std::string_view(PolicyStatusName(policy.status)) ==
            expected["status"].as<const char*>() &&
        policy.schema == expected["schema"].as<uint16_t>() &&
        policy.size == expected["size"].as<uint16_t>() &&
        policy.epoch == expected["epoch"].as<uint32_t>() &&
        (valid ? identity.buildIdLength == 32U
               : identity.buildIdLength == 0U &&
                     identity.imageSha256 == std::array<uint8_t, 32U>{});
    if (!matches) {
        std::printf("CORPUS: %s integrity=%u policy=%s/%u/%u/%u\n",
                    testCase["name"].as<const char*>(), valid ? 1U : 0U,
                    PolicyStatusName(policy.status),
                    static_cast<unsigned>(policy.schema),
                    static_cast<unsigned>(policy.size),
                    static_cast<unsigned>(policy.epoch));
    }
    return Expect(matches, "shared complete-image boot-policy vector agrees");
}

bool TestSharedBootPolicyCorpus() {
    std::ifstream input("test/fixtures/firmware_boot_policy.json");
    DynamicJsonDocument corpus(32768U);
    if (!Expect(input.good() && !deserializeJson(corpus, input),
                "shared full-image corpus loads") ||
        !Expect(corpus["cases"].size() == 15U &&
                    corpus["format"]["current_producer_epoch"].as<uint32_t>() ==
                        modules::OTA_BOOT_POLICY_CURRENT_EPOCH,
                "shared vectors identify the current SDK-only producer")) {
        return false;
    }
    for (const JsonObjectConst testCase : corpus["cases"].as<JsonArrayConst>()) {
        const std::string_view hex = testCase["image_hex"].as<const char*>();
        std::vector<uint8_t> image(hex.size() / 2U);
        for (size_t index = 0U; index < image.size(); ++index) {
            image[index] = static_cast<uint8_t>(
                (HexNibble(hex[index * 2U]) << 4U) |
                HexNibble(hex[index * 2U + 1U]));
        }
        for (const size_t pattern : {1U, 7U, 23U, 255U, 288U, 289U, 303U, 512U}) {
            modules::OtaImageIdentityCollector collector;
            modules::OtaImageIdentity identity;
            const bool valid = collector.Begin(image.size(), ESP32S3_CHIP_ID) &&
                AppendPattern(collector, image, pattern) &&
                collector.Finish(identity);
            if (!ExpectCorpusIdentity(valid, identity, testCase)) return false;
        }
        // Split every byte around the SDK/custom descriptor boundary and its
        // fields, so an Append boundary cannot complete a truncated segment.
        for (size_t split = 287U; split <= 305U; ++split) {
            modules::OtaImageIdentityCollector collector;
            modules::OtaImageIdentity identity;
            if (!Expect(collector.Begin(image.size(), ESP32S3_CHIP_ID) &&
                            collector.Append(image.data(), split) &&
                            !collector.Finish(identity) &&
                            identity.bootPolicy.status ==
                                modules::OtaBootPolicyStatus::Unavailable,
                        "descriptor prefix publishes no unverified evidence")) {
                return false;
            }
            const bool valid =
                collector.Append(image.data() + split, image.size() - split) &&
                collector.Finish(identity);
            if (!ExpectCorpusIdentity(valid, identity, testCase)) return false;
        }
        PartitionFixture fixture{image};
        const modules::OtaImagePartitionReader reader{
            static_cast<uint32_t>(image.size() + 4096U), &fixture,
            PartitionFixture::Read};
        modules::OtaImageIdentityCollector collector;
        modules::OtaImageIdentity identity;
        if (!ExpectCorpusIdentity(
                collector.ReadPartition(reader, ESP32S3_CHIP_ID, identity),
                identity, testCase) ||
            !Expect(fixture.lastEnd == image.size() && fixture.maxRead <= 512U,
                    "corpus partition reads stop at verified image boundary")) {
            return false;
        }
        if (identity.bootPolicy.status == modules::OtaBootPolicyStatus::Known) {
            const auto encoded = modules::EncodeOtaBootPolicyDescriptor(
                identity.bootPolicy.epoch);
            if (!Expect(std::memcmp(image.data() + 288U, encoded.data(),
                                    encoded.size()) == 0,
                        "producer LE encoding agrees with shared artifact")) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    if (!TestPartitionReadsExactImageAndFailsClosed() ||
        !TestCollectsExactBinaryIdentitiesAcrossFragments() ||
        !TestRejectsMalformedFramingAndIdentity() ||
        !TestExactBoundaryAbortAndRetry() ||
        !TestSharedBootPolicyCorpus()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
