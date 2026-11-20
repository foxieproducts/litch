#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
static uint64_t modelNow = 0;
#include "firmware/led_runtime_admission.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/performance_telemetry.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_delivery_telemetry.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/current_game_log.hpp"
#include "modules/current_game_log_persistence.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "virtual_litch_session.hpp"
#include "bounded_hint_engine.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/board_storage_recovery.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/settings_registry.hpp"
#include "modules/first_player_migration.hpp"
#include "modules/game_persistence_scrub.hpp"
static std::function<void(unsigned)> advance;
static TaskFunction_t task;
static void *taskContext;
struct Stop {};
static unsigned notified = 0;
TaskHandle_t BatchCreate(TaskFunction_t f, const char *, uint32_t, void *c, uint32_t, StackType_t*, StaticTask_t*,
                       int) {
    task = f;
    taskContext = c;
    return reinterpret_cast<TaskHandle_t>(1U);
}
void BatchDelay(uint32_t n) { advance(n ? n : 1); }
void BatchDelete(TaskHandle_t) {}
uint32_t BatchTake(BaseType_t, uint32_t) {
    if (!notified)
        throw Stop{};
    notified = 0;
    return 1;
}
BaseType_t BatchGive(TaskHandle_t) {
    ++notified;
    return pdPASS;
}
unsigned BatchHighWater(TaskHandle_t) { return 0; }
static TaskHandle_t modeledTaskHandle = (void *)1;
TaskHandle_t xTaskGetCurrentTaskHandle() { return modeledTaskHandle; }
static SemaphoreHandle_t queueMutex = nullptr;
SemaphoreHandle_t BatchCreateMutex(StaticSemaphore_t* storage) {
    queueMutex = xSemaphoreCreateMutexStatic(storage);
    return queueMutex;
}
#define File BatchCapturedFile
#define LittleFS BatchCapturedLittleFS
#define ARDUINO 1
#define SettingsPersistenceWorker BatchSettingsPersistenceWorker
#define CurrentGameLogService BatchCurrentGameLogService
#define xTaskCreateStaticPinnedToCore BatchCreate
#define vTaskDelay BatchDelay
#define vTaskDelete BatchDelete
#define ulTaskNotifyTake BatchTake
#define xTaskNotifyGive BatchGive
#define uxTaskGetStackHighWaterMark BatchHighWater
#define xSemaphoreCreateMutexStatic BatchCreateMutex
#include "sdk/LittleFS.h"
static int BatchStat(const char *p, struct stat *out) {
    batch_sdk::Touch("stat");
    return ::stat(batch_sdk::Path(p).c_str(), out);
}
static int BatchTruncate(const char *p, off_t n) {
    batch_sdk::Touch("truncate");
    return ::truncate(batch_sdk::Path(p).c_str(), n);
}
namespace fxe {
bool TryMountLittleFsErasedPartitionSafe(bool) {
    batch_sdk::Touch("mount");
    return true;
}
} // namespace fxe
// Native SettingsStore SaveImpl remains the aggregate owner. The actual worker
// still acquires its hooks around that save; native filesystem calls are not
// individually observed by the log adapter SDK below.
#include "../../src/settings_persistence_worker.cpp"
#include "../../src/settings_persistence_worker_recovery.cpp"
#undef xSemaphoreCreateMutexStatic
#include "sdk/esp_attr.h"
#include "runtime/progress_watchdog.hpp"
namespace observed_progress {
using namespace runtime;
}
#define runtime observed_progress
#include "../../src/progress_watchdog_platform.cpp"
#undef runtime
namespace runtime {
void CountedFeedProgressWatchdog() { observed_progress::FeedProgressWatchdog(); }
} // namespace runtime
#define FeedProgressWatchdog CountedFeedProgressWatchdog
#define stat(path, out) BatchStat(path, out)
#define truncate(path, n) BatchTruncate(path, n)
#include "../../src/current_game_log_storage.cpp"
#undef FeedProgressWatchdog
#undef stat
#undef truncate
#include "../../src/current_game_log_service.cpp"
#include "../../src/current_game_log_service_session.cpp"
#undef ARDUINO
#undef File
#undef LittleFS
#undef SettingsPersistenceWorker
#undef CurrentGameLogService

static void Need(bool b, const char *s) {
    if (!b) {
        std::printf("FAIL %s\n", s);
        std::exit(4);
    }
}
static unsigned storageObservationDepth = 0U;
static void ObserveStorage(void*, const bool begin) {
    if (begin) {
        ++storageObservationDepth;
    } else {
        Need(storageObservationDepth != 0U, "storage observation scope balances");
        --storageObservationDepth;
    }
}
static unsigned ioCost = 0;
static uint64_t ioMs = 0, waitMs = 0;
static bool inIo = false;
static firmware::BatchSettingsPersistenceWorker* measuredWorker = nullptr;
static uint32_t lastObservedLogLease = 0U;
static void ObserveIo(const char *) {
    // Each SDK touch occurs inside its actual log lease. Native SettingsStore
    // uses stdio and does not enter this adapter callback.
    lastObservedLogLease = measuredWorker->GetDiagnostics().flashAccesses;
    if (storageObservationDepth == 0U) ++batch_sdk::unleased;
    inIo = true;
    advance(ioCost);
    ioMs += ioCost;
    inIo = false;
}
static std::filesystem::path FixturePath() {
    return std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path() /
           "data/terminal_checkmate_107.json";
}

// Opt-in diagnosis consumes the exact read-only capture without making an
// ignored physical evidence file a prerequisite for ordinary native runs.
static int RunCapturedDuckSaveCase(const char* capture, const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    batch_sdk::root = root;
    batch_sdk::access = nullptr;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modelNow = 0U;
    advance = [](const unsigned ms) {
        modelNow += ms;
        fxe::ManualMillis::SetNowMs(modelNow);
    };
    std::ifstream input(capture);
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    DynamicJsonDocument completed(16384U);
    Need(!deserializeJson(completed, json), "captured Duck JSON decodes");
    modules::GamePersistenceState state;
    Need(modules::ReadGamePersistenceState(completed, state), "captured Duck persistence reads");
    modules::game_history::GameHistorySnapshotReader reader(state);
    modules::game_history::GameHistoryCaptureSnapshot snapshot;
    const bool built = reader.Build(snapshot);
    std::printf("CAPTURED_DUCK snapshot_valid=%u state_bytes=%zu ply=%u revision=%llu\n",
        built, sizeof(state), state.gameHistory.currentPly,
        static_cast<unsigned long long>(state.gameHistory.lineRevision));
    Need(built, "captured Duck snapshot reader succeeds");

    fxe::SettingsStoreConfig config;
    const auto path = (root / "settings.json").string();
    config.path = path.c_str();
    fxe::SettingsStore source(config);
    source.Doc().set(completed);
    modules::current_game_log::CommitPointer capturedPointer;
    Need(modules::current_game_log::ReadCommitPointer(source.Doc(), capturedPointer), "captured pointer reads");
    source.Doc().remove("CURRENT_GAME_LOG_V2");
    source.Doc()["GAME_FEN"] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    source.Doc().remove("GAME_UNDO_BASE_FEN");
    source.Doc().remove("GAME_UNDO_BASE_MOVED_SQUARES");
    source.Doc().remove("GAME_UNDO_HISTORY");
    source.Doc().remove("GAME_DUCK_UNDO_V1");
    source.Doc()["GAME_VARIANT_V1"] = "01022701000001342c00";
    source.Doc()["GAME_INTERACTION_V1"] = "0306ff01000000000000000000000000020000000700ffff00008010efffffff00008010efff27ff0127ffffff00ffff00000000";
    source.Doc()["GAME_HISTORY_V1"][2U] = "0000000000000001";
    source.Doc()["GAME_HISTORY_V1"][3U] = 0U;
    hardware::RmtFlashArbiter arbiter;
    advance = [&](const unsigned ms) {
        modelNow += ms; fxe::ManualMillis::SetNowMs(modelNow);
        if (arbiter.TryBeginRender()) arbiter.EndRender();
    };
    firmware::BatchSettingsPersistenceWorker worker(config);
    firmware::CurrentGameLogStorage storage(worker);
    firmware::BatchCurrentGameLogService service(storage.Ops());
    Need(service.Configure(worker, source.Doc()) && (worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "captured worker configures");
    const auto save = [&](const char* stage) {
        Need(arbiter.TryBeginRender(), "captured render handoff"); arbiter.EndRender();
        Need(worker.Enqueue(source.Doc()), "captured snapshot enqueues");
        Need(arbiter.TryBeginRender(), "captured render handoff"); arbiter.EndRender();
        try { task(taskContext); } catch (const Stop&) {}
        const auto diagnostics = worker.GetDiagnostics();
        std::printf("CAPTURED_DUCK stage=%s requested=%u completed=%u failures=%u result=%u ms=%u ply=%u crc=%08x bytes=%u\n",
            stage, diagnostics.requested, diagnostics.completed, worker.GetFailureCount(),
            static_cast<unsigned>(service.LastResult()), diagnostics.settingsMs,
            service.CommittedPointer().currentPly, service.CommittedPointer().prefixCrc32,
            service.CommittedPointer().byteLength);
        return worker.GetFailureCount() == 0U;
    };
    Need(save("prior-pending"), "prior staged Duck snapshot saves");
    const auto prior = service.CommittedPointer();
    Need(prior.byteLength == capturedPointer.byteLength && prior.prefixCrc32 == capturedPointer.prefixCrc32,
         "reconstructed base matches physical durable length and CRC");
#if defined(LITCH_ESP32S3)
    if (std::getenv("LITCH_CURRENT_GAME_RESIDENT") != nullptr) {
        namespace store = modules::litchcraft_frame_store;
        {
            firmware::LitchcraftFrameFile files;
            Need(files.Remove(), "captured resident starts in isolated frame files");
            store::FrameStore seed(files.StoreOps());
            store::Manifest manifest{};
            manifest.frameCount = store::FRAME_COUNT;
            store::TransactionToken token{};
            Need(seed.Recover() == store::Status::NoArtwork &&
                 seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
                 "captured 256-frame resident begins");
            store::Frame frame{};
            frame.durationMs = 50U;
            for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
                frame.emitters[index].color = {static_cast<uint8_t>(index), 17U, 33U};
                Need(seed.WriteFrame(token, index, frame) == store::Status::Ok,
                     "captured resident frame writes");
            }
            Need(seed.Commit(token) == store::Status::Ok, "captured resident commits");
        }
        worker.NotifyLitchcraftFrameRuntimeReady();
        worker.NotifyLitchcraftFramePlayback(std::getenv("LITCH_CURRENT_GAME_PLAYING") != nullptr);
        try { task(taskContext); } catch (const Stop&) {}
        Need(worker.LitchcraftFrameStream().FrameCount() == store::FRAME_COUNT &&
             worker.LitchcraftFrameStream().TryPresentRequested(), "captured resident boot recovery");
        std::printf("CAPTURED_DUCK resident_frames=%u storage_active=%u generation=%u\n",
            worker.LitchcraftFrameStream().FrameCount(),
            worker.GetDiagnostics().frameStoreStorageActive,
            worker.LitchcraftFrameStream().Generation());
    }
#endif
    source.Doc().set(completed);
    Need(save("captured-complete"), "completed captured Duck snapshot saves");
    Need(service.CommittedPointer().currentPly == 1U, "captured compound turn promotes once");
    std::printf("PASS captured_Duck_pending_to_complete_real_worker_prepare\n");
    return 0;
}
int RunFullSaveCase(int argc, char **argv) {
    Need(argc == 4, "root cadence ioCost");
    std::filesystem::path root = argv[1];
    unsigned cadence = std::atoi(argv[2]);
    ioCost = std::atoi(argv[3]);
    std::filesystem::create_directories(root);
    batch_sdk::root = root;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);
    modelNow = 0;
    hardware::RmtFlashArbiter a;
    hardware::LedDeliveryTelemetry led;
    firmware::LedRuntimeAdmission admission;
    bool renderActive = false;
    uint64_t completeAt = 0;
    unsigned attemptsDuringIo = 0;
    unsigned transfer = 11;
    auto tick = [&] {
        fxe::ManualMillis::SetNowMs(modelNow);
        if (renderActive && modelNow >= completeAt) {
            led.OnOutcome(hardware::LedDeliveryOutcome::Confirmed, modelNow);
            a.EndRender();
            renderActive = false;
        }
        if (admission.Update(false, modelNow % cadence == 0, modelNow)) {
            led.OnAttempt(modelNow);
            if (a.TryBeginRender()) {
                // SDK flash/cache exclusion is outside this host cadence model.
                led.OnSubmission(modelNow);
                renderActive = true;
                completeAt = modelNow + transfer;
                admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed, modelNow);
            } else {
                led.OnOutcome(hardware::LedDeliveryOutcome::FlashDenied, modelNow);
                admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, modelNow);
                if (inIo)
                    ++attemptsDuringIo;
            }
        }
    };
    advance = [&](unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            ++modelNow;
            if (!inIo)
                ++waitMs;
            tick();
        }
    };
    tick();
    fxe::SettingsStoreConfig cfg;
    std::string path = (root / "s").string();
    cfg.path = path.c_str();
    fxe::SettingsStore source(cfg);
    std::ifstream fixture(FixturePath());
    std::string json((std::istreambuf_iterator<char>(fixture)), {});
    Need(!deserializeJson(source.Doc(), json), "actual107 fixture decode");

    const auto journeyPath = root / "j";
    std::filesystem::copy_file(FixturePath(), journeyPath,
                               std::filesystem::copy_options::overwrite_existing);
    // Frozen storage replay bounds incidental Hint work and bypasses its
    // calculation minimum/Classic preliminary pass; timing has separate coverage.
    test_support::BoundedHintEngine engine;
    virtual_litch_config vc{};
    virtual_litch_default_config(&vc);
    vc.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession journey(vc, engine, journeyPath.c_str());
    auto terminal = journey.State().expected_occupied_mask;
    journey.ObserveBootHallMask(terminal);
    Need(journey.UndoHistoryMoveCount() == 107, "journey loaded107");
    Need(journey.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X,
                               true) == VIRTUAL_LITCH_OK,
         "rawX");
    journey.Advance(20);
    journey.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
    journey.Advance(21);
    Need(journey.IsUndoActionPending(), "UndoPending");
    auto lifted = terminal & ~(1ULL << 40), premate = lifted | (1ULL << 19);
    journey.ObserveHallMask(lifted);
    journey.ObserveHallMask(premate);
    journey.Advance(1);
    Need(!journey.IsUndoActionPending() && journey.UndoHistoryMoveCount() == 106, "restore106");
    journey.Advance(2000);
    DynamicJsonDocument restoreDoc(16384);
    restoreDoc.set(journey.SettingsForPersistenceTest().Doc());
    journey.ObserveHallMask(lifted);
    journey.ObserveHallMask(terminal);
    journey.Advance(2000);
    Need(journey.UndoHistoryMoveCount() == 107 && engine.IsInCheckmate(modules::SideToMove::Black),
         "remate107");
    DynamicJsonDocument remateDoc(16384);
    remateDoc.set(journey.SettingsForPersistenceTest().Doc());
    // Restore deterministic measurement origin after actual production journey.
    modelNow = 0;
    fxe::ManualMillis::SetNowMs(0);
    firmware::BatchSettingsPersistenceWorker worker(cfg);
    firmware::CurrentGameLogStorage storage(worker);
    firmware::BatchCurrentGameLogService service(storage.Ops());
    Need(service.Configure(worker, source.Doc()), "full service configure");
    Need((worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "actual worker begin");
    measuredWorker = &worker;
    batch_sdk::access = &ObserveIo;
    for (unsigned transaction = 0; transaction < 5; ++transaction) {
        if (transaction == 2)
            source.Doc().set(restoreDoc);
        if (transaction == 3)
            source.Doc().set(remateDoc);
        if (transaction)
            modules::current_game_log::WriteCommitPointer(source.Doc(), service.CommittedPointer());
        const unsigned expectedPly = transaction == 2 ? 106 : 107;
        auto countsBefore = batch_sdk::counts;
        auto feedsBefore = batch_wdt_sdk::resets;
        auto readBytesBefore = batch_sdk::readBytes, writeBytesBefore = batch_sdk::writeBytes;
        auto at = modelNow;
        auto waits = waitMs;
        auto ios = ioMs;
        auto before = worker.GetDiagnostics();
        lastObservedLogLease = before.flashAccesses;
        auto wall = std::chrono::steady_clock::now();
        Need(worker.Enqueue(source.Doc()), "enqueue full snapshot");
        try {
            task(taskContext);
        } catch (const Stop &) {
        }
        auto d = worker.GetDiagnostics();
        Need(modelNow - at <= ioMs - ios + transfer,
             "saving waits only for actual I/O and an in-flight RMT transfer, never a future animation frame");
        Need(d.completed == d.requested && worker.GetFailureCount() == 0,
             "durable full transaction completed");
        Need(service.CommittedPointer().present &&
                 service.CommittedPointer().currentPly == expectedPly,
             "actual full line committed107");
        const uint32_t logPrepareLeases = lastObservedLogLease - before.flashAccesses;
        Need(logPrepareLeases <= 90U, "full Prepare bounded log handoff cost");
        std::printf("LOG_PREPARE index=%u leases=%u native_settings_backend=stdio\n",
                    transaction, logPrepareLeases);
        auto l = led.GetSnapshot();
        std::printf(
            "SAVE index=%u cadence=%u io_cost_ms=%u modeled_total_ms=%llu render_wait_ms=%llu "
            "io_ms=%llu native_wall_ms=%.3f flash_acquisitions=%u max_LED_gap_ms=%u unleased=%u "
            "completed=%u service_result=%u\n",
            transaction, cadence, ioCost, (unsigned long long)(modelNow - at),
            (unsigned long long)(waitMs - waits), (unsigned long long)(ioMs - ios),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall)
                .count(),
            d.flashAccesses - before.flashAccesses, l.maxConfirmationGapMs, batch_sdk::unleased,
            d.completed, unsigned(service.LastResult()));
        std::printf("PROGRESS index=%u ply=%u watchdog_feeds=%u uid=%s revision=%s\n", transaction,
                    expectedPly, batch_wdt_sdk::resets - feedsBefore,
                    source.Doc()["GAME_HISTORY_V1"][1].as<const char *>(),
                    source.Doc()["GAME_HISTORY_V1"][2].as<const char *>());
        if (transaction == 0 || transaction == 2 || transaction == 3)
            Need(batch_wdt_sdk::resets > feedsBefore,
                 "actual watchdog owner fed completed storage operations");
        std::printf("BYTES index=%u read=%llu write=%llu\n", transaction,
                    batch_sdk::readBytes - readBytesBefore,
                    batch_sdk::writeBytes - writeBytesBefore);
        for (auto &[k, v] : batch_sdk::counts)
            std::printf("TRANSACTION_IO index=%u operation=%s count=%u\n", transaction, k.c_str(),
                        v - countsBefore[k]);
        for (const char *name : {"s", "current_game_a.bin", "current_game_b.bin"}) {
            auto from = root / name;
            if (std::filesystem::exists(from))
                std::filesystem::copy_file(
                    from, root / ("state" + std::to_string(transaction) + "-" + name),
                    std::filesystem::copy_options::overwrite_existing);
        }
    }

    auto ops = storage.Ops();
    auto authority = service.CommittedPointer();
    Need(ops.beginBatch(ops.context), "independent durable validator batch");
    modules::current_game_log::CurrentGameLog reopened(ops, {});
    Need(reopened.Open(authority) == modules::current_game_log::Result::Ready,
         "full durable parser reopened107");
    ops.endBatch(ops.context);
    for (unsigned fault = 0; fault < 2; ++fault) {
        // Both controlled failures target the alternate candidate slot; prior committed authority
        // must survive.
        source.Doc().set(restoreDoc);
        source.Doc()["GAME_HISTORY_V1"][2] = "0000000000000081";
        modules::current_game_log::WriteCommitPointer(source.Doc(), authority);
        if (fault == 0)
            batch_sdk::shortRead = true;
        else
            batch_sdk::failLogRename = true;
        auto failures = worker.GetFailureCount();
        Need(worker.Enqueue(source.Doc()), "fault transaction enqueue");
        try {
            task(taskContext);
        } catch (const Stop &) {
        }
        Need(worker.GetFailureCount() > failures &&
                 service.CommittedPointer().generation == authority.generation,
             "failed candidate does not promote");
        fxe::SettingsStore durable(cfg);
        Need(durable.Load(), "prior settings survives failure");
        modules::current_game_log::CommitPointer prior;
        Need(modules::current_game_log::ReadCommitPointer(durable.Doc(), prior) &&
                 prior.generation == authority.generation,
             "prior durable pointer survives failure");
        Need(ops.beginBatch(ops.context), "prior validator batch");
        modules::current_game_log::CurrentGameLog intact(ops, {});
        Need(intact.Open(prior) == modules::current_game_log::Result::Ready,
             "prior log intact after failure");
        ops.endBatch(ops.context);
        std::printf("GUARD controlled_fault=%s result=PASS\n",
                    fault == 0 ? "short_read" : "log_promotion_failure");
    }
    Need(worker.Enqueue(source.Doc()), "retry exact failed snapshot");
    try {
        task(taskContext);
    } catch (const Stop &) {
    }
    Need(service.CommittedPointer().currentPly == 106, "subsequent transaction recovers106");
    source.Doc().set(remateDoc);
    source.Doc()["GAME_HISTORY_V1"][2] = "0000000000000082";
    modules::current_game_log::WriteCommitPointer(source.Doc(), service.CommittedPointer());
    Need(worker.Enqueue(source.Doc()), "recovery remate");
    try {
        task(taskContext);
    } catch (const Stop &) {
    }
    Need(service.CommittedPointer().currentPly == 107, "recovery remate107");
    auto slot = service.CommittedPointer().slot;
    const auto committedFile =
        root /
        (slot == modules::current_game_log::Slot::A ? "current_game_a.bin" : "current_game_b.bin");
    auto data = std::vector<uint8_t>(std::filesystem::file_size(committedFile));
    {
        std::ifstream in(committedFile, std::ios::binary);
        in.read((char *)data.data(), data.size());
    }
    auto original = data;
    data[20] ^= 1;
    {
        std::ofstream out(committedFile, std::ios::binary);
        out.write((char *)data.data(), data.size());
    }
    Need(ops.beginBatch(ops.context), "corrupt validator batch");
    modules::current_game_log::CurrentGameLog corrupt(ops, {});
    Need(corrupt.Open(service.CommittedPointer()) != modules::current_game_log::Result::Ready,
         "corrupt CRC rejected");
    ops.endBatch(ops.context);
    {
        std::ofstream out(committedFile, std::ios::binary);
        out.write((char *)original.data(), original.size());
    }
    std::printf("GUARD corrupt_crc_rejected_prior_authority_and_recovery=PASS\n");
    // Adapter boundary cases exercise EOF, cross-window reads, mutation and slot invalidation.
    const auto probeSlot = slot == modules::current_game_log::Slot::A
                               ? modules::current_game_log::Slot::B
                               : modules::current_game_log::Slot::A;
    std::array<uint8_t, 600> pattern{};
    for (unsigned i = 0; i < pattern.size(); ++i)
        pattern[i] = i % 251;
    Need(ops.beginBatch(ops.context), "adapter guard batch");
    Need(ops.replace(ops.context, probeSlot, pattern.data(), pattern.size()), "guard replace");
    std::array<uint8_t, 300> read{};
    for (auto pair : {std::pair<unsigned, unsigned>{0, 1}, {250, 20}, {590, 10}, {0, 300}}) {
        Need(ops.read(ops.context, probeSlot, pair.first, read.data(), pair.second) &&
                 std::memcmp(read.data(), pattern.data() + pair.first, pair.second) == 0,
             "cache crosswindow or fallback exact");
    }
    Need(ops.read(ops.context, probeSlot, 0, read.data(), 1), "prime cache before owner guard");
    modeledTaskHandle = nullptr;
    Need(!ops.read(ops.context, probeSlot, 0, read.data(), 1), "cache cannot bypass worker owner");
    modeledTaskHandle = (void *)1;
    Need(!ops.read(ops.context, probeSlot, 599, read.data(), 2), "EOF rejected");
    Need(ops.read(ops.context, probeSlot, 600, read.data(), 0), "zero EOF accepted");
    uint8_t extra = 241;
    Need(ops.append(ops.context, probeSlot, &extra, 1), "append invalidates");
    Need(ops.read(ops.context, probeSlot, 600, read.data(), 1) && read[0] == extra,
         "new appended byte visible");
    Need(ops.truncate(ops.context, probeSlot, 20), "truncate invalidates");
    Need(!ops.read(ops.context, probeSlot, 20, read.data(), 1), "truncated bytes unavailable");
    pattern[0] = 99;
    Need(ops.replace(ops.context, probeSlot, pattern.data(), 300), "replace invalidates");
    Need(ops.read(ops.context, probeSlot, 0, read.data(), 1) && read[0] == 99,
         "replacement visible");
    Need(ops.read(ops.context, slot, 0, read.data(), 1), "slot switch invalidates");
    Need(ops.read(ops.context, probeSlot, 0, read.data(), 1) && read[0] == 99, "switch back exact");
    ops.endBatch(ops.context);
    std::printf("GUARD cache_EOF_crosswindow_large_mutation_slot_batch=PASS adapter_size=%zu\n",
                sizeof(storage));
    batch_sdk::access = nullptr;
    fxe::SettingsStore verify(cfg);
    Need(verify.Load(), "native settings reopen");
    modules::current_game_log::CommitPointer pointer;
    Need(modules::current_game_log::ReadCommitPointer(verify.Doc(), pointer) &&
             pointer.currentPly == 107,
         "promoted pointer durable107");
    for (auto &[k, v] : batch_sdk::counts)
        std::printf("IO operation=%s count=%u\n", k.c_str(), v);
    Need(!batch_sdk::unleased, "actual filesystem calls remain within worker observation scope");
    measuredWorker = nullptr;
    std::printf("PASS full_worker_Prepare_parser_transaction_durable107\n");
    return 0;
}

static void CheckFsLease() {
    if (storageObservationDepth == 0U) ++batch_sdk::unleased;
}
static void Checkpoint(runtime::VirtualLitchSession &s, const char *name, const char *next) {
    auto st = s.State();
    modules::ControlMutationOutcome ack{};
    bool present = s.TryGetLastBoardControlOutcome(ack);
    auto f = s.SemanticFrame();
    auto p = s.PhysicalFrame();
    auto h = s.HomeStateSnapshot();
    std::printf("JOURNEY checkpoint=%s time_ms=%u physical=%016llx expected=%016llx phase=%u "
                "required=%u pending_token=%u history=%u undo_pending=%u ack_present=%u request=%u "
                "status=%u action_state=%u display=%.*s next=%s fen=%s\n",
                name, st.now_ms, (unsigned long long)st.physical_occupied_mask,
                (unsigned long long)st.expected_occupied_mask, st.interaction_phase,
                st.required_action, st.pending_action_token, s.UndoHistoryMoveCount(),
                s.IsUndoActionPending(), present, ack.requestId, unsigned(ack.status),
                unsigned(ack.actionState), int(s.DisplayText().size()), s.DisplayText().data(),
                next, h.fen ? h.fen : "");
    bool rotate = modules::ComputeSquareRotate(s.BoardRotate(), s.FirstPlayerIsP2(), false);
    for (unsigned square : {19U, 40U}) {
        auto mapped = runtime::MapSquareIndex(square, rotate);
        std::printf("CUE checkpoint=%s square=%s semantic_legacy_rgb=", name,
                    square == 19 ? "d6" : "a3");
        for (unsigned i = 0; i < f.leds_per_square; ++i) {
            auto c = f.board[square][i];
            std::printf("%u,%u,%u;", c.r, c.g, c.b);
        }
        std::printf(" mapped_V3_square=%u physical_rgbw=", mapped);
        for (unsigned i = 0; i < 4; ++i) {
            auto c = p.pixels[mapped * 4 + i];
            std::printf("%u,%u,%u,%u;", c.r, c.g, c.b, c.w);
        }
        std::printf("\n");
    }
}
// The workload reaches the production storage adapter and arbitration owners.
// Native SDK boundaries replace task scheduling, LittleFS and RMT completion.
// The complete Prepare callback and binary log parser are intentionally absent.
// 250 ms rejects the observed multi-second freeze; it is not a timing contract.
int RunJourney(const char *settingsPath, unsigned cost, bool restoreOnly) {
    batch_sdk::bytes.assign(2048, 7);
    batch_sdk::root = std::filesystem::path(settingsPath).parent_path();
    {
        std::ofstream file(batch_sdk::root / "current_game_a.bin", std::ios::binary);
        file.write(reinterpret_cast<const char *>(batch_sdk::bytes.data()),
                   batch_sdk::bytes.size());
    }
    batch_sdk::calls = batch_sdk::unleased = 0;
    batch_sdk::access = nullptr;
    batch_wdt_sdk::resets = 0;
    advance = nullptr;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);
    // Use the same bounded Hint policy as the other frozen-clock storage replays.
    test_support::BoundedHintEngine engine;
    virtual_litch_config cfg{};
    virtual_litch_default_config(&cfg);
    cfg.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession session(cfg, engine, settingsPath);
    auto terminal = session.State().expected_occupied_mask;
    session.ObserveBootHallMask(terminal);
    Need(session.UndoHistoryMoveCount() == 107, "full actual107 history");
    Checkpoint(session, "loaded107mate", "press raw P1 X");
    Need(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X,
                               true) == VIRTUAL_LITCH_OK,
         "raw Undo down");
    session.Advance(20);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
    session.Advance(20);
    session.Advance(1);
    Need(session.IsUndoActionPending() && session.UndoHistoryMoveCount() == 106, "Undo ACK and106");
    modules::ControlMutationOutcome undoAck{};
    Need(session.TryGetLastBoardControlOutcome(undoAck) &&
             undoAck.status == modules::ControlMutationStatus::Applied &&
             undoAck.actionState == modules::ControlActionState::UndoPending,
         "actual typed Applied UndoPending ACK");
    Checkpoint(session, "rawX-Applied-UndoPending106", "lift queen a3");
    const auto lifted = terminal & ~(1ULL << 40), premate = lifted | (1ULL << 19);
    session.ObserveHallMask(lifted);
    Checkpoint(session, "Undo-lift-a3", "place queen d6");
    session.ObserveHallMask(premate);
    session.Advance(1);
    Need(!session.IsUndoActionPending() && session.State().expected_occupied_mask == premate,
         "physical restore complete");
    Checkpoint(session, "Undo-return-d6-playready106", "play Qd6-a3");
    if (!restoreOnly) {
        session.ObserveHallMask(lifted);
        Checkpoint(session, "remate-lift-d6", "place queen a3");
        session.ObserveHallMask(terminal);
        session.Advance(2000);
        Need(session.UndoHistoryMoveCount() == 107 &&
                 engine.IsInCheckmate(modules::SideToMove::Black),
             "production mate action/result");
        Checkpoint(session, "remate107-terminal", "terminal animation continues; X available");
    } else {
        for (unsigned settle = 0; settle < 100; ++settle)
            session.Advance(20);
        Checkpoint(session, "restore106-contention-start", "play remains ready");
    }
    const unsigned historyCount = session.UndoHistoryMoveCount();
    auto &settings = session.SettingsForPersistenceTest();
    const std::string hex = settings.Doc()["GAME_UNDO_HISTORY"].as<std::string>();
    Need(hex.size() == historyCount * 14, "actual authoritative history encoding");
    modules::SettingsStoreConfig wc;
    wc.path = settingsPath;
    modules::SettingsStore source(wc);
    source.Doc().set(settings.Doc());
    hardware::RmtFlashArbiter arbiter;
    firmware::LedRuntimeAdmission admission;
    hardware::LedDeliveryTelemetry delivery;
    fxe::PerformanceTelemetry loop;
    uint32_t now = session.State().now_ms;
    uint64_t latched = 0, previousDesired = 0;
    unsigned desiredChanges = 0, deliveredChanges = 0;
    auto tick = [&] {
        fxe::ManualMillis::SetNowMs(now);
        loop.OnLoop();
        auto desired = session.PhysicalFrame().frame_hash;
        if (desired != previousDesired) {
            ++desiredChanges;
            previousDesired = desired;
        }
        if (admission.Update(false, true, now)) {
            delivery.OnAttempt(now);
            if (arbiter.TryBeginRender()) {
                delivery.OnSubmission(now);
                if (latched != desired)
                    ++deliveredChanges;
                latched = desired;
                delivery.OnOutcome(hardware::LedDeliveryOutcome::Confirmed, now);
                arbiter.EndRender();
                admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed, now);
            } else {
                delivery.OnOutcome(hardware::LedDeliveryOutcome::FlashDenied, now);
                admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, now);
            }
        }
    };
    advance = [&](unsigned duration) {
        while (duration) {
            unsigned d = std::min(duration, 20U);
            now += d;
            session.Advance(d);
            tick();
            duration -= d;
        }
    };
    tick();
    firmware::BatchSettingsPersistenceWorker worker(wc);
    Need((worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "existing actual worker Begin");
    firmware::CurrentGameLogStorage storage(worker);
    auto ops = storage.Ops();
    batch_sdk::access = [](const char *) { CheckFsLease(); };
    Need(ops.beginBatch(ops.context), "actual production BeginBatch");
    modules::LitchMaxEngine replay;
    Need(replay.SetFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
         "history replay initial");
    unsigned reads = 0;
    for (unsigned ply = 0; ply < historyCount; ++ply) {
        uint8_t b[7];
        const auto feedsBefore = batch_wdt_sdk::resets;
        Need(ops.read(ops.context, modules::current_game_log::Slot::A, ply * 7, b, 1),
             "header read");
        ++reads;
        Need(ops.read(ops.context, modules::current_game_log::Slot::A, ply * 7, b, 7),
             "record read");
        ++reads;
        Need(batch_wdt_sdk::resets == feedsBefore + 2,
             "both successful batched reads feed real progress SDK owner");
        auto byte = [&](unsigned i) {
            char t[3] = {hex[(ply * 7 + i) * 2], hex[(ply * 7 + i) * 2 + 1], 0};
            return unsigned(std::strtoul(t, nullptr, 16));
        };
        unsigned from = byte(5), to = byte(6);
        char uci[6] = {char('a' + from % 8),
                       char('8' - from / 8),
                       char('a' + to % 8),
                       char('8' - to / 8),
                       0,
                       0};
        auto flag = byte(4);
        if (flag) {
            char p = char(flag);
            if (p == 'q' || p == 'r' || p == 'b' || p == 'n')
                uci[4] = p;
        }
        Need(replay.ApplyUciMove(uci), "real engine history replay");
        const auto beforeDelay = batch_wdt_sdk::resets;
        advance(cost);
        Need(batch_wdt_sdk::resets == beforeDelay, "scheduler delay never feeds storage watchdog");
    }
    ops.endBatch(ops.context);
    advance(600);
    const uint8_t payload[] = {1, 2, 3};
    Need(ops.append(ops.context, modules::current_game_log::Slot::A, payload, sizeof(payload)),
         "neighbor protected Append");
    Need(ops.replace(ops.context, modules::current_game_log::Slot::B, payload, sizeof(payload)),
         "neighbor protected Replace");
    Need(ops.beginBatch(ops.context), "neighbor begin batch");
    Need(!ops.beginBatch(ops.context), "nested batch rejected");
    uint8_t one = 0;
    Need(ops.read(ops.context, modules::current_game_log::Slot::B, 0, &one, 1),
         "neighbor cached read");
    Need(!ops.read(ops.context, modules::current_game_log::Slot::B, 999999, &one, 1),
         "out of range read rejected");
    Need(!ops.read(ops.context, modules::current_game_log::Slot::B, 0, nullptr, 1),
         "null read rejected");
    Need(ops.append(ops.context, modules::current_game_log::Slot::B, payload, sizeof(payload)),
         "cached file closes inside Append lease");
    Need(ops.replace(ops.context, modules::current_game_log::Slot::B, payload, sizeof(payload)),
         "batch Replace remains protected");
    ops.endBatch(ops.context);
    ops.endBatch(ops.context);
    Need(ops.read(ops.context, modules::current_game_log::Slot::B, 0, &one, 1),
         "outside batch read protected");
    std::filesystem::remove(batch_sdk::root / "current_game_b.bin");
    Need(!ops.truncate(ops.context, modules::current_game_log::Slot::B, 0),
         "absent native truncate path rejects");
    Need(arbiter.TryBeginRender(), "failure paths release flash lease");
    arbiter.EndRender();
    auto led = delivery.GetSnapshot();
    auto cadence = loop.GetSnapshot();
    std::printf(
        "REGRESSION actual_history=%u native_mate=%u batch_reads=%u controlled_CPU_ms_per_ply=%u "
        "loop_max_ms=%u desired_animation_changes=%u delivered_changes=%u max_confirm_gap_ms=%u "
        "flash_denied=%u sdk_calls=%u unobserved_FS_calls=%u flash_lease_acquisitions=%u "
        "completed_render_generations=%u watchdog_resets=%u\n",
        historyCount, restoreOnly ? 0U : 1U, reads, cost, cadence.maxLoopDeltaMs, desiredChanges,
        deliveredChanges, led.maxConfirmationGapMs, led.outcomes[0], batch_sdk::calls,
        batch_sdk::unleased, worker.GetDiagnostics().flashAccesses,
        arbiter.CompletedRenderGeneration(), batch_wdt_sdk::resets);
    bool pass = led.maxConfirmationGapMs <= 250 && batch_sdk::unleased == 0 &&
                (restoreOnly || (desiredChanges > 1 && deliveredChanges > 1));
    std::printf("%s "
                "terminal_animation_confirmed_gap_must_be_le250ms_during_bounded_between_operation_"
                "validation\n",
                pass ? "PASS" : "FAIL");
    advance = nullptr;
    batch_sdk::access = nullptr;
    return pass ? 0 : 1;
}


static int RunOpeningUndoAndDuckJourney(const std::filesystem::path& root, const bool duck,
    const char* followup = nullptr, const bool failFinalSave = false) {
    std::filesystem::create_directories(root);
    batch_sdk::root = root;
    batch_sdk::counts.clear(); batch_sdk::calls = batch_sdk::unleased = 0U;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    modelNow = 0U; fxe::ManualMillis::SetNowMs(0U);
    virtual_litch_config cfg{}; virtual_litch_default_config(&cfg);
    cfg.hardware = VIRTUAL_LITCH_HARDWARE_S3; cfg.strong_power_source = 1U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(cfg, engine);
    unsigned identitySequence = 0U;
    session.RunControlIngress(+[](void* context, modules::SettingsStore&, modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
        game.SetGameHistoryIdentityGenerator(+[](void* inner, modules::game_history::GameId& id) {
            id = {}; id.bytes.back() = static_cast<uint8_t>(++*static_cast<unsigned*>(inner)); return true;
        }, context);
        Need(game.ResetEngine(), "normal new-game creates production history identity");
    }, &identitySequence);
    auto& source = session.SettingsForPersistenceTest();
    hardware::RmtFlashArbiter arbiter;
    const std::string path = (root / "s").string();
    fxe::SettingsStoreConfig storageConfig; storageConfig.path = path.c_str();
    firmware::BatchSettingsPersistenceWorker worker(storageConfig);
    firmware::CurrentGameLogStorage storage(worker);
    firmware::BatchCurrentGameLogService service(storage.Ops());
    Need(service.Configure(worker, source.Doc()) && (worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "opening journey real persistence owners begin");
    firmware::BatchSettingsPersistenceWorker::SaveReceipt receipt = 0U;
    source.SetAsyncSaveAdmissionCallback([&](const JsonDocument& document) {
        auto admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
        (void)worker.EnqueueTracked(document, receipt, &admission);
        return admission;
    });
    batch_sdk::access = +[](const char*) {};
    advance = [&](unsigned n) {
        for (unsigned i = 0U; i < n; ++i) {
            ++modelNow; fxe::ManualMillis::SetNowMs(modelNow);
            if (arbiter.TryBeginRender()) arbiter.EndRender();
        }
    };
    const auto syncClock = [&] { modelNow = session.State().now_ms; fxe::ManualMillis::SetNowMs(modelNow); };
    const auto trace = [&](const char* stage, const double wallMs) {
        modules::GamePersistenceState state;
        Need(modules::ReadGamePersistenceState(source.Doc(), state), "journey reads actual snapshot");
        const auto d = worker.GetDiagnostics();
        const auto policy = modules::led_render_policy::MakePowerBudgetPolicyConfig({true, false});
        std::string identity; serializeJson(source.Doc()["GAME_HISTORY_V1"][1U], identity);
        std::printf("OPENING_JOURNEY kind=%s stage=%s now=%u wall_host_ms=%.3f variant=%u game_id=%s revision=%llu ply=%u history=%u physical=%016llx expected=%016llx phase=%u req=%u done=%u receipt=%u outcome=%u prepare=%u source_error=%u confirmed=%u disabled=%u saved=%u failed=%u settings_model_ms=%u flash_accesses=%u log_bytes=%u log_ply=%u cap_ma=%u io_touches=%u text=%.*s\n",
          duck ? "DuckSetupAddRemove" : "OpeningTwoUndo", stage, session.State().now_ms, wallMs,
          unsigned(session.GameVariant()), identity.c_str(), (unsigned long long)state.gameHistory.lineRevision,
          state.gameHistory.currentPly, session.UndoHistoryMoveCount(),
          (unsigned long long)session.State().physical_occupied_mask, (unsigned long long)session.State().expected_occupied_mask,
          session.State().interaction_phase, d.requested, d.completed, receipt, unsigned(worker.GetSaveOutcome(receipt)),
          unsigned(service.LastResult()), unsigned(source.GetLastError()), source.HasConfirmedStorageFailure(), source.IsDisabled(),
          source.GetSaveCount(), worker.GetFailureCount(), d.settingsMs, d.flashAccesses,
          service.CommittedPointer().byteLength, service.CommittedPointer().currentPly, policy.maxLedCurrentMa,
          batch_sdk::calls, int(session.DisplayText().size()), session.DisplayText().data());
        std::fflush(stdout);
    };
    const auto input = [&](const char* stage, auto action) {
        const auto before = batch_sdk::calls;
        const auto at = std::chrono::steady_clock::now();
        action(); syncClock();
        const double wall = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count();
        trace(stage, wall);
        Need(batch_sdk::calls == before, "Hall/control and variant prep precede asynchronous filesystem work");
    };
    const auto drain = [&](const char* stage) {
        Need(session.Advance(120U) == VIRTUAL_LITCH_OK, "save delay advances"); syncClock();
        const auto requestedBefore = worker.GetDiagnostics().requested;
        const auto at = std::chrono::steady_clock::now();
        if (failFinalSave && std::string(stage)=="second-undo-save") {
            std::filesystem::permissions(root, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        }
        source.Update();
        if (worker.GetDiagnostics().requested != requestedBefore) {
            Need(arbiter.TryBeginRender(), "opening preworker render"); arbiter.EndRender();
            try { task(taskContext); } catch(const Stop&) {}
        }
        const auto wall = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count();
        if (modelNow > session.State().now_ms) Need(session.Advance(modelNow-session.State().now_ms) == VIRTUAL_LITCH_OK, "worker elapsed session update");
        else Need(session.Advance(1U) == VIRTUAL_LITCH_OK, "runtime failure policy observes completion");
        trace(stage,wall);
        if(receipt) { Need(worker.ReleaseSaveReceipt(receipt), "release observed receipt"); receipt = 0U; }
        if (failFinalSave) std::filesystem::permissions(root, std::filesystem::perms::owner_all);
    };
    constexpr uint64_t initial = 0xFFFF00000000FFFFULL;
    uint64_t hall = initial;
    input("starting-board", [&] { session.ObserveBootHallMask(hall); session.Advance(2001U); });
    drain("starting-save");
    if (duck) {
        constexpr uint64_t h4 = 1ULL << 39U;
        input("duck-h4-added", [&] { hall |= h4; session.ObserveHallMask(hall); session.Advance(2001U); });
        drain("duck-added-save");
        input("duck-h4-removed", [&] { hall &= ~h4; session.ObserveHallMask(hall); session.Advance(2001U); });
        drain("duck-removed-save");
        input("duck-h4-readded", [&] { hall |= h4; session.ObserveHallMask(hall); session.Advance(2001U); });
        drain("duck-readded-save");
        const bool ok = !source.IsDisabled() && !source.HasConfirmedStorageFailure() && worker.GetFailureCount() == 0U;
        std::printf("%s DuckSetupAddRemove real-Hall variant/snapshot/worker/log save sequence\n",ok?"PASS":"FAIL");
        return ok?0:1;
    }
    const auto move = [&](const char* stage, unsigned from, unsigned to) {
        input(stage,[&] { hall &= ~(1ULL << from); session.ObserveHallMask(hall); hall |= 1ULL << to; session.ObserveHallMask(hall); session.Advance(2001U); });
    };
    move("e2e4",52U,36U); drain("e2e4-save");
    Need(session.UndoHistoryMoveCount()==1U && service.CommittedPointer().currentPly==1U,"first played snapshot durable");
    move("e7e5",12U,28U); drain("e7e5-save");
    Need(session.UndoHistoryMoveCount()==2U && service.CommittedPointer().currentPly==2U,"second played snapshot durable");
    const auto undo = [&](const char* stage,unsigned from,unsigned to) {
        input(stage,[&] {
            session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::X,true); session.Advance(20U);
            session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::X,false); session.Advance(20U);
            Need(session.IsUndoActionPending(),"physical X opens inverse plan");
            hall &= ~(1ULL << from); session.ObserveHallMask(hall); hall |= 1ULL << to; session.ObserveHallMask(hall); session.Advance(2001U);
        });
    };
    undo("undo-e5e7",28U,12U); drain("first-undo-save");
    Need(session.UndoHistoryMoveCount()==1U,"first completed Undo leaves one ply");
    const auto preFinalAuthority = service.CommittedPointer();
    std::ifstream beforePrimary(path); const std::string primaryBefore((std::istreambuf_iterator<char>(beforePrimary)), {});
    undo("undo-e4e2",36U,52U); drain("second-undo-save");
    Need(session.UndoHistoryMoveCount()==0U && hall==initial,"second completed Undo restores ordinary start");
    if (failFinalSave) {
        std::ifstream afterPrimary(path); const std::string primaryAfter((std::istreambuf_iterator<char>(afterPrimary)), {});
        const auto authorityAfter = service.CommittedPointer();
        const bool retained = primaryBefore==primaryAfter && authorityAfter.gameId==preFinalAuthority.gameId &&
            authorityAfter.lineRevision==preFinalAuthority.lineRevision && authorityAfter.byteLength==preFinalAuthority.byteLength &&
            authorityAfter.prefixCrc32==preFinalAuthority.prefixCrc32 && worker.GetFailureCount()!=0U;
        std::printf("%s OpeningTwoUndo controlled failed promotion preserves exact old primary and committed authority; RAM remains Fresh Setup\n",retained?"PASS":"FAIL");
        return retained?0:1;
    }
    Need(!source.Doc()[modules::KEY_GAME_ACCEPTED_PLAY].as<bool>(), "final untimed Undo snapshot clears accepted-play provenance");
    {
        test_support::BoundedHintEngine restoredEngine;
        runtime::VirtualLitchSession restored(cfg, restoredEngine, path.c_str());
        restored.ObserveBootHallMask(initial);
        Need(restored.Advance(201U) == VIRTUAL_LITCH_OK &&
             restored.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK &&
             restored.HomeStateSnapshot().placementMode && restored.State().human_illegal_mask == 0ULL,
             "durable reload after final untimed Undo restores ordinary Setup");
        std::printf("PASS OpeningTwoUndo durable_reload untimed_fresh_setup=restored\n");
    }
    const bool oldAckRejected = service.AdmitArchiveAcknowledgement(preFinalAuthority, {}) == modules::current_game_sync::ArchiveAdmission::Rejected;
    const bool ok = !source.IsDisabled() && !source.HasConfirmedStorageFailure() && worker.GetFailureCount()==0U && !service.CommittedPointer().present && oldAckRejected;
    std::printf("ARCHIVE_BOUNDARY old_identity_ack_rejected=%u old_files_retained=%u\n",oldAckRejected,
        std::filesystem::exists(root/"current_game_a.bin") || std::filesystem::exists(root/"current_game_b.bin"));
    std::printf("%s OpeningTwoUndo ordinary e2e4/e7e5/raw-X Hall inverses must remain durably writable\n",ok?"PASS":"FAIL");
    if (ok && (followup != nullptr && std::string(followup) == "classic")) {
        move("after-undo-Classic-e2e4",52U,36U); drain("after-undo-Classic-turn-save");
        const bool nextOk = !source.IsDisabled() && worker.GetFailureCount()==0U && service.CommittedPointer().currentPly==1U && !(service.CommittedPointer().gameId==preFinalAuthority.gameId);
        std::printf("%s OpeningTwoUndo followup Classic move under new identity\n",nextOk?"PASS":"FAIL");
        return nextOk?0:1;
    }
    if (ok && (followup != nullptr && std::string(followup) == "duck")) {
        const bool refuseConsumer = std::getenv("LITCH_OPENING_CONSUMER_REFUSAL") != nullptr;
        const bool relocateOpening = std::getenv("LITCH_OPENING_FREE_DUCK") != nullptr;
        input("after-undo-add-Duck", [&] { hall |= 1ULL << (relocateOpening ? 44U : 39U); session.ObserveHallMask(hall); session.Advance(2001U); });
        drain("after-undo-Duck-selected-save");
        if (refuseConsumer) {
            std::unique_lock<std::mutex> held(*queueMutex);
            firmware::BatchSettingsPersistenceWorker::SaveReceipt busyReceipt = 0U;
            auto admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
            auto request = std::async(std::launch::async, [&] {
                return worker.EnqueueTracked(source.Doc(), busyReceipt, &admission);
            });
            const bool returned = request.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
            held.unlock();
            const bool accepted = request.get();
            Need(returned && !accepted && busyReceipt == 0U &&
                     admission == fxe::SettingsStore::AsyncSaveAdmission::Deferred,
                 "held consumer queue lock returns Deferred without waiting or owning a receipt");
        }
        if (relocateOpening) {
            input("after-undo-Duck-blocked-e2", [&] {
                hall &= ~(1ULL << 52U); session.ObserveHallMask(hall); session.Advance(20U);
                hall |= 1ULL << 52U; session.ObserveHallMask(hall); session.Advance(20U);
            });
            Need(session.HomeStateSnapshot().vexMoveFirst,
                 "first blocked pickup requests free Vex relocation through production ingress");
            input("after-undo-Duck-free-e3h4", [&] {
                hall &= ~(1ULL << 44U); session.ObserveHallMask(hall);
                hall |= 1ULL << 39U; session.ObserveHallMask(hall); session.Advance(2001U);
            });
            if (std::getenv("LITCH_OPENING_COALESCE") == nullptr) drain("after-undo-Duck-free-save");
        }
        move("after-undo-Duck-e2e4",52U,36U);
        if (std::getenv("LITCH_OPENING_COALESCE") == nullptr) drain("after-undo-Duck-pending-save");
        input("after-undo-Duck-h4h5", [&] { hall &= ~(1ULL<<39U); session.ObserveHallMask(hall); hall |= 1ULL<<31U; session.ObserveHallMask(hall); session.Advance(2001U); });
        if (refuseConsumer) {
            // Resource refusal in the real log adapter, not a failed flash
            // primitive. The shared preferences remain live during this save.
            batch_sdk::staleSize = true;
            source.Doc()["BRIGHTNESS"] = 7U;
            source.MarkDirty();
        }
        drain("after-undo-Duck-turn-save");
        if (refuseConsumer) {
            Need(worker.GetFailureCount() == 1U && !source.HasConfirmedStorageFailure() &&
                     !source.HasTerminalSaveFailure() && !source.IsDisabled(),
                 "consumer metadata refusal cannot disable global settings saving");
            batch_sdk::staleSize = false;
            source.Doc()["BRIGHTNESS"] = 8U;
            source.MarkDirty();
            source.Update();
            session.Advance(2000U);
            drain("after-undo-Duck-consumer-retry");
            fxe::SettingsStore durable(storageConfig);
            Need(durable.Load() && durable.ReadValue("BRIGHTNESS", 0U) == 8U &&
                     service.CommittedPointer().currentPly == 1U && !source.IsDisabled(),
                 "consumer retry durably saves the latest preference and complete Duck turn");
        }
        const bool overlapArchive = std::getenv("LITCH_OPENING_ARCHIVE_OVERLAP") != nullptr;
        if (overlapArchive) Need(service.AdmitArchiveAcknowledgement(service.CommittedPointer(), {}) ==
            modules::current_game_sync::ArchiveAdmission::Accepted, "prior line acknowledgement admitted");
        move("after-undo-Duck-e7e5", 12U, 28U);
        input("after-undo-Duck-h5h6", [&] {
            hall &= ~(1ULL << 31U); session.ObserveHallMask(hall);
            hall |= 1ULL << 23U; session.ObserveHallMask(hall); session.Advance(2001U);
            source.Doc()["BRIGHTNESS"] = 8U; source.MarkDirty();
        });
        drain("after-undo-Duck-second-turn-save");
        if (overlapArchive) {
            bool succeeded = true;
            modules::current_game_log::CommitPointer completion;
            Need(service.TakeArchiveCompletion(succeeded, completion) && !succeeded &&
                     !service.TakeArchiveCompletion(succeeded, completion),
                 "stale archive acknowledgement fails exactly once without vetoing the newer turn");
        }
        fxe::SettingsStore finalDisk(storageConfig);
        Need(finalDisk.Load() && finalDisk.ReadValue("BRIGHTNESS", 0U) == 8U,
             "second Duck turn persists the latest unrelated preference");
        const bool followupOk = !source.IsDisabled() &&
            worker.GetFailureCount()==(refuseConsumer ? 1U : 0U) && service.CommittedPointer().currentPly==2U;
        std::printf("%s OpeningTwoUndo followup selecting Duck and playing two compound turns\n",followupOk?"PASS":"FAIL");
        return followupOk?0:1;
    }
    return ok?0:1;
}

// Deterministically advance between the actual stdio wrapper's start/end
// clock reads; no fake save implementation or wall-clock delay is involved.
static uint32_t witnessClockStep = 0U;
static uint32_t OperationWitnessClock() {
    modelNow += witnessClockStep;
    return static_cast<uint32_t>(modelNow);
}
static void RunWorkerOperationWitness(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    StaticSemaphore_t heldMutex;
    auto semaphore = xSemaphoreCreateMutexStatic(&heldMutex);
    std::promise<void> held, release;
    auto heldSignal = held.get_future();
    auto releaseSignal = release.get_future();
    std::thread holder([&] {
        Need(xSemaphoreTake(semaphore, portMAX_DELAY) == pdTRUE, "contention fixture owns mutex");
        held.set_value();
        releaseSignal.wait();
        xSemaphoreGive(semaphore);
    });
    heldSignal.wait();
    Need(xSemaphoreTake(semaphore, 0U) != pdTRUE, "zero-timeout native mutex probe returns immediately when busy");
    release.set_value();
    holder.join();
    const std::string path = (root / "s").string();
    fxe::SettingsStoreConfig config; config.path = path.c_str();
    fxe::SettingsStore source(config);
    source.Doc()["BRIGHTNESS"] = 3U;
    hardware::RmtFlashArbiter arbiter;
    firmware::BatchSettingsPersistenceWorker worker(config);
    Need((worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "operation witness worker begins");
    fxe::ScopedMillisProviderOverride clock(OperationWitnessClock);
    modelNow = 0U;
    advance = [&](const unsigned ms) {
        modelNow += ms;
        if (arbiter.TryBeginRender()) arbiter.EndRender();
    };
    const auto save = [&] {
        Need(worker.Enqueue(source.Doc()), "witness enqueues actual native settings save");
        try { task(taskContext); } catch (const Stop&) {}
        return worker.GetDiagnostics();
    };
    witnessClockStep = 2000U;
    const auto first = save();
    Need(first.settingsOperationsAvailable && first.settingsWorstOperation.durationMs == 2000U &&
         first.settingsOperationWorkerGeneration == first.completed,
         "coherent worker pair identifies slow prior save generation");
    Need(first.settingsTiming.available && first.settingsTiming.succeeded &&
             first.settingsTiming.generation == first.completed &&
             first.settingsTiming.sequence != 0U && first.settingsTiming.fileCalls != 0U &&
             first.settingsTiming.fileMs >= first.settingsWorstOperation.durationMs &&
             first.settingsTiming.flash.work.outerWaitMs == 0U &&
             first.settingsTiming.fileMs + first.settingsTiming.flash.work.outerWaitMs <=
                 first.settingsTiming.elapsedMs,
         "completed request records file duration without outer LED admission waits");
    struct PublicationProbe {
        firmware::BatchSettingsPersistenceWorker& worker;
        firmware::BatchSettingsPersistenceWorker::PersistenceTiming expected;
        unsigned calls = 0U;
    } publication{worker, first.settingsTiming};
    worker.SetSettingsStorageAccessTestHook([](void* context) {
        auto& probe = *static_cast<PublicationProbe*>(context);
        const auto timing = probe.worker.GetDiagnostics().settingsTiming;
        Need(timing.available && timing.sequence == probe.expected.sequence &&
                 timing.generation == probe.expected.generation &&
                 timing.succeeded == probe.expected.succeeded &&
                 timing.elapsedMs == probe.expected.elapsedMs &&
                 timing.fileCalls == probe.expected.fileCalls &&
                 timing.fileMs == probe.expected.fileMs,
             "real in-flight filesystem callbacks retain the prior completed timing");
        ++probe.calls;
    }, &publication);
    Need(worker.Enqueue(source.Doc()), "next ordinary request enqueues");
    const auto pending = worker.GetDiagnostics();
    Need(pending.settingsOperationWorkerGeneration == first.completed &&
         pending.settingsWorstOperation.durationMs == 2000U &&
         pending.settingsTiming.sequence == first.settingsTiming.sequence &&
         pending.settingsTiming.fileMs == first.settingsTiming.fileMs,
         "pending request preserves previous completed operation pair");
    witnessClockStep = 366U;
    try { task(taskContext); } catch (const Stop&) {}
    const auto second = worker.GetDiagnostics();
    Need(second.settingsOperationsAvailable && second.settingsOperationWorkerGeneration == second.completed &&
         second.completed != first.completed && second.settingsWorstOperation.durationMs == 366U &&
         second.settingsLastOperation.saveGeneration == second.settingsWorstOperation.saveGeneration &&
         second.settingsLastOperation.saveGeneration != first.settingsLastOperation.saveGeneration,
         "new worker generation resets worst instead of retaining older 2 second operation");
    Need(second.settingsLastOperation.kind == fxe::SettingsSaveOperationKind::Rename &&
         second.settingsLastOperation.path == fxe::SettingsSavePathRole::Pending &&
         second.settingsLastOperation.destination == fxe::SettingsSavePathRole::Primary &&
         second.settingsLastOperation.succeeded && source.GetSaveCount() == 2U,
         "cached last operation reflects successful actual save promotion");
    Need(publication.calls != 0U && second.settingsTiming.succeeded &&
             second.settingsTiming.sequence == first.settingsTiming.sequence + 1U &&
             second.settingsTiming.generation == second.completed &&
             second.settingsTiming.fileMs < first.settingsTiming.fileMs,
         "only terminal completion replaces the prior aggregate with this request");
    publication.expected = second.settingsTiming;
    std::printf("WORKER_OPERATION_WITNESS prior_worker=%u prior_ms=%u worker=%u save=%u last=%s worst_ms=%u\n",
        first.completed, first.settingsWorstOperation.durationMs, second.completed,
        second.settingsLastOperation.saveGeneration,
        fxe::SettingsSaveOperationKindToString(second.settingsLastOperation.kind),
        second.settingsWorstOperation.durationMs);
    // A real failed fopen is followed by the existing worker retry. Its
    // earlier slow operation must survive faster accesses in the same request.
    std::filesystem::create_directories(path + ".tmp");
    std::ofstream(path + ".tmp/block") << "preserve until controlled retry";
    std::filesystem::create_directories(path + ".retry");
    bool retryReleased = false;
    advance = [&](const unsigned ms) {
        modelNow += ms;
        if (ms >= 250U) {
            std::filesystem::remove_all(path + ".tmp");
            std::filesystem::remove_all(path + ".retry");
            witnessClockStep = 90U;
            retryReleased = true;
        }
        if (arbiter.TryBeginRender()) arbiter.EndRender();
    };
    witnessClockStep = 800U;
    const auto retried = save();
    Need(retryReleased && source.GetSaveCount() == 3U && worker.GetFailureCount() == 1U &&
         retried.settingsWorstOperation.durationMs == 800U &&
         retried.settingsLastOperation.durationMs == 90U &&
         retried.settingsWorstOperation.saveGeneration != retried.settingsLastOperation.saveGeneration &&
         retried.settingsOperationWorkerGeneration == retried.completed,
         "same worker request retains slow failed-attempt witness across faster successful retry");
    Need(retried.settingsTiming.available && retried.settingsTiming.succeeded &&
             retried.settingsTiming.sequence == second.settingsTiming.sequence + 1U &&
             retried.settingsTiming.generation == retried.completed &&
             retried.settingsTiming.fileCalls > second.settingsTiming.fileCalls &&
             retried.settingsTiming.elapsedMs > retried.settingsTiming.fileMs &&
             retried.settingsTiming.pagingCalls == 0U &&
             retried.settingsTiming.flash.paging.outerCalls == 0U,
         "retry aggregate includes failed attempt and wait without inventing artwork paging");
    std::printf("WORKER_OPERATION_RETRY worker=%u slow_save=%u last_save=%u worst_ms=%u last_ms=%u failures=%u\n",
        retried.completed, retried.settingsWorstOperation.saveGeneration,
        retried.settingsLastOperation.saveGeneration, retried.settingsWorstOperation.durationMs,
        retried.settingsLastOperation.durationMs, worker.GetFailureCount());
    publication.expected = retried.settingsTiming;
    std::filesystem::create_directories(path + ".tmp");
    std::ofstream(path + ".tmp/block") << "preserve through exhausted retries";
    std::filesystem::create_directories(path + ".retry");
    advance = [&](const unsigned ms) {
        modelNow += ms;
        if (arbiter.TryBeginRender()) arbiter.EndRender();
    };
    witnessClockStep = 50U;
    const auto failed = save();
    Need(failed.settingsTiming.available && !failed.settingsTiming.succeeded &&
             failed.settingsTiming.sequence == retried.settingsTiming.sequence + 1U &&
             failed.settingsTiming.generation == failed.completed &&
             failed.settingsTiming.fileCalls != 0U && worker.GetFailureCount() == 4U &&
             source.GetSaveCount() == 3U && !source.HasTerminalSaveFailure(),
         "exhausted unconfirmed path refusals fail one request without disabling the writer");
    publication.expected = failed.settingsTiming;
    Need(worker.Enqueue(source.Doc()), "successor request is accepted after failed timing");
    Need(worker.GetDiagnostics().settingsTiming.sequence == failed.settingsTiming.sequence &&
             !worker.GetDiagnostics().settingsTiming.succeeded,
         "pending successor retains the failed result until it actually completes");
    std::filesystem::remove_all(path + ".tmp");
    std::filesystem::remove_all(path + ".retry");
    witnessClockStep = 20U;
    try { task(taskContext); } catch (const Stop&) {}
    const auto recovered = worker.GetDiagnostics();
    Need(recovered.settingsTiming.succeeded &&
             recovered.settingsTiming.sequence == failed.settingsTiming.sequence + 1U &&
             recovered.settingsTiming.generation == recovered.completed &&
             source.GetSaveCount() == 4U && !source.HasTerminalSaveFailure(),
         "successful successor replaces failed timing and preserves real recovery semantics");
    std::printf("WORKER_TRANSACTION_TIMING first=%u retry=%u failed=%u recovered=%u callbacks=%u\n",
        first.settingsTiming.sequence, retried.settingsTiming.sequence,
        failed.settingsTiming.sequence, recovered.settingsTiming.sequence, publication.calls);
    worker.SetSettingsStorageAccessTestHook(nullptr, nullptr);
    witnessClockStep = 0U;
    advance = nullptr;
}

static void RunOwnerSettingsResetBootRetirement(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    batch_sdk::root = root; batch_sdk::access = nullptr; notified = 0U;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    modelNow = 0U; fxe::ManualMillis::SetNowMs(0U);
    hardware::RmtFlashArbiter arbiter;
    advance = [&](unsigned n) {
        modelNow += n; fxe::ManualMillis::SetNowMs(modelNow);
        if (arbiter.TryBeginRender()) arbiter.EndRender();
    };
    const std::string path = (root / "settings.json").string();
    fxe::SettingsStoreConfig config; config.path = path.c_str();
    config.allowSynchronousDeferredSaves = false;
    config.migrateFunc = [](JsonDocument& document) {
        const bool first = modules::first_player_migration::CanonicalizeAndCleanup(document);
        return modules::settings_registry::CanonicalizeLoadedSettings(document) || first;
    };
    config.scrubFunc = [](JsonDocument& document,fxe::SettingsRemovedKeys* removed) {
        const bool artwork = modules::ScrubLitchcraftRecord(document,removed);
        return modules::ScrubGamePersistenceSettings(document,removed) || artwork;
    };
    fxe::SettingsStore source(config);
    std::ifstream input(FixturePath());
    std::string seed((std::istreambuf_iterator<char>(input)), {});
    Need(!deserializeJson(source.Doc(),seed), "reset hook uses real saved107-ply game");
    modules::BoardIdentityPersistence identity;
    auto entropy = +[](void*,uint8_t* bytes,size_t count) { std::fill_n(bytes,count,uint8_t{0x42U}); return true; };
    Need(identity.Prepare(source.Doc(),entropy,nullptr), "reset seed preserves real board identity");
    source.Doc()["BRIGHTNESS"] = 8U;
    firmware::BatchSettingsPersistenceWorker worker(config);
    firmware::CurrentGameLogStorage storage(worker);
    firmware::BatchCurrentGameLogService service(storage.Ops());
    Need(service.Configure(worker,source.Doc()) && (worker.SetStorageAccessTestHook(&ObserveStorage, nullptr), worker.Begin(source)), "reset proof binds actual worker transaction hooks");
    const auto save = [&](auto& owner,const JsonDocument& document) {
        firmware::BatchSettingsPersistenceWorker::SaveReceipt receipt = 0U;
        Need(owner.EnqueueTracked(document,receipt), "reset proof tracked snapshot accepted");
        Need(arbiter.TryBeginRender(), "reset proof fresh frame admitted"); arbiter.EndRender();
        try { task(taskContext); } catch(const Stop&) {}
        Need(owner.GetSaveOutcome(receipt) == firmware::BatchSettingsPersistenceWorker::SaveOutcome::Saved,
             "actual PrepareBuffer/current-line hook/verified SettingsStore transaction succeeds");
        Need(owner.ReleaseSaveReceipt(receipt), "reset proof releases exact terminal receipt");
    };
    save(worker,source.Doc());
    Need(service.CommittedPointer().present && service.CommittedPointer().currentPly == 107U,
         "actual current-line authority exists before reset");
    StaticJsonDocument<1024U> safe;
    Need(modules::BuildBoardStorageRecoverySnapshot(source.Doc(),safe), "same reset snapshot builder retains safety and identity");
    save(worker,safe); save(worker,safe);
    fxe::SettingsStore reset(config);
    Need(reset.Load() && !reset.Doc().containsKey(modules::KEY_GAME_FEN) &&
             !reset.Doc().containsKey("BRIGHTNESS"), "real hook cannot restore reset game/preferences");
    modules::current_game_log::CommitPointer resetPointer;
    Need(modules::current_game_log::ReadCommitPointer(reset.Doc(),resetPointer), "reset pointer is readable");
    const uint32_t resetTotal = reset.GetPersistedSaveTotal();
    auto backupConfig = config; const auto backupPath = path+".bak"; backupConfig.path = backupPath.c_str();
    fxe::SettingsStore backup(backupConfig);
    Need(backup.Load() && !backup.Doc().containsKey(modules::KEY_GAME_FEN) &&
             !backup.Doc().containsKey("BRIGHTNESS") &&
             backup.GetPersistedSaveTotal()+1U == resetTotal,
         "second actual transaction rotated verified reset into backup rather than returning a no-op receipt");
    fxe::SettingsStore bootSource(config);
    Need(bootSource.Load(), "boot loads actual reset document");
    modules::BoardIdentityPersistence bootIdentity;
    uint8_t nextIdentityByte = 0x43U;
    Need(bootIdentity.Prepare(bootSource.Doc(),+[](void* context,uint8_t* bytes,size_t count) {
        auto& value = *static_cast<uint8_t*>(context);
        std::fill_n(bytes,count,value++); return true;
    },&nextIdentityByte) && !bootIdentity.IsDurable(),
         "boot prepares identity and its real game-identity source before game initialization");
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator game(bootSource,engine,stateMachine);
    game.SetGameHistoryIdentityGenerator(+[](void* context,modules::game_history::GameId& id) {
        return static_cast<modules::BoardIdentityPersistence*>(context)->GenerateGameIdentity(id);
    },&bootIdentity);
    auto degradation = modules::PersistenceDegradation::None;
    Need(modules::LoadGameForPersistenceSession(bootSource,game,degradation) &&
             bootSource.Doc().containsKey(modules::KEY_GAME_FEN) &&
             bootSource.Doc().containsKey(modules::KEY_GAME_HISTORY_V1),
         "production boot game initialization prepares fresh game before configuring current-line worker");
    firmware::BatchSettingsPersistenceWorker bootWorker(config);
    firmware::CurrentGameLogStorage bootStorage(bootWorker);
    firmware::BatchCurrentGameLogService bootService(bootStorage.Ops());
    struct Observer { modules::BoardIdentityPersistence* identity; firmware::BatchCurrentGameLogService* service; bool retired = false; } observer{&bootIdentity,&bootService};
    Need(bootWorker.SetDurableSnapshotCallback(+[](void* context,const JsonDocument& saved) {
        auto& observed = *static_cast<Observer*>(context);
        modules::current_game_log::CommitPointer pointer;
        observed.retired = modules::current_game_log::ReadCommitPointer(saved,pointer) && !pointer.present &&
            !observed.service->CommittedPointer().present;
        Need(observed.retired,"existing PrepareReplacement/Complete retires pointer before identity promotion");
        observed.identity->OnDurableSettingsSnapshot(saved);
    },&observer), "actual durable callback observes completed current-line transaction");
    Need(bootService.Configure(bootWorker,bootSource.Doc()) && (bootWorker.SetStorageAccessTestHook(&ObserveStorage, nullptr), bootWorker.Begin(bootSource)),
         "real boot ordering configures persistence only after game initialization");
    save(bootWorker,bootSource.Doc());
    Need(observer.retired && bootIdentity.IsDurable(), "owner identity becomes durable only after existing retirement completes");
    fxe::SettingsStore reopened(config);
    modules::current_game_log::CommitPointer finalPointer;
    Need(reopened.Load() && modules::current_game_log::ReadCommitPointer(reopened.Doc(),finalPointer) && !finalPointer.present,
         "fresh disk load proves retired current-line authority after first boot save");
    std::printf("[OWNER_RESET_REAL_HOOK] reset_pointer_retained=%u reset_saves=%u safe_backup=1 boot_retired_before_identity=1\n",
                resetPointer.present,resetTotal);
    advance = nullptr; batch_sdk::access = nullptr;
}

int main() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::path("build") / ("fullsave-" + unique);
    std::filesystem::create_directories(root);
    if (std::getenv("LITCH_OWNER_SETTINGS_RESET_BOOT") != nullptr) {
        RunOwnerSettingsResetBootRetirement(root / "owner-reset");
        std::filesystem::remove_all(root); return 0;
    }
    if (std::getenv("LITCH_SAVE_OPERATION_WITNESS") != nullptr) {
        RunWorkerOperationWitness(root / "operation-witness");
        std::filesystem::remove_all(root);
        return 0;
    }
    RunWorkerOperationWitness(root / "operation-witness");
    if (const char* capture = std::getenv("LITCH_CURRENT_GAME_CAPTURE")) {
        return RunCapturedDuckSaveCase(capture, root / "captured-duck");
    }
    if (const char* journey = std::getenv("LITCH_OPENING_JOURNEY")) {
        const char* follow = std::getenv("LITCH_OPENING_FOLLOWUP_DUCK") ? "duck" :
            (std::getenv("LITCH_OPENING_FOLLOWUP_CLASSIC") ? "classic" : nullptr);
        const int result = RunOpeningUndoAndDuckJourney(root / "opening", std::string(journey)=="duck", follow,
            std::getenv("LITCH_OPENING_FAIL_FINAL_SAVE") != nullptr);
        std::filesystem::remove_all(root);
        return result;
    }
    RunOwnerSettingsResetBootRetirement(root / "owner-reset");
    int failed = 0;
    for (const bool restore : {false, true}) {
        const auto path = root / (restore ? "restore.json" : "mate.json");
        std::filesystem::copy_file(FixturePath(), path);
        failed += RunJourney(path.c_str(), 60U, restore);
    }
    for (const unsigned cadence : {20U, 40U}) {
        for (const unsigned cost : {0U, 1U}) {
            const auto path = root / (std::to_string(cadence) + std::to_string(cost));
            const auto cadenceArg = std::to_string(cadence), costArg = std::to_string(cost);
            std::string argv0 = "full", argv1 = path.string(), argv2 = cadenceArg, argv3 = costArg;
            char *argv[] = {argv0.data(), argv1.data(), argv2.data(), argv3.data()};
            failed += RunFullSaveCase(4, argv);
        }
    }
    failed += RunOpeningUndoAndDuckJourney(root / "opening-classic", false, "classic");
    failed += RunOpeningUndoAndDuckJourney(root / "opening-duck", false, "duck");
    failed += RunOpeningUndoAndDuckJourney(root / "opening-failed-open", false, nullptr, true);
    failed += RunOpeningUndoAndDuckJourney(root / "duck-setup", true);
    std::filesystem::remove_all(root);
    return failed ? 1 : 0;
}
