#include "modules/ble_service.hpp"

#if defined(ARDUINO) && defined(LITCH_BLE)
#include <Arduino.h>
#endif
#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_ota_reboot_grace.hpp"
#include "modules/ble_service_internal.hpp"
#include "runtime/boot_guard.hpp"

namespace modules {
void BleService::Update(const SettingsStore& settings) {
#if defined(LITCH_BLE)
#if defined(LITCH_BLE_OTA)
    // Finalized firmware owns this grace period. A queued name/enable change
    // must not tear down the stack and revoke the already announced reboot.
    if (m_otaPendingReboot) {
        UpdateConnectionSessionNotifications();
        if (!m_otaRebootGraceArmed &&
            (!IsConnected() || !m_connectionSession.IsActivated())) {
            // Finalization remains authoritative if the peer retires before
            // its receipt can be submitted; keep the existing grace duration.
            m_otaRebootArmedAtMs = fxe::Millis();
            m_otaRebootGraceArmed = true;
        }
        if (m_otaRebootGraceArmed &&
            IsBleOtaRebootGraceElapsed(fxe::Millis(), m_otaRebootArmedAtMs)) {
            const uint32_t nowMs = fxe::Millis();
            const uint16_t nativeHandle = m_ownerNativeHandle.load(std::memory_order_acquire);
            if (m_server != nullptr && nativeHandle != ble_internal::INVALID_CONN_HANDLE) {
                if (!m_otaRebootDisconnectIssued) {
                    // Let GAP deliver termination instead of making the phone
                    // wait for supervision timeout after a silent controller reset.
                    m_otaRebootDisconnectIssued = true;
                    m_otaRebootDisconnectAtMs = nowMs;
                    const bool accepted = m_server->disconnect(nativeHandle);
                    logger::Trace("ble: ota disconnect request accepted=%u", accepted ? 1U : 0U);
                }
                // The native callback clears this handle. SDK rejection or a
                // missing callback must never strand already finalized firmware.
                if (m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE &&
                    static_cast<uint32_t>(nowMs - m_otaRebootDisconnectAtMs) <
                        BLE_OTA_REBOOT_DISCONNECT_GRACE_MS) return;
            }
            logger::Warn("ble: ota complete, rebooting");
            if (m_bootGuard != nullptr) {
                m_bootGuard->PrepareForIntentionalReboot();
            }
            ESP.restart();
        }
        return;
    }
#endif
    // Native clear owns controller work until its terminal outcome is released.
    // Defer disable/rename teardown; the ordinary next Update applies it after
    // polling has completed, without stopping GAP or replacing the live stack.
    if (m_ownerClearToken != 0U) {
        UpdateOwnerPairing();
        return;
    }
    if (!m_recoveryOnly) m_settings = const_cast<SettingsStore*>(&settings);
    const bool enabled = m_recoveryOnly || (m_diagnosticRadioOverride
        ? m_diagnosticRadioEnabled : ReadEnabledSetting(settings));
    const fxe::FixedString<32> desiredName = m_recoveryOnly
        ? fxe::FixedString<32>(RECOVERY_DEVICE_NAME) : ReadDeviceNameSetting(settings);

    if (!enabled) {
        if (OwnerPairingEnabled()) {
            if (!m_initialized) InitStack(&settings);
            m_enabled = false;
            StopAdvertising();
            if (m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE &&
                m_server != nullptr) m_server->disconnect(m_ownerNativeHandle.load(std::memory_order_acquire));
            UpdateOwnerPairing();
            return;
        }
        if (m_initialized) {
            ShutdownStack();
        }
        m_enabled = false;
        return;
    }

    if (m_initialized && desiredName != m_deviceName) {
        // Native admission precedes main-loop activation. A rename must retain
        // that real link too, until its native disconnect clears the handle.
        if (!IsConnected() && m_ownerNativeHandle.load(std::memory_order_acquire) ==
                                  ble_internal::INVALID_CONN_HANDLE) {
            logger::Info("ble: device name changed; applying before advertising");
            ShutdownStack();
        }
    }

    if (!m_initialized) {
        InitStack(m_recoveryOnly ? nullptr : &settings);
    }
    if (!m_initialized) {
        return;
    }

    UpdateOwnerPairing();
    PollConnectionRenewal();
    UpdateFastConnectionPreference();
#if defined(LITCH_HW_VARIANT_S3)
    const uint32_t phyEpoch = m_bleSettings.CurrentConnectionEpoch();
    const uint16_t phyHandle = m_connectionHandle;
    const auto currentPhyPeer = [&]() {
        return m_server != nullptr && IsConnected() && OwnerProductAccess() &&
            phyHandle != ble_internal::INVALID_CONN_HANDLE &&
            m_bleSettings.OwnsConnectionEpoch(phyEpoch) &&
            m_ownerNativeEpoch.load(std::memory_order_acquire) == phyEpoch &&
            m_ownerNativeHandle.load(std::memory_order_acquire) == phyHandle &&
            !m_ownerQuiescing.load(std::memory_order_acquire);
    };
    if (m_preferredPhyEpoch != phyEpoch && currentPhyPeer()) {
        m_preferredPhyEpoch = phyEpoch; // Once per physical epoch, including SDK errors.
        m_preferredPhyPending = false;
        if (!m_linkObservation.phyKnown) {
            uint8_t txPhy = 0U, rxPhy = 0U;
            const bool observed = m_server->getPhy(phyHandle, &txPhy, &rxPhy);
            if (observed && currentPhyPeer() && txPhy >= 1U && txPhy <= 3U &&
                rxPhy >= 1U && rxPhy <= 3U) {
                m_linkObservation.txPhy = txPhy;
                m_linkObservation.rxPhy = rxPhy;
                m_linkObservation.phyKnown = true;
            }
        }
        if (currentPhyPeer() && !(m_linkObservation.phyKnown &&
                m_linkObservation.txPhy == 2U && m_linkObservation.rxPhy == 2U)) {
            // Main-loop SDK calls can wait for HCI acceptance; never callback/flash work.
            // A rejected preference leaves the current (including 1M) link usable.
            m_preferredPhyPending = true;
            const bool accepted = m_server->updatePhy(phyHandle, BLE_GAP_LE_PHY_2M_MASK,
                                                       BLE_GAP_LE_PHY_2M_MASK, 0U);
            if (!accepted || !currentPhyPeer()) m_preferredPhyPending = false;
            logger::Trace("ble: preferred_phy=2m request_accepted=%u selected_phy=unproven callback_status=unavailable",
                          accepted ? 1U : 0U);
        }
    }
#endif
    m_enabled = true;
    if (!m_recoveryOnly) FlushConsoleOutputBuffer();
    StartAdvertising();
    if (!m_recoveryOnly && OwnerProductAccess() &&
        m_bleSettings.UpdateChangeNotifications(settings, IsConnected())) {
        m_consoleStreamSnapshotRequested.store(true, std::memory_order_release);
    }
    UpdateFirmwareDigestNotification();
    UpdateConnectionSessionNotifications();
    if (!m_recoveryOnly) UpdateLitchcraftNotifications();

    if (IsConnected() && !IsOtaInProgress() && !m_bleLinkTestInProgress && m_server != nullptr &&
        m_connectionHandle != ble_internal::INVALID_CONN_HANDLE) {
        const uint32_t nowMs = fxe::Millis();
        const uint32_t connectedElapsedMs =
            (m_connectedAtMs == 0U) ? 0U : static_cast<uint32_t>(nowMs - m_connectedAtMs);
        const uint32_t idleElapsedMs =
            (m_lastClientActivityMs == 0U) ? connectedElapsedMs
                                           : static_cast<uint32_t>(nowMs - m_lastClientActivityMs);
        if (!m_staleDisconnectIssued &&
            connectedElapsedMs >= ble_internal::STALE_SESSION_DISCONNECT_GRACE_MS &&
            idleElapsedMs >= ble_internal::STALE_SESSION_IDLE_GRACE_MS) {
            m_staleDisconnectIssued = true;
            logger::Warn("ble: stale session disconnect handle=%u connected_ms=%lu idle_ms=%lu",
                         static_cast<unsigned>(m_connectionHandle),
                         static_cast<unsigned long>(connectedElapsedMs),
                         static_cast<unsigned long>(idleElapsedMs));
            (void)m_server->disconnect(m_connectionHandle);
        }
    }

#if defined(LITCH_BLE_OTA)
    UpdateOtaInactivityTimeout();

    if (m_otaInProgress && IsConnected()) {
        const uint32_t nowMs = fxe::Millis();
        if (m_lastOtaStatusMs == 0 ||
            (nowMs - m_lastOtaStatusMs) >= ble_internal::OTA_STATUS_HEARTBEAT_INTERVAL_MS) {
            m_lastOtaStatusMs = nowMs;
            NotifyOtaStatus("progress");
        }
    }

#endif
#else
    (void)settings;
#endif
}

} // namespace modules
