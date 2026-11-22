#include <cstdint>
#include <cstdio>
#include <cstring>

#include "modules/firmware_digest_protocol.hpp"
#include "modules/firmware_digest_provider.hpp"

namespace {

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool TestDigestStateIsCachedAndThreadSafeAtBoundary() {
    modules::FirmwareDigestProvider provider;
    if (!Expect(provider.State() == modules::FirmwareDigestState::Unavailable,
                "digest begins unavailable") ||
        !Expect(provider.TryBegin(), "first request becomes pending") ||
        !Expect(provider.State() == modules::FirmwareDigestState::Pending,
                "request publishes pending")) {
        return false;
    }

    uint8_t digest[modules::FirmwareDigestProvider::DIGEST_BYTES] = {};
    for (uint8_t index = 0U; index < sizeof(digest); ++index) {
        digest[index] = index;
    }
    provider.Complete(digest);

    uint8_t copied[modules::FirmwareDigestProvider::DIGEST_BYTES] = {};
    return Expect(provider.State() == modules::FirmwareDigestState::Ready,
                  "completion publishes ready") &&
           Expect(provider.CopyDigest(copied, sizeof(copied)),
                  "ready digest copies") &&
           Expect(std::memcmp(digest, copied, sizeof(digest)) == 0,
                  "cached digest is exact") &&
           Expect(!provider.TryBegin(), "ready digest is not recomputed");
}

bool TestCompactProtocolIsBoundedAndDeterministic() {
    uint8_t digest[modules::FirmwareDigestProvider::DIGEST_BYTES] = {};
    for (uint8_t index = 0U; index < sizeof(digest); ++index) {
        digest[index] = index;
    }
    char payload[32] = {};
    if (!Expect(modules::FormatFirmwareDigestState(
                    modules::FirmwareDigestState::Pending,
                    payload,
                    sizeof(payload)),
                "pending formats") ||
        !Expect(std::strcmp(payload, "{\"h\":\"p\"}") == 0,
                "pending token is stable")) {
        return false;
    }
    if (!Expect(modules::FormatFirmwareDigestFragment(
                    digest, 0U, payload, sizeof(payload)),
                "first fragment formats") ||
        !Expect(std::strcmp(payload, "{\"h0\":\"00010203\"}") == 0,
                "first fragment is stable")) {
        return false;
    }
    return Expect(modules::FormatFirmwareDigestFragment(
                      digest, 7U, payload, sizeof(payload)),
                  "last fragment formats") &&
           Expect(std::strcmp(payload, "{\"h7\":\"1c1d1e1f\"}") == 0,
                  "last fragment is stable") &&
           Expect(modules::FormatFirmwareDigestState(
                      modules::FirmwareDigestState::Ready,
                      payload,
                      sizeof(payload)),
                  "ready formats") &&
           Expect(std::strcmp(payload, "{\"h\":\"r\"}") == 0,
                  "ready token is stable");
}

} // namespace

int main() {
    if (!TestDigestStateIsCachedAndThreadSafeAtBoundary()) {
        return 1;
    }
    if (!TestCompactProtocolIsBoundedAndDeterministic()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
