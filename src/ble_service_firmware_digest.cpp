#include "modules/ble_service.hpp"

#include "modules/firmware_digest_protocol.hpp"

namespace modules {

void BleService::ReserveBootFirmwareDigest() {
#if defined(LITCH_BLE)
    if (!m_bootDigestReserved && m_firmwareDigestProvider.TryBegin())
        m_bootDigestReserved = true;
#endif
}

void BleService::ReleaseBootFirmwareDigest() {
#if defined(LITCH_BLE)
    if (m_bootDigestReserved) {
        m_firmwareDigestProvider.Fail();
        m_bootDigestReserved = false;
    }
#endif
}

void BleService::PublishVerifiedRunningFirmwareDigest(const uint8_t* const digest) {
#if defined(LITCH_BLE)
    // Only the normal-health owner's complete running-image proof is supplied.
    // A standalone digest job retains its own publication if already pending.
    if (digest != nullptr && (m_bootDigestReserved || m_firmwareDigestProvider.TryBegin())) {
        m_firmwareDigestProvider.Complete(digest);
        m_bootDigestReserved = false;
    }
#else
    (void)digest;
#endif
}

FirmwareDigestState BleService::CachedRunningFirmwareDigest(
    uint8_t* const digest, const size_t capacity) const {
#if defined(LITCH_BLE)
    const auto state = m_firmwareDigestProvider.State();
    return state == FirmwareDigestState::Ready && !m_firmwareDigestProvider.CopyDigest(digest, capacity)
        ? FirmwareDigestState::Error : state;
#else
    (void)digest;
    (void)capacity;
    return FirmwareDigestState::Unavailable;
#endif
}

FirmwareDigestState BleService::ReadRunningFirmwareDigest(
    uint8_t* const outDigest, const size_t outCapacity) {
#if defined(LITCH_BLE)
    if (!StartEsp32FirmwareDigest(m_firmwareDigestProvider)) {
        return FirmwareDigestState::Error;
    }
    const FirmwareDigestState state = m_firmwareDigestProvider.State();
    if (state == FirmwareDigestState::Ready &&
        !m_firmwareDigestProvider.CopyDigest(outDigest, outCapacity)) {
        return FirmwareDigestState::Error;
    }
    return state;
#else
    (void)outDigest;
    (void)outCapacity;
    return FirmwareDigestState::Unavailable;
#endif
}

#if defined(LITCH_BLE)
void BleService::RequestFirmwareDigestNotification() {
    m_digestNotifyRequested = true;
    m_digestNotifyFragment = 0U;
    m_lastDigestNotifyState = FirmwareDigestState::Unavailable;
    if (!StartEsp32FirmwareDigest(m_firmwareDigestProvider)) {
        m_firmwareDigestProvider.Fail();
    }
    UpdateFirmwareDigestNotification();
}

void BleService::UpdateFirmwareDigestNotification() {
    if (!m_digestNotifyRequested || !IsConnected() || m_otaInProgress) {
        return;
    }

    const FirmwareDigestState state = m_firmwareDigestProvider.State();
    char payload[32] = {};
    if (state == FirmwareDigestState::Pending) {
        if (m_lastDigestNotifyState == FirmwareDigestState::Pending) {
            return;
        }
        if (FormatFirmwareDigestState(state, payload, sizeof(payload)) &&
            NotifyOtaJson(payload)) {
            m_lastDigestNotifyState = state;
        }
        return;
    }

    if (state == FirmwareDigestState::Error ||
        state == FirmwareDigestState::Unavailable) {
        if (FormatFirmwareDigestState(state, payload, sizeof(payload)) &&
            NotifyOtaJson(payload)) {
            m_lastDigestNotifyState = state;
            m_digestNotifyRequested = false;
        }
        return;
    }

    uint8_t digest[FirmwareDigestProvider::DIGEST_BYTES] = {};
    if (!m_firmwareDigestProvider.CopyDigest(digest, sizeof(digest))) {
        return;
    }
    if (m_digestNotifyFragment < 8U) {
        if (FormatFirmwareDigestFragment(digest,
                                         m_digestNotifyFragment,
                                         payload,
                                         sizeof(payload)) &&
            NotifyOtaJson(payload)) {
            ++m_digestNotifyFragment;
        }
        m_lastDigestNotifyState = FirmwareDigestState::Ready;
        return;
    }

    if (FormatFirmwareDigestState(state, payload, sizeof(payload)) &&
        NotifyOtaJson(payload)) {
        m_digestNotifyRequested = false;
    }
}
#endif

} // namespace modules
