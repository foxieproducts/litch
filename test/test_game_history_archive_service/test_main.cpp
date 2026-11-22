#include <cstdio>
#include <string>

#include "firmware/game_history_archive_service.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/settings_store.hpp"
#include "game_history_memory_storage.hpp"

namespace {
using namespace modules;
using namespace modules::game_history;
int failures=0;
void Check(bool ok,const char* message) { if (!ok) { ++failures; std::printf("[FAIL] %s\n",message); } }
GamePersistenceState Initial() {
    GamePersistenceState state;
    Check(state.fen.Assign("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),"initial FEN fits");
    state.undoBaseFen=state.fen;
    state.gameHistory.present=true;
    state.gameHistory.gameId.bytes[0]=42U;
    state.gameHistory.lineRevision=1U;
    DuckGameDurableState variant;
    variant.variant=GameVariant::Classic;
    char encoded[32]={};
    Check(EncodeDuckGameDurableState(variant,encoded,sizeof(encoded)),"variant encodes");
    Check(state.variantState.Assign(encoded),"variant fits");
    return state;
}
void TestDurableCallbackReopensUncertainJournal() {
    for (bool written : {false,true}) {
        game_history_test::FakeStorage storage;
        GameHistoryJournalStorageOps ops{&storage,&game_history_test::FakeStorage::Stat,
            &game_history_test::FakeStorage::Read,&game_history_test::FakeStorage::Replace,
            &game_history_test::FakeStorage::Append,&game_history_test::FakeStorage::Promote};
        firmware::GameHistoryArchiveService service(ops);
        SettingsStoreConfig config;
        config.path="build/test_history_service.json";
        std::remove(config.path); std::remove("build/test_history_service.json.bak");
        std::remove("build/test_history_service.json.tmp");
        SettingsStore source(config);
        auto state=Initial();
        Check(WriteGamePersistenceState(source.Doc(),state) && source.SaveNowBlocking(),
              "real settings snapshot is durable before history callback");
        SettingsStore loaded(config);
        Check(loaded.Load(),"durable settings snapshot reloads");
        service.OnDurableSettingsSnapshot(loaded.Doc());
        Check(storage.journalAppends==0U && !service.IsReady(),"unconfigured service is inert");
        Check(!service.Configure(0U) && service.Configure(0x4242U) && !service.Configure(0x4343U),
              "journal identity configures once and cannot silently replace ownership");
        storage.failJournalAppend=1U;
        storage.writeBeforeFailure=written;
        service.OnDurableSettingsSnapshot(loaded.Doc());
        Check(!service.IsReady() && service.FailureCount()==1U,
              "uncertain append makes service readiness reflect the closed journal");
        service.OnDurableSettingsSnapshot(loaded.Doc());
        Check(service.IsReady() && service.Journal().Summary().newestAllocated==1U,
              "same durable snapshot reopens and recovers exactly one base");
        const auto attempts=storage.journalAppends;
        service.OnDurableSettingsSnapshot(loaded.Doc());
        Check(storage.journalAppends==attempts && service.FailureCount()==1U,
              "recovered callback is idempotent without another failure");
        // Startup may conservatively classify a transient scan read failure
        // as an invalid suffix. The next durable callback must rescan it.
        firmware::GameHistoryArchiveService reopened(ops);
        reopened.Configure(0x4242U);
        storage.failJournalRead=storage.journalReads+1U;
        reopened.OnDurableSettingsSnapshot(loaded.Doc());
        Check(!reopened.IsReady(),"failed startup scan is not a ready history service");
        storage.failJournalRead=0U;
        reopened.OnDurableSettingsSnapshot(loaded.Doc());
        Check(reopened.IsReady() && !reopened.Journal().Summary().hasInvalidSuffix &&
                  reopened.Journal().Summary().newestAllocated==1U &&
                  storage.journalAppends==attempts,
              "transient startup read recovers without rewriting known history");
        auto& bytes=storage.artifacts[game_history_test::FakeStorage::Index(JournalArtifact::Journal)].bytes;
        bytes.push_back(0xFFU);
        const auto damaged=bytes;
        firmware::GameHistoryArchiveService corrupt(ops);
        corrupt.Configure(0x4242U);
        corrupt.OnDurableSettingsSnapshot(loaded.Doc());
        corrupt.OnDurableSettingsSnapshot(loaded.Doc());
        Check(!corrupt.IsReady() && bytes==damaged,
              "actual invalid suffix remains untouched across recovery attempts");
        bytes.pop_back();
        Record latest;
        Check(service.Journal().ReadLatestRecord(latest)==JournalSegmentRead::Ready &&
                  latest.gameId==state.gameHistory.gameId && latest.ply==0U,
              "recovery preserves the durable game identity and position");
    }
}
void TestIneligibleSnapshotDoesNotOpenStorage() {
    game_history_test::FakeStorage storage;
    firmware::GameHistoryArchiveService service({&storage,&game_history_test::FakeStorage::Stat,
        &game_history_test::FakeStorage::Read,&game_history_test::FakeStorage::Replace,
        &game_history_test::FakeStorage::Append,&game_history_test::FakeStorage::Promote});
    service.Configure(0x4545U);
    DynamicJsonDocument document(8192U);
    auto state=Initial();
    DuckGameDurableState variant;
    char encoded[32]={};
    EncodeDuckGameDurableState(variant,encoded,sizeof(encoded));
    Check(state.variantState.Assign(encoded),"variant fits");
    Check(WriteGamePersistenceState(document,state),"undecided setup snapshot writes");
    service.OnDurableSettingsSnapshot(document);
    Check(!service.IsReady() && service.FailureCount()==0U && storage.journalAppends==0U,
          "undecided setup is expected and never opens a false Classic journal");
    state=Initial();
    WriteGamePersistenceState(document,state);
    document[KEY_GAME_HISTORY_V1]="malformed";
    service.OnDurableSettingsSnapshot(document);
    Check(!service.IsReady() && storage.journalAppends==0U,
          "quarantined history cannot cause journal writes");
}
}
int main() {
    TestDurableCallbackReopensUncertainJournal();
    TestIneligibleSnapshotDoesNotOpenStorage();
    std::printf("[GAME_HISTORY_ARCHIVE_SERVICE] failures=%d\n",failures);
    return failures==0 ? 0 : 1;
}
