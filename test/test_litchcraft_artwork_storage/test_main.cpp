#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <esp_partition.h>

#include "firmware/litchcraft_artwork_storage.hpp"
#include "firmware/litchcraft_frame_file.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/millis.hpp"
#include "freertos/task.h"
#include "current_line_memory_storage.hpp"
#include "modules/current_game_log_persistence.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/initial_fen.hpp"
#include "modules/settings_registry.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "firmware/settings_physical_access.hpp"
#include "logger.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_persistence.hpp"
#include "modules/litchcraft_presentation.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/litchcraft_wire_codec.hpp"
#include "virtual_litch_session.hpp"
#include "bounded_hint_engine.hpp"
#include "../test_rickroll_virtual_litch_journey/bouncing_balls_rgb_fixture.hpp"
#include "../test_rickroll_virtual_litch_journey/rickroll_rgb_fixture.hpp"

extern "C" void LitchcraftSetComposeSquareCheckpointForTest(
    void (*checkpoint)(uint8_t));

namespace h3_compose_probe {
std::mutex mutex;
std::condition_variable changed;
bool armed = false;
bool held = false;
bool released = false;
}
extern "C" void LitchcraftH3AfterSquare(const uint8_t square) {
    if (square != 31U) return;
    std::unique_lock<std::mutex> lock(h3_compose_probe::mutex);
    if (!h3_compose_probe::armed) return;
    h3_compose_probe::held = true;
    h3_compose_probe::changed.notify_all();
    if (!h3_compose_probe::changed.wait_for(lock, std::chrono::seconds(2), [] {
            return h3_compose_probe::released;
        })) {
        std::puts("[H3_PROBE_TIMEOUT] Compose checkpoint was not released");
        std::abort();
    }
    h3_compose_probe::armed = false;
}

namespace {
int failures = 0;
void Check(const bool ok, const char* const message) {
    if (!ok) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}

void ProductionFileWritePageMatchesStoreBatch() {
    Check(firmware::LitchcraftFrameFile::ACCESS_BYTES ==
              modules::litchcraft_frame_store::StoreReceiver::WRITE_BATCH_BYTES,
          "production filesystem page matches the staged Store body batch");
}

struct MemoryStorage {
    using Artifact = firmware::LitchcraftArtworkStorage::Artifact;
    using Status = firmware::LitchcraftArtworkStorage::Status;
    std::array<std::vector<uint8_t>, 3U> files{};
    enum class FaultPoint : uint8_t {
        None,
        AfterRemoveCurrent,
        AfterRemoveLastGood,
        AfterMoveCurrentToLastGood,
        AfterPromotePending,
    };
    FaultPoint faultPoint = FaultPoint::None;
    bool failPendingPromotion = false;
    bool corruptPendingWrite = false;
    bool corruptCurrentRead = false;
    bool failCurrentRead = false;
    unsigned replaceCalls = 0U;
    size_t maximumReplaceBytes = 0U;
    size_t maximumReadBytes = 0U;
    const uint8_t* lastReplaceSource = nullptr;

    static size_t Index(const Artifact artifact) {
        return static_cast<size_t>(artifact);
    }
    static Status Stat(void* context, const Artifact artifact, size_t& size) {
        const auto& file = static_cast<MemoryStorage*>(context)->files[Index(artifact)];
        size = file.size();
        return file.empty() ? Status::Missing : Status::Present;
    }
    static bool Read(void* context, const Artifact artifact, const size_t offset,
                     uint8_t* const out, const size_t size) {
        auto& self = *static_cast<MemoryStorage*>(context);
        if (self.failCurrentRead && artifact == Artifact::Current) return false;
        self.maximumReadBytes = std::max(self.maximumReadBytes, size);
        const auto& file = self.files[Index(artifact)];
        if ((out == nullptr && size != 0U) || offset > file.size() ||
            size > file.size() - offset) return false;
        if (size != 0U) std::memcpy(out, file.data() + offset, size);
        if (size != 0U && self.corruptCurrentRead && artifact == Artifact::Current) {
            out[0] ^= 1U;
        }
        return true;
    }
    static bool Replace(void* context, const Artifact artifact,
                        const uint8_t* const bytes, const size_t size) {
        auto& self = *static_cast<MemoryStorage*>(context);
        ++self.replaceCalls;
        self.maximumReplaceBytes = std::max(self.maximumReplaceBytes, size);
        self.lastReplaceSource = bytes;
        auto& file = self.files[Index(artifact)];
        file.assign(bytes, bytes + size);
        if (self.corruptPendingWrite && artifact == Artifact::Pending) {
            file.back() ^= 1U;
        }
        return true;
    }
    static bool ReplaceChunk(void* context, const Artifact artifact,
                             const size_t offset, const uint8_t* const bytes,
                             const size_t size, const size_t totalSize) {
        auto& self = *static_cast<MemoryStorage*>(context);
        if (offset == 0U) {
            ++self.replaceCalls;
            self.lastReplaceSource = bytes;
        }
        self.maximumReplaceBytes = std::max(self.maximumReplaceBytes, size);
        auto& file = self.files[Index(artifact)];
        if (offset == 0U) file.clear();
        if (offset != file.size() || offset > totalSize ||
            size > totalSize - offset) return false;
        file.insert(file.end(), bytes, bytes + size);
        if (self.corruptPendingWrite && artifact == Artifact::Pending) {
            file.back() ^= 1U;
            return false;
        }
        return true;
    }
    static bool Remove(void* context, const Artifact artifact) {
        auto& self = *static_cast<MemoryStorage*>(context);
        self.files[Index(artifact)].clear();
        return !((artifact == Artifact::Current &&
                   self.faultPoint == FaultPoint::AfterRemoveCurrent) ||
                  (artifact == Artifact::LastGood &&
                   self.faultPoint == FaultPoint::AfterRemoveLastGood));
    }
    static bool Rename(void* context, const Artifact from, const Artifact to) {
        auto& self = *static_cast<MemoryStorage*>(context);
        if (self.failPendingPromotion && from == Artifact::Pending &&
            to == Artifact::Current) return false;
        auto& source = self.files[Index(from)];
        auto& target = self.files[Index(to)];
        if (source.empty() || !target.empty()) return false;
        target = std::move(source);
        source.clear();
        return !((from == Artifact::Current && to == Artifact::LastGood &&
                   self.faultPoint == FaultPoint::AfterMoveCurrentToLastGood) ||
                  (from == Artifact::Pending && to == Artifact::Current &&
                   self.faultPoint == FaultPoint::AfterPromotePending));
    }
    firmware::LitchcraftArtworkStorage::Ops Ops() {
        return {this, &Stat, &Read, &Replace, &Remove, &Rename};
    }
    firmware::LitchcraftArtworkStorage::Ops ChunkedOps() {
        return {this, &Stat, &Read, &Replace, &Remove, &Rename,
                &ReplaceChunk};
    }
};

void RunTransactionStateMachine() {
    using Storage = firmware::LitchcraftArtworkStorage;
    MemoryStorage memory;
    Storage storage(memory.Ops());
    const std::array<uint8_t, 4U> a{{1U, 2U, 3U, 4U}};
    const std::array<uint8_t, 4U> b{{5U, 6U, 7U, 8U}};
    const std::array<uint8_t, 4U> c{{9U, 10U, 11U, 12U}};
    const std::array<uint8_t, 4U> d{{13U, 14U, 15U, 16U}};
    Check(storage.SetCurrentKnowledge(Storage::CurrentKnowledge::InvalidOrMissing) &&
              storage.Save(a.data(), a.size()) &&
              memory.files[0U] == std::vector<uint8_t>(a.begin(), a.end()) &&
              memory.files[1U].empty() && memory.files[2U].empty(),
          "first promotion installs only Current");
    Check(storage.Save(b.data(), b.size()) &&
              memory.files[0U] == std::vector<uint8_t>(b.begin(), b.end()) &&
              memory.files[1U] == std::vector<uint8_t>(a.begin(), a.end()),
          "second promotion retains prior Current as LastGood");

    memory.failPendingPromotion = true;
    Check(!storage.Save(c.data(), c.size()) && memory.files[0U].empty() &&
              memory.files[1U] == std::vector<uint8_t>(b.begin(), b.end()) &&
              memory.files[2U] == std::vector<uint8_t>(c.begin(), c.end()),
          "interrupted promotion leaves Pending and recoverable LastGood");
    memory.failPendingPromotion = false;
    Check(storage.Save(d.data(), d.size()) &&
              memory.files[0U] == std::vector<uint8_t>(d.begin(), d.end()) &&
              memory.files[1U] == std::vector<uint8_t>(b.begin(), b.end()),
          "retry from missing Current preserves LastGood until new promotion");

    memory.corruptPendingWrite = true;
    Check(!storage.Save(c.data(), c.size()) &&
              memory.files[0U] == std::vector<uint8_t>(d.begin(), d.end()),
          "pending readback mismatch cannot disturb Current");
    std::array<uint8_t, 8U> sentinel{};
    sentinel.fill(0xa5U);
    memory.files[0U].resize(Storage::SLOT_CAPACITY + 1U);
    size_t reported = 0U;
    Check(storage.Read(Storage::Artifact::Current, sentinel.data(),
                       sentinel.size(), reported) == Storage::Status::Present &&
              reported == Storage::SLOT_CAPACITY + 1U && sentinel[0] == 0xa5U,
          "oversized source reports length without copying into caller storage");
}

void RunDenseVerificationCadence() {
    using Storage = firmware::LitchcraftArtworkStorage;
    MemoryStorage memory;
    Storage storage(memory.ChunkedOps());
    struct AccessProbe {
        unsigned windows = 0U;
        bool active = false;
    } probe;
    storage.SetAccessOps({&probe,
        [](void* context) {
            auto& self = *static_cast<AccessProbe*>(context);
            Check(!self.active, "storage access windows never nest");
            self.active = true;
            ++self.windows;
        },
        [](void* context) {
            auto& self = *static_cast<AccessProbe*>(context);
            Check(self.active, "each storage window releases before the next access");
            self.active = false;
        }});
    std::vector<uint8_t> bytes(
        modules::LitchcraftPersistence::ADVANCED_MAX_ENVELOPE_BYTES, 0x5aU);
    Check(storage.SetCurrentKnowledge(Storage::CurrentKnowledge::InvalidOrMissing) &&
              storage.Save(bytes.data(), bytes.size()),
          "maximum eight-frame envelope seeds the transaction");
    probe.windows = 0U;
    bytes.back() ^= 1U;
    Check(storage.Save(bytes.data(), bytes.size()),
          "maximum eight-frame envelope rotates and verifies both artifacts");
    std::printf("[ARTWORK_VERIFY_CADENCE] bytes=%zu accesses=%u max_write=%zu max_read=%zu\n",
                bytes.size(), probe.windows,
                memory.maximumReplaceBytes, memory.maximumReadBytes);
    Check(probe.windows <= 60U &&
              memory.maximumReplaceBytes <= Storage::WRITE_CHUNK_BYTES &&
              memory.maximumReadBytes <= 512U && !probe.active,
          "dense save bounds write/read windows without imposing render pacing");
}

modules::litchcraft_animation::ValidatedDocument AdvancedDocument(
    const uint32_t marker) {
    modules::litchcraft_animation::Document document;
    document.frameCount = 8U;
    document.sparkleSeed = marker;
    for (size_t index = 0U; index < document.favorites.size(); ++index) {
        document.favorites[index] = {
            static_cast<uint8_t>(index + 1U),
            static_cast<uint8_t>(index + 11U),
            static_cast<uint8_t>(index + 21U)};
    }
    for (auto& frame : document.frames) frame.durationMs = 100U;
    document.frames[0U].emitters[0U].color = {
        static_cast<uint8_t>(marker), 2U, 3U};
    modules::litchcraft_animation::ValidatedDocument adopted;
    Check(modules::litchcraft_animation::ValidatedDocument::TryAdopt(
              document, adopted),
          "fault-injection artwork fixture is valid");
    return adopted;
}

struct DirectAdvancedLane {
    using Persistence = modules::LitchcraftPersistence;
    explicit DirectAdvancedLane(MemoryStorage& source)
        : memory(source), storage(source.Ops()) {}

    MemoryStorage& memory;
    firmware::LitchcraftArtworkStorage storage;
    Persistence::SaveOutcome outcome = Persistence::SaveOutcome::Pending;
    uint32_t receipt = 0U;

    Persistence::Ops Ops() {
        return {
            this,
            [](void*, const JsonDocument&, uint32_t&) {
                return fxe::SettingsStore::AsyncSaveAdmission::Rejected;
            },
            [](void* context, const uint32_t receipt) {
                auto& self = *static_cast<DirectAdvancedLane*>(context);
                Check(receipt == self.receipt,
                      "direct lane polls the retained transaction");
                return self.outcome;
            },
            [](void* context, const uint32_t receipt) {
                auto& self = *static_cast<DirectAdvancedLane*>(context);
                Check(receipt == self.receipt,
                      "direct lane releases the retained transaction");
                self.receipt = 0U;
            },
            [](void* context, const Persistence::AdvancedSlot slot,
               uint8_t* out, const size_t capacity, size_t& size) {
                auto& self = *static_cast<DirectAdvancedLane*>(context);
                const auto artifact = slot == Persistence::AdvancedSlot::Current
                    ? firmware::LitchcraftArtworkStorage::Artifact::Current
                    : firmware::LitchcraftArtworkStorage::Artifact::LastGood;
                switch (self.storage.Read(artifact, out, capacity, size)) {
                    case firmware::LitchcraftArtworkStorage::Status::Missing:
                        return Persistence::AdvancedReadOutcome::Missing;
                    case firmware::LitchcraftArtworkStorage::Status::Present:
                        return Persistence::AdvancedReadOutcome::Present;
                    case firmware::LitchcraftArtworkStorage::Status::IoFailure:
                        return Persistence::AdvancedReadOutcome::Failed;
                }
                return Persistence::AdvancedReadOutcome::Failed;
            },
            [](void* context, const uint8_t* bytes, const size_t size,
               uint32_t& receipt) {
                auto& self = *static_cast<DirectAdvancedLane*>(context);
                if (self.receipt != 0U) {
                    return fxe::SettingsStore::AsyncSaveAdmission::Deferred;
                }
                receipt = self.receipt = 1U;
                self.outcome = self.storage.Save(bytes, size)
                    ? Persistence::SaveOutcome::Saved
                    : Persistence::SaveOutcome::Failed;
                return fxe::SettingsStore::AsyncSaveAdmission::Accepted;
            }};
    }
};

modules::LitchcraftPersistence::AdvancedRecordStatus RestoreAdvanced(
    MemoryStorage& memory, uint32_t& marker) {
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_artwork_reboot.json";
    modules::SettingsStore settings(config);
    (void)settings.Load();
    modules::LitchcraftSession session;
    session.Configure(3U, 1000U);
    DirectAdvancedLane lane(memory);
    auto activation =
        std::make_unique<modules::LitchcraftPersistence::AdvancedStorage>();
    modules::LitchcraftPersistence persistence(settings, session);
    persistence.Configure(lane.Ops());
    persistence.ConfigureAdvancedStorage(*activation);
    const auto status = persistence.RestoreAdvanced();
    marker = persistence.HasAdvancedDocument()
        ? persistence.AdvancedDocument().Value().sparkleSeed : 0U;
    return status;
}

void RunPromotionPowerLossRecovery() {
    using Persistence = modules::LitchcraftPersistence;
    using Knowledge =
        firmware::LitchcraftArtworkStorage::CurrentKnowledge;
    constexpr uint32_t OLD_MARKER = 31U;
    constexpr uint32_t NEW_MARKER = 83U;
    MemoryStorage seed;
    {
        fxe::SettingsStoreConfig config;
        config.path = "build/test_litchcraft_artwork_seed.json";
        modules::SettingsStore settings(config);
        (void)settings.Load();
        modules::LitchcraftSession session;
        session.Configure(3U, 1000U);
        DirectAdvancedLane lane(seed);
        auto activation = std::make_unique<Persistence::AdvancedStorage>();
        Persistence persistence(settings, session);
        persistence.Configure(lane.Ops());
        persistence.ConfigureAdvancedStorage(*activation);
        Check(lane.storage.SetCurrentKnowledge(Knowledge::InvalidOrMissing),
              "empty boot source permits its first transaction");
        fxe::ManualMillis::SetNowMs(0U);
        Check(persistence.SetAdvancedDocument(
                  AdvancedDocument(OLD_MARKER), true),
              "old validated artwork enters the production envelope owner");
        fxe::ManualMillis::SetNowMs(5000U);
        persistence.Update();
        persistence.Update();
        Check(!persistence.HasFailure() && !seed.files[0U].empty(),
              "old artwork fixture reaches a verified Current artifact");
    }
    const auto oldEnvelope = seed.files[0U];

    for (const auto fault : {
             MemoryStorage::FaultPoint::AfterRemoveCurrent,
             MemoryStorage::FaultPoint::AfterPromotePending}) {
        MemoryStorage memory;
        memory.files[0U] = oldEnvelope;
        memory.files[0U].back() ^= 0x80U;
        memory.files[1U] = oldEnvelope;
        DirectAdvancedLane lane(memory);
        fxe::SettingsStoreConfig config;
        config.path = "build/test_litchcraft_artwork_recovered_save.json";
        modules::SettingsStore settings(config);
        (void)settings.Load();
        modules::LitchcraftSession session;
        session.Configure(3U, 1000U);
        auto activation = std::make_unique<Persistence::AdvancedStorage>();
        Persistence persistence(settings, session);
        persistence.Configure(lane.Ops());
        persistence.ConfigureAdvancedStorage(*activation);
        Check(persistence.RestoreAdvanced() ==
                  Persistence::AdvancedRecordStatus::Recovered &&
                  persistence.AdvancedDocument().Value().sparkleSeed ==
                      OLD_MARKER &&
                  lane.storage.SetCurrentKnowledge(Knowledge::InvalidOrMissing),
              "corrupt Current recovers explicit validated LastGood knowledge");
        fxe::ManualMillis::SetNowMs(100U);
        Check(persistence.SetAdvancedDocument(
                  AdvancedDocument(NEW_MARKER), true),
              "new edit remains admissible after scoped recovery");
        memory.faultPoint = fault;
        fxe::ManualMillis::SetNowMs(5100U);
        persistence.Update();
        persistence.Update();
        uint32_t restoredMarker = 0U;
        const auto rebootStatus = RestoreAdvanced(memory, restoredMarker);
        const bool afterPromotion =
            fault == MemoryStorage::FaultPoint::AfterPromotePending;
        Check(rebootStatus ==
                  (afterPromotion
                       ? Persistence::AdvancedRecordStatus::Valid
                       : Persistence::AdvancedRecordStatus::Recovered) &&
                  restoredMarker == (afterPromotion ? NEW_MARKER : OLD_MARKER),
              "power loss at every recovered-source promotion boundary reboots to explicit old/new validated artwork");
    }

    for (const auto fault : {
             MemoryStorage::FaultPoint::AfterRemoveLastGood,
             MemoryStorage::FaultPoint::AfterMoveCurrentToLastGood,
             MemoryStorage::FaultPoint::AfterPromotePending}) {
        MemoryStorage memory;
        memory.files[0U] = oldEnvelope;
        DirectAdvancedLane lane(memory);
        Check(lane.storage.SetCurrentKnowledge(Knowledge::Validated),
              "clean restore identifies validated Current before rotation");
        memory.faultPoint = fault;
        const auto newer = AdvancedDocument(NEW_MARKER);
        auto activation = std::make_unique<Persistence::AdvancedStorage>();
        fxe::SettingsStoreConfig config;
        config.path = "build/test_litchcraft_artwork_clean_save.json";
        modules::SettingsStore settings(config);
        (void)settings.Load();
        modules::LitchcraftSession session;
        session.Configure(3U, 1000U);
        Persistence persistence(settings, session);
        persistence.Configure(lane.Ops());
        persistence.ConfigureAdvancedStorage(*activation);
        Check(persistence.RestoreAdvanced() ==
                  Persistence::AdvancedRecordStatus::Valid,
              "clean fault fixture restores validated Current");
        fxe::ManualMillis::SetNowMs(100U);
        Check(persistence.SetAdvancedDocument(newer, true),
              "clean fault fixture accepts newer artwork");
        fxe::ManualMillis::SetNowMs(5100U);
        persistence.Update();
        persistence.Update();
        uint32_t restoredMarker = 0U;
        const auto rebootStatus = RestoreAdvanced(memory, restoredMarker);
        const bool afterPromotion =
            fault == MemoryStorage::FaultPoint::AfterPromotePending;
        Check((rebootStatus == Persistence::AdvancedRecordStatus::Valid ||
               rebootStatus == Persistence::AdvancedRecordStatus::Recovered) &&
                  restoredMarker == (afterPromotion ? NEW_MARKER : OLD_MARKER),
              "power loss at every clean rotation boundary retains old/new validated artwork");
    }
}
} // namespace

namespace worker_fixture {
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
bool notified = false;
bool stopping = false;
bool paused = false;
std::atomic<bool> atWait{false};
std::atomic<uint32_t> waitEntries{0U};
// Focused refill witness: deny the existing100ms fallback so only the real
// production notification can complete the next window. Default behavior stays.
std::atomic<bool> holdTimedFallback{false};
std::atomic<uint32_t> heldFallbackWaits{0U};
struct Stop {};
template <typename Predicate>
void Wait(Predicate predicate, const char* const label = "worker fixture") {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3), predicate)) {
        std::printf("[FAIL] %s timeout\n", label);
        std::abort();
    }
}
} // namespace worker_fixture

namespace hardware {
class ArtworkTestArbiter {
  public:
    // Controlled storage-leaf hold/count seam, independent of render state.
    static void ObserveStorage(void* context, const bool begin) {
        auto& self = *static_cast<ArtworkTestArbiter*>(context);
        if (!begin) { self.EndFlash(); return; }
        std::unique_lock<std::mutex> lock(self.blockMutex);
        if (self.blockNextFlash) {
            self.blockNextFlash = false;
            self.blockedFlash = true;
            self.blockChanged.notify_all();
            self.blockChanged.wait(lock, [&] { return self.releaseBlockedFlash; });
            self.blockedFlash = false;
        }
        if (self.flashLeaseActive) ++self.overlappingFlashLeases;
        self.flashLeaseActive = true;
        ++self.acquisitions;
    }
    void BlockNextFlash() {
        std::lock_guard<std::mutex> lock(blockMutex);
        blockNextFlash = true;
        blockedFlash = false;
        releaseBlockedFlash = false;
    }
    void WaitForBlockedFlash() {
        std::unique_lock<std::mutex> lock(blockMutex);
        blockChanged.wait(lock, [this] { return blockedFlash; });
    }
    bool WaitForBlockedFlashFor(const std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(blockMutex);
        return blockChanged.wait_for(lock, bound, [this] { return blockedFlash; });
    }
    bool IsFlashBlocked() {
        std::lock_guard<std::mutex> lock(blockMutex);
        return blockedFlash;
    }
    void ReleaseBlockedFlash() {
        std::lock_guard<std::mutex> lock(blockMutex);
        releaseBlockedFlash = true;
        blockChanged.notify_all();
    }
    void EndFlash() {
        flashLeaseActive = false;
        if (restoreRootAfterFlash.exchange(false)) {
            restoredRoot.store(
                restoreFrom != nullptr && restoreTo != nullptr &&
                ::rmdir(restoreTo) == 0 &&
                std::rename(restoreFrom, restoreTo) == 0);
        }
    }
    bool flashLeaseActive = false;
    unsigned overlappingFlashLeases = 0U;
    unsigned acquisitions = 0U;
    const char* restoreFrom = nullptr;
    const char* restoreTo = nullptr;
    std::atomic<bool> restoreRootAfterFlash{false};
    std::atomic<bool> restoredRoot{false};
  private:
    std::mutex blockMutex;
    std::condition_variable blockChanged;
    bool blockNextFlash = false;
    bool blockedFlash = false;
    bool releaseBlockedFlash = false;
};
} // namespace hardware

TaskHandle_t ArtworkTaskCreatePinnedToCore(TaskFunction_t task, const char*,
                                         uint32_t, void* context, uint32_t,
                                         StackType_t*, StaticTask_t*, int) {
    worker_fixture::worker = std::thread([=] {
        try { task(context); } catch (const worker_fixture::Stop&) {}
    });
    return reinterpret_cast<TaskHandle_t>(1U);
}
void (*artworkTaskDelayHook)(void*, uint32_t) = nullptr;
void* artworkTaskDelayContext = nullptr;
void ArtworkTaskDelay(const uint32_t ms) {
    if (artworkTaskDelayHook != nullptr) {
        artworkTaskDelayHook(artworkTaskDelayContext, ms);
    }
    std::this_thread::yield();
}
void ArtworkTaskDelete(TaskHandle_t) {}
TickType_t ArtworkTickCount() { return fxe::Millis(); }
TaskHandle_t ArtworkCurrentTaskHandle() {
    return std::this_thread::get_id() == worker_fixture::worker.get_id()
        ? reinterpret_cast<TaskHandle_t>(1U) : nullptr;
}
uint32_t ArtworkNotifyTake(bool, const uint32_t waitMs) {
    std::unique_lock<std::mutex> lock(worker_fixture::mutex);
    worker_fixture::atWait.store(true);
    worker_fixture::waitEntries.fetch_add(1U);
    worker_fixture::changed.notify_all();
    const auto ready = [] {
        return (worker_fixture::notified && !worker_fixture::paused) ||
            worker_fixture::stopping;
    };
    const bool holdFallback = waitMs != UINT32_MAX && worker_fixture::holdTimedFallback.load();
    if (holdFallback) worker_fixture::heldFallbackWaits.fetch_add(1U);
    const bool signalled = waitMs == UINT32_MAX || holdFallback
        ? (worker_fixture::changed.wait(lock, ready), true)
        : worker_fixture::changed.wait_for(
              lock, std::chrono::milliseconds(waitMs), ready);
    if (worker_fixture::stopping) throw worker_fixture::Stop{};
    if (!signalled) {
        worker_fixture::atWait.store(false);
        return 0U;
    }
    worker_fixture::notified = false;
    worker_fixture::atWait.store(false);
    return 1U;
}
void ArtworkNotifyGive(TaskHandle_t) {
    std::lock_guard<std::mutex> lock(worker_fixture::mutex);
    worker_fixture::notified = true;
    worker_fixture::changed.notify_all();
}

const esp_partition_t* ArtworkFindSettingsPartition(
    const esp_partition_type_t type, const esp_partition_subtype_t subtype,
    const char* const label) {
    Check(type == ESP_PARTITION_TYPE_DATA &&
              subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS &&
              label != nullptr && std::strcmp(label, "spiffs") == 0,
          "production worker resolves the exact settings filesystem partition");
    static const esp_partition_t partition = [] {
        esp_partition_t value;
        value.type = ESP_PARTITION_TYPE_DATA;
        value.subtype = ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
        std::strcpy(value.label, "spiffs");
        return value;
    }();
    return &partition;
}

using UBaseType_t = uint32_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ARDUINO 1
#define LITCH_ESP32S3 1
#define SettingsPersistenceWorker ArtworkSettingsPersistenceWorker
#define CurrentGameLogService ArtworkCurrentGameLogService
#define CurrentGameLogStorage ArtworkCurrentGameLogStorage
#define xTaskCreateStaticPinnedToCore ArtworkTaskCreatePinnedToCore
#define vTaskDelay ArtworkTaskDelay
#define vTaskDelete ArtworkTaskDelete
#define ulTaskNotifyTake ArtworkNotifyTake
#define xTaskNotifyGive ArtworkNotifyGive
#define xTaskGetCurrentTaskHandle ArtworkCurrentTaskHandle
#define xTaskGetTickCount ArtworkTickCount
#define esp_partition_find_first ArtworkFindSettingsPartition
#if !defined(LITCH_ALL_NATIVE_AGGREGATE) && !defined(main)
// The aggregate gets this real SDK observer from the Arduino save fixture.
// Standalone worker runs retain that same owner; POSIX file work must never
// escape into an unmodeled partition operation.
extern "C" int64_t esp_timer_get_time() { return int64_t(fxe::Millis()) * 1000; }
extern "C" esp_err_t __real_esp_partition_read(const esp_partition_t*, size_t, void*, size_t) { std::abort(); }
extern "C" esp_err_t __real_esp_partition_write(const esp_partition_t*, size_t, const void*, size_t) { std::abort(); }
extern "C" esp_err_t __real_esp_partition_erase_range(const esp_partition_t*, size_t, size_t) { std::abort(); }
#include "../../src/settings_physical_access.cpp"
#endif
#include "../../src/settings_persistence_worker.cpp"
#include "../../src/settings_persistence_worker_recovery.cpp"
#include "../../src/current_game_log_storage.cpp"
#include "../../src/current_game_log_service_session.cpp"
#include "../../src/current_game_log_service.cpp"
#undef CurrentGameLogService
#undef CurrentGameLogStorage
#undef esp_partition_find_first
#undef xTaskNotifyGive
#undef xTaskGetCurrentTaskHandle
#undef xTaskGetTickCount
#undef ulTaskNotifyTake
#undef vTaskDelete
#undef vTaskDelay
#undef xTaskCreateStaticPinnedToCore
#undef SettingsPersistenceWorker
#undef LITCH_ESP32S3
#undef ARDUINO

namespace {
bool CorruptStoredFramePayload(const char* const path,
                               const uint16_t frameIndex) {
    char suffix[8]{};
    std::snprintf(suffix, sizeof(suffix), ".%02u", unsigned(frameIndex / firmware::LitchcraftFrameFile::FRAMES_PER_BODY_FILE));
    const std::string physical = std::string(path) + suffix;
    std::FILE* const file = std::fopen(physical.c_str(), "r+b");
    if (file == nullptr) return false;
    size_t offset = 0U;
    bool valid = true;
    const uint16_t localFrame = frameIndex % firmware::LitchcraftFrameFile::FRAMES_PER_BODY_FILE;
    for (uint16_t index = 0U; index <= localFrame && valid; ++index) {
        std::array<uint8_t,
                   modules::litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES>
            header{};
        valid = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
            std::fread(header.data(), 1U, header.size(), file) ==
                header.size();
        if (!valid) break;
        const uint16_t encodedBytes = fxe::ReadU16LE(header.data());
        if (index == localFrame) {
            uint8_t byte = 0U;
            const long payloadOffset = static_cast<long>(
                offset + header.size());
            valid = std::fseek(file, payloadOffset, SEEK_SET) == 0 &&
                std::fread(&byte, 1U, 1U, file) == 1U;
            byte ^= 0x80U;
            valid = valid && std::fseek(file, payloadOffset, SEEK_SET) == 0 &&
                std::fwrite(&byte, 1U, 1U, file) == 1U &&
                std::fflush(file) == 0;
            break;
        }
        offset += header.size() + encodedBytes;
    }
    valid = std::fclose(file) == 0 && valid;
    return valid;
}

modules::LitchcraftSession::Packet LegacyLitchcraftRequest(
    const modules::LitchcraftSession::Op operation,
    const uint32_t session, const uint16_t request) {
    modules::LitchcraftSession::Packet packet{};
    packet.bytes[0] = modules::LitchcraftSession::MAGIC;
    packet.bytes[1] = static_cast<uint8_t>(operation);
    for (uint8_t index = 0U; index < 4U; ++index) {
        packet.bytes[2U + index] =
            static_cast<uint8_t>(session >> (index * 8U));
    }
    packet.bytes[6] = static_cast<uint8_t>(request);
    packet.bytes[7] = static_cast<uint8_t>(request >> 8U);
    packet.size = 8U;
    return packet;
}

void RunPredecessorPlaybackWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);

    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(),
          "predecessor worker fixture starts from blank banks");
    store::FrameStore seed(frameFiles.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = store::FRAME_COUNT;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.predecessorDeltas = true;
    manifest.sparkleSeed = 0x24681357U;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 301U, token) == store::Status::Ok,
          "predecessor worker seed begins");
    store::Frame frame{};
    frame.durationMs = 50U;
    frame.emitters[0].color = {20U, 1U, 2U};
    Check(seed.WriteFrame(token, 0U, frame) == store::Status::Ok,
          "predecessor worker writes frame zero");
    for (uint16_t index = 1U; index < store::FRAME_COUNT; ++index) {
        frame.durationMs = static_cast<uint16_t>(50U + index);
        frame.emitters[index - 1U].color.r = static_cast<uint8_t>(index);
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "predecessor worker writes accumulating frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "predecessor worker seed commits");

    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_predecessor_playback_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "predecessor worker fixture loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "predecessor worker starts on the production settings task");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); },
                         "predecessor worker waits for runtime readiness");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker.NotifyLitchcraftFramePlayback();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == store::FRAME_COUNT &&
            stream.TryPresentRequested() && worker.IsIdle() &&
            worker_fixture::atWait.load();
    }, "predecessor worker boot recovery");

    Check(stream.Request(200U),
          "production worker accepts a distant predecessor request");
    const unsigned beforeDistant = arbiter.acquisitions;
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && worker.IsIdle() &&
            worker_fixture::atWait.load();
    }, "predecessor worker multi-slice distant load");
    store::Frame displayed{};
    uint16_t displayedIndex = 0U;
    const bool copiedDistant = stream.CopyDisplayed(displayed, displayedIndex);
    if (!copiedDistant || displayedIndex != 200U ||
        displayed.durationMs != 250U ||
        displayed.emitters[199U].color.r != 200U ||
        arbiter.acquisitions - beforeDistant > 420U) {
        std::printf("[PREDECESSOR_WORKER] distant copied=%u index=%u duration=%u pixel=%u acquisitions=%u\n",
                    copiedDistant ? 1U : 0U, displayedIndex,
                    displayed.durationMs,
                    displayed.emitters[199U].color.r,
                    arbiter.acquisitions - beforeDistant);
    }
    Check(copiedDistant &&
              displayedIndex == 200U && displayed.durationMs == 250U &&
              displayed.emitters[199U].color.r == 200U &&
              arbiter.acquisitions - beforeDistant <= 420U,
          "production worker preserves exact artwork across bounded slices");

    Check(stream.Request(201U) && stream.TryPresentRequested() &&
              stream.Request(202U),
          "prefetched successor presents before the next storage wake");
    const unsigned beforeNear = arbiter.acquisitions;
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && worker.IsIdle() &&
            worker_fixture::atWait.load();
    }, "predecessor worker nearest-cache resume");
    const bool copiedNear = stream.CopyDisplayed(displayed, displayedIndex);
    if (!copiedNear || displayedIndex != 202U ||
        displayed.durationMs != 252U ||
        displayed.emitters[201U].color.r != 202U ||
        arbiter.acquisitions - beforeNear > 12U) {
        std::printf("[PREDECESSOR_WORKER] near copied=%u index=%u duration=%u pixel=%u acquisitions=%u\n",
                    copiedNear ? 1U : 0U, displayedIndex,
                    displayed.durationMs,
                    displayed.emitters[201U].color.r,
                    arbiter.acquisitions - beforeNear);
    }
    Check(copiedNear &&
              displayedIndex == 202U && displayed.durationMs == 252U &&
              displayed.emitters[201U].color.r == 202U &&
              arbiter.acquisitions - beforeNear <= 12U,
          "recreated union store resumes from the nearest published predecessor");

    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(),
          "predecessor worker fixture removes frame banks");
}

void RunReverseSnapshotCreditPrefetch() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(0U);

    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "reverse prefetch starts with blank banks");
    store::FrameStore seed(frameFiles.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 4U;
    manifest.active = true;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "reverse prefetch seed begins");
    for (uint16_t frameIndex = 0U; frameIndex < manifest.frameCount;
         ++frameIndex) {
        store::Frame frame{};
        frame.durationMs = 100U;
        for (uint16_t square = 0U; square < 64U; ++square) {
            frame.emitters[square].color = {
                static_cast<uint8_t>(square + frameIndex + 1U),
                static_cast<uint8_t>(square * 3U + 1U),
                static_cast<uint8_t>(square * 5U + 1U)};
        }
        Check(seed.WriteFrame(token, frameIndex, frame) == store::Status::Ok,
              "reverse prefetch seed writes varied frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "reverse prefetch seed commits");
    Check(seed.CurrentBodyBytes() > 4U * 170U,
          "reverse prefetch fixture spans more than four receipts");

    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_reverse_prefetch_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "reverse prefetch settings load");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "reverse prefetch worker starts");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); },
                         "reverse prefetch worker waits");
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "reverse prefetch ingress begins");
    constexpr uint32_t SESSION = 0x12345678U;
    constexpr uint32_t OWNER = 91U;
    std::array<uint8_t, 17U> snapshot{};
    snapshot[0U] = 0x4dU;
    snapshot[1U] = 2U;
    snapshot[2U] = store::STORE_SNAPSHOT;
    fxe::WriteU32LE(SESSION, snapshot.data() + 3U);
    fxe::WriteU16LE(1U, snapshot.data() + 7U);
    fxe::WriteU32LE(1U, snapshot.data() + 9U);
    Check(queue.Enqueue(snapshot.data(), snapshot.size(), 1U),
          "reverse prefetch request enqueues");
    const store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "reverse prefetch first receipt ready");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    uint32_t expectedCursor = 0U;
    uint32_t verifiedCursor = 0U;
    uint8_t ready = 0U;
    for (; ready < 4U; ++ready) {
        responseSize = 0U;
        if (!worker.PeekLitchcraftFrameResponse(
                response.data(), response.size(), responseSize)) break;
        if (responseSize <= 60U || response[2U] != store::STORE_SNAPSHOT_CHUNK ||
            fxe::ReadU32LE(response.data() + 28U) != expectedCursor) break;
        expectedCursor += static_cast<uint32_t>(responseSize - 60U);
        if (ready < 3U) {
            worker.AcceptLitchcraftFrameResponse();
            verifiedCursor = expectedCursor;
        }
    }
    Check(ready == 4U && expectedCursor >= 4U * 170U,
          "one worker service fills four ordered reverse credits without a wake");
    auto resumed = snapshot;
    fxe::WriteU16LE(2U, resumed.data() + 7U);
    fxe::WriteU32LE(verifiedCursor, resumed.data() + 13U);
    Check(queue.Enqueue(resumed.data(), resumed.size(), 2U),
          "read interruption queues a new request at the verified cursor");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    worker.AcceptLitchcraftFrameResponse();
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::Wait([&] {
        responseSize = 0U;
        return worker.PeekLitchcraftFrameResponse(
                   response.data(), response.size(), responseSize) &&
            fxe::ReadU16LE(response.data() + 11U) == 2U &&
            worker_fixture::atWait.load();
    }, "reverse read resumes at the verified cursor");
    Check(response[2U] == store::STORE_SNAPSHOT_CHUNK &&
              fxe::ReadU32LE(response.data() + 28U) == verifiedCursor,
          "new read replaces the old suffix without a cursor gap");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER + 1U);
    responseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(
              response.data(), response.size(), responseSize),
          "replacement owner immediately revokes prefetched snapshot bytes");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(), "reverse prefetch fixture removes banks");
}


void NormalFirstSnapshotWindowDrainRefillsWithoutNewRequest() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = worker_fixture::stopping = worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    worker_fixture::holdTimedFallback.store(false);
    worker_fixture::heldFallbackWaits.store(0U);
    fxe::ManualMillis::SetNowMs(0U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "normal refill isolates production frame banks");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{}; manifest.frameCount = 8U; manifest.active = true;
    manifest.predecessorDeltas = true;
    store::TransactionToken token{};
    Check(seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "normal refill begins a real saved body");
    for (uint16_t frameIndex = 0U; frameIndex < manifest.frameCount; ++frameIndex) {
        store::Frame frame{}; frame.durationMs = 50U;
        for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter)
            frame.emitters[emitter].color = {static_cast<uint8_t>(emitter + frameIndex * 31U),
                static_cast<uint8_t>(emitter * 37U + frameIndex * 53U),
                static_cast<uint8_t>(emitter * 71U + frameIndex)};
        Check(seed.WriteFrame(token, frameIndex, frame) == store::Status::Ok,
              "normal refill writes every authored frame through production layout");
    }
    Check(seed.Commit(token) == store::Status::Ok, "normal refill commits source");
    std::vector<uint8_t> expected(seed.CurrentBodyBytes()); size_t encoded = 0U;
    Check(seed.ReadWireBody(1U, 0U, expected.data(), expected.size(), encoded) == store::Status::Ok &&
              encoded == expected.size() && expected.size() > 4U * 438U,
          "normal saved body necessarily outlives its first four-response window");
    files.CloseCachedHandles();
    fxe::SettingsStoreConfig config; config.path = "build/test_snapshot_normal_refill.json";
    fxe::SettingsStore settings(config); Check(settings.Load(), "normal refill settings load");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config); Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "normal refill starts actual worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == manifest.frameCount && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "normal boot recovery presents its saved first frame");
    virtual_litch_config virtualConfig{}; virtual_litch_default_config(&virtualConfig);
    virtualConfig.initial_time_ms = fxe::Millis();
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(&stream,
        [](void* context, bool playing, uint32_t session, bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, session, active);
        }, &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsActive() &&
              !board.Litchcraft().IsAnimationPlaying() && board.Litchcraft().SessionId() != 0U &&
              stream.DisplayedIndex() == 0U && stream.PresentationCount() != 0U,
          "normal VirtualLitch runtime adopts and presents paused saved authority");
    worker_fixture::Wait([&] {
        const auto io = worker.GetDiagnostics();
        return worker.IsIdle() && io.frameStoreRuntimeReady && !io.frameStoreStorageActive &&
            !io.frameStorePlaybackPending && !io.frameStoreModeSavePending &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "normal paused presentation and mode bookkeeping fully converge");
    // Only now deny fallback: boot/presentation uses its ordinary production
    // lifecycle. No private worker flag is changed to make a read wake fail.
    worker_fixture::holdTimedFallback.store(true);
    worker_fixture::heldFallbackWaits.store(0U);
    modules::BleOtaChunkQueue queue; Check(queue.Begin(), "normal refill uses actual ingress queue");
    constexpr uint32_t OWNER = 91U;
    const uint32_t SESSION = board.Litchcraft().SessionId();
    std::array<uint8_t, 17U> request{{0x4dU, 2U, store::STORE_SNAPSHOT}};
    fxe::WriteU32LE(SESSION, request.data() + 3U); fxe::WriteU16LE(1U, request.data() + 7U);
    fxe::WriteU32LE(1U, request.data() + 9U);
    Check(queue.Enqueue(request.data(), request.size(), 1U), "one normal Snapshot request enters production");
    const store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{}; size_t size = 0U;
    worker_fixture::Wait([&] { return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size) &&
        worker_fixture::atWait.load(); }, "normal first window is fully prepared");
    std::vector<uint8_t> received; unsigned windows = 0U, chunks = 0U;
    bool delivered = true;
    while (received.size() < expected.size()) {
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::paused = true;
            Check(!worker_fixture::notified,
                  "full response window waits instead of immediately self-waking/spinning");
        }
        const auto io = worker.GetDiagnostics();
        Check(io.frameStoreRuntimeReady && !io.frameStorePlaybackPending &&
                  !io.frameStoreModeSavePending && !board.Litchcraft().IsAnimationPlaying(),
              "normal paused window has no playback-pending OR to supply a refill wake");
        std::printf("[NORMAL_SNAPSHOT_PRECONDITION] window=%u runtime=%u playback_pending=%u "
                    "mode_pending=%u storage=%u playing=%u presentations=%lu\n",
                    windows + 1U, io.frameStoreRuntimeReady, io.frameStorePlaybackPending,
                    io.frameStoreModeSavePending, io.frameStoreStorageActive,
                    board.Litchcraft().IsAnimationPlaying(), static_cast<unsigned long>(stream.PresentationCount()));
        const uint32_t waitEntries = worker_fixture::waitEntries.load();
        unsigned ready = 0U;
        while (ready < 4U) {
            size = 0U;
            if (!worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size)) break;
            Check(size > 60U && response[2U] == store::STORE_SNAPSHOT_CHUNK && response[13U] == 0U &&
                      fxe::ReadU32LE(response.data() + 7U) == 1U &&
                      fxe::ReadU32LE(response.data() + 20U) == expected.size() &&
                      fxe::ReadU32LE(response.data() + 24U) == fxe::Crc32(expected.data(), expected.size()) &&
                      fxe::ReadU32LE(response.data() + 28U) == received.size(),
                  "each consumed response retains authoritative identity and contiguous cursor");
            Check(worker.IsLitchcraftBulkTransferActive(),
                  "prepared snapshot tail stays bulk until the final counted response is handed off");
            received.insert(received.end(), response.begin() + 60U, response.begin() + size);
            worker.AcceptLitchcraftFrameResponse(); ++ready; ++chunks;
        }
        ++windows;
        Check(worker_fixture::waitEntries.load() == waitEntries,
              "draining paused producer cannot create worker busy-spin");
        if (received.size() >= expected.size()) {
            Check(!worker.IsLitchcraftBulkTransferActive(),
                  "draining the complete prepared read retires bulk without waiting for another worker wake");
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            // Accept legitimately also wakes pending playback bookkeeping.
            // Completion is exact body + eventual idle, not absence of a wake.
            std::printf("[NORMAL_SNAPSHOT_COMPLETE] notified=%u playback_pending=%u\n",
                        worker_fixture::notified, worker.GetDiagnostics().frameStorePlaybackPending);
            worker_fixture::paused = false;
            worker_fixture::changed.notify_all();
            break;
        }
        bool notified = false;
        const uint32_t start = fxe::Millis();
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            notified = worker_fixture::notified;
            Check(notified, "normal first-window drain must notify refill without another request");
            fxe::ManualMillis::AdvanceMs(10U);
            worker_fixture::paused = false;
            worker_fixture::changed.notify_all();
        }
        bool next = false;
        {
            std::unique_lock<std::mutex> lock(worker_fixture::mutex);
            // Host timeout is only a deadlock guard; the100ms production
            // fallback is denied. App deadline uses the controlled10ms clock.
            next = worker_fixture::changed.wait_for(lock, std::chrono::milliseconds(250), [&] {
                size = 0U;
                return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size) &&
                    worker_fixture::atWait.load();
            });
        }
        std::printf("[NORMAL_SNAPSHOT_REFILL] window=%u cursor=%zu notified=%u next=%u "
                    "controlled_ms=%u fallback_denied=%u\n", windows, received.size(), notified, next,
                    fxe::Millis() - start, worker_fixture::heldFallbackWaits.load());
        Check(next && fxe::Millis() - start <= 10U && worker_fixture::heldFallbackWaits.load() != 0U,
              "normal refill meets controlled deadline without100ms fallback or resumed Snapshot");
        if (!next) { delivered = false; break; }
    }
    Check(delivered && received == expected && windows > 1U && chunks > 4U,
          "one initial request eventually delivers every exact canonical byte/CRC");
    if (delivered) {
        worker_fixture::Wait([&] {
            size = 0U;
            return worker.IsIdle() && !worker.GetDiagnostics().frameStorePlaybackPending &&
                !worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size) &&
                !worker_fixture::notified && worker_fixture::atWait.load();
        }, "complete read and any legitimate mode/playback bookkeeping converge");
        // Existing cancellation path must still outrank a buffered fresh read.
        fxe::WriteU16LE(2U, request.data() + 7U);
        Check(queue.Enqueue(request.data(), request.size(), 2U), "control case starts another read");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        worker_fixture::Wait([&] { size = 0U; return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size) &&
            fxe::ReadU16LE(response.data() + 11U) == 2U && worker_fixture::atWait.load(); }, "control window ready");
        { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = true; }
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER + 1U);
        Check(!worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size),
              "replacement owner immediately revokes buffered previous-read responses");
        std::array<uint8_t, 9U> cancel{{0x4dU, 2U, store::STORE_CANCEL}};
        fxe::WriteU32LE(SESSION, cancel.data() + 3U); fxe::WriteU16LE(7U, cancel.data() + 7U);
        worker.NotifyLitchcraftFramePriority(cancel.data(), cancel.size(), OWNER + 1U);
        { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = false; worker_fixture::changed.notify_all(); }
        worker_fixture::Wait([&] { size = 0U; return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), size) &&
            response[2U] == store::STORE_STATE && fxe::ReadU16LE(response.data() + 11U) == 7U &&
            worker_fixture::atWait.load(); }, "priority Cancel progresses after owner replacement");
        Check(response[13U] == static_cast<uint8_t>(store::WireStatus::Incomplete),
              "priority Cancel reports no active upload instead of stalling behind readback");
    }
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true; worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join(); worker_fixture::holdTimedFallback.store(false);
    Check(files.Remove(), "normal refill removes owned frame banks");
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::remove((std::string(config.path) + suffix).c_str());
}

void RunFrameSaveFailureRetryWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(20000U);

    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "save retry fixture starts from blank banks");
    store::FrameStore seed(frameFiles.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 4U;
    manifest.active = true;
    manifest.sparkleSeed = 7U;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "save retry fixture begins a real four-frame Store");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        store::Frame frame{};
        frame.emitters[0U].color = {
            static_cast<uint8_t>(30U + index), 1U, 2U};
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "save retry fixture writes each frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "save retry fixture commits its source artwork");

    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_frame_save_retry_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "save retry fixture loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "save retry fixture starts the production storage worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == 4U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "save retry fixture presents frame zero");
    bool painted = stream.PaintDisplayed(0U, 0U, 1U, {71U, 2U, 3U}, true);
    for (uint16_t index = 1U; index <= 2U; ++index) {
        painted = painted && stream.Request(index);
        worker.NotifyLitchcraftFramePlayback();
        worker_fixture::Wait([&] {
            return stream.TryPresentRequested() &&
                stream.DisplayedIndex() == index &&
                worker_fixture::atWait.load();
        }, "save retry fixture presents another painted frame");
        painted = painted && stream.PaintDisplayed(
            index, static_cast<uint8_t>(index), 1U,
            {static_cast<uint8_t>(71U + index), 2U, 3U}, true);
    }
    Check(painted, "three distinct frames retain acknowledged paint in RAM");

    const char* const blockedRoot =
        firmware::LitchcraftFrameFile::DEFAULT_ROOT0_PATH;
    const char* const savedRoot =
        "build/test_litchcraft_frame_save_retry_worker.root0";
    Check(::rename(blockedRoot, savedRoot) == 0 &&
              ::mkdir(blockedRoot, 0700) == 0,
          "save retry fixture blocks the stage file");
    Check(stream.Request(3U), "fourth frame is accepted while three slots are dirty");
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return worker.GetLitchcraftFrameSaveFailureCount() >= 1U &&
            worker_fixture::atWait.load();
    }, "first save failure is observed at the production worker");
    const bool blockedRootRestored = ::rmdir(blockedRoot) == 0 &&
        ::rename(savedRoot, blockedRoot) == 0;
    Check(stream.DisplayedIndex() == 2U && blockedRootRestored,
          "failed save preserves the displayed frame and releases the fault");
    fxe::ManualMillis::SetNowMs(22000U);
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && stream.DisplayedIndex() == 3U &&
            worker_fixture::atWait.load();
    }, "pending fourth selection retries after flash recovers");
    std::puts("[LITCHCRAFT_SAVE_RETRY_JOURNEY] fault=stage-write "
              "requested=3 first-display=2 retry-display=3 "
              "next=paint_or_return");

    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(), "save retry fixture removes frame banks");
}

void RunUnpublishedResponseWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "response lease fixture starts from empty files");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 3U;
    manifest.active = true;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "response lease fixture stages three dense frames");
    store::Frame frame{};
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter) {
            frame.emitters[emitter].color = {
                static_cast<uint8_t>(emitter),
                static_cast<uint8_t>(emitter * 3U),
                static_cast<uint8_t>(emitter * 7U + index)};
        }
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "response lease fixture writes exact dense frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "response lease fixture publishes its baseline");
    const uint32_t bodyBytes = seed.CurrentBodyBytes();
    const uint32_t bodyCrc = seed.CurrentBodyCrc32();
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_response_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "response lease fixture loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "response lease starts real worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); });
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] {
        return worker_fixture::atWait.load() &&
            worker.LitchcraftFrameStream().Generation() == 1U;
    }, "response lease recovers committed authority");
    constexpr uint32_t SESSION = 123U;
    constexpr uint32_t OWNER = 17U;
    const store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "response lease queue starts");
    std::array<uint8_t, 10U> inspect{0x4dU, 2U, 1U};
    fxe::WriteU32LE(SESSION, inspect.data() + 3U);
    fxe::WriteU16LE(1U, inspect.data() + 7U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    Check(queue.Enqueue(inspect.data(), inspect.size(), 1U),
          "response lease queues Inspect");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
            response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "response lease Inspect completes");
    const auto inspectResponse = response;
    const size_t inspectSize = responseSize;
    worker.AcceptLitchcraftFrameResponse();
    std::array<uint8_t, 17U> snapshot{0x4dU, 2U, store::STORE_SNAPSHOT};
    fxe::WriteU32LE(SESSION, snapshot.data() + 3U);
    fxe::WriteU16LE(2U, snapshot.data() + 7U);
    fxe::WriteU32LE(1U, snapshot.data() + 9U);
    arbiter.BlockNextFlash();
    Check(queue.Enqueue(snapshot.data(), snapshot.size(), 2U),
          "response lease queues Snapshot");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    arbiter.WaitForBlockedFlash();
    Check(worker.ReplayLitchcraftFrameResponse(1U, OWNER) &&
              worker.PeekLitchcraftFrameResponse(
                  response.data(), response.size(), responseSize) &&
              responseSize == inspectSize && response == inspectResponse,
          "Query can replay exact Inspect while worker output is unpublished");
    worker.AcceptLitchcraftFrameResponse();
    arbiter.ReleaseBlockedFlash();
    std::vector<uint8_t> downloaded;
    while (downloaded.size() < bodyBytes) {
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(
                response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "response lease drains one bounded read window");
        Check(response[2U] == store::STORE_SNAPSHOT_CHUNK &&
                  response[13U] == 0U &&
                  fxe::ReadU32LE(response.data() + 28U) == downloaded.size() &&
                  responseSize == 60U + std::min<size_t>(438U,
                      bodyBytes - downloaded.size()),
              "prefetched maximum-size responses retain exact ordered cursors");
        if (responseSize <= 60U) break;
        downloaded.insert(downloaded.end(), response.begin() + 60U,
                          response.begin() + responseSize);
        worker.AcceptLitchcraftFrameResponse();
    }
    Check(downloaded.size() == bodyBytes &&
              fxe::Crc32(downloaded.data(), downloaded.size()) == bodyCrc,
          "full credit-window drains reproduce the exact committed body");
    // A priority request arriving during read I/O retires the old visibility.
    // Snapshot-only Cancel currently returns Incomplete; it must still run,
    // rather than be hidden behind the response that was being prepared.
    fxe::WriteU16LE(3U, snapshot.data() + 7U);
    arbiter.BlockNextFlash();
    Check(queue.Enqueue(snapshot.data(), snapshot.size(), 3U),
          "response lease queues interrupted read");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    arbiter.WaitForBlockedFlash();
    std::array<uint8_t, 9U> cancel{0x4dU, 2U, store::STORE_CANCEL};
    fxe::WriteU32LE(SESSION, cancel.data() + 3U);
    fxe::WriteU16LE(3U, cancel.data() + 7U);
    worker.NotifyLitchcraftFramePriority(cancel.data(), cancel.size(), OWNER);
    arbiter.ReleaseBlockedFlash();
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
            response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "priority Cancel survives an interrupted response lease");
    Check(response[2U] == store::STORE_STATE &&
              response[13U] == static_cast<uint8_t>(store::WireStatus::Incomplete),
          "priority outcome supersedes the response prepared before Cancel");
    worker.AcceptLitchcraftFrameResponse();
    fxe::WriteU16LE(4U, snapshot.data() + 7U);
    arbiter.BlockNextFlash();
    Check(queue.Enqueue(snapshot.data(), snapshot.size(), 4U),
          "response lease queues a read from the retiring connection");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    arbiter.WaitForBlockedFlash();
    modules::BleOtaChunkQueue replacement;
    Check(replacement.Begin(), "replacement connection queue starts");
    fxe::WriteU16LE(5U, inspect.data() + 7U);
    Check(replacement.Enqueue(inspect.data(), inspect.size(), 1U),
          "replacement connection queues its own Inspect");
    worker.NotifyLitchcraftFrameIngress(replacement, projection, OWNER + 1U);
    Check(!worker.PeekLitchcraftFrameResponse(
              response.data(), response.size(), responseSize),
          "connection change keeps the retiring output unpublished");
    arbiter.ReleaseBlockedFlash();
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
            response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "replacement connection receives its own response");
    Check(response[2U] == store::STORE_STATE && response[13U] == 0U &&
              fxe::ReadU16LE(response.data() + 11U) == 5U &&
              !worker.ReplayLitchcraftFrameResponse(4U, OWNER),
          "old read bytes and Query cannot cross the owner lifetime boundary");
    worker.AcceptLitchcraftFrameResponse();
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "response lease removes fixture banks");
}

void RunFreshStoreEntryWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    const char* const root0 =
        firmware::LitchcraftFrameFile::DEFAULT_ROOT0_PATH;
    (void)::rmdir(root0);
    Check(files.Remove(), "fresh journey starts with no Store root");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_fresh_entry_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "fresh journey loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "fresh journey starts production persistence worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); },
                         "fresh journey waits for worker startup");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "fresh boot leaves Store absent until mode entry");
    Check(stream.Generation() == 0U && stream.FrameCount() == 0U,
          "boot does not publish an inactive blank root");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    worker.NotifyLitchcraftFrameSession(false, 91U, true);
    worker.NotifyLitchcraftFrameSession(false, 91U, false);
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
               worker.IsIdle() && worker_fixture::atWait.load();
    }, "Enter then Exit before worker bootstrap keeps the empty Store durable");
    Check(stream.Generation() == 0U && stream.FrameCount() == 0U,
          "cancelled first entry does not publish a root or strand mode save");
    std::puts("[FRESH_STORE_ENTRY_JOURNEY] Enter->Exit-before-bootstrap | "
              "store=empty mode=Setup barrier=clear next=physical-entry");

    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        },
        &worker);
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK &&
              !board.Litchcraft().IsActive(),
          "fresh V3 remains in Setup while no Store exists");
    Check(::mkdir(root0, 0700) == 0,
          "fresh journey makes the first Store root temporarily unwritable");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    const auto button = [&](const runtime::VirtualLitchPhysicalButton key,
                            const bool down) {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, key, down) ==
                  VIRTUAL_LITCH_OK &&
                  board.Advance(30U) == VIRTUAL_LITCH_OK,
              "fresh journey button level settles through production debounce");
    };
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "fresh journey forms human-staggered entry chord");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK &&
              !board.Litchcraft().IsActive(),
          "fresh journey waits for release after entry confirmation");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    Check(board.Litchcraft().IsActive(),
          "physical chord enters Litchcraft before Store bootstrap completes");
    modules::BleOtaChunkQueue readQueue;
    Check(readQueue.Begin(), "fresh journey starts shared Store ingress");
    constexpr uint32_t READ_OWNER = 63U;
    const uint32_t readSession = board.Litchcraft().SessionId();
    const store::StoreProjection readProjection{
        readSession, 0U, 3U, true, false};
    std::array<uint8_t, 10U> inspect{};
    inspect[0U] = 0x4dU;
    inspect[1U] = 2U;
    inspect[2U] = 1U; // Store Inspect
    fxe::WriteU32LE(readSession, inspect.data() + 3U);
    fxe::WriteU16LE(1U, inspect.data() + 7U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    Check(readQueue.Enqueue(inspect.data(), inspect.size(), 1U),
          "app Inspect queues during the pending physical Enter root");
    worker.NotifyLitchcraftFrameIngress(
        readQueue, readProjection, READ_OWNER);
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> readResponse{};
    size_t readResponseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   readResponse.data(), readResponse.size(),
                   readResponseSize) && worker_fixture::atWait.load();
    }, "failed blank-root creation returns a bounded Store response");
    Check(readResponse[2U] == store::STORE_STATE &&
              readResponse[13U] == static_cast<uint8_t>(
                  store::WireStatus::ResourceLimit),
          "flash failure reports a retriable Store resource error");
    std::puts("[FRESH_STORE_ENTRY_JOURNEY] root-fault | "
              "Inspect=ResourceLimit mode=Litchcraft next=retry");
    worker.AcceptLitchcraftFrameResponse();
    Check(::rmdir(root0) == 0,
          "fresh journey removes its temporary root-write fault");
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 2000U);
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return stream.Generation() == 1U &&
               !worker.LitchcraftFrameModeSavePending() &&
               worker_fixture::atWait.load();
    }, "fresh Store retries after transient root failure");
    fxe::WriteU16LE(2U, inspect.data() + 7U);
    Check(readQueue.Enqueue(inspect.data(), inspect.size(), 2U),
          "app retries Inspect after the visible storage error");
    worker.NotifyLitchcraftFrameIngress(
        readQueue, readProjection, READ_OWNER);
    readResponseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   readResponse.data(), readResponse.size(),
                   readResponseSize) &&
               fxe::ReadU16LE(readResponse.data() + 11U) == 2U &&
               worker_fixture::atWait.load();
    }, "retried Inspect sees the entered Store root");
    const uint32_t inspectedGeneration =
        fxe::ReadU32LE(readResponse.data() + 7U);
    Check(readResponse[2U] == store::STORE_STATE &&
              readResponse[13U] == 0U &&
              inspectedGeneration == 1U &&
              fxe::ReadU16LE(readResponse.data() + 15U) == 1U,
          "Inspect observes the committed one-frame root after Enter");
    std::puts("[FRESH_STORE_ENTRY_JOURNEY] root-retry | "
              "Inspect=frame1/generation1 next=Snapshot");
    worker.AcceptLitchcraftFrameResponse();
    std::array<uint8_t, 17U> snapshot{};
    snapshot[0U] = 0x4dU;
    snapshot[1U] = 2U;
    snapshot[2U] = store::STORE_SNAPSHOT;
    fxe::WriteU32LE(readSession, snapshot.data() + 3U);
    fxe::WriteU16LE(3U, snapshot.data() + 7U);
    fxe::WriteU32LE(inspectedGeneration, snapshot.data() + 9U);
    Check(readQueue.Enqueue(snapshot.data(), snapshot.size(), 3U),
          "app Snapshot requests the inspected Store generation");
    worker.NotifyLitchcraftFrameIngress(
        readQueue, readProjection, READ_OWNER);
    readResponseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   readResponse.data(), readResponse.size(),
                   readResponseSize) &&
               fxe::ReadU16LE(readResponse.data() + 11U) == 3U &&
               worker_fixture::atWait.load();
    }, "snapshot of the just-inspected root responds without conflict");
    Check(readResponse[2U] == store::STORE_SNAPSHOT_CHUNK &&
              readResponse[13U] == 0U &&
              fxe::ReadU32LE(readResponse.data() + 7U) ==
                  inspectedGeneration,
          "near-immediate Inspect to Snapshot reads one stable generation");
    const uint32_t snapshotBytes =
        fxe::ReadU32LE(readResponse.data() + 20U);
    uint32_t received = static_cast<uint32_t>(readResponseSize - 60U);
    worker.AcceptLitchcraftFrameResponse();
    while (received < snapshotBytes) {
        readResponseSize = 0U;
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(
                       readResponse.data(), readResponse.size(),
                       readResponseSize) &&
                   fxe::ReadU16LE(readResponse.data() + 11U) == 3U &&
                   worker_fixture::atWait.load();
        }, "fresh snapshot continues through the verified body");
        Check(readResponse[2U] == store::STORE_SNAPSHOT_CHUNK &&
                  readResponse[13U] == 0U &&
                  fxe::ReadU32LE(readResponse.data() + 28U) == received &&
                  readResponseSize > 60U,
              "snapshot suffix retains its inspected generation and cursor");
        if (readResponseSize <= 60U) break;
        received += static_cast<uint32_t>(readResponseSize - 60U);
        worker.AcceptLitchcraftFrameResponse();
    }
    worker_fixture::Wait([&] {
        return stream.Generation() == 1U && stream.FrameCount() == 1U &&
               stream.TryPresentRequested() && worker_fixture::atWait.load();
    }, "physical entry creates and pages one blank Store frame");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "bootstrap leaves physical mode active and shows a blank square");
    std::puts("[FRESH_STORE_ENTRY_JOURNEY] boot->X+O | mode=Litchcraft "
              "store=1/frame1 square9=black next=place-piece");
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[9U][0U].r != 0U &&
              stream.HasUnpersistedEdit(),
          "first physical placement paints the new Store immediately");
    stream.RequestImmediateSave();
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return !stream.HasUnpersistedEdit() && worker_fixture::atWait.load();
    }, "fresh physical paint persists after worker flush");
    store::FrameStore reboot(files.StoreOps());
    store::Frame saved{};
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.CurrentManifest().frameCount == 1U &&
              reboot.ReadFrame(0U, saved) == store::Status::Ok &&
              saved.emitters[9U * 4U].color.r != 0U,
          "reboot recovers the physically painted square from schema-4 Store");
    Check(board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK,
          "fresh journey lifts piece before the exit chord");
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "fresh journey forms exit chord with human stagger");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive(),
          "fresh journey waits for exit chord release");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    Check(!board.Litchcraft().IsActive(),
          "physical Exit returns the V3 to Setup");
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
               worker_fixture::atWait.load();
    }, "fresh journey persists Exit as a root-only mode change");
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "fresh journey forms re-entry chord");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK,
          "fresh journey completes re-entry confirmation");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
               worker_fixture::atWait.load();
    }, "re-entry persists active mode without blocking render");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() &&
              board.SemanticFrame().board[9U][0U].r != 0U,
          "re-entry keeps the physically painted Store frame visible");
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "fresh journey forms final Exit chord");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK,
          "fresh journey completes final Exit confirmation");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
               worker_fixture::atWait.load();
    }, "final Exit persists inactive mode before reboot");
    store::FrameStore exited(files.StoreOps());
    Check(exited.Recover() == store::Status::Ok &&
              !exited.CurrentManifest().active &&
              exited.CurrentManifest().frameCount == 1U,
          "reboot after Exit retains the artwork but restores Setup mode");
    std::puts("[FRESH_STORE_ENTRY_JOURNEY] place->save->exit->reboot | "
              "paint=durable mode=Setup next=app-sync");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "fresh journey removes its Store fixture");
}



std::vector<uint8_t> ResidentFileBytes(const char* path);

void RunEmptyOnlyRetirementPreservesAcceptedNew() {
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    namespace store = modules::litchcraft_frame_store;
    Stream empty;
    Check(empty.RetireDocument(Stream::RetirementScope::EmptyOnly),
          "prelaunch cleanup can retire the truly empty live authority");
    Stream live;
    store::Manifest manifest{};
    Check(live.StageFirstFrame(manifest, 1U, store::Frame{}),
          "empty-retirement countercheck stages the current document");
    Check(live.NewBlankDocument(live.Generation(), live.LiveRevision(), {}) ==
              Stream::GuardedEditResult::Changed &&
              live.PaintDisplayed(0U, 9U, 1U, {37U, 91U, 143U}, true),
          "New and a brush edit are accepted before the cleanup lock");
    const auto revision = live.LiveRevision();
    store::Frame displayed{}; uint16_t index = 0U;
    Check(!live.RetireDocument(Stream::RetirementScope::EmptyOnly) &&
              live.HasPendingBlankDocument() && live.LiveRevision() == revision &&
              live.CopyDisplayed(displayed, index) &&
              displayed.emitters[9U * 4U].color.r == 37U,
          "atomic EmptyOnly retirement refuses to discard an accepted New or drawing");
    Check(live.RetireDocument(), "explicit manual clear retains current-document semantics");
    std::puts("[DEVELOPMENT_FORMAT_RESET_RACE] New=accepted paint=accepted empty-retire=rejected drawing=preserved");
}

void RunDevelopmentFormatResetWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using File = firmware::LitchcraftFrameFile;
    namespace store = modules::litchcraft_frame_store;
    namespace layout = modules::litchcraft_frame_store_layout;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(20000U);
    File files;
    Check(files.Remove(), "development reset starts with isolated artwork files");
    {
        store::FrameStore original(files.StoreOps());
        store::Manifest manifest{};
        manifest.frameCount = 1U;
        store::TransactionToken token{};
        Check(original.Begin(manifest, 0U, 0x4f4c44U, token) == store::Status::Ok &&
                  original.WriteFrame(token, 0U, store::Frame{}) == store::Status::Ok &&
                  original.Commit(token) == store::Status::Ok,
              "development reset seeds a complete production Store");
    }
    auto ops = files.StoreOps();
    std::array<uint8_t, layout::ROOT_BYTES> root{};
    Check(ops.read(ops.context, 0U, root.data(), root.size()),
          "development reset reads its committed production root");
    root[2U] = '5'; root[4U] = 5U;
    fxe::WriteU32LE(fxe::Crc32(root.data(), root.size() - 4U),
                    root.data() + root.size() - 4U);
    Check(ops.write(ops.context, 0U, root.data(), root.size()) && ops.sync(ops.context),
          "development reset seeds the recognized CRC-valid LC5 root");
    files.CloseCachedHandles();
    const std::string segmented = std::string(File::DEFAULT_BODY0_PATH) + ".00";
    Check(::rename(segmented.c_str(), File::DEFAULT_BODY0_PATH) == 0,
          "development reset uses the old monolithic body path");
    std::FILE* orphan = std::fopen(File::DEFAULT_BODY1_PATH, "wb");
    Check(orphan != nullptr && std::fputc(0x5a, orphan) != EOF &&
              std::fclose(orphan) == 0,
          "development reset seeds an obsolete inactive body");
    const auto rootBefore = ResidentFileBytes(File::DEFAULT_ROOT0_PATH);
    const auto bodyBefore = ResidentFileBytes(File::DEFAULT_BODY0_PATH);
    const auto orphanBefore = ResidentFileBytes(File::DEFAULT_BODY1_PATH);
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_development_reset.json";
    (void)::unlink(config.path);
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "development reset loads isolated settings");
    settings.Doc()["FEN"] = "8/8/8/8/8/8/4K3/7k w - - 0 1";
    settings.Doc()["BRIGHTNESS"] = 13U;
    settings.Doc()["BOARD_ID"] = "development-reset-preserved-board";
    settings.Doc()["PAIRING_OWNER"] = "preserved-phone";
    settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
    Check(settings.SaveNowBlocking(), "development reset seeds unrelated durable settings");
    const auto settingsBefore = ResidentFileBytes(config.path);
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "development reset starts actual persistence worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    const auto stop = [&] {
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::stopping = true;
            worker_fixture::changed.notify_all();
        }
        worker_fixture::worker.join();
        Check(files.Remove(), "development reset removes isolated artwork fixture");
        (void)::unlink(config.path);
    };
    worker_fixture::Wait([&] { return worker.IsIdle() && worker_fixture::atWait.load(); },
                         "development reset finishes on the production worker");
    store::FrameStore reboot(files.StoreOps());
    const auto recovery = reboot.Recover();
    const bool retained = recovery == store::Status::PrelaunchLC5 &&
        ResidentFileBytes(File::DEFAULT_ROOT0_PATH) == rootBefore &&
        ResidentFileBytes(File::DEFAULT_BODY0_PATH) == bodyBefore &&
        ResidentFileBytes(File::DEFAULT_BODY1_PATH) == orphanBefore;
    const bool unchanged = ResidentFileBytes(config.path) == settingsBefore;
    std::printf("[DEVELOPMENT_FORMAT_RESET] boot_recovery=%u retained=%u source_unchanged=%u\n",
                unsigned(recovery), unsigned(retained), unsigned(unchanged));
    Check(retained && unchanged &&
              worker.LitchcraftFrameClearState() == Worker::FrameClearState::Idle,
          "boot retains recognized prelaunch LC5 and every source byte without clearing");
    if (!retained) { stop(); return; }
    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session, const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, session, active);
        }, &worker);
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK && !board.Litchcraft().IsActive(),
          "reset returns to normal physical Setup");
    const auto button = [&](runtime::VirtualLitchPhysicalButton key, const bool down) {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, key, down) == VIRTUAL_LITCH_OK &&
                  board.Advance(30U) == VIRTUAL_LITCH_OK,
              "development reset enters through production button debounce");
    };
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK, "reset entry uses human-staggered X+O");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK, "reset entry completes physical hold");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    worker_fixture::Wait([&] {
        return stream.Generation() != 0U && stream.FrameCount() == 1U &&
               !worker.LitchcraftFrameModeSavePending() && stream.TryPresentRequested() &&
               worker_fixture::atWait.load();
    }, "reset enters a committed normal blank Store");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsActive() &&
              board.SemanticFrame().board[9U][0U].r == 0U &&
              !worker.GetDiagnostics().frameStoreModeSaveFailed,
          "normal mode and blank frame are ready after development reset");
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[9U][0U].r != 0U,
          "physical placement immediately paints after development reset");
    stream.RequestImmediateSave();
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] { return !stream.HasUnpersistedEdit() && worker_fixture::atWait.load(); },
                         "first physical paint persists after development reset");
    Check(ResidentFileBytes(config.path) == settingsBefore,
          "reset, mode entry and paint preserve game, brightness, pairing, identity and safety settings");
    std::puts("[DEVELOPMENT_FORMAT_RESET_JOURNEY] empty occupancy | X+O accepted | "
              "mode=Litchcraft blank=frame1 | square9 paint=visible/durable | next=draw-or-Open");
    stop();
}


void RunPrelaunchRootIoFailureWorkerCountercheck() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using File = firmware::LitchcraftFrameFile;
    namespace store = modules::litchcraft_frame_store;
    namespace layout = modules::litchcraft_frame_store_layout;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(20000U);
    File files;
    Check(files.Remove(), "root I/O countercheck starts isolated files");
    {
        store::FrameStore original(files.StoreOps());
        store::Manifest manifest{}; manifest.frameCount = 1U;
        store::TransactionToken token{};
        Check(original.Begin(manifest, 0U, 0x494fU, token) == store::Status::Ok &&
                  original.WriteFrame(token, 0U, store::Frame{}) == store::Status::Ok &&
                  original.Commit(token) == store::Status::Ok,
              "root I/O countercheck seeds a complete Store");
    }
    auto ops = files.StoreOps();
    std::array<uint8_t, layout::ROOT_BYTES> root{};
    Check(ops.read(ops.context, 0U, root.data(), root.size()),
          "root I/O countercheck reads seed root");
    root[2U] = '5'; root[4U] = 5U; root[6U] = 1U;
    fxe::WriteU32LE(fxe::Crc32(root.data(), root.size() - 4U),
                    root.data() + root.size() - 4U);
    Check(ops.write(ops.context, layout::Layout::RootOffset(1U, 0U), root.data(), root.size()) &&
              ops.sync(ops.context), "root I/O countercheck seeds recognized old alternate root");
    files.CloseCachedHandles();
    const auto rootBefore = ResidentFileBytes(File::DEFAULT_ROOT1_PATH);
    const std::string body = std::string(File::DEFAULT_BODY0_PATH) + ".00";
    const auto bodyBefore = ResidentFileBytes(body.c_str());
    Check(::unlink(File::DEFAULT_ROOT0_PATH) == 0 &&
              ::mkdir(File::DEFAULT_ROOT0_PATH, 0700) == 0,
          "root I/O countercheck forces a real filesystem read failure");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_prelaunch_io.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "root I/O countercheck loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "root I/O countercheck starts actual worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] { return worker.IsIdle() && worker_fixture::atWait.load(); },
                         "root I/O failure finishes bounded recovery attempts");
    Check(worker.GetDiagnostics().frameStoreRecovery == uint8_t(store::Status::IoFailure) &&
              worker.LitchcraftFrameClearState() == Worker::FrameClearState::Idle &&
              worker.LitchcraftFrameStream().FrameCount() == 0U &&
              ResidentFileBytes(File::DEFAULT_ROOT1_PATH) == rootBefore &&
              ResidentFileBytes(body.c_str()) == bodyBefore,
          "unreadable root plus recognizable LC5 preserves artwork and does not queue reset");
    std::puts("[DEVELOPMENT_FORMAT_RESET_IO] root-read=failed old-root=preserved body=preserved reset=not-requested");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(::rmdir(File::DEFAULT_ROOT0_PATH) == 0 && files.Remove(),
          "root I/O countercheck removes its isolated fixture");
}

void RunCorruptStoreReplacementWorkerJourney(const bool retryRoot) {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    namespace layout = modules::litchcraft_frame_store_layout;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(20000U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "corrupt journey starts with isolated Store files");
    const char* const root = firmware::LitchcraftFrameFile::DEFAULT_ROOT0_PATH;
    std::vector<uint8_t> damaged(layout::ROOT0_FILE_BYTES, 0U);
    damaged[0U] = 0xa5U;
    std::FILE* seed = std::fopen(root, "wb");
    Check(seed != nullptr, "corrupt journey opens its invalid root fixture");
    if (seed == nullptr) return;
    Check(std::fwrite(damaged.data(), 1U, damaged.size(), seed) == damaged.size() &&
              std::fclose(seed) == 0,
          "corrupt journey seeds damaged production root bytes");
    const auto rootUnchanged = [&] {
        std::FILE* current = std::fopen(root, "rb");
        if (current == nullptr) return false;
        std::vector<uint8_t> bytes(damaged.size());
        const bool same = std::fread(bytes.data(), 1U, bytes.size(), current) ==
                bytes.size() && std::fgetc(current) == EOF && bytes == damaged;
        return std::fclose(current) == 0 && same;
    };
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_corrupt_replacement_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "corrupt journey loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "corrupt journey starts production worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    const auto stop = [&] {
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::stopping = true;
            worker_fixture::changed.notify_all();
        }
        worker_fixture::worker.join();
        Check(files.Remove(), "corrupt journey removes isolated Store files");
    };
    worker_fixture::Wait([&] { return worker.IsIdle() && worker_fixture::atWait.load(); },
                         "corrupt boot finishes without replacing the invalid root");
    Check(rootUnchanged() && stream.FrameCount() == 0U,
          "corrupt boot preserves bytes and exposes no fabricated artwork");
    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        }, &worker);
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK,
          "corrupt journey reaches physical Setup");
    const auto button = [&](const runtime::VirtualLitchPhysicalButton key,
                            const bool down) {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, key, down) ==
                  VIRTUAL_LITCH_OK && board.Advance(30U) == VIRTUAL_LITCH_OK,
              "corrupt journey debounces physical entry chord");
    };
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK, "corrupt journey staggers X+O");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK,
          "corrupt journey completes physical entry hold");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreModeSaveFailed &&
               worker_fixture::atWait.load();
    }, "corrupt entry reports unavailable durable mode without overwriting data");
    Check(board.Litchcraft().IsActive() && rootUnchanged() &&
              stream.Generation() == 0U && stream.FrameCount() == 0U,
          "acknowledged Litchcraft entry preserves damaged root until explicit Open");
    std::puts("[CORRUPT_STORE_REPLACEMENT_JOURNEY] empty occupancy | X+O accepted | "
              "mode=Litchcraft artwork=absent diagnostic=Corrupt next=explicit-Open");
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "corrupt journey starts existing Store ingress");
    const uint32_t session = board.Litchcraft().SessionId();
    constexpr uint32_t OWNER = 73U;
    constexpr uint16_t TRANSFER = 9U;
    const store::StoreProjection projection{session, 0U, 3U, true, false};
    uint32_t ordinal = 0U;
    const auto enqueue = [&](const uint8_t* bytes, const size_t size) {
        Check(queue.Enqueue(bytes, size, ++ordinal), "corrupt journey queues typed Store packet");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    };
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    const auto waitResponse = [&](const uint16_t request) {
        responseSize = 0U;
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(response.data(), response.size(),
                       responseSize) && fxe::ReadU16LE(response.data() + 11U) == request &&
                   worker_fixture::atWait.load();
        }, "corrupt journey receives fenced Store response");
    };
    std::array<uint8_t, 10U> inspect{{0x4dU, 2U, 1U}};
    fxe::WriteU32LE(session, inspect.data() + 3U);
    fxe::WriteU16LE(1U, inspect.data() + 7U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    enqueue(inspect.data(), inspect.size());
    waitResponse(1U);
    Check(response[2U] == store::STORE_STATE && response[13U] == 0U &&
              fxe::ReadU32LE(response.data() + 7U) == 0U &&
              fxe::ReadU16LE(response.data() + 15U) == 0U && rootUnchanged(),
          "corrupt active Store permits observational Inspect for explicit replacement");
    std::printf("[CORRUPT_STORE_REPLACEMENT_JOURNEY] Inspect status=%u frames=%u generation=%lu\n",
                response[13U], fxe::ReadU16LE(response.data() + 15U),
                static_cast<unsigned long>(fxe::ReadU32LE(response.data() + 7U)));
    worker.AcceptLitchcraftFrameResponse();
    if (response[13U] != 0U) { stop(); return; }

    std::vector<uint8_t> body;
    store::Frame previous{};
    store::Frame frame{};
    frame.durationMs = 50U;
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        frame.emitters[9U * 4U].color = {static_cast<uint8_t>(40U + index), 2U, 3U};
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
        size_t size = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
                  index % store::KEYFRAME_INTERVAL == 0U ? nullptr : &previous,
                  frame, encoded.data(), encoded.size(), size),
              "corrupt journey encodes canonical 256-frame artwork");
        const size_t offset = body.size();
        body.resize(offset + store::WIRE_RECORD_HEADER_BYTES + size);
        fxe::WriteU16LE(static_cast<uint16_t>(size), body.data() + offset);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), size), body.data() + offset + 2U);
        std::copy_n(encoded.data(), size, body.data() + offset + store::WIRE_RECORD_HEADER_BYTES);
        previous = frame;
    }
    std::array<uint8_t, 53U> begin{{0x4dU, 2U, store::STORE_BEGIN}};
    fxe::WriteU32LE(session, begin.data() + 3U);
    fxe::WriteU16LE(TRANSFER, begin.data() + 7U);
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), begin.data() + 13U);
    fxe::WriteU32LE(fxe::Crc32(body.data(), body.size()), begin.data() + 17U);
    fxe::WriteU16LE(store::FRAME_COUNT, begin.data() + 21U);
    begin[23U] = 3U;
    fxe::WriteU32LE(1U, begin.data() + 25U);
    begin[24U] = 0x15U; // Invalid reserved bit, then active/paused predecessor records.
    enqueue(begin.data(), begin.size());
    waitResponse(TRANSFER);
    Check(response[13U] == static_cast<uint8_t>(store::WireStatus::Malformed) &&
              rootUnchanged() && stream.FrameCount() == 0U,
          "corrupt recovery still rejects malformed replacement without changing the root");
    worker.AcceptLitchcraftFrameResponse();
    begin[24U] = 5U;
    enqueue(begin.data(), begin.size());
    worker_fixture::Wait([&] { return !queue.TryPeekLitchcraftFrameStore() &&
                                    worker_fixture::atWait.load(); },
                         "corrupt journey admits explicit validated Begin");
    responseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
              rootUnchanged(),
          "validated Begin is admitted without replacing the damaged root");
    if (responseSize != 0U) { stop(); return; }
    for (size_t offset = 0U; offset < body.size(); offset += 400U) {
        const size_t count = std::min<size_t>(400U, body.size() - offset);
        std::array<uint8_t, 413U> chunk{};
        std::copy_n(begin.data(), 9U, chunk.data());
        chunk[2U] = store::STORE_CHUNK;
        fxe::WriteU32LE(static_cast<uint32_t>(offset), chunk.data() + 9U);
        std::copy_n(body.data() + offset, count, chunk.data() + 13U);
        enqueue(chunk.data(), 13U + count);
        worker_fixture::Wait([&] {
            responseSize = 0U;
            return (!queue.TryPeekLitchcraftFrameStore() ||
                    worker.PeekLitchcraftFrameResponse(response.data(), response.size(),
                                                       responseSize)) &&
                   worker_fixture::atWait.load();
        }, "corrupt journey drains bounded body packet");
        Check(responseSize == 0U, "validated body packet proceeds without a refusal");
        if (responseSize != 0U) {
            std::printf("[CORRUPT_STORE_REPLACEMENT_JOURNEY] refused cursor=%lu status=%u\n",
                        static_cast<unsigned long>(offset), response[13U]);
            stop();
            return;
        }
    }
    {
        store::FrameStore staged(files.StoreOps());
        Check(staged.Recover() == store::Status::NoArtwork &&
                  stream.FrameCount() == 0U,
              "accepted body staging exposes no valid root or artwork before Commit");
    }
    if (retryRoot) Check(worker.BlockLitchcraftFrameRootWritesForTest(true),
                         "corrupt journey blocks only root publication");
    std::array<uint8_t, 13U> commit{};
    std::copy_n(begin.data(), 9U, commit.data());
    commit[2U] = store::STORE_COMMIT;
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), commit.data() + 9U);
    enqueue(commit.data(), commit.size());
    if (retryRoot) {
        worker_fixture::Wait([&] { return worker.LitchcraftFrameCommitPending() &&
                                        worker_fixture::atWait.load(); },
                             "corrupt replacement waits for durable root publication");
        Check(!queue.TryPeekLitchcraftFrameStore(),
              "accepted Commit releases ingress credit while the board retries publication");
        store::FrameStore unpublished(files.StoreOps());
        Check(unpublished.Recover() == store::Status::NoArtwork &&
                  stream.FrameCount() == 0U,
              "failed Commit keeps the replacement hidden until durable publication");
        Check(!worker.RequestLitchcraftFrameClear(),
              "clear refuses an irrevocably accepted Commit publication");
        Check(worker.BlockLitchcraftFrameRootWritesForTest(false),
              "corrupt journey releases root publication fault");
        fxe::ManualMillis::SetNowMs(fxe::Millis() + 2000U);
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    }
    waitResponse(TRANSFER);
    Check(response[13U] == 0U && !worker.LitchcraftFrameCommitPending(),
          "corrupt replacement receives durable Commit acknowledgement");
    worker.AcceptLitchcraftFrameResponse();
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 2000U);
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
               !worker.GetDiagnostics().frameStoreModeSaveFailed &&
               stream.FrameCount() == store::FRAME_COUNT && stream.TryPresentRequested() &&
               worker_fixture::atWait.load();
    }, "committed corrupt replacement clears recovery and mode barriers");
    Check(worker.GetDiagnostics().frameStoreRecovery == static_cast<uint8_t>(store::Status::Ok) &&
              board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsActive() &&
              !board.Litchcraft().IsAnimationPlaying() && board.SemanticFrame().board[9U][0U].r == 40U,
          "validated replacement displays first frame paused with settled recovery");
    store::FrameStore reboot(files.StoreOps());
    store::Frame last{};
    Check(reboot.Recover() == store::Status::Ok &&
              reboot.CurrentManifest().frameCount == store::FRAME_COUNT &&
              reboot.CurrentManifest().active && !reboot.CurrentManifest().playbackOnEntry &&
              reboot.ReadFrame(255U, last) == store::Status::Ok &&
              last.emitters[9U * 4U].color.r == static_cast<uint8_t>(295U),
          "corrupt replacement is reboot-readable through the last of256 frames");
    std::printf("[CORRUPT_STORE_REPLACEMENT_JOURNEY] malformed rejected -> explicit Open | "
                "Commit=%s mode=Litchcraft frames=256 paused square9=40 next=Play recovery=Ok\n",
                retryRoot ? "retry-durable" : "immediate-durable");

    std::array<uint8_t, 17U> snapshot{{0x4dU, 2U, store::STORE_SNAPSHOT}};
    fxe::WriteU32LE(session, snapshot.data() + 3U);
    fxe::WriteU16LE(20U, snapshot.data() + 7U);
    fxe::WriteU32LE(stream.Generation(), snapshot.data() + 9U);
    enqueue(snapshot.data(), snapshot.size());
    std::vector<uint8_t> downloaded;
    do {
        waitResponse(20U);
        Check(response[2U] == store::STORE_SNAPSHOT_CHUNK && response[13U] == 0U &&
                  responseSize > 60U &&
                  fxe::ReadU32LE(response.data() + 28U) == downloaded.size(),
              "reverse256 snapshot uses exact authoritative cursor and successful response");
        if (responseSize <= 60U) { stop(); return; }
        downloaded.insert(downloaded.end(), response.begin() + 60U,
                          response.begin() + responseSize);
        worker.AcceptLitchcraftFrameResponse();
    } while (downloaded.size() < body.size());
    Check(downloaded == body, "production worker reverse sync returns identical canonical256 artwork");
    const auto readTiming = worker.GetDiagnostics();
    Check(readTiming.storageReadState == 7U && readTiming.storageReadBytes >= 256U * layout::FRAME_SLOT_BYTES &&
              readTiming.storageReadCalls != 0U && readTiming.storageReadHash == fxe::Crc32(body.data(), body.size()),
          "actual reverse Store producer records complete request-local file bytes and canonical CRC");

    const uint32_t priorGeneration = stream.Generation();
    const auto priorColor = board.Litchcraft().Color();
    const auto priorTarget = board.Litchcraft().Target();
    const bool priorErase = board.Litchcraft().IsErasing();
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    const char* const clearBackup = "build/test_litchcraft_clear_retry.root0";
    if (retryRoot) {
        (void)std::remove(clearBackup);
        Check(std::rename(root, clearBackup) == 0 && ::mkdir(root, 0700) == 0,
              "clear journey temporarily blocks root filesystem reads and writes");
        arbiter.BlockNextFlash();
    }
    Check(worker.RequestLitchcraftFrameClear() && stream.FrameCount() == 0U,
          "console clear retires live artwork immediately before worker flash");
    fxe::WriteU16LE(42U, begin.data() + 7U);
    fxe::WriteU32LE(priorGeneration, begin.data() + 9U);
    enqueue(begin.data(), begin.size());
    const uint32_t newerEpoch = retryRoot ? queue.AdvanceEpoch() : 0U;
    Check(worker.LitchcraftFrameClearPending(), "queued clear retains its lifecycle barrier");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    if (retryRoot) {
        arbiter.WaitForBlockedFlash();
        Check(worker.LitchcraftFrameClearPending(), "running clear retains lifecycle barrier");
        // Invoke the production session callback while flash is held. The later
        // physical Exit/re-entry checkpoints cover the actual chord seam.
        worker.NotifyLitchcraftFrameSession(false, session, false);
        worker.NotifyLitchcraftFrameSession(false, session, true);
        arbiter.ReleaseBlockedFlash();
        worker_fixture::Wait([&] {
            return worker.LitchcraftFrameClearState() == Worker::FrameClearState::Failed &&
                   worker_fixture::atWait.load();
        }, "clear reports storage failure while preserving newer mode authority");
        Check(queue.CurrentEpoch() == newerEpoch && stream.FrameCount() == 0U &&
                  worker.LitchcraftFrameModeSavePending(),
              "clear leaves newer epoch and unresolved mode barrier intact with prior output hidden");
        Check(::rmdir(root) == 0 && std::rename(clearBackup, root) == 0,
              "clear journey restores its exact root after the controlled fault");
        Check(!worker.RequestLitchcraftFrameClear() &&
                  worker.TakeLitchcraftFrameClearResult() == Worker::FrameClearState::Failed,
              "failure remains retained until Console consumes it before retry");
        Check(worker.RequestLitchcraftFrameClear(),
              "clear retries after failure even though live presentation is already retired");
    }
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameClearState() == Worker::FrameClearState::Succeeded &&
               stream.FrameCount() == 1U && stream.TryPresentRequested() &&
               !queue.TryPeekLitchcraftFrameStore() && worker_fixture::atWait.load();
    }, "clear durably retires residents and adopts one paused blank above old fences");
    responseSize = 0U;
    Check(stream.Generation() > priorGeneration &&
              !worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
              !worker.LitchcraftFrameModeSavePending() &&
              board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsActive() &&
              !board.Litchcraft().IsAnimationPlaying() &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "clear discards old queued upload and leaves active blank paused without old output");
    store::FrameStore cleared(files.StoreOps());
    store::Frame blank{};
    Check(cleared.Recover() == store::Status::Ok && cleared.ResidentCount() == 1U &&
              cleared.CurrentManifest().frameCount == 1U &&
              cleared.ReadFrame(0U, blank) == store::Status::Ok &&
              blank.emitters[9U * 4U].color.r == 0U,
          "clear reboot recovers only the new blank, never retired animated residents");
    const uint32_t firstClearGeneration = stream.Generation();
    Check(!worker.RequestLitchcraftFrameClear() &&
              worker.TakeLitchcraftFrameClearResult() == Worker::FrameClearState::Succeeded,
          "success remains retained until Console consumes it before repeat");
    Check(worker.RequestLitchcraftFrameClear(), "repeated console clear is admitted");
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameClearState() == Worker::FrameClearState::Succeeded &&
               stream.Generation() > firstClearGeneration && stream.FrameCount() == 1U &&
               stream.TryPresentRequested() && worker_fixture::atWait.load();
    }, "second clear advances durable authority without resetting generation");
    Check(board.Litchcraft().Color().r == priorColor.r &&
              board.Litchcraft().Color().g == priorColor.g &&
              board.Litchcraft().Color().b == priorColor.b &&
              board.Litchcraft().Target() == priorTarget &&
              board.Litchcraft().IsErasing() == priorErase,
          "explicit clear preserves tool target and selected color");
    Check(worker.TakeLitchcraftFrameClearResult() == Worker::FrameClearState::Succeeded &&
              worker.TakeLitchcraftFrameClearResult() == Worker::FrameClearState::Idle,
          "terminal clear result is retained until consumed exactly once by Console owner");
    const auto chord = [&] {
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::paused = true;
        }
        button(runtime::VirtualLitchPhysicalButton::X, true);
        Check(board.Advance(70U) == VIRTUAL_LITCH_OK, "clear exit/re-entry chord staggers X+O");
        button(runtime::VirtualLitchPhysicalButton::O, true);
        Check(board.Advance(1440U) == VIRTUAL_LITCH_OK, "clear chord completes hold");
        button(runtime::VirtualLitchPhysicalButton::X, false);
        button(runtime::VirtualLitchPhysicalButton::O, false);
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::paused = false;
            worker_fixture::changed.notify_all();
        }
        worker_fixture::Wait([&] {
            return !worker.LitchcraftFrameModeSavePending() && worker_fixture::atWait.load();
        }, "clear Exit/re-entry settles durable mode");
    };
    chord();
    Check(!board.Litchcraft().IsActive() && worker.RequestLitchcraftFrameClear(),
          "inactive Setup admits clear without entering Litchcraft");
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameClearState() == Worker::FrameClearState::Succeeded &&
               worker_fixture::atWait.load();
    }, "inactive clear reaches terminal success");
    store::FrameStore inactive(files.StoreOps());
    Check(inactive.Recover() == store::Status::NoArtwork && stream.FrameCount() == 0U &&
              !board.Litchcraft().IsActive(), "inactive clear reboot retains no artwork and Setup");
    chord();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == 1U && stream.TryPresentRequested() &&
               worker_fixture::atWait.load();
    }, "next physical Enter creates the sole paused blank above retirement fence");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsActive() &&
              board.SemanticFrame().board[9U][0U].r == 0U,
          "clear recovery physical entry displays blank without retired animation");
    std::puts("[STORE_CLEAR_TWO_WAY_JOURNEY] upload256 -> download256 exact | clear+stale-Begin "
              "-> blank paused -> repeat clear -> physical Exit -> inactive clear -> physical Enter blank");
    stop();
}

void RunNoRootReentryRaceWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "reentry race starts with no Store root");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_no_root_reentry_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "reentry race loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "reentry race starts production persistence worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); },
                         "reentry race waits for worker startup");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "reentry race starts from an absent Store");
    struct Reentry final {
        Worker* worker;
        std::atomic<bool> fired{false};
    } reentry{&worker};
    worker.SetLitchcraftFrameBeforeSyncTestHook(
        [](void* context) {
            auto& state = *static_cast<Reentry*>(context);
            if (!state.fired.exchange(true)) {
                state.worker->SetLitchcraftFrameBeforeSyncTestHook(
                    nullptr, nullptr);
                state.worker->NotifyLitchcraftFrameSession(false, 92U, true);
            }
        },
        &reentry);
    worker.NotifyLitchcraftFrameSession(false, 92U, false);
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return reentry.fired.load() && stream.Generation() == 1U &&
            stream.FrameCount() == 1U &&
            !worker.LitchcraftFrameModeSavePending() &&
            worker_fixture::atWait.load();
    }, "new Enter during empty Store sync keeps its worker wake");
    std::puts("[NO_ROOT_REENTRY_RACE] inactive-root=absent "
              "concurrent-Enter=root1 barrier=clear next=paint");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "reentry race removes frame banks");
}


std::vector<uint8_t> ResidentRgb(const char* const encoded, const size_t size,
                                const uint32_t crc) {
    std::vector<uint8_t> rgb;
    rgb.reserve(size);
    uint32_t accumulator = 0U;
    uint8_t bits = 0U;
    for (const char* cursor = encoded; *cursor != '\0' && *cursor != '='; ++cursor) {
        const char value = *cursor;
        const uint8_t digit = value >= 'A' && value <= 'Z' ? value - 'A'
            : value >= 'a' && value <= 'z' ? value - 'a' + 26U
            : value >= '0' && value <= '9' ? value - '0' + 52U
            : value == '+' ? 62U : 63U;
        accumulator = (accumulator << 6U) | digit;
        bits = static_cast<uint8_t>(bits + 6U);
        if (bits < 8U) continue;
        bits = static_cast<uint8_t>(bits - 8U);
        rgb.push_back(static_cast<uint8_t>(accumulator >> bits));
        accumulator &= bits == 0U ? 0U : (1U << bits) - 1U;
    }
    Check(rgb.size() == size && fxe::Crc32(rgb.data(), rgb.size()) == crc,
          "resident lifetime corpus matches exact app RGB payload");
    return rgb;
}

modules::litchcraft_frame_store::Frame ResidentFrame(
    const std::vector<uint8_t>& rgb, const uint16_t index, const bool rickroll) {
    modules::litchcraft_frame_store::Frame frame{};
    frame.durationMs = 50U;
    for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter) {
        const size_t offset = (index * frame.emitters.size() + emitter) * 3U;
        const size_t column = (emitter / 4U) % 8U;
        const size_t side = emitter % 4U;
        const bool backdrop = rickroll &&
            ((column == 0U && side == 0U) || (column == 7U && side == 2U));
        frame.emitters[emitter].color = backdrop ? modules::Rgb{8U, 16U, 24U}
            : modules::Rgb{rgb[offset], rgb[offset + 1U], rgb[offset + 2U]};
        frame.emitters[emitter].effect = backdrop ? 1U : 0U;
        frame.emitters[emitter].effectDurationMs = backdrop ? 1600U : 0U;
    }
    return frame;
}

bool SameResidentFrame(const modules::litchcraft_frame_store::Frame& left,
                        const modules::litchcraft_frame_store::Frame& right) {
    if (left.durationMs != right.durationMs) return false;
    for (size_t index = 0U; index < left.emitters.size(); ++index) {
        const auto& a = left.emitters[index];
        const auto& b = right.emitters[index];
        if (a.color.r != b.color.r || a.color.g != b.color.g ||
            a.color.b != b.color.b || a.effect != b.effect ||
            a.effectDurationMs != b.effectDurationMs) return false;
    }
    return true;
}

std::vector<std::string> ResidentPhysicalPaths() {
    using File = firmware::LitchcraftFrameFile;
    std::vector<std::string> paths{File::DEFAULT_ROOT0_PATH, File::DEFAULT_ROOT1_PATH};
    for (const char* base : {File::DEFAULT_BODY0_PATH, File::DEFAULT_BODY1_PATH}) {
        for (uint8_t part = 0U; part < File::BODY_FILES_PER_BANK; ++part) {
            char suffix[8]{}; std::snprintf(suffix, sizeof(suffix), ".%02u", unsigned(part));
            const std::string path = std::string(base) + suffix;
            struct stat info{};
            if (::stat(path.c_str(), &info) == 0) paths.push_back(path);
            else Check(errno == ENOENT, "resident snapshot distinguishes lazy absence from filesystem failure");
        }
    }
    return paths;
}

std::vector<uint8_t> ResidentFileBytes(const char* const path) {
    std::vector<uint8_t> bytes;
    std::FILE* input = std::fopen(path, "rb");
    Check(input != nullptr, "resident selection snapshots production file");
    if (input == nullptr) return bytes;
    for (int byte = std::fgetc(input); byte != EOF; byte = std::fgetc(input)) {
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    Check(std::fclose(input) == 0, "resident selection closes file snapshot");
    return bytes;
}


void RunSettingsReadDiagnosticWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    worker_fixture::notified = false; worker_fixture::stopping = false;
    worker_fixture::paused = false; worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(60000U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "readonly worker starts with no artwork fixture");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_settings_read_diagnostic_worker.json";
    (void)::unlink(config.path);
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "readonly worker loads its live settings owner");
    settings.Doc()["FEN"] = "8/8/8/8/8/8/4K3/7k w - - 0 1";
    settings.Doc()["BRIGHTNESS"] = 13U;
    settings.Doc()["PAIRING"] = "preserved";
    Check(settings.SaveNowBlocking(), "readonly worker seeds durable primary");
    const auto sourceBefore = [&] { std::string value; serializeJson(settings.Doc(), value); return value; }();
    const auto diskBefore = ResidentFileBytes(config.path);
    const auto savesBefore = settings.GetSaveCount();
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "readonly diagnostic uses actual storage task");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] { return worker.IsIdle() && worker_fixture::atWait.load(); },
                         "readonly diagnostic waits for production startup");
    Check(worker.RequestSettingsReadDiagnostic(), "worker admits one readonly diagnostic");
    worker_fixture::Wait([&] { return worker.GetDiagnostics().storageReadState == 3U && worker_fixture::atWait.load(); },
                         "worker independently reads and validates primary");
    const auto measured = worker.GetDiagnostics();
    uint32_t hash = 2166136261U;
    for (const auto byte : diskBefore) { hash ^= byte; hash *= 16777619U; }
    std::string liveAfter; serializeJson(settings.Doc(), liveAfter);
    Check(measured.storageReadBytes == diskBefore.size() && measured.storageReadHash == hash &&
              sourceBefore == liveAfter && ResidentFileBytes(config.path) == diskBefore &&
              settings.GetSaveCount() == savesBefore && settings.GetErrorCount() == 0U &&
              measured.requested == 0U && measured.completed == 0U && measured.receipt == 0U,
          "readonly task reports real bytes/hash without changing disk, live game/settings or save receipts");
    std::FILE* broken = std::fopen(config.path, "wb");
    Check(broken != nullptr && std::fputs("{bad", broken) >= 0 && std::fclose(broken) == 0,
          "readonly worker injects an invalid primary read source");
    const auto damaged = ResidentFileBytes(config.path);
    Check(worker.RequestSettingsReadDiagnostic(), "readonly failure can be requested independently");
    worker_fixture::Wait([&] { return worker.GetDiagnostics().storageReadState == 4U && worker_fixture::atWait.load(); },
                         "invalid primary reports failed validation");
    liveAfter.clear(); serializeJson(settings.Doc(), liveAfter);
    Check(worker.GetDiagnostics().storageReadBytes == damaged.size() &&
              ResidentFileBytes(config.path) == damaged && liveAfter == sourceBefore &&
              settings.GetSaveCount() == savesBefore && settings.GetErrorCount() == 0U,
          "diagnostic failure never repairs source, falls back, applies RAM data or changes save health");
    Check(worker.Enqueue(settings.Doc()), "ordinary queued save follows a failed readonly diagnostic");
    worker_fixture::Wait([&] { return worker.IsIdle() && worker.GetDiagnostics().completed != 0U && worker_fixture::atWait.load(); },
                         "private document is reusable by ordinary PrepareBuffer and verified save");
    const auto verifiedSave = worker.GetDiagnostics();
    const auto verifiedCount = settings.GetSaveCount();
    Check(verifiedSave.settingsTiming.available && verifiedSave.settingsTiming.succeeded &&
              worker.RequestSettingsReadDiagnostic(),
          "independent read follows an actual verified worker save");
    worker_fixture::Wait([&] { return worker.GetDiagnostics().storageReadState == 3U && worker_fixture::atWait.load(); },
                         "independent read completes without republishing save state");
    const auto afterRead = worker.GetDiagnostics();
    Check(afterRead.settingsTiming.sequence == verifiedSave.settingsTiming.sequence &&
              afterRead.settingsTiming.generation == verifiedSave.settingsTiming.generation &&
              afterRead.settingsTiming.elapsedMs == verifiedSave.settingsTiming.elapsedMs &&
              afterRead.settingsLastOperation.saveGeneration == verifiedSave.settingsLastOperation.saveGeneration &&
              afterRead.requested == verifiedSave.requested && afterRead.completed == verifiedSave.completed &&
              settings.GetSaveCount() == verifiedCount,
          "readonly completion preserves last verified save sample, generations and durable-save count");
    std::puts("[SETTINGS_READ_DIAGNOSTIC_WORKER] exact bytes/hash live+disk unchanged; invalid primary rejected; normal save and verified sample retained");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true; worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "readonly worker removes isolated frame files");
    (void)::unlink(config.path);
    const std::string backup = std::string(config.path) + ".bak";
    const std::string temporary = std::string(config.path) + ".tmp";
    (void)::unlink(backup.c_str()); (void)::unlink(temporary.c_str());
}

// Model 100 ms per completed frame write at the existing production timing
// boundary. Real worker/FIFO/POSIX files remain in use; this excludes silicon,
// inner LittleFS operations and SDK render handoff timing.
void RunBoundedIngressWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    Check(::mkdir("build", 0777) == 0 || errno == EEXIST, "bounded ingress fixture directory exists");
    worker_fixture::notified = worker_fixture::stopping = false;
    worker_fixture::paused = true; worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(0U);
    firmware::LitchcraftFrameFile files; Check(files.Remove(), "bounded ingress isolates frame banks");
    store::FrameStore baseline(files.StoreOps()); store::Manifest saved{};
    saved.frameCount = 1U; saved.active = true; store::TransactionToken savedToken{};
    Check(baseline.Recover() == store::Status::NoArtwork &&
        baseline.Begin(saved, 0U, 1U, savedToken) == store::Status::Ok &&
        baseline.WriteBlankFirstFrame(savedToken) == store::Status::Ok &&
        baseline.Commit(savedToken) == store::Status::Ok,
        "bounded ingress starts with a real prior resident to preserve");
    const uint32_t priorGeneration = baseline.Generation();
    fxe::SettingsStoreConfig config; config.path = "build/test_bounded_ingress.json";
    fxe::SettingsStore settings(config); Check(settings.Load(), "bounded ingress loads settings");
    modules::BleOtaChunkQueue queue; Check(queue.Begin(), "bounded ingress starts actual domain FIFO");
    struct Model {
        std::mutex mutex; std::condition_variable changed;
        std::atomic<bool> active{false};
        bool waiting = false, resume = false;
        uint32_t frames = 0U;
        void AfterWrite() {
            if (!active.load()) return;
            fxe::ManualMillis::AdvanceMs(100U);
            std::unique_lock<std::mutex> lock(mutex);
            ++frames; waiting = true; changed.notify_all();
            if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return resume; })) std::abort();
            resume = false; waiting = false;
        }
    } model;
    hardware::ArtworkTestArbiter arbiter; Worker worker(config);
    worker.SetLitchcraftFrameTimingTestHook([](void* context,
        const store::FrameStore::TimingPhase phase, uint32_t) {
        if (phase == store::FrameStore::TimingPhase::BodyWrite)
            static_cast<Model*>(context)->AfterWrite();
    }, &model);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "bounded ingress starts actual persistence worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); });
    worker.NotifyLitchcraftFrameRuntimeReady();
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = false; worker_fixture::changed.notify_all(); }
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameStream().TryPresentRequested() &&
            !worker.GetDiagnostics().frameStoreStorageActive && worker_fixture::atWait.load();
    }, "bounded ingress starts after production boot presents and releases prior resident");
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = true; }
    std::vector<uint8_t> body; store::Frame previous{};
    const char* const rainbowPath = std::getenv("LITCH_RAINBOW_BODY");
    if (rainbowPath != nullptr) {
        std::FILE* input = std::fopen(rainbowPath, "rb");
        Check(input != nullptr, "temporary production Rainbow body opens");
        if (input == nullptr) std::abort();
        std::fseek(input, 0, SEEK_END); const auto size = std::ftell(input); std::rewind(input);
        body.resize(static_cast<size_t>(size));
        Check(std::fread(body.data(), 1U, body.size(), input) == body.size(), "temporary Rainbow body is complete");
        std::fclose(input);
        Check(body.size() == 69683U && fxe::Crc32(body.data(), body.size()) == 3175823813U,
              "temporary Rainbow body matches exported production identity, not phone verification");
    } else for (uint16_t index = 0U; index < 256U; ++index) {
        store::Frame frame{}; frame.durationMs = 50U;
        for (uint16_t emitter = 0U; emitter < 24U; ++emitter)
            frame.emitters[emitter].color = {static_cast<uint8_t>(40U + index + emitter * 7U),
                static_cast<uint8_t>(2U + emitter * 3U), 3U};
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{}; size_t size = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(index % store::KEYFRAME_INTERVAL == 0U
            ? nullptr : &previous, frame, encoded.data(), encoded.size(), size), "bounded corpus uses real FrameCodec");
        const size_t at = body.size(); body.resize(at + 6U + size);
        fxe::WriteU16LE(static_cast<uint16_t>(size), body.data() + at);
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), size), body.data() + at + 2U);
        std::copy_n(encoded.data(), size, body.data() + at + 6U); previous = frame;
    }
    constexpr uint32_t SESSION = 191U, OWNER = 91U;
    const store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    std::array<uint8_t, 53U> begin{{0x4dU, 2U, store::STORE_BEGIN}};
    fxe::WriteU32LE(SESSION, begin.data() + 3U); fxe::WriteU16LE(9U, begin.data() + 7U);
    fxe::WriteU32LE(priorGeneration, begin.data() + 9U);
    fxe::WriteU32LE(static_cast<uint32_t>(body.size()), begin.data() + 13U);
    fxe::WriteU32LE(fxe::Crc32(body.data(), body.size()), begin.data() + 17U);
    fxe::WriteU16LE(256U, begin.data() + 21U); begin[23U] = 3U; begin[24U] = rainbowPath ? 13U : 5U;
    fxe::WriteU32LE(23U, begin.data() + 25U);
    Check(queue.EnqueueReserved(begin.data(), begin.size(), 1U) == modules::BleOtaChunkQueue::Admission::Accepted,
          "bounded ingress admits Begin");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = false; worker_fixture::changed.notify_all(); }
    worker_fixture::Wait([&] { return !queue.TryPeekLitchcraftFrameStore() && worker_fixture::atWait.load(); }, "actual worker accepts Begin");
    std::array<uint8_t, 512U> response{}; size_t responseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
        worker.GetDiagnostics().frameStoreStorageActive, "valid Begin establishes active transfer without refusal");
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] { return stream.TryPresentRequested() && worker_fixture::atWait.load(); },
                         "Commit Show fixture presents its actual prior resident");
    modules::LitchcraftSession presentationSession;
    presentationSession.Configure(3U, SESSION - 1U);
    presentationSession.AttachFrameStream(&stream);
    presentationSession.SetIndependentPresentation(true);
    modules::litchcraft_presentation::Inputs commitInputs{};
    modules::litchcraft_presentation::Renderer commitRenderer;
    const modules::litchcraft_presentation::StreamPort commitPort{&stream, nullptr, nullptr, true};
    modules::BoardLedFrame commitPixels{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> commitStatus{};
    presentationSession.CapturePresentation(commitInputs);
    auto confirmed = commitRenderer.Compose(commitInputs, commitPort, commitPixels, commitStatus);
    Check(commitRenderer.NotePresented(confirmed, commitPort) &&
              presentationSession.ApplyPresentationFeedback(confirmed),
          "Commit fixture confirms prior production renderer Show");
    modules::litchcraft_presentation::Feedback heldCommit{};
    bool commitHeld = false, commitReleased = false, commitWaited = false;
    uint32_t beforeCommitPass = 0U;
    std::vector<std::vector<uint8_t>> packets;
    for (size_t at = 0U; at < body.size(); at += 217U) {
        const size_t count = std::min<size_t>(217U, body.size() - at);
        std::vector<uint8_t> packet(13U + count); std::copy_n(begin.data(), 9U, packet.data());
        packet[2U] = store::STORE_CHUNK; fxe::WriteU32LE(static_cast<uint32_t>(at), packet.data() + 9U);
        std::copy_n(body.data() + at, count, packet.data() + 13U); packets.push_back(std::move(packet));
    }
    std::vector<uint8_t> commit(13U); std::copy_n(begin.data(), 9U, commit.data());
    commit[2U] = store::STORE_COMMIT; fxe::WriteU32LE(static_cast<uint32_t>(body.size()), commit.data() + 9U);
    packets.push_back(std::move(commit));
    size_t accepted = 0U; uint32_t deferrals = 0U, lastAdmission = 0U, maximumGap = 0U, commitAdmission = 0U;
    const auto fill = [&] {
        const size_t before = accepted;
        while (accepted < packets.size()) {
            const auto& packet = packets[accepted];
            if (queue.EnqueueReserved(packet.data(), packet.size(), static_cast<uint32_t>(accepted + 2U)) !=
                modules::BleOtaChunkQueue::Admission::Accepted) { ++deferrals; break; }
            ++accepted; worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        }
        if (accepted != before) lastAdmission = fxe::Millis();
        if (before != packets.size() && accepted == packets.size()) commitAdmission = fxe::Millis();
    };
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = true; }
    fill(); Check(accepted == modules::BleOtaChunkQueue::CAPACITY && accepted < packets.size(), "real FIFO starts full with retained upload suffix");
    model.active.store(true);
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::paused = false; worker_fixture::changed.notify_all(); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(model.mutex);
        model.changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return model.waiting; });
        if (model.waiting) {
            if (accepted < packets.size()) maximumGap = std::max(maximumGap, fxe::Millis() - lastAdmission);
            fill();
            if (model.frames == 256U && !commitHeld) {
                presentationSession.CapturePresentation(commitInputs);
                heldCommit = commitRenderer.Compose(commitInputs, commitPort, commitPixels, commitStatus);
                commitHeld = heldCommit.ready && stream.HasPresentationCandidate();
                beforeCommitPass = worker_fixture::waitEntries.load();
            }
            model.resume = true; model.changed.notify_all();
        }
        lock.unlock();
        if (commitHeld && !commitReleased) {
            bool settled = false;
            {
                std::lock_guard<std::mutex> fixtureLock(worker_fixture::mutex);
                settled = worker_fixture::waitEntries.load() > beforeCommitPass &&
                    worker_fixture::atWait.load() && !worker_fixture::notified;
            }
            if (settled) {
                std::array<uint8_t, 9U> unrelatedCancel{0x4dU, 2U, store::STORE_CANCEL};
                fxe::WriteU32LE(SESSION, unrelatedCancel.data() + 3U);
                fxe::WriteU16LE(10U, unrelatedCancel.data() + 7U);
                worker.NotifyLitchcraftFramePriority(unrelatedCancel.data(), unrelatedCancel.size(), OWNER);
                worker_fixture::Wait([&] {
                    return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
                        worker_fixture::atWait.load();
                }, "unrelated priority Cancel resolves while original Commit stays queued");
                const bool unrelatedRejected = response[13U] == 6U &&
                    fxe::ReadU16LE(response.data() + 11U) == 10U && queue.TryPeekLitchcraftFrameStore();
                worker.AcceptLitchcraftFrameResponse();
                worker_fixture::Wait([&] {
                    return worker_fixture::atWait.load() && !worker_fixture::notified &&
                        !worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize);
                }, "same prepared Commit remains pending after unrelated Cancel");
                Check(unrelatedRejected, "unrelated Cancel preserves original prepared Commit identity");
                store::FrameStore prior(files.StoreOps());
                commitWaited = unrelatedRejected && !worker.PeekLitchcraftFrameResponse(
                    response.data(), response.size(), responseSize) &&
                    queue.TryPeekLitchcraftFrameStore() && stream.Generation() == priorGeneration &&
                    !stream.ReplacementPending() && prior.Recover() == store::Status::Ok &&
                    prior.Generation() == priorGeneration;
                commitRenderer.Cancel(heldCommit, commitPort);
                presentationSession.SetIndependentPresentation(false);
                commitReleased = true;
            }
        }
        if (worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) && worker_fixture::atWait.load()) break;
    }
    // Release even on a failed observation; no retained Show can hang cleanup.
    commitRenderer.Cancel(heldCommit, commitPort);
    presentationSession.SetIndependentPresentation(false);
    const uint32_t commitTail = fxe::Millis() - commitAdmission;
    model.active.store(false);
    Check(commitHeld && commitReleased && commitWaited,
          "original prepared Commit waits for transient Show with prior root protected");
    std::printf("[HELD_SHOW_COMMIT] request=9 held=%u pending_before_release=%u released=%u status=%u resubmits=0 body_reuploads=0 Show=controlled\n",
        unsigned(commitHeld), unsigned(commitWaited), unsigned(commitReleased), unsigned(response[13U]));
    Check(deferrals != 0U && accepted == packets.size(), "real FIFO defers and later admits the complete suffix");
    Check(maximumGap < 2000U, "compressed worker ingress does not pin accepted frontier across modeled two-second deadline");
    Check(responseSize == 111U && response[13U] == 0U && model.frames == 256U &&
              worker.GetDiagnostics().frameStoreIngressPackets == packets.size() + 1U,
          "Commit waits for all frames, counts each admitted packet once and returns unchanged authoritative receipt");
    std::printf("[BOUNDED_WORKER_INGRESS] corpus=%s bytes=%zu packets=%zu frames=%u receipt_bytes=%zu receipt_status=%u max_frontier_gap_ms=%u commit_tail_ms=%u commit_6s_model_gate=%u model_frame_write_ms=100\n",
        rainbowPath ? "production_rainbow_temp" : "controlled_256", body.size(), packets.size(), model.frames,
        responseSize, unsigned(response[13U]), maximumGap, commitTail, unsigned(commitTail <= 6000U));
    worker.AcceptLitchcraftFrameResponse();
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex); worker_fixture::stopping = true; worker_fixture::changed.notify_all(); }
    worker_fixture::worker.join();
    store::FrameStore recovered(files.StoreOps());
    Check(recovered.Recover() == store::Status::Ok && recovered.CurrentManifest().frameCount == 256U &&
        recovered.CurrentBodyCrc32() == (rainbowPath ? 271692598U : fxe::Crc32(body.data(), body.size())),
        "bounded ingress preserves canonical CRC and durable root identity");
    Check(files.Remove(), "bounded ingress removes isolated frame files");
}

void RunResidentSelectionReleaseWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(40000U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "resident release journey starts with empty banks");
    const auto rr = ResidentRgb(rickroll_fixture::RGB_BASE64,
                                rickroll_fixture::RGB_BYTES,
                                rickroll_fixture::RGB_CRC32);
    const auto balls = ResidentRgb(bouncing_balls_fixture::RGB_BASE64,
                                   bouncing_balls_fixture::RGB_BYTES,
                                   bouncing_balls_fixture::RGB_CRC32);
    store::FrameStore seed(files.StoreOps());
    Check(seed.Recover() == store::Status::NoArtwork,
          "resident release seed recovers empty Store");
    store::FrameStore::ResidentDescriptor resident[2]{};
    for (uint8_t slot = 0U; slot < 2U; ++slot) {
        store::Manifest manifest{};
        manifest.frameCount = slot == 0U ? 32U : 256U;
        manifest.active = true;
        manifest.sparkleSeed = 1U;
        store::TransactionToken token{};
        Check(seed.Begin(manifest, slot, 700U + slot, token) == store::Status::Ok,
              "resident release seed begins exact RR/Balls corpus");
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            const auto frame = ResidentFrame(slot == 0U ? rr : balls,
                                              index, slot == 0U);
            Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
                  "resident release seed writes exact app frame");
        }
        Check(seed.Commit(token) == store::Status::Ok &&
                  seed.CurrentResident(resident[slot]),
              "resident release seed commits complete corpus");
    }
    const auto paths = ResidentPhysicalPaths();
    std::vector<std::vector<uint8_t>> beforeSelection(paths.size());
    for (size_t index = 0U; index < paths.size(); ++index) {
        beforeSelection[index] = ResidentFileBytes(paths[index].c_str());
    }
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_resident_release_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "resident release journey loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "resident release journey starts real worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    const auto released = [&] {
        return worker.IsIdle() && !worker.GetDiagnostics().frameStoreStorageActive &&
            worker_fixture::atWait.load();
    };
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && released();
    }, "resident release boot presents newer Balls then destroys Store union");
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "resident release journey opens production ingress queue");
    constexpr uint32_t SESSION = 0x52aabbccU;
    constexpr uint32_t OWNER = 73U;
    store::StoreProjection projection{SESSION, 0U, 3U, true, false};
    uint16_t request = 0U;
    uint32_t ordinal = 1U;
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    const auto send = [&](const uint8_t* bytes, const size_t size) {
        Check(queue.Enqueue(bytes, size, ordinal++),
              "resident release journey admits production Store packet");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(
                response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "resident release journey receives exact worker receipt");
    };
    for (const uint8_t slot : {0U, 1U, 0U}) {
        std::array<uint8_t, 18U> select{0x4dU, 2U, store::STORE_SELECT};
        fxe::WriteU32LE(SESSION, select.data() + 3U);
        fxe::WriteU16LE(++request, select.data() + 7U);
        select[9U] = resident[slot].rootSlot;
        fxe::WriteU32LE(resident[slot].generation, select.data() + 10U);
        fxe::WriteU32LE(resident[slot].bodyCrc32, select.data() + 14U);
        send(select.data(), select.size());
        Check(responseSize > 15U && response[13U] == 0U &&
                  response[14U] == resident[slot].rootSlot &&
                  fxe::ReadU32LE(response.data() + 7U) == resident[slot].generation,
              "exact resident selection receipt names intended RR/Balls root");
        worker.AcceptLitchcraftFrameResponse();
        worker_fixture::Wait([&] {
            return stream.TryPresentRequested() && released();
        }, "selected paused resident destroys real Store union");
        std::array<uint8_t, 9U> inspect{0x4dU, 2U, store::STORE_RESIDENTS};
        fxe::WriteU32LE(SESSION, inspect.data() + 3U);
        fxe::WriteU16LE(++request, inspect.data() + 7U);
        send(inspect.data(), inspect.size());
        store::Frame displayed{};
        uint16_t displayedIndex = 99U;
        const bool correct = responseSize > 15U && response[13U] == 0U &&
            response[14U] == resident[slot].rootSlot &&
            fxe::ReadU32LE(response.data() + 7U) == resident[slot].generation &&
            stream.Generation() == resident[slot].generation &&
            stream.CopyDisplayed(displayed, displayedIndex) && displayedIndex == 0U &&
            SameResidentFrame(displayed, ResidentFrame(slot == 0U ? rr : balls, 0U, slot == 0U));
        Check(correct, "paused resident survives actual Store release/recover and metadata read");
        std::printf("[RESIDENT_RELEASE_JOURNEY] %s receipt_generation=%lu stream_generation=%lu "
                    "expected=%lu root=%u body_crc=%08lx result=%s storage=recreated next=Play\n",
                    slot == 0U ? "RR" : "Balls",
                    static_cast<unsigned long>(fxe::ReadU32LE(response.data() + 7U)),
                    static_cast<unsigned long>(stream.Generation()),
                    static_cast<unsigned long>(resident[slot].generation), response[14U],
                    static_cast<unsigned long>(resident[slot].bodyCrc32), correct ? "accepted" : "WRONG_ROOT");
        worker.AcceptLitchcraftFrameResponse();
        worker_fixture::Wait(released, "resident metadata read releases recreated Store");
    }
    Check(ResidentPhysicalPaths() == paths, "resident selection preserves lazy part presence as well as bytes");
    for (size_t index = 0U; index < paths.size(); ++index) {
        Check(ResidentFileBytes(paths[index].c_str()) == beforeSelection[index],
              "A/B switches and worker reconstruction write no root or body bytes");
    }
    // A complete but changed descriptor must not satisfy the retained proof.
    // Corrupt only metadata identity, preserving the old visible RGB frame.
    namespace layout = modules::litchcraft_frame_store_layout;
    std::array<uint8_t, layout::FRAME_SLOT_BYTES> rootScratch{};
    layout::Layout rootStorage(files.StoreOps(), rootScratch.data(), rootScratch.size());
    layout::Root staleRoot{};
    uint8_t staleCopy = 0U;
    Check(rootStorage.LatestRoot(resident[0U].rootSlot, staleRoot, staleCopy) ==
              layout::Status::Ok,
          "resident proof fault reads the actual selected root");
    staleRoot.bodyCrc32 ^= 1U;
    ++staleRoot.serial;
    Check(rootStorage.PublishRoot(staleRoot, staleCopy ^ 1U) == layout::Status::Ok,
          "resident proof fault publishes valid metadata with mismatched content identity");
    std::array<uint8_t, 9U> faultInspect{0x4dU, 2U, store::STORE_RESIDENTS};
    fxe::WriteU32LE(SESSION, faultInspect.data() + 3U);
    fxe::WriteU16LE(++request, faultInspect.data() + 7U);
    send(faultInspect.data(), faultInspect.size());
    store::Frame faultDisplay{};
    uint16_t faultIndex = 99U;
    Check(response[13U] == static_cast<uint8_t>(store::WireStatus::ResourceLimit) &&
              worker.GetDiagnostics().frameStoreRecovery ==
                  static_cast<uint8_t>(store::Status::RevisionConflict) &&
              stream.Generation() == resident[0U].generation &&
              stream.CopyDisplayed(faultDisplay, faultIndex) && faultIndex == 0U &&
              SameResidentFrame(faultDisplay, ResidentFrame(rr, 0U, true)),
          "mismatched retained CRC rejects recovery and preserves complete displayed RR");
    std::puts("[RESIDENT_RELEASE_JOURNEY] stale-resident-CRC | result=rejected "
              "display=RR generation=1 next=repair-storage");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait(released, "rejected retained proof releases Store");
    std::FILE* repaired = std::fopen(paths[0U].c_str(), "wb");
    Check(repaired != nullptr, "resident proof recovery opens saved root bytes");
    if (repaired != nullptr) {
        Check(std::fwrite(beforeSelection[0U].data(), 1U,
                          beforeSelection[0U].size(), repaired) == beforeSelection[0U].size() &&
                  std::fclose(repaired) == 0,
              "resident proof recovery restores the exact original root");
    }
    worker.NotifyLitchcraftFrameSession(false, SESSION, true);
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() && released();
    }, "restored resident proof recovers through existing mode lifecycle");
    Check(stream.Generation() == resident[0U].generation && stream.FrameCount() == 32U &&
              !worker.GetDiagnostics().frameStoreModeSaveFailed,
          "repair resumes selected RR without another animation transfer");
    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        },
        &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() && board.Litchcraft().FrameCount() == 32U,
          "production virtual board renders the retained RR document");
    projection.session = board.Litchcraft().SessionId();
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() && released();
    }, "RR renderer settles entry and destroys Store before ordered Play");
    std::array<uint8_t, 6U> runtimeCommand{1U, 0U};
    fxe::WriteU32LE(resident[0U].generation, runtimeCommand.data() + 2U);
    const uint32_t presentationsBeforePlay = stream.PresentationCount();
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, runtimeCommand.data(), runtimeCommand.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "ordinary shared ordered Play accepts the selected RR generation");
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreStorageActive &&
            worker_fixture::atWait.load();
    }, "ordinary Play reconstructs real Store after idle release");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsAnimationPlaying() && stream.FrameCount() == 32U &&
              stream.Generation() == resident[0U].generation,
          "Play wake retains RR authority and remains physically playing");
    Check(board.Advance(50U) == VIRTUAL_LITCH_OK,
          "RR Play reaches its first authored physical frame boundary");
    worker_fixture::Wait([&] {
        return stream.RequestedIndex() == 1U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "RR Play advances through production worker paging and presentation");
    store::Frame playingFrame{};
    uint16_t playingIndex = 99U;
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsAnimationPlaying() && stream.FrameCount() == 32U &&
              stream.Generation() == resident[0U].generation &&
              stream.PresentationCount() > presentationsBeforePlay &&
              stream.CopyDisplayed(playingFrame, playingIndex) && playingIndex == 1U &&
              SameResidentFrame(playingFrame, ResidentFrame(rr, 1U, true)) &&
              // The legacy semantic middle LED averages top/bottom. Its left
              // LED remains an exact, unaffected authored RGB sample.
              board.SemanticFrame().board[9U][0U].r == playingFrame.emitters[36U].color.r &&
              board.SemanticFrame().board[9U][0U].g == playingFrame.emitters[36U].color.g &&
              board.SemanticFrame().board[9U][0U].b == playingFrame.emitters[36U].color.b,
          "ordered Play advances actual RR pixels and rendered frame without Balls adoption");
    std::printf("[RESIDENT_RELEASE_JOURNEY] Play-after-idle | generation=%lu frames=%u "
                "playing=%u displayed=%u presentations=%lu frame_equal=%u "
                "left9=%u expected_left9=%u result=%s next=Pause\n",
                static_cast<unsigned long>(stream.Generation()), stream.FrameCount(),
                board.Litchcraft().IsAnimationPlaying() ? 1U : 0U, playingIndex,
                static_cast<unsigned long>(stream.PresentationCount()),
                SameResidentFrame(playingFrame, ResidentFrame(rr, 1U, true)) ? 1U : 0U,
                board.SemanticFrame().board[9U][0U].r, playingFrame.emitters[36U].color.r,
                stream.Generation() == resident[0U].generation &&
                    board.Litchcraft().IsAnimationPlaying() && playingIndex == 1U
                    ? "accepted" : "WRONG_PLAYBACK");
    runtimeCommand[0U] = 0U;
    runtimeCommand[1U] = 1U;
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, runtimeCommand.data(), runtimeCommand.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "ordinary Pause ends RR playback without replacing its body");
    runtimeCommand[0U] = 2U;
    runtimeCommand[1U] = 0U;
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, runtimeCommand.data(), runtimeCommand.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "ordinary frame selection returns RR to frame zero before Hall paint");
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && released();
    }, "paused RR frame zero releases Store before physical paint");
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              !board.Litchcraft().IsAnimationPlaying(),
          "paused RR frame zero renders before physical piece placement");
    store::Frame priorPaint{};
    uint16_t displayedIndex = 99U;
    Check(stream.CopyDisplayed(priorPaint, displayedIndex) && displayedIndex == 0U,
          "physical RR paint starts on the confirmed displayed frame");
    Check(board.ObserveHallMask(UINT64_C(1) << 9U) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK && stream.HasUnpersistedEdit(),
          "Hall placement paints the retained RR through production input");
    store::Frame painted{};
    Check(stream.CopyDisplayed(painted, displayedIndex) &&
              !SameResidentFrame(painted, priorPaint),
          "physical paint changes visible RR pixels");
    stream.RequestImmediateSave();
    worker.NotifyLitchcraftFramePlayback();
    worker_fixture::Wait([&] {
        return !stream.HasUnpersistedEdit() && released();
    }, "physical RR edit publishes a new root then releases real Store union");
    const uint32_t editedGeneration = stream.Generation();
    const auto inspectCurrent = [&] {
        std::array<uint8_t, 9U> inspect{0x4dU, 2U, store::STORE_RESIDENTS};
        fxe::WriteU32LE(projection.session, inspect.data() + 3U);
        fxe::WriteU16LE(++request, inspect.data() + 7U);
        send(inspect.data(), inspect.size());
    };
    inspectCurrent();
    store::Frame afterEdit{};
    Check(editedGeneration > resident[1U].generation && response[13U] == 0U &&
              fxe::ReadU32LE(response.data() + 7U) == editedGeneration &&
              stream.FrameCount() == 32U && stream.CopyDisplayed(afterEdit, displayedIndex) &&
              SameResidentFrame(afterEdit, painted),
          "retained identity follows physical edit generation and preserves painted RR");
    std::printf("[RESIDENT_RELEASE_JOURNEY] Hall-square9 | mode=Litchcraft "
                "frames=32 generation=%lu dirty=0 result=accepted next=lift-piece\n",
                static_cast<unsigned long>(editedGeneration));
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait(released, "edited RR metadata read releases Store");
    Check(board.ObserveHallMask(0U) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK,
          "resident journey lifts the physical paint piece");
    const auto button = [&](const runtime::VirtualLitchPhysicalButton key,
                            const bool down) {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, key, down) ==
                  VIRTUAL_LITCH_OK && board.Advance(30U) == VIRTUAL_LITCH_OK,
              "resident mode transition uses production button debounce");
    };
    button(runtime::VirtualLitchPhysicalButton::X, true);
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "resident Exit chord allows ordinary human stagger");
    button(runtime::VirtualLitchPhysicalButton::O, true);
    Check(board.Advance(1440U) == VIRTUAL_LITCH_OK,
          "resident Exit chord reaches confirmation");
    button(runtime::VirtualLitchPhysicalButton::X, false);
    button(runtime::VirtualLitchPhysicalButton::O, false);
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() && released();
    }, "physical Exit updates selected root identity and releases Store");
    projection.active = false;
    const uint32_t exitedGeneration = stream.Generation();
    inspectCurrent();
    Check(!board.Litchcraft().IsActive() && response[13U] == 0U &&
              fxe::ReadU32LE(response.data() + 7U) == exitedGeneration &&
              exitedGeneration > editedGeneration && stream.FrameCount() == 32U &&
              stream.CopyDisplayed(afterEdit, displayedIndex) &&
              SameResidentFrame(afterEdit, painted),
          "retained identity follows mode-root generation without restoring old RR");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait(released, "inactive RR metadata releases Store");
    Check(worker.RequestLitchcraftFrameClear(), "resident journey requests explicit Clear");
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameClearState() == Worker::FrameClearState::Succeeded &&
            released();
    }, "Clear retires selected resident and releases Store");
    inspectCurrent();
    Check(response[13U] == 0U && response[15U] == 0U && stream.FrameCount() == 0U,
          "Clear discards retained resident proof and cannot resurrect RR or Balls");
    std::puts("[RESIDENT_RELEASE_JOURNEY] Exit->Clear | mode=Setup residents=0 "
              "frames=0 result=accepted next=Enter");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait(released, "cleared metadata read releases Store");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "resident release journey removes exact fixture banks");
}


modules::litchcraft_frame_store::Frame ArbitraryResidentFrame(
    const uint16_t index, const uint8_t base) {
    modules::litchcraft_frame_store::Frame frame{};
    frame.durationMs = static_cast<uint16_t>(70U + index * 35U);
    for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter) {
        auto& value = frame.emitters[emitter];
        value.color = {static_cast<uint8_t>(base + index * 11U + emitter % 27U),
                       static_cast<uint8_t>(31U + emitter % 83U),
                       static_cast<uint8_t>(5U + index * 19U)};
        value.effect = emitter % 16U == 0U ? 1U : 0U;
        value.effectDurationMs = value.effect == 0U ? 0U : 1600U;
    }
    return frame;
}

void RunArbitraryResidentReleasePlayWorkerJourney(const bool holdRecovery = false) {
    Check(::mkdir("build", 0777) == 0 || errno == EEXIST,
          "arbitrary resident standalone fixture directory exists");
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(50000U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "arbitrary resident journey starts from empty banks");
    const uint16_t authoredFrames = holdRecovery ? 8U : 3U;
    const auto authoredFrame = [&](const uint16_t index, const uint8_t base) {
        auto frame = ArbitraryResidentFrame(index, base);
        if (holdRecovery) for (auto& emitter : frame.emitters) {
            emitter.effect = 0U;
            emitter.effectDurationMs = 0U;
        }
        return frame;
    };
    store::FrameStore seed(files.StoreOps());
    Check(seed.Recover() == store::Status::NoArtwork,
          "arbitrary resident seed recovers blank Store");
    store::FrameStore::ResidentDescriptor chosen{};
    for (uint8_t document = 0U; document < 2U; ++document) {
        store::Manifest manifest{};
        manifest.frameCount = document == 0U ? authoredFrames : 5U;
        manifest.topology = document == 0U ? 2U : 1U;
        manifest.active = true;
        manifest.sparkleSeed = 0x12340U + document;
        manifest.favorites[0U] = {7U, 61U, 113U};
        store::TransactionToken token{};
        Check(seed.Begin(manifest, document, 810U + document, token) == store::Status::Ok,
              "arbitrary resident seed begins a distinct document");
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            const auto frame = authoredFrame(index, document == 0U ? 17U : 141U);
            Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
                  "arbitrary resident seed writes colors, effects and authored dwell");
        }
        Check(seed.Commit(token) == store::Status::Ok,
              "arbitrary resident seed commits a complete document");
        if (document == 0U) Check(seed.CurrentResident(chosen), "arbitrary older proof captured");
    }
    const auto paths = ResidentPhysicalPaths();
    std::vector<std::vector<uint8_t>> beforeSelection(paths.size());
    for (size_t index = 0U; index < paths.size(); ++index) {
        beforeSelection[index] = ResidentFileBytes(paths[index].c_str());
    }
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_arbitrary_resident_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "arbitrary resident loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "arbitrary resident starts real worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    const auto released = [&] {
        return worker.IsIdle() && !worker.GetDiagnostics().frameStoreStorageActive &&
            worker_fixture::atWait.load();
    };
    worker_fixture::Wait([&] { return stream.TryPresentRequested() && released(); },
                         "arbitrary newer document boot releases real Store");
    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = holdRecovery ? VIRTUAL_LITCH_HARDWARE_V3_3 : VIRTUAL_LITCH_HARDWARE_V3;
    if (holdRecovery) {
        virtualConfig.brightness_scale_256 = 256U;
        virtualConfig.max_led_current_ma = 12000U;
    }
    runtime::VirtualLitchSession board(virtualConfig);
    board.SettingsForPersistenceTest().Doc()["BOOT_DISPLAY"] = "off";
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session, const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, session, active);
        }, &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK, "arbitrary board adopts boot document");
    worker_fixture::Wait([&] { return !worker.LitchcraftFrameModeSavePending() && released(); },
                         "arbitrary board settles actual mode authority");
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "arbitrary resident opens production Store ingress");
    const uint32_t session = board.Litchcraft().SessionId();
    const store::StoreProjection projection{session, 0U, 2U, true, false};
    constexpr uint32_t OWNER = 81U;
    uint32_t ordinal = 1U;
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    const auto send = [&](const uint8_t* packet, const size_t size) {
        Check(queue.Enqueue(packet, size, ordinal++), "arbitrary Store packet admitted");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "arbitrary exact Store receipt");
    };
    std::array<uint8_t, 18U> select{0x4dU, 2U, store::STORE_SELECT};
    fxe::WriteU32LE(session, select.data() + 3U);
    fxe::WriteU16LE(1U, select.data() + 7U);
    select[9U] = chosen.rootSlot;
    fxe::WriteU32LE(chosen.generation, select.data() + 10U);
    fxe::WriteU32LE(chosen.bodyCrc32, select.data() + 14U);
    if (!holdRecovery) {
        store::Frame previous{};
        uint16_t previousIndex = UINT16_MAX;
        const uint32_t previousGeneration = stream.Generation();
        Check(stream.CopyDisplayed(previous, previousIndex) && !stream.HasUnpersistedEdit(),
              "held Show starts from clean confirmed prior artwork");
        board.Litchcraft().SetIndependentPresentation(true);
        modules::litchcraft_presentation::Inputs inputs{};
        modules::litchcraft_presentation::Renderer renderer;
        const modules::litchcraft_presentation::StreamPort presentationPort{
            &stream, [](void* context, bool playing, uint32_t modeSession, bool active, bool) {
                static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, modeSession, active);
            }, &worker, true};
        modules::BoardLedFrame pixels{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        board.Litchcraft().CapturePresentation(inputs);
        auto confirmed = renderer.Compose(inputs, presentationPort, pixels, status);
        Check(renderer.NotePresented(confirmed, presentationPort) &&
                  board.Litchcraft().ApplyPresentationFeedback(confirmed),
              "confirmed renderer establishes the prior physical Show boundary");
        worker_fixture::Wait([&] { return worker_fixture::atWait.load() && !worker_fixture::notified; },
                             "confirmed prior Show settles its ordinary worker wake");
        board.Litchcraft().CapturePresentation(inputs);
        const auto held = renderer.Compose(inputs, presentationPort, pixels, status);
        const bool heldCandidate = held.ready && stream.HasPresentationCandidate();
        const uint32_t beforePass = worker_fixture::waitEntries.load();
        Check(queue.Enqueue(select.data(), select.size(), ordinal++),
              "held Show admits the original resident Select exactly once");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        bool workerRan = false;
        {
            std::unique_lock<std::mutex> lock(worker_fixture::mutex);
            workerRan = worker_fixture::changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return worker_fixture::waitEntries.load() > beforePass &&
                    worker_fixture::atWait.load() && !worker_fixture::notified;
            });
        }
        const bool responseWhileHeld = worker.PeekLitchcraftFrameResponse(
            response.data(), response.size(), responseSize);
        const unsigned heldStatus = responseWhileHeld ? response[13U] : UINT8_MAX;
        store::Frame protectedFrame{};
        uint16_t protectedIndex = UINT16_MAX;
        bool protectedPrior = stream.Generation() == previousGeneration &&
            stream.CopyDisplayed(protectedFrame, protectedIndex) && protectedIndex == previousIndex &&
            SameResidentFrame(protectedFrame, previous);
        for (size_t index = 0U; index < paths.size(); ++index)
            protectedPrior = protectedPrior && ResidentFileBytes(paths[index].c_str()) == beforeSelection[index];
        // Release even after an observation fails; the original request owns its
        // existing worker scheduling, without another Select or body upload.
        renderer.Cancel(held, presentationPort);
        const bool releasedShow = !stream.HasPresentationCandidate();
        board.Litchcraft().SetIndependentPresentation(false);
        Check(heldCandidate && workerRan && protectedPrior && releasedShow,
              "held Show reaches the real worker and releases with prior pixels and root protected");
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "original Select completes after Show release without another request");
        const bool completed = response[13U] == 0U &&
            fxe::ReadU16LE(response.data() + 11U) == 1U &&
            fxe::ReadU32LE(response.data() + 7U) == chosen.generation;
        std::printf("[HELD_SHOW_SELECT] request=1 enqueued=1 held=%u terminal_while_held=%u held_status=%u "
                    "prior_generation=%lu wanted_generation=%lu prior_protected=%u released=%u "
                    "after_release_status=%u completed=%u resubmits=0 body_packets=0 Show=controlled\n",
            unsigned(heldCandidate), unsigned(responseWhileHeld), heldStatus,
            static_cast<unsigned long>(previousGeneration), static_cast<unsigned long>(chosen.generation),
            unsigned(protectedPrior), unsigned(releasedShow), unsigned(response[13U]), unsigned(completed));
        Check(completed, "one resident Select must complete after transient Show release instead of terminal ResourceLimit");
        if (!completed) {
            worker.AcceptLitchcraftFrameResponse();
            {
                std::lock_guard<std::mutex> lock(worker_fixture::mutex);
                worker_fixture::stopping = true; worker_fixture::changed.notify_all();
            }
            worker_fixture::worker.join();
            Check(files.Remove(), "failed held-Show Select removes isolated fixture banks");
            return;
        }
    } else {
        send(select.data(), select.size());
    }
    Check(response[13U] == 0U, "arbitrary older resident selection accepted");
    worker.AcceptLitchcraftFrameResponse();
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK,
          "arbitrary renderer adopts selected older authority");
    worker_fixture::Wait([&] { return stream.TryPresentRequested() && released(); },
                         "arbitrary selected resident destroys actual Store union");
    std::array<uint8_t, 10U> inspect{0x4dU, 2U, 1U};
    fxe::WriteU32LE(session, inspect.data() + 3U);
    fxe::WriteU16LE(2U, inspect.data() + 7U);
    inspect[9U] = store::RESIDENT_LISTING_PROBE;
    send(inspect.data(), inspect.size());
    Check(responseSize >= 66U && response[13U] == 0U &&
              response[19U] == chosen.rootSlot &&
              fxe::ReadU32LE(response.data() + 7U) == chosen.generation &&
              fxe::ReadU16LE(response.data() + 15U) == authoredFrames &&
              fxe::ReadU32LE(response.data() + 26U) == chosen.bodyCrc32 &&
              stream.Topology() == 2U,
          "Inspect after real release proves exact arbitrary digest, shape and topology");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait(released, "arbitrary Inspect releases Store before Play");
    if (holdRecovery) {
        constexpr uint32_t settleMs = modules::litchcraft_physical_presentation::
            ModeCrossfade<hardware::MAX_PHYSICAL_LED_COUNT>::DURATION_MS;
        Check(board.Advance(settleMs) == VIRTUAL_LITCH_OK &&
                  !board.Litchcraft().IsAnimationPlaying(),
              "paused Craft mode crossfade settles before the Play recovery interval");
        worker_fixture::Wait(released, "paused settled canvas releases Store before ordered Play");
    }
    std::array<uint8_t, 6U> play{1U, 0U};
    fxe::WriteU32LE(chosen.generation, play.data() + 2U);
    struct RecoveryHold {
        hardware::ArtworkTestArbiter& arbiter;
        bool armed;
        RecoveryHold(hardware::ArtworkTestArbiter& value, const bool active)
            : arbiter(value), armed(active) { if (armed) arbiter.BlockNextFlash(); }
        void Release() { if (armed) { arbiter.ReleaseBlockedFlash(); armed = false; } }
        ~RecoveryHold() { Release(); }
    } hold(arbiter, holdRecovery);
    const uint32_t presentationsBeforePlay = stream.PresentationCount();
    Check(board.Litchcraft().ApplyConnectionOrdered(2U, play.data(), play.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "ordinary ordered Play accepts arbitrary older resident");
    if (holdRecovery) {
        Check(arbiter.WaitForBlockedFlashFor(std::chrono::seconds(2)),
              "Play reaches held real Store recovery access without pre-render reconstruction wait");
        constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3_3S3);
        struct BoardObservation {
            std::vector<uint8_t> semanticRgb, producedRgbw;
            uint32_t semanticCrc = 0U, producedCrc = 0U, boardEnergy = 0U;
            uint16_t index = UINT16_MAX;
        };
        std::optional<BoardObservation> previousOutput, heldFrameOne;
        const auto sample = [&](const uint32_t elapsed, const bool blocked) {
            store::Frame visible{};
            uint16_t index = UINT16_MAX;
            Check(stream.CopyDisplayed(visible, index) && index < authoredFrames &&
                      SameResidentFrame(visible, authoredFrame(index, 17U)),
                  "startup render retains one complete authored frame while recovery is delayed");
            Check(board.SemanticFrame().board[9U][0U].r == visible.emitters[36U].color.r &&
                      board.SemanticFrame().board[9U][0U].g == visible.emitters[36U].color.g &&
                      board.SemanticFrame().board[9U][0U].b == visible.emitters[36U].color.b,
                  "startup observation includes actual rendered authored pixels");
            BoardObservation output;
            output.index = index;
            for (const auto& square : board.SemanticFrame().board)
                for (const auto& pixel : square) {
                    output.semanticRgb.push_back(pixel.r);
                    output.semanticRgb.push_back(pixel.g);
                    output.semanticRgb.push_back(pixel.b);
                }
            const auto& produced = board.PhysicalFrame();
            Check(produced.led_count == topology.physicalBoardLedCount + topology.physicalStatusLedCount &&
                      produced.channels_per_led == 3U,
                  "V3.3 produced output has its actual RGB board and control topology");
            // Board LEDs precede the timed control/status chain. Capture explicit
            // channel bytes from the fitted output, not the whole-frame hash.
            for (uint16_t led = 0U; led < topology.physicalBoardLedCount; ++led) {
                const auto pixel = produced.pixels[led];
                Check(pixel.w == 0U, "V3.3 RGB output has no invented white die in its RGBW carrier");
                for (const uint8_t channel : {pixel.r, pixel.g, pixel.b, pixel.w}) {
                    output.producedRgbw.push_back(channel);
                    output.boardEnergy += channel;
                }
            }
            output.semanticCrc = fxe::Crc32(output.semanticRgb.data(), output.semanticRgb.size());
            output.producedCrc = fxe::Crc32(output.producedRgbw.data(), output.producedRgbw.size());
            Check(std::any_of(output.semanticRgb.begin(), output.semanticRgb.end(),
                             [](const uint8_t value) { return value != 0U; }) && output.boardEnergy != 0U,
                  "authored semantic board and fitted produced board are nonblank");
            if (previousOutput && previousOutput->index != output.index) {
                Check(output.semanticRgb != previousOutput->semanticRgb &&
                          output.semanticCrc != previousOutput->semanticCrc &&
                          output.producedRgbw != previousOutput->producedRgbw &&
                          output.producedCrc != previousOutput->producedCrc,
                      "authored frame changes alter whole semantic and fitted board output");
            }
            if (blocked && elapsed == 70U) {
                Check(index == 1U, "startup reaches authored frame one before the delayed miss");
                heldFrameOne = output;
            }
            if (blocked && elapsed >= 175U) {
                Check(index == 1U && stream.RequestedIndex() == 2U && heldFrameOne &&
                          output.semanticRgb == heldFrameOne->semanticRgb &&
                          output.semanticCrc == heldFrameOne->semanticCrc &&
                          output.producedRgbw == heldFrameOne->producedRgbw &&
                          output.producedCrc == heldFrameOne->producedCrc &&
                          output.boardEnergy == heldFrameOne->boardEnergy,
                      "every held deadline preserves the entire semantic and produced board");
            }
            if (!blocked && heldFrameOne) {
                Check(index != 1U && output.semanticRgb != heldFrameOne->semanticRgb &&
                          output.semanticCrc != heldFrameOne->semanticCrc &&
                          output.producedRgbw != heldFrameOne->producedRgbw &&
                          output.producedCrc != heldFrameOne->producedCrc,
                      "actual recovery visibly changes both whole-board produced representations");
            }
            std::printf("[PLAY_STARTUP] t=%lu recovery_held=%u requested=%u displayed=%u "
                        "selected=%u semantic_rgb_crc=%08lx fitted_board_rgbw_carrier_crc=%08lx "
                        "fitted_board_energy=%lu board_leds=%u left9=%u,%u,%u presentations=%lu\n",
                static_cast<unsigned long>(elapsed), unsigned(blocked), stream.RequestedIndex(), index,
                board.Litchcraft().SelectedFrame(), static_cast<unsigned long>(output.semanticCrc),
                static_cast<unsigned long>(output.producedCrc), static_cast<unsigned long>(output.boardEnergy),
                topology.physicalBoardLedCount, board.SemanticFrame().board[9U][0U].r,
                board.SemanticFrame().board[9U][0U].g, board.SemanticFrame().board[9U][0U].b,
                static_cast<unsigned long>(stream.PresentationCount() - presentationsBeforePlay));
            previousOutput = std::move(output);
            return index;
        };
        std::printf("[PLAY_STARTUP_INPUT] generation=%lu frames=%u topology=%u body_crc=%08lx "
                    "cache_slots=%u hardware=V3.3 boot_display=off model_hold_ms=600 "
                    "brightness_scale=256 current_limit_ma=12000 channels=RGB carrier=RGBW "
                    "paused_mode_crossfade=settled output=produced_not_sdk_delivery\n",
            static_cast<unsigned long>(chosen.generation), authoredFrames, stream.Topology(),
            static_cast<unsigned long>(chosen.bodyCrc32), unsigned(stream.CACHE_SLOTS));
        constexpr uint32_t deadlines[] = {0U, 70U, 175U, 315U, 490U, 600U};
        uint32_t previous = 0U;
        uint16_t first = UINT16_MAX, last = UINT16_MAX;
        for (const auto elapsed : deadlines) {
            Check(board.Advance(elapsed - previous) == VIRTUAL_LITCH_OK &&
                      arbiter.IsFlashBlocked() && board.Litchcraft().IsAnimationPlaying(),
                  "production rendering advances through authored deadlines before worker release");
            last = sample(elapsed, true);
            if (first == UINT16_MAX) first = last;
            previous = elapsed;
        }
        const uint16_t missedRequest = stream.RequestedIndex();
        Check(first != last && missedRequest == last + 1U,
              "playback visibly starts before held recovery exhausts retained authored frames");
        hold.Release();
        worker_fixture::Wait([&] {
            return worker.GetDiagnostics().frameStoreStorageActive && worker_fixture::atWait.load();
        }, "released real recovery finishes after the observed startup interval");
        Check(board.Advance(0U) == VIRTUAL_LITCH_OK,
              "render observes recovery completion without advancing away its timing");
        const uint16_t recovered = sample(600U, false);
        Check(recovered >= missedRequest && recovered < authoredFrames,
              "recovery presents pending authored content under the existing catch-up policy");
        // Observe catch-up at the same virtual instant; only after each actual
        // render let the normal worker consume its existing wake/prefetch work.
        for (unsigned step = 0U; step < 3U; ++step) {
            worker_fixture::Wait([&] {
                return worker_fixture::atWait.load() && !worker_fixture::notified;
            }, "ordinary recovery/prefetch work completes after observed render");
            Check(board.Advance(0U) == VIRTUAL_LITCH_OK, "recovery samples current physical pixels");
            const auto settled = sample(previous, false);
            if (settled >= authoredFrames) break;
            uint32_t nextBoundary = 0U;
            for (uint16_t frame = 0U; frame <= settled; ++frame)
                nextBoundary += authoredFrame(frame, 17U).durationMs;
            Check(nextBoundary > previous, "recovered authored deadline lies ahead of observed time");
            if (nextBoundary <= previous) break;
            Check(board.Advance(nextBoundary - previous) == VIRTUAL_LITCH_OK,
                  "recovered playback reaches the next authored deadline");
            previous = nextBoundary;
            const auto after = sample(previous, false);
            Check(after == (settled + 1U) % authoredFrames,
                  "normal prefetched worker resumes actual authored pixels");
        }
        std::printf("[PLAY_STARTUP_RESULT] first=%u held_last=%u pending=%u recovered=%u "
                    "failures=%d scope=controlled_delay_not_physical_latency\n",
            first, last, missedRequest, recovered, failures);
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::stopping = true; worker_fixture::changed.notify_all();
        }
        worker_fixture::worker.join();
        Check(files.Remove(), "startup characterization removes isolated Store files");
        return;
    }
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreStorageActive && worker_fixture::atWait.load();
    }, "arbitrary Play reconstructs production Store");
    Check(board.Advance(70U) == VIRTUAL_LITCH_OK,
          "arbitrary Play reaches authored seventy millisecond boundary");
    worker_fixture::Wait([&] {
        return stream.RequestedIndex() == 1U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "arbitrary Play pages and presents next exact frame");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Litchcraft().IsAnimationPlaying() &&
              stream.Generation() == chosen.generation && stream.FrameCount() == 3U &&
              stream.PresentationCount() > presentationsBeforePlay &&
              stream.CopyDisplayed(displayed, displayedIndex) && displayedIndex == 1U &&
              SameResidentFrame(displayed, ArbitraryResidentFrame(1U, 17U)) &&
              board.SemanticFrame().board[9U][0U].r == displayed.emitters[36U].color.r,
          "arbitrary Play preserves duration/effects and renders exact generated frame");
    Check(ResidentPhysicalPaths() == paths, "arbitrary selection preserves lazy part presence as well as bytes");
    for (size_t index = 0U; index < paths.size(); ++index) {
        Check(ResidentFileBytes(paths[index].c_str()) == beforeSelection[index],
              "arbitrary select/release/Inspect/Play writes no root or body bytes");
    }
    std::printf("[ARBITRARY_RESIDENT_JOURNEY] generation=%lu frames=%u topology=%u "
                "playing=%u displayed=%u duration=%u body_crc=%08lx next=Pause\n",
                static_cast<unsigned long>(stream.Generation()), stream.FrameCount(), stream.Topology(),
                board.Litchcraft().IsAnimationPlaying() ? 1U : 0U, displayedIndex, displayed.durationMs,
                static_cast<unsigned long>(chosen.bodyCrc32));
    play[0U] = 0U; play[1U] = 1U;
    Check(board.Litchcraft().ApplyConnectionOrdered(2U, play.data(), play.size()) ==
              modules::LitchcraftSession::Status::Ok,
          "held-head retirement pauses through the existing ordered command");
    worker_fixture::Wait(released, "paused prior resident releases Store before admission-race check");
    // The same held-head seam must retire original intent on Cancel, owner
    // replacement and Exit. These follow the original full Play journey.
    store::FrameStore::ResidentDescriptor otherResident{};
    Check(seed.CurrentResident(otherResident), "held-head retirement has the other saved resident proof");
    board.Litchcraft().SetIndependentPresentation(true);
    modules::litchcraft_presentation::Renderer retirementRenderer;
    modules::litchcraft_presentation::Inputs retirementInputs{};
    modules::BoardLedFrame retirementPixels{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> retirementStatus{};
    const modules::litchcraft_presentation::StreamPort retirementPort{
        &stream, [](void* context, bool playing, uint32_t modeSession, bool active, bool) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, modeSession, active);
        }, &worker, true};
    board.Litchcraft().CapturePresentation(retirementInputs);
    auto retirementShow = retirementRenderer.Compose(
        retirementInputs, retirementPort, retirementPixels, retirementStatus);
    Check(retirementRenderer.NotePresented(retirementShow, retirementPort) &&
              board.Litchcraft().ApplyPresentationFeedback(retirementShow),
          "held-head retirement confirms actual prior renderer output");
    for (uint16_t retirement = 0U; retirement < 3U; ++retirement) {
        worker_fixture::Wait([&] { return worker_fixture::atWait.load() && !worker_fixture::notified; });
        board.Litchcraft().CapturePresentation(retirementInputs);
        retirementShow = retirementRenderer.Compose(
            retirementInputs, retirementPort, retirementPixels, retirementStatus);
        const bool held = retirementShow.ready && stream.HasPresentationCandidate();
        const uint32_t prior = stream.Generation();
        select[9U] = otherResident.rootSlot;
        fxe::WriteU32LE(otherResident.generation, select.data() + 10U);
        fxe::WriteU32LE(otherResident.bodyCrc32, select.data() + 14U);
        const uint16_t retiredRequest = 40U + retirement;
        fxe::WriteU16LE(retiredRequest, select.data() + 7U);
        const uint32_t beforePass = worker_fixture::waitEntries.load();
        if (retirement == 0U) arbiter.BlockNextFlash();
        Check(queue.Enqueue(select.data(), select.size(), ordinal++), "retirement queues one original Select");
        worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
        bool workerRan = false;
        if (retirement == 0U) {
            workerRan = arbiter.WaitForBlockedFlashFor(std::chrono::seconds(2));
        } else {
            std::unique_lock<std::mutex> lock(worker_fixture::mutex);
            workerRan = worker_fixture::changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return worker_fixture::waitEntries.load() > beforePass &&
                    worker_fixture::atWait.load() && !worker_fixture::notified;
            });
        }
        const bool pending = !worker.PeekLitchcraftFrameResponse(
            response.data(), response.size(), responseSize) && queue.TryPeekLitchcraftFrameStore();
        if (retirement == 0U) {
            std::array<uint8_t, 9U> cancel{0x4dU, 2U, store::STORE_CANCEL};
            fxe::WriteU32LE(session, cancel.data() + 3U);
            fxe::WriteU16LE(retiredRequest, cancel.data() + 7U);
            worker.NotifyLitchcraftFramePriority(cancel.data(), cancel.size(), OWNER);
            // Cancel is already published before the first Stage admission.
            arbiter.ReleaseBlockedFlash();
        } else if (retirement == 1U) {
            worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER + 1U);
        } else {
            board.Litchcraft().Exit();
        }
        // Always retire the controlled Show, including after a failed check.
        retirementRenderer.Cancel(retirementShow, retirementPort);
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "retired held Select reaches its original terminal response");
        const unsigned expectedStatus = retirement == 0U ? 0U : 2U;
        const bool retired = response[13U] == expectedStatus &&
            fxe::ReadU16LE(response.data() + 11U) == retiredRequest &&
            !queue.TryPeekLitchcraftFrameStore() && stream.FrameCount() == authoredFrames &&
            stream.Topology() == 2U && (retirement == 2U || stream.Generation() == prior);
        Check(held && workerRan && pending && retired,
              "Cancel, owner replacement and Exit retire held Select without later adoption");
        std::printf("[HELD_SHOW_SELECT_RETIRE] trigger=%s request=%u pending=%u status=%u retired=%u resubmits=0 Show=controlled\n",
            retirement == 0U ? "Cancel" : retirement == 1U ? "Owner" : "Exit",
            retiredRequest, unsigned(pending), unsigned(response[13U]), unsigned(retired));
        worker.AcceptLitchcraftFrameResponse();
    }
    board.Litchcraft().SetIndependentPresentation(false);
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "arbitrary resident removes isolated fixture banks");
}


uint32_t PlaybackProbeRgbCrc(const modules::litchcraft_frame_store::Frame& frame) {
    std::array<uint8_t, 256U * 3U> rgb{};
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        rgb[index * 3U] = frame.emitters[index].color.r;
        rgb[index * 3U + 1U] = frame.emitters[index].color.g;
        rgb[index * 3U + 2U] = frame.emitters[index].color.b;
    }
    return fxe::Crc32(rgb.data(), rgb.size());
}

uint32_t H3BoardRgbCrc(const modules::BoardLedFrame& board) {
    std::array<uint8_t, 256U * 3U> bytes{};
    size_t cursor = 0U;
    for (const auto& square : board) {
        for (const auto color : {square.left, square.top, square.right, square.bottom}) {
            bytes[cursor++] = color.r;
            bytes[cursor++] = color.g;
            bytes[cursor++] = color.b;
        }
    }
    return fxe::Crc32(bytes.data(), bytes.size());
}

void RunStructuralComposeCoherenceProbe() {
    std::printf("[H3_SNAPSHOT_COST] frameBytes=%zu emitterBytes=%zu composedFrameBytes=%zu\n",
        sizeof(modules::litchcraft_frame_store::Frame), sizeof(modules::litchcraft_frame_store::Emitter),
        sizeof(modules::litchcraft_animation::ComposedFrame));
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using Status = modules::LitchcraftSession::Status;
    namespace store = modules::litchcraft_frame_store;
    fxe::ManualMillis::SetNowMs(0U);
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    worker_fixture::waitEntries.store(0U);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "H3 resets isolated production files");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 3U;
    manifest.active = true;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "H3 starts actual three-frame Store");
    std::array<store::Frame, 3U> corpus{};
    for (uint16_t index = 0U; index < corpus.size(); ++index) {
        auto& frame = corpus[index];
        frame.durationMs = 100U;
        for (size_t emitter = 0U; emitter < frame.emitters.size(); ++emitter) {
            frame.emitters[emitter].color = {
                static_cast<uint8_t>(20U + index * 60U + emitter / 8U),
                static_cast<uint8_t>(30U + (emitter % 4U) * 40U),
                static_cast<uint8_t>(1U + emitter % 127U)};
        }
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "H3 writes distinct complete four-direction frames");
    }
    Check(seed.Commit(token) == store::Status::Ok, "H3 commits corpus before observation");
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_litchcraft_h3_probe.json";
    fxe::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "H3 loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(settingsConfig);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "H3 starts actual S3 settings worker thread");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == 3U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load() && !worker_fixture::notified;
    }, "H3 recovers and presents frame zero");
    const auto pauseWorker = [&] {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    };
    const auto resumeWorker = [&] {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    };
    pauseWorker();
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(&stream,
        [](void* context, const bool playing, const uint32_t session, const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, session, active);
        }, &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Advance(1000U) == VIRTUAL_LITCH_OK,
          "H3 adopts session and settles entry");
    const uint8_t seek[] = {2U, 1U, 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(2U, seek, sizeof(seek), 101U) == Status::Ok,
          "H3 selects middle frame through ordered runtime control");
    resumeWorker();
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && stream.DisplayedIndex() == 1U &&
            worker_fixture::atWait.load() && !worker_fixture::notified;
    }, "H3 production worker loads selected frame");
    pauseWorker();
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK, "H3 session samples selected frame before history");
    const uint8_t erase[] = {1U, 1U, 0U, 0U, 0U};
    constexpr uint8_t opcodes[] = {10U, 6U, 7U};
    constexpr const char* labels[] = {"Delete", "Undo", "Redo"};
    LitchcraftSetComposeSquareCheckpointForTest(&LitchcraftH3AfterSquare);
    for (size_t step = 0U; step < 3U; ++step) {
        const uint8_t* payload = step == 0U ? erase : nullptr;
        const size_t payloadSize = step == 0U ? sizeof(erase) : 0U;
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(stream.Generation(), guard.data());
        fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
        Check(board.Litchcraft().ApplyConnectionOrdered(8U, guard.data(), guard.size(), 101U) == Status::Ok,
              "H3 guards actual structural operation");
        store::Frame before{};
        uint16_t beforeIndex = UINT16_MAX;
        Check(stream.CopyDisplayed(before, beforeIndex), "H3 captures full old RAM frame");
        const uint32_t beforeGeneration = stream.Generation();
        {
            std::lock_guard<std::mutex> lock(h3_compose_probe::mutex);
            h3_compose_probe::armed = true;
            h3_compose_probe::held = false;
            h3_compose_probe::released = false;
        }
        modules::BoardLedFrame composed{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        std::thread renderer([&] { board.Litchcraft().Compose(composed, status); });
        {
            std::unique_lock<std::mutex> lock(h3_compose_probe::mutex);
            if (!h3_compose_probe::changed.wait_for(lock, std::chrono::milliseconds(500), [] {
                    return h3_compose_probe::held;
                })) {
                std::printf("[H3_PROBE_TIMEOUT] %s Compose did not reach square31\n", labels[step]);
                std::abort();
            }
        }
        store::Frame protectedBefore{};
        uint16_t protectedIndex = UINT16_MAX;
        Check(stream.CopyDisplayed(protectedBefore, protectedIndex) &&
                  PlaybackProbeRgbCrc(protectedBefore) == PlaybackProbeRgbCrc(before),
              "H3 held Compose begins from one complete old frame");
        arbiter.BlockNextFlash();
        Check(board.Litchcraft().ApplyConnectionOrdered(opcodes[step], payload, payloadSize, 101U) == Status::Ok &&
                  stream.Generation() == beforeGeneration && stream.HasUnpersistedEdit(),
              "H3 guarded structural history is Applied in RAM before storage service");
        store::Frame accepted{};
        uint16_t acceptedIndex = UINT16_MAX;
        Check(stream.CopyDisplayed(accepted, acceptedIndex) &&
                  PlaybackProbeRgbCrc(accepted) == PlaybackProbeRgbCrc(corpus[step == 1U ? 1U : 0U]),
              "H3 RAM publication selects the complete expected new frame");
        resumeWorker();
        if (!arbiter.WaitForBlockedFlashFor(std::chrono::milliseconds(500))) {
            std::printf("[H3_PROBE_TIMEOUT] %s worker did not reach first production access\n", labels[step]);
            std::abort();
        }
        Check(stream.Generation() == beforeGeneration,
              "H3 storage remains unpublished after the ordered live receipt");
        const uint32_t beforePass = worker_fixture::waitEntries.load();
        arbiter.ReleaseBlockedFlash();
        {
            std::unique_lock<std::mutex> lock(worker_fixture::mutex);
            if (!worker_fixture::changed.wait_for(lock, std::chrono::milliseconds(500), [&] {
                    return stream.Generation() == beforeGeneration + 1U &&
                        worker_fixture::waitEntries.load() > beforePass &&
                        worker_fixture::atWait.load() && !worker_fixture::notified;
                })) {
                std::printf("[H3_PROBE_TIMEOUT] %s actual structural publication did not complete\n", labels[step]);
                std::abort();
            }
            worker_fixture::paused = true;
        }
        store::Frame after{};
        uint16_t afterIndex = UINT16_MAX;
        Check(stream.CopyDisplayed(after, afterIndex), "H3 captures full new published RAM frame");
        const uint32_t oldCrc = PlaybackProbeRgbCrc(before);
        const uint32_t newCrc = PlaybackProbeRgbCrc(after);
        Check(oldCrc != newCrc && newCrc == PlaybackProbeRgbCrc(corpus[step == 1U ? 1U : 0U]),
              "H3 publication has a distinct complete expected new frame");
        {
            std::lock_guard<std::mutex> lock(h3_compose_probe::mutex);
            h3_compose_probe::released = true;
            h3_compose_probe::changed.notify_all();
        }
        renderer.join();
        const uint32_t actualCrc = H3BoardRgbCrc(composed);
        size_t oldEmitters = 0U, newEmitters = 0U, unexpectedEmitters = 0U;
        for (size_t square = 0U; square < composed.size(); ++square) {
            const auto& leds = composed[square];
            const std::array<modules::Rgb, 4U> colors{{leds.left, leds.top, leds.right, leds.bottom}};
            for (size_t direction = 0U; direction < colors.size(); ++direction) {
                const auto color = colors[direction];
                const auto old = before.emitters[square * 4U + direction].color;
                const auto fresh = after.emitters[square * 4U + direction].color;
                const bool isOld = color.r == old.r && color.g == old.g && color.b == old.b;
                const bool isNew = color.r == fresh.r && color.g == fresh.g && color.b == fresh.b;
                oldEmitters += isOld ? 1U : 0U;
                newEmitters += isNew ? 1U : 0U;
                unexpectedEmitters += !isOld && !isNew ? 1U : 0U;
                Check(isOld || isNew,
                      "H3 every direction matches a valid old or new source emitter");
            }
        }
        std::printf("[H3_COHERENCE] action=%s gen=%lu->%lu frame=%u->%u oldCRC=%08lx newCRC=%08lx actualCRC=%08lx oldEmitters=%zu newEmitters=%zu unexpected=%zu\n",
            labels[step], static_cast<unsigned long>(beforeGeneration), static_cast<unsigned long>(stream.Generation()),
            beforeIndex, afterIndex, static_cast<unsigned long>(oldCrc), static_cast<unsigned long>(newCrc),
            static_cast<unsigned long>(actualCrc), oldEmitters, newEmitters, unexpectedEmitters);
        Check(actualCrc == oldCrc && oldEmitters == 256U && newEmitters == 0U,
              "H3 in-flight Compose holds one complete old frame through RAM publication and real worker save");
        modules::BoardLedFrame settled{};
        board.Litchcraft().Compose(settled, status);
        Check(H3BoardRgbCrc(settled) == newCrc,
              "H3 next uninterrupted Compose publishes the exact complete new frame");
    }
    const auto sameFrame = [](const store::Frame& left, const store::Frame& right) {
        if (left.durationMs != right.durationMs) return false;
        for (size_t index = 0U; index < left.emitters.size(); ++index) {
            const auto& a = left.emitters[index];
            const auto& b = right.emitters[index];
            if (a.color.r != b.color.r || a.color.g != b.color.g || a.color.b != b.color.b ||
                a.effect != b.effect || a.effectDurationMs != b.effectDurationMs) return false;
        }
        return true;
    };
    const auto guarded = [&](const uint8_t operation, const uint8_t* payload, size_t size) {
        std::array<uint8_t, 8U> guard{};
        fxe::WriteU32LE(stream.Generation(), guard.data());
        fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
        return board.Litchcraft().ApplyConnectionOrdered(8U, guard.data(), guard.size(), 101U) == Status::Ok &&
            board.Litchcraft().ApplyConnectionOrdered(operation, payload, size, 101U) == Status::Ok;
    };
    const auto select = [&](const uint8_t index) {
        std::array<uint8_t, 6U> payload{2U, index};
        fxe::WriteU32LE(stream.Generation(), payload.data() + 2U);
        return board.Litchcraft().ApplyConnectionOrdered(2U, payload.data(), payload.size(), 101U) == Status::Ok &&
            board.Advance(0U) == VIRTUAL_LITCH_OK && stream.DisplayedIndex() == index;
    };
    store::Frame acceptedCopy{};
    uint16_t sourceIndex = UINT16_MAX;
    Check(stream.CopyDisplayed(acceptedCopy, sourceIndex) && sourceIndex == 0U,
          "COW real-worker journey starts from the clean original frame");
    const auto leasesBeforeCopies = arbiter.acquisitions;
    const uint8_t addA[] = {0U, 0U, 1U, 0U, 0U};
    const uint8_t addB[] = {0U, 1U, 2U, 0U, 0U};
    Check(guarded(10U, addA, sizeof(addA)) && guarded(10U, addB, sizeof(addB)),
          "COW real guarded Add A then Add B are Applied while the actual worker is paused");
    Check(select(1U), "COW reselects earlier copy A through runtime control");
    const uint8_t paintA[] = {1U, 37U, 0U, 3U, 15U, 201U, 7U, 42U, 9U};
    Check(guarded(3U, paintA, sizeof(paintA)), "COW later pointer paint changes only earlier copy A");
    auto expectedA = acceptedCopy;
    for (size_t slot = 36U; slot < 40U; ++slot) expectedA.emitters[slot].color = {201U, 7U, 42U};
    store::Frame liveA{}, liveB{};
    uint16_t liveIndex = UINT16_MAX;
    Check(stream.CopyDisplayed(liveA, liveIndex) && liveIndex == 1U && sameFrame(liveA, expectedA),
          "COW live A retains its exact later paint and every frame field");
    Check(select(2U) && stream.CopyDisplayed(liveB, liveIndex) && liveIndex == 2U && sameFrame(liveB, acceptedCopy),
          "COW live B remains the earlier immutable full-frame snapshot");
    Check(arbiter.acquisitions == leasesBeforeCopies && stream.HasUnpersistedEdit(),
          "COW acceptance and source repaint occurred without servicing the paused storage worker");
    std::puts("[COW_SNAPSHOT] stage=held-worker AddA=Applied AddB=Applied paintA=Applied liveA=exact liveB=unchanged next=save-and-reopen");

    const uint8_t addC[] = {0U, 2U, 3U, 0U, 0U};
    Check(guarded(10U, addC, sizeof(addC)) && select(1U),
          "pressure accepts third pinned copy and selects oldest A before its save");
    struct SaveBoundary final {
        std::mutex mutex;
        std::condition_variable changed;
        bool armed = false;
        bool held = false;
        bool released = false;
        void Arm() {
            std::lock_guard<std::mutex> lock(mutex);
            armed = true; held = false; released = false;
        }
        void WaitHeld() {
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return held; })) std::abort();
        }
        void Release() {
            std::lock_guard<std::mutex> lock(mutex);
            released = true; changed.notify_all();
        }
        static void At(void* context, store::FrameStore::TimingPhase phase, uint32_t) {
            auto& self = *static_cast<SaveBoundary*>(context);
            std::unique_lock<std::mutex> lock(self.mutex);
            if (!self.armed || phase != store::FrameStore::TimingPhase::BodyPrepare) return;
            self.armed = false; self.held = true;
            {
                std::lock_guard<std::mutex> fixtureLock(worker_fixture::mutex);
                worker_fixture::paused = true;
            }
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.released; });
        }
    } boundary;
    worker.SetLitchcraftFrameTimingTestHook(&SaveBoundary::At, &boundary);
    boundary.Arm();
    const auto beforeSaveGeneration = stream.Generation();
    resumeWorker(); boundary.WaitHeld();
    // BodyPrepare is reported after InsertFrame and its verified staging write:
    // this is a later RAM edit, not a changed pre-save snapshot.
    const uint8_t paintDuringA[] = {1U, 38U, 0U, 3U, 15U, 4U, 231U, 71U, 12U};
    Check(guarded(3U, paintDuringA, sizeof(paintDuringA)),
          "real worker holds captured A payload while later A paint is accepted");
    for (size_t slot = 48U; slot < 52U; ++slot) expectedA.emitters[slot].color = {4U, 231U, 71U};
    Check(select(3U), "pressure returns Timeline to third copied frame before fourth Add");
    const uint8_t addD[] = {0U, 3U, 4U, 0U, 0U};
    std::array<uint8_t, 8U> pressureGuard{};
    fxe::WriteU32LE(stream.Generation(), pressureGuard.data());
    fxe::WriteU32LE(stream.LiveRevision(), pressureGuard.data() + 4U);
    Check(board.Litchcraft().ApplyConnectionOrdered(8U, pressureGuard.data(), pressureGuard.size(), 101U) == Status::Ok,
          "pressure guards the exact fourth Add predecessor");
    const auto pressureRevision = stream.LiveRevision();
    Check(board.Litchcraft().ApplyConnectionOrdered(10U, addD, sizeof(addD), 101U) == Status::Busy &&
              stream.FrameCount() == 5U && stream.LiveRevision() == pressureRevision,
          "fourth Add is retained Pending with no partial frame count or revision");
    boundary.Release();
    worker_fixture::Wait([&] {
        return stream.Generation() == beforeSaveGeneration + 1U && worker_fixture::atWait.load();
    }, "pressure first captured A save completes at real worker wait");
    Check(board.Litchcraft().ApplyConnectionOrdered(10U, addD, sizeof(addD), 101U) == Status::Busy &&
              stream.FrameCount() == 5U && stream.LiveRevision() == pressureRevision,
          "A remains dirty after its captured save, so it cannot free a slot for fourth Add");
    boundary.Arm(); resumeWorker(); boundary.WaitHeld();
    Check(stream.FrameCount() == 5U && stream.LiveRevision() == pressureRevision,
          "fourth Add remains unapplied until the second pinned copy B saves");
    boundary.Release();
    worker_fixture::Wait([&] {
        return stream.Generation() == beforeSaveGeneration + 2U && worker_fixture::atWait.load();
    }, "pressure second copy B actually publishes");
    worker.SetLitchcraftFrameTimingTestHook(nullptr, nullptr);
    resumeWorker();
    std::array<store::Frame, 8U> expectedCopies{};
    expectedCopies.fill(acceptedCopy);
    expectedCopies[0U] = expectedA;
    for (uint8_t copy = 4U; copy <= 8U; ++copy) {
        const uint8_t add[] = {0U, static_cast<uint8_t>(copy - 1U), copy, 0U, 0U};
        auto status = Status::Busy;
        if (copy != 4U) {
            fxe::WriteU32LE(stream.Generation(), pressureGuard.data());
            fxe::WriteU32LE(stream.LiveRevision(), pressureGuard.data() + 4U);
            Check(board.Litchcraft().ApplyConnectionOrdered(8U, pressureGuard.data(), pressureGuard.size(), 101U) == Status::Ok,
                  "pressure retains exact guard for each subsequent Add intent");
        }
        for (uint16_t attempt = 0U; attempt < 3000U && status == Status::Busy; ++attempt) {
            status = board.Litchcraft().ApplyConnectionOrdered(10U, add, sizeof(add), 101U);
            if (status == Status::Busy) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(status == Status::Ok && stream.FrameCount() == 2U + copy && select(copy),
              "all eight real-worker Add intents eventually apply once and in order");
        expectedCopies[copy - 1U] = expectedCopies[copy - 2U];
        const uint8_t paint[] = {copy, static_cast<uint8_t>(40U + copy), 0U, 3U, 15U, copy, 81U, 192U, copy};
        Check(guarded(3U, paint, sizeof(paint)), "post-pressure paint stays attached to its accepted new frame");
        for (size_t slot = copy * 4U; slot < copy * 4U + 4U; ++slot) {
            expectedCopies[copy - 1U].emitters[slot].color = {copy, 81U, 192U};
        }
        store::Frame live{}; uint16_t index = UINT16_MAX;
        Check(stream.CopyDisplayed(live, index) && index == copy && sameFrame(live, expectedCopies[copy - 1U]),
              "each drained copied frame has exact live RGB effects and durations");
    }
    std::puts("[COW_PRESSURE] total_Add=8 first3=Applied fourth=Pending in_save_A_mutation=accepted first_A_save_keeps_dirty=1 B_save_required=1 ordered=exact live=all_fields");
    resumeWorker();
    worker_fixture::Wait([&] {
        return !stream.HasUnpersistedEdit() && worker_fixture::atWait.load() && !worker_fixture::notified;
    }, "COW actual worker saves both accepted copies and later A paint");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    firmware::LitchcraftFrameFile reopenedFiles;
    store::FrameStore reopened(reopenedFiles.StoreOps());
    Check(reopened.Recover() == store::Status::Ok && reopened.CurrentManifest().frameCount == 10U,
          "COW fresh file/store reopen recovers the complete ten-frame pressure layout");
    store::Frame savedA{}, savedB{};
    const bool savedAExact = reopened.ReadFrame(1U, savedA) == store::Status::Ok && sameFrame(savedA, expectedA);
    const bool savedBExact = reopened.ReadFrame(2U, savedB) == store::Status::Ok && sameFrame(savedB, acceptedCopy);
    Check(savedAExact, "COW reopened durable A keeps the later source paint in all exact frame fields");
    Check(savedBExact, "COW reopened durable B MUST retain the pre-paint snapshot, not later A bytes");
    std::printf("[COW_SNAPSHOT] stage=reopened durableA_exact=%u durableB_unchanged=%u comparison=all-RGB-effects-effect-durations-and-frame-duration\n",
        savedAExact ? 1U : 0U, savedBExact ? 1U : 0U);
    for (uint16_t index = 0U; index < 8U; ++index) {
        store::Frame saved{};
        Check(reopened.ReadFrame(index + 1U, saved) == store::Status::Ok && sameFrame(saved, expectedCopies[index]),
              "all eight copied frames reopen with exact independent RGB effects effect durations and frame duration");
    }
    std::puts("[COW_PRESSURE] reopened_frames=10 copied_frames=8 exact=all_fields");
    LitchcraftSetComposeSquareCheckpointForTest(nullptr);
    Check(files.Remove(), "H3 removes isolated fixture files");
}

void RunQuietEditPlaybackProbe() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    using Status = modules::LitchcraftSession::Status;
    namespace store = modules::litchcraft_frame_store;
    const auto rgb = ResidentRgb(bouncing_balls_fixture::RGB_BASE64,
        bouncing_balls_fixture::RGB_BYTES, bouncing_balls_fixture::RGB_CRC32);
    for (const uint16_t dwell : {50U, 75U, 100U}) {
        for (const unsigned history : {0U, 1U, 2U}) {
            const char* const label = history == 0U ? "clean"
                : history == 1U ? "Rotate" : "Rotate-Undo";
            fxe::ManualMillis::SetNowMs(0U);
            worker_fixture::notified = false;
            worker_fixture::stopping = false;
            worker_fixture::paused = false;
            worker_fixture::atWait.store(false);
            worker_fixture::waitEntries.store(0U);
            firmware::LitchcraftFrameFile files;
            Check(files.Remove(), "H1 probe resets isolated real files");
            store::FrameStore seed(files.StoreOps());
            store::Manifest manifest{};
            manifest.frameCount = 256U;
            manifest.active = true;
            manifest.sparkleSeed = 7U;
            store::TransactionToken token{};
            Check(seed.Recover() == store::Status::NoArtwork &&
                      seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
                  "H1 probe starts real 256-frame Store");
            for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
                auto frame = ResidentFrame(rgb, index, false);
                frame.durationMs = dwell;
                Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
                      "H1 probe writes rich complete RGB frames");
            }
            Check(seed.Commit(token) == store::Status::Ok,
                  "H1 probe commits initial corpus before observation");
            fxe::SettingsStoreConfig settingsConfig;
            settingsConfig.path = "build/test_litchcraft_h1_probe.json";
            fxe::SettingsStore settings(settingsConfig);
            Check(settings.Load(), "H1 probe loads settings");
            hardware::ArtworkTestArbiter arbiter;
            Worker worker(settingsConfig);
            Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "H1 probe runs real S3 worker thread");
            worker.NotifyLitchcraftFrameRuntimeReady();
            auto& stream = worker.LitchcraftFrameStream();
            worker_fixture::Wait([&] {
                return stream.FrameCount() == 256U && stream.TryPresentRequested() &&
                    worker_fixture::atWait.load() && !worker_fixture::notified;
            }, "H1 setup recovers and presents frame zero");
            const auto pauseWorker = [&] {
                std::lock_guard<std::mutex> lock(worker_fixture::mutex);
                worker_fixture::paused = true;
            };
            const auto resumeWorker = [&] {
                std::lock_guard<std::mutex> lock(worker_fixture::mutex);
                worker_fixture::paused = false;
                worker_fixture::changed.notify_all();
            };
            const auto observePass = [&](const uint32_t before) {
                std::unique_lock<std::mutex> lock(worker_fixture::mutex);
                const bool complete = worker_fixture::changed.wait_for(
                    lock, std::chrono::milliseconds(200), [&] {
                        return worker_fixture::waitEntries.load() > before &&
                            worker_fixture::atWait.load() && !worker_fixture::notified;
                    });
                if (!complete) {
                    const auto d = worker.GetDiagnostics();
                    std::printf("[H1_PROBE_TIMEOUT] history=%s dwell=%u now=%lu wait_before=%lu waits=%lu atWait=%u notified=%u paused=%u pending=%u resident_active=%u dirty=%u requested=%u displayed=%u flash=%lu\n",
                        label, dwell, static_cast<unsigned long>(fxe::Millis()), static_cast<unsigned long>(before),
                        static_cast<unsigned long>(worker_fixture::waitEntries.load()), worker_fixture::atWait.load() ? 1U : 0U,
                        worker_fixture::notified ? 1U : 0U, worker_fixture::paused ? 1U : 0U,
                        d.frameStorePlaybackPending ? 1U : 0U, d.frameStoreStorageActive ? 1U : 0U,
                        stream.HasUnpersistedEdit() ? 1U : 0U, stream.RequestedIndex(), stream.DisplayedIndex(),
                        static_cast<unsigned long>(d.flashAccesses));
                    std::fflush(stdout);
                }
                Check(complete, "H1 observation has a bounded real worker opportunity");
                return complete;
            };
            const auto cached = [&](const uint16_t index) {
                store::LiveSnapshot snapshot{};
                const auto authority = stream.ReadLiveAuthority();
                snapshot.generation = authority.generation;
                snapshot.revision = authority.revision;
                store::Frame frame{};
                return stream.CaptureLiveSnapshot(index, snapshot) &&
                    stream.CopyLiveFrame(snapshot, frame) == Stream::LiveFrameResult::Cached;
            };
            const auto displayedCrc = [&] {
                store::Frame frame{};
                uint16_t index = 0xffffU;
                Check(stream.CopyDisplayed(frame, index), "H1 observes published RAM without flash read");
                return PlaybackProbeRgbCrc(frame);
            };
            pauseWorker();
            virtual_litch_config config{};
            virtual_litch_default_config(&config);
            config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
            runtime::VirtualLitchSession board(config);
            board.ObserveBootHallMask(0U);
            board.Litchcraft().AttachFrameStream(&stream,
                [](void* context, const bool playing, const uint32_t session, const bool active) {
                    static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(playing, session, active);
                }, &worker);
            Check(board.Advance(0U) == VIRTUAL_LITCH_OK && board.Advance(1000U) == VIRTUAL_LITCH_OK,
                  "H1 adopts real timeline and settles entry crossfade before editing");
            const uint8_t seek[] = {2U, 0U, 1U, 0U, 0U, 0U};
            Check(board.Litchcraft().ApplyConnectionOrdered(2U, seek, sizeof(seek), 101U) == Status::Ok &&
                      board.Advance(0U) == VIRTUAL_LITCH_OK,
                  "H1 setup explicitly seeks frame zero after crossfade to isolate paging from pause-anchor drift");
            const uint32_t setupPass = worker_fixture::waitEntries.load();
            resumeWorker();
            worker.NotifyLitchcraftFramePlayback(true);
            if (!observePass(setupPass)) std::abort();
            pauseWorker();
            Check(cached(1U), "H1 setup has one warm successor");
            const uint32_t initialRevision = stream.LiveRevision();
            const uint32_t initialEpoch = stream.DocumentEpoch();
            auto persistedPreimage = ResidentFrame(rgb, 0U, false);
            persistedPreimage.durationMs = dwell;
            Check(stream.Generation() == 1U && !stream.HasUnpersistedEdit(),
                  "H1 begins with a clean committed generation-one preimage");
            if (history != 0U) {
                std::array<uint8_t, 8U> guard{};
                fxe::WriteU32LE(stream.Generation(), guard.data());
                fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
                const uint8_t rotate[] = {0U, 15U, 0U, 0U, 0U, 2U};
                Check(board.Litchcraft().ApplyConnectionOrdered(8U, guard.data(), guard.size(), 101U) == Status::Ok &&
                          board.Litchcraft().ApplyConnectionOrdered(9U, rotate, sizeof(rotate), 101U) == Status::Ok,
                      "H1 Rotate enters through guarded ordered session");
                if (history == 2U) {
                    Check(board.Litchcraft().ApplyConnectionOrdered(6U, nullptr, 0U, 101U) == Status::Ok,
                          "H1 Undo enters through guarded ordered history");
                    Check(displayedCrc() == PlaybackProbeRgbCrc(ResidentFrame(rgb, 0U, false)),
                          "H1 Undo restores all 256 source emitters");
                }
                Check(stream.HasUnpersistedEdit(), "H1 ordered edit leaves real dirty RAM slot");
            }
            store::Frame expectedSavedFrame{};
            uint16_t editedIndex = UINT16_MAX;
            Check(stream.CopyDisplayed(expectedSavedFrame, editedIndex) && editedIndex == 0U,
                  "H1 captures the complete accepted frame before playback");
            const bool restoresPreimage = SameResidentFrame(expectedSavedFrame, persistedPreimage);
            Check(restoresPreimage == (history != 1U) &&
                      stream.LiveRevision() == initialRevision + history &&
                      stream.DocumentEpoch() == initialEpoch,
                  "H1 Rotate is a real difference and Undo restores every persisted semantic field");
            const uint32_t expectedGeneration = restoresPreimage ? 1U : 2U;
            const uint32_t playStartedAt = fxe::Millis();
            const uint32_t playAccesses = worker.GetDiagnostics().flashAccesses;
            const uint8_t play[] = {1U, 0U, 1U, 0U, 0U, 0U};
            Check(board.Litchcraft().ApplyConnectionOrdered(2U, play, sizeof(play), 101U) == Status::Ok &&
                      board.Litchcraft().IsAnimationPlaying(),
                  "H1 Play accepts without synchronous persistence");
            const uint32_t playPass = worker_fixture::waitEntries.load();
            resumeWorker();
            if (!observePass(playPass)) std::abort();
            Check(worker.GetDiagnostics().flashAccesses == playAccesses,
                  "H1 Play acknowledgement requires no flash access in this warm-start case");
            pauseWorker();
            arbiter.BlockNextFlash();
            const uint32_t boundary1Pass = worker_fixture::waitEntries.load();
            Check(board.Advance(dwell) == VIRTUAL_LITCH_OK && stream.DisplayedIndex() == 1U,
                  "H1 first authored boundary uses complete warm frame one");
            const auto physicalBoardCrc = [&] {
                const auto& output = board.PhysicalFrame();
                Check(output.led_count >= 256U, "H1 physical output has all 256 V3 board LEDs");
                return fxe::Crc32(reinterpret_cast<const uint8_t*>(output.pixels),
                    256U * sizeof(output.pixels[0]));
            };
            const uint32_t frame1Crc = displayedCrc();
            Check(frame1Crc == PlaybackProbeRgbCrc(ResidentFrame(rgb, 1U, false)),
                  "H1 frame one matches all 256 emitters");
            resumeWorker();
            const bool preboundaryAccess = arbiter.WaitForBlockedFlashFor(std::chrono::milliseconds(200));
            if (preboundaryAccess) {
                Check(board.Advance(25U) == VIRTUAL_LITCH_OK && stream.DisplayedIndex() == 1U,
                      "H1 controlled flash gate occupies 25 authored ms before clean demand");
                arbiter.ReleaseBlockedFlash();
                if (!observePass(boundary1Pass)) std::abort();
            } else {
                if (!observePass(boundary1Pass)) std::abort();
                Check(!arbiter.IsFlashBlocked(),
                      "H1 dirty quiet window suppresses successor flash access");
            }
            const bool beforeBoundaryCached = cached(2U);
            std::printf("[H1_SCHEDULING] history=%s dwell=%u now=%lu before_boundary2 cached2=%u dirty=%u generation=%lu flash=%lu\n",
                label, dwell, static_cast<unsigned long>(fxe::Millis()), beforeBoundaryCached ? 1U : 0U,
                stream.HasUnpersistedEdit() ? 1U : 0U, static_cast<unsigned long>(stream.Generation()),
                static_cast<unsigned long>(worker.GetDiagnostics().flashAccesses - playAccesses));
            pauseWorker();
            const uint32_t boundary2Pass = worker_fixture::waitEntries.load();
            Check(board.Advance(preboundaryAccess ? dwell - 25U : dwell) == VIRTUAL_LITCH_OK,
                  "H1 reaches exact second authored boundary");
            const bool boundary2Ready = stream.DisplayedIndex() == 2U;
            // Check the authored boundary before granting any worker time there.
            Check(boundary2Ready, "H1 complete successor is ready at its authored boundary");
            const uint32_t frame2Crc = displayedCrc();
            const uint32_t frame2PhysicalCrc = physicalBoardCrc();
            resumeWorker();
            if (!preboundaryAccess) {
                Check(arbiter.WaitForBlockedFlashFor(std::chrono::milliseconds(200)),
                      "H1 worker first reads successor at demand boundary");
                Check(board.Advance(25U) == VIRTUAL_LITCH_OK && stream.DisplayedIndex() == 2U &&
                          stream.RequestedIndex() == 2U && displayedCrc() == frame2Crc &&
                          physicalBoardCrc() == frame2PhysicalCrc,
                      "H1 later cold read preserves the ready frame two for 25 authored ms");
                arbiter.ReleaseBlockedFlash();
            }
            if (!observePass(boundary2Pass)) std::abort();
            pauseWorker();
            Check(board.Advance(0U) == VIRTUAL_LITCH_OK && stream.DisplayedIndex() == 2U &&
                      displayedCrc() == PlaybackProbeRgbCrc(ResidentFrame(rgb, 2U, false)),
                  "H1 released demand publishes complete frame two");
            std::printf("[H1_SCHEDULING] history=%s dwell=%u boundary2_ready=%u after_gate now=%lu requested=%u displayed=%u crc256=%08lx physical256=%08lx generation=%lu dirty=%u\n",
                label, dwell, boundary2Ready ? 1U : 0U, static_cast<unsigned long>(fxe::Millis()),
                stream.RequestedIndex(), stream.DisplayedIndex(), static_cast<unsigned long>(displayedCrc()),
                static_cast<unsigned long>(physicalBoardCrc()), static_cast<unsigned long>(stream.Generation()), stream.HasUnpersistedEdit() ? 1U : 0U);
            const uint8_t expectedUndo = stream.UndoCount();
            const uint8_t expectedRedo = stream.RedoCount();
            uint32_t priorGeneration = stream.Generation();
            while (static_cast<uint32_t>(fxe::Millis() - playStartedAt) < 6000U) {
                const uint32_t before = worker_fixture::waitEntries.load();
                Check(board.Advance(dwell) == VIRTUAL_LITCH_OK, "H1 bounded six-second timeline observation");
                resumeWorker();
                worker.NotifyLitchcraftFramePlayback(true);
                if (!observePass(before)) std::abort();
                pauseWorker();
                Check(board.Advance(0U) == VIRTUAL_LITCH_OK, "H1 presents completed worker page");
                const uint16_t shown = stream.DisplayedIndex();
                Check(displayedCrc() == PlaybackProbeRgbCrc(ResidentFrame(rgb, shown, false)),
                      "H1 sustained displayed image is one complete source frame");
                const uint32_t settledPass = worker_fixture::waitEntries.load();
                resumeWorker();
                worker.NotifyLitchcraftFramePlayback(true);
                if (!observePass(settledPass)) std::abort();
                pauseWorker();
                if (history != 0U && static_cast<uint32_t>(fxe::Millis() - playStartedAt) < 5000U) {
                    Check(stream.Generation() == 1U && stream.HasUnpersistedEdit(),
                          "H1 paging cannot persist dirty artwork before five-second quiet deadline");
                }
                if (stream.Generation() != priorGeneration) {
                    std::printf("[H1_SCHEDULING] history=%s dwell=%u durable_change now=%lu generation=%lu dirty=%u flash=%lu\n",
                        label, dwell, static_cast<unsigned long>(fxe::Millis()),
                        static_cast<unsigned long>(stream.Generation()), stream.HasUnpersistedEdit() ? 1U : 0U,
                        static_cast<unsigned long>(worker.GetDiagnostics().flashAccesses - playAccesses));
                    priorGeneration = stream.Generation();
                }
            }
            Check(stream.UndoCount() == expectedUndo && stream.RedoCount() == expectedRedo,
                  "H1 playback paging and quiet save preserve native edit history");
            Check(!stream.HasUnpersistedEdit() && stream.Generation() == expectedGeneration &&
                      stream.LiveRevision() == initialRevision + history &&
                      stream.DocumentEpoch() == initialEpoch,
                  "H1 quiet worker keeps the returned preimage at generation one and saves real Rotate exactly once");
            store::Frame recoveredFrame{};
            Check(seed.Recover(store::FrameStore::RecoveryPurpose::RootOnly) == store::Status::Ok &&
                      seed.Generation() == expectedGeneration && seed.CurrentManifest().frameCount == 256U &&
                      seed.ReadFrame(0U, recoveredFrame) == store::Status::Ok &&
                      SameResidentFrame(recoveredFrame, expectedSavedFrame),
                  "H1 recovered root retains exact RGB, effects, effect durations and dwell");
            std::printf("[H1_SCHEDULING] history=%s dwell=%u observation_end now=%lu requested=%u displayed=%u generation=%lu dirty=%u\n",
                label, dwell, static_cast<unsigned long>(fxe::Millis()), stream.RequestedIndex(), stream.DisplayedIndex(),
                static_cast<unsigned long>(stream.Generation()), stream.HasUnpersistedEdit() ? 1U : 0U);
            {
                std::lock_guard<std::mutex> lock(worker_fixture::mutex);
                worker_fixture::stopping = true;
                worker_fixture::changed.notify_all();
            }
            worker_fixture::worker.join();
            Check(files.Remove(), "H1 removes isolated probe banks");
        }
    }
}

// The real settings worker and VirtualLitch share one source document. The
// controlled host fopen fault is released before the successor writes; the
// cooperative policy and both warning projections are production owners.
void RunSupersededSettingsWarningJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    enum class Fault { Superseded, Exhausted, ExhaustedWithNewer, Preparation, Readback, StaleSize };
    for (const auto fault : {Fault::Superseded, Fault::Exhausted,
                             Fault::ExhaustedWithNewer, Fault::Preparation, Fault::Readback, Fault::StaleSize}) {
        const bool successor = fault == Fault::Superseded ||
                               fault == Fault::ExhaustedWithNewer;
        const bool terminal = fault == Fault::Readback;
        const char* label = fault == Fault::Superseded ? "superseded" :
            fault == Fault::Exhausted ? "exhausted" :
            fault == Fault::ExhaustedWithNewer ? "third_failure_newer" :
            fault == Fault::Readback ? "readback_failed" :
            fault == Fault::StaleSize ? "metadata_refused" : "prepare_failed";
        const std::string path = std::string("build/settings_warning_") + label + ".json";
        const std::string pending = path + ".tmp";
        const std::string retry = path + ".retry";
        for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
            std::remove((path + suffix).c_str());
        }
        worker_fixture::notified = false;
        worker_fixture::stopping = false;
        worker_fixture::paused = false;
        worker_fixture::atWait.store(false);
        virtual_litch_config virtualConfig{};
        virtual_litch_default_config(&virtualConfig);
        virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
        test_support::BoundedHintSession boardFixture(virtualConfig, path.c_str());
        auto& board = boardFixture.session;
        auto& settings = board.SettingsForPersistenceTest();
        Check(board.ObserveHallMask(UINT64_C(0xFFFF00000000FFFF)) == VIRTUAL_LITCH_OK &&
                  board.Advance(200U) == VIRTUAL_LITCH_OK && board.Advance(6000U) == VIRTUAL_LITCH_OK,
              "saving classification journey accepts full opening and expires setup acknowledgement");
        settings.Doc()["BRIGHTNESS"] = 7U;
        settings.Doc()["UNRELATED"] = "retained";
        settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
        Check(settings.SaveNowBlocking(), "warning journey seeds verified primary7");
        const uint32_t initialTotal = settings.GetPersistedSaveTotal();
        const std::string initialFen = board.HomeStateSnapshot().fen;
        fxe::SettingsStoreConfig config;
        config.path = path.c_str();
        auto primary = [&] {
            fxe::SettingsStore disk(config);
            Check(disk.Load() && disk.ReadValue("UNRELATED", "") == std::string("retained") &&
                      disk.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false),
                  "verified document retains unrelated preference and known protection");
            return disk.ReadValue("BRIGHTNESS", 0U);
        };
        hardware::ArtworkTestArbiter arbiter;
        Worker worker(config);
        firmware::ArtworkCurrentGameLogStorage logStorage(worker);
        const auto priorSdkRoot = batch_sdk::root;
        const auto metadataRoot = std::filesystem::path(path + ".sdk");
        if (fault == Fault::StaleSize) {
            std::filesystem::remove_all(metadataRoot);
            std::filesystem::create_directories(metadataRoot);
            batch_sdk::root = metadataRoot;
            batch_sdk::staleSize = true;
        }
        struct Observer {
            bool rejectPreparation = false;
            modules::current_game_log::StorageOps metadataStorage{};
            unsigned prepared = 0U, failed = 0U, saved = 0U, durable = 0U;
            static bool Prepare(void* context, JsonDocument&) {
                auto& self = *static_cast<Observer*>(context);
                ++self.prepared;
                if (self.metadataStorage.append != nullptr) {
                    const uint8_t bytes[] = {1U, 2U, 3U};
                    return self.metadataStorage.append(self.metadataStorage.context,
                        modules::current_game_log::Slot::A, bytes, sizeof(bytes));
                }
                return !self.rejectPreparation;
            }
            static void Result(void* context, Worker::SnapshotTransactionOutcome outcome,
                               const JsonDocument&) {
                auto& self = *static_cast<Observer*>(context);
                if (outcome == Worker::SnapshotTransactionOutcome::Saved) ++self.saved;
                else ++self.failed;
            }
            static void Durable(void* context, const JsonDocument&) {
                ++static_cast<Observer*>(context)->durable;
            }
        } observer;
        observer.rejectPreparation = fault == Fault::Preparation;
        if (fault == Fault::StaleSize) observer.metadataStorage = logStorage.Ops();
        Check(worker.SetSnapshotTransactionCallbacks(&Observer::Prepare, &Observer::Result, &observer) &&
                  worker.SetDurableSnapshotCallback(&Observer::Durable, &observer) &&
                  (worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "warning journey starts actual worker with outcome observers");
        worker_fixture::Wait([&] { return worker_fixture::atWait.load(); }, "warning worker initial wait");
        Worker::SaveReceipt receipt = 0U;
        struct RestoreAtFailedAccess {
            hardware::ArtworkTestArbiter& arbiter;
            Worker& worker;
            modules::SettingsStore& source;
            Worker::SaveReceipt& receipt;
            const char* pending;
            const char* retry;
            unsigned restoreAt;
            bool enqueueAtRestore;
            unsigned accesses = 0U;
            std::atomic<bool> restored{false};
            static void Access(void* context) {
                auto& self = *static_cast<RestoreAtFailedAccess*>(context);
                // Each failed SaveImpl performs three released writer accesses.
                // Enqueue on access9 admits a successor only during failure3,
                // after its fopen has failed and before the worker tests budget.
                if (++self.accesses != self.restoreAt) return;
                const bool restored = ::rmdir(self.pending) == 0 && ::rmdir(self.retry) == 0;
                if (self.enqueueAtRestore) {
                    Check(self.worker.EnqueueTracked(self.source.Doc(), self.receipt),
                          "newer snapshot admitted during third failed access");
                }
                self.arbiter.BlockNextFlash();
                self.restored.store(restored);
                worker_fixture::changed.notify_all();
            }
        } restore{arbiter, worker, settings, receipt, pending.c_str(), retry.c_str(),
                  fault == Fault::Superseded ? 3U : 9U,
                  fault == Fault::ExhaustedWithNewer};
        if (fault == Fault::Readback) {
            worker.SetSettingsStorageAccessTestHook([](void* context) {
                const auto& path = *static_cast<const std::string*>(context);
                for (const auto* suffix : {".tmp", ".retry"}) {
                    const auto file = path + suffix;
                    struct stat status{};
                    if (::stat(file.c_str(), &status) == 0 && S_ISREG(status.st_mode)) {
                        if (auto* out = std::fopen(file.c_str(), "wb")) {
                            std::fputs("corrupt", out);
                            std::fclose(out);
                        }
                    }
                }
            }, const_cast<std::string*>(&path));
            arbiter.BlockNextFlash();
        } else if (fault != Fault::Preparation && fault != Fault::StaleSize) {
            Check(::mkdir(pending.c_str(), 0700) == 0 && ::mkdir(retry.c_str(), 0700) == 0,
                  "warning fixture obstructs only the two staging paths");
            if (successor) worker.SetSettingsStorageAccessTestHook(&RestoreAtFailedAccess::Access, &restore);
            arbiter.BlockNextFlash();
        }
        settings.Doc()["BRIGHTNESS"] = 8U;
        Check(successor ? worker.Enqueue(settings.Doc()) : worker.EnqueueTracked(settings.Doc(), receipt),
              "old immutable settings generation admitted");
        if (fault != Fault::Preparation && fault != Fault::StaleSize) {
            Check(arbiter.WaitForBlockedFlashFor(std::chrono::seconds(3)),
                  "old settings generation held at first real access");
            settings.Doc()["BRIGHTNESS"] = 9U;
            if (fault == Fault::Superseded) {
                Check(worker.EnqueueTracked(settings.Doc(), receipt) && receipt != 0U,
                      "newest9 queued while8 is Writing");
            }
            arbiter.ReleaseBlockedFlash();
        }
        if (successor) {
            worker_fixture::Wait([&] { return restore.restored.load(); }, "restore staging after selected failed attempt");
            Check(arbiter.WaitForBlockedFlashFor(std::chrono::seconds(3)) && restore.restored.load(),
                  "successor held Writing after actual old failure and restored I/O");
            Check(worker.GetDiagnostics().completed == 1U &&
                      worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Pending,
                  "old generation completed; successor exact receipt is still Pending");
        } else {
            std::unique_lock<std::mutex> lock(worker_fixture::mutex);
            if (!worker_fixture::changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed && worker_fixture::atWait.load();
            })) {
                // The fault outcome and worker parking are separate predicates.
                // Sample only atomics and fixture state protected by this lock.
                const auto diagnostics = worker.GetDiagnostics();
                std::printf(
                    "[FAIL] actual terminal failure timeout case=%s receipt=%lu outcome=%u atWait=%u notified=%u paused=%u requested=%lu completed=%lu failures=%lu error=%u flash_accesses=%lu wait_entries=%lu\n",
                    label, static_cast<unsigned long>(receipt),
                    unsigned(worker.GetSaveOutcome(receipt)),
                    worker_fixture::atWait.load() ? 1U : 0U,
                    worker_fixture::notified ? 1U : 0U,
                    worker_fixture::paused ? 1U : 0U,
                    static_cast<unsigned long>(diagnostics.requested),
                    static_cast<unsigned long>(diagnostics.completed),
                    static_cast<unsigned long>(worker.GetFailureCount()),
                    unsigned(worker.GetLastError()),
                    static_cast<unsigned long>(diagnostics.flashAccesses),
                    static_cast<unsigned long>(worker_fixture::waitEntries.load()));
                std::fflush(stdout);
                std::abort();
            }
        }
        auto checkpoint = [&](const char* phase, const bool expectTerminal) {
            Check(board.Advance(0U) == VIRTUAL_LITCH_OK, "production cooperative persistence policy tick");
            const uint32_t pulse = board.State().now_ms % 5000U;
            Check(board.Advance(pulse == 0U ? 0U : 5000U - pulse) == VIRTUAL_LITCH_OK,
                  "timed storage marker checkpoint reaches its five-second pulse boundary");
            const auto home = board.HomeStateSnapshot();
            const auto health = board.BoardHealthConnectionState();
            const bool confirmedIo = fault == Fault::Readback;
            const auto& physical = board.PhysicalFrame();
            const auto usb = physical.pixels[physical.led_count - 32U + 27U];
            const bool amber = usb.r != 0U && usb.g != 0U && usb.b == 0U;
            Check(amber == confirmedIo,
                  "mapped fixed USB amber marker requires actual readback failure");
            Check(home.persistenceDegradation == (confirmedIo
                      ? modules::PersistenceDegradation::StorageIo : modules::PersistenceDegradation::None) &&
                      health.context[0U] == (confirmedIo ? 0x46U : expectTerminal ? 0x44U : 0x40U),
                  "only confirmed readback failure marks Home and encoded storage health");
            Check(settings.HasConfirmedStorageFailure() == confirmedIo,
                  "source retains only witnessed storage failure evidence");
            if (fault == Fault::StaleSize && batch_sdk::staleSize) {
                Check(std::filesystem::file_size(metadataRoot / "current_game_a.bin") == 3U,
                      "real adapter wrote complete bytes; only stale ENOMEM metadata refused preparation");
            }
            Check(settings.IsDisabled() == expectTerminal,
                  "only confirmed shared storage failure enters session-only policy");
            if (fault == Fault::Preparation && observer.saved == 0U) {
                Check(worker.GetDiagnostics().flashAccesses == 0U,
                      "preparation refusal performs zero filesystem accesses");
            }
            Check(std::string(home.fen) == initialFen &&
                      settings.ReadValue("UNRELATED", "") == std::string("retained") &&
                      settings.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false),
                  "policy preserves game, preferences and independent known protection");
            std::printf("[SETTINGS_WARNING] case=%s phase=%s terminal=%u disabled=%u home_ps=%u health_context=%02X ram=%u primary=%u attempts_failed=%lu source_errors=%lu receipt=%u durable_total=%lu next=%s\n",
                label, phase, settings.HasTerminalSaveFailure(), settings.IsDisabled(),
                unsigned(home.persistenceDegradation), unsigned(health.context[0U]),
                settings.ReadValue("BRIGHTNESS", 0U), primary(),
                static_cast<unsigned long>(worker.GetFailureCount()), static_cast<unsigned long>(settings.GetErrorCount()),
                unsigned(worker.GetSaveOutcome(receipt)), static_cast<unsigned long>(settings.GetPersistedSaveTotal()),
                settings.IsDisabled() ? "continue_RAM_only" : "continue_saving");
            std::printf("[SAVING_CLASSIFICATION_PLAYBACK] case=%s at_ms=%lu occupancy=%016llX expected=%016llX correction=%016llX input=full-opening/worker-%s accepted=full-opening mode=playable-opening text=see-Home-line usb27=%u,%u,%u,%u next=lift-e2/place-e4 settle=ordinary-turn-expiry health=%s saving=%s\n",
                label, static_cast<unsigned long>(board.State().now_ms),
                static_cast<unsigned long long>(board.State().physical_occupied_mask),
                static_cast<unsigned long long>(board.State().expected_occupied_mask),
                static_cast<unsigned long long>(board.State().correction_mask), label,
                usb.r, usb.g, usb.b, usb.w,
                home.persistenceDegradation == modules::PersistenceDegradation::StorageIo
                    ? "saving-attention" : "quiet",
                settings.IsDisabled() ? "session-only" : "worker-enabled");
            std::printf("[SAVING_CLASSIFICATION_HOME] %s\n", board.HomeStateLine().CStr());
        };
        Check(worker.GetLastError() != fxe::SettingsStoreError::None,
              "failed attempt remains visible in worker diagnostics");
        Check(settings.HasTerminalSaveFailure() == terminal && primary() == 7U &&
                  settings.GetPersistedSaveTotal() == initialTotal,
              "only preparation failure or exhausted retries certify terminal source failure");
        checkpoint("old_failed_before_successor_save", terminal);
        if (successor) {
            arbiter.ReleaseBlockedFlash();
            worker_fixture::Wait([&] {
                return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved && worker_fixture::atWait.load();
            }, "newest9 verified primary promotion");
            Check(primary() == 9U && settings.GetPersistedSaveTotal() == initialTotal + 1U &&
                      observer.prepared == 2U && observer.failed == 1U && observer.saved == 1U && observer.durable == 1U,
                  "failed old callback stays truthful; only verified newest9 advances durable total");
            checkpoint("newest9_verified", terminal);
        } else {
            Check(observer.prepared == 1U && observer.failed == 1U && observer.saved == 0U && observer.durable == 0U,
                  "terminal failure fabricates neither Saved nor durable completion");
            if (fault != Fault::Preparation && fault != Fault::Readback && fault != Fault::StaleSize) {
                Check(::rmdir(pending.c_str()) == 0 && ::rmdir(retry.c_str()) == 0,
                      "restore healthy staging paths after exhaustion");
            }
        }
        Check(worker.GetFailureCount() == (fault == Fault::Superseded || fault == Fault::Preparation || fault == Fault::StaleSize ? 1U : 3U),
              "worker preserves each attempt diagnostic and per-generation retry budget");
        Check(worker.ReleaseSaveReceipt(receipt), "retire completed exact receipt");
        if (!terminal && !settings.IsDisabled()) {
            observer.rejectPreparation = false;
            if (fault == Fault::StaleSize) batch_sdk::staleSize = false;
            std::filesystem::remove_all(pending);
            std::filesystem::remove_all(retry);
            settings.Doc()["BRIGHTNESS"] = 10U;
            Check(worker.EnqueueTracked(settings.Doc(), receipt), "later preference remains saveable after supersession");
            worker_fixture::Wait([&] {
                return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved && worker_fixture::atWait.load();
            }, "later edit verified");
            Check(primary() == 10U && settings.GetPersistedSaveTotal() == initialTotal + (successor ? 2U : 1U),
                  "later preference saves once without losing prior authority");
            checkpoint("later_edit_verified", false);
            Check(worker.ReleaseSaveReceipt(receipt), "retire later edit receipt");
        } else if (terminal) {
            Check(!worker.Enqueue(settings.Doc()), "healthy storage cannot auto-clear true session-only barrier");
            modules::ControlMutationOutcome outcome;
            Check(board.MutateSettingJson(41U, "BRIGHTNESS", "6", outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      outcome.persistenceTier == modules::ControlMutationPersistenceTier::None &&
                      settings.ReadValue("BRIGHTNESS", 0U) == 6U,
                  "validated ordinary control remains usable in RAM after true failure");
            checkpoint("healthy_paths_RAM_control", true);
        }
        {
            std::lock_guard<std::mutex> lock(worker_fixture::mutex);
            worker_fixture::stopping = true;
            worker_fixture::changed.notify_all();
        }
        worker_fixture::worker.join();
        if (fault == Fault::StaleSize) {
            batch_sdk::staleSize = false;
            batch_sdk::root = priorSdkRoot;
            std::filesystem::remove_all(metadataRoot);
        }
    }
}

void RunSettingsDuringPlaybackWorkerJourney(const bool artworkStall = false) {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "settings during playback starts from isolated frame banks");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 4U;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    store::Frame frame{};
    frame.durationMs = 100U;
    frame.emitters[0].color = {31U, 2U, 3U};
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, frame) == store::Status::Ok &&
              seed.WriteFrame(token, 1U, frame) == store::Status::Ok &&
              seed.WriteFrame(token, 2U, frame) == store::Status::Ok &&
              seed.WriteFrame(token, 3U, frame) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "settings during playback seeds a verified active animation");
    const char* const path = "build/test_settings_during_playback.json";
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
    fxe::SettingsStoreConfig config;
    config.path = path;
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "settings during playback loads usable preferences");
    size_t descriptorCount = 0U;
    const auto* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    for (size_t i = 0U; i < descriptorCount; ++i) {
        const auto& descriptor = descriptors[i];
        if (!descriptor.IsPersisted() || descriptor.IsRetired()) continue;
        if (descriptor.type == fxe::SettingType::Bool) {
            settings.Doc()[descriptor.key] = descriptor.defaultUint != 0U;
        } else if (descriptor.type == fxe::SettingType::UInt32) {
            settings.Doc()[descriptor.key] = descriptor.defaultUint;
        } else {
            settings.Doc()[descriptor.key] = descriptor.defaultStr != nullptr
                ? descriptor.defaultStr : "";
        }
    }
    modules::GamePersistenceState game;
    Check(game.undoBaseFen.Assign(modules::INITIAL_FEN) &&
              game.fen.Assign(artworkStall
                  ? "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 4 3"
                  : "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 128 65"),
          "resident current-line journey assigns a legal full undo tail");
    modules::DuckGameDurableState classic;
    classic.variant = modules::GameVariant::Classic;
    char encodedVariant[32U] = {};
    Check(modules::EncodeDuckGameDurableState(classic, encodedVariant, sizeof(encodedVariant)) &&
              game.variantState.Assign(encodedVariant), "resident current-line variant encodes");
    game.gameHistory.present = true;
    game.gameHistory.gameId.bytes[0U] = 0x56U;
    game.gameHistory.currentPly = artworkStall ? 4U : modules::UNDO_HISTORY_MOVE_CAPACITY;
    game.gameHistory.lineRevision = game.gameHistory.currentPly + 1U;
    for (uint16_t ply = 0U; ply < game.gameHistory.currentPly; ++ply) {
        const uint8_t cycle = static_cast<uint8_t>(ply % 4U);
        const uint8_t move[modules::UNDO_HISTORY_FIELDS_PER_MOVE] = {
            static_cast<uint8_t>(ply % 2U), static_cast<uint8_t>(ply % 2U == 0U ? 'N' : 'n'),
            0U, modules::game_history::INVALID_SQUARE, 0U,
            static_cast<uint8_t>(cycle == 0U ? 62U : cycle == 1U ? 6U : cycle == 2U ? 45U : 21U),
            static_cast<uint8_t>(cycle == 0U ? 45U : cycle == 1U ? 21U : cycle == 2U ? 62U : 6U)};
        std::memcpy(game.undoHistory + game.undoHistoryCount, move, sizeof(move));
        game.undoHistoryCount += sizeof(move);
    }
    Check(modules::WriteGamePersistenceState(settings.Doc(), game),
          "resident current-line journey writes the full valid game snapshot");
    modules::game_history::GameHistoryCaptureSnapshot snapshot;
    Check(modules::game_history::GameHistorySnapshotReader(game).Build(snapshot),
          "resident current-line source validates before worker parsing");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    persistence_test::CurrentLineMemoryStorage currentLineStorage;
    firmware::ArtworkCurrentGameLogService currentLine(currentLineStorage.Ops());
    Check(currentLine.Configure(worker, settings.Doc()) && (worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "settings during playback starts real worker and current-line service");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == manifest.frameCount && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "settings during playback boot recovery");
    worker.NotifyLitchcraftFramePlayback(true);
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreStorageActive && worker_fixture::atWait.load();
    }, "settings during playback resident storage");
    struct PagingDuringSnapshot {
        modules::litchcraft_frame_stream::FrameStream& stream;
        unsigned accesses = 0U;
        bool requested = false;
        static void Access(void* context) {
            auto& self = *static_cast<PagingDuringSnapshot*>(context);
            ++self.accesses;
            // The SDK hook supplies a demand boundary while the real settings
            // writer and current-line transaction own the worker. Paging itself
            // remains production FrameStream/FrameFile behavior.
            if (!self.requested) self.requested = self.stream.Request(3U);
        }
    } paging{stream};
    worker.SetSettingsStorageAccessTestHook(&PagingDuringSnapshot::Access, &paging);
    for (const bool brownoutLatched : {false, true}) {
        settings.Doc()["BRIGHTNESS"] = brownoutLatched ? 7U : 6U;
        if (brownoutLatched) {
            settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
            settings.Doc()["LED_BROWNOUT_INCIDENT_COUNT"] = 2U;
            settings.ArmPersistenceGuard();
        }
        const uint32_t completed = worker.GetCompletedSaveCount();
        const bool accepted = settings.SaveNow();
        Check(accepted, "healthy settings save is admitted while animation plays, with or without brownout history");
        if (accepted) {
            worker_fixture::Wait([&] {
                return worker.GetCompletedSaveCount() > completed && worker_fixture::atWait.load();
            }, "settings during playback verified completion");
            fxe::SettingsStore reboot(config);
            Check(reboot.Load() && reboot.ReadValue("BRIGHTNESS", 0U) == (brownoutLatched ? 7U : 6U) &&
                      reboot.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false) == brownoutLatched &&
                      settings.GetLastError() == fxe::SettingsStoreError::None,
                  "save survives reboot and retains independent power protection");
        }
        Check(paging.requested && paging.accesses > 0U &&
                  stream.TryPresentRequested() && stream.DisplayedIndex() == 3U,
              "real settings access services an uncached resident frame demand");
        Check(worker.GetDiagnostics().frameStoreStorageActive &&
                  stream.FrameCount() == manifest.frameCount && stream.Generation() == 1U,
              "ordinary save preserves playback resident and artwork authority");
        std::printf("[SETTINGS_DURING_PLAYBACK] brownout=%u admitted=%u saved=%u errors=%u next=continue_playback\n",
                    brownoutLatched ? 1U : 0U, accepted ? 1U : 0U,
                    worker.GetCompletedSaveCount() - completed, settings.GetErrorCount());
    }

    if (artworkStall) {
        // Reproduce the shared-worker scheduling seam, not physical flash time.
        // The real frame save is held inside its existing file-access lease.
        const uint32_t completed = worker.GetCompletedSaveCount();
        arbiter.BlockNextFlash();
        Check(stream.PaintDisplayedIfRevision(stream.LiveRevision(), 3U, 9U, 15U,
                  {211U, 18U, 69U}, false) ==
                  modules::litchcraft_frame_stream::FrameStream::GuardedEditResult::Changed,
              "artwork stall starts from a genuine dirty displayed frame");
        stream.RequestImmediateSave();
        worker.NotifyLitchcraftFramePlayback(false);
        if (!arbiter.WaitForBlockedFlashFor(std::chrono::milliseconds(500))) std::abort();
        Check(game.fen.Assign("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 5 3"),
              "stalled save journey captures the actual next knight move");
        const uint8_t nextMove[modules::UNDO_HISTORY_FIELDS_PER_MOVE] = {
            0U, 'N', 0U, modules::game_history::INVALID_SQUARE, 0U, 62U, 45U};
        std::memcpy(game.undoHistory + game.undoHistoryCount, nextMove, sizeof(nextMove));
        game.undoHistoryCount += sizeof(nextMove);
        ++game.gameHistory.currentPly;
        ++game.gameHistory.lineRevision;
        Check(modules::WriteGamePersistenceState(settings.Doc(), game) &&
                  modules::game_history::GameHistorySnapshotReader(game).Build(snapshot),
              "stalled save journey writes a valid fifth committed ply through the production game serializer");
        settings.Doc()["BRIGHTNESS"] = 12U;
        settings.MarkMustSaveDirty();
        Check(settings.SaveNow(), "settings and fifth game ply enter the existing independent slot during artwork I/O");
        settings.Doc()["BRIGHTNESS"] = 13U;
        settings.MarkMustSaveDirty();
        Check(settings.SaveNow(), "newest pending brightness coalesces without allocating or overwriting an in-flight settings writer");
        fxe::ManualMillis::AdvanceMs(19318U);
        Check(worker.GetCompletedSaveCount() == completed && !settings.IsDisabled() &&
                  !settings.HasTerminalSaveFailure() && settings.GetErrorCount() == 0U,
              "nineteen-second artwork stall delays both receipts but is not a terminal settings failure or SessionOnly transition");
        fxe::SettingsStore beforeResume(config);
        modules::GamePersistenceState oldGame;
        Check(beforeResume.Load() && beforeResume.ReadValue("BRIGHTNESS", 0U) == 7U &&
                  modules::ReadGamePersistenceState(beforeResume.Doc(), oldGame) &&
                  oldGame.gameHistory.currentPly == 4U,
              "power-loss equivalent fresh read before worker release still contains old brightness and four game plies");
        std::puts("[ARTWORK_STALL_SETTINGS] modeled_stall_ms=19318 artwork_worker=blocked latest_game_ply=5 durable_game_ply=4 live_brightness=13 durable_brightness=7 completed=unchanged disabled=0 terminal_failure=0");
        arbiter.ReleaseBlockedFlash();
        worker_fixture::Wait([&] {
            return worker.GetCompletedSaveCount() == completed + 1U &&
                !stream.HasUnpersistedEdit() && worker_fixture::atWait.load();
        }, "artwork release lets the same worker save coalesced settings and real current-game transaction");
        fxe::SettingsStore afterResume(config);
        modules::GamePersistenceState newestGame;
        Check(afterResume.Load() && afterResume.ReadValue("BRIGHTNESS", 0U) == 13U &&
                  modules::ReadGamePersistenceState(afterResume.Doc(), newestGame) &&
                  newestGame.fen.Equals(game.fen.CStr()) && newestGame.gameHistory.currentPly == 5U &&
                  !settings.IsDisabled() && !settings.HasTerminalSaveFailure() &&
                  currentLine.FailureCount() == 0U,
              "worker recovery durably retains the fifth game move and newest brightness without falsely disabling persistence");
        std::puts("[ARTWORK_STALL_SETTINGS] release=saved fifth_ply_exact=1 brightness=13 pending_coalesced=1 disabled=0 terminal_failure=0");
    }
    if (!artworkStall && worker.GetCompletedSaveCount() == 2U) {
        // Hold the real worker at flash admission, then submit a later edit.
        // Its single resident slot must preserve the accepted snapshot and
        // retain new intent without turning bounded backpressure into an error.
        const uint32_t completed = worker.GetCompletedSaveCount();
        arbiter.BlockNextFlash();
        settings.Doc()["BRIGHTNESS"] = 8U;
        Check(settings.SaveNow(), "resident slot accepts one immutable snapshot");
        arbiter.WaitForBlockedFlash();
        settings.Doc()["BRIGHTNESS"] = 9U;
        settings.MarkDirty();
        Check(!settings.SaveNow() && settings.GetErrorCount() == 0U &&
                  settings.GetLastError() == fxe::SettingsStoreError::None,
              "occupied resident slot defers a newer edit without overwriting or reporting failure");
        arbiter.ReleaseBlockedFlash();
        worker_fixture::Wait([&] {
            return worker.GetCompletedSaveCount() == completed + 1U && worker_fixture::atWait.load();
        }, "resident slot completes its accepted snapshot");
        fxe::SettingsStore firstSaved(config);
        Check(firstSaved.Load() && firstSaved.ReadValue("BRIGHTNESS", 0U) == 8U,
              "an edit while the worker writes cannot mutate the accepted snapshot");
        fxe::ManualMillis::AdvanceMs(config.deferredSaveDelayMs);
        settings.Update();
        worker_fixture::Wait([&] {
            return worker.GetCompletedSaveCount() == completed + 2U && worker_fixture::atWait.load();
        }, "newest deferred preference is saved during playback");
        fxe::SettingsStore latestSaved(config);
        Check(latestSaved.Load() && latestSaved.ReadValue("BRIGHTNESS", 0U) == 9U,
              "normal deferred retry persists the newest preference without stopping animation");

        // Real write failure remains visible; only verified completion clears
        // it. The historical brownout clamp survives both failure and recovery.
        const std::string pending = std::string(path) + ".tmp";
        const std::string retry = std::string(path) + ".retry";
        Check(::mkdir(pending.c_str(), 0700) == 0 && ::mkdir(retry.c_str(), 0700) == 0,
              "settings failure fixture blocks both bounded staging paths");
        settings.Doc()["BRIGHTNESS"] = 10U;
        Worker::SaveReceipt receipt = 0U;
        Check(worker.EnqueueTracked(settings.Doc(), receipt) && receipt != 0U,
              "actual storage-failure snapshot receives its exact receipt during playback");
        worker_fixture::Wait([&] {
            return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed && worker_fixture::atWait.load();
        }, "resident settings terminal write failure");
        fxe::SettingsStore afterFailure(config);
        Check(afterFailure.Load() && afterFailure.ReadValue("BRIGHTNESS", 0U) == 9U &&
                  settings.GetLastError() != fxe::SettingsStoreError::None &&
                  !settings.IsDisabled() && worker.GetFailureCount() == 3U,
              "three bounded failed attempts retain prior saved preference and truthful recoverable failure");
        Check(::rmdir(pending.c_str()) == 0 && ::rmdir(retry.c_str()) == 0 &&
                  worker.ReleaseSaveReceipt(receipt),
              "transient storage obstruction and completed receipt can be retired safely");
        settings.Doc()["BRIGHTNESS"] = 11U;
        settings.MarkDirty();
        settings.Update();
        fxe::ManualMillis::AdvanceMs(config.mustSaveDelayMs);
        settings.Update();
        worker_fixture::Wait([&] {
            return worker.GetCompletedSaveCount() == completed + 3U && worker_fixture::atWait.load();
        }, "healthy storage recovers while playback remains resident");
        fxe::SettingsStore recovered(config);
        Check(recovered.Load() && recovered.ReadValue("BRIGHTNESS", 0U) == 11U &&
                  recovered.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false) &&
                  recovered.ReadValue("LED_BROWNOUT_INCIDENT_COUNT", 0U) == 2U &&
                  settings.GetLastError() == fxe::SettingsStoreError::None &&
                  worker.GetDiagnostics().frameStoreStorageActive && stream.Generation() == 1U,
              "verified retry saves latest preference, clears only storage failure, and preserves protection and artwork");
        std::puts("[SETTINGS_DURING_PLAYBACK] occupied=deferred failures=3 recovered=verified brightness=11 brownout=retained artwork_generation=1 next=continue_playback");
    }
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    fxe::SettingsStore gameDisk(config);
    modules::GamePersistenceState durableGame;
    modules::current_game_log::CommitPointer durablePointer;
    Check(gameDisk.Load() && modules::ReadGamePersistenceState(gameDisk.Doc(), durableGame) &&
              durableGame.fen.Equals(game.fen.CStr()) &&
              durableGame.undoHistoryCount == game.undoHistoryCount &&
              std::memcmp(durableGame.undoHistory, game.undoHistory, game.undoHistoryCount) == 0 &&
              modules::current_game_log::ReadCommitPointer(gameDisk.Doc(), durablePointer) &&
              durablePointer.present && durablePointer.currentPly == game.gameHistory.currentPly &&
              durablePointer.archiveRequired && currentLine.FailureCount() == 0U &&
              currentLineStorage.beginBatchCalls == currentLineStorage.endBatchCalls,
          "resident current-line save, failure and recovery preserve the full game and exactly closed batches");
    std::printf("[CURRENT_LINE_DURING_PLAYBACK] resident=%u frame_generation=%lu displayed_frame=%u settings_accesses=%u game_ply=%u game_bytes=%zu saved=%lu failed=%lu service_failures=%lu next=continue_playback\n",
                worker.GetDiagnostics().frameStoreStorageActive ? 1U : 0U,
                static_cast<unsigned long>(stream.Generation()), stream.DisplayedIndex(), paging.accesses, durablePointer.currentPly,
                measureJson(gameDisk.Doc()), static_cast<unsigned long>(worker.GetCompletedSaveCount()),
                static_cast<unsigned long>(worker.GetFailureCount()),
                static_cast<unsigned long>(currentLine.FailureCount()));
    Check(files.Remove(), "settings during playback removes isolated frame banks");
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
}


// The real worker, stdio SettingsStore and FrameFile feed the production
// VirtualLitch session. Controlled access cost runs render at 5 ms throughout a
// pending receipt; it does not emulate paging or save completion in the test.
void RunSlowSettingsPlaybackWorkerJourney(const bool failWrites,
                                          const bool immediatePlaySave = false) {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "slow settings journey clears isolated frame banks");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 16U;
    manifest.active = true;
    manifest.playbackOnEntry = !immediatePlaySave;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "slow settings journey seeds committed distinct 50 ms frames");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        store::Frame frame{};
        frame.durationMs = 50U;
        frame.emitters[0].color = {static_cast<uint8_t>(40U + index), 1U, 2U};
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "slow settings journey writes distinct authored frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "slow settings journey commits verified frame authority");
    const char* const path = "build/test_slow_settings_playback.json";
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
    fxe::SettingsStoreConfig config;
    config.path = path;
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "slow settings journey loads healthy settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "slow settings journey starts real worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == manifest.frameCount &&
            stream.TryPresentRequested() && worker_fixture::atWait.load() &&
            (!immediatePlaySave || !worker.GetDiagnostics().frameStoreStorageActive);
    }, "slow settings journey boot frame");
    if (immediatePlaySave) {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    virtual_litch_config boardConfig{};
    virtual_litch_default_config(&boardConfig);
    boardConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(boardConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        }, &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.SemanticFrame().board[0][0].r == 40U,
          "slow settings journey displays actual first frame");
    const uint8_t play[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(2U, play, sizeof(play)) ==
              modules::LitchcraftSession::Status::Ok,
          "slow settings journey enters Play through production ordered action");
    if (!immediatePlaySave) {
        worker_fixture::Wait([&] {
            return worker.GetDiagnostics().frameStoreStorageActive &&
                !worker.LitchcraftFrameModeSavePending() && worker_fixture::atWait.load();
        }, "slow settings journey prefetch");
    }
    struct Cadence {
        Worker& worker;
        runtime::VirtualLitchSession& board;
        Worker::SaveReceipt receipt = 0U;
        uint32_t elapsedMs = 0U;
        uint32_t renderSamples = 0U;
        uint32_t mismatches = 0U;
        uint32_t firstMismatchMs = 0U;
        uint32_t pendingBoundaries = 0U;
        uint32_t accesses = 0U;
        uint32_t retryDelayMs = 0U;
        uint8_t firstActual = 0U;
        uint8_t firstExpected = 0U;
        std::mutex renderMutex;
        std::condition_variable renderChanged;
        uint32_t renderRequestMs = 0U;
        void WaitForRender(const uint32_t ms) {
            std::unique_lock<std::mutex> lock(renderMutex);
            renderRequestMs = ms;
            renderChanged.notify_all();
            renderChanged.wait(lock, [&] { return renderRequestMs == 0U; });
        }
        void Advance(const uint32_t ms) {
            uint32_t left = ms;
            while (left != 0U) {
                const uint32_t step = std::min<uint32_t>(left, 5U);
                elapsedMs += step;
                left -= step;
                const uint8_t expected = static_cast<uint8_t>(
                    40U + (elapsedMs / 50U) % 16U);
                const auto result = board.Advance(step);
                const uint8_t actual = board.SemanticFrame().board[0][0].r;
                ++renderSamples;
                if (worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Pending &&
                    elapsedMs % 50U == 0U) ++pendingBoundaries;
                if (result != VIRTUAL_LITCH_OK || actual != expected) {
                    if (mismatches++ == 0U) {
                        firstMismatchMs = elapsedMs;
                        firstActual = actual;
                        firstExpected = expected;
                    }
                }
            }
        }
        static void Access(void* context) {
            auto& self = *static_cast<Cadence*>(context);
            ++self.accesses;
            self.WaitForRender(5U);
        }
        static void Delay(void* context, const uint32_t ms) {
            auto& self = *static_cast<Cadence*>(context);
            if (ms > 1U) {
                self.retryDelayMs += ms;
                self.WaitForRender(ms);
            }
        }
    } cadence{worker, board};
    const std::string pending = std::string(path) + ".tmp";
    const std::string retry = std::string(path) + ".retry";
    if (failWrites) {
        Check(::mkdir(pending.c_str(), 0700) == 0 &&
                  ::mkdir(retry.c_str(), 0700) == 0,
              "slow settings journey blocks both actual staging paths");
    }
    settings.Doc()["BRIGHTNESS"] = 13U;
    settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
    settings.Doc()["LED_BROWNOUT_INCIDENT_COUNT"] = 2U;
    if (!immediatePlaySave) arbiter.BlockNextFlash();
    Check(worker.EnqueueTracked(settings.Doc(), cadence.receipt) &&
              cadence.receipt != 0U,
          "slow settings journey admits exact receipt during playback");
    if (!immediatePlaySave) arbiter.WaitForBlockedFlash();
    else Check(!worker.GetDiagnostics().frameStoreStorageActive,
               "immediate Play and Save are admitted before resident storage starts");
    worker.SetSettingsStorageAccessTestHook(&Cadence::Access, &cadence);
    artworkTaskDelayHook = &Cadence::Delay;
    artworkTaskDelayContext = &cadence;
    if (!immediatePlaySave) arbiter.ReleaseBlockedFlash();
    else {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    const auto watchdog = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.GetSaveOutcome(cadence.receipt) == Worker::SaveOutcome::Pending ||
           !worker_fixture::atWait.load()) {
        std::unique_lock<std::mutex> lock(cadence.renderMutex);
        cadence.renderChanged.wait_for(lock, std::chrono::milliseconds(20), [&] {
            return cadence.renderRequestMs != 0U;
        });
        const uint32_t requestedMs = cadence.renderRequestMs;
        lock.unlock();
        if (requestedMs != 0U) {
            cadence.Advance(requestedMs);
            lock.lock();
            cadence.renderRequestMs = 0U;
            cadence.renderChanged.notify_all();
        }
        if (std::chrono::steady_clock::now() >= watchdog) {
            std::puts("[FAIL] slow settings journey render/worker feedback watchdog");
            std::abort();
        }
    }
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    artworkTaskDelayHook = nullptr;
    artworkTaskDelayContext = nullptr;
    worker.SetSettingsStorageAccessTestHook(nullptr, nullptr);
    Check(cadence.pendingBoundaries >= 5U && cadence.renderSamples >= 50U,
          "slow settings corpus crosses uncached frame boundaries before receipt completion");
    Check(!arbiter.flashLeaseActive && arbiter.overlappingFlashLeases == 0U,
          "settings paging never reacquires flash before its prior lease is released");
    Check(cadence.mismatches == 0U,
          "distinct 50 ms semantic LED frames keep authored cadence while settings receipt is pending");
    Check(stream.Generation() == 1U && worker.GetDiagnostics().frameStoreStorageActive,
          "slow save keeps committed playback authority and resident storage");
    Check(worker.GetSaveOutcome(cadence.receipt) ==
              (failWrites ? Worker::SaveOutcome::Failed : Worker::SaveOutcome::Saved) &&
              worker.GetFailureCount() == (failWrites ? 3U : 0U) &&
              (!failWrites || cadence.retryDelayMs == 500U),
          "paging preserves exact receipt and three attempts with two 250 ms retry waits");
    if (!failWrites) {
        fxe::SettingsStore reboot(config);
        Check(reboot.Load() && reboot.ReadValue("BRIGHTNESS", 0U) == 13U &&
                  reboot.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false) &&
                  reboot.ReadValue("LED_BROWNOUT_INCIDENT_COUNT", 0U) == 2U,
              "slow verified save retains preference and independent power latch");
    }
    std::printf("[SLOW_SETTINGS_PLAYBACK] entry=%s write=%s duration_ms=%u accesses=%u "
                "pending_boundaries=%u render_samples=%u cadence_mismatches=%u "
                "first_ms=%u expected=%u actual=%u failures=%u retry_ms=%u\n",
                immediatePlaySave ? "immediate-play-save" : "resident",
                failWrites ? "failed" : "saved", cadence.elapsedMs, cadence.accesses,
                cadence.pendingBoundaries, cadence.renderSamples, cadence.mismatches,
                cadence.firstMismatchMs, cadence.firstExpected, cadence.firstActual,
                worker.GetFailureCount(), cadence.retryDelayMs);
    Check(worker.ReleaseSaveReceipt(cadence.receipt),
          "slow settings journey releases completed exact receipt");
    if (failWrites) {
        Check(::rmdir(pending.c_str()) == 0 && ::rmdir(retry.c_str()) == 0,
              "slow settings journey removes transient write obstruction");
    }
    Check(files.Remove(), "slow settings journey removes isolated frame banks");
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
}


void RunPagingOnlyPendingNewJourney() {
    namespace store = modules::litchcraft_frame_store;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "paging-only New fixture starts with isolated banks");
    uint32_t accesses = 0U;
    Check(files.SetAccessOps({&accesses, [](void* context) {
              ++*static_cast<uint32_t*>(context);
          }, [](void*, firmware::LitchcraftFrameFile::AccessOperation, size_t, uint32_t) {}}), "paging-only New uses real file access observer");
    store::FrameStore frames(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 2U;
    store::Frame first{};
    first.emitters[0].color = {72U, 3U, 4U};
    store::TransactionToken token{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
              frames.WriteFrame(token, 0U, first) == store::Status::Ok &&
              frames.WriteFrame(token, 1U, first) == store::Status::Ok &&
              frames.Commit(token) == store::Status::Ok,
          "paging-only New fixture seeds verified predecessor");
    Stream stream;
    Check(stream.StageFirstFrame(frames.CurrentManifest(), frames.Generation(), first) &&
              stream.NewBlankDocument(1U, stream.LiveRevision(), manifest.favorites) ==
                  Stream::GuardedEditResult::Changed,
          "paging-only New fixture publishes accepted blank RAM document");
    const uint32_t before = accesses;
    Check(stream.Service(frames, false, Stream::ServiceMode::PagingOnly) ==
              Stream::ServiceResult::Idle && accesses == before &&
              stream.HasPendingBlankDocument() && frames.Generation() == 1U,
          "settings interleave cannot persist pending New or mutate its predecessor root");
    const uint32_t pagingAccesses = accesses - before;
    Check(stream.Service(frames, false, Stream::ServiceMode::PagingAndEdits) ==
              Stream::ServiceResult::Idle &&
              !stream.HasPendingBlankDocument() && frames.Generation() == 2U &&
              stream.Generation() == 2U && stream.FrameCount() == 1U,
          "normal worker mutation slice persists New after paging-only deferral");
    store::Frame blank{};
    uint16_t index = 99U;
    Check(stream.CopyDisplayed(blank, index) && index == 0U &&
              blank.emitters[0].color.r == 0U && blank.durationMs == 1000U,
          "New stays visibly blank across interleave and durable publication");
    std::printf("[SETTINGS_NEW_PAGING_ONLY] interleave_accesses=%u saved_generation=%u "
                "frame_count=%u next=paint\n", pagingAccesses, frames.Generation(), stream.FrameCount());
    Check(files.Remove(), "paging-only New fixture removes isolated banks");
}

void RunNewDuringConfigureWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "New sync race starts from isolated banks");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 2U;
    store::Frame first{};
    first.emitters[0].color = {93U, 2U, 1U};
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, first) == store::Status::Ok &&
              seed.WriteFrame(token, 1U, first) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "New sync race seeds committed predecessor root");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_new_during_configure.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "New sync race loads healthy settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    auto& stream = worker.LitchcraftFrameStream();
    // Force ordinary recovery's Configure branch; the hook then publishes New
    // precisely after its pending check and before Configure's publication lock.
    auto priorLive = manifest;
    priorLive.frameCount = 3U;
    Check(stream.StageFirstFrame(priorLive, 1U, first),
          "New sync race prepares live/Store reconciliation boundary");
    struct Race {
        Worker& worker;
        std::atomic<bool> fired{false};
        bool accepted = false;
    } race{worker};
    worker.SetLitchcraftFrameBeforeConfigureTestHook([](void* context) {
        auto& state = *static_cast<Race*>(context);
        state.worker.SetLitchcraftFrameBeforeConfigureTestHook(nullptr, nullptr);
        auto& live = state.worker.LitchcraftFrameStream();
        state.accepted = live.NewBlankDocument(live.Generation(), live.LiveRevision(), {}) ==
            Stream::GuardedEditResult::Changed;
        state.worker.NotifyLitchcraftFramePlayback(false);
        state.fired.store(true, std::memory_order_release);
    }, &race);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "New sync race starts real worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] {
        return race.fired.load(std::memory_order_acquire) && worker_fixture::atWait.load();
    }, "New sync race reaches next worker wait");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    store::FrameStore reboot(files.StoreOps());
    const auto recovered = reboot.Recover();
    Check(race.accepted && !stream.HasPendingBlankDocument() &&
              stream.Generation() == 2U && stream.FrameCount() == 1U &&
              recovered == store::Status::Ok && reboot.Generation() == 2U &&
              reboot.CurrentManifest().frameCount == 1U,
          "failed Configure must retain concurrent New wake through verified blank root publication");
    store::Frame visible{};
    uint16_t visibleIndex = 99U;
    Check(stream.CopyDisplayed(visible, visibleIndex) && visibleIndex == 0U &&
              visible.emitters[0].color.r == 0U,
          "concurrent New remains blank instead of restoring predecessor content");
    std::printf("[NEW_CONFIGURE_WAKE] accepted=%u pending=%u live_generation=%u "
                "saved_generation=%u visible_red=%u next=paint\n",
                race.accepted ? 1U : 0U, stream.HasPendingBlankDocument() ? 1U : 0U,
                stream.Generation(), reboot.Generation(), visible.emitters[0].color.r);
    Check(files.Remove(), "New sync race removes isolated frame banks");
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::remove((std::string(config.path) + suffix).c_str());
    }
}

void RunSummaryReadHandleAliasCorpus() {
    namespace fixed = modules::litchcraft_frame_store_layout;
    for (const uint8_t resident : {0U, 1U}) {
        firmware::LitchcraftFrameFile files;
        Check(files.Remove(), "summary alias corpus isolates backing files");
        const auto ops = files.StoreOps();
        const size_t root = fixed::Layout::RootOffset(resident, 0U);
        const size_t summary = fixed::Layout::SummaryOffset(resident, 0U);
        const size_t tail = fixed::Layout::SummaryOffset(resident, 1U);
        const uint8_t rootByte = 42U;
        std::array<uint8_t, 4U> read{};
        const std::array<uint8_t, 4U> first{{7U, 8U, 9U, 10U}};
        const std::array<uint8_t, 4U> second{{17U, 18U, 19U, 20U}};
        Check(ops.write(ops.context, root, &rootByte, 1U) && ops.sync(ops.context),
              "summary alias corpus starts with a short root file");
        const auto before = files.ReadOpensForTest();
        Check(ops.read(ops.context, summary, read.data(), read.size()) &&
                  read == std::array<uint8_t, 4U>{{255U, 255U, 255U, 255U}} &&
                  ops.read(ops.context, summary, read.data(), read.size()) &&
                  files.ReadOpensForTest() == before + 1U,
              "summary chunk reads retain one handle even before a tail exists");
        Check(ops.write(ops.context, summary, first.data(), first.size()) &&
                  ops.sync(ops.context) &&
                  ops.read(ops.context, summary, read.data(), read.size()) && read == first &&
                  files.ReadOpensForTest() == before + 2U,
              "summary write invalidates retained file size and refreshes grown tail");
        Check(ops.write(ops.context, root, &rootByte, 1U) && ops.sync(ops.context) &&
                  ops.read(ops.context, summary, read.data(), read.size()) && read == first &&
                  files.ReadOpensForTest() == before + 3U,
              "root write invalidates the retained summary alias of the same physical file");
        Check(ops.read(ops.context, tail, read.data(), read.size()) &&
                  read == std::array<uint8_t, 4U>{{255U, 255U, 255U, 255U}} &&
                  ops.write(ops.context, tail, second.data(), second.size()) &&
                  ops.sync(ops.context) &&
                  ops.read(ops.context, tail, read.data(), read.size()) && read == second &&
                  files.ReadOpensForTest() == before + 4U,
              "second summary page growth cannot reuse the prior cached size");
        const auto other = fixed::Layout::RootOffset(1U - resident, 0U);
        Check(ops.write(ops.context, other, &rootByte, 1U) && ops.sync(ops.context) &&
                  ops.read(ops.context, tail, read.data(), read.size()) && read == second &&
                  files.ReadOpensForTest() == before + 4U,
              "write to the other physical root does not evict a valid retained summary");
        uint8_t currentRoot = 0U;
        Check(ops.read(ops.context, root, &currentRoot, 1U) && currentRoot == rootByte &&
                  ops.read(ops.context, root, &currentRoot, 1U) &&
                  files.ReadOpensForTest() == before + 6U,
              "authoritative root reads always reopen instead of using retained summary data");
        files.CloseCachedHandles();
        Check(ops.read(ops.context, summary, read.data(), read.size()) && read == first &&
                  files.ReadOpensForTest() == before + 7U,
              "explicit owner teardown closes the retained summary handle");
        Check(files.Remove() && ops.read(ops.context, summary, read.data(), read.size()) &&
                  read == std::array<uint8_t, 4U>{{255U, 255U, 255U, 255U}},
              "removal closes summary handles before deleting their backing roots");
    }
}

struct CountedFrameFile final {
    modules::litchcraft_frame_store::FrameStore::Ops file;
    size_t reads = 0U, readBytes = 0U, frameLoads = 0U;
    size_t writes = 0U, writeBytes = 0U, syncs = 0U;
    static bool Read(void* context, size_t offset, uint8_t* out, size_t size) {
        auto& self = *static_cast<CountedFrameFile*>(context);
        ++self.reads;
        self.readBytes += size;
        if (size == modules::litchcraft_frame_store_layout::FRAME_SLOT_BYTES) ++self.frameLoads;
        return self.file.read(self.file.context, offset, out, size);
    }
    static bool Write(void* context, size_t offset, const uint8_t* bytes, size_t size) {
        auto& self = *static_cast<CountedFrameFile*>(context);
        ++self.writes;
        self.writeBytes += size;
        return self.file.write(self.file.context, offset, bytes, size);
    }
    static bool Sync(void* context) {
        auto& self = *static_cast<CountedFrameFile*>(context);
        ++self.syncs;
        return self.file.sync(self.file.context);
    }
    modules::litchcraft_frame_store::FrameStore::Ops Ops() {
        return {this, &Read, &Write, &Sync};
    }
    void Reset() { reads = readBytes = frameLoads = writes = writeBytes = syncs = 0U; }
};

void RunOrdinaryPaintRealFileIoCorpus() {
    namespace store = modules::litchcraft_frame_store;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    for (const uint16_t count : {1U, 4U, 256U}) {
        firmware::LitchcraftFrameFile files;
        Check(files.Remove(), "real paint IO corpus isolates file banks");
        CountedFrameFile io{files.StoreOps()};
        store::FrameStore frames(io.Ops());
        store::Manifest manifest{};
        manifest.frameCount = count;
        manifest.active = true;
        manifest.predecessorDeltas = true;
        store::Frame first{};
        first.emitters[0].color = {20U, 21U, 22U};
        store::TransactionToken seed{};
        Check(frames.Recover() == store::Status::NoArtwork &&
                  frames.Begin(manifest, 0U, 21U, seed) == store::Status::Ok,
              "real paint IO corpus starts valid predecessor");
        for (uint16_t index = 0U; index < count; ++index) {
            Check(frames.WriteFrame(seed, index, first) == store::Status::Ok,
                  "real paint IO corpus seeds authored frames");
        }
        Check(frames.Commit(seed) == store::Status::Ok, "real paint IO corpus commits");
        Stream pager;
        Check(pager.StageFirstFrame(frames.CurrentManifest(), frames.Generation(), first),
              "real paint IO corpus adopts live cache");
        for (const bool changed : {false, true}) {
            io.Reset();
            const auto opens = files.ReadOpensForTest();
            const auto generation = frames.Generation();
            auto color = first.emitters[0].color;
            if (changed) color = {99U, 17U, 44U};
            const auto started = std::chrono::steady_clock::now();
            const bool painted = pager.PaintDisplayed(0U, 0U, 1U, color, false);
            pager.RequestImmediateSave();
            (void)pager.Service(frames, false);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::printf("[REAL_PAINT_IO] frames=%u changed=%u reads=%zu read_bytes=%zu frame_loads=%zu read_opens=%u writes=%zu write_bytes=%zu syncs=%zu saved_delta=%u host_service_us=%lld\n",
                count, changed ? 1U : 0U, io.reads, io.readBytes, io.frameLoads,
                files.ReadOpensForTest() - opens, io.writes, io.writeBytes, io.syncs,
                frames.Generation() - generation, static_cast<long long>(elapsed));
            Check(painted == changed && !pager.HasUnpersistedEdit(),
                  "real paint IO corpus preserves no-op and durable truth");
            Check(changed || io.writes == 0U, "identical real-file paint writes nothing");
            Check(files.ReadOpensForTest() - opens <= 10U,
                  "changed-frame save reuses one retained summary handle instead of reopening every chunk");
            if (changed) {
                Check(io.writes == 8U && io.writeBytes == 4589U && io.syncs == 4U,
                      "read-handle reuse preserves verified body, summary and root durability writes");
            }
        }
        for (uint16_t edit = 0U; edit < 3U; ++edit) {
            const uint16_t index = count == 1U ? 0U :
                static_cast<uint16_t>(edit * (count - 1U) / 2U);
            Check(pager.Request(index), "repeat paint IO corpus requests authored frame");
            for (uint8_t slice = 0U; slice < 3U; ++slice) {
                (void)pager.Service(frames, false, Stream::ServiceMode::PagingOnly);
                if (pager.TryPresentRequested()) break;
            }
            store::Frame displayed{};
            uint16_t displayedIndex = 0U;
            Check(pager.CopyDisplayed(displayed, displayedIndex) && displayedIndex == index,
                  "repeat paint IO corpus paints the production paged frame");
            io.Reset();
            const auto opens = files.ReadOpensForTest();
            const auto generation = frames.Generation();
            auto color = displayed.emitters[0].color;
            color.r = static_cast<uint8_t>(111U + edit);
            Check(pager.PaintDisplayed(index, 0U, 1U, color, false),
                  "repeat paint IO corpus changes a real displayed frame");
            // Force only the save eligibility boundary; Service and all storage
            // calls below are production. The ordinary five-second idle wait
            // and physical S3 elapsed time are deliberately not benchmarked here.
            pager.RequestImmediateSave();
            (void)pager.Service(frames, false);
            std::printf("[REAL_PAINT_REPEAT_IO] frames=%u index=%u edit=%u reads=%zu read_bytes=%zu frame_loads=%zu read_opens=%u writes=%zu write_bytes=%zu syncs=%zu saved_delta=%u\n",
                count, index, edit, io.reads, io.readBytes, io.frameLoads,
                files.ReadOpensForTest() - opens, io.writes, io.writeBytes, io.syncs,
                frames.Generation() - generation);
            // Before Begin, equality reads one validated logical slot. One
            // fold plus one save verifies two slots and reads at most three
            // cached/prefetched frames, four summary pages and ten root copies.
            // Six slot-sized regions bound bytes; four complete-slot reads
            // exclude the two chunked verification readbacks. Neither bound
            // grows with document frameCount.
            constexpr size_t readBudget =
                6U * modules::litchcraft_frame_store_layout::FRAME_SLOT_BYTES +
                4U * store::WIRE_SUMMARY_PAGE_BYTES +
                10U * modules::litchcraft_frame_store_layout::ROOT_BYTES;
            Check(!pager.HasUnpersistedEdit() && frames.Generation() == generation + 1U &&
                      io.frameLoads <= 4U && io.readBytes <= readBudget &&
                      io.writes == 17U && io.writeBytes == 9562U && io.syncs == 8U &&
                      files.ReadOpensForTest() - opens <= 25U,
                  "repeated saves and stage folds stay bounded independently of document length");
            io.Reset();
            Check(!pager.PaintDisplayed(index, 0U, 1U, color, false),
                  "repeating the saved colour remains a no-op");
            pager.RequestImmediateSave();
            (void)pager.Service(frames, false);
            Check(io.writes == 0U && frames.Generation() == generation + 1U,
                  "post-save no-op never folds or republishes the saved root");
        }
        Check(files.Remove(), "real paint IO corpus removes banks");
    }
}

void RunInterruptedStrokeReplacementJourney() {
    const uint32_t entryNowMs = fxe::Millis();
    namespace store = modules::litchcraft_frame_store;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "New replacement probe isolates real file banks");
    store::FrameStore frames(files.StoreOps());
    store::Manifest manifest{};
    manifest.active = true;
    manifest.frameCount = 256U;
    store::Frame first{};
    first.emitters[0].color = {72U, 3U, 4U};
    store::TransactionToken seed{};
    Check(frames.Recover() == store::Status::NoArtwork &&
              frames.Begin(manifest, 0U, 1U, seed) == store::Status::Ok,
          "New replacement probe seeds a large predecessor");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        Check(frames.WriteFrame(seed, index, first) == store::Status::Ok,
              "New replacement probe writes predecessor frame");
    }
    Check(frames.Commit(seed) == store::Status::Ok,
          "New replacement probe commits predecessor");
    Stream stream;
    Check(stream.StageFirstFrame(frames.CurrentManifest(), frames.Generation(), first) &&
              stream.NewBlankDocument(frames.Generation(), stream.LiveRevision(), {}) ==
                  Stream::GuardedEditResult::Changed,
          "New replacement probe accepts instant New");
    (void)stream.Service(frames, false);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(config);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(&stream, nullptr, nullptr);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK &&
              board.Litchcraft().IsActive() && !stream.HasPendingBlankDocument(),
          "interrupted replacement adopts persisted New through production runtime");
    std::array<uint8_t, 8U> guard{};
    fxe::WriteU32LE(frames.Generation(), guard.data());
    fxe::WriteU32LE(stream.LiveRevision(), guard.data() + 4U);
    const uint8_t paint[] = {0U, 1U, 0U, 1U, 15U, 99U, 17U, 44U, 7U};
    Check(board.Litchcraft().ApplyConnectionOrdered(8U, guard.data(), guard.size(), 101U) ==
              modules::LitchcraftSession::Status::Ok &&
              board.Litchcraft().ApplyConnectionOrdered(3U, paint, sizeof(paint), 101U) ==
              modules::LitchcraftSession::Status::Ok,
          "interrupted replacement accepts held paint through guarded production input");
    stream.RequestImmediateSave();
    for (uint8_t attempt = 0U; attempt < 12U && stream.HasUnpersistedEdit(); ++attempt) {
        (void)stream.Service(frames, false);
    }
    Check(!stream.HasUnpersistedEdit(),
          "New replacement probe saves paint while stroke remains open");
    store::Frame incoming{};
    incoming.emitters[0].color = {44U, 55U, 66U};
    std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES + store::WIRE_RECORD_HEADER_BYTES> body{};
    size_t encoded = 0U;
    Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
              nullptr, incoming, body.data() + store::WIRE_RECORD_HEADER_BYTES,
              store::WIRE_FRAME_MAX_BYTES, encoded),
          "New replacement probe encodes app's one-frame replacement");
    fxe::WriteU16LE(static_cast<uint16_t>(encoded), body.data());
    fxe::WriteU32LE(fxe::Crc32(body.data() + store::WIRE_RECORD_HEADER_BYTES, encoded),
                    body.data() + 2U);
    const size_t bodySize = encoded + store::WIRE_RECORD_HEADER_BYTES;
    manifest.frameCount = 1U;
    store::TransactionToken transfer{};
    const auto begin = frames.BeginWire(manifest, frames.Generation(), 2U,
        static_cast<uint32_t>(bodySize), fxe::Crc32(body.data(), bodySize), transfer);
    const auto write = frames.WriteBodyChunk(transfer, 0U, body.data(), bodySize);
    const auto prepare = frames.PrepareCommit(transfer);
    const bool openAccepted = stream.ReserveReplacement(frames.Generation(), manifest,
                                                        frames.IncomingGeneration());
    Check(begin == store::Status::Ok && write == store::Status::Ok &&
              prepare == store::Status::Ok && !openAccepted,
          "clean open stroke reproduces post-prepare replacement refusal");
    const uint32_t paintedFingerprint = stream.DisplayedFingerprint();
    const uint8_t tool[] = {0U, 15U, 99U, 17U, 44U};
    Check(board.Litchcraft().ApplyConnectionOrdered(5U, tool, sizeof(tool), 101U) ==
              modules::LitchcraftSession::Status::Ok &&
              stream.DisplayedFingerprint() == paintedFingerprint &&
              stream.UndoCount() == 1U,
          "explicit Use app Tool closes one undoable stroke without repainting");
    const bool closedAccepted = stream.ReserveReplacement(frames.Generation(), manifest,
                                                          frames.IncomingGeneration());
    Check(closedAccepted, "closing stroke alone admits the same prepared replacement");
    std::printf("[NEW_PAINT_REPLACEMENT_FILE_PROBE] begin=%u write=%u prepare=%u "
                "dirty=%u open_admitted=%u closed_admitted=%u generation=%u\n",
        static_cast<unsigned>(begin), static_cast<unsigned>(write),
        static_cast<unsigned>(prepare), stream.HasUnpersistedEdit() ? 1U : 0U,
        openAccepted ? 1U : 0U, closedAccepted ? 1U : 0U, frames.Generation());
    Check(frames.Abort(transfer) == store::Status::Ok && files.Remove(),
          "New replacement probe removes isolated banks");
    fxe::ManualMillis::SetNowMs(entryNowMs);
}

void RunQuietPlaybackExpiryWorkerJourney() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    const uint32_t entryNowMs = fxe::Millis();
    fxe::ManualMillis::SetNowMs(0U);
    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "quiet playback starts from blank banks");
    store::FrameStore seed(frameFiles.StoreOps());
    Check(seed.Recover() == store::Status::NoArtwork,
          "quiet playback recovers blank storage");
    store::Manifest manifest{};
    manifest.frameCount = store::FRAME_COUNT;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.sparkleSeed = 0x13572468U;
    store::TransactionToken token{};
    Check(seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "quiet playback seed begins");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        store::Frame frame{};
        frame.durationMs = static_cast<uint16_t>(50U + index);
        frame.emitters[0].color = {static_cast<uint8_t>(30U + index), 1U, 2U};
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "quiet playback seed writes authored frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "quiet playback seed commits");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_quiet_playback_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "quiet playback loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "quiet playback starts actual worker");
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == store::FRAME_COUNT &&
            stream.TryPresentRequested() && worker_fixture::atWait.load();
    }, "quiet playback boot recovery");
    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession board(virtualConfig);
    board.ObserveBootHallMask(0U);
    board.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        }, &worker);
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK,
          "quiet playback adopts committed artwork");
    const uint8_t playStored[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, playStored, sizeof(playStored)) ==
              modules::LitchcraftSession::Status::Ok,
          "quiet playback starts through explicit Play");
    worker_fixture::Wait([&] {
        return !worker_fixture::notified && worker_fixture::atWait.load() &&
            !worker.GetDiagnostics().frameStorePlaybackPending;
    }, "quiet playback prefetch settles");
    const uint32_t accessesBefore = worker.GetDiagnostics().flashAccesses;
    uint32_t elapsedMs = 0U;
    uint32_t boundaries = 0U;
    uint32_t maximumBoundaryAccesses = 0U;
    uint16_t currentFrame = 0U;
    bool exactBoundaries = true;
    // No ingress, Inspect, Snapshot or keepalive is sent during this journey.
    // Each authored next frame must already be ready when its clock expires.
    while (elapsedMs < 21000U) {
        const uint32_t durationMs = 50U + currentFrame;
        const uint32_t accesses = worker.GetDiagnostics().flashAccesses;
        const uint32_t waitEntries = worker_fixture::waitEntries.load();
        currentFrame = static_cast<uint16_t>((currentFrame + 1U) % store::FRAME_COUNT);
        exactBoundaries = exactBoundaries &&
            board.Advance(durationMs) == VIRTUAL_LITCH_OK &&
            stream.DisplayedIndex() == currentFrame &&
            board.SemanticFrame().board[0][0].r ==
                static_cast<uint8_t>(30U + currentFrame);
        worker_fixture::Wait([&] {
            return worker_fixture::waitEntries.load() > waitEntries &&
                !worker_fixture::notified && worker_fixture::atWait.load() &&
                !worker.GetDiagnostics().frameStorePlaybackPending;
        }, "quiet playback pages the next authored boundary");
        const uint32_t boundaryAccesses = worker.GetDiagnostics().flashAccesses - accesses;
        maximumBoundaryAccesses = std::max(maximumBoundaryAccesses, boundaryAccesses);
        elapsedMs += durationMs;
        ++boundaries;
        Check(stream.Generation() == 1U && worker.GetDiagnostics().frameStoreStorageActive,
              "quiet playback preserves resident authority");
    }
    const uint32_t accesses = worker.GetDiagnostics().flashAccesses - accessesBefore;
    Check(exactBoundaries, "quiet playback misses no authored frame boundary");
    Check(maximumBoundaryAccesses <= 1U && accesses <= boundaries,
          "quiet playback has no ten-second close/recovery/reopen flash burst");
    std::printf("[FRAME256_QUIET_PLAYBACK] elapsed=%lu boundaries=%lu accesses=%lu max-boundary-accesses=%lu exact=%u generation=%lu\n",
        static_cast<unsigned long>(elapsedMs), static_cast<unsigned long>(boundaries),
        static_cast<unsigned long>(accesses), static_cast<unsigned long>(maximumBoundaryAccesses),
        exactBoundaries ? 1U : 0U, static_cast<unsigned long>(stream.Generation()));
    const uint8_t pauseStored[] = {
        2U, static_cast<uint8_t>(currentFrame), 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, pauseStored, sizeof(pauseStored)) ==
              modules::LitchcraftSession::Status::Ok,
          "quiet playback pauses through the actual ordered owner");
    worker_fixture::Wait([&] {
        return worker.IsIdle() && !worker_fixture::notified && worker_fixture::atWait.load();
    }, "paused quiet playback releases its resident storage");
    Check(!worker.GetDiagnostics().frameStoreStorageActive,
          "pause releases quiet resident storage without waiting for expiry");
    const uint8_t resumeStored[] = {
        1U, static_cast<uint8_t>(currentFrame), 1U, 0U, 0U, 0U};
    Check(board.Litchcraft().ApplyConnectionOrdered(
              2U, resumeStored, sizeof(resumeStored)) ==
              modules::LitchcraftSession::Status::Ok,
          "quiet playback resumes through the actual ordered owner");
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreStorageActive &&
            !worker.GetDiagnostics().frameStorePlaybackPending &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "quiet playback restores residency after pause");
    // Playback residency must not exempt a genuinely abandoned transaction or
    // reverse read from its existing owner timeout, including pending receipts.
    modules::BleOtaChunkQueue queue;
    Check(queue.Begin(), "quiet playback timeout queue begins");
    constexpr uint32_t SESSION = 0x31415926U;
    constexpr uint32_t OWNER = 17U;
    const store::StoreProjection projection{SESSION, currentFrame, 3U, true, true};
    std::array<uint8_t, 53U> begin{};
    begin[0U] = 0x4dU;
    begin[1U] = 2U;
    begin[2U] = store::STORE_BEGIN;
    fxe::WriteU32LE(SESSION, begin.data() + 3U);
    fxe::WriteU16LE(2U, begin.data() + 7U);
    fxe::WriteU32LE(1U, begin.data() + 9U);
    fxe::WriteU32LE(6U, begin.data() + 13U);
    fxe::WriteU16LE(1U, begin.data() + 21U);
    begin[23U] = 3U;
    begin[24U] = 3U;
    fxe::WriteU32LE(1U, begin.data() + 25U);
    Check(queue.Enqueue(begin.data(), begin.size(), 1U),
          "playing timeout admits replacement Begin");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    worker_fixture::Wait([&] {
        return !queue.TryPeekLitchcraftFrameStore() &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "playing abandoned optimistic Begin is admitted");
    std::array<uint8_t, 13U> checkpoint{};
    checkpoint[0U] = 0x4dU;
    checkpoint[1U] = 2U;
    checkpoint[2U] = store::STORE_CHECKPOINT;
    fxe::WriteU32LE(SESSION, checkpoint.data() + 3U);
    fxe::WriteU16LE(2U, checkpoint.data() + 7U);
    Check(queue.Enqueue(checkpoint.data(), checkpoint.size(), 2U),
          "playing timeout verifies the admitted transaction");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
            response[2U] == store::STORE_CHECKPOINT_RECEIPT &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "playing abandoned upload holds its checkpoint receipt");
    Check(responseSize > 13U && response[13U] == 0U &&
              board.Litchcraft().IsAnimationPlaying(),
          "abandoned upload is real and playback remains desired");
    fxe::ManualMillis::AdvanceMs(10001U);
    worker_fixture::Wait([&] {
        return !worker.GetDiagnostics().frameStoreStorageActive &&
            !worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "playing abandoned upload expires");
    Check(stream.Generation() == 1U && board.Litchcraft().IsAnimationPlaying(),
          "upload expiry preserves committed artwork and playback intent");
    worker.NotifyLitchcraftFramePlayback(true);
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().frameStoreStorageActive &&
            !worker.GetDiagnostics().frameStorePlaybackPending &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "playing artwork resumes residency after transfer expiry");
    std::array<uint8_t, 17U> snapshot{};
    snapshot[0U] = 0x4dU;
    snapshot[1U] = 2U;
    snapshot[2U] = store::STORE_SNAPSHOT;
    fxe::WriteU32LE(SESSION, snapshot.data() + 3U);
    fxe::WriteU16LE(3U, snapshot.data() + 7U);
    fxe::WriteU32LE(1U, snapshot.data() + 9U);
    Check(queue.Enqueue(snapshot.data(), snapshot.size(), 3U),
          "playing timeout admits reverse Snapshot");
    worker.NotifyLitchcraftFrameIngress(queue, projection, OWNER);
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "playing abandoned Snapshot holds its reverse credits");
    Check(responseSize > 60U && response[2U] == store::STORE_SNAPSHOT_CHUNK &&
              response[13U] == 0U && board.Litchcraft().IsAnimationPlaying(),
          "abandoned read is real and playback remains desired");
    fxe::ManualMillis::AdvanceMs(10001U);
    worker_fixture::Wait([&] {
        return !worker.GetDiagnostics().frameStoreStorageActive &&
            !worker.PeekLitchcraftFrameResponse(response.data(), response.size(), responseSize) &&
            worker_fixture::atWait.load();
    }, "playing abandoned reverse read expires");
    Check(stream.Generation() == 1U && board.Litchcraft().IsAnimationPlaying(),
          "read expiry preserves committed artwork and playback intent");
    std::puts("[FRAME256_QUIET_PLAYBACK] abandoned-upload=expired abandoned-read=expired playback-desired=1");
    worker.NotifyLitchcraftFramePlayback(false);
    worker_fixture::Wait([&] {
        return worker.IsIdle() && !worker_fixture::notified && worker_fixture::atWait.load();
    }, "stopped quiet playback releases storage residency");
    Check(!worker.GetDiagnostics().frameStoreStorageActive,
          "stopped quiet playback retains bounded released residency");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(), "quiet playback removes internal banks");
    fxe::ManualMillis::SetNowMs(entryNowMs);
}

void RunFramePlaybackWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);

    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "playback worker fixture starts from blank banks");
    store::FrameStore seed(frameFiles.StoreOps());
    Check(seed.Recover() == store::Status::NoArtwork,
          "playback worker seed recovers blank storage");
    store::Manifest manifest{};
    manifest.frameCount = store::FRAME_COUNT;
    manifest.active = true;
    manifest.playbackOnEntry = true;
    manifest.sparkleSeed = 0x13572468U;
    store::TransactionToken token{};
    Check(seed.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
          "playback worker seed begins");
    for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
        store::Frame frame{};
        frame.durationMs = static_cast<uint16_t>(50U + index);
        frame.emitters[0].color = {
            static_cast<uint8_t>(30U + index), 1U, 2U};
        Check(seed.WriteFrame(token, index, frame) == store::Status::Ok,
              "playback worker seed writes frame");
    }
    Check(seed.Commit(token) == store::Status::Ok,
          "playback worker seed commits");

    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_frame_playback_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "playback worker fixture loads settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "playback worker starts on the existing settings task");
    worker_fixture::Wait([&] {
        return worker_fixture::atWait.load();
    }, "playback worker waits for runtime readiness");
    Check(worker.LitchcraftFrameStream().FrameCount() == 0U,
          "boot frame recovery cannot run before the live render loop is ready");
    constexpr const char* backupRoot =
        "build/litchcraft_frames.r0.transient-read-backup";
    const char* const root = firmware::LitchcraftFrameFile::DEFAULT_ROOT0_PATH;
    (void)std::remove(backupRoot);
    Check(std::rename(root, backupRoot) == 0 && ::mkdir(root, 0700) == 0,
          "playback boot fault turns the first root read into a transient I/O failure");
    arbiter.restoreFrom = backupRoot;
    arbiter.restoreTo = root;
    arbiter.restoreRootAfterFlash.store(true);
    worker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = worker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == store::FRAME_COUNT &&
            stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "transient boot read recovers without a phone or playback command");
    Check(arbiter.restoredRoot.load(),
          "boot recovery retries after the controlled transient root read failure");
    worker.NotifyLitchcraftFramePlayback(true);
    worker_fixture::Wait([&] {
        return !worker.IsIdle() && worker_fixture::atWait.load();
    }, "playing boot recovery keeps Store resident");
    store::Frame displayed{};
    uint16_t displayedIndex = 99U;
    Check(stream.CopyDisplayed(displayed, displayedIndex) &&
              displayedIndex == 0U && displayed.emitters[0].color.r == 30U &&
              !worker.IsIdle(),
          "playing boot recovery keeps Store resident across frame boundaries");
    Check(!worker.IsLitchcraftBulkTransferActive(),
          "ordinary resident playback and startup recovery are not large transfers");

    // A reverse document reconciliation reads the same resident Store. Its
    // response must not tear down playback's decode cursor while the board is
    // still playing; dense frames need more than one bounded storage slice.
    modules::BleOtaChunkQueue snapshotQueue;
    Check(snapshotQueue.Begin(),
          "playing snapshot fixture starts the shared bulk queue");
    constexpr uint32_t SNAPSHOT_SESSION = 0x31415926U;
    constexpr uint32_t SNAPSHOT_OWNER = 17U;
    std::array<uint8_t, 17U> snapshot{};
    snapshot[0U] = 0x4dU;
    snapshot[1U] = 2U;
    snapshot[2U] = modules::litchcraft_frame_store::STORE_SNAPSHOT;
    fxe::WriteU32LE(SNAPSHOT_SESSION, snapshot.data() + 3U);
    fxe::WriteU16LE(1U, snapshot.data() + 7U);
    fxe::WriteU32LE(1U, snapshot.data() + 9U);
    fxe::WriteU32LE(0U, snapshot.data() + 13U);
    Check(snapshotQueue.Enqueue(snapshot.data(), snapshot.size(), 1U),
          "playing snapshot fixture enqueues reconciliation readback");
    arbiter.BlockNextFlash();
    worker.NotifyLitchcraftFrameIngress(
        snapshotQueue,
        {SNAPSHOT_SESSION, 0U, 3U, true, true},
        SNAPSHOT_OWNER);
    arbiter.WaitForBlockedFlash();
    // Playback can cross a frame boundary while the storage worker is reading
    // this response. That cursor movement must not invalidate and consume the
    // already-admitted snapshot request.
    worker.NotifyLitchcraftFrameIngress(
        snapshotQueue,
        {SNAPSHOT_SESSION, 1U, 3U, true, true},
        SNAPSHOT_OWNER);
    arbiter.ReleaseBlockedFlash();
    std::array<uint8_t,
               modules::litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES>
        snapshotResponse{};
    size_t snapshotResponseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   snapshotResponse.data(), snapshotResponse.size(),
                   snapshotResponseSize) && worker_fixture::atWait.load();
    }, "playing snapshot response");
    Check(snapshotResponseSize > 60U &&
              snapshotResponse[2U] ==
                  modules::litchcraft_frame_store::STORE_SNAPSHOT_CHUNK &&
              !worker.IsIdle(),
          "reverse snapshot response preserves the playback-owned Store");
    worker.AcceptLitchcraftFrameResponse();

    // A replacement may take minutes. Suspend cosmetic artwork clocks and
    // lookahead through the same projection used by the main runtime; keep
    // foreground status, preferences and cancellation live.
    virtual_litch_config transferConfig{};
    virtual_litch_default_config(&transferConfig);
    transferConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession transferJourney(transferConfig);
    transferJourney.ObserveBootHallMask(0U);
    transferJourney.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        },
        &worker);
    Check(transferJourney.Advance(0U) == VIRTUAL_LITCH_OK &&
              transferJourney.SemanticFrame().board[0][0].r == 30U,
          "open-transfer journey starts on committed frame zero");
    const uint8_t playStored[] = {1U, 0U, 1U, 0U, 0U, 0U};
    Check(transferJourney.Litchcraft().ApplyConnectionOrdered(
              2U, playStored, sizeof(playStored)) ==
              modules::LitchcraftSession::Status::Ok,
          "open-transfer fixture starts playback through explicit Play");
    std::array<uint8_t, 53U> replacementBegin{};
    replacementBegin[0U] = 0x4dU;
    replacementBegin[1U] = 2U;
    replacementBegin[2U] = modules::litchcraft_frame_store::STORE_BEGIN;
    fxe::WriteU32LE(SNAPSHOT_SESSION, replacementBegin.data() + 3U);
    fxe::WriteU16LE(2U, replacementBegin.data() + 7U);
    fxe::WriteU32LE(1U, replacementBegin.data() + 9U);
    fxe::WriteU32LE(6U, replacementBegin.data() + 13U);
    fxe::WriteU16LE(1U, replacementBegin.data() + 21U);
    replacementBegin[23U] = 3U;
    replacementBegin[24U] = 3U;
    fxe::WriteU32LE(1U, replacementBegin.data() + 25U);
    Check(snapshotQueue.Enqueue(replacementBegin.data(), replacementBegin.size(),
                                2U),
          "playing replacement fixture begins a background transaction");
    worker.NotifyLitchcraftFrameIngress(
        snapshotQueue, {SNAPSHOT_SESSION, 0U, 3U, true, true},
        SNAPSHOT_OWNER);
    worker_fixture::Wait([&] {
        return !snapshotQueue.TryPeekLitchcraftFrameStore() &&
            worker_fixture::atWait.load();
    }, "playing replacement Begin reaches the storage worker");
    Check(worker.IsLitchcraftBulkTransferActive(),
          "actual admitted Begin remains bulk between incoming chunks");
    transferJourney.Litchcraft().SetBulkTransferActive(worker.IsLitchcraftBulkTransferActive());
    worker.SetLitchcraftPlaybackSuspended(true);
    const auto heldElapsed = transferJourney.Litchcraft().AnimationElapsedMs();
    const auto heldPhase = transferJourney.Litchcraft().AnimationFrameElapsedMs();
    const uint32_t savesBeforeUploadPreference = worker.GetCompletedSaveCount();
    settings.Doc()["BRIGHTNESS"] = 12U;
    Check(settings.SaveNow(), "ordinary preference is admitted during an open replacement upload");
    worker_fixture::Wait([&] {
        return worker.GetCompletedSaveCount() > savesBeforeUploadPreference && worker_fixture::atWait.load();
    }, "preference saves without cancelling replacement upload");
    fxe::SettingsStore uploadPreference(config);
    Check(uploadPreference.Load() && uploadPreference.ReadValue("BRIGHTNESS", 0U) == 12U &&
              stream.Generation() == 1U && worker.GetDiagnostics().frameStoreStorageActive,
          "preference persists while playback and incoming Store authority remain intact");
    std::array<uint8_t, 10U> transferInspect{};
    transferInspect[0U] = 0x4dU;
    transferInspect[1U] = 2U;
    transferInspect[2U] = 1U;
    fxe::WriteU32LE(SNAPSHOT_SESSION, transferInspect.data() + 3U);
    transferInspect[9U] = modules::litchcraft_frame_store::RESIDENT_LISTING_PROBE;
    fxe::WriteU16LE(3U, transferInspect.data() + 7U);
    Check(snapshotQueue.Enqueue(transferInspect.data(), transferInspect.size(),
                                3U),
          "open upload queues its first foreground Inspect");
    fxe::WriteU16LE(4U, transferInspect.data() + 7U);
    Check(snapshotQueue.Enqueue(transferInspect.data(), transferInspect.size(),
                                4U),
          "open upload leaves another foreground request pending");
    worker.NotifyLitchcraftFrameIngress(
        snapshotQueue, {SNAPSHOT_SESSION, 0U, 3U, true, true},
        SNAPSHOT_OWNER);
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   snapshotResponse.data(), snapshotResponse.size(),
                   snapshotResponseSize) &&
            snapshotQueue.TryPeekLitchcraftFrameStore() &&
            worker_fixture::atWait.load();
    }, "open upload retains both an undelivered response and queued ingress");
    Check(transferJourney.Advance(101U) == VIRTUAL_LITCH_OK &&
              transferJourney.SemanticFrame().board[0][0].r == 30U &&
              stream.DisplayedIndex() == 0U && stream.RequestedIndex() == 0U &&
              transferJourney.Litchcraft().IsAnimationPlaying() &&
              transferJourney.Litchcraft().AnimationElapsedMs() == heldElapsed &&
              transferJourney.Litchcraft().AnimationFrameElapsedMs() == heldPhase,
          "admitted upload holds the visible frame and phase with explicit Play intent intact");
    std::puts(
        "[FRAME256_PLAYBACK_JOURNEY] background-upload | occupancy=empty authority=Litchcraft/active "
        "accepted=Begin frame=0 pixel0=30 playing-intent=1 cosmetic=held "
        "status=healthy ingress=queued response=pending next=continue-or-cancel");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   snapshotResponse.data(), snapshotResponse.size(),
                   snapshotResponseSize) &&
            fxe::ReadU16LE(snapshotResponse.data() + 11U) == 4U &&
            worker_fixture::atWait.load();
    }, "second Inspect completes while artwork is held");
    worker.AcceptLitchcraftFrameResponse();

    // Keep the same replacement transaction alive for more than 100 seconds
    // of authored board time. Checkpoints model an app that continues making
    // progress slowly; authored frame/effect clocks stay held while ordinary
    // status and cancellation remain live, without sleeping for 100 seconds.
    uint32_t playbackElapsedMs = 101U;
    uint32_t nextKeepaliveMs = 5000U;
    uint32_t nextOrdinal = 5U;
    uint16_t currentFrame = 0U;
    bool fullPlaybackHealthy = true;
    while (playbackElapsedMs < 100000U && fullPlaybackHealthy) {
        const uint32_t durationMs = 50U + currentFrame;
        if (playbackElapsedMs + durationMs >= nextKeepaliveMs) {
            std::array<uint8_t, 13U> checkpoint{};
            checkpoint[0U] = 0x4dU;
            checkpoint[1U] = 2U;
            checkpoint[2U] =
                modules::litchcraft_frame_store::STORE_CHECKPOINT;
            fxe::WriteU32LE(SNAPSHOT_SESSION, checkpoint.data() + 3U);
            fxe::WriteU16LE(2U, checkpoint.data() + 7U);
            fullPlaybackHealthy = snapshotQueue.Enqueue(
                checkpoint.data(), checkpoint.size(), nextOrdinal++);
            worker.NotifyLitchcraftFrameIngress(
                snapshotQueue, {SNAPSHOT_SESSION, currentFrame, 3U, true, true},
                SNAPSHOT_OWNER);
            worker_fixture::Wait([&] {
                snapshotResponseSize = 0U;
                return worker.PeekLitchcraftFrameResponse(
                           snapshotResponse.data(), snapshotResponse.size(),
                           snapshotResponseSize) &&
                    snapshotResponse[2U] == modules::litchcraft_frame_store::
                        STORE_CHECKPOINT_RECEIPT &&
                    snapshotResponse[13U] == 0U &&
                    worker_fixture::atWait.load();
            }, "long replacement transfer checkpoint");
            worker.AcceptLitchcraftFrameResponse();
            nextKeepaliveMs += 5000U;
        }
        fullPlaybackHealthy = fullPlaybackHealthy &&
            transferJourney.Advance(durationMs) == VIRTUAL_LITCH_OK &&
            stream.DisplayedIndex() == currentFrame && stream.RequestedIndex() == currentFrame &&
            stream.Generation() == 1U && worker.IsLitchcraftBulkTransferActive() &&
            transferJourney.Litchcraft().AnimationElapsedMs() == heldElapsed &&
            transferJourney.Litchcraft().AnimationFrameElapsedMs() == heldPhase &&
            transferJourney.SemanticFrame().board[0][0].r == 30U;
        playbackElapsedMs += durationMs;
    }
    Check(fullPlaybackHealthy && playbackElapsedMs >= 100000U,
          "committed artwork holds its complete phase through a 100-second active replacement");
    std::printf(
        "[FRAME256_PLAYBACK_JOURNEY] sustained-upload | elapsed=%lu "
        "frame=%u generation=1 ingress=checkpointed next=cancel\n",
        static_cast<unsigned long>(playbackElapsedMs),
        static_cast<unsigned>(currentFrame));
    std::array<uint8_t, 9U> replacementCancel{};
    replacementCancel[0U] = 0x4dU;
    replacementCancel[1U] = 2U;
    replacementCancel[2U] = modules::litchcraft_frame_store::STORE_CANCEL;
    fxe::WriteU32LE(SNAPSHOT_SESSION, replacementCancel.data() + 3U);
    fxe::WriteU16LE(2U, replacementCancel.data() + 7U);
    worker.NotifyLitchcraftFramePriority(
        replacementCancel.data(), replacementCancel.size(), SNAPSHOT_OWNER);
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   snapshotResponse.data(), snapshotResponse.size(),
                   snapshotResponseSize) &&
            snapshotResponse[2U] == modules::litchcraft_frame_store::
                STORE_STATE &&
            snapshotResponse[13U] == 0U &&
            worker_fixture::atWait.load();
    }, "playing replacement Cancel succeeds after 100 seconds");
    worker.AcceptLitchcraftFrameResponse();
    Check(!worker.IsLitchcraftBulkTransferActive(),
          "Cancel retires actual upload activity without classifying its status as bulk");
    transferJourney.Litchcraft().SetBulkTransferActive(false);
    worker.SetLitchcraftPlaybackSuspended(false);
    Check(transferJourney.Advance(49U - heldPhase) == VIRTUAL_LITCH_OK &&
              stream.DisplayedIndex() == 0U && transferJourney.Litchcraft().IsAnimationPlaying(),
          "Cancel resumes only the original remaining dwell without 100 seconds of catch-up");
    Check(transferJourney.Advance(1U) == VIRTUAL_LITCH_OK &&
              transferJourney.SemanticFrame().board[0][0].r == 31U,
          "resumed artwork crosses its next authored boundary exactly once");
    std::puts("[FRAME256_PLAYBACK_JOURNEY] Cancel | accepted=1 bulk=idle frame=1 pixel0=31 "
        "status=healthy playing-intent=1 next=normal-playback no-catchup=1");
    Check(stream.Request(0U), "production journey returns to frame zero");
    worker.NotifyLitchcraftFramePlayback(true);
    worker_fixture::Wait([&] {
        return stream.TryPresentRequested() && worker_fixture::atWait.load();
    }, "production journey restores frame zero after replacement Cancel");

    virtual_litch_config virtualConfig{};
    virtual_litch_default_config(&virtualConfig);
    virtualConfig.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    runtime::VirtualLitchSession virtualLitch(virtualConfig);
    virtualLitch.ObserveBootHallMask(0U);
    virtualLitch.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        },
        &worker);
    Check(virtualLitch.Advance(0U) == VIRTUAL_LITCH_OK &&
              virtualLitch.Litchcraft().IsActive() &&
              virtualLitch.Litchcraft().SessionId() != 0U &&
              virtualLitch.SemanticFrame().board[0][0].r == 30U,
          "production coordinator adopts boot-restored frame zero and renders it");
    Check(!virtualLitch.Litchcraft().IsAnimationPlaying() &&
              virtualLitch.Litchcraft().ApplyConnectionOrdered(
                  2U, playStored, sizeof(playStored)) ==
                  modules::LitchcraftSession::Status::Ok,
          "restored artwork stays paused until explicit Play");
    std::puts(
        "[FRAME256_PLAYBACK_JOURNEY] boot | authority=Litchcraft/active "
        "frame=0 pixel0=30 status=healthy next=timed-boundary");

    // Inject the disk fault only after the production Pause owner closes its
    // native buffered read handle. TwoAhead retains frames zero through two
    // in RAM, so corrupt the first cold frame and its recovery successor.
    const uint8_t pauseBeforeFault[] = {2U, 0U, 1U, 0U, 0U, 0U};
    Check(virtualLitch.Litchcraft().ApplyConnectionOrdered(
              2U, pauseBeforeFault, sizeof(pauseBeforeFault)) ==
              modules::LitchcraftSession::Status::Ok,
          "corrupt-frame fault pauses through the actual ordered owner");
    worker_fixture::Wait([&] {
        return worker.IsIdle() && !worker_fixture::notified && worker_fixture::atWait.load();
    }, "corrupt-frame fault closes the buffered storage reader");
    Check(CorruptStoredFramePayload(
              firmware::LitchcraftFrameFile::DEFAULT_BODY0_PATH, 3U),
          "production journey corrupts the first cold frame after boot prefetch");
    Check(CorruptStoredFramePayload(
              firmware::LitchcraftFrameFile::DEFAULT_BODY0_PATH, 4U),
          "production journey makes the recovery target independently corrupt");
    Check(virtualLitch.Litchcraft().ApplyConnectionOrdered(
              2U, playStored, sizeof(playStored)) ==
              modules::LitchcraftSession::Status::Ok,
          "corrupt-frame journey resumes after controlled disk mutation");
    worker_fixture::Wait([&] {
        return !worker.GetDiagnostics().frameStorePlaybackPending &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "corrupt-frame journey resumes resident paging");
    Check(virtualLitch.Advance(49U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 30U,
          "production render cadence keeps frame zero through 49 ms");
    const unsigned beforeCorruptRead = arbiter.acquisitions;
    Check(virtualLitch.Advance(1U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 31U,
          "production render boundary swaps the prefetched frame at 50 ms");
    worker_fixture::Wait([&] {
        return arbiter.acquisitions > beforeCorruptRead &&
            !worker.GetDiagnostics().frameStorePlaybackPending &&
            !worker_fixture::notified && worker_fixture::atWait.load();
    }, "playback corrupt-frame prefetch convergence");
    Check(arbiter.acquisitions - beforeCorruptRead <= 12U,
          "playing frame decode reuses the resident Store instead of recovering it at every boundary");
    Check(virtualLitch.Advance(51U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 32U,
          "the second prefetched frame remains healthy before the cold fault");
    Check(virtualLitch.Advance(52U) == VIRTUAL_LITCH_OK &&
              stream.RequestedIndex() == 3U && stream.RequestedFrameCorrupt() &&
              virtualLitch.SemanticFrame().board[0][0].r == 32U &&
              std::all_of(
                  std::begin(virtualLitch.SemanticFrame().status),
                  std::end(virtualLitch.SemanticFrame().status),
                  [](const virtual_litch_color color) {
                      return color.r == 48U && color.g == 42U &&
                          color.b == 0U;
                  }),
          "corrupt frame holds the last complete image and renders yellow status");
    std::puts(
        "[FRAME256_PLAYBACK_JOURNEY] corrupt | requested=3 retained=2 "
        "status=yellow next=skip-after-one-frame-duration");

    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = true;
    }
    Check(virtualLitch.Advance(53U) == VIRTUAL_LITCH_OK &&
              stream.RequestedIndex() == 4U,
          "corrupt dwell requests the next independently decodable frame");
    Check(virtualLitch.Advance(106U) == VIRTUAL_LITCH_OK &&
              stream.RequestedIndex() == 4U &&
              virtualLitch.SemanticFrame().board[0][0].r == 32U,
          "delayed flash service cannot rewind recovery to the corrupt frame");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::Wait([&] {
        return stream.RequestedIndex() == 4U &&
            stream.RequestedFrameCorrupt() && worker_fixture::atWait.load();
    }, "playback adjacent-corrupt detection");
    Check(virtualLitch.Advance(0U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 32U &&
              virtualLitch.SemanticFrame().status[0].r == 48U,
          "adjacent corrupt recovery frame begins one bounded yellow dwell");
    Check(virtualLitch.Advance(53U) == VIRTUAL_LITCH_OK &&
              stream.RequestedIndex() == 5U,
          "adjacent corrupt frame advances to the next independent frame");
    worker_fixture::Wait([&] {
        return stream.RequestedIndex() == 5U &&
            stream.TryPresentRequested() && !worker.IsIdle() &&
            worker_fixture::atWait.load();
    }, "playback frame-five recovery");
    Check(virtualLitch.Advance(0U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 35U,
          "recovered frame five publishes through production render");
    Check(virtualLitch.Advance(54U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 35U,
          "recovered frame five receives its complete 55 ms after publication");
    std::puts(
        "[FRAME256_PLAYBACK_JOURNEY] recovered | frame=5 duration=55ms "
        "status=healthy next=exit-reenter");

    const auto exit = LegacyLitchcraftRequest(
        modules::LitchcraftSession::Op::Exit,
        virtualLitch.Litchcraft().SessionId(), 1U);
    virtualLitch.SendLitchcraftPacket(exit.bytes.data(), exit.size);
    Check(!virtualLitch.Litchcraft().IsActive() &&
              virtualLitch.PlacementModeActive(),
          "production coordinator consumes Exit and restores Setup authority");
    const auto enter = LegacyLitchcraftRequest(
        modules::LitchcraftSession::Op::Enter, 0U, 2U);
    virtualLitch.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    Check(virtualLitch.Litchcraft().IsActive() &&
              virtualLitch.SemanticFrame().board[0][0].r != 35U,
          "re-entry renders fallback or recovered frame zero, never stale frame five");
    worker_fixture::Wait([&] {
        return stream.RequestedIndex() == 0U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "playback re-entry frame-zero recovery");
    Check(virtualLitch.Advance(0U) == VIRTUAL_LITCH_OK &&
              virtualLitch.SemanticFrame().board[0][0].r == 30U,
          "re-entry publishes streamed frame zero through production render");
    std::puts(
        "[FRAME256_PLAYBACK_JOURNEY] reentry | fallback=black frame=0 "
        "pixel0=30 authority=Litchcraft next=physical-HIL");

    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
            worker_fixture::atWait.load();
    }, "active root settles before fresh-session mode journey");
    runtime::VirtualLitchSession rebooted(virtualConfig);
    rebooted.ObserveBootHallMask(0U);
    rebooted.Litchcraft().AttachFrameStream(
        &stream,
        [](void* context, const bool playing, const uint32_t session,
           const bool active) {
            static_cast<Worker*>(context)->NotifyLitchcraftFrameSession(
                playing, session, active);
        },
        &worker);
    Check(rebooted.Advance(0U) == VIRTUAL_LITCH_OK &&
              rebooted.Litchcraft().IsActive() &&
              rebooted.Litchcraft().SessionId() != 0U,
          "fresh board session adopts active Store with a valid interaction session");
    const auto exitButton = [&](const runtime::VirtualLitchPhysicalButton key,
                                const bool down) {
        Check(rebooted.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, key, down) ==
                  VIRTUAL_LITCH_OK &&
                  rebooted.Advance(30U) == VIRTUAL_LITCH_OK,
              "fresh-session Exit button settles through production debounce");
    };
    exitButton(runtime::VirtualLitchPhysicalButton::X, true);
    Check(rebooted.Advance(70U) == VIRTUAL_LITCH_OK,
          "fresh-session Exit forms physical chord");
    exitButton(runtime::VirtualLitchPhysicalButton::O, true);
    Check(rebooted.Advance(1440U) == VIRTUAL_LITCH_OK,
          "fresh-session Exit completes physical confirmation");
    exitButton(runtime::VirtualLitchPhysicalButton::X, false);
    exitButton(runtime::VirtualLitchPhysicalButton::O, false);
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameModeSavePending() &&
            worker_fixture::atWait.load();
    }, "fresh-session physical Exit persists the inactive Store root");
    store::FrameStore afterRebootExit(frameFiles.StoreOps());
    Check(!rebooted.Litchcraft().IsActive() &&
              rebooted.PlacementModeActive() &&
              afterRebootExit.Recover() == store::Status::Ok &&
              !afterRebootExit.CurrentManifest().active,
          "fresh-session physical Exit leaves Setup durable for the next boot");
    std::puts("[FRAME256_PLAYBACK_JOURNEY] fresh-session->physical-Exit | "
              "session=nonzero mode=Setup root=inactive next=reenter");

    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(), "playback worker fixture removes frame banks");
}

void RunSchema3RetirementWorkerLane(const bool publishBeforeReplacement) {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using Persistence = modules::LitchcraftPersistence;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "retirement worker starts from blank frame files");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 1U;
    manifest.active = true;
    manifest.sparkleSeed = 7U;
    store::Frame frame{};
    frame.durationMs = 100U;
    frame.emitters[0].color = {99U, 0U, 0U};
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 11U, token) == store::Status::Ok &&
              seed.WriteFrame(token, 0U, frame) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "retirement worker seeds a durable streamed artwork");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_retirement_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "retirement worker loads settings");
    hardware::ArtworkTestArbiter arbiter;
    MemoryStorage memory;
    Worker worker(config);
    Check(worker.SetLitchcraftArtworkStorageOps(memory.ChunkedOps()) &&
              (worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "retirement uses production settings worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); });
    auto& stream = worker.LitchcraftFrameStream();
    if (publishBeforeReplacement) {
        worker.NotifyLitchcraftFrameRuntimeReady();
        worker.NotifyLitchcraftFramePlayback();
        worker_fixture::Wait([&] {
            return stream.Generation() == 1U && stream.TryPresentRequested() &&
                worker.IsIdle() && worker_fixture::atWait.load();
        }, "retirement worker restores stream");
    }
    Check(worker.SetLitchcraftArtworkCurrentKnowledge(
              firmware::LitchcraftArtworkStorage::CurrentKnowledge::InvalidOrMissing),
          "retirement worker marks absent schema-3 storage");
    Persistence::AdvancedEnvelope envelope;
    Check(Persistence::EncodeAdvancedEnvelope(AdvancedDocument(37U).Value(), true, envelope),
          "retirement worker receives a real canonical envelope");
    stream.RetireDocument(); // Transport/render adoption is covered in the persistence suite.
    for (const bool failSave : {true, false}) {
        if (!publishBeforeReplacement && failSave) continue;
        memory.corruptPendingWrite = failSave;
        uint32_t receipt = 0U;
        Check(worker.EnqueueLitchcraftArtworkTracked(
                  envelope.bytes.data(), envelope.size, receipt),
              "retirement worker admits replacement without copying its envelope");
        worker_fixture::Wait([&] {
            return worker.GetSaveOutcome(receipt) == (failSave
                ? Worker::SaveOutcome::Failed : Worker::SaveOutcome::Saved) &&
                worker_fixture::atWait.load();
        }, "retirement worker finishes replacement save");
        Check(worker.ReleaseSaveReceipt(receipt), "retirement worker releases exact save receipt");
        // Force the actual union-store reconstruction/synchronization path used
        // by reconnect/playback after the artwork lane releases its scratch.
        worker.NotifyLitchcraftFrameRuntimeReady();
        worker.NotifyLitchcraftFramePlayback();
        worker_fixture::Wait([&] {
            return !worker_fixture::notified && worker.IsIdle() &&
                worker_fixture::atWait.load();
        }, "retirement worker resynchronizes disk authority");
        Check(stream.Generation() == 0U && stream.FrameCount() == 0U &&
                  !stream.TryPresentRequested(),
              "failed or successful save never republishes retired runtime artwork");
        store::FrameStore reboot(files.StoreOps());
        Check(reboot.Recover() == (failSave ? store::Status::Ok : store::Status::NoArtwork) &&
                  reboot.Generation() == (failSave ? 1U : 2U) &&
                  reboot.HasArtwork() == failSave,
              "reboot retains stream on failed save but honors successful durable retirement");
    }
    {
        store::Frame replacement{};
        replacement.durationMs = 50U;
        replacement.emitters[0].color = {0U, 41U, 0U};
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
        size_t encodedSize = 0U;
        Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
                  nullptr, replacement, encoded.data(), encoded.size(), encodedSize),
              "retired Store fixture encodes a new frame");
        std::array<uint8_t,
                   store::WIRE_RECORD_HEADER_BYTES + store::WIRE_FRAME_MAX_BYTES> body{};
        fxe::WriteU16LE(static_cast<uint16_t>(encodedSize), body.data());
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize), body.data() + 2U);
        std::copy_n(encoded.data(), encodedSize,
                    body.data() + store::WIRE_RECORD_HEADER_BYTES);
        const size_t bodySize = store::WIRE_RECORD_HEADER_BYTES + encodedSize;
        modules::BleOtaChunkQueue queue;
        Check(queue.Begin(), "retired Store fixture starts production ingress");
        constexpr uint32_t session = 0x24681357U;
        constexpr uint32_t owner = 97U;
        constexpr uint16_t transfer = 19U;
        const store::StoreProjection projection{session, 0U, 3U, true, false};
        std::array<uint8_t, 53U> begin{};
        begin[0] = 0x4dU;
        begin[1] = 2U;
        begin[2] = store::STORE_BEGIN;
        fxe::WriteU32LE(session, begin.data() + 3U);
        fxe::WriteU16LE(transfer, begin.data() + 7U);
        fxe::WriteU32LE(2U, begin.data() + 9U);
        fxe::WriteU32LE(static_cast<uint32_t>(bodySize), begin.data() + 13U);
        fxe::WriteU32LE(fxe::Crc32(body.data(), bodySize), begin.data() + 17U);
        fxe::WriteU16LE(1U, begin.data() + 21U);
        begin[23] = 3U;
        begin[24] = 3U;
        fxe::WriteU32LE(1U, begin.data() + 25U);
        std::array<uint8_t, 13U> commit{};
        std::copy_n(begin.data(), 9U, commit.data());
        commit[2] = store::STORE_COMMIT;
        fxe::WriteU32LE(static_cast<uint32_t>(bodySize), commit.data() + 9U);
        bool queued = queue.Enqueue(begin.data(), begin.size(), 1U);
        uint32_t ordinal = 2U;
        for (size_t offset = 0U; offset < bodySize; offset += 400U) {
            const size_t count = std::min<size_t>(400U, bodySize - offset);
            std::array<uint8_t, 413U> chunk{};
            std::copy_n(begin.data(), 9U, chunk.data());
            chunk[2] = store::STORE_CHUNK;
            fxe::WriteU32LE(static_cast<uint32_t>(offset),
                            chunk.data() + 9U);
            std::copy_n(body.data() + offset, count, chunk.data() + 13U);
            queued = queue.Enqueue(chunk.data(), 13U + count, ordinal++) &&
                queued;
        }
        Check(queued,
              "retired Store fixture queues new document body");
        worker.NotifyLitchcraftFrameIngress(queue, projection, owner);
        worker_fixture::Wait([&] {
            return !queue.TryPeekLitchcraftFrameStore() &&
                worker_fixture::atWait.load();
        }, "retired Store body reaches the production receiver");
        if (publishBeforeReplacement) {
            Check(worker.BlockLitchcraftFrameRootWritesForTest(true),
                  "retired Store blocks production file root publication");
        }
        Check(queue.Enqueue(commit.data(), commit.size(), ordinal),
              "retired Store fixture queues root publication");
        worker.NotifyLitchcraftFrameIngress(queue, projection, owner);
        if (publishBeforeReplacement) {
            worker_fixture::Wait([&] {
                return worker.LitchcraftFrameCommitPending() &&
                    worker_fixture::atWait.load();
            }, "retired Store observes failed publication before retry");
            Check(stream.Generation() == 0U && stream.FrameCount() == 0U &&
                      worker.LitchcraftFrameCommitPending(),
                  "unpublished replacement remains hidden while OTA Store barrier is closed");
            store::FrameStore beforeRetry(files.StoreOps());
            Check(beforeRetry.Recover() == store::Status::NoArtwork &&
                      beforeRetry.Generation() == 2U,
                  "failed root publication leaves the prior durable root");
            Check(worker.BlockLitchcraftFrameRootWritesForTest(false),
                  "retired Store releases root publication retry");
            fxe::ManualMillis::SetNowMs(fxe::Millis() + 1000U);
            worker.NotifyLitchcraftFrameIngress(queue, projection, owner);
        }
        std::array<uint8_t, store::STORE_RESPONSE_MAX_BYTES> response{};
        size_t responseSize = 0U;
        worker_fixture::Wait([&] {
            return worker.PeekLitchcraftFrameResponse(
                       response.data(), response.size(), responseSize) &&
                worker_fixture::atWait.load();
        }, "retired Store replacement terminal response");
        Check(responseSize == 66U && response[19U] <= 1U && response[20U] == 1U && response[13U] == 0U &&
                  stream.Generation() == 3U && stream.FrameCount() == 1U &&
                  !worker.LitchcraftFrameCommitPending(),
              "new Store document is accepted after durable schema-3 retirement");
        worker.AcceptLitchcraftFrameResponse();
    }
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(files.Remove(), "retirement worker removes its frame fixture");
}

void RunWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    using Admission = fxe::SettingsStore::AsyncSaveAdmission;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    fxe::SettingsStoreConfig config;
    config.path = "build/test_litchcraft_artwork_worker.json";
    fxe::SettingsStore settings(config);
    Check(settings.Load(), "worker fixture loads settings");
    hardware::ArtworkTestArbiter arbiter;
    MemoryStorage memory;
    Worker worker(config);
    Check(worker.SetLitchcraftArtworkStorageOps(memory.ChunkedOps()) &&
              (worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)),
          "existing settings_io worker accepts the auxiliary storage lane");
    worker.NotifyLitchcraftFrameRuntimeReady();
    Check(worker.SetLitchcraftArtworkCurrentKnowledge(
              firmware::LitchcraftArtworkStorage::CurrentKnowledge::
                  InvalidOrMissing),
          "boot restore explicitly marks an empty advanced source");
    std::array<uint8_t, 64U> frozen{};
    for (size_t index = 0U; index < frozen.size(); ++index) {
        frozen[index] = static_cast<uint8_t>(index + 1U);
    }
    uint32_t receipt = 0U;
    Admission admission = Admission::Rejected;
    Check(worker.EnqueueLitchcraftArtworkTracked(
              frozen.data(), frozen.size(), receipt, &admission) &&
              admission == Admission::Accepted && receipt != 0U,
          "advanced bytes receive one tracked asynchronous admission");
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved &&
               worker_fixture::atWait.load();
    });
    Check(memory.lastReplaceSource == frozen.data() &&
              memory.files[0U] == std::vector<uint8_t>(frozen.begin(), frozen.end()),
          "settings_io consumes the caller-frozen pointer without a body copy");
    uint32_t competingReceipt = 0U;
    Admission competingAdmission = Admission::Rejected;
    Check(!worker.EnqueueLitchcraftArtworkTracked(
              frozen.data(), frozen.size(), competingReceipt, &competingAdmission) &&
              competingReceipt == 0U && competingAdmission == Admission::Deferred,
          "saturated retained receipt rejects a competing artwork without replacing frozen bytes");
    settings.Doc()["AFTER_ADVANCED"] = 1U;
    Check(!worker.Enqueue(settings.Doc()) &&
              worker.ReleaseSaveReceipt(receipt) && worker.Enqueue(settings.Doc()),
          "retained advanced receipt preserves settings transaction ordering");
    std::array<uint8_t, 64U> restored{};
    size_t restoredSize = 0U;
    Check(worker.ReadLitchcraftArtwork(
              firmware::LitchcraftArtworkStorage::Artifact::Current,
              restored.data(), restored.size(), restoredSize) ==
                  firmware::LitchcraftArtworkStorage::Status::Present &&
              restoredSize == frozen.size() && restored == frozen &&
              worker.GetCompletedSaveCount() == 1U &&
              arbiter.acquisitions >= 3U,
          "synchronous read waits for older settings_io work and uses the RMT flash arbiter");

    memory.corruptPendingWrite = true;
    receipt = 0U;
    admission = Admission::Rejected;
    Check(worker.EnqueueLitchcraftArtworkTracked(
              frozen.data(), frozen.size(), receipt, &admission),
          "scoped failure fixture admits advanced bytes");
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed &&
               worker_fixture::atWait.load();
    });
    Check(worker.GetLitchcraftArtworkFailureCount() == 1U &&
              worker.GetFailureCount() == 0U,
          "advanced failure remains scoped and does not poison settings health");
    Check(worker.ReleaseSaveReceipt(receipt),
          "failed advanced receipt releases exactly once");

    memory.corruptPendingWrite = false;
    memory.corruptCurrentRead = true;
    Check(worker.EnqueueLitchcraftArtworkTracked(
              frozen.data(), frozen.size(), receipt, &admission),
          "promoted readback corruption fixture receives an exact receipt");
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed &&
               worker_fixture::atWait.load();
    });
    Check(worker.GetLitchcraftArtworkFailureCount() == 2U &&
              worker.ReleaseSaveReceipt(receipt),
          "worker never publishes Saved when promoted Current fails exact readback");

    // Generate the envelope with the production authority/codec owner, then
    // send it through the actual shared worker rather than a settings-only fake.
    memory.corruptCurrentRead = false;
    MemoryStorage seed;
    DirectAdvancedLane seedLane(seed);
    auto body = std::make_unique<modules::LitchcraftPersistence::AdvancedStorage>();
    modules::LitchcraftSession seedSession;
    modules::LitchcraftPersistence seedPersistence(settings, seedSession);
    seedPersistence.Configure(seedLane.Ops());
    seedPersistence.ConfigureAdvancedStorage(*body);
    Check(seedLane.storage.SetCurrentKnowledge(
              firmware::LitchcraftArtworkStorage::CurrentKnowledge::InvalidOrMissing),
          "eight-frame worker regression seeds an empty valid envelope lane");
    const auto eightFrames = AdvancedDocument(0x12345678U);
    fxe::ManualMillis::SetNowMs(0U);
    seedPersistence.SetAdvancedDocument(eightFrames, true);
    fxe::ManualMillis::SetNowMs(5000U);
    seedPersistence.Update(); seedPersistence.Update();
    const auto envelope = seed.files[0U];
    Check(worker.SetLitchcraftArtworkCurrentKnowledge(
              firmware::LitchcraftArtworkStorage::CurrentKnowledge::InvalidOrMissing) &&
              worker.EnqueueLitchcraftArtworkTracked(
                  envelope.data(), envelope.size(), receipt, &admission),
          "real shared worker admits the exact eight-frame envelope");
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved &&
               worker_fixture::atWait.load();
    });
    Check(worker.ReleaseSaveReceipt(receipt) && memory.files[0U] == envelope &&
              memory.maximumReplaceBytes <=
                  firmware::LitchcraftArtworkStorage::WRITE_CHUNK_BYTES,
          "shared worker retains bounded durable artwork writes without waiting for future LED frames");
    const auto priorEnvelope = memory.files[0U];
    memory.faultPoint = MemoryStorage::FaultPoint::AfterPromotePending;
    const unsigned attemptsBeforePromotion = memory.replaceCalls;
    Check(worker.EnqueueLitchcraftArtworkTracked(
              envelope.data(), envelope.size(), receipt, &admission),
          "post-promotion error receives an exact worker receipt");
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) != Worker::SaveOutcome::Pending &&
               worker_fixture::atWait.load();
    });
    Check(worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved &&
              memory.replaceCalls == attemptsBeforePromotion + 1U &&
              memory.files[1U] == priorEnvelope,
          "verified promotion reports Saved on its first attempt and retains LastGood");
    Check(worker.ReleaseSaveReceipt(receipt), "post-promotion receipt releases");
    for (const bool readFailure : {false, true}) {
        memory.corruptCurrentRead = !readFailure;
        memory.failCurrentRead = readFailure;
        Check(worker.EnqueueLitchcraftArtworkTracked(
                  envelope.data(), envelope.size(), receipt, &admission),
              "post-promotion verification fault is admitted");
        worker_fixture::Wait([&] {
            return worker.GetSaveOutcome(receipt) != Worker::SaveOutcome::Pending &&
                   worker_fixture::atWait.load();
        });
        Check(worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed &&
                  worker.ReleaseSaveReceipt(receipt),
              "post-promotion mismatch or read failure cannot certify durability");
    }
    memory.corruptCurrentRead = false;
    memory.failCurrentRead = false;
    memory.faultPoint = MemoryStorage::FaultPoint::None;
    for (uint32_t change = 1U; change <= 2U; ++change) {
        settings.Doc()["UNRELATED_AFTER_EIGHT"] = change;
        Check(worker.EnqueueTracked(settings.Doc(), receipt, &admission),
              "unrelated settings save enters the same production worker");
        worker_fixture::Wait([&] {
            return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved &&
                   worker_fixture::atWait.load();
        });
        Check(worker.ReleaseSaveReceipt(receipt) && memory.files[0U] == envelope,
              "two successful unrelated settings saves cannot replace the binary artwork");
    }
    modules::SettingsStore rebootSettings(config);
    Check(rebootSettings.Load(), "reboot reloads separately saved settings");
    DirectAdvancedLane rebootLane(memory);
    modules::LitchcraftSession rebootSession;
    modules::LitchcraftPersistence reboot(rebootSettings, rebootSession);
    reboot.Configure(rebootLane.Ops());
    reboot.ConfigureAdvancedStorage(*body);
    Check(reboot.RestoreAdvanced() ==
              modules::LitchcraftPersistence::AdvancedRecordStatus::Valid &&
              modules::litchcraft_animation::DocumentsEqual(
                  reboot.AdvancedDocument().Value(), eightFrames.Value()),
          "reboot after unrelated settings saves restores all eight frames byte-exact semantically");
    std::puts("[ARTWORK_REBOOT_NEGATIVE] durable_frames=8 unrelated_settings_saves=2 restored_frames=8 exact=1");

    // Exercise the real persistence owner and worker through a dirty-art exit.
    // Only filesystem operations and the RMT/RTOS boundary are controlled here.
    modules::LitchcraftSession exitSession;
    exitSession.Configure(3U, 1000U);
    exitSession.RestoreArtwork({}, true, 0U);
    modules::LitchcraftPersistence exitPersistence(settings, exitSession);
    using Persistence = modules::LitchcraftPersistence;
    exitPersistence.Configure({
        &worker,
        [](void* context, const JsonDocument& doc, uint32_t& token) {
            Admission result = Admission::Rejected;
            static_cast<Worker*>(context)->EnqueueTracked(doc, token, &result);
            return result;
        },
        [](void* context, const uint32_t token) {
            switch (static_cast<Worker*>(context)->GetSaveOutcome(token)) {
                case Worker::SaveOutcome::Pending: return Persistence::SaveOutcome::Pending;
                case Worker::SaveOutcome::Saved: return Persistence::SaveOutcome::Saved;
                default: return Persistence::SaveOutcome::Failed;
            }
        },
        [](void* context, const uint32_t token) {
            static_cast<Worker*>(context)->ReleaseSaveReceipt(token);
        },
        [](void*, Persistence::AdvancedSlot, uint8_t*, size_t, size_t&) {
            return Persistence::AdvancedReadOutcome::Missing;
        },
        [](void* context, const uint8_t* bytes, size_t size, uint32_t& token) {
            Admission result = Admission::Rejected;
            static_cast<Worker*>(context)->EnqueueLitchcraftArtworkTracked(
                bytes, size, token, &result);
            return result;
        },
        [](void* context,
           const modules::litchcraft_animation::Document& document,
           const bool active, Persistence::AdvancedEnvelope& envelope,
           uint32_t& token) {
            Admission result = Admission::Rejected;
            static_cast<Worker*>(context)->
                EnqueueLitchcraftArtworkDocumentTracked(
                    document, active, envelope, token, &result);
            return result;
        }});
    exitPersistence.ConfigureAdvancedStorage(*body);
    Check(exitPersistence.SetAdvancedDocument(AdvancedDocument(99U), true),
          "exit fixture changes the retained eight-frame document");
    const auto lastGoodBeforeExit = memory.files[0U];
    const unsigned attemptsBeforeExit = memory.replaceCalls;
    memory.faultPoint = MemoryStorage::FaultPoint::AfterPromotePending;
    Check(worker.SetLitchcraftArtworkCurrentKnowledge(
              firmware::LitchcraftArtworkStorage::CurrentKnowledge::Validated),
          "exit fixture starts with the verified current artifact");
    exitSession.Exit();
    exitPersistence.Update();
    const auto artworkReceipt = exitPersistence.GetAdvancedDiagnostics().submittedReceipt;
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(artworkReceipt) != Worker::SaveOutcome::Pending &&
               worker_fixture::atWait.load();
    });
    exitPersistence.Update();
    worker_fixture::Wait([&] {
        return worker.GetDiagnostics().outcome != Worker::SaveOutcome::Pending &&
               worker_fixture::atWait.load();
    });
    exitPersistence.Update();
    Check(!exitSession.IsActive() &&
              exitPersistence.FlushState() == Persistence::FlushStatus::Saved &&
              memory.replaceCalls == attemptsBeforeExit + 1U &&
              memory.files[1U] == lastGoodBeforeExit,
          "dirty-art exit completes after one verified artwork attempt despite rename error, retaining LastGood");

    // The bulk FIFO epoch belongs to queue retirement, not to the BLE
    // connection owner. Advancing it for OTA must not silently discard a
    // schema-4 packet from the still-current connection.
    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "frame worker fixture starts from blank banks");
    modules::litchcraft_frame_store::FrameStore frameSeed(
        frameFiles.StoreOps());
    Check(frameSeed.Recover() ==
              modules::litchcraft_frame_store::Status::NoArtwork,
          "frame ingress starvation fixture recovers blank storage");
    modules::litchcraft_frame_store::Manifest frameManifest{};
    frameManifest.frameCount = 3U;
    frameManifest.active = true;
    frameManifest.playbackOnEntry = true;
    modules::litchcraft_frame_store::TransactionToken frameToken{};
    Check(frameSeed.Begin(frameManifest, 0U, 1U, frameToken) ==
              modules::litchcraft_frame_store::Status::Ok,
          "frame ingress starvation fixture begins persisted artwork");
    for (uint16_t index = 0U; index < frameManifest.frameCount; ++index) {
        modules::litchcraft_frame_store::Frame frame{};
        frame.emitters[0].color.r = static_cast<uint8_t>(index + 1U);
        Check(frameSeed.WriteFrame(frameToken, index, frame) ==
                  modules::litchcraft_frame_store::Status::Ok,
              "frame ingress starvation fixture writes persisted frame");
    }
    Check(frameSeed.Commit(frameToken) ==
              modules::litchcraft_frame_store::Status::Ok,
          "frame ingress starvation fixture commits persisted artwork");
    modules::BleOtaChunkQueue frameQueue;
    Check(frameQueue.Begin(), "frame ingress queue begins");
    const uint32_t queueEpoch = frameQueue.AdvanceEpoch();
    constexpr uint32_t FRAME_OWNER = 77U;
    constexpr uint32_t FRAME_SESSION = 0x24681357U;
    std::array<uint8_t, 10U> inspect{};
    inspect[0] = 0x4dU;
    inspect[1] = 2U;
    inspect[2] = 1U;
    fxe::WriteU32LE(FRAME_SESSION, inspect.data() + 3U);
    fxe::WriteU16LE(9U, inspect.data() + 7U);
    inspect[9] = modules::litchcraft_frame_store::RESIDENT_LISTING_PROBE;
    Check(queueEpoch != FRAME_OWNER &&
              frameQueue.Enqueue(inspect.data(), inspect.size(), 1U),
          "schema-4 ingress deliberately uses independent queue and owner epochs");
    const modules::litchcraft_frame_store::StoreProjection projection{
        FRAME_SESSION, 0U, 3U, false, false};
    worker.NotifyLitchcraftFrameIngress(
        frameQueue, projection, FRAME_OWNER);
    std::array<uint8_t,
               modules::litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES>
        frameResponse{};
    size_t frameResponseSize = 0U;
    worker_fixture::Wait([&] {
        return worker.PeekLitchcraftFrameResponse(
                   frameResponse.data(), frameResponse.size(),
                   frameResponseSize) && worker_fixture::atWait.load();
    }, "frame inspect response");
    Check(frameResponseSize >= 21U &&
              frameResponse[2] ==
                  modules::litchcraft_frame_store::STORE_STATE &&
              frameResponse[13] == 0U,
          "schema-4 ingress completes with bounded foreground flash priority without waiting on a static render");
    Check(worker.LitchcraftFrameStream().Generation() == 1U &&
              worker.LitchcraftFrameStream().FrameCount() == 3U,
          "cold Store ingress aligns stream authority before replacement Commit");
    const size_t retainedResponseSize = frameResponseSize;
    const auto retainedResponse = frameResponse;

    const modules::litchcraft_frame_store::StoreProjection changedProjection{
        FRAME_SESSION, 0U, 3U, true, true};
    worker.NotifyLitchcraftFrameIngress(
        frameQueue, changedProjection, FRAME_OWNER);
    frameResponseSize = 0U;
    Check(worker.PeekLitchcraftFrameResponse(
              frameResponse.data(), frameResponse.size(), frameResponseSize) &&
              frameResponseSize == retainedResponseSize &&
              std::equal(frameResponse.begin(),
                         frameResponse.begin() + frameResponseSize,
                         retainedResponse.begin()),
          "same-owner playback projection cannot erase an undelivered schema-4 receipt");

    worker.AcceptLitchcraftFrameResponse();
    Check(worker.ReplayLitchcraftFrameResponse(9U, FRAME_OWNER),
          "specific lost Inspect receipt is replayable without repeating flash work");
    frameResponseSize = 0U;
    Check(worker.PeekLitchcraftFrameResponse(
              frameResponse.data(), frameResponse.size(), frameResponseSize) &&
              frameResponseSize == retainedResponseSize &&
              std::equal(frameResponse.begin(),
                         frameResponse.begin() + frameResponseSize,
                         retainedResponse.begin()),
          "retained Store receipt reuses the exact response bytes");
    worker.AcceptLitchcraftFrameResponse();
    Check(!worker.ReplayLitchcraftFrameResponse(8U, FRAME_OWNER) &&
              !worker.ReplayLitchcraftFrameResponse(9U, FRAME_OWNER + 1U),
          "unavailable requests and replacement connection cannot replay stale Store evidence");
    std::array<uint8_t, 53U> immediateBegin{};
    immediateBegin[0] = 0x4dU;
    immediateBegin[1] = 2U;
    immediateBegin[2] = modules::litchcraft_frame_store::STORE_BEGIN;
    fxe::WriteU32LE(FRAME_SESSION, immediateBegin.data() + 3U);
    fxe::WriteU16LE(10U, immediateBegin.data() + 7U);
    fxe::WriteU32LE(1U, immediateBegin.data() + 9U);
    fxe::WriteU32LE(6U, immediateBegin.data() + 13U);
    fxe::WriteU32LE(0U, immediateBegin.data() + 17U);
    fxe::WriteU16LE(1U, immediateBegin.data() + 21U);
    immediateBegin[23] = 3U;
    immediateBegin[24] = 1U;
    fxe::WriteU32LE(1U, immediateBegin.data() + 25U);
    Check(frameQueue.Enqueue(immediateBegin.data(), immediateBegin.size(), 2U),
          "persisted-artwork fixture enqueues Begin immediately after Inspect");
    worker.NotifyLitchcraftFrameIngress(
        frameQueue, changedProjection, FRAME_OWNER);
    worker_fixture::Wait([&] {
        return !frameQueue.TryPeekLitchcraftFrameStore() &&
            !worker.IsIdle() && worker_fixture::atWait.load();
    }, "persisted artwork Inspect-to-Begin admission");
    frameResponseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(
              frameResponse.data(), frameResponse.size(), frameResponseSize),
          "Inspect does not starve immediate optimistic Begin or add an ACK barrier");

    std::array<uint8_t, 9U> immediateCancel{};
    immediateCancel[0] = 0x4dU;
    immediateCancel[1] = 2U;
    immediateCancel[2] = modules::litchcraft_frame_store::STORE_CANCEL;
    fxe::WriteU32LE(FRAME_SESSION, immediateCancel.data() + 3U);
    fxe::WriteU16LE(10U, immediateCancel.data() + 7U);
    worker.NotifyLitchcraftFramePriority(
        immediateCancel.data(), immediateCancel.size(), FRAME_OWNER);
    worker_fixture::Wait([&] {
        frameResponseSize = 0U;
        return worker.PeekLitchcraftFrameResponse(
                   frameResponse.data(), frameResponse.size(),
                   frameResponseSize) && frameResponse[13] == 0U;
    }, "persisted artwork immediate Begin cleanup");
    worker.AcceptLitchcraftFrameResponse();
    Check(!worker.ReplayLitchcraftFrameResponse(9U, FRAME_OWNER),
          "a newer Store response cannot reuse an older retained receipt length");
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "persisted artwork immediate Begin releases storage");
    // A complete frame-zero record remains staged while the rest of the body
    // arrives. Cancel must leave the prior committed authority undisturbed.
    const uint32_t generationBeforeIncompleteUpload =
        worker.LitchcraftFrameStream().Generation();
    modules::litchcraft_frame_store::Frame provisionalFrame{};
    provisionalFrame.durationMs = 50U;
    provisionalFrame.emitters[0].color.r = 99U;
    std::array<uint8_t,
               modules::litchcraft_frame_store::WIRE_FRAME_MAX_BYTES>
        provisionalEncoded{};
    size_t provisionalEncodedSize = 0U;
    Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
              nullptr, provisionalFrame, provisionalEncoded.data(),
              provisionalEncoded.size(), provisionalEncodedSize),
          "preview rollback fixture encodes frame zero");
    std::vector<uint8_t> provisionalBody;
    const auto appendRecord = [&provisionalBody](
                                  const uint8_t* bytes,
                                  const size_t size) {
        const size_t offset = provisionalBody.size();
        provisionalBody.resize(
            offset + modules::litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES +
            size);
        fxe::WriteU16LE(static_cast<uint16_t>(size),
                        provisionalBody.data() + offset);
        fxe::WriteU32LE(fxe::Crc32(bytes, size),
                        provisionalBody.data() + offset + 2U);
        std::copy_n(
            bytes, size,
            provisionalBody.data() + offset +
                modules::litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES);
    };
    appendRecord(provisionalEncoded.data(), provisionalEncodedSize);
    std::array<uint8_t,
               modules::litchcraft_frame_store::WIRE_FRAME_MAX_BYTES>
        repeatEncoded{};
    size_t repeatEncodedSize = 0U;
    Check(modules::litchcraft_wire_codec::FrameCodec::Encode(
              &provisionalFrame, provisionalFrame, repeatEncoded.data(),
              repeatEncoded.size(), repeatEncodedSize),
          "preview rollback fixture encodes repeated frames");
    for (uint16_t index = 1U;
         index < modules::litchcraft_frame_store::FRAME_COUNT; ++index) {
        appendRecord(repeatEncoded.data(), repeatEncodedSize);
    }
    Check(provisionalBody.size() == 3424U,
          "preview rollback fixture uses the exact repeated 256-frame body");
    std::array<uint8_t, 53U> previewBegin{};
    previewBegin[0] = 0x4dU;
    previewBegin[1] = 2U;
    previewBegin[2] = modules::litchcraft_frame_store::STORE_BEGIN;
    fxe::WriteU32LE(FRAME_SESSION, previewBegin.data() + 3U);
    fxe::WriteU16LE(12U, previewBegin.data() + 7U);
    fxe::WriteU32LE(1U, previewBegin.data() + 9U);
    fxe::WriteU32LE(static_cast<uint32_t>(provisionalBody.size()),
                    previewBegin.data() + 13U);
    fxe::WriteU32LE(
        fxe::Crc32(provisionalBody.data(), provisionalBody.size()),
        previewBegin.data() + 17U);
    fxe::WriteU16LE(modules::litchcraft_frame_store::FRAME_COUNT,
                    previewBegin.data() + 21U);
    previewBegin[23] = 3U;
    previewBegin[24] = 3U;
    fxe::WriteU32LE(1U, previewBegin.data() + 25U);
    Check(frameQueue.Enqueue(previewBegin.data(), previewBegin.size(), 3U),
          "preview rollback fixture enqueues Begin");
    uint32_t previewCursor = 0U;
    uint32_t previewOrdinal = 4U;
    constexpr uint32_t PREVIEW_PACKET_BYTES = 400U;
    const uint32_t previewIngressBytes = static_cast<uint32_t>(
        ((modules::litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES +
          provisionalEncodedSize + PREVIEW_PACKET_BYTES - 1U) /
         PREVIEW_PACKET_BYTES) * PREVIEW_PACKET_BYTES);
    while (previewCursor < previewIngressBytes) {
        const size_t count = std::min<size_t>(
            PREVIEW_PACKET_BYTES, previewIngressBytes - previewCursor);
        std::array<uint8_t, 413U> chunk{};
        chunk[0] = 0x4dU;
        chunk[1] = 2U;
        chunk[2] = modules::litchcraft_frame_store::STORE_CHUNK;
        fxe::WriteU32LE(FRAME_SESSION, chunk.data() + 3U);
        fxe::WriteU16LE(12U, chunk.data() + 7U);
        fxe::WriteU32LE(previewCursor, chunk.data() + 9U);
        std::copy_n(provisionalBody.data() + previewCursor, count,
                    chunk.data() + 13U);
        Check(frameQueue.Enqueue(chunk.data(), 13U + count,
                                 previewOrdinal++),
              "preview rollback fixture enqueues ordered chunk");
        previewCursor += static_cast<uint32_t>(count);
    }
    worker.NotifyLitchcraftFrameIngress(
        frameQueue, changedProjection, FRAME_OWNER);
    worker_fixture::Wait([&] {
        return !frameQueue.TryPeekLitchcraftFrameStore() &&
            worker_fixture::atWait.load();
    }, "provisional frame-zero ingress");
    modules::litchcraft_frame_store::Frame provisionalDisplay{};
    uint16_t provisionalIndex = 99U;
    const bool displayedDuringIncompleteUpload =
        worker.LitchcraftFrameStream().CopyDisplayed(
            provisionalDisplay, provisionalIndex);
    Check(worker.LitchcraftFrameStream().Generation() ==
              generationBeforeIncompleteUpload &&
              (!displayedDuringIncompleteUpload ||
               provisionalDisplay.emitters[0].color.r != 99U) &&
              previewCursor < provisionalBody.size(),
          "a complete frame-zero record stays staged until the whole upload is validated");
    fxe::WriteU16LE(12U, immediateCancel.data() + 7U);
    worker.NotifyLitchcraftFramePriority(
        immediateCancel.data(), immediateCancel.size(), FRAME_OWNER);
    worker_fixture::Wait([&] {
        frameResponseSize = 0U;
        return worker.PeekLitchcraftFrameResponse(
                   frameResponse.data(), frameResponse.size(),
                   frameResponseSize) && frameResponse[13] == 0U &&
            worker.LitchcraftFrameStream().Generation() ==
                generationBeforeIncompleteUpload;
    }, "incomplete upload Cancel");
    modules::litchcraft_frame_store::Frame rejectedDisplay{};
    uint16_t rejectedIndex = 99U;
    const bool displayedAfterCancel = worker.LitchcraftFrameStream().CopyDisplayed(
        rejectedDisplay, rejectedIndex);
    Check(!displayedAfterCancel ||
              rejectedDisplay.emitters[0].color.r != 99U,
          "Cancel leaves the incomplete replacement invisible");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "incomplete upload Cancel releases storage");

    modules::BleOtaChunkQueue replacementQueue;
    Check(replacementQueue.Begin(), "replacement owner queue begins");
    worker.NotifyLitchcraftFrameIngress(
        replacementQueue, changedProjection, FRAME_OWNER + 1U);
    frameResponseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(
              frameResponse.data(), frameResponse.size(), frameResponseSize),
          "a response from the disconnected owner is never visible to its replacement");

    // A begun transfer owns the shared union without a successful-Begin ACK.
    // Priority Cancel publishes its exact terminal receipt and releases it.
    std::array<uint8_t, 53U> begin{};
    begin[0] = 0x4dU;
    begin[1] = 2U;
    begin[2] = modules::litchcraft_frame_store::STORE_BEGIN;
    fxe::WriteU32LE(FRAME_SESSION, begin.data() + 3U);
    fxe::WriteU16LE(10U, begin.data() + 7U);
    fxe::WriteU32LE(1U, begin.data() + 9U);
    fxe::WriteU32LE(6U, begin.data() + 13U);
    fxe::WriteU32LE(0U, begin.data() + 17U);
    fxe::WriteU16LE(1U, begin.data() + 21U);
    begin[23] = 3U;
    begin[24] = 1U;
    fxe::WriteU32LE(1U, begin.data() + 25U);
    Check(replacementQueue.Enqueue(begin.data(), begin.size(), 2U),
          "abandoned-transfer fixture enqueues Begin");
    fxe::ManualMillis::SetNowMs(100U);
    worker.NotifyLitchcraftFrameIngress(
        replacementQueue, changedProjection, FRAME_OWNER + 1U);
    worker_fixture::Wait([&] {
        return !replacementQueue.TryPeekLitchcraftFrameStore() &&
            !worker.IsIdle() && worker_fixture::atWait.load();
    }, "optimistic frame Begin admission");
    frameResponseSize = 0U;
    Check(!worker.PeekLitchcraftFrameResponse(
              frameResponse.data(), frameResponse.size(), frameResponseSize),
          "successful Begin emits no response barrier");
    std::array<uint8_t, 9U> cancel{};
    cancel[0] = 0x4dU;
    cancel[1] = 2U;
    cancel[2] = modules::litchcraft_frame_store::STORE_CANCEL;
    fxe::WriteU32LE(FRAME_SESSION, cancel.data() + 3U);
    fxe::WriteU16LE(10U, cancel.data() + 7U);
    worker.NotifyLitchcraftFramePriority(
        cancel.data(), cancel.size(), FRAME_OWNER + 1U);
    worker_fixture::Wait([&] {
        frameResponseSize = 0U;
        return worker.PeekLitchcraftFrameResponse(
                   frameResponse.data(), frameResponse.size(),
                   frameResponseSize) && frameResponse[13] == 0U;
    }, "priority Cancel response");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "priority Cancel release");
    Check(worker.IsIdle(),
          "priority Cancel retires an optimistic Begin immediately");
    worker.NotifyLitchcraftFramePriority(
        cancel.data(), cancel.size(), FRAME_OWNER + 1U);
    worker_fixture::Wait([&] {
        frameResponseSize = 0U;
        return worker.PeekLitchcraftFrameResponse(
                   frameResponse.data(), frameResponse.size(),
                   frameResponseSize) && frameResponse[13] == 0U;
    }, "duplicate Cancel response");
    worker.AcceptLitchcraftFrameResponse();
    worker_fixture::Wait([&] {
        return worker.IsIdle() && worker_fixture::atWait.load();
    }, "duplicate Cancel release");
    Check(worker.IsIdle(),
          "duplicate priority Cancel replays its exact terminal receipt");

    // An abandoned transfer with no Cancel still expires while its Begin
    // response remains pending.
    fxe::WriteU16LE(11U, begin.data() + 7U);
    Check(replacementQueue.Enqueue(begin.data(), begin.size(), 3U),
          "uncancelled timeout fixture enqueues a second Begin");
    fxe::ManualMillis::SetNowMs(200U);
    worker.NotifyLitchcraftFrameIngress(
        replacementQueue, changedProjection, FRAME_OWNER + 1U);
    worker_fixture::Wait([&] {
        return !replacementQueue.TryPeekLitchcraftFrameStore() &&
            !worker.IsIdle() && worker_fixture::atWait.load();
    }, "uncancelled optimistic Begin admission");
    fxe::ManualMillis::SetNowMs(10201U);
    worker_fixture::Wait([&] {
        frameResponseSize = 0U;
        return worker.IsIdle() &&
            !worker.PeekLitchcraftFrameResponse(
                frameResponse.data(), frameResponse.size(), frameResponseSize) &&
            worker_fixture::atWait.load();
    }, "uncancelled Begin timeout");
    Check(worker.IsIdle() && !worker.IsLitchcraftBulkTransferActive(),
          "10 s timeout releases an uncancelled transfer and its render hold despite pending response");
    Check(frameFiles.Remove(), "frame worker fixture removes internal banks");

    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
}
} // namespace

#include "test_disabled_storage_worker.hpp"
#include "boot_artwork_recovery_journey.hpp"

int main() {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    if (std::getenv("LITCH_WORKER_RR_HOLD_PROBE") != nullptr) {
        RunBoundedIngressWorkerJourney(); return failures == 0 ? 0 : 1;
    }
    std::puts("[LITCHCRAFT_ARTWORK_STORAGE] start");
    if (std::getenv("LITCH_WORKER_PLAY_STARTUP_PROBE") != nullptr) {
        RunArbitraryResidentReleasePlayWorkerJourney();
        RunArbitraryResidentReleasePlayWorkerJourney(true);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_STORAGE_READ_PROBE") != nullptr) {
        RunSettingsReadDiagnosticWorkerJourney();
        RunCorruptStoreReplacementWorkerJourney(false);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_BOOT_NO_WRITE") != nullptr) {
        RunFreshStoreEntryWorkerJourney();
        RunDevelopmentFormatResetWorkerJourney();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_WAKE_MODE_PROBE") != nullptr) {
        RunBootArtworkRecoveryRaceWorkerLane(true, true);
        RunBootArtworkRecoveryRaceWorkerLane(false, true);
        RunWakeModeTerminalWorkerLane(false);
        RunWakeModeTerminalWorkerLane(true);
        RunWakeModeTerminalWorkerLane(false, true);
        InitialStoreModeExplicitChoice();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_BOOT_ARTWORK_PROBE") != nullptr) {
        RunBootArtworkRecoveryRaceWorkerLane();
        RunBootArtworkRecoveryRaceWorkerLane(true);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_REVERSE_REFILL") != nullptr) {
        NormalFirstSnapshotWindowDrainRefillsWithoutNewRequest();
        RunReverseSnapshotCreditPrefetch();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_PAINT_IO_PROBE") != nullptr) {
        RunSummaryReadHandleAliasCorpus();
        RunOrdinaryPaintRealFileIoCorpus();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_NEW_REPLACE_PROBE") != nullptr) {
        RunInterruptedStrokeReplacementJourney();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_QUIET_PLAYBACK_PROBE") != nullptr) {
        RunQuietPlaybackExpiryWorkerJourney();
        RunWorkerLane();
        RunReverseSnapshotCreditPrefetch();
        std::printf("[QUIET_PLAYBACK_WORKER] failures=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_PLAYBACK_H1_PROBE") != nullptr) {
        RunQuietEditPlaybackProbe();
        std::printf("[H1_SCHEDULING] failures=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_COMPOSE_H3_PROBE") != nullptr) {
        RunStructuralComposeCoherenceProbe();
        std::printf("[H3_COHERENCE] failures=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_DEVELOPMENT_FORMAT_PROBE") != nullptr) {
        RunEmptyOnlyRetirementPreservesAcceptedNew();
        RunDevelopmentFormatResetWorkerJourney();
        RunPrelaunchRootIoFailureWorkerCountercheck();
        RunCorruptStoreReplacementWorkerJourney(false);
        RunCorruptStoreReplacementWorkerJourney(true);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_SETTINGS_NEW_PROBE") != nullptr) {
        RunPagingOnlyPendingNewJourney();
        RunNewDuringConfigureWorkerJourney();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_SETTINGS_CADENCE_PROBE") != nullptr) {
        RunSlowSettingsPlaybackWorkerJourney(false);
        RunSlowSettingsPlaybackWorkerJourney(true);
        RunSlowSettingsPlaybackWorkerJourney(false, true);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_SETTINGS_WARNING_PROBE") != nullptr) {
        RunSupersededSettingsWarningJourney();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_ARTWORK_STALL_PROBE") != nullptr) {
        RunSettingsDuringPlaybackWorkerJourney(true);
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_SETTINGS_PROBE") != nullptr) {
        RunSettingsDuringPlaybackWorkerJourney();
        return failures == 0 ? 0 : 1;
    }
    if (std::getenv("LITCH_WORKER_DISABLED_STORAGE_PROBE") != nullptr) {
        RunDisabledStorageWorkerLane();
        std::printf("[DISABLED_STORAGE_WORKER] failures=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    RunSummaryReadHandleAliasCorpus();
    RunOrdinaryPaintRealFileIoCorpus();
    RunDisabledStorageWorkerLane();
    ProductionFileWritePageMatchesStoreBatch();
    RunTransactionStateMachine();
    RunDenseVerificationCadence();
    RunPromotionPowerLossRecovery();
    RunPredecessorPlaybackWorkerLane();
    RunFrameSaveFailureRetryWorkerLane();
    RunUnpublishedResponseWorkerLane();
    RunSettingsReadDiagnosticWorkerJourney();
    RunFreshStoreEntryWorkerJourney();
    RunEmptyOnlyRetirementPreservesAcceptedNew();
    RunDevelopmentFormatResetWorkerJourney();
    RunPrelaunchRootIoFailureWorkerCountercheck();
    RunCorruptStoreReplacementWorkerJourney(false);
    RunCorruptStoreReplacementWorkerJourney(true);
    RunNoRootReentryRaceWorkerLane();
    RunBootArtworkRecoveryRaceWorkerLane();
    RunBootArtworkRecoveryRaceWorkerLane(true);
    RunBootArtworkRecoveryRaceWorkerLane(true, true);
    RunBootArtworkRecoveryRaceWorkerLane(false, true);
    RunWakeModeTerminalWorkerLane(false);
    RunWakeModeTerminalWorkerLane(true);
    RunWakeModeTerminalWorkerLane(false, true);
    InitialStoreModeExplicitChoice();
    RunResidentSelectionReleaseWorkerJourney();
    RunArbitraryResidentReleasePlayWorkerJourney();
    RunArbitraryResidentReleasePlayWorkerJourney(true);
    RunSupersededSettingsWarningJourney();
    RunSettingsDuringPlaybackWorkerJourney();
    RunSettingsDuringPlaybackWorkerJourney(true);
    RunSlowSettingsPlaybackWorkerJourney(false);
    RunSlowSettingsPlaybackWorkerJourney(true);
    RunSlowSettingsPlaybackWorkerJourney(false, true);
    RunPagingOnlyPendingNewJourney();
    RunNewDuringConfigureWorkerJourney();
    RunInterruptedStrokeReplacementJourney();
    RunQuietPlaybackExpiryWorkerJourney();
    RunFramePlaybackWorkerLane();
    RunQuietEditPlaybackProbe();
    RunStructuralComposeCoherenceProbe();
    RunSchema3RetirementWorkerLane(true);
    RunSchema3RetirementWorkerLane(false);
    RunWorkerLane();
    RunBoundedIngressWorkerJourney();
    RunReverseSnapshotCreditPrefetch();
    NormalFirstSnapshotWindowDrainRefillsWithoutNewRequest();
    std::printf("[LITCHCRAFT_ARTWORK_STORAGE] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
