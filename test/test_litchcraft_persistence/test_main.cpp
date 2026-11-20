#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>
#include "allocation_probe.hpp"

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_animation_codec.hpp"
#include "modules/litchcraft_persistence.hpp"
#include "modules/ble_event_queue.hpp"
#include "modules/ble_ota_chunk_queue.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "runtime/game_clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "virtual_litch_session.hpp"

namespace {
using Art = modules::LitchcraftSession;
using Persistence = modules::LitchcraftPersistence;
using Admission = fxe::SettingsStore::AsyncSaveAdmission;
constexpr const char* PATH = "build/test_litchcraft_persistence.json";
int failures = 0;
void Check(bool result, const char* message) {
    if (!result) { std::printf("[FAIL] %s\n", message); ++failures; }
}
modules::SettingsStoreConfig Config() {
    modules::SettingsStoreConfig config;
    config.path = PATH;
    config.scrubFunc = &modules::ScrubLitchcraftRecord;
    return config;
}
void RemoveFiles() {
    std::filesystem::create_directories("build");
    for (const char* suffix : {"", ".bak", ".tmp"}) std::remove((std::string(PATH) + suffix).c_str());
}
bool Equal(const Art::Canvas& a, const Art::Canvas& b) {
    return std::equal(a.begin(), a.end(), b.begin(), [](auto x, auto y) {
        return x.r == y.r && x.g == y.g && x.b == y.b;
    });
}
bool Equal(const modules::LitchcraftQuickPalette& a,
           const modules::LitchcraftQuickPalette& b) {
    return std::equal(a.begin(), a.end(), b.begin(), [](auto x, auto y) {
        return x.r == y.r && x.g == y.g && x.b == y.b;
    });
}
Art::Packet Request(Art& art, Art::Op op, uint16_t id, std::initializer_list<uint8_t> payload = {}) {
    Art::Packet p;
    p.bytes[0] = Art::MAGIC;
    p.bytes[1] = uint8_t(op);
    const auto session = op == Art::Op::Enter ? 0U : art.SessionId();
    for (unsigned i = 0U; i < 4U; ++i) p.bytes[2U + i] = session >> (8U * i);
    p.bytes[6] = id;
    p.bytes[7] = id >> 8U;
    p.size = 8U;
    for (auto byte : payload) p.bytes[p.size++] = byte;
    return p;
}
void Send(Art& art, Art::Op op, uint16_t id, std::initializer_list<uint8_t> payload = {}) {
    const auto p = Request(art, op, id, payload);
    art.HandlePacket(p.bytes.data(), p.size, true, 0U);
}
std::vector<Art::Packet> Drain(Art& art) {
    std::vector<Art::Packet> packets;
    Art::Packet p;
    while (art.PeekPacket(p)) { packets.push_back(p); art.AcceptPacket(); }
    return packets;
}
bool HasAck(const std::vector<Art::Packet>& packets, uint16_t id, Art::Status status) {
    return std::any_of(packets.begin(), packets.end(), [&](const auto& p) {
        return p.bytes[1] == uint8_t(Art::Event::State) &&
            (uint16_t(p.bytes[10]) | uint16_t(p.bytes[11]) << 8U) == id && p.bytes[12] == uint8_t(status);
    });
}
struct FileSink {
    modules::SettingsStore writer{Config()};
    Admission admission = Admission::Accepted;
    Persistence::SaveOutcome outcome = Persistence::SaveOutcome::Pending;
    std::string frozen;
    uint32_t nextReceipt = 0U;
    uint32_t retained = 0U;
    unsigned writes = 0U;
    unsigned admissions = 0U;
    Persistence::Ops Ops() {
        return {this,
            [](void* context, const JsonDocument& document, uint32_t& receipt) {
                auto& self = *static_cast<FileSink*>(context);
                if (self.admission != Admission::Accepted) return self.admission;
                if (self.retained != 0U) return Admission::Deferred;
                self.frozen.clear();
                serializeJson(document, self.frozen);
                receipt = self.retained = ++self.nextReceipt;
                ++self.admissions;
                self.outcome = Persistence::SaveOutcome::Pending;
                return Admission::Accepted;
            },
            [](void* context, uint32_t receipt) {
                auto& self = *static_cast<FileSink*>(context);
                Check(receipt == self.retained, "poll owns exact retained receipt");
                return self.outcome;
            },
            [](void* context, uint32_t receipt) {
                auto& self = *static_cast<FileSink*>(context);
                Check(receipt == self.retained, "release owns exact retained receipt");
                self.retained = 0U;
            }};
    }
    void Complete(bool success = true) {
        Check(retained != 0U, "completion requires accepted frozen snapshot");
        if (success) {
            writer.Doc().clear();
            Check(!deserializeJson(writer.Doc(), frozen), "frozen snapshot parses");
            success = writer.SaveNow();
            if (success) ++writes;
        }
        outcome = success ? Persistence::SaveOutcome::Saved : Persistence::SaveOutcome::Failed;
    }
};
struct Fixture {
    modules::SettingsStore settings{Config()};
    Art art;
    FileSink sink;
    Persistence persistence{settings, art};
    Fixture() {
        RemoveFiles();
        Check(settings.Load(), "load initial file-backed settings");
        art.Configure(3U, 1000U);
        art.BeginConnection();
        persistence.Configure(sink.Ops());
        Check(persistence.Restore() == modules::LitchcraftRecordStatus::Missing, "fresh board has no art record");
    }
    void Tick(uint32_t ms) {
        fxe::ManualMillis::SetNowMs(ms);
        art.Update();
        persistence.Update();
    }
    void Complete(uint32_t ms, bool success = true) { sink.Complete(success); Tick(ms); }
};
modules::LitchcraftRecord ReadSaved() {
    modules::SettingsStore reader{Config()};
    Check(reader.Load(), "restart reads file-backed settings");
    modules::LitchcraftRecord result;
    Check(modules::ReadLitchcraftRecord(reader.Doc(), result) ==
              modules::LitchcraftRecordStatus::Valid,
          "restart validates art record and checksum");
    return result;
}
void DebounceAndReceiptGenerations() {
    Fixture f;
    f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
    f.Tick(50U); Send(f.art, Art::Op::Paint, 2U, {1, 0, 3, 0});
    f.Tick(99U); Check(f.sink.admissions == 0U, "mode waits its independent 100ms tier");
    f.Tick(100U); f.Complete(100U);
    Check(ReadSaved().active && ReadSaved().canvas[0].r == 0U,
          "mode save retains durable canvas rather than bypassing artwork debounce");
    const auto generation = f.art.CanvasGeneration();
    f.Tick(4000U); Send(f.art, Art::Op::Paint, 3U, {2, 0, 3, 0});
    Send(f.art, Art::Op::Tool, 4U, {0, 1, 5, 6, 7});
    Check(f.art.CanvasGeneration() == generation,
          "same RGB paint and tool changes do not dirty canvas pixels");
    f.Tick(8999U); Check(f.sink.admissions == 1U,
                        "canvas and palette wait for the latest mutation plus 4999ms");
    f.Tick(9000U); Check(f.sink.admissions == 2U,
                        "combined artwork queues at the shared trailing deadline");
    f.Tick(9010U); Send(f.art, Art::Op::Paint, 5U, {3, 0, 3, 1});
    f.Complete(9010U);
    Check(f.persistence.IsDirty() && ReadSaved().canvas[4].r == 0U,
          "older durable receipt cannot clear a later canvas mutation");
    f.Tick(14009U); Check(f.sink.admissions == 2U, "new mutation resets trailing deadline");
    f.Tick(14010U); f.Complete(14010U);
    Check(!f.persistence.IsDirty() && ReadSaved().canvas[4].r == 5U && f.sink.writes == 3U,
          "latest canvas commits once after its own idle deadline");
    const auto memory = f.settings.Doc().memoryUsage();
    for (unsigned i = 0U; i < 20U; ++i) {
        f.persistence.RequestFlush(); f.persistence.Update(); f.Complete(14010U);
    }
    Check(f.settings.Doc().memoryUsage() == memory, "repeated art serialization does not grow JSON memory pool");
}
void ContinuousDrawingAndNoops() {
    Fixture f;
    f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
    f.Tick(100U); f.Complete(100U);
    for (uint16_t i = 0U; i < 12U; ++i) {
        f.Tick(200U + i * 4000U);
        Send(f.art, Art::Op::Paint, i + 2U, {uint8_t(i + 1U), 0, 3, uint8_t(i)});
        f.persistence.Update();
    }
    Check(f.sink.writes == 1U && f.persistence.IsDirty(), "continuous drawing has no max-dirty-age or per-stroke writes");
    f.Tick(49199U); Check(f.sink.admissions == 1U, "continuous drawing stops at last actual mutation plus4999");
    f.Tick(49200U); f.Complete(49200U);
    Send(f.art, Art::Op::Clear, 14U);
    const auto changed = f.art.CanvasGeneration();
    Send(f.art, Art::Op::Clear, 15U);
    Check(f.art.CanvasGeneration() == changed, "clearing an already blank canvas does not dirty again");
    f.Tick(54200U); f.Complete(54200U);
    Check(ReadSaved().canvas[0].r == 0U, "explicit blank canvas is a valid durable record");
}
void PaletteDebounceAndMigration() {
    Fixture f;
    f.Tick(0U);
    Send(f.art, Art::Op::Enter, 1U);
    f.Tick(100U);
    f.Complete(100U);

    f.Tick(200U);
    Send(f.art, Art::Op::Tool, 2U, {0U, 15U, 17U, 34U, 51U});
    Check(f.art.PaletteGeneration() != 0U && f.persistence.IsDirty(),
          "choosing a distinct paint color dirties the persistent quick palette");
    f.Tick(5199U);
    Check(f.sink.admissions == 1U,
          "quick-palette changes wait for five seconds of inactivity");
    f.Tick(5200U);
    f.Complete(5200U);
    Check(!f.persistence.IsDirty() &&
              Equal(ReadSaved().quickPalette, f.art.RecentColors()) &&
              ReadSaved().quickPalette[0].r == 17U,
          "one coalesced save restores the board's latest quick-palette order");

    modules::SettingsStore legacy{Config()};
    Check(legacy.Load(), "load palette migration fixture");
    const auto current = ReadSaved();
    std::array<uint8_t, 770U> legacyBytes{};
    legacyBytes[0] = 1U;
    legacyBytes[1] = current.active ? 1U : 0U;
    for (size_t i = 0U; i < current.canvas.size(); ++i) {
        legacyBytes[2U + i * 3U] = current.canvas[i].r;
        legacyBytes[3U + i * 3U] = current.canvas[i].g;
        legacyBytes[4U + i * 3U] = current.canvas[i].b;
    }
    auto record = legacy.Doc()[modules::LITCHCRAFT_ART_KEY].as<JsonArray>();
    record.remove(4U);
    record.remove(3U);
    record[0] = 1U;
    record[3] = fxe::Crc32(legacyBytes.data(), legacyBytes.size());
    modules::LitchcraftRecord migrated;
    Check(modules::ReadLitchcraftRecord(legacy.Doc(), migrated) ==
                  modules::LitchcraftRecordStatus::Valid &&
              Equal(migrated.quickPalette,
                    modules::LITCHCRAFT_DEFAULT_QUICK_PALETTE),
          "version-one artwork migrates to the default board quick palette");
}
void CanvasAndPaletteShareTrailingDeadline() {
    {
        Fixture f;
        f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
        f.Tick(100U); f.Complete(100U);
        f.Tick(200U); Send(f.art, Art::Op::Paint, 2U, {1U, 0U, 3U, 0U});
        f.Tick(4200U); Send(f.art, Art::Op::Tool, 3U, {0U, 15U, 17U, 34U, 51U});
        f.Tick(5200U);
        Check(f.sink.admissions == 1U,
              "a newer palette edit postpones an older canvas save");
        f.Tick(9199U);
        Check(f.sink.admissions == 1U,
              "combined artwork waits until just before the latest deadline");
        f.Tick(9200U); f.Complete(9200U);
        const auto saved = ReadSaved();
        Check(f.sink.admissions == 2U && !f.persistence.IsDirty() &&
                  saved.canvas[0].r == 255U && saved.quickPalette[0].r == 17U,
              "canvas then palette commits in one verified artwork save");
    }
    {
        Fixture f;
        f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
        f.Tick(100U); f.Complete(100U);
        f.Tick(200U); Send(f.art, Art::Op::Tool, 2U, {0U, 15U, 17U, 34U, 51U});
        f.Tick(4200U); Send(f.art, Art::Op::Paint, 3U, {1U, 0U, 3U, 0U});
        f.Tick(5200U);
        Check(f.sink.admissions == 1U,
              "a newer canvas edit postpones an older palette save");
        f.Tick(9199U);
        Check(f.sink.admissions == 1U,
              "reverse-order artwork waits until just before the latest deadline");
        f.Tick(9200U); f.Complete(9200U);
        const auto saved = ReadSaved();
        Check(f.sink.admissions == 2U && !f.persistence.IsDirty() &&
                  saved.canvas[0].r == 17U && saved.quickPalette[0].r == 17U,
              "palette then canvas commits in one verified artwork save");
    }
}

modules::litchcraft_animation::ValidatedDocument AdvancedDocument(
    const uint8_t frameCount, const uint8_t marker) {
    using namespace modules::litchcraft_animation;
    Document document;
    document.frameCount = frameCount;
    document.sparkleSeed = 0x10203040U + marker;
    for (uint8_t index = 0U; index < document.favorites.size(); ++index) {
        document.favorites[index] = {uint8_t(marker + index + 1U),
                                     uint8_t(index * 3U + 1U),
                                     uint8_t(index * 5U + 2U)};
    }
    for (uint8_t frame = 0U; frame < frameCount; ++frame) {
        document.frames[frame].durationMs = uint16_t(100U * (frame + 1U));
        document.frames[frame].emitters[0].color = {marker, frame, 1U};
    }
    ValidatedDocument adopted;
    Check(ValidatedDocument::TryAdopt(document, adopted),
          "advanced fixture adopts a valid schema-three document");
    return adopted;
}

struct AdvancedSink {
    Persistence::AdvancedEnvelope current{};
    Persistence::AdvancedEnvelope lastGood{};
    Persistence::AdvancedEnvelope pending{};
    Persistence::SaveOutcome outcome = Persistence::SaveOutcome::Pending;
    uint32_t retained = 0U;
    uint32_t nextReceipt = 0U;
    unsigned jsonAdmissions = 0U;
    unsigned binaryAdmissions = 0U;
    unsigned reads = 0U;
    uint32_t readCostMs = 0U;
    Admission admission = Admission::Accepted;
    std::string frozenJson;
    bool pendingAdvanced = false;
    bool currentPresent = false;
    bool lastGoodPresent = false;
    bool failCurrentRead = false;
    bool failLastGoodRead = false;
    bool reportOversizedCurrent = false;

    Persistence::Ops Ops() {
        return {
            this,
            [](void* context, const JsonDocument& document, uint32_t& receipt) {
                auto& self = *static_cast<AdvancedSink*>(context);
                if (self.admission != Admission::Accepted) return self.admission;
                if (self.retained != 0U) return Admission::Deferred;
                self.frozenJson.clear();
                serializeJson(document, self.frozenJson);
                receipt = self.retained = ++self.nextReceipt;
                self.outcome = Persistence::SaveOutcome::Pending;
                self.pendingAdvanced = false;
                ++self.jsonAdmissions;
                return Admission::Accepted;
            },
            [](void* context, const uint32_t receipt) {
                auto& self = *static_cast<AdvancedSink*>(context);
                Check(receipt == self.retained,
                      "advanced poll owns the exact retained receipt");
                return self.outcome;
            },
            [](void* context, const uint32_t receipt) {
                auto& self = *static_cast<AdvancedSink*>(context);
                Check(receipt == self.retained,
                      "advanced release owns the exact retained receipt");
                self.retained = 0U;
            },
            [](void* context, const Persistence::AdvancedSlot slot,
               uint8_t* const bytes, const size_t capacity, size_t& size) {
                auto& self = *static_cast<AdvancedSink*>(context);
                ++self.reads;
                fxe::ManualMillis::SetNowMs(fxe::Millis() + self.readCostMs);
                if ((slot == Persistence::AdvancedSlot::Current &&
                     self.failCurrentRead) ||
                    (slot == Persistence::AdvancedSlot::LastGood &&
                     self.failLastGoodRead)) {
                    return Persistence::AdvancedReadOutcome::Failed;
                }
                if (slot == Persistence::AdvancedSlot::Current &&
                    self.reportOversizedCurrent) {
                    size = Persistence::ADVANCED_SLOT_BYTES + 1U;
                    return Persistence::AdvancedReadOutcome::Present;
                }
                const bool present = slot == Persistence::AdvancedSlot::Current
                    ? self.currentPresent : self.lastGoodPresent;
                const auto& source = slot == Persistence::AdvancedSlot::Current
                    ? self.current : self.lastGood;
                if (!present) return Persistence::AdvancedReadOutcome::Missing;
                if (source.size > capacity) return Persistence::AdvancedReadOutcome::Failed;
                std::copy_n(source.bytes.begin(), source.size, bytes);
                size = source.size;
                return Persistence::AdvancedReadOutcome::Present;
            },
            [](void* context, const uint8_t* const bytes, const size_t size,
               uint32_t& receipt) {
                auto& self = *static_cast<AdvancedSink*>(context);
                if (self.admission != Admission::Accepted) return self.admission;
                if (self.retained != 0U) return Admission::Deferred;
                if (size > self.pending.bytes.size()) return Admission::Rejected;
                std::copy_n(bytes, size, self.pending.bytes.begin());
                self.pending.size = size;
                receipt = self.retained = ++self.nextReceipt;
                self.outcome = Persistence::SaveOutcome::Pending;
                self.pendingAdvanced = true;
                ++self.binaryAdmissions;
                return Admission::Accepted;
            }};
    }

    void CompleteSuccess() {
        Check(retained != 0U, "advanced completion requires an accepted record");
        if (pendingAdvanced) {
            if (currentPresent) {
                lastGood = current;
                lastGoodPresent = true;
            }
            current = pending;
            currentPresent = true;
        }
        outcome = Persistence::SaveOutcome::Saved;
    }

    void CompleteInterrupted() {
        Check(retained != 0U, "interruption requires an accepted record");
        if (currentPresent) {
            lastGood = current;
            lastGoodPresent = true;
        }
        current = pending;
        currentPresent = true;
        current.bytes[current.size - 1U] ^= 0x80U;
        outcome = Persistence::SaveOutcome::Failed;
    }
};

struct DeferredEncodingSink {
    runtime::GameStateMachine* stateMachine = nullptr;
    const modules::litchcraft_animation::Document* document = nullptr;
    modules::litchcraft_animation::Document frozenDocument{};
    Persistence::AdvancedEnvelope* envelope = nullptr;
    Persistence::SaveOutcome outcome = Persistence::SaveOutcome::Pending;
    uint32_t retained = 0U;
    uint32_t nextReceipt = 0U;
    unsigned documentAdmissions = 0U;
    unsigned staticAdmissions = 0U;
    bool workerMutexHeld = true;
    bool active = false;
    bool advancedPending = false;

    Persistence::Ops Ops() {
        Persistence::Ops ops;
        ops.context = this;
        ops.enqueue = [](void* context, const JsonDocument&, uint32_t& receipt) {
            auto& self = *static_cast<DeferredEncodingSink*>(context);
            if (self.retained != 0U) return Admission::Deferred;
            receipt = self.retained = ++self.nextReceipt;
            self.outcome = Persistence::SaveOutcome::Pending;
            self.advancedPending = false;
            ++self.staticAdmissions;
            return Admission::Accepted;
        };
        ops.poll = [](void* context, const uint32_t receipt) {
            auto& self = *static_cast<DeferredEncodingSink*>(context);
            Check(receipt == self.retained,
                  "deferred encoding poll owns exact retained receipt");
            return self.outcome;
        };
        ops.release = [](void* context, const uint32_t receipt) {
            auto& self = *static_cast<DeferredEncodingSink*>(context);
            Check(receipt == self.retained,
                  "deferred encoding release owns exact retained receipt");
            self.retained = 0U;
            self.document = nullptr;
            self.envelope = nullptr;
        };
        ops.readAdvanced = [](void*, Persistence::AdvancedSlot, uint8_t*,
                              size_t, size_t&) {
            return Persistence::AdvancedReadOutcome::Missing;
        };
        ops.enqueueAdvancedDocument = [](
            void* context,
            const modules::litchcraft_animation::Document& document,
            const bool active, Persistence::AdvancedEnvelope& envelope,
            uint32_t& receipt) {
            auto& self = *static_cast<DeferredEncodingSink*>(context);
            Check(self.stateMachine != nullptr &&
                      self.stateMachine->IsPlacementModeActive(),
                  "accepted Exit restores Setup before persistence admission");
            if (self.workerMutexHeld || self.retained != 0U) {
                return Admission::Deferred;
            }
            self.frozenDocument = document;
            self.document = &self.frozenDocument;
            self.envelope = &envelope;
            self.active = active;
            self.advancedPending = true;
            self.outcome = Persistence::SaveOutcome::Pending;
            receipt = self.retained = ++self.nextReceipt;
            ++self.documentAdmissions;
            return Admission::Accepted;
        };
        return ops;
    }

    void CompleteAdvanced() {
        Check(advancedPending && document != nullptr && envelope != nullptr,
              "background encoder owns the admitted frozen document");
        Check(Persistence::EncodeAdvancedEnvelope(
                  *document, active, *envelope),
              "background worker encodes the frozen advanced document");
        outcome = Persistence::SaveOutcome::Saved;
    }

    void CompleteStatic() {
        Check(!advancedPending && retained != 0U,
              "inactive mode settings follow advanced durability");
        outcome = Persistence::SaveOutcome::Saved;
    }
};

struct V2Fixture {
    enum class Authority { Advanced, Static, Unconfigured };
    modules::SettingsStore settings{Config()};
    Art art;
    AdvancedSink sink;
    Persistence::AdvancedStorage storage;
    Persistence persistence{settings, art};
    uint16_t next = 2U;
    uint32_t epoch = 1U;
    bool eligible = true;
    uint64_t occupancy = 0U;
    explicit V2Fixture(Authority authority = Authority::Advanced,
                       const bool active = true) {
        fxe::ManualMillis::SetNowMs(0U);
        art.Configure(3U, 1000U);
        art.BeginConnection();
        if (active) {
            Send(art, Art::Op::Enter, 1U);
            Drain(art);
        }
        persistence.Configure(sink.Ops());
        if (authority == Authority::Unconfigured) return;
        persistence.ConfigureAdvancedStorage(storage);
        if (authority == Authority::Advanced) {
            Check(persistence.SetAdvancedDocument(AdvancedDocument(8U, 11U), active), "v2 initial authority");
        }
    }
    static uint32_t Read(const uint8_t* bytes, size_t count) {
        uint32_t value = 0U;
        for (size_t i = 0U; i < count; ++i) value |= uint32_t(bytes[i]) << (i * 8U);
        return value;
    }
    static void Put(Art::BulkPacket& packet, uint32_t value, size_t count) {
        for (size_t i = 0U; i < count; ++i) packet.bytes[packet.size++] = value >> (i * 8U);
    }
    Art::BulkPacket Request(uint8_t op) {
        Art::BulkPacket packet;
        packet.bytes[packet.size++] = 0x4dU;
        packet.bytes[packet.size++] = 2U;
        packet.bytes[packet.size++] = op;
        Put(packet, art.SessionId(), 4U);
        Put(packet, next++, 2U);
        return packet;
    }
    void Ingress(const Art::BulkPacket& packet) {
        // Same normalizer and bounded union/queue as char0106's SDK callback.
        modules::BleEvent event;
        modules::BleEventQueue queue;
        Check(modules::MakeLitchcraftIngressEvent(packet.bytes.data(), packet.size, epoch, event), "v2 callback admits complete bytes");
        Check(queue.Begin() && queue.Enqueue(event) && queue.TryDequeue(event), "v2 callback event survives bounded queue");
        Check(event.payload.litchcraft.connectionEpoch == epoch, "v2 callback preserves connection epoch");
        art.HandlePacket(event.payload.litchcraft.input, event.payload.litchcraft.len,
                         eligible, occupancy);
    }
    Art::BulkPacket Pop(size_t mtu = 244U) {
        Art::BulkPacket packet;
        Check(art.PeekPacket(packet, mtu), "v2 expected notification available");
        if (packet.size != 0U) art.AcceptPacket();
        return packet;
    }
    Art::BulkPacket Ack(const Art::BulkPacket& request, Art::Status status = Art::Status::Ok) {
        // Drain prior unsolicited playback/tool state; dedicated cases below
        // explicitly consume and verify those events before another request.
        Art::BulkPacket pending;
        while (art.PeekPacket(pending, 244U) &&
               ((pending.bytes[0] == 0x4dU &&
                 (pending.bytes[2] == 0x88U || pending.bytes[2] == 0x89U)) ||
                (pending.bytes[0] == 0x4cU && pending.bytes[1] == 0x82U))) art.AcceptPacket();
        Ingress(request);
        const auto packet = Pop();
        if (packet.bytes[13] != uint8_t(status) || packet.bytes[2] != 0x81U) {
            std::printf("[V2_ACK] op=%u request=%u actual_event=%u actual_status=%u expected=%u\n",
                        request.bytes[2], unsigned(Read(request.bytes.data() + 7U, 2U)),
                        packet.bytes[2], packet.bytes[13], uint8_t(status));
        }
        Check(packet.bytes[0] == 0x4dU && packet.bytes[2] == 0x81U &&
                  Read(packet.bytes.data() + 11U, 2U) == Read(request.bytes.data() + 7U, 2U) &&
                  packet.bytes[13] == uint8_t(status), "v2 exact request/status ACK");
        Check(packet.size == 18U && (packet.bytes[14] & 0xf0U) == 0U,
              "v2 State remains 18 bytes with reserved flag bits clear");
        return packet;
    }
    modules::litchcraft_animation::EncodedDocument Encoded(
        uint8_t marker = 19U, uint8_t frameCount = 8U) {
        modules::litchcraft_animation::EncodedDocument bytes;
        Check(modules::litchcraft_animation::EncodeDocument(
                  AdvancedDocument(frameCount, marker), bytes),
              "v2 encode canonical fixture");
        return bytes;
    }
    Art::BulkPacket Begin(const modules::litchcraft_animation::EncodedDocument& bytes) {
        auto packet = Request(9U);
        Put(packet, persistence.AdvancedGeneration(), 4U);
        Put(packet, bytes.size, 2U);
        Put(packet, bytes.crc32, 4U);
        return packet;
    }
    Art::BulkPacket TransferOp(uint8_t op, uint16_t transfer) {
        auto packet = Request(op);
        Put(packet, transfer, 2U);
        return packet;
    }
    void Upload(const modules::litchcraft_animation::EncodedDocument& bytes,
                uint16_t transfer, size_t payload) {
        const auto baseRevision = persistence.AdvancedGeneration();
        for (size_t offset = 0U; offset < bytes.size;) {
            auto packet = TransferOp(10U, transfer);
            Put(packet, offset, 2U);
            const auto count = std::min(payload - 13U, bytes.size - offset);
            std::copy_n(bytes.bytes.begin() + offset, count, packet.bytes.begin() + packet.size);
            packet.size += count;
            Ack(packet);
            offset += count;
            Check(persistence.AdvancedGeneration() == baseRevision, "partial canonical bytes never replace live authority");
        }
    }
};

Art::BulkPacket V2Edit(V2Fixture& f, uint8_t op,
                       std::initializer_list<uint8_t> payload) {
    auto packet = f.Request(op);
    for (auto value : payload) V2Fixture::Put(packet, value, 1U);
    return packet;
}
Art::BulkPacket V2FrameEdit(V2Fixture& f, uint8_t action, uint8_t source,
                           uint8_t destination = 0U, uint16_t duration = 0U) {
    auto packet = f.Request(5U);
    V2Fixture::Put(packet, f.persistence.AdvancedGeneration(), 4U);
    for (auto value : {action, source, destination}) V2Fixture::Put(packet, value, 1U);
    V2Fixture::Put(packet, duration, 2U);
    return packet;
}
void V2EditValidationMasksEffectsAndStrokeAuthority() {
    V2Fixture f;
    auto revision = f.persistence.AdvancedGeneration();
    const auto original = f.persistence.AdvancedDocument();
    for (const uint8_t mask : {1U, 3U, 7U, 15U}) {
        auto paint = V2Edit(f, 3U, {0, 1, 0, 3, mask, 61, 72, 83, 2, 3, 4});
        const auto ack = f.Ack(paint);
        Check(f.persistence.AdvancedGeneration() == ++revision, "one multi-square packet is exactly one document revision");
        Check(f.Ack(paint).bytes == ack.bytes && f.persistence.AdvancedGeneration() == revision,
              "Paint retry replays frozen ACK without repeating revision or operation");
        const auto& emitters = f.persistence.AdvancedDocument().Value().frames[0].emitters;
        for (size_t square : {2U, 3U, 4U}) for (size_t slot = 0U; slot < 4U; ++slot) {
            if ((mask & (1U << slot)) != 0U) Check(emitters[square * 4U + slot].color.r == 61U, "one through four mask emitters receive exact intended RGB");
        }
        f.Ack(V2Edit(f, 3U, {0, 1, 0, 3, mask, 61, 72, 83, 2, 3, 4}));
        Check(f.persistence.AdvancedGeneration() == revision, "identical semantic paint with fresh request is a no-op");
    }
    for (uint8_t effect = 0U; effect <= 8U; ++effect) {
        const uint16_t duration = effect == 0U ? 0U : 500U;
        auto packet = V2Edit(f, 4U, {0, effect, uint8_t(duration), uint8_t(duration >> 8U), 15, 2, 0, 3, 2});
        const auto prior = f.persistence.AdvancedGeneration();
        const auto ack = f.Ack(packet);
        Check(f.Ack(packet).bytes == ack.bytes, "Effect retry exact frozen ACK");
        const auto& emitter = f.persistence.AdvancedDocument().Value().frames[0].emitters[8];
        Check(uint8_t(emitter.effect) == effect && emitter.durationMs == duration && emitter.color.r == 61U,
              "all eight effect styles and Off preserve source RGB");
        f.Ack(V2Edit(f, 4U, {0, effect, uint8_t(duration), uint8_t(duration >> 8U), 15, 2, 0, 3, 2}));
        Check(f.persistence.AdvancedGeneration() == prior + (effect == 0U ? 0U : 1U), "effect no-op and changed revision exact");
    }
    revision = f.persistence.AdvancedGeneration();
    for (const auto malformed : {
             V2Edit(f, 3U, {8, 1, 0, 3, 15, 1, 2, 3, 0}),
             V2Edit(f, 3U, {0, 0, 0, 3, 15, 1, 2, 3, 0}),
             V2Edit(f, 3U, {0, 1, 0, 4, 15, 1, 2, 3, 0}),
             V2Edit(f, 3U, {0, 1, 0, 3, 0, 1, 2, 3, 0}),
             V2Edit(f, 3U, {0, 1, 0, 3, 16, 1, 2, 3, 0}),
             V2Edit(f, 3U, {0, 1, 0, 3, 15, 1, 2, 3, 0, 64}),
             V2Edit(f, 4U, {0, 1, 144, 1, 15, 1, 0, 3, 0}),
             V2Edit(f, 4U, {0, 1, 244, 1, 15, 0, 0, 3, 0}),
             V2Edit(f, 4U, {0, 0, 244, 1, 15, 1, 0, 3, 0})}) f.Ack(malformed, Art::Status::Malformed);
    f.Ack(V2Edit(f, 4U, {0, 9, 244, 1, 15, 1, 0, 3, 0}), Art::Status::Unsupported);
    Check(f.persistence.AdvancedGeneration() == revision, "every invalid field rejected before any square/revision mutation");
    const auto undoBefore = f.art.UndoCount();
    f.Ack(V2Edit(f, 3U, {1, 11, 0, 1, 1, 10, 20, 30, 1}));
    f.Ack(V2Edit(f, 4U, {2, 1, 244, 1, 1, 11, 0, 2, 1}), Art::Status::Malformed);
    f.Ack(V2Edit(f, 2U, {2, 2}), Art::Status::Busy);
    f.Ack(V2FrameEdit(f, 2U, 1U, 2U), Art::Status::Busy);
    f.Ack(V2Edit(f, 4U, {1, 8, 16, 39, 1, 11, 0, 2, 1}));
    Check(f.art.UndoCount() == undoBefore + 1U, "RGB/effect packets share one frame-latched stroke operation");
    f.Ack(f.Request(6U));
    Check(f.persistence.AdvancedDocument().Value().frames[1].emitters[4].color.r == original.Value().frames[1].emitters[4].color.r,
          "Undo restores entire mixed stroke");
    f.Ack(f.Request(7U));
    Check(uint8_t(f.persistence.AdvancedDocument().Value().frames[1].emitters[4].effect) == 8U,
          "Redo restores RGB/effect operation");
    const auto eraseUndoBefore = f.art.UndoCount();
    f.Ack(V2Edit(f, 4U, {1, 0, 0, 0, 1, 12, 0, 1, 1}));
    f.Ack(V2Edit(f, 3U, {1, 12, 0, 2, 1, 0, 0, 0, 1}));
    const auto& erased = f.persistence.AdvancedDocument().Value().frames[1].emitters[4];
    Check(erased.color.r == 0U && erased.color.g == 0U && erased.color.b == 0U &&
              erased.effect == modules::litchcraft_animation::Effect::Off && erased.durationMs == 0U,
          "erase clears RGB and effect metadata for the selected emitter");
    Check(f.art.UndoCount() == eraseUndoBefore + 1U,
          "paired effect-off and black paint are one erase undo boundary");
    f.Ack(f.Request(6U));
    const auto& restored = f.persistence.AdvancedDocument().Value().frames[1].emitters[4];
    Check(restored.effect == modules::litchcraft_animation::Effect::Sparkle && restored.durationMs == 10000U,
          "one Undo restores the erased emitter RGB and effect metadata");
    std::puts("[V2_EDITS] paint/effect/masks/validation/exact revisions/stroke latch/undo/redo passed");
}

void V2AtomicCanvasEdit() {
    V2Fixture f;
    Send(f.art, Art::Op::Tool, f.next++, {1U, 2U, 9U, 10U, 11U});
    Drain(f.art);
    Check(f.art.IsErasing(), "held-swatch Fill starts from a different Erase tool");
    const auto initial = f.persistence.AdvancedDocument();
    auto revision = f.persistence.AdvancedGeneration();
    const auto undoBefore = f.art.UndoCount();
    f.Ack(f.Request(13U));
    Check(f.persistence.AdvancedGeneration() == revision,
          "canvas-edit feature probe is read-only and minimum-MTU safe");
    const auto request = [&](const bool erase, const uint8_t frame = 0U) {
        auto packet = f.Request(13U);
        V2Fixture::Put(packet, f.persistence.AdvancedGeneration(), 4U);
        // Shared wire vector: base revision LE, frame, mask, RGB, erase flag.
        for (const auto byte : {frame, uint8_t(erase ? 15U : 5U),
                               uint8_t(erase ? 0U : 0x34U),
                               uint8_t(erase ? 0U : 0x56U),
                               uint8_t(erase ? 0U : 0x78U), uint8_t(erase)}) {
            V2Fixture::Put(packet, byte, 1U);
        }
        return packet;
    };
    const auto fill = request(false);
    Check(fill.size == 19U && fill.bytes[2] == 13U && fill.bytes[14] == 5U &&
              fill.bytes[15] == 0x34U && fill.bytes[18] == 0U,
          "Fill encodes one 19-byte command at the minimum ATT payload");
    f.Ingress(fill);
    const auto& painted = f.persistence.AdvancedDocument().Value();
    Check(f.persistence.AdvancedGeneration() == revision + 1U &&
              f.art.Artwork()[252U].r == 0x34U,
          "all selected-frame emitters are authoritative/renderable before ACK drain");
    Check(!f.art.IsErasing() && f.art.Target() == 5U &&
              f.art.Color().r == 0x34U && f.art.Color().g == 0x56U &&
              f.art.Color().b == 0x78U && f.art.UndoCount() == undoBefore + 1U,
          "one accepted Fill atomically selects its Paint RGB/mask with exactly one history entry");
    for (size_t index = 0; index < 256U; ++index) {
        const auto& old = initial.Value().frames[0].emitters[index];
        const auto& pixel = painted.frames[0].emitters[index];
        Check(pixel.color.r == ((index % 2U == 0U) ? 0x34U : old.color.r) &&
                  pixel.effect == old.effect && pixel.durationMs == old.durationMs,
              "Fill honors mask and preserves effects on all 64 squares");
        Check(painted.frames[1].emitters[index].color.r ==
                  initial.Value().frames[1].emitters[index].color.r,
              "hidden animation frames are unchanged");
    }
    const auto ack = f.Pop();
    Check(ack.bytes[0] == 0x4dU && ack.bytes[2] == 0x81U && ack.bytes[13] == 0U,
          "Fill queues its authoritative V2 ACK before asynchronous tool readback");
    const auto tool = f.Pop();
    Check(tool.bytes[0] == 0x4cU && tool.bytes[1] == 0x82U &&
              V2Fixture::Read(tool.bytes.data() + 10U, 2U) == 0U &&
              tool.bytes[12] == 0x34U && tool.bytes[13] == 0x56U &&
              tool.bytes[14] == 0x78U && tool.bytes[15] == 0U && tool.bytes[16] == 5U,
          "Fill publishes exact unsolicited Paint RGB/mask through existing Tool readback");
    f.Ack(fill);
    Check(f.persistence.AdvancedGeneration() == revision + 1U &&
              f.art.UndoCount() == undoBefore + 1U,
          "lost-ACK replay creates exactly one whole-canvas history operation");
    Art::BulkPacket replayEvent;
    while (f.art.PeekPacket(replayEvent, 244U)) {
        Check(replayEvent.bytes[0] != 0x4cU || replayEvent.bytes[1] != 0x82U,
              "lost-ACK replay never repeats tool selection or its event");
        f.art.AcceptPacket();
    }
    const auto clear = request(true);
    f.Ack(clear);
    Check(!f.art.IsErasing() && f.art.Target() == 5U && f.art.Color().r == 0x34U,
          "Erase All preserves the selected Paint tool and color");
    for (const auto& pixel : f.persistence.AdvancedDocument().Value().frames[0].emitters) {
        Check(pixel.color.r == 0U && pixel.color.g == 0U && pixel.color.b == 0U &&
                  pixel.effect == modules::litchcraft_animation::Effect::Off && pixel.durationMs == 0U,
              "Erase All atomically clears all 256 colors and effects");
    }
    f.Ack(f.Request(6U));
    Check(f.persistence.AdvancedDocument().Value().frames[0].emitters[252U].color.r == 0x34U,
          "one Undo restores the complete erased frame");
    f.Ack(f.Request(6U));
    Check(modules::litchcraft_animation::DocumentsEqual(
              initial.Value(), f.persistence.AdvancedDocument().Value()),
          "second Undo restores pre-Fill authority with hidden frames and effects");
    auto stale = request(false);
    stale.bytes[9] ^= 1U;
    f.Ack(stale, Art::Status::RevisionConflict);
    f.Ack(request(false, 1U), Art::Status::RevisionConflict);
    auto malformed = request(true);
    malformed.bytes[14] = 1U;
    f.Ack(malformed, Art::Status::Malformed);
    auto oldSession = request(false);
    oldSession.bytes[3] ^= 1U;
    f.Ack(oldSession, Art::Status::StaleSession);
    Check(modules::litchcraft_animation::DocumentsEqual(
              initial.Value(), f.persistence.AdvancedDocument().Value()),
          "stale revision/frame/session and malformed erasure never mutate authority");
    Check(!f.art.IsErasing() && f.art.Target() == 5U && f.art.Color().r == 0x34U,
          "rejected Canvas Edits preserve the active tool");
    f.art.EndConnection();
    f.art.BeginConnection();
    f.Ack(f.Request(13U));
    f.Ack(request(false));
    Check(f.persistence.AdvancedDocument().Value().frames[0].emitters[0].color.r == 0x34U,
          "reconnect reacquires feature support against retained document authority");
}

void ConnectionOrderedToolUsesDomainOwner() {
    V2Fixture f;
    const std::array<uint8_t, 5U> tool = {0U, 5U, 0x34U, 0x56U, 0x78U};
    Check(f.art.ApplyConnectionOrdered(5U, tool.data(), tool.size()) ==
              Art::Status::Ok &&
              !f.art.IsErasing() && f.art.Target() == 5U &&
              f.art.Color().r == 0x34U && f.art.Color().g == 0x56U &&
              f.art.Color().b == 0x78U,
          "connection Tool delegates to authoritative Litchcraft selection");
    const std::array<uint8_t, 5U> malformed = {0U, 0U, 1U, 2U, 3U};
    Check(f.art.ApplyConnectionOrdered(5U, malformed.data(),
                                       malformed.size()) ==
              Art::Status::Malformed &&
              f.art.Target() == 5U && f.art.Color().r == 0x34U,
          "invalid connection Tool cannot mutate domain state");

    const std::array<uint8_t, 2U> select = {2U, 1U};
    const std::array<uint8_t, 2U> play = {1U, 1U};
    const std::array<uint8_t, 2U> pause = {0U, 1U};
    Check(f.art.ApplyConnectionOrdered(2U, select.data(), select.size()) ==
              Art::Status::Ok &&
              f.art.SelectedFrame() == 1U && !f.art.IsAnimationPlaying(),
          "connection Timeline selects through the existing runtime owner");
    Check(f.art.ApplyConnectionOrdered(2U, play.data(), play.size()) ==
              Art::Status::Ok && f.art.IsAnimationPlaying(),
          "connection Timeline resumes the selected frame");
    fxe::ManualMillis::AdvanceMs(75U);
    Check(f.art.ApplyConnectionOrdered(2U, pause.data(), pause.size()) ==
              Art::Status::Ok && !f.art.IsAnimationPlaying() &&
              f.art.AnimationFrameElapsedMs() == 75U,
          "connection Timeline pause retains an exact frame-local anchor");
}

void V2AtomicCheckerEdit() {
    using namespace modules::litchcraft_animation;
    for (const uint8_t action : {5U, 6U, 7U, 8U}) {
        V2Fixture f;
        Document detailed = f.persistence.AdvancedDocument().Value();
        for (size_t index = 0U; index < EMITTER_COUNT; ++index) {
            detailed.frames[0].emitters[index] = {
                {static_cast<uint8_t>(index), static_cast<uint8_t>(index + 1U),
                 static_cast<uint8_t>(255U - index)}, Effect::Sparkle, 1000U};
        }
        ValidatedDocument validated;
        Check(ValidatedDocument::TryAdopt(detailed, validated) &&
                  f.persistence.SetAdvancedDocument(validated, true),
              "checker fixture retains asymmetric RGB/effects");
        const auto initial = f.persistence.AdvancedDocument();
        const auto revision = f.persistence.AdvancedGeneration();
        const auto undoBefore = f.art.UndoCount();
        auto probe = f.Request(13U);
        V2Fixture::Put(probe, 3U, 1U);
        f.Ack(probe);
        Check(f.persistence.AdvancedGeneration() == revision,
              "canvas extension probe is read-only");
        auto checker = f.Request(13U);
        V2Fixture::Put(checker, revision, 4U);
        const bool erase = action >= 7U;
        for (const uint8_t byte : {uint8_t(0), uint8_t(5),
                                   uint8_t(erase ? 0U : 0x12U),
                                   uint8_t(erase ? 0U : 0x34U),
                                   uint8_t(erase ? 0U : 0x56U), action}) {
            V2Fixture::Put(checker, byte, 1U);
        }
        Check(checker.size == 19U, "checker edit fits one minimum-ATT packet");
        f.Ack(checker);
        const auto& edited = f.persistence.AdvancedDocument().Value().frames[0];
        const bool targetDark = action == 6U || action == 8U;
        for (uint8_t square = 0U; square < 64U; ++square) {
            const bool dark = ((square % 8U) + (square / 8U)) % 2U != 0U;
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                const auto index = static_cast<size_t>(square) * 4U + slot;
                const auto& pixel = edited.emitters[index];
                const auto& old = initial.Value().frames[0].emitters[index];
                const bool selected = dark == targetDark && (slot == 0U || slot == 2U);
                Check(pixel.color.r == (selected ? (erase ? 0U : 0x12U) : old.color.r) &&
                          pixel.color.g == (selected ? (erase ? 0U : 0x34U) : old.color.g) &&
                          pixel.color.b == (selected ? (erase ? 0U : 0x56U) : old.color.b) &&
                          pixel.effect == (selected && erase ? Effect::Off : old.effect) &&
                          static_cast<uint16_t>(pixel.durationMs) ==
                              (selected && erase ? 0U : static_cast<uint16_t>(old.durationMs)),
                      "checker actions change only exact parity/mask RGB/effect/duration");
            }
        }
        Check(f.persistence.AdvancedGeneration() == revision + 1U &&
                  f.art.UndoCount() == undoBefore + 1U,
              "checker edit is one authoritative history boundary");
    }
    V2Fixture malformed;
    const auto revision = malformed.persistence.AdvancedGeneration();
    auto erase = malformed.Request(13U);
    V2Fixture::Put(erase, revision, 4U);
    for (const uint8_t byte : {uint8_t(0), uint8_t(5), uint8_t(1),
                               uint8_t(0), uint8_t(0), uint8_t(7)}) {
        V2Fixture::Put(erase, byte, 1U);
    }
    malformed.Ack(erase, Art::Status::Malformed);
    Check(malformed.persistence.AdvancedGeneration() == revision,
          "checker erase rejects nonzero RGB before mutation");
    std::puts("[V2_CHECKER] actions5-8/parity/mask/erase/effects/malformed RGB passed");
}

void V2AtomicRotation() {
    using namespace modules::litchcraft_animation;
    for (uint8_t action : {2U, 3U, 4U}) {
        V2Fixture f;
        Document detailed = f.persistence.AdvancedDocument().Value();
        for (uint8_t frame = 0U; frame < detailed.frameCount; ++frame) {
            for (size_t index = 0U; index < EMITTER_COUNT; ++index) {
                const auto effect = static_cast<Effect>(index % 9U);
                detailed.frames[frame].emitters[index] = {
                    {static_cast<uint8_t>(index), static_cast<uint8_t>(frame + 3U),
                     static_cast<uint8_t>(255U - index)}, effect,
                    static_cast<uint16_t>(effect == Effect::Off ? 0U : 500U + 100U * (index % 5U))};
            }
        }
        ValidatedDocument validated;
        Check(ValidatedDocument::TryAdopt(detailed, validated) &&
                  f.persistence.SetAdvancedDocument(validated, true),
              "rotation fixture retains asymmetric RGB/effects on all eight frames");
        const auto initial = f.persistence.AdvancedDocument();
        const auto revision = f.persistence.AdvancedGeneration();
        auto probe = f.Request(13U);
        V2Fixture::Put(probe, 2U, 1U);
        f.Ack(probe);
        Check(f.persistence.AdvancedGeneration() == revision,
              "rotation extension probe is read-only");
        const auto request = [&]() {
            auto packet = f.Request(13U);
            V2Fixture::Put(packet, f.persistence.AdvancedGeneration(), 4U);
            for (uint8_t byte : {uint8_t(0), uint8_t(15), uint8_t(0),
                                 uint8_t(0), uint8_t(0), action}) {
                V2Fixture::Put(packet, byte, 1U);
            }
            return packet;
        };
        auto malformed = request(); malformed.bytes[14] = 1U;
        f.Ack(malformed, Art::Status::Malformed);
        malformed = request(); malformed.bytes[15] = 1U;
        f.Ack(malformed, Art::Status::Malformed);
        auto stale = request(); stale.bytes[9] ^= 1U;
        f.Ack(stale, Art::Status::RevisionConflict);
        auto wrongFrame = request(); wrongFrame.bytes[13] = 1U;
        f.Ack(wrongFrame, Art::Status::RevisionConflict);
        Check(DocumentsEqual(initial.Value(), f.persistence.AdvancedDocument().Value()),
              "invalid rotation cannot change any frame or effect");
        Document expected = initial.Value();
        const uint8_t turns = action - 1U;
        for (uint8_t destination = 0U; destination < 64U; ++destination) {
            const uint8_t row = destination / 8U;
            const uint8_t column = destination % 8U;
            // Independent inverse-coordinate oracle, including asymmetric
            // RGB/effect metadata at every emitter in the fixture.
            const uint8_t source = action == 2U ? (7U - column) * 8U + row :
                action == 3U ? (7U - row) * 8U + 7U - column :
                               column * 8U + 7U - row;
            for (uint8_t emitter = 0U; emitter < 4U; ++emitter) {
                expected.frames[0].emitters[destination * 4U + emitter] =
                    initial.Value().frames[0].emitters[source * 4U + (emitter + 4U - turns) % 4U];
            }
        }
        const auto rotate = request();
        f.Ack(rotate);
        Check(rotate.size == 19U && !f.art.IsAnimationPlaying() &&
                  f.persistence.AdvancedGeneration() == revision + 1U &&
                  f.art.UndoCount() == 1U &&
                  DocumentsEqual(expected, f.persistence.AdvancedDocument().Value()),
              "one minimum-MTU rotation atomically maps all colors, effects and directions on selected frame only");
        f.Ack(rotate);
        Check(f.persistence.AdvancedGeneration() == revision + 1U && f.art.UndoCount() == 1U,
              "rotation receipt replay cannot rotate again or add history");
        f.Ack(f.Request(6U));
        Check(DocumentsEqual(initial.Value(), f.persistence.AdvancedDocument().Value()),
              "one Undo restores all selected-frame colors and effect metadata");
    }
}

void V2InspectRepublishesSharedTool() {
    V2Fixture f;
    const auto expectTool = [&](const Art::Packet& packet, const bool erase,
                                const uint16_t request) {
        Check(packet.size == 17U && packet.bytes[0] == 0x4cU && packet.bytes[1] == 0x82U &&
                  V2Fixture::Read(packet.bytes.data() + 2U, 4U) == f.art.SessionId() &&
                  V2Fixture::Read(packet.bytes.data() + 10U, 2U) == request &&
                  packet.bytes[12] == 0x12U && packet.bytes[13] == 0x34U &&
                  packet.bytes[14] == 0x56U && packet.bytes[15] == uint8_t(erase) &&
                  packet.bytes[16] == 5U,
              "shared Tool readback preserves exact session, RGB, Erase and target mask");
    };
    const auto appTool = [&](const bool erase) {
        const uint16_t id = f.next++;
        const auto compact = Request(f.art, Art::Op::Tool, id,
                                     {uint8_t(erase), 5U, 0x12U, 0x34U, 0x56U});
        Art::BulkPacket ingress;
        ingress.size = compact.size;
        std::copy_n(compact.bytes.begin(), compact.size, ingress.bytes.begin());
        f.Ingress(ingress);
        const auto packets = Drain(f.art);
        Check(f.art.IsErasing() == erase && packets.size() == 2U &&
                  HasAck(packets, id, Art::Status::Ok),
              "compact app Tool selects the board's shared Draw/Erase state and acknowledges it");
        if (!packets.empty()) expectTool(packets.front(), erase, id);
    };
    const auto holdCircle = [&]() {
        f.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                             modules::UiOwner::P1});
        fxe::ManualMillis::AdvanceMs(1000U);
        f.art.Update();
        f.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                             modules::UiOwner::P1});
    };
    const auto inspect = [&](const bool effectClock, const bool erase) {
        const auto revision = f.art.Revision();
        const auto documentRevision = f.persistence.AdvancedGeneration();
        const auto history = f.art.UndoCount();
        auto packet = f.Request(1U);
        if (effectClock) V2Fixture::Put(packet, 1U, 1U);
        f.Ack(packet);
        const auto packets = Drain(f.art);
        const size_t toolIndex = effectClock ? 4U : 2U;
        Check(packets.size() > toolIndex &&
                  packets[0].bytes[2] == 0x88U && packets[1].bytes[2] == 0x89U,
              "fresh Inspect returns contiguous timeline metadata followed by shared Tool readback");
        if (packets.size() > toolIndex) expectTool(packets[toolIndex], erase, 0U);
        Check(std::count_if(packets.begin(), packets.end(), [](const auto& response) {
                  return response.bytes[0] == 0x4cU && response.bytes[1] == 0x82U;
              }) == 1U, "Inspect publishes exactly one Tool event before any pending document resync");
        Check(f.art.Revision() == revision &&
                  f.persistence.AdvancedGeneration() == documentRevision && f.art.UndoCount() == history,
              "Inspect tool publication is observational and adds no artwork mutation or history");
    };
    appTool(false);
    holdCircle();
    Check(f.art.IsErasing(), "physical Circle hold selects Erase after the app selected Draw");
    const auto physical = Drain(f.art);
    if (!physical.empty()) expectTool(physical.front(), true, 0U);
    Check(physical.size() == 2U, "physical Circle publishes Tool plus State exactly once");
    inspect(false, true);
    appTool(false);
    appTool(true);
    f.art.EndConnection();
    holdCircle();
    Check(!f.art.IsErasing(), "physical Circle hold remains authoritative while disconnected");
    f.art.BeginConnection(); ++f.epoch; f.next = 1U;
    inspect(true, false);
    for (const uint8_t probe : {2U, 3U}) {
        auto packet = f.Request(1U);
        V2Fixture::Put(packet, probe, 1U);
        f.Ack(packet);
        Check(Drain(f.art).empty(), "stream support probes remain State-only without tool publication");
    }
    std::puts("[V2_SHARED_TOOL] app-Draw/physical-Erase/Inspect/app-Erase/offline-Draw/reconnect-readback passed");
}

void CompactControlPreemptsLegacySnapshotTraffic() {
    const auto setup = [](Art& art) {
        art.Configure(3U, 1701U);
        art.BeginConnection();
        Send(art, Art::Op::Enter, 1U);
        Drain(art);
        Send(art, Art::Op::Snapshot, 2U, {1U});
        Art::BulkPacket packet;
        Check(art.PeekPacket(packet, 244U),
              "priority fixture exposes legacy snapshot admission");
        art.AcceptPacket();
        Check(art.PeekPacket(packet, 244U),
              "priority fixture exposes legacy snapshot begin");
        art.AcceptPacket();
        Check(art.PeekPacket(packet, 244U),
              "priority fixture exposes legacy snapshot tool");
        art.AcceptPacket();
        Check(art.PeekPacket(packet, 244U) &&
                  packet.bytes[1] ==
                      static_cast<uint8_t>(Art::Event::SnapshotChunk),
              "priority fixture reaches legacy snapshot chunks");
    };
    const auto expectReplyBeforeChunk = [&](const Art::Op op,
                                            const Art::Event event,
                                            const char* message) {
        Art art;
        setup(art);
        const auto control = op == Art::Op::Tool
            ? Request(art, op, 3U, {0U, 15U, 9U, 8U, 7U})
            : Request(art, op, 3U);
        art.HandlePacket(control.bytes.data(), control.size, true, 0U);
        Art::BulkPacket reply;
        bool found = false;
        for (uint8_t index = 0U; index < 3U && art.PeekPacket(reply, 244U);
             ++index) {
            Check(reply.bytes[1] !=
                      static_cast<uint8_t>(Art::Event::SnapshotChunk),
                  "control transaction stays contiguous ahead of snapshot chunks");
            if (reply.bytes[1] == static_cast<uint8_t>(event) &&
                (uint16_t(reply.bytes[10]) |
                 (uint16_t(reply.bytes[11]) << 8U)) == 3U) {
                found = true;
                break;
            }
            art.AcceptPacket();
        }
        Check(found, message);
    };
    expectReplyBeforeChunk(
        Art::Op::Inspect, Art::Event::State,
        "Inspect State reply preempts legacy snapshot chunks");
    expectReplyBeforeChunk(
        Art::Op::Tool, Art::Event::Tool,
        "Tool readback preempts legacy snapshot chunks");
    expectReplyBeforeChunk(
        Art::Op::Exit, Art::Event::State,
        "Exit acknowledgement preempts legacy snapshot chunks");
}

void CompactControlIngressPreemptsStreamChunks() {
    modules::BleEventQueue queue;
    Check(queue.Begin(), "priority ingress queue begins");
    for (uint8_t index = 0U; index < 12U; ++index) {
        uint8_t chunk[14] = {0x4dU, 2U, 15U};
        chunk[7] = static_cast<uint8_t>(index + 1U);
        modules::BleEvent event;
        Check(modules::MakeLitchcraftIngressEvent(
                  chunk, sizeof(chunk), 71U, event) &&
                  queue.Enqueue(event),
              "stream chunk queues before compact control");
    }
    Art art;
    art.Configure(3U, 1702U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    const auto exit = Request(art, Art::Op::Exit, 44U);
    modules::BleEvent control;
    Check(modules::MakeLitchcraftIngressEvent(
              exit.bytes.data(), exit.size, 71U, control) &&
              queue.Enqueue(control),
          "compact Exit ingress is admitted behind queued chunks");
    modules::BleEvent first;
    Check(queue.TryDequeue(first) &&
              first.type == modules::BleEventType::Litchcraft &&
              first.payload.litchcraft.len == exit.size &&
              first.payload.litchcraft.input[0] == Art::MAGIC &&
              first.payload.litchcraft.input[1] ==
                  static_cast<uint8_t>(Art::Op::Exit),
          "compact Exit ingress is never head-of-line behind stream chunks");
}

void EveryInteractiveIngressPreemptsBulk() {
    const auto expect = [](const uint8_t magic, const uint8_t op,
                           const char* const message) {
        modules::BleEventQueue queue;
        Check(queue.Begin(), "interactive priority queue begins");
        for (uint8_t index = 0U; index < 3U; ++index) {
            uint8_t chunk[14] = {0x4dU, 2U, 15U};
            chunk[7] = static_cast<uint8_t>(index + 1U);
            modules::BleEvent event;
            Check(modules::MakeLitchcraftIngressEvent(
                      chunk, sizeof(chunk), 81U, event) &&
                      queue.Enqueue(event),
                  "bulk fixture queues before interactive control");
        }
        uint8_t bytes[20] = {};
        bytes[0] = magic;
        bytes[1] = magic == 0x4dU ? 2U : op;
        bytes[2] = magic == 0x4dU ? op : 0U;
        const size_t size = magic == 0x4dU ? 9U : 8U;
        modules::BleEvent control;
        Check(modules::MakeLitchcraftIngressEvent(
                  bytes, size, 81U, control) && queue.Enqueue(control),
              "interactive control is admitted behind bulk");
        modules::BleEvent first;
        Check(queue.TryDequeue(first) &&
                  first.type == modules::BleEventType::Litchcraft &&
                  first.payload.litchcraft.input[0] == magic &&
                  first.payload.litchcraft.input[magic == 0x4dU ? 2U : 1U] == op,
              message);
    };
    for (const uint8_t op : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 12U, 14U}) {
        expect(0x4cU, op, "every legacy human control preempts bulk ingress");
    }
    for (const uint8_t op : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 12U, 13U}) {
        expect(0x4dU, op, "every v2 human control preempts bulk ingress");
    }

    modules::BleEventQueue queue;
    Check(queue.Begin(), "Setup priority queue begins");
    uint8_t chunk[14] = {0x4dU, 2U, 15U};
    modules::BleEvent bulk;
    Check(modules::MakeLitchcraftIngressEvent(
              chunk, sizeof(chunk), 81U, bulk) && queue.Enqueue(bulk),
          "Setup priority fixture queues bulk");
    modules::BleEvent setting;
    setting.type = modules::BleEventType::SettingWrite;
    Check(queue.Enqueue(setting), "Setup write is admitted behind bulk");
    modules::BleEvent first;
    Check(queue.TryDequeue(first) &&
              first.type == modules::BleEventType::SettingWrite,
          "Setup write preempts bulk ingress");
}

void OrdinaryControlReplyPreemptsQueuedCells() {
    Art art;
    art.Configure(3U, 1732U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    Send(art, Art::Op::Paint, 2U, {1U, 0U, 3U, 0U});
    Art::BulkPacket packet;
    Check(art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::State) &&
              (uint16_t(packet.bytes[10]) |
               (uint16_t(packet.bytes[11]) << 8U)) == 2U,
          "ordinary compact ACK preempts queued Cell traffic");
    art.AcceptPacket();
    Check(art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::Cell),
          "preempted Cell remains queued after compact ACK");
}

void V2ControlBundlesPreemptActiveSnapshotInOrder() {
    const auto beginSnapshot = [](V2Fixture& fixture) {
        const auto snapshot = fixture.Request(8U);
        fixture.Ack(snapshot);
        const auto begin = fixture.Pop(244U);
        Check(begin.bytes[2] == 0x84U,
              "v2 priority fixture reaches snapshot Begin");
    };
    const auto expectEvent = [](const Art::BulkPacket& packet,
                                const uint8_t event,
                                const char* const message) {
        Check(packet.bytes[0] == (event == 0x82U ? 0x4cU : 0x4dU) &&
                  packet.bytes[event == 0x82U ? 1U : 2U] == event,
              message);
    };

    {
        V2Fixture fixture;
        beginSnapshot(fixture);
        auto timeline = fixture.Request(2U);
        V2Fixture::Put(timeline, 0U, 1U);
        V2Fixture::Put(timeline, 0U, 1U);
        fixture.Ingress(timeline);
        expectEvent(fixture.Pop(), 0x81U,
                    "Timeline State preempts active snapshot chunks");
        expectEvent(fixture.Pop(), 0x88U,
                    "Timeline low half stays contiguous after State");
        expectEvent(fixture.Pop(), 0x89U,
                    "Timeline high half stays contiguous after low half");
        expectEvent(fixture.Pop(), 0x85U,
                    "snapshot resumes only after complete Timeline bundle");
    }

    {
        V2Fixture fixture;
        beginSnapshot(fixture);
        const auto inspect = fixture.Request(1U);
        fixture.Ingress(inspect);
        expectEvent(fixture.Pop(), 0x81U,
                    "Inspect State preempts active snapshot chunks");
        expectEvent(fixture.Pop(), 0x88U,
                    "Inspect timeline low stays behind its State");
        expectEvent(fixture.Pop(), 0x89U,
                    "Inspect timeline high stays behind its low half");
        expectEvent(fixture.Pop(), 0x82U,
                    "Inspect Tool stays behind its complete timeline");
        expectEvent(fixture.Pop(), 0x85U,
                    "snapshot resumes only after complete Inspect bundle");
    }
}

void QueuedToolReplyPreemptsCellAndSnapshotBulk() {
    Art art;
    art.Configure(3U, 1730U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    Send(art, Art::Op::Snapshot, 2U, {1U});
    Art::BulkPacket packet;
    for (uint8_t index = 0U; index < 3U; ++index) {
        Check(art.PeekPacket(packet, 244U),
              "deep-priority fixture advances snapshot header");
        art.AcceptPacket();
    }
    Send(art, Art::Op::Paint, 3U, {1U, 0U, 3U, 0U});
    Check(art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::State),
          "paint ACK itself preempts snapshot bulk");
    art.AcceptPacket();
    Send(art, Art::Op::Tool, 4U, {1U, 5U, 7U, 8U, 9U});
    Check(art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::Tool) &&
              (uint16_t(packet.bytes[10]) |
               (uint16_t(packet.bytes[11]) << 8U)) == 4U &&
              packet.bytes[15] == 1U && packet.bytes[16] == 5U,
          "Tool reply behind queued Cell still preempts snapshot/bulk traffic");
}

void DuplicateToolReplayPublishesCurrentAuthority() {
    Art art;
    art.Configure(3U, 1731U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    const auto draw = Request(
        art, Art::Op::Tool, 2U, {0U, 5U, 0x12U, 0x34U, 0x56U});
    art.HandlePacket(draw.bytes.data(), draw.size, true, 0U);
    Drain(art);
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                       modules::UiOwner::P1});
    fxe::ManualMillis::AdvanceMs(1000U);
    art.Update();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                       modules::UiOwner::P1});
    Drain(art);
    const uint32_t revision = art.Revision();
    const uint8_t undo = art.UndoCount();
    art.HandlePacket(draw.bytes.data(), draw.size, true, 0U);
    const auto replay = Drain(art);
    Check(art.IsErasing() && art.Revision() == revision &&
              art.UndoCount() == undo &&
              std::any_of(replay.begin(), replay.end(), [](const auto& reply) {
                  return reply.bytes[1] ==
                             static_cast<uint8_t>(Art::Event::Tool) &&
                      (uint16_t(reply.bytes[10]) |
                       (uint16_t(reply.bytes[11]) << 8U)) == 2U &&
                      reply.bytes[12] == 0x12U && reply.bytes[13] == 0x34U &&
                      reply.bytes[14] == 0x56U && reply.bytes[15] == 1U &&
                      reply.bytes[16] == 5U;
              }),
          "duplicate successful Tool is observational and republishes current physical Erase authority");
}

void PhysicalBrushAuthorityPreemptsBulkAndFencesSessions() {
    Art art;
    art.Configure(3U, 1710U);
    art.BeginConnection();
    Send(art, Art::Op::Enter, 1U);
    Drain(art);
    Send(art, Art::Op::Snapshot, 2U, {1U});
    Art::BulkPacket packet;
    for (uint8_t index = 0U; index < 3U; ++index) {
        Check(art.PeekPacket(packet, 244U),
              "physical brush priority fixture advances snapshot header");
        art.AcceptPacket();
    }
    Check(art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::SnapshotChunk),
          "physical brush priority fixture reaches snapshot bulk");
    const uint32_t session = art.SessionId();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                       modules::UiOwner::P1});
    fxe::ManualMillis::AdvanceMs(1000U);
    const uint32_t before = fxe::Millis();
    art.Update();
    art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                       modules::UiOwner::P1});
    Check(fxe::Millis() - before < 50U && art.PeekPacket(packet, 244U) &&
              packet.bytes[1] == static_cast<uint8_t>(Art::Event::Tool) &&
              V2Fixture::Read(packet.bytes.data() + 2U, 4U) == session &&
              packet.bytes[15] == 1U,
          "physical Circle hold publishes session-safe Erase before snapshot bulk in-cycle");

    Art fenced;
    fenced.Configure(3U, 1711U);
    fenced.BeginConnection();
    Send(fenced, Art::Op::Enter, 1U);
    Drain(fenced);
    fenced.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                          modules::UiOwner::P1});
    fxe::ManualMillis::AdvanceMs(1000U);
    fenced.Update();
    fenced.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                          modules::UiOwner::P1});
    const uint32_t retiredSession = fenced.SessionId();
    Send(fenced, Art::Op::Exit, 2U);
    Send(fenced, Art::Op::Enter, 3U);
    const uint32_t replacementSession = fenced.SessionId();
    Send(fenced, Art::Op::Tool, 4U, {0U, 5U, 0x12U, 0x34U, 0x56U});
    const auto replies = Drain(fenced);
    Check(replacementSession != retiredSession &&
              std::none_of(replies.begin(), replies.end(), [&](const auto& reply) {
                  return reply.bytes[1] == static_cast<uint8_t>(Art::Event::Tool) &&
                      V2Fixture::Read(reply.bytes.data() + 2U, 4U) == retiredSession;
              }) &&
              std::any_of(replies.begin(), replies.end(), [&](const auto& reply) {
                  return reply.bytes[1] == static_cast<uint8_t>(Art::Event::Tool) &&
                      V2Fixture::Read(reply.bytes.data() + 2U, 4U) == replacementSession &&
                      (uint16_t(reply.bytes[10]) |
                       (uint16_t(reply.bytes[11]) << 8U)) == 4U &&
                      reply.bytes[12] == 0x12U && reply.bytes[15] == 0U;
              }),
          "new-session app Tool cannot be overwritten by a late retired-session Brush event");
}

void IdempotentEnterRetiresPriorV2Transfer() {
    V2Fixture f;
    const auto snapshot = f.Request(8U);
    f.Ingress(snapshot);
    Check(f.persistence.AdvancedTransportStorage() != nullptr,
          "v2 snapshot owns the advanced transport lease before Enter");
    const uint32_t session = f.art.SessionId();
    const uint32_t before = fxe::Millis();
    Send(f.art, Art::Op::Enter, 500U);
    Check(fxe::Millis() - before < 50U && f.art.IsActive() &&
              f.art.SessionId() == session &&
              f.persistence.AdvancedTransportStorage() == nullptr,
          "idempotent Enter keeps authority and retires prior v2 transfer in-cycle");
}

void FreshEnterRetiresDiscardedV2PendingResponse() {
    V2Fixture f(V2Fixture::Authority::Advanced, false);
    const auto inactiveInspect = f.Request(1U);
    f.Ingress(inactiveInspect);
    Send(f.art, Art::Op::Enter, 3U);
    Drain(f.art);
    Check(f.art.IsActive(),
          "fresh Enter replaces inactive Inspect response authority");
    f.next = 4U;
    const auto laterInspect = f.Request(1U);
    const auto response = f.Ack(laterInspect);
    Check(response.bytes[13] == static_cast<uint8_t>(Art::Status::Ok),
          "later v2 request is not permanently Busy after fresh Enter discarded an inactive response");
}

void V2EffectClockOptIn() {
    V2Fixture f;
    const auto inspect = [&]() {
        auto packet = f.Request(1U);
        V2Fixture::Put(packet, 1U, 1U);
        f.Ack(packet);
    };
    fxe::ManualMillis::SetNowMs(1234U); f.art.Update();
    inspect();
    const auto frameLow = f.Pop(20U); const auto frameHigh = f.Pop(20U);
    const auto effectLow = f.Pop(20U); const auto effectHigh = f.Pop(20U);
    Check(frameLow.bytes[2] == 0x88U && frameHigh.bytes[2] == 0x89U &&
              effectLow.bytes[2] == 0x8bU && effectHigh.bytes[2] == 0x8cU &&
              effectLow.size == 20U && effectHigh.size == 20U &&
              effectLow.bytes[15] == 0U && effectHigh.bytes[15] == 0U &&
              V2Fixture::Read(effectLow.bytes.data() + 13U, 2U) ==
                  V2Fixture::Read(frameLow.bytes.data() + 13U, 2U) &&
              V2Fixture::Read(effectHigh.bytes.data() + 13U, 2U) ==
                  V2Fixture::Read(frameLow.bytes.data() + 13U, 2U) &&
              V2Fixture::Read(effectLow.bytes.data() + 16U, 4U) == 1234U &&
              V2Fixture::Read(effectHigh.bytes.data() + 16U, 4U) == 0U,
          "opt-in Inspect returns one bounded frozen frame/effect clock sample while frames are paused");
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 10U); f.art.Update();
    fxe::ManualMillis::SetNowMs(9U); f.art.Update();
    inspect(); f.Pop(); f.Pop();
    const auto wrappedLow = f.Pop(20U);
    fxe::ManualMillis::SetNowMs(509U); f.art.Update();
    const auto wrappedHigh = f.Pop(20U);
    Check(V2Fixture::Read(wrappedLow.bytes.data() + 16U, 4U) == 9U &&
              V2Fixture::Read(wrappedHigh.bytes.data() + 16U, 4U) == 1U &&
              V2Fixture::Read(wrappedLow.bytes.data() + 13U, 2U) ==
                  V2Fixture::Read(wrappedHigh.bytes.data() + 13U, 2U),
          "effect-clock pair preserves elapsed64 through millis wrap and delayed delivery");
    f.art.EndConnection(); f.art.BeginConnection(); f.next = 1U;
    f.Ack(f.Request(1U)); f.Pop(); f.Pop();
    Art::BulkPacket remainder;
    bool unexpectedEffect = false;
    while (f.art.PeekPacket(remainder, 20U)) {
        unexpectedEffect |= remainder.bytes[2] == 0x8bU || remainder.bytes[2] == 0x8cU;
        f.art.AcceptPacket();
    }
    Check(!unexpectedEffect, "new legacy connection receives no unrequested effect-clock extension");
}

void V2FrameOperationsAndHistoryBounds() {
    V2Fixture f;
    f.Ack(V2FrameEdit(f, 0U, 0U), Art::Status::ResourceLimit);
    for (uint8_t count = 8U; count > 1U; --count) {
        const auto ack = f.Ack(V2FrameEdit(f, 1U, count - 1U));
        Check(ack.bytes[15] == count - 1U && ack.bytes[16] == count - 2U, "Delete selects preceding remaining frame");
    }
    f.Ack(V2FrameEdit(f, 1U, 0U), Art::Status::ResourceLimit);
    auto add = V2FrameEdit(f, 0U, 0U);
    const auto addAck = f.Ack(add);
    Check(addAck.bytes[15] == 2U && addAck.bytes[16] == 1U, "Add-copy selects inserted complete frame");
    Check(f.persistence.AdvancedDocument().Value().frames[1].emitters[0].color.r ==
              f.persistence.AdvancedDocument().Value().frames[0].emitters[0].color.r, "Add copies full source frame");
    Check(f.Ack(add).bytes == addAck.bytes, "Add retry does not add another frame");
    f.Ack(V2FrameEdit(f, 0U, 0U), Art::Status::Malformed);
    f.Ack(V2FrameEdit(f, 3U, 0U, 0U, 50U));
    f.Ack(V2FrameEdit(f, 3U, 1U, 0U, 137U));
    f.Ack(V2FrameEdit(f, 3U, 1U, 0U, 5000U));
    for (auto duration : {0U, 49U, 5001U}) f.Ack(V2FrameEdit(f, 3U, 0U, 0U, duration), Art::Status::Malformed);
    f.Ack(V2FrameEdit(f, 1U, 0U, 1U), Art::Status::Malformed);
    f.Ack(V2FrameEdit(f, 2U, 0U, 1U, 100U), Art::Status::Malformed);
    const auto move = f.Ack(V2FrameEdit(f, 2U, 1U, 0U));
    Check(move.bytes[16] == 0U && f.persistence.AdvancedDocument().Value().frames[0].durationMs == 5000U,
          "Move removes then inserts and follows selected frame identity");
    auto revision = f.persistence.AdvancedGeneration();
    f.Ack(V2FrameEdit(f, 2U, 0U, 0U));
    f.Ack(V2FrameEdit(f, 3U, 0U, 0U, 5000U));
    Check(f.persistence.AdvancedGeneration() == revision, "same position/duration operations do not revise document");
    auto stale = V2FrameEdit(f, 3U, 0U, 0U, 900U);
    stale.bytes[9] ^= 1U;
    f.Ack(stale, Art::Status::RevisionConflict);
    const auto beforeCapacityFill = f.persistence.AdvancedDocument();
    for (uint8_t i = 1U; i <= modules::LitchcraftHistory::OP_CAPACITY; ++i) {
        f.Ack(V2Edit(f, 3U, {0, i, 0, 3, 1,
                            static_cast<uint8_t>(100U + i), 0, 0, 0}));
    }
    Check(f.art.UndoCount() == modules::LitchcraftHistory::OP_CAPACITY,
          "small operation history retains the full bounded operation capacity");
    for (size_t i = 0U; i < modules::LitchcraftHistory::OP_CAPACITY; ++i) {
        const auto ack = f.Ack(f.Request(6U));
        Check((ack.bytes[14] & 12U) ==
                  (i + 1U == modules::LitchcraftHistory::OP_CAPACITY ? 8U : 12U),
              "State tracks remaining Undo and Redo after oldest-operation eviction");
    }
    Check(f.art.UndoCount() == 0U &&
              f.art.RedoCount() == modules::LitchcraftHistory::OP_CAPACITY &&
              modules::litchcraft_animation::DocumentsEqual(
                  f.persistence.AdvancedDocument().Value(),
                  beforeCapacityFill.Value()),
          "bounded undos reach the retained oldest boundary without losing the latest undo");
    revision = f.persistence.AdvancedGeneration();
    Check((f.Ack(f.Request(6U), Art::Status::NoHistory).bytes[14] & 12U) == 8U,
          "No-history refusal still exposes authoritative Redo availability");
    Check(f.persistence.AdvancedGeneration() == revision, "No history does not revise document");
    f.Ack(f.Request(7U));
    f.Ack(V2Edit(f, 3U, {0, 31, 0, 3, 1, 100, 0, 0, 0}));
    Check(f.art.RedoCount() == 0U, "new accepted document operation retires redo branch");
    f.Ack(f.Request(7U), Art::Status::NoHistory);
    std::puts("[V2_EDITS] frame bounds/copy/delete/move/duration and 64-operation eviction passed");
}

void V2TimelineWrapFrozenPairsAndConnectionLifetime() {
    V2Fixture f;
    const auto generation = f.persistence.AdvancedGeneration();
    const auto canvasGeneration = f.art.CanvasGeneration();
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 10U);
    f.art.Update();
    f.Ack(V2Edit(f, 2U, {1, 0}));
    auto low = f.Pop(); auto high = f.Pop();
    Check(low.bytes[2] == 0x88U && high.bytes[2] == 0x89U && low.bytes[15] == 1U &&
              V2Fixture::Read(low.bytes.data() + 13U, 2U) != 0U &&
              V2Fixture::Read(low.bytes.data() + 13U, 2U) == V2Fixture::Read(high.bytes.data() + 13U, 2U),
          "Play returns one nonzero paired frozen timeline sample");
    fxe::ManualMillis::SetNowMs(9U); f.art.Update();
    f.Ack(f.Request(1U));
    low = f.Pop();
    fxe::ManualMillis::SetNowMs(109U); f.art.Update();
    high = f.Pop();
    Check(V2Fixture::Read(low.bytes.data() + 16U, 4U) == 20U &&
              V2Fixture::Read(high.bytes.data() + 16U, 4U) == 0U &&
              V2Fixture::Read(low.bytes.data() + 13U, 2U) == V2Fixture::Read(high.bytes.data() + 13U, 2U),
          "uint32 clock wrap advances elapsed and delayed high half stays frozen");
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 100U); f.art.Update();
    fxe::ManualMillis::SetNowMs(1000U); f.art.Update();
    auto inspect = f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(V2Fixture::Read(high.bytes.data() + 16U, 4U) == 1U, "accumulated elapsed preserves high 32 bits across repeated clock wraps");
    f.Ack(V2Edit(f, 2U, {0, inspect.bytes[16]}));
    low = f.Pop(); high = f.Pop();
    const auto frozenLow = V2Fixture::Read(low.bytes.data() + 16U, 4U);
    fxe::ManualMillis::SetNowMs(3000U); f.art.Update();
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(V2Fixture::Read(low.bytes.data() + 16U, 4U) == frozenLow && low.bytes[15] == 0U,
          "pause freezes elapsed independently of subsequent input/render cadence");
    f.Ack(V2Edit(f, 2U, {2, 3})); low = f.Pop(); high = f.Pop();
    uint64_t expected = 0U;
    modules::litchcraft_animation::TryFrameStartElapsed(f.persistence.AdvancedDocument(), 3U, expected);
    Check(V2Fixture::Read(low.bytes.data() + 16U, 4U) == expected && high.bytes[15] == 3U,
          "select-frame seeks exactly to cumulative frame start");
    f.Ack(V2Edit(f, 2U, {1, 3})); f.Pop(); f.Pop();
    f.Ack(f.Request(1U)); low = f.Pop();
    f.art.EndConnection(); f.art.BeginConnection(); f.next = 1U;
    fxe::ManualMillis::SetNowMs(3500U); f.art.Update();
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(low.bytes[2] == 0x88U && high.bytes[2] == 0x89U && low.bytes[15] == 1U &&
              V2Fixture::Read(low.bytes.data() + 16U, 4U) == expected + 500U,
          "disconnect drops missing pending half; reconnect gets fresh coherent pair while playback continues");
    const auto staleFrame = high.bytes[15];
    fxe::ManualMillis::SetNowMs(5000U); f.art.Update();
    Check(f.art.SelectedFrame() != staleFrame,
          "delivery delay actually advances beyond the app-visible Pause frame");
    auto pauseAcrossBoundary = f.Ack(V2Edit(f, 2U, {0, staleFrame}));
    low = f.Pop(); high = f.Pop();
    uint64_t namedFrameStart = 0U;
    Check(modules::litchcraft_animation::TryFrameStartElapsed(
              f.persistence.AdvancedDocument(), staleFrame, namedFrameStart),
          "tap-visible Pause names a valid frame after a transport boundary");
    Check((pauseAcrossBoundary.bytes[14] & 2U) == 0U && low.bytes[15] == 0U &&
              high.bytes[15] == staleFrame &&
              V2Fixture::Read(low.bytes.data() + 16U, 4U) ==
                  static_cast<uint32_t>(namedFrameStart) &&
              V2Fixture::Read(high.bytes.data() + 16U, 4U) ==
                  static_cast<uint32_t>(namedFrameStart >> 32U),
          "Pause freezes the named tap-visible frame after delivery crosses a boundary");
    Check(V2Fixture::Read(low.bytes.data() + 13U, 2U) != 0U &&
              V2Fixture::Read(low.bytes.data() + 13U, 2U) ==
                  V2Fixture::Read(high.bytes.data() + 13U, 2U),
          "named Pause returns one coherent nonzero paired sample");
    fxe::ManualMillis::SetNowMs(6500U); f.art.Update();
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(low.bytes[15] == 0U && high.bytes[15] == staleFrame &&
              V2Fixture::Read(low.bytes.data() + 16U, 4U) ==
                  static_cast<uint32_t>(namedFrameStart) &&
              V2Fixture::Read(high.bytes.data() + 16U, 4U) ==
                  static_cast<uint32_t>(namedFrameStart >> 32U),
          "named frame and elapsed stay frozen through a later render/input interval");
    Check(f.persistence.AdvancedGeneration() == generation && f.art.CanvasGeneration() == canvasGeneration &&
              f.art.UndoCount() == 0U, "play/pause/seek/ticks/reconnect never dirty document generations or history");
    std::puts("[V2_TIMELINE] paired samples/uint32 wrap/uint64 elapsed/pause/seek/reconnect passed");
}

void V2PhysicalEditsConflictWithStagedReplacement() {
    V2Fixture f;
    auto bytes = f.Encoded();
    const auto begin = f.Begin(bytes);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Ack(begin); f.Upload(bytes, transfer, 244U);
    f.Ack(V2Edit(f, 2U, {1, 0})); f.Pop(); f.Pop();
    f.art.ObserveOccupancy(UINT64_C(1) << 9U);
    auto revision = f.persistence.AdvancedGeneration();
    Check(revision == 2U && f.persistence.AdvancedTransportStorage() != nullptr &&
              f.persistence.AdvancedDocument().Value().frames[0].emitters[36].color.r == f.art.Color().r,
          "real Hall seam paints sole authority while replacement remains staged");
    Drain(f.art);
    f.Ack(V2Edit(f, 2U, {2, 1}), Art::Status::Busy);
    f.art.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(350U); f.art.Update(); Drain(f.art);
    f.Ack(f.TransferOp(11U, transfer), Art::Status::RevisionConflict);
    Check(f.persistence.AdvancedGeneration() == revision && f.persistence.AdvancedTransportStorage() == nullptr,
          "staged commit conflict retires bytes and preserves unseen physical work");
    f.Ack(f.Request(6U));
    Check(f.persistence.AdvancedGeneration() == ++revision && f.art.RedoCount() == 1U,
          "app Undo reaches physical history operation");
    f.art.ObserveButton({modules::UserButton::Press, fxe::Button::Event::Press, modules::UiOwner::P1});
    f.art.ObserveButton({modules::UserButton::Press, fxe::Button::Event::Release, modules::UiOwner::P1});
    Check(f.persistence.AdvancedGeneration() == revision &&
              f.art.RedoCount() == 1U,
          "unused physical center cannot consume app Redo history");
    f.Ack(f.Request(7U));
    Check(f.persistence.AdvancedGeneration() == ++revision &&
              f.art.RedoCount() == 0U,
          "app Redo reaches the shared document history");
    Drain(f.art);
    f.Ack(V2Edit(f, 3U, {0, 8, 0, 1, 1, 11, 22, 33, 0}));
    f.art.EndConnection(); f.art.BeginConnection(); f.next = 1U;
    f.Ack(f.Request(6U));
    Check(f.art.RedoCount() == 1U, "disconnect closes app stroke retaining accepted edits as one Undo");
    f.art.ObserveOccupancy(UINT64_C(1) << 10U); Drain(f.art);
    const auto beforeTool = f.persistence.AdvancedDocument();
    revision = f.persistence.AdvancedGeneration();
    const auto history = f.art.UndoCount();
    f.art.ObserveButton({modules::UserButton::Up, fxe::Button::Event::Press, modules::UiOwner::P1});
    Check(f.persistence.AdvancedGeneration() == revision + 1U && f.art.UndoCount() == history + 1U &&
              f.persistence.AdvancedDocument().Value().favorites[0].r == f.art.RecentColors()[0].r &&
              f.art.PaletteGeneration() == 0U,
          "physical favorite adjustment and held footprint are one advanced revision/history operation");
    Drain(f.art); f.Ack(f.Request(6U));
    Check(modules::litchcraft_animation::DocumentsEqual(beforeTool.Value(), f.persistence.AdvancedDocument().Value()),
          "Undo physical tool change restores both full favorite list and held RGB footprint");
    std::puts("[V2_PHYSICAL] production Hall/app/shared-history/staging conflict/interrupted-stroke passed");
}

void V2SameFrameAppStrokeCrossesPhysicalBrush() {
    constexpr uint8_t BRUSH = 9U;
    constexpr uint8_t APP_ONLY = 10U;
    constexpr uint8_t NEXT_BRUSH = 11U;
    V2Fixture held;
    const auto initial = held.persistence.AdvancedDocument();
    held.art.ObserveOccupancy(UINT64_C(1) << BRUSH);
    Drain(held.art);
    Check(held.art.UndoCount() == 1U,
          "held physical brush creates its first history operation before app ingress");
    // A frame mismatch, selection, and structural edit all leave the held
    // physical frame latched. The following same-frame stroke must cross it.
    held.Ack(V2Edit(held, 3U, {1U, 41U, 0U, 3U, 1U, 9U, 8U, 7U, APP_ONLY}),
             Art::Status::Busy);
    held.Ack(V2Edit(held, 2U, {2U, 1U}), Art::Status::Busy);
    held.Ack(V2FrameEdit(held, 0U, 0U), Art::Status::Busy);
    Check(held.art.SelectedFrame() == 0U &&
              held.persistence.AdvancedDocument().Value().frames[1].emitters[0].color.r ==
                  initial.Value().frames[1].emitters[0].color.r,
          "held physical brush rejects frame movement without changing another frame");

    const auto beforeApp = held.persistence.AdvancedGeneration();
    held.Ack(V2Edit(held, 3U, {0U, 41U, 0U, 1U, 1U, 9U, 8U, 7U, APP_ONLY}));
    held.Ack(V2Edit(held, 4U, {0U, 2U, 164U, 6U, 1U, 41U, 0U, 2U, BRUSH}));
    const auto& appEdited = held.persistence.AdvancedDocument().Value().frames[0];
    Check(held.persistence.AdvancedGeneration() == beforeApp + 2U &&
              appEdited.emitters[APP_ONLY * 4U].color.r == 9U &&
              appEdited.emitters[BRUSH * 4U].effect ==
                  modules::litchcraft_animation::Effect::Flash &&
              appEdited.emitters[BRUSH * 4U].durationMs == 1700U &&
              held.art.UndoCount() == 2U,
          "same-frame app Paint and Effect end the held brush and share one app Undo operation");

    const auto afterApp = held.persistence.AdvancedGeneration();
    held.art.ObserveOccupancy(UINT64_C(1) << BRUSH);
    Check(held.persistence.AdvancedGeneration() == afterApp,
          "app stroke does not repaint a physical brush that never moved");
    held.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                            modules::UiOwner::P1});
    fxe::ManualMillis::AdvanceMs(1000U);
    held.art.Update();
    held.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                            modules::UiOwner::P1});
    const auto& physicallyErased = held.persistence.AdvancedDocument().Value().frames[0].emitters[BRUSH * 4U];
    Check(held.art.IsErasing() && physicallyErased.color.r == 0U &&
              physicallyErased.color.g == 0U && physicallyErased.color.b == 0U &&
              physicallyErased.effect == modules::litchcraft_animation::Effect::Off &&
              physicallyErased.durationMs == 0U,
          "next physical erase accepts the crossed brush and removes its app effect metadata");
    held.art.ObserveOccupancy(UINT64_C(1) << NEXT_BRUSH);
    held.art.ObserveOccupancy(0U);
    fxe::ManualMillis::AdvanceMs(Art::PHYSICAL_RELEASE_MS);
    held.art.Update();
    Check(held.art.UndoCount() == 3U,
          "app-to-physical continuation is one new physical history boundary");
    Drain(held.art);
    held.Ack(held.Request(6U));
    const auto& beforePhysicalContinuation = held.persistence.AdvancedDocument().Value().frames[0];
    Check(beforePhysicalContinuation.emitters[BRUSH * 4U].effect ==
              modules::litchcraft_animation::Effect::Flash &&
              beforePhysicalContinuation.emitters[NEXT_BRUSH * 4U].color.r ==
                  initial.Value().frames[0].emitters[NEXT_BRUSH * 4U].color.r,
          "Undo restores the app stroke after one physical continuation operation");
    held.Ack(held.Request(6U));
    const auto& beforeAppStroke = held.persistence.AdvancedDocument().Value().frames[0];
    Check(beforeAppStroke.emitters[BRUSH * 4U].effect ==
              initial.Value().frames[0].emitters[BRUSH * 4U].effect &&
              beforeAppStroke.emitters[APP_ONLY * 4U].color.r ==
                  initial.Value().frames[0].emitters[APP_ONLY * 4U].color.r,
          "second Undo restores the original physical brush without consuming its operation");
    held.Ack(held.Request(6U));
    Check(modules::litchcraft_animation::DocumentsEqual(
              held.persistence.AdvancedDocument().Value(), initial.Value()),
          "third Undo restores the exact pre-brush document");

    V2Fixture gap;
    gap.art.ObserveOccupancy(UINT64_C(1) << BRUSH);
    gap.art.ObserveOccupancy(0U);
    Drain(gap.art);
    fxe::ManualMillis::AdvanceMs(Art::PHYSICAL_RELEASE_MS - 1U);
    gap.Ack(V2Edit(gap, 3U, {0U, 77U, 0U, 3U, 1U, 31U, 32U, 33U, APP_ONLY}));
    const auto gapHistory = gap.art.UndoCount();
    fxe::ManualMillis::AdvanceMs(1U);
    gap.art.Update();
    Check(gapHistory == 2U && gap.art.UndoCount() == gapHistory,
          "same-frame app stroke within the 350 ms release group closes the physical stroke once");
    std::puts("[V2_PHYSICAL_APP_CROSS] held/gap/same-frame/effect/erase/Undo/frame-latch passed");
}

void V2PhysicalEraseClearsEffectMetadata() {
    V2Fixture f;
    f.Ack(V2Edit(f, 4U, {0, 2, 164, 6, 1, 1, 0, 3, 9}));
    const auto history = f.art.UndoCount();
    f.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press,
                         modules::UiOwner::P1});
    fxe::ManualMillis::AdvanceMs(1000U);
    f.art.Update();
    f.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release,
                         modules::UiOwner::P1});
    Check(f.art.IsErasing(), "physical O hold selects Erase before stamping");
    Drain(f.art);
    f.art.ObserveOccupancy(UINT64_C(1) << 9U);
    const auto& erased = f.persistence.AdvancedDocument().Value().frames[0].emitters[36];
    Check(erased.color.r == 0U && erased.color.g == 0U && erased.color.b == 0U &&
              erased.effect == modules::litchcraft_animation::Effect::Off && erased.durationMs == 0U,
          "physical Erase clears selected emitter RGB and effect metadata immediately");
    f.art.ObserveOccupancy(0U);
    fxe::ManualMillis::SetNowMs(fxe::Millis() + Art::PHYSICAL_RELEASE_MS);
    f.art.Update();
    Check(f.art.UndoCount() == history + 1U,
          "physical effect erase closes as one shared artwork history operation");
    Drain(f.art);
    f.Ack(f.Request(6U));
    const auto& restored = f.persistence.AdvancedDocument().Value().frames[0].emitters[36];
    Check(restored.effect == modules::litchcraft_animation::Effect::Flash && restored.durationMs == 1700U,
          "one app Undo restores a physical effect erase");
    std::puts("[V2_PHYSICAL_ERASE] effect-metadata/one-undo passed");
}

void V2DenseHistoryByteEvictionAndNetZeroStroke() {
    using namespace modules::litchcraft_animation;
    modules::LitchcraftHistory typicalHistory;
    Document typical = AdvancedDocument(8U, 1U).Value();
    const Document typicalInitial = typical;
    for (uint8_t step = 1U;
         step <= modules::LitchcraftHistory::MIN_TYPICAL_UNDO_STEPS; ++step) {
        Document before = typical;
        auto& emitter = typical.frames[(step - 1U) % 8U]
                            .emitters[(step * 13U) % EMITTER_COUNT];
        emitter.color = {step, static_cast<uint8_t>(step + 1U),
                         static_cast<uint8_t>(step + 2U)};
        Check(typicalHistory.Record(before, typical, 0U, 0U, false),
              "typical semantic edit records one delta operation");
    }
    const Document typicalFinal = typical;
    Check(typicalHistory.UndoCount() ==
              modules::LitchcraftHistory::MIN_TYPICAL_UNDO_STEPS &&
              typicalHistory.BytesUsed() < modules::LitchcraftHistory::BYTE_CAPACITY,
          "bounded delta arena guarantees at least 50 typical edit steps");
    uint8_t selected = 0U;
    for (size_t step = 0U;
         step < modules::LitchcraftHistory::MIN_TYPICAL_UNDO_STEPS; ++step) {
        Check(typicalHistory.Undo(typical, selected),
              "every guaranteed typical edit is undoable");
    }
    Check(DocumentsEqual(typical, typicalInitial),
          "50 typical undos restore the exact starting document");
    for (size_t step = 0U;
         step < modules::LitchcraftHistory::MIN_TYPICAL_UNDO_STEPS; ++step) {
        Check(typicalHistory.Redo(typical, selected),
              "every guaranteed typical edit is redoable");
    }
    Check(DocumentsEqual(typical, typicalFinal),
          "50 typical redos restore the exact edited document");

    modules::LitchcraftHistory history;
    Document before = AdvancedDocument(8U, 1U).Value();
    Document after = before;
    // Worst sparse-run fragmentation: one changed byte, one unchanged byte.
    // This exceeds the arena with the old run-only encoding, even though a
    // dense reversible XOR fits. Raw storage is intentional supporting proof
    // of the bound for every possible layout, including padding bytes.
    auto* fragmented = reinterpret_cast<uint8_t*>(&after);
    for (size_t i = 0U; i < sizeof(after); i += 2U) fragmented[i] ^= 0x01U;
    Check(history.Record(before, after, 0U, 0U, false),
          "worst-case fragmented document retains its newest Undo in fixed storage");
    auto recovered = after;
    uint8_t recoveredFrame = 0U;
    Check(history.Undo(recovered, recoveredFrame) &&
              std::memcmp(&recovered, &before, sizeof(before)) == 0,
          "dense fallback restores every original byte");
    Check(history.Redo(recovered, recoveredFrame) &&
              std::memcmp(&recovered, &after, sizeof(after)) == 0,
          "dense fallback Redo restores every changed byte");
    after = before;
    for (uint8_t step = 1U; step <= 12U; ++step) {
        before = after;
        for (auto& frame : after.frames) for (auto& emitter : frame.emitters) {
            emitter.color = {step, uint8_t(step + 1U), uint8_t(step + 2U)};
            emitter.effect = static_cast<Effect>(step % 8U + 1U);
            emitter.durationMs = 500U + step * 100U;
        }
        Check(history.Record(before, after, 0U, 0U, false), "dense accepted operation retains its own undo");
        Check(history.BytesUsed() <= modules::LitchcraftHistory::BYTE_CAPACITY && history.UndoCount() <= 2U,
              "dense operations evict complete oldest deltas within byte budget");
        auto verify = after;
        uint8_t frame = 0U;
        Check(history.Undo(verify, frame) && DocumentsEqual(verify, before), "byte eviction never loses current operation undo");
        Check(history.Redo(verify, frame) && DocumentsEqual(verify, after), "dense redo exactly restores every effect/RGB emitter");
    }
    V2Fixture f;
    f.Ack(V2Edit(f, 3U, {0, 1, 0, 3, 1, 7, 8, 9, 1}));
    const auto count = f.art.UndoCount();
    f.Ack(V2Edit(f, 3U, {0, 2, 0, 1, 1, 90, 80, 70, 2}));
    f.Ack(V2Edit(f, 3U, {0, 2, 0, 0, 1, 0, 0, 0, 2}));
    Check(f.art.UndoCount() == count, "stroke returning to original RGB retires only its own empty history operation");
    f.Ack(V2Edit(f, 3U, {0, 2, 0, 2, 1, 1, 2, 3, 3}));
    Check(f.art.UndoCount() == count + 1U, "continued net-zero stroke cannot absorb preceding operation");
    f.Ack(f.Request(6U));
    Check(f.persistence.AdvancedDocument().Value().frames[0].emitters[4].color.r == 7U &&
              f.persistence.AdvancedDocument().Value().frames[0].emitters[12].color.r == 0U,
          "undo after net-zero continuation preserves preceding independent stroke");
    std::puts("[V2_HISTORY] dense byte eviction/latest own undo/full redo/net-zero continuation passed");
}

void V2DocumentMutationsAloneDirtyPersistence() {
    V2Fixture f;
    f.persistence.RequestFlush();
    for (size_t i = 0U; i < 3U && f.persistence.FlushState() != Persistence::FlushStatus::Saved; ++i) {
        f.persistence.Update();
        if (f.sink.retained != 0U) f.sink.CompleteSuccess();
        f.persistence.Update();
    }
    Check(!f.persistence.IsDirty(), "initial authoritative document and mode become durable");
    const auto writes = f.sink.binaryAdmissions;
    f.Ack(V2Edit(f, 2U, {1, 0})); f.Pop(); f.Pop();
    fxe::ManualMillis::SetNowMs(500U); f.art.Update();
    auto ack = f.Ack(f.Request(1U)); f.Pop(); f.Pop();
    f.Ack(V2Edit(f, 2U, {0, ack.bytes[16]})); f.Pop(); f.Pop();
    f.Ack(V2Edit(f, 2U, {2, 0})); f.Pop(); f.Pop();
    f.Ack(V2Edit(f, 3U, {0, 1, 0, 3, 1, 0, 0, 0, 1}));
    fxe::ManualMillis::SetNowMs(6000U); f.art.Update(); f.persistence.Update();
    Check(!f.persistence.IsDirty() && f.sink.binaryAdmissions == writes,
          "timeline/ticks/inspect/no-op painting never request document persistence");
    f.Ack(V2FrameEdit(f, 3U, 0U, 0U, 137U));
    f.Ack(V2Edit(f, 3U, {0, 2, 0, 3, 1, 10, 20, 30, 1}));
    Check(f.persistence.IsDirty() && f.art.CanvasGeneration() == 0U,
          "advanced RGB dirties only sole document authority, never legacy canvas generation");
    fxe::ManualMillis::SetNowMs(10999U); f.persistence.Update();
    Check(f.sink.binaryAdmissions == writes, "document persistence retains five-second trailing debounce");
    fxe::ManualMillis::SetNowMs(11000U); f.persistence.Update();
    Check(f.sink.binaryAdmissions == writes + 1U, "real advanced edit submits one document snapshot after idle");
    f.sink.CompleteSuccess(); f.persistence.Update();
    Check(!f.persistence.IsDirty() && f.sink.current.size > Persistence::ADVANCED_BODY_OFFSET + 33U &&
              f.sink.current.bytes[Persistence::ADVANCED_BODY_OFFSET + 32U] == 137U &&
              f.sink.current.bytes[Persistence::ADVANCED_BODY_OFFSET + 33U] == 0U,
          "verified persistence retains an arbitrary exact frame duration");
    std::puts("[V2_PERSISTENCE] only document mutations dirty/persist; runtime and no-ops remain clean");
}

void V2StaticMigrationUndoPreservesDuplicateFavorites() {
    V2Fixture f(V2Fixture::Authority::Static);
    modules::LitchcraftQuickPalette favorites{{
        {11, 22, 33}, {11, 22, 33}, {0, 0, 0}, {255, 255, 255},
        {0, 0, 0}, {11, 22, 33}, {2, 3, 4}, {2, 3, 4}}};
    Art::Canvas original;
    for (size_t i = 0U; i < original.size(); ++i) original[i] = {uint8_t(i), uint8_t(255U - i), 7U};
    f.art.RestoreArtwork(original, true, 0U, &favorites);
    f.art.BeginConnection(); f.next = 1U;
    const auto encoded = f.Encoded();
    const auto begin = f.Begin(encoded);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Ack(begin);
    for (size_t offset = 0U; offset < encoded.size;) {
        auto chunk = f.TransferOp(10U, transfer);
        V2Fixture::Put(chunk, offset, 2U);
        const auto count = std::min<size_t>(231U, encoded.size - offset);
        std::copy_n(encoded.bytes.begin() + offset, count, chunk.bytes.begin() + chunk.size);
        chunk.size += count; offset += count; f.Ack(chunk);
    }
    f.Ack(f.TransferOp(11U, transfer));
    Check(f.art.UndoCount() == 1U, "first schema3 replacement retains its own Undo");
    const auto undo = f.Ack(f.Request(6U));
    const auto& restored = f.persistence.AdvancedDocument().Value();
    Check(undo.bytes[17] == 3U && restored.frameCount == 1U &&
              Equal(restored.favorites, favorites) && Equal(f.art.Artwork(), original),
          "migration Undo restores exact static RGB and all eight ordered favorites as one-frame schema3");
    const auto& emitters = restored.frames[0].emitters;
    Check(std::all_of(emitters.begin(), emitters.end(), [](const auto& emitter) {
              return emitter.effect == modules::litchcraft_animation::Effect::Off && emitter.durationMs == 0U;
          }), "static migration never invents effects");
    f.Ack(f.Request(7U));
    Check(f.persistence.AdvancedDocument().Value().frameCount == 8U, "Redo restores complete incoming animation after static migration Undo");
    std::puts("[V2_MIGRATION] static RGB/ordered duplicate favorites/one-frame schema3 Undo/full Redo passed");
    std::printf("[V2_FEATURE_MEMORY] session=%zu persistence=%zu bodies=%zu render=%zu history=%zu total=%zu headroom=%zu\n",
        sizeof(Art), sizeof(Persistence), sizeof(Persistence::AdvancedStorage),
        sizeof(modules::litchcraft_animation::ComposedFrame), sizeof(modules::LitchcraftHistory),
        sizeof(Art) + sizeof(Persistence) + sizeof(Persistence::AdvancedStorage) + sizeof(modules::litchcraft_animation::ComposedFrame),
        64U * 1024U - sizeof(Art) - sizeof(Persistence) - sizeof(Persistence::AdvancedStorage) - sizeof(modules::litchcraft_animation::ComposedFrame));
}

void V2EditPauseAndEntryClockReset() {
    V2Fixture f(V2Fixture::Authority::Static);
    auto document = AdvancedDocument(2U, 11U).Value();
    document.playbackOnEntry = true;
    modules::litchcraft_animation::ValidatedDocument adopted;
    Check(modules::litchcraft_animation::ValidatedDocument::TryAdopt(document, adopted) &&
              f.persistence.SetAdvancedDocument(adopted, true), "playback-on-entry fixture adopted");
    Check(!f.art.IsAnimationPlaying(),
          "saved playback-on-entry flag does not start playback");
    const uint8_t play[] = {1U, 0U};
    Check(f.art.ApplyConnectionOrdered(2U, play, sizeof(play)) == Art::Status::Ok,
          "fixture starts playback through explicit Play");
    fxe::ManualMillis::SetNowMs(50U); f.art.Update();
    f.Ack(V2Edit(f, 3U, {0, 1, 0, 3, 1, 10, 20, 30, 1}));
    f.Ack(f.Request(1U)); auto low = f.Pop(); auto high = f.Pop();
    Check(low.bytes[15] == 0U && V2Fixture::Read(low.bytes.data() + 16U, 4U) == 50U,
          "accepted edit pauses current elapsed without manufacturing a frame seek");
    const auto generation = f.persistence.AdvancedGeneration();
    f.art.Exit(); Drain(f.art);
    fxe::ManualMillis::SetNowMs(500U); f.art.Update();
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(low.bytes[15] == 0U, "inactive session does not play a retained document");
    Send(f.art, Art::Op::Enter, f.next++);
    Check(f.art.IsActive(), "existing Enter remains the sole app admission for advanced documents");
    Drain(f.art);
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(low.bytes[15] == 0U && high.bytes[15] == 0U && V2Fixture::Read(low.bytes.data() + 16U, 4U) == 0U,
          "app re-entry resets frame/time zero and stays paused");
    f.art.Exit(); Drain(f.art);
    fxe::ManualMillis::SetNowMs(2000U);
    f.art.ObserveEntryButton({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1}, 2000U, true);
    f.art.ObserveEntryButton({modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1}, 2020U, true);
    f.art.ObserveEntryButton({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1}, 3500U, true);
    fxe::ManualMillis::SetNowMs(2000U); f.art.Update(); Drain(f.art);
    f.Ack(f.Request(1U)); low = f.Pop(); high = f.Pop();
    Check(f.art.IsActive() && low.bytes[15] == 0U && high.bytes[15] == 0U &&
              V2Fixture::Read(low.bytes.data() + 16U, 4U) == 0U &&
              f.persistence.AdvancedGeneration() == generation,
          "physical entry resets clock while preserving complete document revision");
    std::puts("[V2_LIFECYCLE] edit pause/app Enter/physical hold/exit/clock reset passed");
}

void V2StateHistoryAvailabilityAndFrozenRetry() {
    V2Fixture f;
    Check((f.Ack(f.Request(1U)).bytes[14] & 12U) == 0U,
          "advanced document with empty history exposes neither action");
    Check((f.Ack(V2Edit(f, 3U, {0, 1, 0, 3, 1, 10, 20, 30, 1})).bytes[14] & 12U) == 4U,
          "accepted app edit exposes Undo immediately in its State");
    f.Ack(V2Edit(f, 3U, {0, 2, 0, 3, 1, 40, 50, 60, 2}));
    const auto undo = f.Request(6U);
    const auto undoAck = f.Ack(undo);
    Check((undoAck.bytes[14] & 12U) == 12U,
          "Undo exposes both actions when older operations remain");
    const auto redoAck = f.Ack(f.Request(7U));
    Check((redoAck.bytes[14] & 12U) == 4U && f.art.RedoCount() == 0U,
          "Redo clears availability after reaching newest operation");
    const auto replay = f.Ack(undo);
    Check(replay.bytes == undoAck.bytes && replay.size == undoAck.size &&
              (replay.bytes[14] & 12U) == 12U && f.art.RedoCount() == 0U,
          "identical retry replays frozen history flags rather than current availability");

    V2Fixture physical;
    physical.Ack(physical.Request(1U)); physical.Pop(); physical.Pop();
    physical.art.ObserveOccupancy(UINT64_C(1) << 12U);
    bool foundPhysicalState = false;
    for (const auto& packet : Drain(physical.art)) {
        if (packet.bytes[0] == 0x4dU && packet.bytes[2] == 0x81U &&
            V2Fixture::Read(packet.bytes.data() + 11U, 2U) == 0U) {
            foundPhysicalState = true;
            Check(packet.size == 18U && (packet.bytes[14] & 12U) == 4U,
                  "physical Hall edit exposes Undo on unsolicited authoritative State");
        }
    }
    Check(foundPhysicalState, "physical edit publishes history availability without another app edit");
    Check((physical.Ack(physical.Request(6U)).bytes[14] & 12U) == 8U,
          "app Undo of physical operation exposes only Redo at history start");
    Check((physical.Ack(physical.Request(7U)).bytes[14] & 12U) == 4U,
          "Redo of physical operation exposes only Undo at history end");

    V2Fixture absent(V2Fixture::Authority::Static);
    Send(absent.art, Art::Op::Paint, absent.next++, {1, 0, 3, 1}); Drain(absent.art);
    Check(absent.art.UndoCount() != 0U, "no-document fixture has unrelated legacy history");
    const auto noDocument = absent.Ack(absent.Request(1U));
    Check((noDocument.bytes[14] & 12U) == 0U && noDocument.bytes[15] == 0U &&
              noDocument.bytes[16] == 0U && noDocument.bytes[17] == 0U,
          "no advanced document never leaks legacy history availability into its zero tuple");
    std::puts("[V2_HISTORY_FLAGS] app/physical/Undo/Redo/no-document/reserved bits/frozen retry passed");
}

void V2PhysicalBootHeapAdmitsAdvancedReplacement() {
    V2Fixture f(V2Fixture::Authority::Static);
    V2Fixture physical(V2Fixture::Authority::Static);
    // V3 production diagnostic: 12,260 bytes free, largest block 7,668.
    // Reproduce that fragmentation at the actual history allocation boundary,
    // then send REPLACE_BEGIN through the production ingress normalizer/queue.
    const auto encoded = f.Encoded();
    const auto begin = f.Begin(encoded);
    const size_t previousLimit = litchcraft_allocation_probe::maximumBytes;
    litchcraft_allocation_probe::maximumBytes = 7668U;
    litchcraft_allocation_probe::calls = 0U;
    litchcraft_allocation_probe::largestRequest = 0U;
    f.Ingress(begin);
    const auto reply = f.Pop();
    std::printf("[V2_BOOT_HEAP] largest=7668 requested=%zu status=%u\n",
                litchcraft_allocation_probe::largestRequest, reply.bytes[13]);
    Check(reply.bytes[13] == uint8_t(Art::Status::Ok),
          "physical boot heap admits advanced REPLACE_BEGIN without ResourceLimit");
    if (reply.bytes[13] != uint8_t(Art::Status::Ok)) {
        litchcraft_allocation_probe::maximumBytes = previousLimit;
        return;
    }
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Upload(encoded, transfer, 244U);
    f.Ack(f.TransferOp(11U, transfer));
    Check(f.persistence.HasAdvancedDocument() && f.art.UndoCount() == 1U,
          "constrained boot heap connects all eight frames with initial Undo");
    f.Ack(f.Request(6U));
    Check(f.persistence.AdvancedDocument().Value().frameCount == 1U,
          "constrained-heap replacement Undo restores static source");
    f.Ack(f.Request(7U));
    Check(f.persistence.AdvancedDocument().Value().frameCount == 8U,
          "constrained-heap Redo restores all eight animation frames");
    Check(litchcraft_allocation_probe::calls == 0U,
          "Begin/chunks/commit/Undo/Redo never request late heap storage");
    // Simplified physical paint controls preserve the fixed history owner.
    // Reject every nonthrowing allocation, not merely the observed large one.
    litchcraft_allocation_probe::maximumBytes = 0U;
    physical.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1});
    physical.art.ObserveButton({modules::UserButton::Up, fxe::Button::Event::Press, modules::UiOwner::P1});
    physical.art.ObserveButton({modules::UserButton::Up, fxe::Button::Event::Release, modules::UiOwner::P1});
    physical.art.ObserveButton({modules::UserButton::P1O, fxe::Button::Event::Release, modules::UiOwner::P1});
    Drain(physical.art);
    Check(physical.art.FrameCount() == 1U && physical.art.UndoCount() == 0U,
          "physical O+Up cannot activate animation or create history");
    physical.art.ObserveOccupancy(UINT64_C(1) << 12U);
    physical.art.ObserveOccupancy(0U);
    fxe::ManualMillis::AdvanceMs(Art::PHYSICAL_RELEASE_MS);
    physical.art.Update();
    Check(physical.art.UndoCount() == 1U, "physical paint creates one bounded history edit");
    Send(physical.art, Art::Op::Undo, physical.next++);
    Check(physical.art.UndoCount() == 0U && physical.art.RedoCount() == 1U,
          "physical paint Undo restores original static frame");
    Send(physical.art, Art::Op::Redo, physical.next++);
    Check(physical.art.FrameCount() == 1U && physical.art.UndoCount() == 1U &&
              litchcraft_allocation_probe::calls == 0U,
          "physical paint and its Undo/Redo never allocate");
    litchcraft_allocation_probe::maximumBytes = previousLimit;
}


void V2ReplyShapeBelongsToRequestContract() {
    V2Fixture f;
    namespace store = modules::litchcraft_frame_store;
    modules::litchcraft_frame_stream::FrameStream stream;
    store::Manifest manifest{};
    manifest.frameCount = 32U;
    manifest.active = true;
    manifest.sparkleSeed = 7U;
    store::Frame first{};
    first.durationMs = 50U;
    Check(stream.StageFirstFrame(manifest, 7U, first), "reply fixture stages Store authority");
    f.art.AttachFrameStream(&stream);
    auto select = f.Request(23U);
    V2Fixture::Put(select, 7U, 4U);
    V2Fixture::Put(select, 2U, 1U);
    V2Fixture::Put(select, 29U, 2U);
    f.Ingress(select);
    const auto selected = f.Pop();
    Check(selected.size == 19U && selected.bytes[2] == 0x8dU &&
              selected.bytes[13] == 0U &&
              V2Fixture::Read(selected.bytes.data() + 7U, 4U) == 7U &&
              V2Fixture::Read(selected.bytes.data() + 17U, 2U) == 29U,
          "Store Select names late frame with Store generation and reply shape");
    const auto low = f.Pop();
    const auto high = f.Pop();
    Check(low.bytes[2] == 0x88U && high.bytes[2] == 0x89U &&
              V2Fixture::Read(low.bytes.data() + 7U, 4U) == 7U &&
              V2Fixture::Read(high.bytes.data() + 7U, 4U) == 7U && high.bytes[15] == 29U,
          "Store timeline retains Store generation after operation-owned replies");
    Drain(f.art);
    f.Ingress(select);
    const auto replay = f.Pop();
    Check(replay.bytes == selected.bytes && replay.size == selected.size,
          "Store runtime exact retry preserves its frozen reply");
    auto changed = select;
    changed.bytes[14U] = 28U;
    f.Ingress(changed);
    const auto refused = f.Pop();
    Check(refused.size == 19U && refused.bytes[2] == 0x8dU &&
              refused.bytes[13] == uint8_t(Art::Status::Malformed),
          "changed Store retry is refused with the Store operation shape");
    auto stale = f.Request(23U);
    V2Fixture::Put(stale, 8U, 4U);
    V2Fixture::Put(stale, 2U, 1U);
    V2Fixture::Put(stale, 3U, 2U);
    f.Ingress(stale);
    const auto staleReply = f.Pop();
    f.Ingress(stale);
    const auto staleReplay = f.Pop();
    Check(staleReply.size == 19U && staleReply.bytes[2] == 0x8dU &&
              staleReply.bytes[13] == uint8_t(Art::Status::RevisionConflict) &&
              staleReplay.bytes == staleReply.bytes && staleReplay.size == staleReply.size,
          "stale Store generation has an exactly replayable Store refusal");
    for (const bool wrongVersion : {false, true}) {
        auto invalid = f.Request(23U);
        invalid.bytes[wrongVersion ? 1U : 3U] ^= 1U;
        f.Ingress(invalid);
        const auto refusal = f.Pop();
        Check(refusal.size == 19U && refusal.bytes[2] == 0x8dU &&
                  refusal.bytes[13] == uint8_t(wrongVersion
                      ? Art::Status::Unsupported : Art::Status::StaleSession),
              "pre-dispatch Store refusal keeps the incoming operation's shape");
    }
    const auto legacy = f.Ack(V2Edit(f, 2U, {0U, 0U}), Art::Status::Busy);
    Check(legacy.bytes[14] == 1U && legacy.bytes[15] == 0U &&
              legacy.bytes[16] == 0U && legacy.bytes[17] == 0U &&
              V2Fixture::Read(legacy.bytes.data() + 7U, 4U) == f.persistence.AdvancedGeneration(),
          "legacy refusal exposes valid empty document metadata, not Store frame 29");
    const auto begin = f.Begin(f.Encoded());
    const auto beginReply = f.Ack(begin);
    const auto beginReplay = f.Ack(begin);
    Check(beginReply.bytes[14] == 1U && beginReply.bytes[15] == 0U &&
              beginReply.bytes[16] == 0U && beginReply.bytes[17] == 0U &&
              beginReplay.bytes == beginReply.bytes && beginReplay.size == beginReply.size,
          "schema-3 replacement Begin and replay retain valid legacy metadata");
    stream.RetireDocument();
    const auto unavailable = f.Request(23U);
    f.Ingress(unavailable);
    const auto absent = f.Pop();
    Check(absent.size == 19U && absent.bytes[2] == 0x8dU &&
              absent.bytes[13] == uint8_t(Art::Status::Unsupported) &&
              V2Fixture::Read(absent.bytes.data() + 7U, 4U) == 0U &&
              V2Fixture::Read(absent.bytes.data() + 3U, 4U) == f.art.SessionId() &&
              V2Fixture::Read(absent.bytes.data() + 11U, 2U) ==
                  V2Fixture::Read(unavailable.bytes.data() + 7U, 2U) &&
              (absent.bytes[14] & 6U) == 0U && absent.bytes[15] == 0U &&
              absent.bytes[16] == 0U && absent.bytes[17] == 0U && absent.bytes[18] == 0U,
          "Store request without Store artwork retains valid empty Store refusal");
}

void V2ReplacementRetiresStreamWithoutBlankOrRuntimeReversion() {
    namespace store = modules::litchcraft_frame_store;
    const int priorFailures = failures;
    for (const uint8_t replacement : {11U, 37U}) {
        const uint8_t frameCount = replacement == 11U ? 1U : 8U;
        V2Fixture f;
        modules::litchcraft_frame_stream::FrameStream stream;
        store::Manifest manifest{};
        manifest.frameCount = 4U;
        manifest.active = true;
        manifest.sparkleSeed = 7U;
        store::Frame frame{};
        frame.durationMs = 100U;
        frame.emitters[0].color = {99U, 0U, 0U};
        Check(stream.StageFirstFrame(manifest, 1U, frame), "retirement fixture stages old stream");
        f.art.AttachFrameStream(&stream);
        const auto renderedRed = [&] {
            modules::BoardLedFrame board{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
            f.art.Compose(board, edge);
            return board[0].left.r;
        };
        Check(f.art.FrameCount() == 4U && renderedRed() == 99U,
              "stream overrides the earlier schema-3 draft before replacement");
        auto bytes = f.Encoded(replacement, frameCount);
        auto begin = f.Begin(bytes);
        auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        f.Ack(begin);
        f.Ack(f.TransferOp(11U, transfer), Art::Status::Incomplete);
        Check(stream.Generation() == 1U && renderedRed() == 99U,
              "incomplete replacement preserves live stream and rendered frame");
        begin = f.Begin(bytes);
        transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        f.Ack(begin);
        auto corrupt = bytes;
        corrupt.bytes[corrupt.size - 1U] ^= 1U;
        f.Upload(corrupt, transfer, 244U);
        f.Ack(f.TransferOp(11U, transfer), Art::Status::IntegrityFailure);
        Check(stream.Generation() == 1U && renderedRed() == 99U,
              "corrupt replacement preserves old stream without a blank frame");
        begin = f.Begin(bytes);
        transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        f.Ack(begin);
        f.Upload(bytes, transfer, 244U);
        Check(renderedRed() == 99U, "complete staging remains invisible until semantic commit");
        f.Ack(f.TransferOp(11U, transfer));
        Check(stream.Generation() == 0U && stream.FrameCount() == 0U &&
                  f.art.FrameCount() == frameCount && renderedRed() == replacement &&
                  f.persistence.AdvancedGeneration() == 2U,
              "accepted one-frame or animated replacement immediately renders schema-3");
        Check(!stream.Configure(manifest, 1U) && renderedRed() == replacement,
              "worker reloading old durable root cannot resurrect retired stream");
        f.persistence.RequestFlush();
        f.persistence.Update();
        Check(f.sink.retained != 0U, "replacement receives a real deferred persistence receipt");
        f.sink.outcome = Persistence::SaveOutcome::Failed;
        f.persistence.Update();
        f.art.BeginConnection();
        Check(stream.Generation() == 0U && renderedRed() == replacement,
              "failed asynchronous save and reconnect never revert accepted RAM artwork");
        f.Ack(V2Edit(f, 3U, {0U, 1U, 0U, 3U, 1U, 47U, 2U, 3U, 0U}));
        Check(renderedRed() == 47U && f.art.FrameCount() == frameCount,
              "ordinary document painting regains visible authority after return");
    }
    {
        V2Fixture f;
        modules::litchcraft_frame_stream::FrameStream unpublished;
        f.art.AttachFrameStream(&unpublished);
        const auto bytes = f.Encoded(11U);
        const auto begin = f.Begin(bytes);
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        f.Ack(begin);
        f.Upload(bytes, transfer, 244U);
        f.Ack(f.TransferOp(11U, transfer));
        Check(unpublished.NeedsDurableRetirement() &&
                  f.persistence.AdvancedGeneration() == 2U,
              "replacement before root publication still requests durable retirement of identical draft");
        store::Manifest oldRoot{};
        Check(!unpublished.Configure(oldRoot, 1U),
              "an unresolved old root cannot override accepted schema-3 artwork");
    }
    if (failures == priorFailures) {
        std::puts("[SCHEMA3_RETURN_JOURNEY] stream=99 incomplete/corrupt=99 commit=draft save-failed/reconnect=draft paint=47 blank=never");
    }
}

void V2MinimumAndHighMtuRoundtripAndExactReplay() {
    for (const size_t payload : {20U, 244U}) {
        V2Fixture f;
        auto bytes = f.Encoded();
        auto begin = f.Begin(bytes);
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        const auto beginAck = f.Ack(begin);
        const auto duplicateBegin = f.Ack(begin);
        Check(beginAck.bytes == duplicateBegin.bytes && beginAck.size == duplicateBegin.size, "Begin retry replays exact frozen ACK");
        f.Upload(bytes, transfer, payload);
        auto commit = f.TransferOp(11U, transfer);
        const auto commitAck = f.Ack(commit);
        Check(f.persistence.AdvancedGeneration() == 2U, "whole transfer advances sole document revision once");
        const auto duplicateCommit = f.Ack(commit);
        Check(commitAck.bytes == duplicateCommit.bytes && f.persistence.AdvancedGeneration() == 2U, "Commit retry exact ACK and no duplicate adoption");
        // Generation change is a watermark, never a static Cell or flat snapshot.
        const auto changed = f.Pop(payload);
        Check(changed.bytes[2] == 0x87U && V2Fixture::Read(changed.bytes.data() + 7U, 4U) == 2U, "document mutation emits v2 watermark");
        auto snapshot = f.Request(8U);
        const auto snapshotAck = f.Ack(snapshot);
        const auto head = f.Pop(payload);
        Check(head.bytes[2] == 0x84U && head.size == 20U &&
                  V2Fixture::Read(head.bytes.data() + 14U, 2U) == bytes.size &&
                  V2Fixture::Read(head.bytes.data() + 16U, 4U) == bytes.crc32,
              "State precedes matching canonical SnapshotBegin with exact length/CRC");
        const auto duplicateSnapshot = f.Ack(snapshot);
        Check(snapshotAck.bytes == duplicateSnapshot.bytes, "snapshot replay does not restart Begin or alter ACK");
        // Change live authority while snapshot is frozen. The outgoing body must
        // remain the previous version, and only one later watermark may follow.
        Check(f.persistence.SetAdvancedDocument(AdvancedDocument(8U, 23U), true), "physical/document owner can advance during snapshot");
        std::vector<uint8_t> received;
        size_t chunks = 0U;
        for (;;) {
            const auto packet = f.Pop(payload);
            Check(V2Fixture::Read(packet.bytes.data() + 7U, 4U) == 2U, "snapshot keeps frozen document revision");
            Check(V2Fixture::Read(packet.bytes.data() + 3U, 4U) == f.art.SessionId() &&
                      V2Fixture::Read(packet.bytes.data() + 11U, 2U) == V2Fixture::Read(snapshot.bytes.data() + 7U, 2U),
                  "snapshot session/request remain frozen");
            if (packet.bytes[2] == 0x86U) break;
            Check(packet.bytes[2] == 0x85U && packet.size <= payload && packet.size > 15U &&
                      V2Fixture::Read(packet.bytes.data() + 13U, 2U) == received.size(), "snapshot bounded contiguous byte offsets");
            received.insert(received.end(), packet.bytes.begin() + 15U, packet.bytes.begin() + packet.size);
            ++chunks;
            if (chunks > 2500U) { Check(false, "snapshot terminates within descriptor-independent bound"); break; }
        }
        Check(received.size() == bytes.size && std::equal(received.begin(), received.end(), bytes.bytes.begin()), "minimum/high MTU snapshot exact canonical bytes");
        Check(chunks == (bytes.size + payload - 16U) / (payload - 15U), "snapshot generated on demand beyond 96 descriptors");
        Check(f.Pop(payload).bytes[2] == 0x87U, "newer live version emits watermark after frozen End");
        Art::BulkPacket empty;
        Check(!f.art.PeekPacket(empty, payload), "snapshot has no duplicate queued admission ACK");
        Check(f.persistence.AdvancedTransportStorage() == nullptr, "End releases shared persistence envelope");
    }
    std::printf("[V2] minimum/high MTU exact roundtrip/replay/frozen snapshot passed\n");
}

void V2InactiveStorageUploadPreservesGameAuthority() {
    V2Fixture f(V2Fixture::Authority::Advanced, false);
    f.eligible = false;
    f.occupancy = UINT64_C(0x00ff00000000ff00);
    const auto session = f.art.SessionId();
    const auto legacyRevision = f.art.Revision();
    const auto generation = f.persistence.AdvancedGeneration();
    const auto bytes = f.Encoded(37U);
    const auto begin = f.Begin(bytes);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);

    f.Ack(begin);
    f.Upload(bytes, transfer, 244U);
    f.Ack(f.TransferOp(11U, transfer));

    Check(!f.art.IsActive() && f.art.SessionId() == session &&
              f.art.Revision() == legacyRevision,
          "inactive storage upload never enters or displays Litchcraft");
    Check(f.persistence.AdvancedGeneration() == generation + 1U &&
              f.persistence.AdvancedDocument().Value().frames[0].emitters[0].color.r == 37U,
          "inactive storage upload adopts the complete artwork independently of game authority");
    Check(f.persistence.IsSaving(),
          "inactive storage upload requests immediate asynchronous persistence");
    std::puts("[V2_INACTIVE_STORAGE] game-state/display isolation/flush request passed");
}

void V2InvalidBootAcceptsExplicitReplacement() {
    V2Fixture f(V2Fixture::Authority::Static);
    f.sink.currentPresent = true;
    f.sink.current.size = 1U;
    f.sink.current.bytes[0] = 0U;
    Check(f.persistence.RestoreAdvanced() ==
              Persistence::AdvancedRecordStatus::Invalid,
          "app regression recreates invalid advanced artwork at boot");

    auto inspect = f.Request(1U);
    const auto inspected = f.Ack(inspect);
    Check(inspected.bytes[15] == 0U && inspected.bytes[17] == 0U,
          "v2 Inspect truthfully exposes no transferable document");
    const auto encoded = f.Encoded(31U);
    const auto begin = f.Begin(encoded);
    f.Ack(begin);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Upload(encoded, transfer, 244U);
    f.Ack(f.TransferOp(11U, transfer));
    Check(f.persistence.HasAdvancedDocument() &&
              f.persistence.AdvancedDocument().Value().sparkleSeed ==
                  AdvancedDocument(8U, 31U).Value().sparkleSeed,
          "explicit app Replace recovers invalid source with one valid document");
}

void V2FencingTimeoutAndLeaseRecovery() {
    V2Fixture f;
    auto bytes = f.Encoded();
    auto begin = f.Begin(bytes);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Ack(begin);
    auto competing = f.Begin(bytes);
    f.Ack(competing, Art::Status::Busy);
    fxe::ManualMillis::SetNowMs(5000U);
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 0U, "transport lease interlocks trailing autosave");
    f.Ack(begin); // Replay must not move the inactivity deadline.
    fxe::ManualMillis::SetNowMs(6000U);
    f.art.Update();
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "six-second timeout not extended by duplicate Begin");
    f.Ack(f.TransferOp(11U, transfer), Art::Status::Incomplete);
    Check(f.persistence.AdvancedGeneration() == 1U, "timeout leaves original complete artwork intact");
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 1U, "retired transfer lets pending autosave progress");
    f.Ack(f.Begin(bytes), Art::Status::Busy);
    f.sink.CompleteSuccess(); f.persistence.Update();
    // Entry's independent mode record may follow the binary autosave receipt.
    if (f.sink.retained != 0U) { f.sink.CompleteSuccess(); f.persistence.Update(); }
    auto pending = f.Begin(bytes); f.Ack(pending);
    const auto pendingId = V2Fixture::Read(pending.bytes.data() + 7U, 2U);
    auto badOffset = f.TransferOp(10U, pendingId);
    V2Fixture::Put(badOffset, 1U, 2U); V2Fixture::Put(badOffset, 3U, 1U);
    f.Ack(badOffset, Art::Status::Incomplete);
    f.Ack(f.TransferOp(11U, pendingId), Art::Status::Incomplete);
    Check(f.persistence.AdvancedTransportStorage() == nullptr && f.persistence.AdvancedGeneration() == 1U, "incomplete Commit retires staging without adoption");
    pending = f.Begin(bytes); f.Ack(pending);
    f.art.BeginConnection(); ++f.epoch;
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "replacement connection discards prior staging");
    f.next = 1U;
    f.Ack(f.TransferOp(11U, V2Fixture::Read(pending.bytes.data() + 7U, 2U)), Art::Status::Incomplete);
    auto fresh = f.Begin(bytes); f.Ack(fresh);
    auto cancel = f.TransferOp(12U, V2Fixture::Read(fresh.bytes.data() + 7U, 2U));
    f.Ack(cancel); f.Ack(cancel);
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "Cancel retry harmless and lease retired");
    auto staleSession = f.Begin(bytes); staleSession.bytes[3] ^= 1U;
    f.Ack(staleSession, Art::Status::StaleSession);
    auto staleBase = f.Begin(bytes); staleBase.bytes[9] ^= 1U;
    f.Ack(staleBase, Art::Status::RevisionConflict);
    Check(f.persistence.AdvancedGeneration() == 1U, "session/base fences preserve document");
    // At 180 seconds, even recent accepted chunks cannot keep a transfer alive.
    fresh = f.Begin(bytes); f.Ack(fresh);
    const auto freshId = V2Fixture::Read(fresh.bytes.data() + 7U, 2U);
    for (uint16_t offset = 0U; offset < 35U; ++offset) {
        fxe::ManualMillis::SetNowMs(6000U + 5000U * (offset + 1U));
        auto chunk = f.TransferOp(10U, freshId);
        V2Fixture::Put(chunk, offset, 2U); V2Fixture::Put(chunk, bytes.bytes[offset], 1U);
        f.Ack(chunk);
    }
    fxe::ManualMillis::SetNowMs(186000U); f.art.Update();
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "180-second absolute deadline bounds continuously progressing transfer");
    std::printf("[V2] deadlines/leases/session/base/cancel/reconnect passed\n");
}

void V2IntegrityReceiptsAndLegacyCompatibility() {
    V2Fixture f;
    auto bytes = f.Encoded();
    auto begin = f.Begin(bytes); f.Ack(begin);
    f.Upload(bytes, V2Fixture::Read(begin.bytes.data() + 7U, 2U), 244U);
    f.storage.envelope.bytes[100] ^= 1U;
    f.Ack(f.TransferOp(11U, V2Fixture::Read(begin.bytes.data() + 7U, 2U)), Art::Status::IntegrityFailure);
    Check(f.persistence.AdvancedGeneration() == 1U && f.persistence.AdvancedTransportStorage() == nullptr, "CRC failure preserves live body and releases staging");
    bytes.bytes[0] = 4U; bytes.crc32 = fxe::Crc32(bytes.bytes.data(), bytes.size);
    begin = f.Begin(bytes); f.Ack(begin);
    f.Upload(bytes, V2Fixture::Read(begin.bytes.data() + 7U, 2U), 244U);
    f.Ack(f.TransferOp(11U, V2Fixture::Read(begin.bytes.data() + 7U, 2U)), Art::Status::Unsupported);
    Check(f.persistence.AdvancedGeneration() == 1U, "unknown schema cannot adopt partial or flattened document");
    auto inspect = f.Request(1U);
    const auto originalAck = f.Ack(inspect);
    for (size_t i = 0U; i < 31U; ++i) f.Ack(f.Request(1U));
    Check(f.Ack(inspect).bytes == originalAck.bytes, "32nd retained request replays exact ACK");
    auto reused = inspect; reused.bytes[2] = 8U;
    f.Ack(reused, Art::Status::Malformed);
    f.Ack(f.Request(1U));
    f.Ack(inspect, Art::Status::StaleRequest);
    Drain(f.art);
    const auto staticCanvas = f.art.Artwork();
    Send(f.art, Art::Op::Inspect, f.next++);
    Check(HasAck(Drain(f.art), f.next - 1U, Art::Status::Unsupported), "old app Inspect refuses advanced flattening");
    Send(f.art, Art::Op::Snapshot, f.next++);
    Check(HasAck(Drain(f.art), f.next - 1U, Art::Status::Unsupported), "old app Snapshot refuses advanced flattening");
    Send(f.art, Art::Op::Clear, f.next++);
    Check(HasAck(Drain(f.art), f.next - 1U, Art::Status::Busy) && Equal(f.art.Artwork(), staticCanvas), "old app mutation cannot change advanced or static source");
    auto staleExit = ::Request(f.art, Art::Op::Exit, f.next++);
    staleExit.bytes[2] ^= 1U;
    f.art.HandlePacket(staleExit.bytes.data(), staleExit.size, true, 0U);
    Check(HasAck(Drain(f.art), f.next - 1U, Art::Status::StaleSession) && f.art.IsActive(), "legacy Exit keeps session fence with advanced owner");
    Send(f.art, Art::Op::Exit, f.next++);
    Check(!f.art.IsActive() && f.persistence.AdvancedGeneration() == 1U,
          "legacy Exit releases mode immediately and preserves advanced document");
    Check(sizeof(Persistence::AdvancedStorage) <= 40U * 1024U && Persistence::ADVANCED_ACTIVATION_BODY_COUNT == 3U,
          "exactly live/staging/envelope bodies fit admission bound independently of history overflow");
    std::printf("[V2] CRC/schema/32-receipt/shared allocator/legacy compatibility passed\n");
}

void V2LongPacketFingerprintReplay() {
    V2Fixture f;
    const auto encoded = f.Encoded();
    const auto begin = f.Begin(encoded);
    f.Ack(begin);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    auto chunk = f.TransferOp(10U, transfer);
    V2Fixture::Put(chunk, 0U, 2U);
    std::copy_n(encoded.bytes.begin(), 231U, chunk.bytes.begin() + chunk.size);
    chunk.size += 231U;
    const auto first = f.Ack(chunk);
    const auto before = f.storage.envelope;
    for (size_t index = 9U; index < chunk.size; ++index) {
        auto changed = chunk;
        changed.bytes[index] ^= 1U;
        f.Ack(changed, Art::Status::Malformed);
    }
    auto shorter = chunk;
    --shorter.size;
    f.Ack(shorter, Art::Status::Malformed);
    Check(f.Ack(chunk).bytes == first.bytes && before.bytes == f.storage.envelope.bytes,
          "maximum-MTU fingerprint rejects changes at every payload offset and length without changing staging or replay");
    for (size_t i = 0U; i < 31U; ++i) f.Ack(f.Request(1U));
    Check(f.Ack(chunk).bytes == first.bytes,
          "long-packet receipt remains exact at the 32-entry replay boundary");
    f.Ack(f.Request(1U));
    f.Ack(chunk, Art::Status::StaleRequest);
}

void V2NoopsConflictsPipeliningAndPausedCleanup() {
    V2Fixture f;
    auto bytes = f.Encoded(11U); // Identical authority uses different transport IDs.
    auto begin = f.Begin(bytes); f.Ingress(begin);
    auto early = f.Request(1U); f.Ingress(early);
    Check(f.Pop().bytes[13] == 0U && f.Pop().bytes[13] == uint8_t(Art::Status::Busy),
          "only one acknowledged request admitted until first notification accepted");
    f.Ack(early);
    f.Upload(bytes, V2Fixture::Read(begin.bytes.data() + 7U, 2U), 244U);
    f.Ack(f.TransferOp(11U, V2Fixture::Read(begin.bytes.data() + 7U, 2U)));
    Check(f.persistence.AdvancedGeneration() == 1U, "identical whole replacement is a document no-op");
    begin = f.Begin(bytes); f.Ack(begin);
    f.Upload(bytes, V2Fixture::Read(begin.bytes.data() + 7U, 2U), 244U);
    Check(f.persistence.SetAdvancedDocument(AdvancedDocument(8U, 29U), true), "independent live edit remains admitted during staging");
    f.Ack(f.TransferOp(11U, V2Fixture::Read(begin.bytes.data() + 7U, 2U)), Art::Status::RevisionConflict);
    Check(f.persistence.AdvancedGeneration() == 2U && f.persistence.AdvancedDocument().Value().frames[0].emitters[0].color.r == 29U,
          "commit base conflict retires staging and preserves independent edit");
    f.Pop(); // Coalesced generation watermark.
    begin = f.Begin(bytes); f.Ack(begin);
    f.art.SetMutationsPaused(true);
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "runtime pause retires transfer before lifecycle flush");
    f.Ack(f.Begin(bytes), Art::Status::Busy);
    f.art.SetMutationsPaused(false);
    begin = f.Begin(bytes); f.Ack(begin);
    f.art.Exit();
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "session Exit releases transport body before durable departure");
    std::printf("[V2_MEMORY] live_staging_outgoing=3 storage=%zu session_with_receipts=%zu combined=%zu\n",
                sizeof(Persistence::AdvancedStorage), sizeof(Art), sizeof(Persistence::AdvancedStorage) + sizeof(Art));
}

// Shared packet fixture format: one name,hex row per complete request/event.
// Snapshot packets reference the separately shared canonical codec-byte file;
// omitted document bytes there are zero. Tests consume fixed bytes, never
// regenerate expectations from the firmware packet encoder.
Art::BulkPacket SharedV2Packet(const std::string& name) {
    std::ifstream input("test/fixtures/litchcraft_v2_protocol_bytes.csv");
    std::string line;
    while (std::getline(input, line)) {
        const auto comma = line.find(',');
        if (comma == std::string::npos || line.substr(0U, comma) != name) continue;
        Art::BulkPacket packet;
        const auto hex = line.substr(comma + 1U);
        Check(hex.size() % 2U == 0U && hex.size() <= 488U, "shared v2 packet bound");
        for (size_t i = 0U; i + 1U < hex.size() && packet.size < packet.bytes.size(); i += 2U) {
            packet.bytes[packet.size++] = static_cast<uint8_t>(std::stoul(hex.substr(i, 2U), nullptr, 16));
        }
        return packet;
    }
    Check(false, ("missing shared v2 row: " + name).c_str());
    return {};
}
void CheckV2Fixture(const Art::BulkPacket& actual, const std::string& name) {
    const auto expected = SharedV2Packet(name);
    Check(actual.size == expected.size && std::equal(actual.bytes.begin(),
              actual.bytes.begin() + actual.size, expected.bytes.begin()), name.c_str());
}
void AdoptSharedV2Document(V2Fixture& fixture) {
    std::ifstream input("test/fixtures/litchcraft_animation_codec_bytes.csv");
    modules::litchcraft_animation::EncodedDocument document;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto comma = line.find(',');
        const auto key = line.substr(0U, comma);
        const auto value = line.substr(comma + 1U);
        if (key == "length") document.size = std::stoul(value);
        else if (key == "crc32") document.crc32 = std::stoul(value, nullptr, 16);
        else {
            const size_t offset = std::stoul(key);
            for (size_t i = 0U; i < value.size(); i += 2U) {
                document.bytes[offset + i / 2U] = static_cast<uint8_t>(std::stoul(value.substr(i, 2U), nullptr, 16));
            }
        }
    }
    Check(modules::litchcraft_animation::DecodeDocument(document.bytes.data(), document.size,
              document.crc32, fixture.storage.staging, fixture.storage.document),
          "shared canonical fixture adopts without another body");
}

void V2AbsentAuthorityExactStateFixtures() {
    V2Fixture f(V2Fixture::Authority::Static);
    const auto begin = SharedV2Packet("replace_begin_no_document_request");
    CheckV2Fixture(f.Ack(begin), "replace_begin_no_document_ack");
    Check(!f.persistence.HasAdvancedDocument() && f.persistence.AdvancedTransportStorage() != nullptr,
          "static authority admits ReplaceBegin while reporting schema/frameCount/selected zero");
    auto competing = begin; competing.bytes[7] = 3U;
    CheckV2Fixture(f.Ack(competing, Art::Status::Busy), "busy_no_document_state");
    f.next = 4U;
    f.Ack(f.TransferOp(12U, 2U));
    V2Fixture unconfigured(V2Fixture::Authority::Unconfigured);
    CheckV2Fixture(unconfigured.Ack(begin, Art::Status::Unsupported), "unsupported_no_document_state");
    std::puts("[V2] absent-authority successful Begin/Busy/Unsupported exact shared bytes passed");
}

void V2SharedSnapshotMinimumAndHighMtuFixtures() {
    for (const size_t payload : {20U, 244U}) {
        V2Fixture f;
        AdoptSharedV2Document(f);
        f.art.Exit(); Drain(f.art);
        const auto snapshot = SharedV2Packet("snapshot_session_zero_request");
        CheckV2Fixture(f.Ack(snapshot), "snapshot_inactive_ack");
        CheckV2Fixture(f.Pop(payload), "snapshot_begin");
        CheckV2Fixture(f.Pop(payload), payload == 20U ? "snapshot_chunk_att20" : "snapshot_chunk_att244");
        Art::BulkPacket packet;
        size_t remaining = 400U;
        do { packet = f.Pop(payload); } while (packet.bytes[2] != 0x86U && --remaining != 0U);
        CheckV2Fixture(packet, "snapshot_end");
        Check(f.persistence.AdvancedTransportStorage() == nullptr, "shared fixture End retires snapshot lease");
    }
    V2Fixture conflict;
    AdoptSharedV2Document(conflict);
    CheckV2Fixture(conflict.Ack(SharedV2Packet("replace_begin_conflict_request"),
                  Art::Status::RevisionConflict), "revision_conflict_state");
    std::puts("[V2] shared ATT20/244 Snapshot Begin/Chunk/End and revision-conflict bytes passed");
}

void V2SessionZeroSnapshotCancelReleasesLeaseWhilePaused() {
    V2Fixture f;
    AdoptSharedV2Document(f);
    f.art.Exit(); Drain(f.art);
    f.art.SetMutationsPaused(true);
    const auto snapshot = SharedV2Packet("snapshot_session_zero_request");
    CheckV2Fixture(f.Ack(snapshot), "snapshot_inactive_ack");
    CheckV2Fixture(f.Pop(), "snapshot_begin");
    fxe::ManualMillis::SetNowMs(5000U); f.persistence.Update();
    Check(f.sink.binaryAdmissions == 0U, "paused outgoing snapshot retains autosave lease until cancelled");
    auto cancel = SharedV2Packet("cancel_session_zero_request");
    auto unrelated = cancel; unrelated.bytes[9] = 9U;
    f.Ack(unrelated, Art::Status::StaleSession);
    auto zeroMutation = SharedV2Packet("replace_begin_no_document_request");
    std::fill_n(zeroMutation.bytes.begin() + 3U, 4U, 0U);
    zeroMutation.bytes[7] = 3U;
    f.Ack(zeroMutation, Art::Status::Busy);
    Check(f.persistence.AdvancedTransportStorage() != nullptr,
          "storage upload cannot steal a paused outgoing snapshot lease");
    cancel.bytes[7] = 4U;
    const auto cancelAfterBusy = f.Ack(cancel);
    Check(cancelAfterBusy.size == 18U && cancelAfterBusy.bytes[2] == 0x81U &&
              V2Fixture::Read(cancelAfterBusy.bytes.data() + 11U, 2U) == 4U &&
              cancelAfterBusy.bytes[13] == uint8_t(Art::Status::Ok) &&
              cancelAfterBusy.bytes[14] == 0U && cancelAfterBusy.bytes[15] == 1U &&
              cancelAfterBusy.bytes[16] == 0U && cancelAfterBusy.bytes[17] == 3U,
          "session-zero Cancel after storage Busy returns one exact inactive State");
    Check(f.persistence.AdvancedTransportStorage() == nullptr, "matching session-zero Cancel releases paused outgoing lease immediately");
    auto nextSnapshot = snapshot; nextSnapshot.bytes[7] = 5U;
    f.Ack(nextSnapshot);
    Check(f.persistence.AdvancedTransportStorage() != nullptr, "new inactive snapshot succeeds immediately without timeout");
    Check(f.Ack(cancel).bytes == cancelAfterBusy.bytes,
          "exact old session-zero Cancel replays its frozen State");
    Check(f.persistence.AdvancedTransportStorage() != nullptr,
          "exact old Cancel receipt replay cannot retire replacement snapshot");
    auto nextCancel = cancel; nextCancel.bytes[7] = 6U; nextCancel.bytes[9] = 5U;
    f.Ack(nextCancel);
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 1U, "Cancel immediately permits due autosave while runtime mutations remain paused");
    // A session-zero request cannot cancel a snapshot admitted with the real
    // session, nor a transfer from the preceding physical connection.
    V2Fixture active;
    auto ownedSnapshot = active.Request(8U); active.Ack(ownedSnapshot);
    active.Ack(cancel, Art::Status::StaleSession);
    Check(active.persistence.AdvancedTransportStorage() != nullptr, "zero-session cancellation cannot claim a nonzero-session snapshot");
    active.art.BeginConnection();
    active.Ack(cancel, Art::Status::StaleSession);
    auto zeroSnapshot = snapshot; zeroSnapshot.bytes[7] = 4U;
    active.Ack(zeroSnapshot);
    auto activeCancel = cancel;
    activeCancel.bytes[7] = 5U;
    activeCancel.bytes[9] = 4U;
    active.Ack(activeCancel);
    active.next = 6U;
    active.Ack(active.Begin(active.Encoded()));
    Check(active.persistence.AdvancedTransportStorage() != nullptr, "active replacement succeeds immediately after zero-session snapshot cancellation");
    std::puts("[V2] paused session-zero snapshot cancel/ownership/reconnect/new-transfer/autosave passed");
}

void V2InactiveNonzeroSessionSnapshotCancel() {
    V2Fixture f;
    f.art.Exit(); Drain(f.art);
    f.art.SetMutationsPaused(true);
    const auto inspected = f.Ack(f.Request(1U));
    const uint32_t retainedSession = V2Fixture::Read(inspected.bytes.data() + 3U, 4U);
    Check(!f.art.IsActive() && retainedSession != 0U && retainedSession == f.art.SessionId(),
          "inactive Inspect retains the nonzero session used by the client");
    const auto snapshot = f.Request(8U);
    const auto transfer = V2Fixture::Read(snapshot.bytes.data() + 7U, 2U);
    f.Ack(snapshot);
    Check(f.Pop().bytes[2] == 0x84U, "inactive nonzero-session snapshot begins while mutations paused");
    fxe::ManualMillis::SetNowMs(5000U); f.persistence.Update();
    Check(f.sink.binaryAdmissions == 0U, "inactive nonzero outgoing snapshot interlocks autosave");
    auto cancel = f.TransferOp(12U, transfer);
    auto wrongSession = cancel; wrongSession.bytes[3] ^= 1U;
    f.Ack(wrongSession, Art::Status::StaleSession);
    auto wrongTransfer = cancel; wrongTransfer.bytes[9] ^= 1U;
    f.Ack(wrongTransfer, Art::Status::StaleSession);
    auto mutation = f.Begin(f.Encoded());
    f.Ack(mutation, Art::Status::StaleSession);
    Check(f.persistence.AdvancedTransportStorage() != nullptr,
          "inactive cancellation exception neither accepts wrong ownership nor authorizes mutation");
    const auto acceptedCancel = f.Ack(cancel);
    Check(f.persistence.AdvancedTransportStorage() == nullptr,
          "exact inactive nonzero-session Cancel releases paused snapshot immediately");
    const auto replacement = f.Request(8U); f.Ack(replacement);
    const auto duplicateCancel = f.Ack(cancel);
    Check(acceptedCancel.size == duplicateCancel.size && acceptedCancel.bytes == duplicateCancel.bytes &&
              f.persistence.AdvancedTransportStorage() != nullptr,
          "exact inactive nonzero Cancel retry replays ACK without touching replacement snapshot");
    f.Ack(f.TransferOp(12U, V2Fixture::Read(replacement.bytes.data() + 7U, 2U)));
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 1U && f.persistence.AdvancedGeneration() == 1U && !f.art.IsActive(),
          "inactive nonzero Cancel permits immediate due autosave without admitting or changing artwork");
    f.art.BeginConnection();
    f.Ack(cancel, Art::Status::StaleSession);
    std::puts("[V2] inactive nonzero Inspect/Snapshot/Cancel paused lease/retry/autosave/epoch passed");
}

void AdvancedPersistenceLane() {
    using AdvancedStatus = Persistence::AdvancedRecordStatus;
    modules::SettingsStore settings{Config()};
    Art art;
    AdvancedSink sink;
    Persistence::AdvancedStorage storage;
    Persistence persistence{settings, art};
    RemoveFiles();
    Check(settings.Load(), "advanced lane loads independent static settings");
    art.Configure(3U, 1000U);
    persistence.Configure(sink.Ops());
    persistence.ConfigureAdvancedStorage(storage);
    Check(persistence.RestoreAdvanced() == AdvancedStatus::Missing,
          "empty binary transaction lane has no advanced artwork");

    auto first = AdvancedDocument(8U, 11U);
    fxe::ManualMillis::SetNowMs(100U);
    Check(persistence.SetAdvancedDocument(first, true),
          "configured binary backend accepts an eight-frame authority");
    fxe::ManualMillis::SetNowMs(5099U);
    persistence.Update();
    Check(sink.binaryAdmissions == 0U && sink.jsonAdmissions == 0U,
          "advanced artwork remains coalesced at 4999ms and never reaches JSON");
    fxe::ManualMillis::SetNowMs(5100U);
    persistence.Update();
    Check(sink.binaryAdmissions == 1U && sink.jsonAdmissions == 0U &&
              sink.pending.size <= Persistence::ADVANCED_SLOT_BYTES,
          "advanced artwork uses only the bounded binary lane at 5000ms");
    auto pendingEdit = AdvancedDocument(8U, 19U);
    fxe::ManualMillis::SetNowMs(5200U);
    Check(persistence.SetAdvancedDocument(pendingEdit, true),
          "newer live artwork may change while an older receipt is pending");
    sink.CompleteSuccess();
    const unsigned readsBeforeCompletion = sink.reads;
    sink.readCostMs = 37U;
    persistence.Update();
    std::printf("[ARTWORK_COMPLETION_LATENCY] injected_read_ms=37 caller_ms=%u reads=%u\n",
                unsigned(fxe::Millis() - 5200U), sink.reads - readsBeforeCompletion);
    Check(sink.reads == readsBeforeCompletion && fxe::Millis() == 5200U,
          "verified worker receipt completes without synchronous flash reads on the input/render caller");
    sink.readCostMs = 0U;
    Check(persistence.IsDirty() && persistence.HasAdvancedDocument() &&
              persistence.AdvancedDocument().Value().sparkleSeed ==
                  pendingEdit.Value().sparkleSeed,
          "verifying older receipt A never overwrites newer live edit B");
    fxe::ManualMillis::SetNowMs(10199U);
    persistence.Update();
    Check(sink.binaryAdmissions == 1U,
          "newer edit B retains its own 4999ms trailing deadline");
    fxe::ManualMillis::SetNowMs(10200U);
    persistence.Update();
    Check(sink.binaryAdmissions == 2U,
          "newer edit B queues at its own 5000ms trailing deadline");
    sink.CompleteSuccess();
    persistence.Update();
    Check(!persistence.IsDirty() &&
              persistence.AdvancedDocument().Value().sparkleSeed ==
                  pendingEdit.Value().sparkleSeed,
          "newer edit B becomes durable after its separate verified receipt");

    Persistence::AdvancedStorage unreadableStorage;
    Persistence unreadable{settings, art};
    unreadable.Configure(sink.Ops());
    unreadable.ConfigureAdvancedStorage(unreadableStorage);
    sink.failCurrentRead = true;
    const auto protectedCurrent = sink.current;
    Check(unreadable.RestoreAdvanced() == AdvancedStatus::BackendUnavailable &&
              unreadable.HasFailure() &&
              !unreadable.SetAdvancedDocument(first, true) &&
              sink.current.size == protectedCurrent.size &&
              sink.current.bytes == protectedCurrent.bytes,
          "unreadable current authority stays blocked and byte-exact instead of becoming replaceable corruption");
    sink.failCurrentRead = false;
    Check(unreadable.RestoreAdvanced() == AdvancedStatus::Valid &&
              unreadable.HasAdvancedDocument() && !unreadable.HasFailure() &&
              unreadable.AdvancedDocument().Value().sparkleSeed ==
                  pendingEdit.Value().sparkleSeed,
          "a transient unreadable current becomes authoritative on a successful retry");

    auto second = AdvancedDocument(8U, 29U);
    fxe::ManualMillis::SetNowMs(11000U);
    Check(persistence.SetAdvancedDocument(second, false),
          "a later advanced document replaces live authority");
    fxe::ManualMillis::SetNowMs(16000U);
    persistence.Update();
    sink.CompleteInterrupted();
    persistence.Update();
    Check(persistence.HasFailure(),
          "interrupted advanced transaction reports a persistence failure");
    Check(sink.jsonAdmissions == 0U,
          "binary write failure never falls back to the 8KiB settings path");

    Persistence::AdvancedStorage restartedStorage;
    Persistence restarted{settings, art};
    restarted.Configure(sink.Ops());
    restarted.ConfigureAdvancedStorage(restartedStorage);
    Check(restarted.RestoreAdvanced() == AdvancedStatus::Recovered &&
              restarted.HasAdvancedDocument() && !restarted.HasFailure() &&
              restarted.AdvancedDocument().Value().sparkleSeed ==
                  pendingEdit.Value().sparkleSeed,
          "restart rejects torn current and atomically recovers last-good artwork");

    sink.current.bytes[Persistence::ADVANCED_BODY_OFFSET] = 99U;
    const size_t futureBodySize =
        static_cast<size_t>(sink.current.bytes[8]) |
        (static_cast<size_t>(sink.current.bytes[9]) << 8U);
    const uint32_t futureBodyCrc = fxe::Crc32(
        sink.current.bytes.data() + Persistence::ADVANCED_BODY_OFFSET,
        futureBodySize);
    for (size_t byte = 0U; byte < 4U; ++byte) {
        sink.current.bytes[12U + byte] =
            static_cast<uint8_t>(futureBodyCrc >> (8U * byte));
    }
    const uint32_t futureEnvelopeCrc = fxe::Crc32(
        sink.current.bytes.data(),
        sink.current.size - Persistence::ADVANCED_ENVELOPE_TRAILER_BYTES);
    for (size_t byte = 0U; byte < 4U; ++byte) {
        sink.current.bytes[sink.current.size -
                           Persistence::ADVANCED_ENVELOPE_TRAILER_BYTES + byte] =
            static_cast<uint8_t>(futureEnvelopeCrc >> (8U * byte));
    }
    Persistence::AdvancedStorage futureStorage;
    Persistence future{settings, art};
    future.Configure(sink.Ops());
    future.ConfigureAdvancedStorage(futureStorage);
    Check(future.RestoreAdvanced() == AdvancedStatus::FutureVersion &&
              !future.HasAdvancedDocument() && future.HasFailure() &&
              !future.SetAdvancedDocument(first, true) &&
              sink.current.bytes[Persistence::ADVANCED_BODY_OFFSET] == 99U,
          "unknown current schema dominates last-good and retains source bytes");

    sink.reportOversizedCurrent = true;
    Persistence::AdvancedStorage oversizedStorage;
    Persistence oversized{settings, art};
    oversized.Configure(sink.Ops());
    oversized.ConfigureAdvancedStorage(oversizedStorage);
    Check(oversized.RestoreAdvanced() == AdvancedStatus::Recovered &&
              !oversized.HasFailure() &&
              oversized.AdvancedDocument().Value().sparkleSeed ==
                  pendingEdit.Value().sparkleSeed,
          "oversized current is rejected before copy and preserves last-good");
    sink.reportOversizedCurrent = false;

    Persistence::AdvancedStorage absentStorage;
    Persistence absent{settings, art};
    const Persistence::Ops staticOnly{nullptr, nullptr, nullptr, nullptr};
    absent.Configure(staticOnly);
    absent.ConfigureAdvancedStorage(absentStorage);
    Check(!absent.IsAdvancedConfigured() &&
              !absent.SetAdvancedDocument(first, true) &&
              !absent.HasAdvancedDocument() &&
              absent.RestoreAdvanced() == AdvancedStatus::BackendUnavailable,
          "backend absence cannot adopt or claim advanced durability");

    Check(Persistence::ADVANCED_ACTIVATION_BODY_COUNT == 3U &&
              sizeof(Persistence::AdvancedStorage) <=
                  Persistence::ADVANCED_ACTIVATION_RAM_LIMIT_BYTES,
          "activation RAM owns three bodies and bounded history overflow");
    Check(Persistence::ADVANCED_SLOT_BYTES <= 20U * 1024U &&
              Persistence::ADVANCED_SLOT_BYTES * 3U <= 60U * 1024U,
          "flash transaction slots retain their separate 60KiB budget");
    std::printf(
        "[LITCHCRAFT_ADVANCED_MEMORY] bodies=%zu activation_bytes=%zu "
        "envelope_bytes=%zu flash_slots_bytes=%zu\n",
        Persistence::ADVANCED_ACTIVATION_BODY_COUNT,
        sizeof(Persistence::AdvancedStorage),
        sizeof(Persistence::AdvancedEnvelope),
        Persistence::ADVANCED_SLOT_BYTES * 3U);
}

void AdvancedAndStaticLifecycleFlushCompose() {
    modules::SettingsStore settings{Config()};
    Art art;
    AdvancedSink sink;
    Persistence::AdvancedStorage storage;
    Persistence persistence{settings, art};
    RemoveFiles();
    Check(settings.Load(), "combined lifecycle lane loads static settings");
    art.Configure(3U, 1000U);
    persistence.Configure(sink.Ops());
    persistence.ConfigureAdvancedStorage(storage);
    Check(persistence.RestoreAdvanced() ==
              Persistence::AdvancedRecordStatus::Missing,
          "combined lifecycle lane starts without advanced artwork");
    auto advanced = AdvancedDocument(8U, 47U);
    Check(persistence.SetAdvancedDocument(advanced, true),
          "combined lifecycle lane adopts advanced artwork");
    settings.Doc()["OTHER_STATE"] = 47U;
    persistence.RequestFlush();
    persistence.Update();
    Check(sink.binaryAdmissions == 1U && sink.jsonAdmissions == 0U &&
              persistence.FlushState() == Persistence::FlushStatus::Pending,
          "lifecycle flush queues advanced artwork before static settings");
    sink.CompleteSuccess();
    persistence.Update();
    Check(sink.binaryAdmissions == 1U && sink.jsonAdmissions == 1U &&
              persistence.FlushState() == Persistence::FlushStatus::Pending,
          "verified advanced receipt continues the same lifecycle flush into static settings");
    sink.CompleteSuccess();
    persistence.Update();
    Check(persistence.FlushState() == Persistence::FlushStatus::Saved &&
              !persistence.IsDirty() && sink.frozenJson.find("OTHER_STATE") !=
                  std::string::npos,
          "one lifecycle flush becomes saved only after advanced and static snapshots succeed");
}

void DirtyAdvancedExitDefersEncodingAndMutexWait() {
    RemoveFiles();
    modules::SettingsStore settings{Config()};
    Check(settings.Load(), "deferred encoding fixture loads settings");
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    struct Presentation {
        uint32_t acquired = 0U;
        uint32_t released = 0U;
    } presentation;
    coordinator.SetLitchcraftPresentationAdmission({&presentation,
        [](void* context, Art::PresentationAccess access) {
            Check(access == Art::PresentationAccess::Control, "Exit maintenance uses control admission when needed");
            return ++static_cast<Presentation*>(context)->acquired;
        },
        [](void* context, uint32_t lease) {
            auto& owner = *static_cast<Presentation*>(context);
            Check(lease == owner.acquired && lease == owner.released + 1U,
                  "Exit maintenance releases exactly the acquired presentation");
            owner.released = lease;
        }});
    Persistence::AdvancedStorage storage;
    DeferredEncodingSink sink;
    sink.stateMachine = &stateMachine;
    coordinator.ArtPersistence().Configure(sink.Ops());
    coordinator.ArtPersistence().ConfigureAdvancedStorage(storage);
    coordinator.Litchcraft().Configure(3U, 1800U);
    coordinator.Litchcraft().BeginConnection();
    Send(coordinator.Litchcraft(), Art::Op::Enter, 1U);
    Drain(coordinator.Litchcraft());
    Check(coordinator.ArtPersistence().SetAdvancedDocument(
              AdvancedDocument(8U, 93U), true),
          "deferred encoding fixture adopts dirty advanced authority");
    const uint32_t before = fxe::Millis();
    const auto exit = Request(coordinator.Litchcraft(), Art::Op::Exit, 2U);
    coordinator.HandleLitchcraftPacket(exit.bytes.data(), exit.size);
    Check(fxe::Millis() == before && !coordinator.Litchcraft().IsActive() &&
              stateMachine.IsPlacementModeActive() && sink.retained == 0U &&
              sink.documentAdmissions == 0U,
          "dirty advanced Exit returns in-cycle while worker mutex is held and Setup owns render");
    const uint32_t exitPublications = presentation.released;
    sink.workerMutexHeld = false;
    coordinator.UpdateLitchcraft();
    Check(sink.documentAdmissions == 1U && sink.retained != 0U &&
              sink.envelope != nullptr && sink.envelope->size == 0U &&
              stateMachine.IsPlacementModeActive(),
          "next-cycle worker admission freezes authority without caller-side encoding");
    sink.CompleteAdvanced();
    coordinator.UpdateLitchcraft();
    Check(sink.staticAdmissions == 1U && stateMachine.IsPlacementModeActive(),
          "background advanced completion continues into inactive mode save");
    sink.CompleteStatic();
    coordinator.UpdateLitchcraft();
    Check(!coordinator.ArtPersistence().IsDirty() &&
              coordinator.ArtPersistence().FlushState() ==
                  Persistence::FlushStatus::Saved &&
              stateMachine.IsPlacementModeActive(),
          "advanced document and inactive mode become durable without reversing accepted Exit");
    Check(presentation.acquired == exitPublications && presentation.released == exitPublications,
          "inactive Exit persistence completes without acquiring or publishing another presentation");
    modules::litchcraft_animation::EncodedDocument encoded;
    Check(modules::litchcraft_animation::EncodeDocument(AdvancedDocument(8U, 93U), encoded),
          "inactive maintenance prepares a canonical staged transfer");
    Art::BulkPacket begin;
    begin.bytes[0U] = 0x4dU; begin.bytes[1U] = 2U; begin.bytes[2U] = 9U; begin.size = 3U;
    V2Fixture::Put(begin, 0U, 4U); V2Fixture::Put(begin, 3U, 2U);
    V2Fixture::Put(begin, coordinator.ArtPersistence().AdvancedGeneration(), 4U);
    V2Fixture::Put(begin, encoded.size, 2U); V2Fixture::Put(begin, encoded.crc32, 4U);
    coordinator.HandleLitchcraftPacket(begin.bytes.data(), begin.size, false);
    Check(coordinator.ArtPersistence().AdvancedTransportStorage() != nullptr,
          "inactive storage transfer retains its bounded transport lease");
    fxe::ManualMillis::AdvanceMs(6000U);
    coordinator.UpdateLitchcraft();
    Check(coordinator.ArtPersistence().AdvancedTransportStorage() == nullptr &&
              presentation.acquired == exitPublications && presentation.released == exitPublications,
          "inactive coordinator expires transport staging without presentation admission or publication");
    std::puts("[ADVANCED_EXIT_LATENCY] mutex_wait_ms=0 encode_on_caller=0 next_render=Setup background_save=Saved");
}

void AdvancedBackpressureRapidEditsExitAndReboot() {
    V2Fixture f;
    f.sink.admission = Admission::Deferred;
    fxe::ManualMillis::SetNowMs(5000U);
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 0U && f.persistence.IsDirty() &&
              !f.persistence.HasFailure(),
          "saturated shared worker retains dirty advanced artwork without a false failure");
    for (uint8_t edit = 1U; edit <= 60U; ++edit) {
        fxe::ManualMillis::SetNowMs(5000U + edit);
        f.Ack(V2Edit(f, 3U, {0, edit, 0, 3, 1, edit, 8, 9, 1}));
        f.persistence.Update();
    }
    const auto latest = f.persistence.AdvancedDocument();
    Check(f.sink.binaryAdmissions == 0U && f.sink.reads == 0U &&
              latest.Value().frames[0].emitters[4].color.r == 60U,
          "rapid production BLE edits acknowledge immediately and coalesce despite storage backpressure");
    Send(f.art, Art::Op::Exit, 1000U);
    Check(HasAck(Drain(f.art), 1000U, Art::Status::Ok),
          "Exit acknowledges mode immediately despite dirty artwork and saturated storage");
    f.persistence.Update();
    Check(!f.art.IsActive() && f.persistence.IsDirty() && f.sink.retained == 0U,
          "exit while saturated preserves dirty artwork without blocking Play");
    f.sink.admission = Admission::Accepted;
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == 1U && f.sink.jsonAdmissions == 0U,
          "released backpressure admits only the newest full document before mode exit");
    f.sink.CompleteInterrupted();
    f.persistence.Update();
    Check(!f.art.IsActive() && f.persistence.HasFailure() &&
              f.persistence.IsDirty() && Drain(f.art).empty(),
          "failed save retains dirty artwork and diagnostics without reversing acknowledged Play");
    f.persistence.RequestFlush();
    f.persistence.Update();
    f.sink.CompleteSuccess();
    f.persistence.Update();
    Check(!f.art.IsActive() && f.sink.jsonAdmissions == 1U,
          "background flush continues to inactive settings after document verification");
    f.sink.CompleteSuccess();
    f.persistence.Update();
    Check(!f.art.IsActive() && !f.persistence.IsDirty() && f.sink.reads == 0U &&
              f.persistence.FlushState() == Persistence::FlushStatus::Saved,
          "explicit lifecycle flush proves durability without another mode change or cooperative flash read");
    const auto durableEightFrames = f.sink.current;
    const auto binarySavesBeforeSettings = f.sink.binaryAdmissions;
    for (uint32_t unrelated = 1U; unrelated <= 2U; ++unrelated) {
        f.settings.Doc()["UNRELATED_SETTING"] = unrelated;
        f.persistence.RequestFlush(); f.persistence.Update();
        Check(!f.sink.pendingAdvanced,
              "unrelated settings flush after acknowledged eight-frame Exit never submits binary artwork");
        f.sink.CompleteSuccess(); f.persistence.Update();
        Check(f.sink.current.size == durableEightFrames.size &&
                  f.sink.current.bytes == durableEightFrames.bytes &&
                  f.sink.binaryAdmissions == binarySavesBeforeSettings,
              "unrelated successful settings saves retain the exact durable eight-frame envelope");
    }
    modules::SettingsStore rebootSettings{Config()};
    Check(!deserializeJson(rebootSettings.Doc(), f.sink.frozenJson),
          "reboot receives the final saved settings snapshot");
    Art rebootArt;
    rebootArt.Configure(3U, 2000U);
    Persistence::AdvancedStorage rebootStorage;
    Persistence reboot{rebootSettings, rebootArt};
    reboot.Configure(f.sink.Ops());
    reboot.ConfigureAdvancedStorage(rebootStorage);
    Check(reboot.Restore() == modules::LitchcraftRecordStatus::Valid &&
              reboot.RestoreAdvanced() == Persistence::AdvancedRecordStatus::Valid &&
              !rebootArt.IsActive() &&
              modules::litchcraft_animation::DocumentsEqual(
                  reboot.AdvancedDocument().Value(), latest.Value()),
          "reboot restores inactive mode and the exact newest eight-frame artwork after saturation and retry");
}

void AdvancedExitReentrySnapshotDuringModeSave() {
    V2Fixture f;
    f.persistence.RequestFlush();
    f.persistence.Update();
    f.sink.CompleteSuccess(); f.persistence.Update();
    f.sink.CompleteSuccess(); f.persistence.Update();
    const auto binarySaves = f.sink.binaryAdmissions;
    const auto oldExit = Request(f.art, Art::Op::Exit, 1000U);
    f.art.HandlePacket(oldExit.bytes.data(), oldExit.size, true, 0U);
    f.persistence.Update();
    Check(f.sink.binaryAdmissions == binarySaves && !f.sink.pendingAdvanced,
          "clean advanced Exit saves only mode and never rewrites unchanged artwork");
    Check(HasAck(Drain(f.art), 1000U, Art::Status::Ok) &&
              !f.art.IsActive() && f.persistence.IsSaving(),
          "Exit ACK precedes the retained inactive-mode receipt");
    const auto exitedSession = f.art.SessionId();
    Send(f.art, Art::Op::Enter, 1001U);
    Drain(f.art);
    const auto enteredSession = f.art.SessionId();
    Check(f.art.IsActive() && enteredSession != exitedSession,
          "immediate reentry receives new authority while the old inactive save remains pending");
    f.sink.CompleteSuccess(); f.persistence.Update();
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 100U);
    f.persistence.Update();
    Check(f.persistence.IsSaving() && !f.sink.pendingAdvanced,
          "reentry's independent 100ms mode save occupies only the static lane");
    f.art.HandlePacket(oldExit.bytes.data(), oldExit.size, true, 0U);
    Drain(f.art);
    Check(f.art.IsActive() && f.art.SessionId() == enteredSession,
          "stale Exit request cannot retire the newer reentry authority");
    f.next = 1002U;
    f.Ack(f.Request(1U));
    f.Ack(f.Request(8U));
    Check(f.persistence.AdvancedTransportStorage() != nullptr,
          "post-Exit Enter/Inspect/Snapshot is not Busy behind unrelated static mode save");
    f.sink.CompleteSuccess(); f.persistence.Update();
    Check(f.art.IsActive() && f.art.SessionId() == enteredSession,
          "delayed mode-save completion preserves new active session and snapshot");
    if (f.persistence.AdvancedTransportStorage() != nullptr) {
        Check(f.Pop().bytes[2] == 0x84U, "post-reentry snapshot begins while static receipt completes");
        std::vector<uint8_t> received;
        for (size_t packetIndex = 0U; packetIndex < 100U; ++packetIndex) {
            const auto packet = f.Pop();
            if (packet.bytes[2] == 0x86U) break;
            Check(packet.bytes[2] == 0x85U &&
                      V2Fixture::Read(packet.bytes.data() + 13U, 2U) == received.size(),
                  "post-reentry snapshot preserves contiguous canonical bytes");
            received.insert(received.end(), packet.bytes.begin() + 15U,
                            packet.bytes.begin() + packet.size);
        }
        const auto expected = f.Encoded(11U);
        Check(received.size() == expected.size &&
                  std::equal(received.begin(), received.end(), expected.bytes.begin()) &&
                  f.persistence.AdvancedTransportStorage() == nullptr,
              "post-ACK snapshot completes with all eight frames unchanged by mode-save completion");
    }
    f.art.EndConnection();
    f.persistence.Update();
    Check(f.art.IsActive() && f.art.SessionId() == enteredSession &&
              f.sink.binaryAdmissions == binarySaves,
          "disconnect after immediate reentry preserves artwork ownership without a stale save");
}

void AdvancedMutationAndDurabilityDiagnostics() {
    V2Fixture f;
    f.persistence.RequestFlush(); f.persistence.Update();
    f.sink.CompleteSuccess(); f.persistence.Update();
    f.sink.CompleteSuccess(); f.persistence.Update();
    const auto oldIdentity = f.persistence.GetAdvancedDiagnostics();
    Check(oldIdentity.savedFrames == 8U && oldIdentity.savedReceipt != 0U &&
              f.art.LastAdvancedMutation().count == 0U,
          "durable eight-frame baseline is distinct from accepted V2 mutations");
    modules::litchcraft_animation::EncodedDocument oneFrame;
    modules::litchcraft_animation::EncodeDocument(AdvancedDocument(1U, 31U), oneFrame);
    const auto begin = f.Begin(oneFrame);
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    f.Ack(begin); f.Upload(oneFrame, transfer, 244U);
    Check(f.art.LastAdvancedMutation().count == 0U,
          "accepted transfer staging is not counted as a document mutation");
    const auto commit = f.TransferOp(11U, transfer);
    f.Ack(commit);
    f.Ack(commit);
    f.Ack(f.Request(1U));
    const auto mutation = f.art.LastAdvancedMutation();
    Check(mutation.count == 1U && mutation.op == 11U && mutation.frames == 1U &&
              mutation.revision == f.persistence.AdvancedGeneration(),
          "accepted eight-to-one replacement records one mutation; replay and Inspect do not inflate it");
    f.Ack(V2FrameEdit(f, 1U, 0U), Art::Status::ResourceLimit);
    f.Ack(V2FrameEdit(f, 3U, 0U, 0U, 100U));
    Check(f.art.LastAdvancedMutation().count == 1U,
          "rejected delete and accepted no-op duration do not replace mutation evidence");
    f.persistence.RequestFlush(); f.persistence.Update();
    auto identity = f.persistence.GetAdvancedDiagnostics();
    Check(identity.frames == 1U && identity.submittedFrames == 1U && identity.savedFrames == 8U &&
              identity.submittedReceipt != identity.savedReceipt &&
              identity.savedCrc == oldIdentity.savedCrc &&
              identity.submittedCrc == V2Fixture::Read(
                  f.sink.pending.bytes.data() + f.sink.pending.size - 4U, 4U),
          "pending replacement reports distinct live/submitted and last-verified envelope identities");
    f.sink.CompleteSuccess(); f.persistence.Update();
    identity = f.persistence.GetAdvancedDiagnostics();
    Check(identity.savedFrames == 1U && identity.savedCrc == identity.submittedCrc &&
              identity.savedReceipt == identity.submittedReceipt,
          "only the verified binary receipt advances saved envelope identity");
    f.sink.CompleteSuccess(); f.persistence.Update();
    Check(f.persistence.GetAdvancedDiagnostics().savedCrc == identity.savedCrc,
          "following static settings receipt cannot change saved artwork identity");
    f.art.EndConnection(); f.art.BeginConnection();
    Check(f.art.LastAdvancedMutation().count == 1U,
          "accepted mutation evidence survives transport disconnect and reconnect");
    Art rebootArt;
    Persistence::AdvancedStorage rebootBody;
    Persistence reboot(f.settings, rebootArt);
    reboot.Configure(f.sink.Ops()); reboot.ConfigureAdvancedStorage(rebootBody);
    Check(reboot.RestoreAdvanced() == Persistence::AdvancedRecordStatus::Valid &&
              reboot.GetAdvancedDiagnostics().savedCrc == identity.savedCrc &&
              reboot.GetAdvancedDiagnostics().savedFrames == 1U &&
              reboot.GetAdvancedDiagnostics().revision == 0U &&
              reboot.GetAdvancedDiagnostics().savedReceipt == 0U &&
              rebootArt.LastAdvancedMutation().count == 0U,
          "boot retains validated envelope identity while runtime mutation and receipt counters reset");
    std::puts("[ARTWORK_MUTATION_POSITIVE] accepted_op=11 mutations=1 frames=1 verified=1 restored=1");
    V2Fixture frameEdit;
    frameEdit.Ack(V2FrameEdit(frameEdit, 1U, 7U));
    Check(frameEdit.art.LastAdvancedMutation().op == 5U &&
              frameEdit.art.LastAdvancedMutation().detail == 1U &&
              frameEdit.art.LastAdvancedMutation().frames == 7U,
          "frame edit diagnostics distinguish accepted Delete from Add or duration changes");
}
void PendingExitFailureAndRecovery() {
    Fixture f;
    f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
    f.Tick(100U); f.Complete(100U);
    f.Tick(200U); Send(f.art, Art::Op::Paint, 2U, {1, 0, 3, 0}); Drain(f.art);
    Send(f.art, Art::Op::Exit, 3U); f.persistence.Update();
    Send(f.art, Art::Op::Exit, 3U); f.persistence.Update();
    Check(!f.art.IsActive() && f.sink.admissions == 2U &&
              HasAck(Drain(f.art), 3U, Art::Status::Ok),
          "Exit and its duplicate acknowledge immediately while one background receipt is pending");
    Send(f.art, Art::Op::Inspect, 10U); Drain(f.art);
    Send(f.art, Art::Op::Exit, 3U);
    Check(HasAck(Drain(f.art), 3U, Art::Status::Ok),
          "same-ID Exit receipt remains idempotent after intervening higher request IDs");
    f.Complete(201U, false);
    Check(!f.art.IsActive() && f.persistence.IsDirty() &&
              f.persistence.FlushState() == Persistence::FlushStatus::Failed &&
              Drain(f.art).empty(),
          "failed background save retains dirty artwork without reversing mode or contradicting its receipt");
    modules::LitchcraftRecord source;
    Check(modules::ReadLitchcraftRecord(f.settings.Doc(), source) == modules::LitchcraftRecordStatus::Valid && source.active,
          "failure restores durable active source record before another settings writer can run");
    const auto attempts = f.sink.admissions;
    f.Tick(10000U); Check(f.sink.admissions == attempts, "terminal failure does not hot-loop flash retries");
    f.persistence.RequestFlush(); f.persistence.Update();
    f.art.EndConnection(); f.art.BeginConnection();
    f.Complete(10001U);
    const auto replies = Drain(f.art);
    Check(!f.art.IsActive() && !ReadSaved().active && ReadSaved().canvas[0].r != 0U &&
              !HasAck(replies, 11U, Art::Status::Ok), "accepted Exit survives reconnect but never replies using a retired request ID");
    Check(replies.empty(), "background save completion cannot publish stale mode notifications after reconnect");
}
void AdmissionAndWholeDocumentFlush() {
    Fixture f;
    f.Tick(0U); f.sink.admission = Admission::Deferred;
    f.persistence.RequestFlush(); f.persistence.Update(); f.persistence.RequestFlush();
    Check(f.persistence.FlushState() == Persistence::FlushStatus::Pending && f.sink.admissions == 0U,
          "backpressure keeps one pending flush instead of claiming persistence");
    f.sink.admission = Admission::Accepted; f.persistence.Update();
    f.settings.Doc()["OTHER_STATE"] = 7U;
    f.persistence.RequestFlush(); f.Complete(1U);
    Check(f.sink.admissions == 1U, "same outstanding flush is idempotent");
    f.persistence.RequestFlush(); f.persistence.Update(); f.Complete(2U);
    modules::SettingsStore reload{Config()}; Check(reload.Load(), "reload whole document");
    Check(reload.Doc()["OTHER_STATE"].as<unsigned>() == 7U, "explicit clean-art lifecycle flush saves current non-art state");
    f.sink.admission = Admission::Rejected;
    f.persistence.RequestFlush(); f.persistence.Update();
    Check(f.persistence.FlushState() == Persistence::FlushStatus::Failed && f.persistence.HasFailure(),
          "unavailable or rejected storage fails truthfully instead of pending forever");
}
void FlushBehindAutosaveAndFailureRecovery() {
    Fixture f;
    f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
    f.Tick(100U); f.Complete(100U);
    f.Tick(200U); Send(f.art, Art::Op::Paint, 2U, {1, 0, 3, 0});
    f.Tick(5200U);
    f.settings.Doc()["OTHER_STATE"] = 33U;
    f.persistence.RequestFlush();
    f.Complete(5200U);
    Check(f.sink.admissions == 3U && f.persistence.FlushState() == Persistence::FlushStatus::Pending,
          "lifecycle request behind an older autosave queues a fresh whole-document snapshot");
    f.Complete(5201U);
    modules::SettingsStore reader{Config()}; Check(reader.Load(), "load post-autosave lifecycle state");
    Check(reader.Doc()["OTHER_STATE"].as<unsigned>() == 33U && f.persistence.FlushState() == Persistence::FlushStatus::Saved,
          "older autosave receipt cannot acknowledge newer prepared game state");
    f.Tick(5300U); Send(f.art, Art::Op::Paint, 3U, {2, 0, 3, 1});
    f.Tick(10300U); f.Complete(10300U, false);
    f.Tick(10400U); Send(f.art, Art::Op::Paint, 4U, {3, 0, 3, 2});
    f.Tick(15400U); f.Complete(15400U);
    Check(!f.persistence.HasFailure() && !f.persistence.IsDirty() && ReadSaved().canvas[8].r != 0U,
          "a fresh artwork mutation retries and clears a prior terminal save failure after its own debounce");
}
void ActualMutationSourcesAndClockWrap() {
    Fixture f;
    f.Tick(0U); Send(f.art, Art::Op::Enter, 1U);
    f.Tick(100U); f.Complete(100U);
    f.Tick(200U); f.art.ObserveOccupancy(1U);
    const auto first = f.art.CanvasGeneration();
    f.Tick(250U); Send(f.art, Art::Op::Tool, 2U, {0, 15, 3, 5, 7});
    Check(f.art.CanvasGeneration() != first && f.art.CanvasChangedAtMs() == 250U,
          "tool selection repainting a resting footprint is an actual artwork mutation");
    f.art.ObserveOccupancy(0U);
    f.Tick(5249U); Check(f.sink.admissions == 1U, "resting-footprint repaint restarts idle deadline");
    f.Tick(5250U); f.Complete(5250U);
    const auto generation = f.art.CanvasGeneration();
    const auto canvas = f.art.Artwork();
    for (uint16_t offset = 0U; offset < Art::ELEMENT_COUNT;) {
        const uint8_t count = std::min<uint16_t>(77U, Art::ELEMENT_COUNT - offset);
        std::vector<uint8_t> packet{Art::MAGIC, Art::BULK_REPLACE};
        for (unsigned i = 0U; i < 4U; ++i) packet.push_back(f.art.SessionId() >> (8U * i));
        packet.insert(packet.end(), {3U, 0U, uint8_t(offset), uint8_t(offset >> 8U),
                                     uint8_t(offset + count == Art::ELEMENT_COUNT), count});
        for (unsigned i = offset; i < offset + count; ++i) {
            packet.insert(packet.end(), {canvas[i].r, canvas[i].g, canvas[i].b});
        }
        f.art.HandleBulkPacket(packet.data(), packet.size(), true, 0U);
        offset += count;
    }
    Check(f.art.CanvasGeneration() == generation && !f.persistence.IsDirty(),
          "confirmed identical full replacement may ACK without dirtying stored RGB");
    Send(f.art, Art::Op::Undo, 4U);
    Check(f.art.CanvasGeneration() != generation, "Undo is a content mutation when its RGB changes");
    const auto undoGeneration = f.art.CanvasGeneration();
    Send(f.art, Art::Op::Redo, 5U);
    Check(f.art.CanvasGeneration() != undoGeneration && Equal(f.art.Artwork(), canvas),
          "Redo restores content and restarts artwork persistence deadline");
    f.Tick(10250U); f.Complete(10250U);
    f.Tick(UINT32_MAX - 200U); Send(f.art, Art::Op::Paint, 6U, {4, 0, 3, 1});
    const auto beforeWrap = f.sink.admissions;
    f.Tick(4798U); Check(f.sink.admissions == beforeWrap, "4999ms remains not due across millis wrap");
    f.Tick(4799U); Check(f.sink.admissions == beforeWrap + 1U, "5000ms becomes due across millis wrap");
    f.Complete(4799U);
}
void CodecRecoveryAndBoot() {
    RemoveFiles();
    modules::SettingsStore writer{Config()};
    Check(writer.Load(), "codec fixture loads");
    modules::LitchcraftRecord original;
    original.active = true;
    for (size_t i = 0U; i < original.canvas.size(); ++i) original.canvas[i] = {uint8_t(i), uint8_t(i * 13U), uint8_t(255U - i)};
    modules::LitchcraftEncodedCanvas encoded;
    modules::LitchcraftEncodedQuickPalette encodedPalette;
    Check(modules::WriteLitchcraftRecord(writer.Doc(), original, encoded,
                                         encodedPalette) && writer.SaveNow(), "save complete four-emitter record");
    const auto first = original;
    original.canvas[0].r = 77U;
    Check(modules::WriteLitchcraftRecord(writer.Doc(), original, encoded,
                                         encodedPalette) && writer.SaveNow(), "save newer record with last-good backup");
    { std::ofstream torn(std::string(PATH) + ".tmp"); torn << "{\"LITCHCRAFT_ART_V1\":"; }
    Check(Equal(ReadSaved().canvas, original.canvas), "torn temp cannot replace last validated primary");
    writer.Doc()[modules::LITCHCRAFT_ART_KEY][4] = 0U;
    writer.Doc()["PRESERVED_OTHER"] = 99U;
    { std::ofstream corrupt(PATH); serializeJson(writer.Doc(), corrupt); }
    Check(Equal(ReadSaved().canvas, first.canvas), "semantic CRC corruption recovers validated backup art");
    writer.Doc()[modules::LITCHCRAFT_ART_KEY][0] = 3U;
    { std::ofstream future(PATH); serializeJson(writer.Doc(), future); }
    Check(Equal(ReadSaved().canvas, first.canvas), "future art schema recovers supported last-good artwork");
    modules::SettingsStore recovered{Config()}; Check(recovered.Load(), "load scoped recovery");
    Check(recovered.Doc()["PRESERVED_OTHER"].as<unsigned>() == 99U, "art recovery preserves other primary state");
    for (const uint8_t hardware : {1U, 2U, 3U}) {
        Art art; art.Configure(hardware, 9000U + hardware);
        Persistence owner(recovered, art);
        Check(owner.Restore() == modules::LitchcraftRecordStatus::Valid && art.IsActive() && art.SessionId() != 0U &&
                  Equal(art.Artwork(), first.canvas), "no-phone boot restores raw independent emitters and active mode on every topology");
        art.ObserveOccupancy(1ULL << 9U);
        Check(Equal(art.Artwork(), first.canvas) && art.IsWaitingForLift(), "first boot Hall report blocks resting pieces without painting");
        art.ObserveOccupancy(0U); art.ObserveOccupancy(1ULL << 10U);
        Check(art.CanvasGeneration() != 0U, "a deliberate later brush paints after boot baseline and lift");
    }
    original.active = false;
    Check(modules::WriteLitchcraftRecord(recovered.Doc(), original, encoded,
                                         encodedPalette), "encode inactive art");
    Art inactive; inactive.Configure(3U, 10000U); Persistence inactiveOwner(recovered, inactive);
    Check(inactiveOwner.Restore() == modules::LitchcraftRecordStatus::Valid && !inactive.IsActive() && inactive.SessionId() != 0U,
          "inactive restored artwork also has a fresh nonzero boot session");
}
void StoreOnlySettingsFlushDoesNotWriteOldArtwork() {
    RemoveFiles();
    modules::SettingsStore settings{Config()};
    Check(settings.Load(), "store-only settings fixture loads");
    settings.Doc()["PRESERVED_OTHER"] = 99U;
    settings.Doc()[modules::LITCHCRAFT_ART_KEY].to<JsonArray>();
    Art art;
    art.Configure(3U, 9000U);
    FileSink sink;
    Persistence persistence{settings, art};
    persistence.ConfigureStoreOnly(sink.Ops());
    Check(!settings.Doc().containsKey(modules::LITCHCRAFT_ART_KEY),
          "store-only boot drops retained old artwork from live settings");
    persistence.RequestFlush();
    persistence.Update();
    Check(sink.retained != 0U, "store-only lifecycle flush admits settings snapshot");
    if (sink.retained == 0U) return;
    sink.Complete();
    persistence.Update();
    Check(persistence.FlushState() == Persistence::FlushStatus::Saved,
          "store-only settings snapshot reaches durable success");
    Check(!sink.writer.Doc().containsKey(modules::LITCHCRAFT_ART_KEY),
          "store-only settings snapshot excludes retired artwork schema");
    Check(sink.writer.Doc()["PRESERVED_OTHER"].as<unsigned>() == 99U,
          "store-only settings snapshot retains unrelated settings");
    Send(art, Art::Op::Enter, 1U);
    persistence.Update();
    Check(!persistence.IsDirty() && sink.admissions == 1U,
          "store-owned mode does not queue an old artwork save");
    sink.admission = Admission::Deferred;
    persistence.RequestFlush();
    persistence.Update();
    Check(persistence.FlushState() == Persistence::FlushStatus::Pending &&
              sink.admissions == 1U,
          "deferred settings admission keeps lifecycle flush pending");
    sink.admission = Admission::Accepted;
    persistence.Update();
    Check(sink.retained != 0U, "deferred settings admission retries");
    if (sink.retained == 0U) return;
    sink.Complete(false);
    persistence.Update();
    Check(persistence.FlushState() == Persistence::FlushStatus::Failed,
          "failed settings receipt cannot pass lifecycle flush");
    persistence.RequestFlush();
    persistence.Update();
    Check(sink.retained != 0U, "explicit retry admits settings after failure");
    if (sink.retained == 0U) return;
    sink.Complete();
    persistence.Update();
    Check(persistence.FlushState() == Persistence::FlushStatus::Saved &&
              !sink.writer.Doc().containsKey(modules::LITCHCRAFT_ART_KEY),
          "retried settings flush succeeds without old artwork");
}
void VirtualPersistenceJourney() {
    RemoveFiles();
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    uint32_t firstSession = 0U;
    Art::Canvas savedCanvas{};
    auto checkpoint = [](runtime::VirtualLitchSession& session, const char* stage, const uint64_t occupancy, const char* input,
                         const char* result, const char* next) {
        const auto pixel = session.SemanticFrame().board[9U][0U];
        std::printf("[LITCHCRAFT_DURABLE_JOURNEY] stage=%s mode=%s session=%lu revision=%lu saving=%u waiting_for_lift=%u "
                    "clock_running=%u square9=%u,%u,%u display=\"%.*s\" occupancy=%llx input=%s result=%s "
                    "next=%s cancellation_expiry=n/a\n", stage,
                    session.Litchcraft().IsActive() ? "Litchcraft" : (session.PlacementModeActive() ? "Setup" : "Gameplay"),
                    static_cast<unsigned long>(session.Litchcraft().SessionId()),
                    static_cast<unsigned long>(session.Litchcraft().Revision()), session.ArtPersistence().IsSaving(),
                    session.Litchcraft().IsWaitingForLift(), session.GameClock()->IsRunning(),
                    pixel.r, pixel.g, pixel.b, int(session.DisplayText().size()), session.DisplayText().data(),
                    static_cast<unsigned long long>(occupancy), input, result, next);
    };
    {
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession session(config, engine, PATH);
        FileSink sink;
        session.ArtPersistence().Configure(sink.Ops());
        session.SettingsForPersistenceTest().SetAsyncSaveAdmissionCallback(
            [](const JsonDocument&) { return Admission::Deferred; });
        session.ObserveBootHallMask(0U); session.Advance(3000U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true);
        session.Advance(30U);
        session.Advance(1470U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false);
        session.Advance(500U);
        session.Advance(100U);
        Check(session.Litchcraft().IsActive(), "durable journey enters through production physical hold");
        sink.Complete(); session.Advance(0U);
        session.ObserveHallMask(UINT64_C(1) << 9U); session.Advance(50U);
        session.ObserveHallMask(0U); session.Advance(50U);
        session.Advance(5000U); sink.Complete(); session.Advance(0U);
        firstSession = session.Litchcraft().SessionId();
        savedCanvas = session.Litchcraft().Artwork();
        Check(savedCanvas[36].r != 0U && Equal(ReadSaved().canvas, savedCanvas) && ReadSaved().active,
              "physical stroke and mode reach the same production file-backed persistence owner");
        Check(!session.GameClock()->IsRunning() && session.UndoHistoryMoveCount() == 0U,
              "physical painting never changes chess clock or move history");
        checkpoint(session, "saved-before-power-loss", 0U, "physical-hold+Hall9-lift", "accepted", "power-cycle-without-phone");
    }
    config.initial_time_ms = 100000U;
    {
        test_support::BoundedHintEngine engine;
        runtime::VirtualLitchSession session(config, engine, PATH);
        FileSink sink;
        session.ArtPersistence().Configure(sink.Ops());
        session.SettingsForPersistenceTest().SetAsyncSaveAdmissionCallback(
            [](const JsonDocument&) { return Admission::Deferred; });
        Check(session.Litchcraft().IsActive() && session.Litchcraft().SessionId() != firstSession &&
                  Equal(session.Litchcraft().Artwork(), savedCanvas), "no-phone VirtualLitch recreation restores mode and canvas with fresh session");
        session.ObserveHallMask(UINT64_C(1) << 9U); session.Advance(50U);
        Check(session.Litchcraft().IsWaitingForLift() && Equal(session.Litchcraft().Artwork(), savedCanvas),
              "resting piece at reboot receives lift cue without becoming a new brush");
        checkpoint(session, "restored-with-resting-piece", 1ULL << 9U, "boot-Hall9", "blocked-resting-piece", "lift-piece-before-painting");
        session.ObserveHallMask(0U); session.Advance(50U);
        session.ObserveHallMask(UINT64_C(1) << 10U); session.Advance(50U);
        session.ObserveHallMask(0U); session.Advance(50U);
        session.Litchcraft().BeginConnection();
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true);
        session.Advance(30U);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false);
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false);
        session.Advance(30U);
        Drain(session.Litchcraft());
        const auto exit = Request(session.Litchcraft(), Art::Op::Exit, 1U);
        session.SendLitchcraftPacket(exit.bytes.data(), exit.size);
        Check(!session.Litchcraft().IsActive() && session.ArtPersistence().IsSaving(),
              "physical Exit releases mode immediately while storage owns the save");
        Drain(session.Litchcraft());
        session.Advance(20U);
        const auto pendingPixel = session.SemanticFrame().board[9U][0U];
        Check(pendingPixel.r != savedCanvas[36].r || pendingPixel.g != savedCanvas[36].g ||
                  pendingPixel.b != savedCanvas[36].b,
              "accepted Exit immediately replaces painting with Setup guidance while storage saves");
        checkpoint(session, "play-while-saving", 0U, "X+O-release", "accepted", "place-chess-pieces-or-enter-painting");
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::Press, false);
        session.Advance(50U);
        sink.Complete(false); session.Advance(0U);
        Check(!session.Litchcraft().IsActive() && session.ArtPersistence().HasFailure() &&
                  session.ArtPersistence().IsDirty(),
              "storage failure preserves Play and dirty artwork with truthful diagnostics");
        const auto failedPixel = session.SemanticFrame().board[9U][0U];
        Check(failedPixel.r != savedCanvas[36].r || failedPixel.g != savedCanvas[36].g ||
                  failedPixel.b != savedCanvas[36].b,
              "save failure cannot put the canvas back over Setup guidance");
        checkpoint(session, "background-save-failed", 0U, "receipt-failed", "Play-preserved", "continue-playing");
        session.ArtPersistence().RequestFlush(); session.Advance(0U);
        sink.Complete(); session.Advance(0U);
        Check(!session.Litchcraft().IsActive() && !ReadSaved().active && ReadSaved().canvas[40].r != 0U &&
                  session.ArtPersistence().FlushState() == Persistence::FlushStatus::Saved,
              "explicit flush saves latest artwork and inactive mode without another Exit");
        Check(!session.GameClock()->IsRunning() && session.UndoHistoryMoveCount() == 0U,
              "reboot and exit preserve chess clock and history invariants");
        const auto& missing = session.SemanticFrame().board[9U];
        Check(session.PlacementModeActive() &&
                  (missing[0U].r != 0U || missing[0U].g != 0U || missing[0U].b != 0U) &&
                  missing[1U].r == 0U && missing[1U].g == 0U && missing[1U].b == 0U &&
                  missing[2U].r == missing[0U].r && missing[2U].g == missing[0U].g &&
                  missing[2U].b == missing[0U].b,
              "durable exit reaches production Setup with owner-colored ordinary sides and dark undetected center");
        checkpoint(session, "exit-durable", 0U, "flush+receipt-saved", "saved", "place-chess-pieces-or-enter-painting");
    }
}
} // namespace

void VirtualAtomicCanvasJourney() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    AdvancedSink sink;
    Persistence::AdvancedStorage storage;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    auto& persistence = session.ArtPersistence();
    persistence.Configure(sink.Ops());
    persistence.ConfigureAdvancedStorage(storage);
    art.BeginConnection();
    const auto enter = Request(art, Art::Op::Enter, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    session.Advance(1U);
    Drain(art);
    // The existing validated persistence fixture supplies starting artwork;
    // every changed action below crosses the production packet/runtime/render seam.
    Check(persistence.SetAdvancedDocument(AdvancedDocument(8U, 11U), true),
          "virtual Canvas Edit journey installs its initial validated artwork");
    session.Advance(250U); // Establish active mode before timing the atomic edit itself.
    Drain(art);
    uint16_t request = 2U;
    const auto v2Packet = [&](const uint8_t op) {
        Art::BulkPacket packet;
        for (const uint8_t byte : {uint8_t(0x4d), uint8_t(2), op})
            V2Fixture::Put(packet, byte, 1U);
        V2Fixture::Put(packet, art.SessionId(), 4U);
        V2Fixture::Put(packet, request++, 2U);
        return packet;
    };
    // One asymmetric corner reaches the real physical topology after rotation;
    // full per-emitter RGB/effect metadata is checked by V2AtomicRotation.
    auto rotate = v2Packet(13U);
    const auto beforeRotation = persistence.AdvancedGeneration();
    V2Fixture::Put(rotate, beforeRotation, 4U);
    for (const uint8_t byte : {0U, 15U, 0U, 0U, 0U, 2U})
        V2Fixture::Put(rotate, byte, 1U);
    session.SendLitchcraftPacket(rotate.bytes.data(), rotate.size);
    bool rotationAcknowledged = false;
    Art::BulkPacket response;
    while (art.PeekPacket(response, 20U)) {
        rotationAcknowledged |= response.bytes[2] == 0x81U &&
            response.bytes[13] == uint8_t(Art::Status::Ok) &&
            V2Fixture::Read(response.bytes.data() + 11U, 2U) == 2U;
        art.AcceptPacket();
    }
    const auto& rotatedPhysical = session.PhysicalFrame();
    Check(rotationAcknowledged && persistence.AdvancedGeneration() == beforeRotation + 1U &&
              art.SelectedFrame() == 0U && !art.IsAnimationPlaying() &&
              rotatedPhysical.pixels[0U].r == 0U &&
              rotatedPhysical.pixels[7U * 4U + 1U].r != 0U,
          "VirtualAtomicRotate moves a8-left to h8-top on the actual V3 physical frame in one acknowledged operation");
    session.Advance(123U);
    auto inspect = v2Packet(1U);
    V2Fixture::Put(inspect, 1U, 1U);
    session.SendLitchcraftPacket(inspect.bytes.data(), inspect.size);
    uint64_t effectElapsed = 0U;
    uint16_t sample = 0U;
    unsigned effectHalves = 0U;
    while (art.PeekPacket(response, 20U)) {
        if (response.bytes[2] == 0x88U) sample = V2Fixture::Read(response.bytes.data() + 13U, 2U);
        if (response.bytes[2] == 0x8bU || response.bytes[2] == 0x8cU) {
            ++effectHalves;
            Check(sample != 0U && sample == V2Fixture::Read(response.bytes.data() + 13U, 2U),
                  "production effect-clock packets share the authoritative frame sample");
            effectElapsed |= uint64_t(V2Fixture::Read(response.bytes.data() + 16U, 4U)) <<
                (response.bytes[2] == 0x8bU ? 0U : 32U);
        }
        art.AcceptPacket();
    }
    Check(effectHalves == 2U && effectElapsed >= 123U && art.AnimationElapsedMs() == 0U,
          "VirtualEffectClock Inspect synchronizes continuously running effects while selected frame remains paused");
    std::printf("[VIRTUAL_ATOMIC_ROTATE_EFFECT_CLOCK] occupancy=0 input=CanvasEdit:CW90 "
                "mode=Litchcraft result=Ok frame=0 physical=a8-left->h8-top "
                "input=Inspect[1] sample=%u effect_ms=%llu frame_ms=0 "
                "next=continue-editing expiry=none\n", sample,
                static_cast<unsigned long long>(effectElapsed));
    const auto edit = [&](const bool erase, const uint8_t frame, const Art::Status expected,
                          const char* stage, const char* next) {
        Art::BulkPacket packet;
        for (const auto byte : {uint8_t(0x4d), uint8_t(2), uint8_t(13)}) {
            V2Fixture::Put(packet, byte, 1U);
        }
        V2Fixture::Put(packet, art.SessionId(), 4U);
        const auto id = request++;
        V2Fixture::Put(packet, id, 2U);
        V2Fixture::Put(packet, persistence.AdvancedGeneration(), 4U);
        for (const auto byte : {frame, uint8_t(15), uint8_t(erase ? 0 : 255),
                               uint8_t(0), uint8_t(0), uint8_t(erase)}) {
            V2Fixture::Put(packet, byte, 1U);
        }
        const auto toolBefore = art.Color();
        const auto eraseBefore = art.IsErasing();
        const auto generationBefore = persistence.AdvancedGeneration();
        const auto undoBefore = art.UndoCount();
        session.SendLitchcraftPacket(packet.bytes.data(), packet.size);
        session.Advance(20U);
        bool acknowledged = false;
        Art::BulkPacket reply;
        while (art.PeekPacket(reply, 244U)) {
            acknowledged |= reply.bytes[0] == 0x4dU && reply.bytes[2] == 0x81U &&
                V2Fixture::Read(reply.bytes.data() + 11U, 2U) == id &&
                reply.bytes[13] == uint8_t(expected);
            art.AcceptPacket();
        }
        Check(acknowledged && art.IsActive(), "virtual Canvas Edit receives exact authoritative ACK in Litchcraft");
        const auto pixel = session.SemanticFrame().board[63U][0U];
        if (expected == Art::Status::Ok) {
            Check(erase ? (pixel.r == 0U && pixel.g == 0U && pixel.b == 0U) : pixel.r != 0U,
                  "virtual Canvas Edit changes the whole rendered board at the next frame");
            Check(persistence.AdvancedGeneration() == generationBefore + 1U &&
                      art.UndoCount() == undoBefore + 1U,
                  "virtual Fill/Erase has exactly one document revision and history boundary");
            if (!erase) {
                Check(!art.IsErasing() && art.Target() == 15U && art.Color().r == 255U &&
                          art.Color().g == 0U && art.Color().b == 0U,
                      "virtual held-swatch Fill selects its Paint color without a preceding Tool command");
                const auto preview = modules::ScaleRgbSaturatingBy256({255U, 0U, 0U}, 48U);
                for (uint8_t index = 0U; index < 32U; ++index) {
                    const bool corner = index % 8U == 0U || index % 8U == 7U;
                    const auto status = session.SemanticFrame().status[index];
                    Check(status.r == (corner ? preview.r : 0U) && status.g == 0U && status.b == 0U,
                          "virtual Fill immediately renders only the eight matching red paint corners");
                    const auto physical = session.PhysicalFrame().pixels[256U + index];
                    Check((physical.r != 0U) == corner && physical.g == 0U && physical.b == 0U,
                          "virtual Fill reaches the physical V3 status chain in the same render cycle");
                }
            } else {
                Check(art.IsErasing() == eraseBefore && art.Color().r == toolBefore.r &&
                          art.Color().g == toolBefore.g && art.Color().b == toolBefore.b,
                      "virtual Erase All clears artwork without changing the persistent tool");
            }
        }
        std::printf("[LITCHCRAFT_ATOMIC_CANVAS_JOURNEY] stage=%s occupancy=0 input=CanvasEdit#%u "
                    "mode=Litchcraft result=%u frame=%u revision=%lu square63=%u,%u,%u "
                    "display=\"%.*s\" next=%s expiry=none tool=%s:%u,%u,%u corners=8\n", stage, id, unsigned(expected), frame,
                    static_cast<unsigned long>(persistence.AdvancedGeneration()), pixel.r, pixel.g, pixel.b,
                    int(session.DisplayText().size()), session.DisplayText().data(), next,
                    art.IsErasing() ? "erase" : "paint", art.Color().r, art.Color().g, art.Color().b);
    };
    edit(false, 0U, Art::Status::Ok, "fill", "paint-or-erase");
    edit(true, 0U, Art::Status::Ok, "erase-all", "paint-or-undo");
    edit(false, 1U, Art::Status::RevisionConflict, "stale-frame-rejected", "refresh-frame-and-retry");
    edit(false, 0U, Art::Status::Ok, "retry-current-frame", "continue-painting");

    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true);
    session.Advance(30U);
    session.Advance(1000U);
    session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false);
    session.Advance(30U);
    const auto checkTool = [&](const bool erase, const uint16_t id) {
        const auto packets = Drain(art);
        Check(std::count_if(packets.begin(), packets.end(), [&](const auto& packet) {
                  return packet.size == 17U && packet.bytes[0] == 0x4cU &&
                      packet.bytes[1] == 0x82U &&
                      V2Fixture::Read(packet.bytes.data() + 10U, 2U) == id &&
                      packet.bytes[12] == 255U && packet.bytes[13] == 0U &&
                      packet.bytes[14] == 0U && packet.bytes[15] == uint8_t(erase) &&
                      packet.bytes[16] == 15U;
              }) == 1U, "VirtualSharedTool emits exactly one matching red/full-mask Draw/Erase readback");
    };
    checkTool(true, 0U);
    const auto status = session.SemanticFrame().status;
    const auto wipe = std::count_if(status, status + modules::EDGE_LED_COUNT, [](const auto& led) {
        return led.r == 18U && led.g == 18U && led.b == 18U;
    });
    Check(art.IsErasing() && wipe == 8U,
          "VirtualSharedTool physical Circle hold selects Erase and renders its eight neutral wipe LEDs");
    art.EndConnection(); art.BeginConnection();
    const auto reconnect = v2Packet(1U);
    session.SendLitchcraftPacket(reconnect.bytes.data(), reconnect.size);
    checkTool(true, 0U);
    const auto draw = Request(art, Art::Op::Tool, request++, {0U, 15U, 255U, 0U, 0U});
    session.SendLitchcraftPacket(draw.bytes.data(), draw.size);
    session.Advance(20U);
    checkTool(false, request - 1U);
    const auto preview = modules::ScaleRgbSaturatingBy256({255U, 0U, 0U}, 48U);
    const auto drawStatus = session.SemanticFrame().status;
    Check(!art.IsErasing() && std::count_if(drawStatus, drawStatus + modules::EDGE_LED_COUNT,
              [&](const auto& led) { return led.r == preview.r && led.g == 0U && led.b == 0U; }) == 8U,
          "VirtualSharedTool app Draw returns the board to eight red paint corners");
    std::printf("[VIRTUAL_SHARED_TOOL] occupancy=0 input=Circle-hold+reconnect-Inspect+app-Draw "
                "mode=Litchcraft result=Ok readbacks=Erase/Erase/Draw LEDs=neutral-wipe->red-corners "
                "display=\"%.*s\" next=paint-or-toggle-Erase expiry=wipe-480ms/cancel-on-Draw\n",
                int(session.DisplayText().size()), session.DisplayText().data());
}

void VirtualPhysicalAppStrokeCrossingJourney() {
    constexpr uint8_t BRUSH = 9U;
    constexpr uint8_t NEXT_BRUSH = 10U;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    AdvancedSink sink;
    Persistence::AdvancedStorage storage;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    session.Advance(3000U);
    auto& art = session.Litchcraft();
    auto& persistence = session.ArtPersistence();
    persistence.Configure(sink.Ops());
    persistence.ConfigureAdvancedStorage(storage);
    art.BeginConnection();
    const auto enter = Request(art, Art::Op::Enter, 1U);
    session.SendLitchcraftPacket(enter.bytes.data(), enter.size);
    session.Advance(20U);
    Drain(art);
    Check(persistence.SetAdvancedDocument(AdvancedDocument(8U, 11U), true),
          "virtual stroke-crossing journey installs its validated document authority");
    session.Advance(20U);
    session.ObserveHallMask(UINT64_C(1) << BRUSH);
    session.Advance(20U);
    Drain(art);

    Art::BulkPacket appPaint;
    for (const uint8_t byte : {uint8_t(0x4d), uint8_t(2), uint8_t(3)}) {
        V2Fixture::Put(appPaint, byte, 1U);
    }
    V2Fixture::Put(appPaint, art.SessionId(), 4U);
    V2Fixture::Put(appPaint, 2U, 2U);
    for (const uint8_t byte : {uint8_t(0), uint8_t(91), uint8_t(0), uint8_t(3),
                               uint8_t(1), uint8_t(21), uint8_t(22), uint8_t(23), BRUSH}) {
        V2Fixture::Put(appPaint, byte, 1U);
    }
    session.SendLitchcraftPacket(appPaint.bytes.data(), appPaint.size);
    session.Advance(20U);
    bool acknowledged = false;
    Art::BulkPacket response;
    while (art.PeekPacket(response, 20U)) {
        acknowledged |= response.bytes[0] == 0x4dU && response.bytes[2] == 0x81U &&
            V2Fixture::Read(response.bytes.data() + 11U, 2U) == 2U &&
            response.bytes[13] == uint8_t(Art::Status::Ok);
        art.AcceptPacket();
    }
    const auto& appEmitter = persistence.AdvancedDocument().Value().frames[0].emitters[BRUSH * 4U];
    Check(acknowledged && appEmitter.color.r == 21U && appEmitter.color.g == 22U &&
              appEmitter.color.b == 23U && art.UndoCount() == 2U,
          "VirtualLitch accepts same-frame app Paint while the production Hall brush remains held");
    session.ObserveHallMask(UINT64_C(1) << NEXT_BRUSH);
    session.Advance(20U);
    session.ObserveHallMask(0U);
    session.Advance(Art::PHYSICAL_RELEASE_MS);
    const auto& physical = session.PhysicalFrame();
    Check(art.UndoCount() == 3U && physical.pixels[NEXT_BRUSH * 4U].r != 0U,
          "VirtualLitch next Hall activation starts a new physical stroke on the actual V3 frame");
    std::printf("[VIRTUAL_PHYSICAL_APP_STROKE] occupancy=0 input=Hall9+app-Paint#2+Hall10-lift "
                "mode=Litchcraft result=%s frame=%u undo=%u physical10=%u,%u,%u "
                "next=continue-painting expiry=physical-release-350ms\n",
                acknowledged ? "Ok" : "rejected", art.SelectedFrame(), art.UndoCount(),
                physical.pixels[NEXT_BRUSH * 4U].r, physical.pixels[NEXT_BRUSH * 4U].g,
                physical.pixels[NEXT_BRUSH * 4U].b);
}

void V2StreamedReplacement() {
    V2Fixture f;
    auto probe = f.Request(1U); V2Fixture::Put(probe, 2U, 1U);
    f.Ack(probe);
    const auto encoded = f.Encoded(77U);
    auto begin = f.Begin(encoded);
    begin.bytes[2] = 14U;
    f.Ack(begin);
    Art::BulkPacket initial;
    while (f.art.PeekPacket(initial, 244U)) f.art.AcceptPacket();
    const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
    const auto revision = f.persistence.AdvancedGeneration();
    modules::BleOtaChunkQueue ingress;
    Check(ingress.Begin(), "bulk FIFO initialized");
    size_t packets = 0U;
    for (size_t offset = 0U; offset < encoded.size;) {
        auto packet = f.TransferOp(15U, transfer);
        V2Fixture::Put(packet, offset, 2U);
        const auto count = std::min<size_t>(231U, encoded.size - offset);
        std::copy_n(encoded.bytes.begin() + offset, count, packet.bytes.begin() + packet.size);
        packet.size += count;
        Check(ingress.Enqueue(packet.bytes.data(), packet.size, packets + 1U),
              "entire max canonical transfer fits production FIFO without main-loop drainage");
        offset += count;
        ++packets;
    }
    auto commit = f.TransferOp(16U, transfer);
    Check(ingress.Enqueue(commit.bytes.data(), commit.size, packets + 1U),
          "final Commit fits same FIFO and cannot overtake chunks");
    const uint8_t* input = nullptr;
    size_t size = 0U;
    uint32_t ordinal = 0U;
    while (ingress.TryAcquireReadSlot(input, size, ordinal)) {
        f.art.HandleBulkPacket(input, size, true, 0U);
        if (input[2] == 15U) {
            Art::BulkPacket reply;
            Check(!f.art.PeekPacket(reply, 244U), "streamed canonical chunk has no protocol ACK or partial canvas publication");
            Check(f.persistence.AdvancedGeneration() == revision, "streaming preserves old live authority until final commit");
        }
        ingress.ReleaseReadSlot();
    }
    const auto ack = f.Pop();
    Check(ack.bytes[13] == 0U && f.persistence.AdvancedGeneration() == revision + 1U,
          "single final bulk receipt publishes complete canonical artwork atomically");
    f.art.HandleBulkPacket(commit.bytes.data(), commit.size, true, 0U);
    const auto replay = f.Pop();
    Check(replay.bytes == ack.bytes && f.persistence.AdvancedGeneration() == revision + 1U,
          "lost Commit ACK retries exact receipt without duplicate adoption or history");
    std::printf("[V2_BULK_REPLACE_JOURNEY] frames=8 chunks=%zu chunk_ACKs=0 final_status=%u revision=%lu\n",
                packets, ack.bytes[13], static_cast<unsigned long>(f.persistence.AdvancedGeneration()));
}

void V2WindowedStreamedReplacement() {
    for (const size_t payload : {20U, 182U, 244U}) {
        V2Fixture f;
        auto probe = f.Request(1U); V2Fixture::Put(probe, 3U, 1U); f.Ack(probe);
        const auto encoded = f.Encoded(77U);
        auto begin = f.Begin(encoded); begin.bytes[2] = 14U; f.Ack(begin);
        Art::BulkPacket reply;
        while (f.art.PeekPacket(reply, 244U)) f.art.AcceptPacket();
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        const auto revision = f.persistence.AdvancedGeneration();
        modules::BleOtaChunkQueue ingress;
        Check(ingress.Begin(), "windowed stream FIFO initialized");
        size_t packets = 0U, window = 0U, checkpoints = 0U;
        const auto drain = [&]() {
            const uint8_t* input = nullptr; size_t size = 0U; uint32_t ordinal = 0U;
            while (ingress.TryAcquireReadSlot(input, size, ordinal)) {
                f.art.HandleBulkPacket(input, size, true, 0U);
                if (input[2] == 15U) {
                    Check(!f.art.PeekPacket(reply, 244U), "window chunks have no application ACK");
                    Check(f.persistence.AdvancedGeneration() == revision,
                          "window drainage cannot publish a partial document");
                }
                ingress.ReleaseReadSlot();
            }
        };
        for (size_t offset = 0U; offset < encoded.size;) {
            auto packet = f.TransferOp(15U, transfer);
            V2Fixture::Put(packet, offset, 2U);
            const auto count = std::min(payload - 13U, encoded.size - offset);
            std::copy_n(encoded.bytes.begin() + offset, count, packet.bytes.begin() + packet.size);
            packet.size += count;
            Check(ingress.Enqueue(packet.bytes.data(), packet.size, ++packets),
                  "bounded window fits FIFO while production loop is deliberately stalled");
            offset += count;
            if (++window == 32U && offset < encoded.size && payload < 244U) {
                auto checkpoint = f.TransferOp(17U, transfer);
                V2Fixture::Put(checkpoint, offset, 2U);
                Check(ingress.Enqueue(checkpoint.bytes.data(), checkpoint.size, ++packets),
                      "checkpoint follows the entire window in the same FIFO");
                drain();
                const auto ack = f.Pop();
                Check(ack.bytes[13U] == 0U, "exact staged cursor grants next window");
                f.art.HandleBulkPacket(checkpoint.bytes.data(), checkpoint.size, true, 0U);
                Check(f.Pop().bytes == ack.bytes,
                      "lost window ACK replays identical receipt without applying bytes twice");
                window = 0U; ++checkpoints;
            }
        }
        auto commit = f.TransferOp(16U, transfer);
        Check(ingress.Enqueue(commit.bytes.data(), commit.size, ++packets),
              "Commit follows final window within FIFO capacity");
        drain();
        Check(f.Pop().bytes[13U] == 0U && f.persistence.AdvancedGeneration() == revision + 1U,
              "windowed final commit verifies full length CRC and revision atomically");
        modules::litchcraft_animation::EncodedDocument restored;
        Check(modules::litchcraft_animation::EncodeDocument(f.persistence.AdvancedDocument(), restored) &&
                  restored.size == encoded.size && restored.crc32 == encoded.crc32 &&
                  std::equal(encoded.bytes.begin(), encoded.bytes.begin() + encoded.size,
                             restored.bytes.begin()),
              "negotiated window streaming restores byte-exact complete artwork");
        Check(ingress.HighWaterMark() <= (payload < 244U ? 33U : 55U),
              "stalled-loop queue occupancy remains below the 64-slot production bound");
        std::printf("[V2_WINDOW_STREAM] payload=%zu checkpoints=%zu fifo_high=%u byte_exact=1\n",
                    payload, checkpoints, ingress.HighWaterMark());
    }
}

void V2StreamedReplacementFaults() {
    {
        V2Fixture f;
        const auto encoded = f.Encoded(77U);
        auto begin = f.Begin(encoded); begin.bytes[2] = 14U; f.Ack(begin);
        Art::BulkPacket initial;
        while (f.art.PeekPacket(initial, 244U)) f.art.AcceptPacket();
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        const auto revision = f.persistence.AdvancedGeneration();
        auto chunk = f.TransferOp(15U, transfer);
        V2Fixture::Put(chunk, 0U, 2U);
        std::copy_n(encoded.bytes.begin(), 169U, chunk.bytes.begin() + chunk.size);
        chunk.size += 169U;
        f.art.HandleBulkPacket(chunk.bytes.data(), chunk.size, true, 0U);
        auto checkpoint = f.TransferOp(17U, transfer);
        V2Fixture::Put(checkpoint, 338U, 2U); // Second ATT182 chunk was lost.
        f.art.HandleBulkPacket(checkpoint.bytes.data(), checkpoint.size, true, 0U);
        const auto ack = f.Pop();
        Check(ack.bytes[13U] == static_cast<uint8_t>(Art::Status::Incomplete) &&
                  f.persistence.AdvancedGeneration() == revision,
              "window checkpoint rejects a missing prefix before granting more FIFO credit");
        auto cancel = f.TransferOp(12U, transfer);
        f.Ack(cancel);
        Check(f.persistence.AdvancedTransportStorage() == nullptr,
              "failed window cancellation releases staging without replacing live artwork");
        auto next = f.Begin(encoded); next.bytes[2] = 14U; f.Ack(next);
        Check(f.persistence.AdvancedTransportStorage() != nullptr,
              "a canceled missing-window transfer admits a fresh exact replacement");
    }
    for (const bool corrupt : {false, true}) {
        V2Fixture f;
        const auto encoded = f.Encoded(77U);
        auto begin = f.Begin(encoded); begin.bytes[2] = 14U;
        f.Ack(begin);
        Art::BulkPacket initial;
        while (f.art.PeekPacket(initial, 244U)) f.art.AcceptPacket();
        const auto revision = f.persistence.AdvancedGeneration();
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        for (size_t offset = 0U; offset < encoded.size;) {
            auto packet = f.TransferOp(15U, transfer);
            V2Fixture::Put(packet, offset, 2U);
            const auto count = std::min<size_t>(231U, encoded.size - offset);
            std::copy_n(encoded.bytes.begin() + offset, count, packet.bytes.begin() + packet.size);
            packet.size += count;
            if (corrupt && offset == 0U) packet.bytes[20U] ^= 1U;
            // Drop one packet, or deliver all packets with one corrupted byte.
            if (corrupt || offset != 231U) {
                f.art.HandleBulkPacket(packet.bytes.data(), packet.size, true, 0U);
            }
            offset += count;
        }
        auto commit = f.TransferOp(16U, transfer);
        f.art.HandleBulkPacket(commit.bytes.data(), commit.size, true, 0U);
        const auto ack = f.Pop();
        Check(ack.bytes[13] == uint8_t(corrupt ? Art::Status::IntegrityFailure : Art::Status::Incomplete),
              "final bulk receipt rejects corruption and missing or reordered bytes");
        Check(f.persistence.AdvancedGeneration() == revision &&
                  f.persistence.AdvancedTransportStorage() == nullptr,
              "failed bulk commit preserves live canvas and releases staging for recovery");
    }
}

void V2InactiveStorageRetainedSession() {
    for (const bool restored : {false, true}) {
      for (const bool streamed : {false, true}) {
        V2Fixture f;
        if (restored) {
            f.art.RestoreArtwork(f.art.Artwork(), false, 0U);
            f.art.BeginConnection();
        } else {
            Send(f.art, Art::Op::Exit, f.next++);
            Drain(f.art);
        }
        Check(!f.art.IsActive() && f.art.SessionId() != 0U,
              "production inactive lifecycle retains nonzero display session");
        const auto retainedSession = f.art.SessionId();
        const auto zero = [](Art::BulkPacket packet) {
            std::fill_n(packet.bytes.begin() + 3U, 4U, 0U);
            return packet;
        };
        const auto encoded = f.Encoded(83U);
        auto begin = zero(f.Begin(encoded)); begin.bytes[2] = streamed ? 14U : 9U;
        f.Ack(begin);
        Art::BulkPacket pending;
        while (f.art.PeekPacket(pending, 244U)) f.art.AcceptPacket();
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        const auto revision = f.persistence.AdvancedGeneration();
        for (size_t offset = 0U; offset < encoded.size;) {
            auto packet = zero(f.TransferOp(streamed ? 15U : 10U, transfer));
            V2Fixture::Put(packet, offset, 2U);
            const auto count = std::min<size_t>(231U, encoded.size - offset);
            std::copy_n(encoded.bytes.begin() + offset, count, packet.bytes.begin() + packet.size);
            packet.size += count;
            if (streamed) f.art.HandleBulkPacket(packet.bytes.data(), packet.size, true, 0U);
            else f.Ack(packet);
            offset += count;
        }
        f.Ack(zero(f.TransferOp(streamed ? 16U : 11U, transfer)));
        Check(!f.art.IsActive() && f.art.SessionId() == retainedSession &&
                  f.persistence.AdvancedGeneration() == revision + 1U,
              "zero-session storage upload survives retained mode session without entering Litchcraft");
        std::printf("[V2_INACTIVE_RETAINED_SESSION] restored=%u streamed=%u retained_session=%lu result=Ok active=0\n",
                    restored, streamed, static_cast<unsigned long>(retainedSession));
      }
    }
    for (const uint8_t transition : {0U, 1U, 2U, 3U}) {
        V2Fixture f(V2Fixture::Authority::Advanced, false);
        const auto encoded = f.Encoded(83U);
        auto begin = f.Begin(encoded); begin.bytes[2] = 14U;
        f.Ack(begin);
        const auto transfer = V2Fixture::Read(begin.bytes.data() + 7U, 2U);
        if (transition == 0U) f.Ack(f.TransferOp(12U, transfer));
        else if (transition == 1U) Send(f.art, Art::Op::Enter, f.next++);
        else if (transition == 2U) {
            f.art.ObserveEntryButton({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1}, 0U, true);
            f.art.ObserveEntryButton({modules::UserButton::P1O, fxe::Button::Event::Press, modules::UiOwner::P1}, 10U, true);
            f.art.ObserveEntryButton({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1}, 3000U, true);
        }
        else f.art.EndConnection();
        Check(f.persistence.AdvancedTransportStorage() == nullptr,
              "Cancel/app entry/local entry/disconnect immediately retire inactive staging");
        Check(f.art.IsActive() == (transition == 1U || transition == 2U),
              "staging cleanup never changes the requested mode transition");
    }
}

void V2StoreOnlyMetadataBeforeFirstArtwork() {
    for (const bool active : {false, true}) {
        V2Fixture f(V2Fixture::Authority::Unconfigured, active);
        for (const bool effectClock : {false, true}) {
            auto inspect = f.Request(1U);
            if (effectClock) V2Fixture::Put(inspect, 1U, 1U);
            const auto reply = f.Ack(inspect);
            Check(reply.bytes[13U] == uint8_t(Art::Status::Ok) &&
                      (reply.bytes[14U] & 1U) == uint8_t(active) &&
                      reply.bytes[15U] == 0U && reply.bytes[17U] == 0U,
                  "Store-only Inspect reports mode without an old staging buffer or artwork root");
            Art::BulkPacket pending;
            while (f.art.PeekPacket(pending, 20U)) f.art.AcceptPacket();
        }
        f.Ack(f.Request(8U), Art::Status::Unsupported);
        Check(!f.persistence.IsAdvancedConfigured(),
              "metadata observation never configures retired advanced storage");
    }
}

int main() {
    V2StoreOnlyMetadataBeforeFirstArtwork();
    V2ReplyShapeBelongsToRequestContract();
    V2ReplacementRetiresStreamWithoutBlankOrRuntimeReversion();
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    DebounceAndReceiptGenerations();
    ContinuousDrawingAndNoops();
    PaletteDebounceAndMigration();
    CanvasAndPaletteShareTrailingDeadline();
    AdvancedPersistenceLane();
    V2MinimumAndHighMtuRoundtripAndExactReplay();
    V2StreamedReplacement();
    V2WindowedStreamedReplacement();
    V2StreamedReplacementFaults();
    V2InactiveStorageRetainedSession();
    V2InactiveStorageUploadPreservesGameAuthority();
    V2InvalidBootAcceptsExplicitReplacement();
    V2EditValidationMasksEffectsAndStrokeAuthority();
    ConnectionOrderedToolUsesDomainOwner();
    V2AtomicCanvasEdit();
    V2AtomicCheckerEdit();
    V2AtomicRotation();
    V2InspectRepublishesSharedTool();
    CompactControlPreemptsLegacySnapshotTraffic();
    CompactControlIngressPreemptsStreamChunks();
    EveryInteractiveIngressPreemptsBulk();
    QueuedToolReplyPreemptsCellAndSnapshotBulk();
    OrdinaryControlReplyPreemptsQueuedCells();
    V2ControlBundlesPreemptActiveSnapshotInOrder();
    DuplicateToolReplayPublishesCurrentAuthority();
    PhysicalBrushAuthorityPreemptsBulkAndFencesSessions();
    IdempotentEnterRetiresPriorV2Transfer();
    FreshEnterRetiresDiscardedV2PendingResponse();
    V2EffectClockOptIn();
    VirtualAtomicCanvasJourney();
    VirtualPhysicalAppStrokeCrossingJourney();
    V2FrameOperationsAndHistoryBounds();
    V2TimelineWrapFrozenPairsAndConnectionLifetime();
    V2PhysicalEditsConflictWithStagedReplacement();
    V2SameFrameAppStrokeCrossesPhysicalBrush();
    V2PhysicalEraseClearsEffectMetadata();
    V2DenseHistoryByteEvictionAndNetZeroStroke();
    V2DocumentMutationsAloneDirtyPersistence();
    V2StaticMigrationUndoPreservesDuplicateFavorites();
    V2EditPauseAndEntryClockReset();
    V2StateHistoryAvailabilityAndFrozenRetry();
    V2PhysicalBootHeapAdmitsAdvancedReplacement();
    V2FencingTimeoutAndLeaseRecovery();
    V2IntegrityReceiptsAndLegacyCompatibility();
    V2LongPacketFingerprintReplay();
    V2NoopsConflictsPipeliningAndPausedCleanup();
    V2AbsentAuthorityExactStateFixtures();
    V2SharedSnapshotMinimumAndHighMtuFixtures();
    V2SessionZeroSnapshotCancelReleasesLeaseWhilePaused();
    V2InactiveNonzeroSessionSnapshotCancel();
    AdvancedAndStaticLifecycleFlushCompose();
    DirtyAdvancedExitDefersEncodingAndMutexWait();
    AdvancedBackpressureRapidEditsExitAndReboot();
    AdvancedExitReentrySnapshotDuringModeSave();
    AdvancedMutationAndDurabilityDiagnostics();
    PendingExitFailureAndRecovery();
    AdmissionAndWholeDocumentFlush();
    FlushBehindAutosaveAndFailureRecovery();
    ActualMutationSourcesAndClockWrap();
    CodecRecoveryAndBoot();
    StoreOnlySettingsFlushDoesNotWriteOldArtwork();
    VirtualPersistenceJourney();
    RemoveFiles();
    std::printf("[LITCHCRAFT_PERSISTENCE] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
