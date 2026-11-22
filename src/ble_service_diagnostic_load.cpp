#include "modules/ble_service.hpp"
#include "modules/physical_input_state_wire_codec.hpp"

namespace modules {
bool BleService::IsConnectionSessionActive() const {
#if defined(LITCH_BLE)
    return IsConnected() && m_connectionSession.IsActivated() &&
        m_connectionSessionSubscribed.load(std::memory_order_acquire) &&
        m_bleSettings.OwnsConnectionEpoch(m_connectionSession.Generation());
#else
    return false;
#endif
}

bool BleService::RefreshDiagnosticPhysicalInputState() {
#if defined(LITCH_BLE)
    if (!IsConnectionSessionActive() || !m_connectionPhysicalOccupancyValid ||
        IsOtaInProgress()) return false;
    uint32_t revision = m_connectionPhysicalInputRevision + 1U;
    if (revision == 0U) revision = 1U;
    const auto body = physical_input_state_wire::Encode(
        m_connectionPhysicalOccupancy, m_connectionButtonBits);
    if (!m_connectionSession.PublishState(physical_input_state_wire::STATE_CODE,
            revision, body.data(), body.size())) return false;
    m_connectionPhysicalInputRevision = revision;
    return true;
#else
    return false;
#endif
}
} // namespace modules
