#include <cstdio>
#include <cstring>

#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_persistence.hpp"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}

bool FillIdentity(void*, uint8_t* output, size_t length) {
    std::memset(output, 0x5a, length);
    return true;
}

void TestOwnerCodec() {
    StaticJsonDocument<128> document;
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Absent,
          "missing record is absent");

    Check(modules::WriteBoardOwner(document, modules::BoardOwnerRecord::Active),
          "active record writes");
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Active,
          "active record roundtrips");

    StaticJsonDocument<128> clearing;
    Check(modules::WriteBoardOwner(clearing, modules::BoardOwnerRecord::Clearing),
          "clearing record writes");
    Check(modules::ReadBoardOwner(clearing) == modules::BoardOwnerRecord::Clearing,
          "clearing record roundtrips");
    Check(modules::WriteBoardOwner(clearing, modules::BoardOwnerRecord::Absent),
          "absent writes explicit state");
    Check(clearing[modules::KEY_BOARD_OWNER_V1][0U] == 2U &&
              clearing[modules::KEY_BOARD_OWNER_V1][1U] == 0U &&
              modules::ReadBoardOwner(clearing) == modules::BoardOwnerRecord::Absent,
          "absent retains the prepared V2 scalar slots");

    Check(!modules::WriteBoardOwner(document, modules::BoardOwnerRecord::Invalid),
          "invalid state cannot be written");
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Active,
          "rejected write preserves existing record");
}

void TestAddingJournal() {
    using namespace modules;
    StaticJsonDocument<4096> document;
    Check(WriteBoardOwner(document, BoardOwnerRecord::Active) &&
              PrepareBoardOwnerJournal(document), "boot reserves bounded V3 journal without changing owner");
    BoardOwnerAddBaseline baseline{};
    baseline.saved.count = 2U;
    baseline.saved.peers[0U].address[0U] = 19U;
    baseline.saved.peers[1U].type = 1U;
    baseline.saved.peers[1U].address[0U] = 23U;
    baseline.rpaCount = 1U;
    baseline.rpas[0U].rpa.address[0U] = 91U;
    baseline.rpas[0U].identity.address[0U] = 99U; // Retain unrelated pre-existing metadata too.
    const size_t usage = document.memoryUsage();
    for (unsigned retry = 0U; retry < 50U; ++retry) {
        Check(WriteBoardOwnerAdding(document, baseline) &&
                  ReadBoardOwner(document) == BoardOwnerRecord::Adding &&
                  ReadBoardOwnerAddBaseline(document).saved.ContainsAll(baseline.saved),
              "Adding journals only exact typed baseline identities");
        std::string saved; serializeJson(document, saved);
        StaticJsonDocument<4096> reopened;
        Check(!deserializeJson(reopened, saved) &&
                  ReadBoardOwner(reopened) == BoardOwnerRecord::Adding &&
                  ReadBoardOwnerAddBaseline(reopened).saved.ContainsAll(baseline.saved) &&
                  ReadBoardOwnerAddBaseline(reopened).rpas[0U].identity == baseline.rpas[0U].identity,
              "Adding survives serialization/reopen with both retained identities");
        Check(WriteBoardOwner(document, BoardOwnerRecord::Active) &&
                  !ReadBoardOwnerAddBaseline(document).Valid() && document.memoryUsage() == usage,
              "success/rollback clears transient journal using the same scalar slots");
    }
    auto invalid = baseline;
    invalid.saved.peers[1U] = invalid.saved.peers[0U];
    Check(!WriteBoardOwnerAdding(document, invalid), "duplicate baseline refuses before mutation");
    invalid = baseline; invalid.saved.count = 3U;
    Check(!WriteBoardOwnerAdding(document, invalid), "full three-phone baseline cannot start Add");
    invalid = baseline; invalid.saved.peers[2U].address[0U] = 24U;
    Check(!WriteBoardOwnerAdding(document, invalid), "unused saved-peer slots must be canonical zero");
    invalid = baseline; invalid.rpas[1U].identity.address[0U] = 25U;
    Check(!WriteBoardOwnerAdding(document, invalid), "unused RPA slots cannot hide unjournaled metadata");
    Check(ReadBoardOwner(document) == BoardOwnerRecord::Active,
          "rejected Add preserves saved owner");
    Check(WriteBoardOwnerAdding(document, baseline), "corruption fixture starts valid");
    document[KEY_BOARD_OWNER_V1][4U] = "not-an-address-byte";
    Check(ReadBoardOwner(document) == BoardOwnerRecord::Invalid,
          "malformed journal never becomes Active or empty");
    StaticJsonDocument<128> small;
    Check(WriteBoardOwner(small, BoardOwnerRecord::Active), "small old record starts valid");
    std::string before, after; serializeJson(small, before);
    Check(!PrepareBoardOwnerJournal(small), "insufficient journal capacity fails before expansion");
    serializeJson(small, after);
    Check(before == after, "capacity refusal preserves exact prior record bytes");
}

void TestInvalidRecords() {
    StaticJsonDocument<256> document;
    document[modules::KEY_BOARD_OWNER_V1] = "active";
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid,
          "non-array is invalid");

    document.clear();
    document[modules::KEY_BOARD_OWNER_V1].to<JsonArray>().add(1U);
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid,
          "short record is invalid");

    document.clear();
    JsonArray longRecord = document[modules::KEY_BOARD_OWNER_V1].to<JsonArray>();
    longRecord.add(1U); longRecord.add(1U); longRecord.add(0U);
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid,
          "long record is invalid");

    for (uint32_t value : {0U, 3U}) {
        document.clear();
        JsonArray record = document[modules::KEY_BOARD_OWNER_V1].to<JsonArray>();
        record.add(value);
        record.add(1U);
        Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid,
              "invalid version or state fails closed");
    }

    document.clear();
    JsonArray wrongType = document[modules::KEY_BOARD_OWNER_V1].to<JsonArray>();
    wrongType.add(1U); wrongType.add("1");
    Check(modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid,
          "string state is invalid");

    StaticJsonDocument<8> tooSmall;
    Check(!modules::WriteBoardOwner(tooSmall, modules::BoardOwnerRecord::Active),
          "capacity failure is reported");
    Check(!tooSmall.containsKey(modules::KEY_BOARD_OWNER_V1),
          "absent capacity failure creates no partial key");

    StaticJsonDocument<160> constrained;
    Check(modules::WriteBoardOwner(constrained, modules::BoardOwnerRecord::Active),
          "constrained fixture starts valid");
    constrained["padding"] = "0123456789012345678901234567890123456789";
    std::string before;
    serializeJson(constrained, before);
    const size_t memoryBefore = constrained.memoryUsage();
    for (unsigned iteration = 0U; iteration < 500U; ++iteration) {
        Check(modules::WriteBoardOwner(constrained, modules::BoardOwnerRecord::Clearing) &&
                  modules::WriteBoardOwner(constrained, modules::BoardOwnerRecord::Absent) &&
                  modules::WriteBoardOwner(constrained, modules::BoardOwnerRecord::Active),
              "prepared transitions do not need spare arena capacity");
    }
    std::string after;
    serializeJson(constrained, after);
    Check(after == before && constrained.memoryUsage() == memoryBefore &&
              modules::ReadBoardOwner(constrained) ==
                  modules::BoardOwnerRecord::Active,
          "repeated transitions preserve stable arena usage and unrelated bytes");

    StaticJsonDocument<256> stickyOverflow;
    Check(modules::WriteBoardOwner(stickyOverflow,
                                   modules::BoardOwnerRecord::Active),
          "sticky overflow fixture starts valid");
    stickyOverflow["overflow"] = std::string(1024U, 'x');
    stickyOverflow.remove("overflow");
    Check(stickyOverflow.overflowed() &&
              stickyOverflow.memoryUsage() < stickyOverflow.capacity(),
          "fixture has sticky overflow with nominal spare capacity");
    std::string stickyBefore;
    serializeJson(stickyOverflow, stickyBefore);
    Check(!modules::WriteBoardOwner(stickyOverflow,
                                    modules::BoardOwnerRecord::Clearing),
          "sticky overflow rejects owner replacement upfront");
    std::string stickyAfter;
    serializeJson(stickyOverflow, stickyAfter);
    Check(stickyAfter == stickyBefore &&
              modules::ReadBoardOwner(stickyOverflow) ==
                  modules::BoardOwnerRecord::Active,
          "sticky overflow preserves owner bytes and semantics");
}

void TestLegacyMigrationAndStrictEnums() {
    for (uint32_t version : {1U, 2U}) {
        for (uint32_t state : {0U, 1U, 2U, 3U}) {
            StaticJsonDocument<128> document;
            JsonArray record = document[modules::KEY_BOARD_OWNER_V1].to<JsonArray>();
            record.add(version); record.add(state);
            const auto expected = state == 1U ? modules::BoardOwnerRecord::Active :
                (state == 2U ? modules::BoardOwnerRecord::Clearing :
                 (state == 0U && version == 2U ? modules::BoardOwnerRecord::Absent :
                  modules::BoardOwnerRecord::Invalid));
            Check(modules::ReadBoardOwner(document) == expected,
                  "only legacy V1 active/clearing and V2 absent/active/clearing read");
            if (expected != modules::BoardOwnerRecord::Invalid) {
                const size_t usage = document.memoryUsage();
                Check(modules::WriteBoardOwner(document, expected) &&
                          document[modules::KEY_BOARD_OWNER_V1][0U] == 2U &&
                          document.memoryUsage() == usage,
                      "legacy upgrade reuses existing scalar slots");
            }
        }
    }
}

void TestHiddenKeyAndDurableIdentity() {
    Check(modules::IsBoardOwnerSettingKey("BOARD_OWNER_V1"),
          "canonical owner key is hidden");
    Check(modules::IsBoardOwnerSettingKey("board_owner_v1"),
          "lowercase owner key is hidden");
    Check(!modules::IsBoardOwnerSettingKey("BOARD_OWNER_V2"),
          "other keys are not hidden");

    StaticJsonDocument<256> document;
    modules::BoardIdentityPersistence identity;
    modules::BoardIdentity value;
    value.bytes.fill(0xccU);
    Check(!identity.GetDurableIdentity(value),
          "identity unavailable before prepare");
    Check(value.bytes[0] == 0xccU,
          "failed getter does not publish zero or provisional identity");
    Check(identity.Prepare(document, &FillIdentity, nullptr),
          "identity prepares");
    Check(!identity.GetDurableIdentity(value),
          "prepared identity remains unavailable before durable promotion");
    identity.OnDurableSettingsSnapshot(document);
    Check(identity.GetDurableIdentity(value) && value.bytes[0] == 0x5aU,
          "durable getter returns exact promoted identity");
}

} // namespace

int main() {
    TestOwnerCodec();
    TestAddingJournal();
    TestInvalidRecords();
    TestLegacyMigrationAndStrictEnums();
    TestHiddenKeyAndDurableIdentity();
    if (failures == 0) {
        std::printf("[BOARD_OWNER_PERSISTENCE] ok\n");
    }
    return failures == 0 ? 0 : 1;
}
