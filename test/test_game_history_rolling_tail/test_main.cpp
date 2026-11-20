#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "game_history_memory_storage.hpp"
#include "../test_duck_coordinator_integration/duck_coordinator_test_support.hpp"
#include "modules/game_history_snapshot_reader.hpp"
#include "modules/litch_max_engine.hpp"

namespace {
using namespace modules;
using namespace modules::game_history;
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}

struct Harness {
    SettingsStoreConfig settingsConfig = duck_coordinator_test::MakeSettingsConfig(
        "build/test_history_rolling_tail.json");
    runtime::GameStateMachineConfig config = duck_coordinator_test::MakeStateMachineConfig();
    SettingsStore settings{settingsConfig};
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine sm{config};
    GamePersistenceCoordinator coordinator{settings, engine, sm};
    uint64_t physical = 0U;

    bool Seed() {
        duck_coordinator_test::RemoveSettingsFiles(settingsConfig.path);
        sm.Reset();
        if (!settings.Load()) return false;
        // Existing durable setup is the fixture boundary. Every subsequent
        // chess/duck action, saved tail, reader and capture uses production code.
        GamePersistenceState state;
        if (!state.fen.Assign(INITIAL_FEN)) return false;
        state.undoBaseFen = state.fen;
        state.gameHistory.present = true;
        state.gameHistory.gameId.bytes[0] = 0x42U;
        state.gameHistory.lineRevision = 1U;
        DuckGameDurableState duck;
        duck.variant = GameVariant::Duck;
        duck.duckSquare = 24U;
        char encoded[32] = {};
        if (!EncodeDuckGameDurableState(duck, encoded, sizeof(encoded)) ||
            !state.variantState.Assign(encoded) ||
            !WriteGamePersistenceState(settings.Doc(), state) ||
            !settings.SaveNow() || !coordinator.LoadOrInitialize()) return false;
        physical = duck_coordinator_test::INITIAL_OCCUPANCY | (1ULL << 24U);
        sm.ApplyPhysicalOccupancySnapshot(physical);
        sm.Tick();
        coordinator.Update();
        return true;
    }

    bool Move(uint8_t from, uint8_t to) {
        const uint8_t duckFrom = coordinator.GetDuckSquare();
        const uint8_t duckTo = duckFrom == 24U ? 25U : 24U;
        physical &= ~(1ULL << from);
        duck_coordinator_test::Observe(sm, 1ULL << from, physical);
        coordinator.UpdateLegalDestinationsMask();
        if ((physical & (1ULL << to)) != 0U) {
            physical &= ~(1ULL << to);
            duck_coordinator_test::Observe(sm, 1ULL << to, physical);
        }
        physical |= 1ULL << to;
        duck_coordinator_test::Observe(sm, 1ULL << to, physical);
        fxe::ManualMillis::AdvanceMs(11U);
        sm.Tick(); coordinator.Update();
        if (coordinator.GetDuckTurnPhase() != DuckTurnPhase::DuckRelocation) return false;
        physical &= ~(1ULL << duckFrom);
        duck_coordinator_test::Observe(sm, 1ULL << duckFrom, physical);
        physical |= 1ULL << duckTo;
        duck_coordinator_test::Observe(sm, 1ULL << duckTo, physical);
        coordinator.Update();
        return coordinator.GetDuckTurnPhase() == DuckTurnPhase::ChessMove &&
               coordinator.GetDuckSquare() == duckTo;
    }
};

uint32_t Counter(const Position& p, size_t offset) {
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) result |= uint32_t(p.bytes[offset+i]) << (i*8U);
    return result;
}

bool CaptureSaved(Harness& h, GameHistoryJournal& journal, uint16_t ply,
                  uint32_t halfmove, uint32_t expectedSequence,
                  game_history_test::FakeStorage* interrupted = nullptr,
                  bool completeWriteBeforeFailure = false) {
    Check(h.settings.SaveNow(), "current game saves before capture");
    SettingsStore loaded(h.settingsConfig);
    GamePersistenceState state;
    if (!loaded.Load() || !ReadGamePersistenceState(loaded.Doc(), state)) return false;
    Check(state.gameHistory.gameId.bytes[0] == 0x42U &&
          state.gameHistory.currentPly == ply && state.gameHistory.lineRevision == ply+1U,
          "durable game identity and revision follow physical turns");
    const uint16_t retainedPlies = ply > 128U ? 128U : ply;
    Check(state.undoHistoryCount / UNDO_HISTORY_FIELDS_PER_MOVE == retainedPlies,
          "real coordinator retains bounded aligned tail");
    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    if (!reader.Build(snapshot)) { Check(false, "real saved tail builds capture view"); return false; }
    const uint16_t basePly = ply - retainedPlies;
    Check(snapshot.basePly == basePly && Counter(snapshot.basePosition, 34U) == 0U &&
          Counter(snapshot.basePosition, 38U) == basePly / 2U + 1U,
          "saved base has a normalized halfmove clock and advances the fullmove after Black");
    GameHistoryCapture capture(journal);
    if (interrupted != nullptr) {
        interrupted->failJournalAppend = interrupted->journalAppends + 1U;
        interrupted->writeBeforeFailure = completeWriteBeforeFailure;
        Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::IoFailure,
              "uncertain suffix append reports failure");
        interrupted->failJournalAppend = 0U;
        Check(journal.Open(0x4252U) == JournalOpenResult::Ready,
              "uncertain suffix reopens from durable journal authority");
    }
    Check(capture.Reconcile(snapshot) ==
              (interrupted != nullptr && completeWriteBeforeFailure
                   ? GameHistoryCaptureResult::NoChange : GameHistoryCaptureResult::Applied),
          "new physical turn or recovered complete append captures exactly once");
    Record latest;
    Check(journal.ReadLatestRecord(latest) == JournalSegmentRead::Ready &&
          latest.ply == ply && latest.outboxSequence == expectedSequence,
          "rollover retains one original base and one record per committed ply");
    Check(Counter(latest.position,34U) == halfmove &&
          Counter(latest.position,38U) == ply/2U+1U,
          "emitted counters continue from durable authority");
    const auto before = journal.Summary().newestAllocated;
    Check(capture.Reconcile(snapshot) == GameHistoryCaptureResult::NoChange &&
          journal.Summary().newestAllocated == before,
          "same saved snapshot replay adds no records");
    return true;
}


void ExportSegments(GameHistoryJournal& journal) {
    FILE* file = std::fopen("build/test_history_rebase_segments.jsonl", "wb");
    Check(file != nullptr, "cross-language segment fixture opens");
    if (!file) return;
    uint32_t ack = 0U;
    while (ack < journal.Summary().newestAllocated) {
        JournalSegmentPlan plan;
        if (journal.PlanSegmentAfter(ack, plan) != JournalSegmentRead::Ready) {
            Check(false, "actual journal plans complete rebase transfer");
            break;
        }
        const auto& d = plan.descriptor;
        std::fprintf(file,
            "{\"encodedBytes\":%u,\"recordCount\":%u,\"flags\":%u,"
            "\"firstOutboxSequence\":%u,\"crc32\":%u,\"hex\":\"",
            unsigned(d.encodedBytes), unsigned(d.recordCount), unsigned(d.flags),
            unsigned(d.firstOutboxSequence), unsigned(d.crc32));
        uint16_t offset = 0U;
        while (offset < d.encodedBytes) {
            uint8_t chunk[12] = {};
            size_t length = 0U;
            if (journal.ReadSegmentChunk(plan, offset, chunk, sizeof(chunk), length) !=
                    JournalSegmentRead::Ready || length == 0U) {
                Check(false, "actual segment streams bounded bytes"); break;
            }
            for (size_t i=0U; i<length; ++i) std::fprintf(file,"%02x",unsigned(chunk[i]));
            offset = static_cast<uint16_t>(offset + length);
        }
        std::fputs("\"}\n",file);
        ack = d.firstOutboxSequence + d.recordCount - 1U;
        Check(journal.AdvanceDurableAck(ack) == JournalMutationResult::Applied,
              "fixture advances modeled ACK for next overlap segment");
    }
    std::fclose(file);
}

void CapturePhysicalUndo(Harness& h, GameHistoryJournal& journal) {
    Check(h.coordinator.RequestUndo(1U), "long-game whole Duck turn Undo accepted");
    uint8_t duckFrom=INVALID_SQUARE, duckTo=INVALID_SQUARE;
    if (!h.sm.TryGetDuckRelocation(duckFrom,duckTo)) {
        Check(false,"Undo asks for the prior duck square"); return;
    }
    h.physical &= ~(1ULL<<duckFrom);
    duck_coordinator_test::Observe(h.sm,1ULL<<duckFrom,h.physical);
    h.physical |= 1ULL<<duckTo;
    duck_coordinator_test::Observe(h.sm,1ULL<<duckTo,h.physical);
    h.coordinator.Update();
    h.coordinator.Update();
    uint8_t from=0U,to=0U;
    if (!h.sm.TryGetUndoPromptMove(from,to)) {
        Check(false,"Undo asks for the prior chess square"); return;
    }
    h.physical &= ~(1ULL<<to);
    duck_coordinator_test::Observe(h.sm,1ULL<<to,h.physical);
    h.physical |= 1ULL<<from;
    duck_coordinator_test::Observe(h.sm,1ULL<<from,h.physical);
    // Undoing the last capture also restores its white pawn.
    h.physical |= 1ULL<<to;
    duck_coordinator_test::Observe(h.sm,1ULL<<to,h.physical);
    fxe::ManualMillis::AdvanceMs(11U);
    h.sm.Tick(); h.coordinator.Update();
    Check(!h.sm.IsUndoPromptActive() && h.coordinator.GetUndoHistoryMoveCount()==127U,
          "physical captured-pawn Undo completes before saving");
    // Ordinary Undo defers its snapshot until the physical prompt completes;
    // let that existing first 500 ms retry boundary run before forcing disk durability.
    fxe::ManualMillis::AdvanceMs(500U);
    h.coordinator.Update();
    Check(h.settings.SaveNow(),"physical Undo saves");
    SettingsStore loaded(h.settingsConfig);
    GamePersistenceState state;
    if (!loaded.Load() || !ReadGamePersistenceState(loaded.Doc(),state)) {
        Check(false,"Undo settings reload"); return;
    }
    GameHistorySnapshotReader reader(state);
    GameHistoryCaptureSnapshot snapshot;
    if (!reader.Build(snapshot)) { Check(false,"Undo tail builds"); return; }
    Check(snapshot.basePly==8U && snapshot.CurrentPly()==135U && snapshot.lineRevision==138U,
          "Undo retains authoritative bounded tail and newer revision");
    GameHistoryCapture capture(journal);
    Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::Applied,
          "Undo emits a revised line");
    Record latest;
    Check(journal.ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
          latest.ply==135U && Counter(latest.position,34U)==0U &&
          Counter(latest.position,38U)==68U,
          "Undo suffix retains exact historical counters");
    const uint32_t before=journal.Summary().newestAllocated;
    Check(capture.Reconcile(snapshot)==GameHistoryCaptureResult::NoChange &&
          journal.Summary().newestAllocated==before,"Undo capture retry is idempotent");
    ExportSegments(journal);
}

void TestRealEngineRollingTail() {
    fxe::ManualMillis::SetNowMs(20000U);
    Harness h;
    if (!h.Seed()) { Check(false,"real-engine Duck setup restores"); return; }
    game_history_test::FakeStorage storage;
    auto journal = storage.Journal();
    Check(journal.Open(0x4252U) == JournalOpenResult::Initialized, "journal opens");
    for (uint16_t ply=1U; ply<=132U; ++ply) {
        const uint8_t cycle = static_cast<uint8_t>((ply-1U)%4U);
        const uint8_t from = cycle==0U ? 62U : cycle==1U ? 6U : cycle==2U ? 45U : 21U;
        const uint8_t to = cycle==0U ? 45U : cycle==1U ? 21U : cycle==2U ? 62U : 6U;
        if (!h.Move(from,to)) { std::printf("failed turn %u\n",ply); Check(false,"physical Duck turn commits"); return; }
        if (ply>=128U && !CaptureSaved(h,journal,ply,ply,ply+1U)) return;
    }
    Check(h.Move(52U,44U), "pawn move commits after rolling tail");
    Check(CaptureSaved(h,journal,133U,0U,134U,&storage), "pawn reset retries from durable anchor");
    Check(h.Move(11U,27U), "Black pawn commits after recovery");
    Check(CaptureSaved(h,journal,134U,0U,135U,&storage,true),
          "complete write before failure recovers without duplicate append");
    Check(h.Move(44U,36U), "White pawn approaches capture");
    Check(CaptureSaved(h,journal,135U,0U,136U), "White pawn snapshot captures");
    Check(h.Move(27U,36U), "Black pawn captures after rolling tail");
    Check(CaptureSaved(h,journal,136U,0U,137U), "capture resets emitted halfmove counter");
    CapturePhysicalUndo(h,journal);
}
} // namespace

int main() {
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    TestRealEngineRollingTail();
    if (!failures) std::puts("[PASS] real-engine history rolling-tail continuity");
    return failures ? 1 : 0;
}
