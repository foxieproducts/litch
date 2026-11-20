#ifdef ARDUINO

#include "firmware/firmware_app_internal.hpp"

#include "logger.hpp"

#include <climits>
#if defined(ESP32)
#include <bootloader_random.h>
#include <esp_random.h>
#include <mbedtls/ctr_drbg.h>
#endif

namespace {
#if defined(ESP32)
mbedtls_ctr_drbg_context g_identityRandom;
bool g_entropyWindow = false;
bool g_randomReady = false;
bool g_initialized = false;

int BootEntropy(void*, unsigned char* output, const size_t length) {
    if (!g_entropyWindow) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    esp_fill_random(output, length);
    return 0;
}
#endif

bool FillIdentityRandom(void*, uint8_t* output, const size_t length) {
#if defined(ESP32)
    return g_randomReady &&
        mbedtls_ctr_drbg_random(&g_identityRandom, output, length) == 0;
#else
    (void)output;
    (void)length;
    return false;
#endif
}

bool GenerateGameIdentity(void* context, modules::game_history::GameId& out) {
    return static_cast<modules::BoardIdentityPersistence*>(context)
        ->GenerateGameIdentity(out);
}
} // namespace

namespace firmware_internal {
namespace {

void OnDurableIdentity(void* context, const JsonDocument& document) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    sys.boardIdentity.OnDurableSettingsSnapshot(document);
    sys.factoryProvisioning.OnDurableSettingsSnapshot(
        document, sys.settingsPersistence.CurrentDurableSnapshotReceipt());
    sys.durableSettingsSnapshotObserved.store(true, std::memory_order_release);
}

} // namespace

void InitializeBoardIdentityEntropy() {
#if defined(ESP32)
    if (g_initialized) return;
    g_initialized = true;
    mbedtls_ctr_drbg_init(&g_identityRandom);
    // Bundled ESP-IDF requires SAR entropy before RF initialization, and requires
    // it disabled before ADC/RF/I2S start. This call runs at the start of SetupApp.
    bootloader_random_enable();
    g_entropyWindow = true;
    constexpr unsigned char personalization[] = "litch-identity-v1";
    const int result = mbedtls_ctr_drbg_seed(
        &g_identityRandom, &BootEntropy, nullptr,
        personalization, sizeof(personalization) - 1U);
    g_entropyWindow = false;
    bootloader_random_disable();
    if (result == 0) {
        // Never re-enable the ADC entropy source during gameplay. If this
        // bounded DRBG interval is exhausted, its disabled entropy callback
        // fails closed; reboot provides a fresh seed.
        mbedtls_ctr_drbg_set_reseed_interval(&g_identityRandom, INT_MAX - 1);
        g_randomReady = true;
    }
#endif
}

void ConfigureBoardIdentityPersistence(FirmwareSystem& sys) {
    if (sys.settings.IsDisabled()) {
        return;
    }
    modules::BoardIdentity installedIdentity;
    const bool hasInstalledIdentity =
        modules::ReadBoardIdentity(sys.settings.Doc(), installedIdentity) ==
        modules::BoardIdentityRead::Current;
    const bool mayPrepareIdentity = hasInstalledIdentity ||
        sys.factoryProvisioning.AllowsNewBoardIdentity();
    const bool identityPrepared = mayPrepareIdentity &&
        sys.boardIdentity.Prepare(
            sys.settings.Doc(), &FillIdentityRandom, nullptr);
    if (!sys.settingsPersistence.SetDurableSnapshotCallback(
            &OnDurableIdentity, &sys)) {
        return;
    }
    if (identityPrepared && !sys.factoryBootOnly) {
        sys.gamePersistence.SetGameHistoryIdentityGenerator(
            &GenerateGameIdentity, &sys.boardIdentity);
    } else if (!mayPrepareIdentity) {
        logger::Error(
            "factory: provisioning marker is not durable; new board identity is withheld");
    }
}

} // namespace firmware_internal
#endif
