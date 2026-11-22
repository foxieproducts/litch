#include "modules/firmware_digest_provider.hpp"

#include <cstring>

#include "foxie_embedded/millis.hpp"

namespace modules {

FirmwareDigestState FirmwareDigestProvider::State() const {
    return m_state.load(std::memory_order_acquire);
}

bool FirmwareDigestProvider::TryBegin() {
    FirmwareDigestState expected = FirmwareDigestState::Unavailable;
    if (m_state.compare_exchange_strong(expected,
                                        FirmwareDigestState::Pending,
                                        std::memory_order_acq_rel)) {
        return true;
    }
    expected = FirmwareDigestState::Error;
    return m_state.compare_exchange_strong(expected,
                                           FirmwareDigestState::Pending,
                                           std::memory_order_acq_rel);
}

bool FirmwareDigestProvider::CopyPendingDigest(const uint8_t* const digest) {
    if (digest == nullptr || State() != FirmwareDigestState::Pending) {
        Fail();
        return false;
    }
    std::memcpy(m_digest, digest, DIGEST_BYTES);
    return true;
}

void FirmwareDigestProvider::Complete(const uint8_t* const digest) {
    if (CopyPendingDigest(digest)) m_state.store(FirmwareDigestState::Ready, std::memory_order_release);
}

void FirmwareDigestProvider::CompleteStandalone(const uint8_t* const digest, const uint32_t timeoutMs) {
    if (!CopyPendingDigest(digest)) return;
    const bool expired = static_cast<uint32_t>(fxe::Millis() - m_standaloneStartedMs) >= timeoutMs;
    m_state.store(expired ? FirmwareDigestState::Error : FirmwareDigestState::Ready, std::memory_order_release);
}

void FirmwareDigestProvider::Fail() {
    m_state.store(FirmwareDigestState::Error, std::memory_order_release);
}

bool FirmwareDigestProvider::CopyDigest(uint8_t* const outDigest,
                                        const size_t outLen) const {
    if (outDigest == nullptr || outLen < DIGEST_BYTES ||
        State() != FirmwareDigestState::Ready) {
        return false;
    }
    std::memcpy(outDigest, m_digest, DIGEST_BYTES);
    return true;
}

} // namespace modules
