#include <cstdio>
#include <string>
#include "modules/support_runtime_stats.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    if (!ok) { std::printf("[FAIL] %s\n", text); ++failures; }
}
std::string Json(const JsonDocument& doc) {
    std::string text;
    serializeJson(doc, text);
    return text;
}
}
int main() {
    using modules::SupportRuntimeStats;
    fxe::SettingsStore settings;
    settings.Doc()["BRIGHTNESS"] = 4U;
    const auto before = Json(settings.Doc());
    const auto revision = settings.GetRevision();
    SupportRuntimeStats stats;
    stats.BeginBoot(settings.Doc());
    stats.BeginBoot(settings.Doc());
    Check(Json(settings.Doc()) == before && settings.GetRevision() == revision &&
          settings.GetSaveCount() == 0U, "SupportRuntimeStatsBootAndSaveJourney: boot is RAM-only, once");
    Check(stats.Snapshot(7U).boots == 1U, "first boot count scoped to metrics introduction");
    DynamicJsonDocument snapshot(2048U);
    snapshot.set(settings.Doc());
    const uint64_t longUptime = (uint64_t{1U} << 32U) + 12345U;
    Check(stats.PrepareSave(snapshot, longUptime), "stamp full-width save checkpoint");
    Check(stats.Snapshot(longUptime).savedCheckpointMs == 0U, "enqueue is not verified save completion");
    stats.NoteSaved(snapshot);
    Check(stats.Snapshot(longUptime).savedCheckpointMs == longUptime, "publish verified checkpoint beyond uint32");
    DynamicJsonDocument older(2048U);
    older.set(snapshot);
    Check(stats.PrepareSave(older, 55U), "old request stamps bounded snapshot");
    stats.NoteSaved(older);
    Check(stats.Snapshot(longUptime).savedCheckpointMs == longUptime, "late older completion cannot regress report checkpoint");
    SupportRuntimeStats reboot;
    reboot.BeginBoot(snapshot);
    Check(reboot.Snapshot(10U).priorRuntimeMs == longUptime &&
          reboot.Snapshot(10U).savedCheckpointMs == 0U &&
          reboot.Snapshot(10U).boots == 2U, "reboot folds last saved checkpoint once and clears this-boot checkpoint in RAM");
    // A retry/old serialized document must be replaced with the current boot's
    // immutable base, never add its old checkpoint on each save.
    for (uint64_t uptime : {100U, 200U, 200U}) {
        Check(reboot.PrepareSave(snapshot, uptime), "repeated save preparation succeeds");
        Check(snapshot[SupportRuntimeStats::KEY]["prior_runtime_ms"] == longUptime,
              "repeated saves/retries do not accumulate current uptime twice");
    }
    SupportRuntimeStats again;
    again.BeginBoot(snapshot);
    Check(again.Snapshot(0U).priorRuntimeMs == longUptime + 200U, "next boot includes only last saved uptime, unsaved tail intentionally lost");
    modules::SupportRuntimeStats::ArmSleepConfirmation(modules::SupportSleepKind::Automatic);
    modules::SupportRuntimeStats::CancelSleepConfirmation();
    SupportRuntimeStats cancelled;
    cancelled.BeginBoot(snapshot);
    Check(cancelled.Snapshot(0U).automaticSleeps == 0U, "SupportSleepMetricsJourney: canceled/refused entry does not count");
    SupportRuntimeStats::ArmSleepConfirmation(modules::SupportSleepKind::Manual);
    SupportRuntimeStats::ConfirmLightSleep();
    SupportRuntimeStats woke;
    woke.BeginBoot(snapshot);
    Check(woke.Snapshot(0U).manualSleeps == 1U && woke.Snapshot(0U).automaticSleeps == 0U,
          "confirmed manual sleep counts once after actual sleep, separately from automatic");
    SupportRuntimeStats consumed;
    consumed.BeginBoot(snapshot);
    Check(consumed.Snapshot(0U).manualSleeps == 0U, "RTC confirmation consumed once; no boot save needed");
    DynamicJsonDocument malformed(1024U);
    malformed[SupportRuntimeStats::KEY]["v"] = 99U;
    malformed[SupportRuntimeStats::KEY]["boots"] = UINT64_MAX;
    SupportRuntimeStats unknown;
    unknown.BeginBoot(malformed);
    Check(!unknown.Snapshot(0U).available, "unknown schema is unavailable history, not invented lifetime total");
    const auto unknownBefore = Json(malformed);
    Check(unknown.PrepareSave(malformed, 123U) && Json(malformed) == unknownBefore,
          "future metric schema is preserved and cannot block ordinary settings durability");
    std::printf("[SUPPORT_RUNTIME_STATS] failures=%d\n", failures);
    return failures ? 1 : 0;
}
