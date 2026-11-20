#include "modules/factory_provisioning_policy.hpp"

#include <cstddef>

namespace modules {
namespace {

constexpr uint8_t MAGIC_0 = 0x4CU;
constexpr uint8_t MAGIC_1 = 0x46U;

uint32_t Crc32(const uint8_t* const bytes, const size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

} // namespace

bool EncodeFactoryProvisioningRecord(
    const FactoryProvisioningState state,
    FactoryProvisioningRecord& out) {
    if (state != FactoryProvisioningState::Pending &&
        state != FactoryProvisioningState::Passed &&
        state != FactoryProvisioningState::Override && state != FactoryProvisioningState::Required) {
        out.fill(0U);
        return false;
    }
    out = {
        MAGIC_0,
        MAGIC_1,
        FACTORY_PROVISIONING_RECORD_VERSION,
        static_cast<uint8_t>(state),
        0U,
        0U,
        0U,
        0U,
    };
    const uint32_t crc = Crc32(out.data(), 4U);
    out[4U] = static_cast<uint8_t>(crc);
    out[5U] = static_cast<uint8_t>(crc >> 8U);
    out[6U] = static_cast<uint8_t>(crc >> 16U);
    out[7U] = static_cast<uint8_t>(crc >> 24U);
    return true;
}

FactoryProvisioningState DecodeFactoryProvisioningRecord(
    const FactoryProvisioningRecord& record) {
    const uint32_t expected = Crc32(record.data(), 4U);
    const uint32_t actual = static_cast<uint32_t>(record[4U]) |
        (static_cast<uint32_t>(record[5U]) << 8U) |
        (static_cast<uint32_t>(record[6U]) << 16U) |
        (static_cast<uint32_t>(record[7U]) << 24U);
    if (record[0U] != MAGIC_0 || record[1U] != MAGIC_1 ||
        record[2U] != FACTORY_PROVISIONING_RECORD_VERSION ||
        expected != actual) {
        return FactoryProvisioningState::Invalid;
    }
    const auto state = static_cast<FactoryProvisioningState>(record[3U]);
    return (state == FactoryProvisioningState::Pending ||
            state == FactoryProvisioningState::Passed || state == FactoryProvisioningState::Override ||
            state == FactoryProvisioningState::Required)
        ? state
        : FactoryProvisioningState::Invalid;
}

void FactoryProvisioningPolicy::Begin(
    const hardware::BoardRevision revision,
    const bool hasInstalledIdentity,
    const bool hasCompletedHardwareTest,
    const ReadState readState,
    const WriteState writeState,
    void* const context) {
    m_revision = revision;
    m_writeState = writeState;
    m_context = context;
    m_autoEnter = false;

    if (!hardware::SupportsProductionHardwareTest(revision)) {
        m_state = FactoryProvisioningState::LegacyExempt;
        return;
    }
    const FactoryProvisioningState initialState = readState != nullptr
        ? readState(context)
        : FactoryProvisioningState::Unavailable;
    m_state.store(initialState, std::memory_order_release);
    if (initialState == FactoryProvisioningState::Passed || initialState == FactoryProvisioningState::Override) return;
    if (initialState == FactoryProvisioningState::Required) { m_autoEnter = true; return; }
    if (initialState == FactoryProvisioningState::Invalid || initialState == FactoryProvisioningState::Unavailable) {
        m_autoEnter = true;
        return; // A failed read cannot be repaired by potentially stale settings.
    }
    if (hasCompletedHardwareTest) {
        m_autoEnter = !Write(FactoryProvisioningState::Passed);
        return;
    }
    if (initialState == FactoryProvisioningState::Absent &&
        hasInstalledIdentity) {
        // Existing V3 boards predate this NVS discriminator. Promote them once
        // rather than treating an ordinary firmware upgrade as factory-new.
        m_autoEnter = !Write(FactoryProvisioningState::Passed);
        return;
    }
    if (initialState == FactoryProvisioningState::Absent) {
        (void)Write(FactoryProvisioningState::Pending);
    }
    m_autoEnter = true;
}

bool FactoryProvisioningPolicy::RequireFactoryTest() {
    m_autoEnter = true;
    return Write(FactoryProvisioningState::Required);
}

bool FactoryProvisioningPolicy::OnDurableHardwareTestRecord(
    const bool recordMatchesRevision, const bool overrideResult,
    const bool freshFactoryResult) {
    if (!hardware::SupportsProductionHardwareTest(m_revision)) return true;
    if (!recordMatchesRevision) return false;
    const auto state = State();
    if ((state == FactoryProvisioningState::Required || state == FactoryProvisioningState::Override || state == FactoryProvisioningState::Unavailable ||
         state == FactoryProvisioningState::Invalid || overrideResult) && !freshFactoryResult)
        return false; // An old settings snapshot cannot undo explicit re-entry.
    if (!overrideResult && state == FactoryProvisioningState::Passed) return true;
    if (!Write(overrideResult ? FactoryProvisioningState::Override : FactoryProvisioningState::Passed))
        return false;
    m_autoEnter = false;
    return true;
}

bool FactoryProvisioningPolicy::Write(
    const FactoryProvisioningState state) {
    if (m_writeState == nullptr || !m_writeState(m_context, state)) {
        m_state.store(
            FactoryProvisioningState::Unavailable,
            std::memory_order_release);
        return false;
    }
    m_state.store(state, std::memory_order_release);
    return true;
}

} // namespace modules
