#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "modules/home_state_notification.hpp"
#include "modules/home_state_wire_codec.hpp"
#include "modules/initial_fen.hpp"
#include "modules/board_state_notification.hpp"
#include "firmware/hybrid_console_transport.hpp"
#include "modules/ble_settings_value_codec.hpp"
#include "modules/settings_registry.hpp"
#include <string>
#include "foxie_embedded/console.hpp"

namespace {

struct BootstrapSerialSink {
    int available() const { return 0; }
    int read() const { return -1; }
    int availableForWrite() const { return 0; }
    size_t write(const uint8_t*, size_t) { return 0U; }
    explicit operator bool() const { return false; }
};
struct BootstrapBleSink {
    std::string bytes;
    bool TryReadConsoleInputChar(char&) { return false; }
    void WriteConsoleOutput(const char* data, size_t size) { bytes.append(data, size); }
};
void CheckBootstrapThroughHybridTransport(const modules::BoardStateNotificationLine& line) {
    BootstrapSerialSink serial;
    BootstrapBleSink ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    // Chunked PrintRaw calls still represent exactly one logical record.
    for (size_t offset = 0U; offset < line.Size(); offset += 13U) {
        const size_t remaining = line.Size() - offset;
        transport.Write(line.CStr() + offset, remaining < 13U ? remaining : 13U);
    }
    TEST_ASSERT_TRUE(ble.bytes.empty());
    transport.Write("\r\n", 2U);
    TEST_ASSERT_EQUAL_STRING((std::string(line.CStr()) + "\n").c_str(), ble.bytes.c_str());
    DynamicJsonDocument decoded(8192);
    TEST_ASSERT_FALSE(deserializeJson(decoded, ble.bytes));
    TEST_ASSERT_EQUAL_STRING("board_state", decoded["event"]);
}

void TestBoardBootstrapRetainsIndicesAndLogicalPalette() {
    modules::SettingsStoreConfig config;
    modules::SettingsStore settings(config);
    settings.Doc()["P1_END_PIECE_WHEEL"] = 40U;
    settings.Doc()["P2_END_PIECE_WHEEL"] = 200U;
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    settings.Doc()["GAME_UNDO_HISTORY"] = "private saved history";
    modules::HomeStateNotification snapshot;
    snapshot.sequence = 4U;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    modules::HomeStateNotificationLine home;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, home));
    const auto health = modules::FormatBoardHealth(
        {}, modules::PersistenceDegradation::None, {});
    modules::BoardStateNotificationLine line;
    TEST_ASSERT_TRUE(modules::TryFormatBoardStateNotification(home, health, settings, line));
    CheckBootstrapThroughHybridTransport(line);
    DynamicJsonDocument parsed(8192);
    TEST_ASSERT_FALSE(deserializeJson(parsed, line.CStr()));
    TEST_ASSERT_EQUAL_STRING("board_state", parsed["event"]);
    TEST_ASSERT_EQUAL_UINT32(4U, parsed["home"]["seq"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(40U, parsed["ends"][0].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(200U, parsed["ends"][1].as<uint32_t>());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "GAME_UNDO_HISTORY"));
    size_t count = 0U;
    const auto* descriptors = modules::settings_registry::EnumerateAll(count);
    size_t index = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const auto& descriptor = descriptors[i];
        if (!modules::ble_settings_value_codec::IsAppVisible(descriptor)) continue;
        TEST_ASSERT_EQUAL_STRING(descriptor.key, parsed["settings"][index][0]);
        if (std::strcmp(descriptor.key, "P1_PIECE_WHEEL") == 0) {
            TEST_ASSERT_EQUAL_UINT32(40U, parsed["settings"][index][1].as<uint32_t>());
        }
        if (std::strcmp(descriptor.key, "P2_PIECE_WHEEL") == 0) {
            TEST_ASSERT_EQUAL_UINT32(200U, parsed["settings"][index][1].as<uint32_t>());
        }
        ++index;
    }
    TEST_ASSERT_EQUAL_UINT32(index, parsed["settings"].size());
    std::printf("[BOARD_BOOTSTRAP] default_bytes=%zu settings=%zu\n", line.Size(), index);

    // Exercise each registered type's longest legal representation. Internal
    // durable state remains excluded even when it is much larger than a packet.
    for (size_t i = 0U; i < count; ++i) {
        const auto& descriptor = descriptors[i];
        if (!modules::ble_settings_value_codec::IsAppVisible(descriptor)) continue;
        if (descriptor.type == fxe::SettingType::UInt32) settings.Doc()[descriptor.key] = descriptor.max;
        if (descriptor.type == fxe::SettingType::Bool) settings.Doc()[descriptor.key] = false;
        if (descriptor.type == fxe::SettingType::Enum) {
            const char* longest = descriptor.defaultStr;
            for (uint8_t j = 0U; j < descriptor.allowedCount; ++j) {
                if (std::strlen(descriptor.allowedValues[j]) > std::strlen(longest)) longest = descriptor.allowedValues[j];
            }
            settings.Doc()[descriptor.key] = longest;
        }
    }
    TEST_ASSERT_TRUE(modules::TryFormatBoardStateNotification(home, health, settings, line));
    CheckBootstrapThroughHybridTransport(line);
    TEST_ASSERT_TRUE(modules::TryFormatBoardStateNotification(
        home, health, settings, line, UINT32_MAX));
    TEST_ASSERT_FALSE(deserializeJson(parsed, line.CStr()));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, parsed["q"].as<uint32_t>());
    CheckBootstrapThroughHybridTransport(line);
    // Reserve the full existing Home/health bounds and maximal revision digits;
    // the maximum-width receipt is already included in the formatted line.
    const size_t bound = line.Size() + (home.Capacity() - 1U - home.Size()) +
                         (health.Capacity() - 1U - health.Size()) + 10U;
    std::printf("[BOARD_BOOTSTRAP] maximum_registered_bound=%zu capacity=%zu\n", bound, line.Capacity());
    TEST_ASSERT_LESS_THAN_UINT32(line.Capacity(), bound);
    settings.Doc()["SETTLE_ANIM_STYLE"] = std::string(600U, 'x');
    TEST_ASSERT_FALSE(modules::TryFormatBoardStateNotification(home, health, settings, line));
    TEST_ASSERT_TRUE(line.Empty());
}

void TestSharedPublicationQuietFallbackAndRecovery() {
    modules::SettingsStoreConfig config;
    modules::SettingsStore settings(config);
    BootstrapSerialSink serial;
    BootstrapBleSink ble;
    firmware_internal::HybridConsoleTransport transport(serial, ble);
    fxe::Console console(transport, {.installBuiltins = false});
    const auto health = modules::FormatBoardHealth(
        {}, modules::PersistenceDegradation::None, {});
    auto lastHealth = health;
    modules::HomeStateNotificationLine empty;
    modules::WriteBoardStatePublication(console, lastHealth,
        {empty, health, settings, false});
    TEST_ASSERT_TRUE(ble.bytes.empty());

    modules::HomeStateNotification snapshot;
    snapshot.sequence = 1U;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    modules::HomeStateNotificationLine home;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, home));
    settings.Doc()["SETTLE_ANIM_STYLE"] = std::string(600U, 'x');
    modules::WriteBoardStatePublication(console, lastHealth,
        {home, health, settings, true});
    const std::string legacy = std::string(health.CStr()) + "\n" + home.CStr() + "\n";
    TEST_ASSERT_EQUAL_STRING(legacy.c_str(), ble.bytes.c_str());
    ble.bytes.clear();
    modules::WriteBoardStatePublication(console, lastHealth,
        {empty, health, settings, false});
    TEST_ASSERT_TRUE(ble.bytes.empty());

    const auto degraded = modules::FormatBoardHealth(
        {}, modules::PersistenceDegradation::StorageIo, {});
    modules::WriteBoardStatePublication(console, lastHealth,
        {empty, degraded, settings, false});
    TEST_ASSERT_EQUAL_STRING((std::string(degraded.CStr()) + "\n").c_str(),
        ble.bytes.c_str());
    ble.bytes.clear();
    settings.Doc()["SETTLE_ANIM_STYLE"] = "mini_orbit";
    modules::WriteBoardStatePublication(console, lastHealth,
        {home, health, settings, true, UINT32_MAX});
    const auto boundary = ble.bytes.find('\n');
    TEST_ASSERT_NOT_EQUAL(std::string::npos, boundary);
    DynamicJsonDocument parsed(8192);
    TEST_ASSERT_FALSE(deserializeJson(parsed, ble.bytes.substr(0U, boundary)));
    TEST_ASSERT_EQUAL_STRING("board_state", parsed["event"]);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, parsed["q"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING(legacy.c_str(), ble.bytes.substr(boundary + 1U).c_str());
}

void TestFormatsVersionedHomeStateLine() {
    modules::HomeStateNotification snapshot;
    snapshot.sequence = 42U;
    snapshot.action = modules::HomeStateAction::Lift;
    snapshot.fen =
        "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.outcome = modules::VariantOutcome{};
    snapshot.boardRotated = false;
    snapshot.whiteOwnerIsP2 = false;
    snapshot.placementMode = false;

    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"event\":\"home_state\",\"v\":1,\"seq\":42,\"a\":\"lift\","
        "\"fen\":\"8/8/8/8/8/8/8/K6k w - - 0 1\","
        "\"occ\":\"8100000000000000\",\"g\":\"c\",\"duck\":\"--\","
        "\"out\":\"n\",\"win\":\"n\",\"rot\":0,\"white\":1,\"pm\":0,"
        "\"up\":0}",
        line.CStr());
}

void TestPublisherSequencesAuthoritativeUndoState() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Progress;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.undoPending = true;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"up\":1"));

    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.undoPending = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"up\":0"));
}

void TestPublisherSequencesAuthoritativeVexMoveFirstState() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Correction;
    snapshot.fen = modules::INITIAL_FEN;
    snapshot.occupiedMask = 0xFFFF10000000FFFFULL;
    snapshot.variant = modules::GameVariant::Duck;
    snapshot.duckSquare = 44U;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"vx\":"));

    snapshot.vexMoveFirst = true;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"vx\":1"));

    snapshot.vexMoveFirst = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"vx\":"));
}

void TestFormatsSequencesAndReplaysAuthoritativeCoachingState() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.coachingStateValid = true;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":0"));

    snapshot.coachingMask = 1U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":1"));

    snapshot.coachingMask = 3U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));

    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));

    snapshot.coachingStateValid = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(4U, publisher.Sequence());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"co\":"));
}

void TestRejectsMalformedAuthoritativeCoachingMask() {
    modules::HomeStateNotification snapshot;
    snapshot.sequence = 1U;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.coachingStateValid = true;
    snapshot.coachingMask = 4U;

    modules::HomeStateNotificationLine line;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.coachingStateValid = false;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"co\":"));
}

void TestFormatsAndSequencesAssistanceProjection() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "8/8/8/8/8/8/4P3/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8110000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.assistance.valid = true;
    snapshot.assistance.manualAffordanceUseful = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Off;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"as\":\"2o\""));

    snapshot.assistance.hintRequested = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Pending;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"as\":\"3p\""));

    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Thinking;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"as\":\"3t\""));

    snapshot.assistance.fromSquare = 52U;
    snapshot.assistance.toSquare = 36U;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::HintReady;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(4U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"as\":\"3he2e4\""));

    snapshot.assistance.manualAffordanceUseful = true;
    snapshot.assistance.coachingForced = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::HintRefreshing;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(5U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"as\":\"7re2e4\""));

    snapshot.variant = modules::GameVariant::Duck;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::DuckCompound;
    snapshot.assistance.duckFromSquare = 24U;
    snapshot.assistance.duckToSquare = 25U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(6U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"as\":\"7be2e4a5b5\""));

    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::DuckRelocation;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(7U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"as\":\"7da5b5\""));

    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::LegalGuide;
    snapshot.assistance.legalSourceSquare = 52U;
    snapshot.assistance.legalDestinationMask =
        (UINT64_C(1) << 36U) | (UINT64_C(1) << 44U);
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(8U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(
        line.CStr(), "\"as\":\"7le20000101000000000e4\""));

    snapshot.assistance.manualAffordanceUseful = false;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Hidden;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(9U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"as\":\"5x\""));

    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(9U, publisher.Sequence());
}

void TestRejectsMalformedAssistanceProjection() {
    modules::HomeStateNotification snapshot;
    snapshot.sequence = 1U;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "8/8/8/8/8/8/4P3/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8110000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.assistance.valid = true;

    modules::HomeStateNotificationLine line;
    snapshot.assistance.manualAffordanceUseful = false;
    snapshot.assistance.coachingForced = false;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Pending;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.hintRequested = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Off;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::HintReady;
    snapshot.assistance.fromSquare = 64U;
    snapshot.assistance.toSquare = 36U;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.fromSquare = 52U;
    snapshot.variant = modules::GameVariant::Duck;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::HintRefreshing;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.variant = modules::GameVariant::Classic;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::DuckCompound;
    snapshot.assistance.duckFromSquare = 24U;
    snapshot.assistance.duckToSquare = 25U;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::LegalGuide;
    snapshot.assistance.legalSourceSquare = 52U;
    snapshot.assistance.legalDestinationMask = 0ULL;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.legalDestinationMask = UINT64_C(1) << 52U;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.legalDestinationMask = UINT64_C(1) << 36U;
    snapshot.assistance.toSquare = 44U;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    snapshot.assistance.toSquare = 36U;
    snapshot.assistance.hintRequested = false;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    snapshot.assistance.hintRequested = true;
    snapshot.assistance.duckFromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    snapshot.assistance.duckToSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.assistance.valid = false;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"as\":"));
}

void TestFormatsAndSequencesPersistenceDegradation() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"ps\""));

    snapshot.persistenceDegradation =
        modules::PersistenceDegradation::QuarantinedRecord;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"q\""));

    snapshot.persistenceDegradation =
        modules::PersistenceDegradation::StorageIo;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));

    snapshot.persistenceDegradation =
        modules::PersistenceDegradation::None;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(4U, publisher.Sequence());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"ps\""));
}

void TestFormatsAndSequencesFreshSetupLifecycle() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Correction;
    snapshot.fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    snapshot.occupiedMask = 0x8386040308882780ULL;
    snapshot.variant = modules::GameVariant::Undecided;
    snapshot.placementMode = true;
    snapshot.freshSetup = true;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"pm\":1,\"fs\":1"));

    snapshot.freshSetup = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"fs\""));
}

void TestFormatsAndSequencesQueuedCpuMovePrompt() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Cpu;
    snapshot.fen =
        "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2";
    snapshot.occupiedMask = 0xFFEF00101000EFFFULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.physicalMoveValid = true;
    snapshot.physicalFromSquare = 6U;   // g8
    snapshot.physicalToSquare = 21U;    // f6

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"a\":\"cpu\""));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"f6\",\"--\"]"));

    snapshot.physicalToSquare = 23U; // h6: a changed prompt is new state.
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"h6\",\"--\"]"));

    snapshot.physicalCaptureSquareValid = true;
    snapshot.physicalCaptureSquare = 31U; // h5: en-passant-style removal.
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"h6\",\"h5\"]"));
    snapshot.action = modules::HomeStateAction::Settle;
    snapshot.occupiedMask |= UINT64_C(1) << snapshot.physicalCaptureSquare;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(4U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"a\":\"settle\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"c\":[\"g8\",\"h6\",\"h5\"]"));
    snapshot.occupiedMask &= ~(UINT64_C(1) << snapshot.physicalCaptureSquare);
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    snapshot.occupiedMask |= UINT64_C(1) << snapshot.physicalCaptureSquare;
    snapshot.action = modules::HomeStateAction::Correction;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    snapshot.physicalMoveValid = false;
    snapshot.physicalCaptureSquareValid = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"c\":"));

}

void TestFormatsAndSequencesLastMovesForBothSides() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen =
        "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2";
    snapshot.occupiedMask = 0xFFEF00101000EFFFULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.whiteLastMoveValid = true;
    snapshot.whiteLastMoveFromSquare = 52U; // e2
    snapshot.whiteLastMoveToSquare = 36U;   // e4
    snapshot.blackLastMoveValid = true;
    snapshot.blackLastMoveFromSquare = 12U; // e7
    snapshot.blackLastMoveToSquare = 28U;   // e5
    snapshot.lastMoveMostRecentSide = 'b';

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"m\":\"e2e4e7e5b\""));

    snapshot.whiteLastMoveToSquare = 44U; // e3
    snapshot.lastMoveMostRecentSide = 'w';
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"m\":\"e2e3e7e5w\""));
}

void TestFormatsAndSequencesClockStateAndTimeoutWinner() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    snapshot.occupiedMask = 0xFFFF00000000FFFFULL;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.clockStateValid = true;
    snapshot.clockEnabled = true;
    snapshot.clockRunning = true;
    snapshot.clockPaused = false;
    snapshot.clockActiveSide = 'w';
    snapshot.clockTimerSetting = 3U;
    snapshot.clockSetupState = 'y';
    snapshot.clockWhiteRemainingSeconds = 300U;
    snapshot.clockBlackRemainingSeconds = 300U;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"cl\":[1,300,300,\"w\",1,0,3,\"y\"]"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"tw\""));

    // Whole-second projection bounds BLE traffic while still updating Home.
    snapshot.clockWhiteRemainingSeconds = 299U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"cl\":[1,299,300,\"w\",1,0,3,\"y\"]"));

    snapshot.clockRunning = false;
    snapshot.clockActiveSide = 'b';
    snapshot.clockBlackRemainingSeconds = 0U;
    snapshot.clockWinner = modules::GameWinner::White;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"tw\":\"w\""));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"cl\":[1,299,0,\"b\",0,0,3,\"y\"]"));
}

void TestPublisherSequencesStateChangesAndReplaysCurrentState() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen =
        "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(1U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":1"));

    TEST_ASSERT_FALSE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(1U, publisher.Sequence());

    snapshot.occupiedMask &= ~(1ULL << 56U);
    snapshot.action = modules::HomeStateAction::Lift;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":2"));

    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":2"));

}

void TestPublisherEmitsResetFenWithUnchangedPhysicalOccupancy() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Correction;
    snapshot.fen = "8/8/8/8/8/8/8/K6k b - - 8 12";
    snapshot.occupiedMask = 0x8100000000000000ULL;
    snapshot.variant = modules::GameVariant::Classic;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(1U, publisher.Sequence());

    snapshot.fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":2"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), snapshot.fen));
}

void TestPublisherKeepsOpeningFenWhenDuckVariantLatches() {
    constexpr const char* kOpeningFen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = kOpeningFen;
    snapshot.occupiedMask = 0xFFFF00000000FFFFULL;
    snapshot.variant = modules::GameVariant::Undecided;

    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":1"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), kOpeningFen));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"g\":\"u\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"duck\":\"--\""));

    snapshot.variant = modules::GameVariant::Duck;
    snapshot.duckSquare = 30U; // g5
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"seq\":2"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), kOpeningFen));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"g\":\"d\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"duck\":\"g5\""));

    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), kOpeningFen));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"g\":\"d\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"duck\":\"g5\""));
}

void TestPresentationOwnsOnlyAValidUndecidedDuckCandidate() {
    modules::GameVariantStateSnapshot variant;
    variant.variant = modules::GameVariant::Undecided;
    variant.duckSquare = modules::INVALID_DUCK_SQUARE;

    TEST_ASSERT_EQUAL_UINT8(
        18U,
        modules::HomeStateDuckSquareForPresentation(variant, 18U));
    TEST_ASSERT_EQUAL_UINT8(
        modules::INVALID_DUCK_SQUARE,
        modules::HomeStateDuckSquareForPresentation(
            variant, modules::INVALID_DUCK_SQUARE));

    variant.variant = modules::GameVariant::Duck;
    variant.duckSquare = 30U;
    TEST_ASSERT_EQUAL_UINT8(
        30U,
        modules::HomeStateDuckSquareForPresentation(variant, 18U));

    variant.variant = modules::GameVariant::Classic;
    variant.duckSquare = modules::INVALID_DUCK_SQUARE;
    TEST_ASSERT_EQUAL_UINT8(
        modules::INVALID_DUCK_SQUARE,
        modules::HomeStateDuckSquareForPresentation(variant, 18U));
}

void TestDuckPlanSequencesOwnershipAndRetiresCoordinates() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Duck;
    snapshot.variant = modules::GameVariant::Duck;
    snapshot.duckSquare = 27U;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    snapshot.duckRelocationOwner = 'c';
    snapshot.duckRelocationFromSquare = 27U;
    snapshot.duckRelocationToSquare = 36U;
    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"dr\":\"cd5e4\""));
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(1U, publisher.Sequence());

    snapshot.action = modules::HomeStateAction::Correction;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"a\":\"corr\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"dr\":\"cd5e4\""));
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(2U, publisher.Sequence());
    snapshot.action = modules::HomeStateAction::Duck;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(3U, publisher.Sequence());

    snapshot.duckRelocationOwner = 'u';
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(4U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"dr\":\"ud5e4\""));
    snapshot.duckRelocationOwner = 'h';
    snapshot.duckRelocationToSquare = modules::INVALID_DUCK_SQUARE;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(5U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"dr\":\"hd5--\""));

    snapshot.duckRelocationOwner = '\0';
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(6U, publisher.Sequence());
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"dr\":"));
    snapshot.duckRelocationOwner = 'c';
    TEST_ASSERT_FALSE(publisher.TryPrepare(snapshot, false, line));
    snapshot.duckRelocationToSquare = 36U;
    snapshot.clockWinner = modules::GameWinner::White;
    TEST_ASSERT_FALSE(publisher.TryPrepare(snapshot, false, line));
}

void TestPendingPromotionPromptKeepsPhysicalAuthorityBounded() {
    modules::HomeStateNotification snapshot;
    snapshot.sequence = UINT32_MAX;
    snapshot.fen = "k1r1b1q1/1P1p1p1p/p1p1p1p1/1n1n1n1n/N1N1N1N1/1P1P1P1P/P1P1P1P1/1Q1B1R1K w - e3 99 999";
    snapshot.action = modules::HomeStateAction::Correction;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.occupiedMask = UINT64_C(0xAAAAAAAAAAAAAAAA);
    snapshot.physicalMoveValid = true;
    snapshot.physicalFromSquare = 9U;
    snapshot.physicalToSquare = 1U;
    snapshot.clockStateValid = true;
    snapshot.clockEnabled = true;
    snapshot.clockRunning = true;
    snapshot.clockPaused = true;
    snapshot.clockTimerSetting = 6U;
    snapshot.clockWhiteRemainingSeconds = 1800U;
    snapshot.clockBlackRemainingSeconds = 1800U;
    snapshot.coachingStateValid = true;
    snapshot.coachingMask = 3U;
    snapshot.persistenceDegradation = modules::PersistenceDegradation::StorageIo;
    snapshot.whiteLastMoveValid = true;
    snapshot.whiteLastMoveFromSquare = 52U;
    snapshot.whiteLastMoveToSquare = 36U;
    snapshot.lastMoveMostRecentSide = 'w';
    snapshot.assistance.valid = true;
    snapshot.assistance.presentation = modules::HomeAssistancePresentation::Hidden;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_LESS_THAN_UINT32(320U, line.Size());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"c\":[\"b7\",\"b8\",\"--\"]"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":["));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));
    modules::home_state_wire::Payload payload;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, payload));
    TEST_ASSERT_EQUAL_UINT32(100U, payload.size());
    TEST_ASSERT_EQUAL_UINT8(9U, payload[62U]);
    TEST_ASSERT_EQUAL_UINT8(1U, payload[63U]);
    TEST_ASSERT_EQUAL_UINT8(255U, payload[64U]);
    TEST_ASSERT_EQUAL_UINT8(0x20U, payload[7U] & 0x60U);
    std::printf("[PROMOTION_HOME_BOUND] bytes=%zu line=%s\n", line.Size(), line.CStr());
    for (const bool placement : {true, false}) {
        auto invalid = snapshot;
        invalid.placementMode = placement;
        invalid.undoPending = !placement;
        TEST_ASSERT_FALSE(modules::IsValidHomeStateNotification(invalid));
    }
    auto invalid = snapshot;
    invalid.physicalCaptureSquareValid = true;
    invalid.physicalCaptureSquare = 1U;
    TEST_ASSERT_FALSE(modules::IsValidHomeStateNotification(invalid));
    invalid = snapshot;
    invalid.physicalFromSquare = 0U;
    TEST_ASSERT_FALSE(modules::IsValidHomeStateNotification(invalid));
    invalid = snapshot;
    invalid.freshSetup = true;
    TEST_ASSERT_FALSE(modules::IsValidHomeStateNotification(invalid));
    snapshot.fen = "r6k/1P6/8/8/8/8/8/K7 w - - 0 1";
    snapshot.physicalToSquare = 0U;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"c\":[\"b7\",\"a8\",\"--\"]"));
    const std::string maximal = std::string(snapshot.fen) + std::string(159U - std::strlen(snapshot.fen), '0');
    snapshot.fen = maximal.c_str();
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_TRUE(line.Empty());
}

void TestFormatterKeepsWorstCasePayloadBounded() {
    modules::HomeStateNotification legal;
    legal.sequence = UINT32_MAX;
    legal.action = modules::HomeStateAction::Cpu;
    constexpr const char* MAX_LEGAL_FEN =
        "k1r1b1q1/1p1p1p1p/p1p1p1p1/1n1n1n1n/"
        "N1N1N1N1/1P1P1P1P/P1P1P1P1/1Q1B1R1K w - e3 99 999";
    legal.fen = MAX_LEGAL_FEN;
    legal.occupiedMask = 0xAAAAAAAAAAAAAAAAULL;
    legal.variant = modules::GameVariant::Duck;
    legal.duckSquare = 63U;
    legal.boardRotated = true;
    legal.whiteOwnerIsP2 = true;
    legal.placementMode = true;
    legal.undoPending = true;
    legal.clockStateValid = true;
    legal.clockEnabled = true;
    legal.clockRunning = true;
    legal.clockPaused = true;
    legal.clockActiveSide = 'b';
    legal.clockTimerSetting = 6U;
    legal.clockSetupState = 'b';
    legal.clockWhiteRemainingSeconds = 1800U;
    legal.clockBlackRemainingSeconds = 1800U;
    legal.physicalMoveValid = true;
    legal.physicalFromSquare = 6U;
    legal.physicalToSquare = 21U;
    legal.physicalCaptureSquareValid = true;
    legal.physicalCaptureSquare = 31U;
    legal.whiteLastMoveValid = true;
    legal.whiteLastMoveFromSquare = 52U;
    legal.whiteLastMoveToSquare = 36U;
    legal.blackLastMoveValid = true;
    legal.blackLastMoveFromSquare = 12U;
    legal.blackLastMoveToSquare = 28U;
    legal.lastMoveMostRecentSide = 'b';

    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_EQUAL_UINT32(316U, static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":["));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"f6\",\"h5\"]"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"m\":\"e2e4e7e5b\""));

    legal.coachingStateValid = true;
    legal.coachingMask = 3U;
    legal.persistenceDegradation =
        modules::PersistenceDegradation::StorageIo;
    legal.fen =
        "8/8/p1p1p1p1/1n1n1n1n/N1N1N1N1/"
        "1P1P1P1P/P1P1P1P1/1Q1B1R1K w - e3 99 999";
    legal.variant = modules::GameVariant::Classic;
    legal.duckSquare = modules::INVALID_DUCK_SQUARE;
    legal.assistance.valid = true;
    legal.assistance.hintRequested = true;
    legal.assistance.manualAffordanceUseful = true;
    legal.assistance.presentation =
        modules::HomeAssistancePresentation::HintReady;
    legal.assistance.fromSquare = 52U;
    legal.assistance.toSquare = 36U;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":["));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"f6\",\"h5\"]"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"as\":\"3he2e4\""));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"m\":"));

    legal.fen = MAX_LEGAL_FEN;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":["));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"f6\",\"h5\"]"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"m\":"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"as\":"));

    legal.variant = modules::GameVariant::Duck;
    legal.duckSquare = 63U;
    legal.assistance.presentation =
        modules::HomeAssistancePresentation::LegalGuide;
    legal.assistance.legalSourceSquare = 52U;
    legal.assistance.legalDestinationMask =
        (UINT64_C(1) << 36U) | (UINT64_C(1) << 44U);
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"up\":1"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":["));
    TEST_ASSERT_NOT_NULL(
        std::strstr(line.CStr(), "\"c\":[\"g8\",\"f6\",\"h5\"]"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"m\":"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"as\":"));

    auto held = legal;
    held.action = modules::HomeStateAction::Lift;
    held.physicalMoveValid = false;
    held.physicalCaptureSquareValid = false;
    held.placementMode = false;
    held.variant = modules::GameVariant::Classic;
    held.duckSquare = modules::INVALID_DUCK_SQUARE;
    held.fen = "4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1";
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(held, line));
    TEST_ASSERT_LESS_THAN_UINT32(modules::HOME_STATE_NOTIFICATION_CAPACITY, line.Size());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "le20000101000000000e4"));
    held.action = modules::HomeStateAction::Settle;
    held.physicalMoveValid = true;
    held.physicalFromSquare = 36U;
    held.physicalToSquare = 43U;
    held.physicalCaptureSquareValid = true;
    held.physicalCaptureSquare = 35U;
    held.occupiedMask = UINT64_C(0x1000080800000010);
    held.fen = "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1";
    held.assistance.presentation = modules::HomeAssistancePresentation::Hidden;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(held, line));
    TEST_ASSERT_LESS_THAN_UINT32(modules::HOME_STATE_NOTIFICATION_CAPACITY, line.Size());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"c\":[\"e4\",\"d3\",\"d4\"]"));

    // The active Duck plan replaces the CPU chess tuple after its physical
    // acknowledgement. Keep it ahead of duplicate history/assistance while
    // preserving all authoritative recovery and clock fields.
    legal.action = modules::HomeStateAction::Duck;
    legal.physicalMoveValid = false;
    legal.physicalCaptureSquareValid = false;
    legal.placementMode = false;
    legal.duckRelocationOwner = 'c';
    legal.duckRelocationFromSquare = 27U;
    legal.duckRelocationToSquare = 36U;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_LESS_THAN_UINT32(320U, static_cast<uint32_t>(line.Size()));
    for (const char* token : {"\"dr\":\"cd5e4\"", "\"up\":1", "\"co\":3",
                              "\"ps\":\"i\"", "\"cl\":["}) {
        TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), token));
    }
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"m\":"));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"as\":"));

    legal.action = modules::HomeStateAction::Correction;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(legal, line));
    TEST_ASSERT_LESS_THAN_UINT32(320U, static_cast<uint32_t>(line.Size()));
    for (const char* token : {"\"dr\":\"cd5e4\"", "\"a\":\"corr\"",
                              "\"up\":1", "\"co\":3", "\"ps\":\"i\"", "\"cl\":["}) {
        TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), token));
    }

    char longestFen[litch_max::FEN_BUFFER_CAPACITY] = {};
    std::memset(longestFen, 'x', sizeof(longestFen) - 1U);

    modules::HomeStateNotification snapshot;
    snapshot.sequence = UINT32_MAX;
    snapshot.action = modules::HomeStateAction::Progress;
    snapshot.fen = longestFen;
    snapshot.occupiedMask = UINT64_MAX;
    snapshot.variant = modules::GameVariant::Duck;
    snapshot.duckSquare = 63U;
    snapshot.outcome.reason = modules::VariantOutcomeReason::KingCaptured;
    snapshot.outcome.winner = modules::GameWinner::White;
    snapshot.boardRotated = true;
    snapshot.whiteOwnerIsP2 = true;
    snapshot.placementMode = true;
    snapshot.freshSetup = true;
    snapshot.coachingStateValid = true;
    snapshot.coachingMask = 3U;
    snapshot.assistance.valid = true;
    snapshot.assistance.hintRequested = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::Pending;

    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"up\""));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"fs\""));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"co\""));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"as\""));

    snapshot.action = modules::HomeStateAction::Cpu;
    snapshot.physicalMoveValid = true;
    snapshot.physicalFromSquare = 6U;
    snapshot.physicalToSquare = 21U;
    TEST_ASSERT_TRUE(modules::TryFormatHomeStateNotification(snapshot, line));
    TEST_ASSERT_LESS_THAN_UINT32(
        modules::HOME_STATE_NOTIFICATION_CAPACITY,
        static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"c\""));

    longestFen[0] = '\n';
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));

    snapshot.sequence = 0U;
    snapshot.fen = "8/8/8/8/8/8/8/K6k w - - 0 1";
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
}

void TestCpuThinkingIsAdditiveBoundedAndRetiresWithSearch() {
    modules::HomeStateNotification snapshot;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    snapshot.occupiedMask = UINT64_C(0xFFFF00000000FFFF);
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.clockStateValid = true;
    snapshot.clockEnabled = true;
    snapshot.clockRunning = true;
    snapshot.clockTimerSetting = 3U;
    snapshot.clockSetupState = 'n';
    snapshot.clockWhiteRemainingSeconds = UINT32_MAX;
    snapshot.clockBlackRemainingSeconds = UINT32_MAX;
    snapshot.coachingStateValid = true;
    snapshot.coachingMask = 3U;
    snapshot.persistenceDegradation = modules::PersistenceDegradation::StorageIo;
    snapshot.cpuThinking = {'w', UINT32_MAX, UINT16_MAX};
    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"th\":[\"w\",4294967295,65535]"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"cl\":"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"co\":3"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"ps\":\"i\""));
    TEST_ASSERT_LESS_THAN_UINT32(320U, static_cast<uint32_t>(line.Size()));
    TEST_ASSERT_FALSE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, true, line));
    TEST_ASSERT_EQUAL_UINT32(1U, publisher.Sequence());
    snapshot.cpuThinking.remainingSeconds = 0U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "4294967295,null]"));
    snapshot.action = modules::HomeStateAction::Correction;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"a\":\"corr\""));
    snapshot.cpuThinking = {};
    snapshot.action = modules::HomeStateAction::Cpu;
    snapshot.physicalMoveValid = true;
    snapshot.physicalFromSquare = 52U;
    snapshot.physicalToSquare = 36U;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"th\":"));
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"c\":[\"e2\",\"e4\",\"--\"]"));
    snapshot.cpuThinking = {'w', 1U, 3U};
    snapshot.sequence = 1U;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
    snapshot.physicalMoveValid = false;
    snapshot.action = modules::HomeStateAction::Idle;
    snapshot.clockWinner = modules::GameWinner::Black;
    TEST_ASSERT_FALSE(modules::TryFormatHomeStateNotification(snapshot, line));
}

} // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(TestCpuThinkingIsAdditiveBoundedAndRetiresWithSearch);
    RUN_TEST(TestFormatsVersionedHomeStateLine);
    RUN_TEST(TestBoardBootstrapRetainsIndicesAndLogicalPalette);
    RUN_TEST(TestSharedPublicationQuietFallbackAndRecovery);
    RUN_TEST(TestPublisherSequencesAuthoritativeUndoState);
    RUN_TEST(TestPublisherSequencesAuthoritativeVexMoveFirstState);
    RUN_TEST(TestFormatsSequencesAndReplaysAuthoritativeCoachingState);
    RUN_TEST(TestRejectsMalformedAuthoritativeCoachingMask);
    RUN_TEST(TestFormatsAndSequencesAssistanceProjection);
    RUN_TEST(TestRejectsMalformedAssistanceProjection);
    RUN_TEST(TestFormatsAndSequencesPersistenceDegradation);
    RUN_TEST(TestFormatsAndSequencesFreshSetupLifecycle);
    RUN_TEST(TestFormatsAndSequencesQueuedCpuMovePrompt);
    RUN_TEST(TestFormatsAndSequencesLastMovesForBothSides);
    RUN_TEST(TestFormatsAndSequencesClockStateAndTimeoutWinner);
    RUN_TEST(TestPublisherSequencesStateChangesAndReplaysCurrentState);
    RUN_TEST(TestPublisherEmitsResetFenWithUnchangedPhysicalOccupancy);
    RUN_TEST(TestPublisherKeepsOpeningFenWhenDuckVariantLatches);
    RUN_TEST(TestPresentationOwnsOnlyAValidUndecidedDuckCandidate);
    RUN_TEST(TestFormatterKeepsWorstCasePayloadBounded);
    RUN_TEST(TestPendingPromotionPromptKeepsPhysicalAuthorityBounded);
    RUN_TEST(TestDuckPlanSequencesOwnershipAndRetiresCoordinates);
#if defined(LITCH_ALL_NATIVE_AGGREGATE)
    // Unity's final summary makes PlatformIO conclude the enclosing aggregate
    // finished and terminate it before the remaining suites run. Preserve the
    // standalone summary, but let the aggregate own its single final result.
    return Unity.TestFailures == 0U ? 0 : 1;
#else
    return UNITY_END();
#endif
}
