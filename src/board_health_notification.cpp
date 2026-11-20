#include "modules/board_health_notification.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/console.hpp"
#include "runtime/panic_diagnostic.hpp"
namespace modules {
namespace {
unsigned RecentStage(const runtime::ProgressRecord& record, const unsigned index) {
    if (index >= record.recentStageCount) return unsigned(runtime::ProgressStage::Idle);
    const unsigned start = (unsigned(record.nextStage) + 8U - record.recentStageCount) % 8U;
    return record.recentStages[(start + index) % 8U];
}
}

BoardHealthLine FormatBoardHealth(const runtime::RuntimeHealthSnapshot& health,
    const PersistenceDegradation persistence,
    const runtime::ProgressSnapshot& progress, const bool sessionOnly) {
    BoardHealthLine line;
    (void)line.Format(
        "{\"event\":\"board_health\",\"v\":1,\"h\":\"%04X%u%04X\","
        "\"ps\":\"%c\",\"rr\":%lu,\"boot\":%lu,\"wd\":%lu,\"sd\":%u}",
        unsigned(health.revision), unsigned(health.state), unsigned(health.causes),
        persistence == PersistenceDegradation::None ? 'n' :
        persistence == PersistenceDegradation::QuarantinedRecord ? 'q' : 'i',
        static_cast<unsigned long>(progress.resetReason),
        static_cast<unsigned long>(progress.bootCount),
        static_cast<unsigned long>(progress.watchdogRestarts), unsigned(sessionOnly));
    return line;
}

BoardHealthConnectionState EncodeBoardHealthConnectionState(
    const runtime::RuntimeHealthSnapshot& health,
    const PersistenceDegradation persistence,
    const runtime::ProgressSnapshot& progress,
    const bool sessionOnly) {
    BoardHealthConnectionState state{};
    state.runtime[0U] = 1U;
    fxe::WriteU16LE(health.revision, state.runtime.data() + 1U);
    state.runtime[3U] = static_cast<uint8_t>(health.state);
    fxe::WriteU16LE(health.causes, state.runtime.data() + 4U);
    state.context[0U] = static_cast<uint8_t>(
        0x40U | static_cast<uint8_t>(persistence) |
        (sessionOnly ? 0x04U : 0U));
    state.context[1U] = progress.resetReason < 0xFFU
        ? static_cast<uint8_t>(progress.resetReason) : 0xFFU;
    fxe::WriteU32LE(progress.bootCount, state.context.data() + 2U);
    fxe::WriteU32LE(progress.watchdogRestarts, state.context.data() + 6U);
    return state;
}
SupportIncidentLine FormatSupportIncident(const runtime::ProgressSnapshot& p,
    const uint32_t saveCount, const uint32_t failureCount,
    const uint8_t bootStorageError) {
    SupportIncidentLine line;
    const auto& c = p.current.context;
    const auto& b = p.previous.context;
    (void)line.Format(
        "{\"event\":\"support_incident\",\"v\":1,\"rr\":%lu,\"boot\":%lu,\"wd\":%lu,"
        "\"up\":%lu,\"prior\":%u,\"prior_up\":%lu,\"phase\":%u,\"variant\":%u,"
        "\"expected\":\"%016llX\",\"observed\":\"%016llX\",\"hc\":%u,\"se\":%u,\"be\":%u,\"flags\":%u,"
        "\"saved\":%lu,\"failed\":%lu,\"prior_phase\":%u,\"prior_variant\":%u,"
        "\"prior_expected\":\"%016llX\",\"prior_observed\":\"%016llX\",\"prior_hc\":%u,\"prior_se\":%u,\"prior_flags\":%u,"
        "\"stages\":[%u,%u,%u,%u],\"stage_ms\":[%lu,%lu,%lu,%lu],\"now_stages\":[%u,%u,%u,%u],\"now_ms\":[%lu,%lu,%lu,%lu],\"recent\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        static_cast<unsigned long>(p.resetReason), static_cast<unsigned long>(p.bootCount),
        static_cast<unsigned long>(p.watchdogRestarts), static_cast<unsigned long>(c.uptimeMs),
        unsigned(p.previousValid), static_cast<unsigned long>(b.uptimeMs), unsigned(c.interactionPhase), unsigned(c.variant),
        static_cast<unsigned long long>(c.expectedOccupancy), static_cast<unsigned long long>(c.observedOccupancy),
        unsigned(c.healthCauses), unsigned(c.storageError), unsigned(bootStorageError), unsigned(c.flags),
        static_cast<unsigned long>(saveCount), static_cast<unsigned long>(failureCount),
        unsigned(b.interactionPhase), unsigned(b.variant), static_cast<unsigned long long>(b.expectedOccupancy),
        static_cast<unsigned long long>(b.observedOccupancy), unsigned(b.healthCauses), unsigned(b.storageError), unsigned(b.flags),
        unsigned(p.previous.stages[0]), unsigned(p.previous.stages[1]), unsigned(p.previous.stages[2]), unsigned(p.previous.stages[3]),
        static_cast<unsigned long>(p.previous.stageAtMs[0]), static_cast<unsigned long>(p.previous.stageAtMs[1]),
        static_cast<unsigned long>(p.previous.stageAtMs[2]), static_cast<unsigned long>(p.previous.stageAtMs[3]),
        unsigned(p.current.stages[0]), unsigned(p.current.stages[1]), unsigned(p.current.stages[2]), unsigned(p.current.stages[3]),
        static_cast<unsigned long>(p.current.stageAtMs[0]), static_cast<unsigned long>(p.current.stageAtMs[1]),
        static_cast<unsigned long>(p.current.stageAtMs[2]), static_cast<unsigned long>(p.current.stageAtMs[3]),
        RecentStage(p.previous, 0U), RecentStage(p.previous, 1U), RecentStage(p.previous, 2U), RecentStage(p.previous, 3U),
        RecentStage(p.previous, 4U), RecentStage(p.previous, 5U), RecentStage(p.previous, 6U), RecentStage(p.previous, 7U));
    return line;
}
void WriteSupportIncident(fxe::Console& console, const SupportIncidentLine& line) {
    console.BeginBulkOutput();
    console.PrintRaw(line.CStr(), line.Size());
    console.PrintRaw("\r\n", 2U);
    console.EndBulkOutput();
#if defined(LITCH_PANIC_DIAGNOSTIC)
    runtime::panic_diagnostic::PrintRetained(console);
#endif
}

}
