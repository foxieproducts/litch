#include <cstdio>
#include "foxie_embedded/test_support/test_assert.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/duck_game_persistence.hpp"
#include <cstring>
#include <string>
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    fxe::test_support::TestResults results("SETTINGS_SNAPSHOT_TRANSACTION");
    results.Start();
    int failures = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) { ++failures; results.Check(false, what); } };
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig cfg; cfg.path = "build/settings_snapshot_transaction.json";
    cfg.deferredSaveDelayMs = 2000U; cfg.mustSaveDelayMs = 100U; cfg.allowSynchronousDeferredSaves = false;
    modules::SettingsStore settings(cfg);
    check(settings.Load(), "SettingsStore load");
    settings.Doc()["VS_CPU_LEVEL"] = 0U; settings.Doc()["TIMER"] = 0U;
    unsigned queued = 0U;
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument&) { ++queued; return true; });
    runtime::GameStateMachineConfig smCfg; smCfg.settleDurationMs = 1U; smCfg.placementStableRequiredMs = 0U;
    runtime::GameStateMachine sm(smCfg); sm.Reset();
    modules::LitchMaxEngine engine; engine.Reset();
    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    check(coord.LoadOrInitialize(), "real-engine coordinator initializes");
    coord.SetBoardSettingsActive(true); // Keep autonomous background search out of a stationary manual-clock diagnostic.
    unsigned guardedCallbacks = 0U;
    settings.SetBeforePersistenceCallback([&]() { ++guardedCallbacks; });
    constexpr uint64_t INITIAL = 0xFFFF00000000FFFFULL, DUCK = 1ULL << 24U;
    auto observe = [&](uint64_t mask) { sm.HandleBoardObservation(sm.GetPhysicalOccupancyMask() ^ mask, mask, fxe::Millis(), sm.GetLastBoardObservationSequence()+1U); coord.Update(); fxe::ManualMillis::AdvanceMs(200U); coord.Update(); };
    for (unsigned i = 0U; i < 12U; ++i) {
        check(coord.ResetEngine(), "repeated explicit New Game accepted");
        check(coord.GetGameVariant() == modules::GameVariant::Undecided, "New Game resets variant authority");
        observe(INITIAL | DUCK);
        check(coord.GetGameVariant() == modules::GameVariant::Duck, "repeated fresh setup activates Duck");
        observe(INITIAL);
        check(coord.GetGameVariant() == modules::GameVariant::Undecided && coord.GetDuckSquare() == modules::INVALID_DUCK_SQUARE, "removal clears Duck authority");
    }
    settings.Update(); unsigned settled = queued;
    for (unsigned i = 0U; i < 100U; ++i) { fxe::ManualMillis::AdvanceMs(100U); coord.Update(); settings.Update(); }
    check(queued == settled, "idle ten seconds causes no further saves");
    modules::ControlMutationService controls; controls.Configure(settings, {});
    StaticJsonDocument<16> value; value.set(5U);
    auto first = controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    value.set(6U); auto changed = controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    unsigned before = queued; fxe::ManualMillis::AdvanceMs(1999U); settings.Update();
    check(queued == before && changed.status == modules::ControlMutationStatus::Applied, "brightness waits through 1999 ms");
    fxe::ManualMillis::AdvanceMs(1U); settings.Update(); check(queued == before + 1U, "brightness queues exactly once at 2 seconds");
    unsigned revision = settings.GetRevision(); auto noop = controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    check(noop.status == modules::ControlMutationStatus::Noop && settings.GetRevision() == revision, "unchanged brightness does not dirty document");
    fxe::ManualMillis::AdvanceMs(3000U); settings.Update(); check(queued == before + 1U, "unchanged brightness causes no later save");
    check(guardedCallbacks == 0U, "ordinary game/settings saves do not revoke power authorization");
    settings.ArmPersistenceGuard();
    settings.SetAsyncSaveAdmissionCallback([](const JsonDocument&) { return fxe::SettingsStore::AsyncSaveAdmission::Deferred; });
    check(!settings.SaveNow() && guardedCallbacks == 0U && settings.IsPersistenceGuardArmed(), "guarded Deferred admission retains intent without blocking-storage guard");
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument&) { ++queued; return true; });
    check(settings.SaveNow() && guardedCallbacks == 0U && !settings.IsPersistenceGuardArmed(), "guarded accepted retry queues without restarting power confirmation");
    unsigned safeCallbacks = guardedCallbacks;
    settings.MarkDirty(); fxe::ManualMillis::AdvanceMs(2000U); settings.Update();
    check(guardedCallbacks == safeCallbacks, "unrelated save after accepted guarded save does not restart confirmation");
    check(coord.LoadExternalFen("4r1k1/8/8/8/8/8/8/4K3 w - - 0 1"), "Classic tactical position loads after twelve Duck cycles");
    check(coord.GetGameVariant() == modules::GameVariant::Classic && coord.GetEngineDerivedState().inCheck && coord.GetEngineDerivedState().threatenedKingSquareValid, "Classic check feedback restored after repeated Duck cycles");
    uint64_t legal = 0U; check(engine.GetLegalDestinationsMask(60U, legal) && (legal & (1ULL << 52U)) == 0ULL, "Classic engine excludes e1-e2 into attacked file after Duck cycles");
    unsigned delayedAttempts = 0U;
    settings.SetAsyncSaveAdmissionCallback([&](const JsonDocument&) { ++delayedAttempts; return fxe::SettingsStore::AsyncSaveAdmission::Deferred; });
    check(coord.ResetEngine(), "delayed-admission New Game applies in RAM");
    observe(INITIAL | DUCK); value.set(7U); (void)controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    check(!settings.SaveNow() && delayedAttempts == 1U && coord.GetGameVariant() == modules::GameVariant::Duck && settings.ReadValue("BRIGHTNESS", 0U) == 7U, "Deferred storage cannot delay live Duck/settings authority");
    DynamicJsonDocument olderQueued(16384U);
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument& document) { olderQueued.set(document); ++queued; return true; });
    check(settings.SaveNow(), "older Duck/brightness snapshot accepted without completion");
    observe(INITIAL); value.set(8U); (void)controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    auto latestRevision = settings.GetRevision();
    DynamicJsonDocument newestRam(16384U); newestRam.set(settings.Doc());
    auto savesBeforeOldCompletion = settings.GetSaveCount();
    settings.ReportAsyncSaveSuccess(measureJson(olderQueued), 1U);
    check(settings.GetSaveCount() == savesBeforeOldCompletion + 1U && settings.GetRevision() == latestRevision && settings.ReadValue("BRIGHTNESS", 0U) == 8U && coord.GetGameVariant() == modules::GameVariant::Undecided, "old async completion changes counters without rolling newer RAM settings/variant back");
    check(std::strcmp(settings.Doc()["GAME_VARIANT_V1"].as<const char*>(), newestRam["GAME_VARIANT_V1"].as<const char*>()) == 0, "old completion preserves latest coupled game document");
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument& document) { ++queued; return document["BRIGHTNESS"].as<unsigned>() == 8U && std::strcmp(document["GAME_VARIANT_V1"].as<const char*>(), newestRam["GAME_VARIANT_V1"].as<const char*>()) == 0; });
    auto beforeNewestQueued = queued; fxe::ManualMillis::AdvanceMs(100U); settings.Update();
    check(queued == beforeNewestQueued + 1U && settings.GetLastError() == fxe::SettingsStoreError::None, "newer pending game/settings snapshot still queues after old completion");
    std::printf("DELAYED_RESULT attempts=%u old_completion_save_count=%u latest_revision=%u brightness=%u variant=%u newest_admissions=%u\n", delayedAttempts, settings.GetSaveCount(), settings.GetRevision(), settings.ReadValue("BRIGHTNESS", 0U), static_cast<unsigned>(coord.GetGameVariant()), queued-beforeNewestQueued);
    unsigned runtimeDirectAccesses = 0U; size_t maximumJsonMemory = settings.Doc().memoryUsage();
    settings.SetBeforeDirectStorageAccessCallback([&]() { ++runtimeDirectAccesses; std::printf("[SETTINGS_SNAPSHOT_CHECKPOINT] DIRECT_STORAGE_ACCESS count=%u mem=%zu overflow=%u brightness=%u\n", runtimeDirectAccesses, settings.Doc().memoryUsage(), settings.Doc().overflowed(), settings.ReadValue("BRIGHTNESS", 0U)); return true; });
    for (unsigned burst = 0U; burst < 256U; ++burst) {
        check(coord.ResetEngine(), "natural burst New Game accepted");
        observe(INITIAL | DUCK); observe(INITIAL); settings.Update();
        if (settings.Doc().memoryUsage() > maximumJsonMemory) maximumJsonMemory = settings.Doc().memoryUsage();
    }
    check(runtimeDirectAccesses == 0U && !settings.Doc().overflowed(), "256 natural New Game/Duck add-remove bursts cause no runtime file reload/overflow");
    std::printf("NATURAL_RESULT bursts=256 capacity=%zu max_memory=%zu overflow=%u runtime_direct_accesses=%u\n", settings.Doc().capacity(), maximumJsonMemory, settings.Doc().overflowed(), runtimeDirectAccesses);
    value.set(7U); (void)controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    check(settings.SaveNowBlocking(), "controlled fixture saves old brightness7"); runtimeDirectAccesses = 0U;
    value.set(8U); (void)controls.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    auto injectRetainedAndDeadPool = [&]() {
        std::string retained(3600U, 'r'); settings.Doc()["AUDIT_CONTROLLED_RETAINED"] = retained;
        auto freePool = settings.Doc().capacity()-settings.Doc().memoryUsage();
        std::string pressure(freePool > 128U ? freePool-128U : 0U, 'x');
        settings.Doc()["AUDIT_CONTROLLED_DEAD_POOL"] = pressure;
        settings.Doc().remove("AUDIT_CONTROLLED_DEAD_POOL");
    };
    injectRetainedAndDeadPool();
    std::printf("FAULT_BEFORE mem=%zu capacity=%zu serialized=%zu overflow=%u brightness=%u variant=%u\n", settings.Doc().memoryUsage(), settings.Doc().capacity(), measureJson(settings.Doc()), settings.Doc().overflowed(), settings.ReadValue("BRIGHTNESS", 0U), static_cast<unsigned>(coord.GetGameVariant()));
    check(!settings.Doc().overflowed() && settings.ReadValue("BRIGHTNESS", 0U) == 8U, "controlled fault starts with valid full RAM document/current brightness8");
    observe(INITIAL | DUCK);
    std::printf("FAULT_AFTER mem=%zu capacity=%zu serialized=%zu overflow=%u brightness=%u live_variant=%u document_variant=%s runtime_direct_accesses=%u\n", settings.Doc().memoryUsage(), settings.Doc().capacity(), measureJson(settings.Doc()), settings.Doc().overflowed(), settings.ReadValue("BRIGHTNESS", 0U), static_cast<unsigned>(coord.GetGameVariant()), settings.Doc()["GAME_VARIANT_V1"].as<const char*>(), runtimeDirectAccesses);
    check(runtimeDirectAccesses == 0U && settings.ReadValue("BRIGHTNESS", 0U) == 8U && coord.GetGameVariant() == modules::GameVariant::Duck, "controlled capacity fault on real Duck latch retains unsaved brightness8 without disk reload");

    std::printf("RESULT failures=%d queued=%u\n", failures, queued); return results.Finish();
}
