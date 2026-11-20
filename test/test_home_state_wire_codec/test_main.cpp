#include <unity.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "modules/home_state_wire_codec.hpp"
#include "modules/initial_fen.hpp"

namespace {

modules::HomeStateNotification RepresentativeSnapshot() {
    modules::HomeStateNotification snapshot;
    snapshot.sequence = 17U;
    snapshot.action = modules::HomeStateAction::Cpu;
    snapshot.fen = modules::INITIAL_FEN;
    snapshot.occupiedMask = UINT64_C(0xFFFF00000000FFFF);
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.boardRotated = true;
    snapshot.freshSetup = true;
    snapshot.undoPending = true;
    snapshot.coachingStateValid = true;
    snapshot.coachingMask = 2U;
    snapshot.persistenceDegradation =
        modules::PersistenceDegradation::QuarantinedRecord;
    snapshot.physicalMoveValid = true;
    snapshot.physicalFromSquare = 12U;
    snapshot.physicalToSquare = 28U;
    snapshot.whiteLastMoveValid = true;
    snapshot.whiteLastMoveFromSquare = 52U;
    snapshot.whiteLastMoveToSquare = 36U;
    snapshot.lastMoveMostRecentSide = 'w';
    snapshot.clockStateValid = true;
    snapshot.clockEnabled = true;
    snapshot.clockRunning = true;
    snapshot.clockActiveSide = 'b';
    snapshot.clockTimerSetting = 3U;
    snapshot.clockSetupState = 'n';
    snapshot.clockWhiteRemainingSeconds = 298U;
    snapshot.clockBlackRemainingSeconds = 300U;
    snapshot.assistance.valid = true;
    snapshot.assistance.hintRequested = true;
    snapshot.assistance.manualAffordanceUseful = true;
    snapshot.assistance.presentation =
        modules::HomeAssistancePresentation::HintReady;
    snapshot.assistance.fromSquare = 52U;
    snapshot.assistance.toSquare = 36U;
    return snapshot;
}

void TestRepresentativeSnapshot() {
    modules::home_state_wire::Payload payload;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(
        RepresentativeSnapshot(), payload));
    TEST_ASSERT_EQUAL_UINT32(100U, payload.size());
    std::ifstream fixture("test/fixtures/home_state_wire_v1.csv");
    TEST_ASSERT_TRUE(fixture.good());
    std::string line;
    std::getline(fixture, line);
    std::getline(fixture, line);
    const size_t comma = line.find(',');
    TEST_ASSERT_NOT_EQUAL(std::string::npos, comma);
    const std::string hex = line.substr(comma + 1U);
    TEST_ASSERT_EQUAL_UINT32(payload.size() * 2U, hex.size());
    for (size_t index = 0U; index < payload.size(); ++index) {
        const auto expected = static_cast<uint8_t>(
            std::stoul(hex.substr(index * 2U, 2U), nullptr, 16));
        TEST_ASSERT_EQUAL_HEX8(expected, payload[index]);
    }

    auto held = RepresentativeSnapshot();
    held.action = modules::HomeStateAction::Lift;
    held.variant = modules::GameVariant::Duck;
    held.duckSquare = 27U;
    held.freshSetup = false;
    held.physicalMoveValid = false;
    held.occupiedMask = (held.occupiedMask | (UINT64_C(1) << 27U)) &
                        ~(UINT64_C(1) << 52U);
    held.assistance.manualAffordanceUseful = false;
    held.assistance.presentation = modules::HomeAssistancePresentation::LegalGuide;
    held.assistance.legalSourceSquare = 52U;
    held.assistance.legalDestinationMask =
        (UINT64_C(1) << 36U) | (UINT64_C(1) << 44U);
    held.assistance.duckFromSquare = 27U;
    held.assistance.duckToSquare = 35U;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(held, payload));
    TEST_ASSERT_EQUAL_UINT8(27U, payload[88U]);
    TEST_ASSERT_EQUAL_UINT8(35U, payload[89U]);
    TEST_ASSERT_EQUAL_UINT8(52U, payload[90U]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, payload[96U]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, payload[97U]);

    held.assistance.duckToSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.duckToSquare = 64U;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.duckToSquare = 27U;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.duckToSquare = 35U;
    held.assistance.fromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    held.assistance.toSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.fromSquare = 52U;
    held.assistance.toSquare = 36U;
    held.assistance.hintRequested = false;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.hintRequested = true;
    held.variant = modules::GameVariant::Classic;
    held.duckSquare = modules::INVALID_DUCK_SQUARE;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(held, payload));
    held.assistance.duckFromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    held.assistance.duckToSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(held, payload));
}

void TestKingQueenAuthorityPublishesWithoutOccupancyChange() {
    auto snapshot = RepresentativeSnapshot();
    snapshot.action = modules::HomeStateAction::Correction;
    snapshot.physicalMoveValid = false;
    modules::HomeStateNotificationPublisher publisher;
    modules::HomeStateNotificationLine line;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    const uint32_t previousSequence = publisher.Sequence();
    snapshot.kingQueenSwapPromptActive = true;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_EQUAL_UINT32(previousSequence + 1U, publisher.Sequence());
    TEST_ASSERT_NOT_NULL(std::strstr(line.CStr(), "\"kq\":1"));
    modules::home_state_wire::Payload payload;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, payload));
    TEST_ASSERT_EQUAL_HEX8(0x80U, payload[6U] & 0x80U);
    snapshot.kingQueenSwapPromptActive = false;
    TEST_ASSERT_TRUE(publisher.TryPrepare(snapshot, false, line));
    TEST_ASSERT_NULL(std::strstr(line.CStr(), "\"kq\":1"));
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, payload));
    TEST_ASSERT_EQUAL_HEX8(0U, payload[6U] & 0x80U);
}

void TestInvalidSnapshotIsFailureAtomic() {
    modules::home_state_wire::Payload payload;
    payload.fill(0x5aU);
    auto snapshot = RepresentativeSnapshot();
    snapshot.fen = nullptr;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(snapshot, payload));
    for (const uint8_t byte : payload) {
        TEST_ASSERT_EQUAL_HEX8(0x5aU, byte);
    }
}

void TestClassicResultUsesOnlyIdleCpuTiming() {
    modules::HomeStateNotification snapshot;
    snapshot.variant = modules::GameVariant::Classic;
    snapshot.fen = "k7/n1K5/1N6/8/8/8/8/8 b - - 0 1";
    modules::home_state_wire::Payload ongoing, terminal;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, ongoing));
    for (const auto result : {modules::HomeClassicResult::Checkmate, modules::HomeClassicResult::Stalemate,
                              modules::HomeClassicResult::Draw}) {
        snapshot.classicResult = result;
        TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, terminal));
        auto expected = ongoing;
        expected[71U] = static_cast<uint8_t>(result);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), terminal.data(), terminal.size());
    }
    snapshot.cpuThinking.side = 'b';
    snapshot.cpuThinking.elapsedHalfSeconds = 0x4030201U;
    TEST_ASSERT_FALSE(modules::home_state_wire::Encode(snapshot, terminal));
    snapshot.classicResult = modules::HomeClassicResult::Ongoing;
    TEST_ASSERT_TRUE(modules::home_state_wire::Encode(snapshot, terminal));
    const uint8_t elapsed[] = {1U, 2U, 3U, 4U};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(elapsed, terminal.data() + 71U, sizeof(elapsed));
}

} // namespace

int main() {
    UNITY_BEGIN();
    RUN_TEST(TestRepresentativeSnapshot);
    RUN_TEST(TestInvalidSnapshotIsFailureAtomic);
    RUN_TEST(TestKingQueenAuthorityPublishesWithoutOccupancyChange);
    RUN_TEST(TestClassicResultUsesOnlyIdleCpuTiming);
    return UNITY_END();
}
