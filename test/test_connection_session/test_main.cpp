#include <algorithm>
#include <array>
#include <cstdio>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/button_state_wire_codec.hpp"
#include "modules/connection_session.hpp"
#include "modules/physical_input_state_wire_codec.hpp"
#include "modules/settings_state_wire_codec.hpp"

namespace modules {
struct ConnectionSessionCreditTestPeer {
    static void SetFinalReservation(ConnectionSession& session) {
        session.m_objectAcceptedThrough = connection_session_codec::LAST_SEQUENCE - 1U;
        session.m_objectGrantThrough = connection_session_codec::LAST_SEQUENCE;
    }
    static void SetReplayCounterNearSaturation(ConnectionSession& session) {
        session.m_successfulReverseObjectReplays = UINT32_MAX - 1U;
    }
};
} // namespace modules

namespace {
namespace wire = modules::connection_session_codec;

int failures = 0;

void Check(const bool value, const char* const message) {
    if (value) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

struct Encoded final {
    std::array<uint8_t, 20U> bytes{};
    size_t size = 0U;
};

Encoded Encode(const wire::PacketView& packet) {
    Encoded encoded;
    Check(wire::Codec::Encode(
              packet, {encoded.bytes.data(), encoded.bytes.size()},
              encoded.size) == wire::Error::None,
          "packet encodes within the minimum payload");
    return encoded;
}

wire::PacketView Take(modules::ConnectionSession& session,
                      const uint32_t generation,
                      std::array<uint8_t, 20U>& bytes) {
    size_t size = 0U;
    Check(session.PeekOutput(generation, bytes.data(), bytes.size(), size),
          "expected output is pending");
    wire::PacketView packet;
    Check(wire::Codec::Decode({bytes.data(), size}, packet, 20U) ==
              wire::Error::None,
          "runtime output decodes");
    session.AcceptOutput(generation);
    return packet;
}

wire::PacketView TakeObjectReceipt(
    modules::ConnectionSession& session, const uint32_t generation,
    std::array<uint8_t,
               modules::ConnectionSession::REVERSE_OBJECT_PACKET_BYTES>& bytes) {
    size_t size = 0U;
    Check(session.PeekOutput(generation, bytes.data(), bytes.size(), size),
          "expected Object receipt is pending");
    wire::PacketView packet;
    Check(wire::Codec::Decode({bytes.data(), size}, packet, 512U) ==
              wire::Error::None,
          "Object receipt decodes at the negotiated packet maximum");
    session.AcceptOutput(generation);
    return packet;
}

void ActivationOrderedOutcomeAndRecovery() {
    modules::ConnectionSession session;
    constexpr uint32_t generation = 7U;
    constexpr uint64_t bootNonce = UINT64_C(0x8899aabbccddeeff);
    constexpr uint64_t clientNonce = UINT64_C(0x1122334455667788);
    Check(session.Begin(generation, bootNonce, 512U, 41U),
          "session begins with negotiated interactive and object transport");

    std::array<uint8_t, 10U> body{};
    fxe::WriteU64LE(clientNonce, body.data());
    fxe::WriteU16LE(512U, body.data() + 8U);
    const auto hello = Encode({{wire::Kind::Control,
                                  uint8_t(wire::Control::Hello), 0U, 0U},
                                 {body.data(), body.size()}});
    Check(session.Ingress(generation, hello.bytes.data(), hello.size) ==
              modules::ConnectionSession::Result::Ok,
          "hello starts negotiation");

    std::array<uint8_t, 20U> welcomeBytes{};
    const auto welcome = Take(session, generation, welcomeBytes);
    std::array<uint8_t, 20U> identityBytes{};
    const auto identity = Take(session, generation, identityBytes);
    Check(welcome.header.code == uint8_t(wire::Control::Welcome) &&
              identity.header.code == uint8_t(wire::Control::Identity) &&
              welcome.header.token == identity.header.token &&
              fxe::ReadU64LE(identity.body.Data()) == bootNonce,
          "welcome and boot identity share one fresh control token");
    Check((fxe::ReadU16LE(identity.body.Data() + 8U) & modules::ConnectionSession::FEATURE_ORDERED_MODE) != 0U,
          "Identity advertises credit-independent compact Ordered mode delivery");

    std::array<uint8_t, 60U> transcript{};
    std::copy_n(hello.bytes.begin(), hello.size, transcript.begin());
    std::copy(welcomeBytes.begin(), welcomeBytes.end(), transcript.begin() + 20U);
    std::copy(identityBytes.begin(), identityBytes.end(), transcript.begin() + 40U);
    std::array<uint8_t, 4U> crc{};
    fxe::WriteU32LE(fxe::Crc32(transcript.data(), transcript.size()), crc.data());
    const auto ready = Encode({{wire::Kind::Control,
                                  uint8_t(wire::Control::Ready),
                                  welcome.header.token, 0U},
                                 {crc.data(), crc.size()}});
    Check(session.Ingress(generation, ready.bytes.data(), ready.size) ==
              modules::ConnectionSession::Result::Ok,
          "exact transcript activates session");
    std::array<uint8_t, 20U> output{};
    const auto activated = Take(session, generation, output);
    Check(activated.header.code == uint8_t(wire::Control::Activated) &&
              session.IsActivated(),
          "activated is emitted only after transcript validation");

    const auto stateOpen = Take(session, generation, output);
    Check(stateOpen.header.code == uint8_t(wire::Control::Open) &&
              stateOpen.header.sequence == 1U &&
              fxe::ReadU16LE(stateOpen.body.Data()) ==
                  modules::ConnectionSession::LITCHCRAFT_STATE_TOPIC &&
              stateOpen.body[3U] ==
                  modules::ConnectionSession::STATE_CAPACITY &&
              fxe::ReadU16LE(stateOpen.body.Data() + 4U) ==
                  modules::ConnectionSession::STATE_BODY_BYTES,
          "board opens one bounded multi-key reverse state stream");
    constexpr uint32_t stateStreamToken = 0x80000001U;
    std::array<uint8_t, 10U> stateOpenedBody{};
    fxe::WriteU32LE(stateStreamToken, stateOpenedBody.data());
    fxe::WriteU16LE(modules::ConnectionSession::STATE_BODY_BYTES,
                    stateOpenedBody.data() + 4U);
    stateOpenedBody[6U] = modules::ConnectionSession::STATE_CAPACITY;
    stateOpenedBody[7U] = 1U;
    const auto stateOpened = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Opened),
          welcome.header.token, stateOpen.header.sequence},
         {stateOpenedBody.data(), stateOpenedBody.size()}});
    Check(session.Ingress(generation, stateOpened.bytes.data(),
                          stateOpened.size) ==
              modules::ConnectionSession::Result::Ok &&
              session.StateStreamToken() == stateStreamToken,
          "central-issued state token completes the reverse stream");

    session.Update(generation);
    const auto reverseObjectOpen = Take(session, generation, output);
    Check(reverseObjectOpen.header.code == uint8_t(wire::Control::Open) &&
              reverseObjectOpen.header.sequence == 2U &&
              fxe::ReadU16LE(reverseObjectOpen.body.Data()) ==
                  modules::ConnectionSession::LITCHCRAFT_REVERSE_OBJECT_TOPIC &&
              reverseObjectOpen.body[3U] ==
                  modules::ConnectionSession::REVERSE_OBJECT_CAPACITY &&
              fxe::ReadU16LE(reverseObjectOpen.body.Data() + 4U) == 502U,
          "board opens a bounded reverse Object stream on the shared endpoint");
    constexpr uint32_t reverseObjectToken = 0x80000002U;
    std::array<uint8_t, 10U> reverseObjectOpenedBody{};
    fxe::WriteU32LE(reverseObjectToken, reverseObjectOpenedBody.data());
    fxe::WriteU16LE(502U, reverseObjectOpenedBody.data() + 4U);
    reverseObjectOpenedBody[6U] =
        modules::ConnectionSession::REVERSE_OBJECT_CAPACITY;
    reverseObjectOpenedBody[7U] = 1U;
    const auto reverseObjectOpened = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Opened),
          welcome.header.token, reverseObjectOpen.header.sequence},
         {reverseObjectOpenedBody.data(), reverseObjectOpenedBody.size()}});
    Check(session.Ingress(generation, reverseObjectOpened.bytes.data(),
                          reverseObjectOpened.size) ==
                  modules::ConnectionSession::Result::Ok &&
              session.ReverseObjectStreamToken() == reverseObjectToken &&
              reverseObjectOpen.body[3U] == 8U,
          "central-issued token grants eight reverse Object positions");

    const auto heartbeat = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Heartbeat),
          welcome.header.token, 0U}, {}});
    Check(session.Ingress(generation, heartbeat.bytes.data(), heartbeat.size) ==
              modules::ConnectionSession::Result::Ok,
          "active connection token admits a reply-free heartbeat");
    size_t heartbeatOutputSize = 0U;
    Check(!session.PeekOutput(generation, output.data(), output.size(),
                              heartbeatOutputSize),
          "heartbeat does not create response traffic");
    const auto wrongHeartbeat = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Heartbeat),
          welcome.header.token + 1U, 0U}, {}});
    Check(session.Ingress(generation, wrongHeartbeat.bytes.data(),
                          wrongHeartbeat.size) ==
              modules::ConnectionSession::Result::Unexpected,
          "heartbeat is fenced by the active connection token");

    const std::array<uint8_t, 4U> stateBody = {2U, 3U, 4U, 5U};
    Check(session.PublishState(
              modules::ConnectionSession::LITCHCRAFT_STATE_CODE, 9U,
              stateBody.data(), stateBody.size()),
          "latest authoritative state is retained");
    const auto state = Take(session, generation, output);
    Check(state.header.kind == wire::Kind::State &&
              state.header.token == stateStreamToken &&
              state.header.sequence == 9U && state.body.Size() == 4U &&
              state.body[3U] == 5U,
          "state is emitted on the reverse stream");
    Check(session.PublishState(
              modules::ConnectionSession::LITCHCRAFT_STATE_CODE, 9U,
              stateBody.data(), stateBody.size()),
          "an exact retained state repeat is accepted");
    size_t duplicateSize = 0U;
    Check(!session.PeekOutput(generation, output.data(), output.size(),
                              duplicateSize),
          "an exact retained state repeat creates no idle notification");
    const auto stateQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          stateStreamToken, 0U}, {}});
    Check(session.Ingress(generation, stateQuery.bytes.data(),
                          stateQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "zero query requests retained state");
    const auto replayedState = Take(session, generation, output);
    Check(replayedState.header.kind == wire::Kind::State &&
              replayedState.header.sequence == 9U &&
              replayedState.body[0U] == 2U,
          "query replays exact retained state without domain work");

    const std::array<uint8_t, 3U> healthBody = {7U, 8U, 9U};
    Check(session.PublishState(2U, 4U, healthBody.data(), healthBody.size()),
          "a second semantic key is retained independently");
    const auto healthState = Take(session, generation, output);
    Check(healthState.header.code == 2U &&
              healthState.header.sequence == 4U &&
              healthState.body[2U] == 9U,
          "second semantic state publishes without replacing Litchcraft");
    const auto litchcraftQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          stateStreamToken,
          modules::ConnectionSession::LITCHCRAFT_STATE_CODE}, {}});
    Check(session.Ingress(generation, litchcraftQuery.bytes.data(),
                          litchcraftQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "a semantic-key query requests only its retained state");
    const auto targetedState = Take(session, generation, output);
    Check(targetedState.header.code ==
              modules::ConnectionSession::LITCHCRAFT_STATE_CODE &&
              targetedState.header.sequence == 9U,
          "targeted replay does not rerun or overwrite another state owner");
    const std::array<uint8_t, 10U> homeFragment = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    Check(session.PublishState(4U, 31U, homeFragment.data(),
                               homeFragment.size()) &&
              session.PublishState(5U, 31U, homeFragment.data(),
                                   homeFragment.size()) &&
              session.PublishState(6U, 31U, homeFragment.data(),
                                   homeFragment.size()),
          "multiple Home fragments can be pending together");
    const std::array<uint8_t, 6U> degradedHealth = {
        1U, 5U, 0U, 2U, 1U, 0U};
    Check(session.PublishState(2U, 6U, degradedHealth.data(),
                               degradedHealth.size()),
          "changed runtime health can arrive behind Home fragments");
    const auto priorityHealth = Take(session, generation, output);
    const auto remainingHome = Take(session, generation, output);
    const auto remainingHome2 = Take(session, generation, output);
    const auto remainingHome3 = Take(session, generation, output);
    Check(priorityHealth.header.code == 2U &&
              priorityHealth.header.sequence == 6U &&
              remainingHome.header.code >= 4U &&
              remainingHome.header.code <= 6U &&
              remainingHome2.header.code >= 4U &&
              remainingHome2.header.code <= 6U &&
              remainingHome3.header.code >= 4U &&
              remainingHome3.header.code <= 6U,
          "runtime degradation preempts Home backlog without dropping it");

    std::array<uint8_t, 8U> openBody{};
    fxe::WriteU16LE(41U, openBody.data());
    openBody[2U] = 0U;
    openBody[3U] = 8U;
    fxe::WriteU16LE(10U, openBody.data() + 4U);
    const auto open = Encode({{wire::Kind::Control,
                                 uint8_t(wire::Control::Open),
                                 welcome.header.token, 1U},
                                {openBody.data(), openBody.size()}});
    Check(session.Ingress(generation, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok,
          "ordered Litchcraft topic opens");
    const auto opened = Take(session, generation, output);
    const uint32_t streamToken = fxe::ReadU32LE(opened.body.Data());
    Check(opened.header.code == uint8_t(wire::Control::Opened) &&
              streamToken != 0U && streamToken != welcome.header.token,
          "opened returns a distinct bounded stream token");
    const auto initialProgress = Take(session, generation, output);
    Check(initialProgress.header.code == uint8_t(wire::Control::Progress) &&
              fxe::ReadU32LE(initialProgress.body.Data() + 4U) == 8U,
          "initial semantic grant reserves the fixed window");

    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_OBJECT_TOPIC,
                    openBody.data());
    openBody[2U] = 1U;
    openBody[3U] = wire::MAX_WINDOW;
    fxe::WriteU16LE(502U, openBody.data() + 4U);
    const auto objectOpen = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Open),
          welcome.header.token, 2U},
         {openBody.data(), openBody.size()}});
    Check(session.Ingress(generation, objectOpen.bytes.data(),
                          objectOpen.size) ==
              modules::ConnectionSession::Result::Ok,
          "bulk Litchcraft object topic opens on its ordinary lane");
    const auto objectOpened = Take(session, generation, output);
    const uint32_t objectToken = fxe::ReadU32LE(objectOpened.body.Data());
    Check(objectOpened.header.sequence == 2U && objectOpened.body[7U] == 1U &&
              objectOpened.body[6U] == modules::ConnectionSession::OBJECT_CAPACITY &&
              fxe::ReadU16LE(objectOpened.body.Data() + 4U) == 502U &&
              objectToken == session.ObjectStreamToken(),
          "object stream returns the negotiated ATT-sized body grant");

    std::array<uint8_t, 502U> storeBegin{};
    storeBegin[0U] = 0x4dU;
    storeBegin[1U] = 2U;
    storeBegin[2U] = 18U;
    std::array<uint8_t, 512U> objectBytes{};
    size_t objectSize = 0U;
    const wire::PacketView objectPacket{
        {wire::Kind::Object, 18U, objectToken, 1U},
        {storeBegin.data(), storeBegin.size()}};
    Check(wire::Codec::Encode(
              objectPacket, {objectBytes.data(), objectBytes.size()},
              objectSize) == wire::Error::None,
          "store record fits its object envelope");
    modules::ConnectionSession::ObjectCommand objectCommand;
    Check(session.Ingress(generation, objectBytes.data(), objectSize,
                          &objectCommand) ==
                  modules::ConnectionSession::Result::Ok &&
              objectCommand.opcode == 18U && objectCommand.body.Size() == 502U,
          "object is proposed to the existing Store ingress owner");
    session.RejectObject(objectToken, 1U);
    Check(session.Ingress(generation, objectBytes.data(), objectSize,
                          &objectCommand) ==
                  modules::ConnectionSession::Result::Ok &&
              session.AcceptObject(objectToken, 1U) ==
                  modules::ConnectionSession::Result::Ok,
          "downstream refusal leaves the exact object retry admissible");
    const auto objectProgress = Take(session, generation, output);
    Check(objectProgress.header.code == uint8_t(wire::Control::Progress) &&
              objectProgress.header.token == objectToken &&
              objectProgress.header.sequence == 1U &&
              fxe::ReadU32LE(objectProgress.body.Data()) == 1U &&
              fxe::ReadU32LE(objectProgress.body.Data() + 4U) ==
                  1U + modules::ConnectionSession::OBJECT_CAPACITY &&
              fxe::ReadU16LE(objectProgress.body.Data() + 8U) == 0U,
          "accepted object advances cumulative admission credit");
    objectCommand = {};
    Check(session.Ingress(generation, objectBytes.data(), objectSize,
                          &objectCommand) ==
                  modules::ConnectionSession::Result::Ok &&
              objectCommand.streamToken == 0U,
          "accepted exact object replay is suppressed");
    objectBytes[12U] ^= 1U;
    Check(session.Ingress(generation, objectBytes.data(), objectSize,
                          &objectCommand) ==
              modules::ConnectionSession::Result::Conflict,
          "changed duplicate object cannot mutate the accepted position");
    const auto objectQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query), objectToken, 0U},
         {}});
    Check(session.Ingress(generation, objectQuery.bytes.data(),
                          objectQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "object query requests retained admission progress");
    const auto replayedObjectProgress = Take(session, generation, output);
    Check(replayedObjectProgress.header.token == objectToken &&
              replayedObjectProgress.header.sequence == 1U &&
              fxe::ReadU32LE(replayedObjectProgress.body.Data() + 4U) ==
                  1U + modules::ConnectionSession::OBJECT_CAPACITY,
          "object query replays the cumulative credit watermark");
    const auto domainReceiptQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query), objectToken, 7U},
         {}});
    Check(session.Ingress(generation, domainReceiptQuery.bytes.data(),
                          domainReceiptQuery.size) ==
                  modules::ConnectionSession::Result::Ok &&
              session.TakeRequestedObjectReceipt() == 7U &&
              session.TakeRequestedObjectReceipt() == 0U,
          "specific Object Query reaches the retained domain receipt owner exactly once");

    std::array<uint8_t, 19U> storeState{};
    storeState[0U] = 0x4dU;
    storeState[1U] = 2U;
    storeState[2U] = 0x8dU;
    fxe::WriteU16LE(7U, storeState.data() + 11U);
    Check(session.PublishReverseObject(0x8dU, 7U, storeState.data(),
                                       storeState.size()),
          "Store worker receipt transfers into the reverse Object window");
    Check(!session.HasBulkReverseObject(), "ordinary retained Store status is not bulk activity");
    std::array<uint8_t,
               modules::ConnectionSession::REVERSE_OBJECT_PACKET_BYTES>
        receiptBytes{};
    const auto receipt = TakeObjectReceipt(session, generation, receiptBytes);
    Check(receipt.header.kind == wire::Kind::Object &&
              receipt.header.code == 0x8dU &&
              receipt.header.token == reverseObjectToken &&
              receipt.header.sequence == 1U && receipt.body.Size() == 19U &&
              fxe::ReadU16LE(receipt.body.Data() + 11U) == 7U,
          "exact Store receipt leaves on the shared session endpoint");
    Check(session.SuccessfulReverseObjectReplays() == 0U,
          "first accepted native reverse send is not a replay");
    const auto receiptQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          reverseObjectToken, 1U},
         {}});
    Check(session.Ingress(generation, receiptQuery.bytes.data(),
                          receiptQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "request Query re-arms the exact retained Object receipt");
    size_t refusedSize = 0U;
    Check(session.PeekOutput(generation, receiptBytes.data(), receiptBytes.size(), refusedSize) &&
          session.SuccessfulReverseObjectReplays() == 0U,
          "a selected retry without native acceptance does not count as a successful replay");
    session.AcceptOutput(generation + 1U);
    Check(session.SuccessfulReverseObjectReplays() == 0U,
          "stale native acceptance cannot increment current-session replay diagnostics");
    const auto replayedReceipt =
        TakeObjectReceipt(session, generation, receiptBytes);
    Check(replayedReceipt.header.sequence == 1U &&
              replayedReceipt.body[2U] == 0x8dU,
          "lost reverse Object recovers by transport sequence without replaying Commit");
    Check(session.SuccessfulReverseObjectReplays() == 1U,
          "accepted retransmission of a previously transmitted retained slot counts exactly once");
    modules::ConnectionSessionCreditTestPeer::SetReplayCounterNearSaturation(session);
    for (unsigned repeat = 0U; repeat < 2U; ++repeat) {
        Check(session.Ingress(generation, receiptQuery.bytes.data(), receiptQuery.size) ==
                  modules::ConnectionSession::Result::Ok,
              "exact retained Query remains admissible at diagnostic saturation");
        (void)TakeObjectReceipt(session, generation, receiptBytes);
        Check(session.SuccessfulReverseObjectReplays() == UINT32_MAX,
              "successful replay diagnostic saturates without wrapping or changing retained delivery");
    }

    for (uint16_t request = 8U; request <= 14U; ++request) {
        fxe::WriteU16LE(request, storeState.data() + 11U);
        Check(session.PublishReverseObject(
                  0x8dU, request, storeState.data(), storeState.size(), true),
              "reverse Object fills each negotiated retained slot");
    }
    fxe::WriteU16LE(15U, storeState.data() + 11U);
    Check(!session.PublishReverseObject(
              0x8dU, 15U, storeState.data(), storeState.size()),
          "a ninth reverse Object receives bounded backpressure");
    const std::array<uint8_t, 2U> urgentHealth = {1U, 9U};
    Check(session.PublishState(
              modules::ConnectionSession::RUNTIME_HEALTH_STATE_CODE, 12U,
              urgentHealth.data(), urgentHealth.size()),
          "interactive health can become pending beside reverse bulk");
    const auto healthBeforeBulk = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(healthBeforeBulk.header.kind == wire::Kind::State &&
              healthBeforeBulk.header.code ==
                  modules::ConnectionSession::RUNTIME_HEALTH_STATE_CODE,
          "health remains ahead of a full reverse Object window");
    const auto receiptQueryAll = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          reverseObjectToken, 0U},
         {}});
    bool retainedWholeHome = true;
    for (uint8_t code = 4U; code <= 13U; ++code) {
        retainedWholeHome = retainedWholeHome &&
            session.PublishState(code, 32U, homeFragment.data(),
                                 homeFragment.size());
    }
    Check(retainedWholeHome,
          "new multi-fragment Home truth can become pending beside reverse bulk");
    uint16_t deliveredHomeCodes = 0U;
    uint8_t homePackets = 0U;
    uint8_t interleavedBulkPackets = 0U;
    for (uint8_t count = 0U; count < 14U && homePackets < 10U; ++count) {
        const auto mixedOutput = TakeObjectReceipt(
            session, generation, receiptBytes);
        if (mixedOutput.header.kind == wire::Kind::State &&
            mixedOutput.header.code >= 4U &&
            mixedOutput.header.code <= 13U &&
            mixedOutput.header.sequence == 32U) {
            ++homePackets;
            deliveredHomeCodes = static_cast<uint16_t>(
                deliveredHomeCodes |
                (1U << (mixedOutput.header.code - 4U)));
        } else if (mixedOutput.header.kind == wire::Kind::Object &&
                   mixedOutput.header.token == reverseObjectToken) {
            ++interleavedBulkPackets;
            Check(session.Ingress(generation, receiptQueryAll.bytes.data(),
                                  receiptQueryAll.size) ==
                      modules::ConnectionSession::Result::Ok,
                  "continuous reverse bulk is replenished during state load");
        }
    }
    Check(homePackets == 10U && deliveredHomeCodes == 0x03ffU &&
              interleavedBulkPackets >= 3U,
          "complete Home truth arrives within fourteen mixed outputs without starving reverse bulk");
    const std::array<uint8_t, 10U> liveLitchcraftState = {
        0U, 0U, 0U, 0U, 1U, 15U, 1U, 2U, 3U, 0U};
    const std::array<uint8_t, 10U> physicalEditState = {
        1U, 0U, 0U, 0U, 0U, 8U, 31U, 0U, 0U, 0U};
    Check(session.PublishState(
              modules::ConnectionSession::LITCHCRAFT_STATE_CODE, 33U,
              liveLitchcraftState.data(), liveLitchcraftState.size()) &&
              session.PublishState(
                  modules::ConnectionSession::LITCHCRAFT_STORE_EDIT_STATE_CODE,
                  34U, physicalEditState.data(), physicalEditState.size()),
          "physical Litchcraft truth can become pending beside reverse bulk");
    uint32_t litchcraftStateCodes = 0U;
    for (uint8_t count = 0U; count < 3U &&
         litchcraftStateCodes !=
             ((UINT32_C(1) << modules::ConnectionSession::LITCHCRAFT_STATE_CODE) |
              (UINT32_C(1) << modules::ConnectionSession::LITCHCRAFT_STORE_EDIT_STATE_CODE));
         ++count) {
        const auto mixedOutput = TakeObjectReceipt(
            session, generation, receiptBytes);
        if (mixedOutput.header.kind == wire::Kind::State) {
            litchcraftStateCodes |= UINT32_C(1) << mixedOutput.header.code;
        } else if (mixedOutput.header.kind == wire::Kind::Object) {
            Check(session.Ingress(generation, receiptQueryAll.bytes.data(),
                                  receiptQueryAll.size) ==
                      modules::ConnectionSession::Result::Ok,
                  "continuous bulk remains recoverable beside Litchcraft truth");
        }
    }
    Check(litchcraftStateCodes ==
              ((UINT32_C(1) << modules::ConnectionSession::LITCHCRAFT_STATE_CODE) |
               (UINT32_C(1) << modules::ConnectionSession::LITCHCRAFT_STORE_EDIT_STATE_CODE)),
          "both physical Litchcraft states arrive within three mixed outputs");
    const auto packedPhysical =
        modules::physical_input_state_wire::Encode(
            UINT64_C(0x8000000000000001),
            static_cast<uint16_t>((1U << 3U) | (1U << 11U)));
    Check(session.PublishState(
              modules::physical_input_state_wire::STATE_CODE, 35U,
              packedPhysical.data(), packedPhysical.size()),
          "all sampled physical inputs fit one retained state packet");
    auto physicalBeforeBulk = TakeObjectReceipt(
        session, generation, receiptBytes);
    if (physicalBeforeBulk.header.kind == wire::Kind::Object) {
        Check(session.Ingress(generation, receiptQueryAll.bytes.data(),
                              receiptQueryAll.size) ==
                  modules::ConnectionSession::Result::Ok,
              "continuous bulk is replenished beside packed physical truth");
        physicalBeforeBulk = TakeObjectReceipt(
            session, generation, receiptBytes);
    }
    Check(physicalBeforeBulk.header.kind == wire::Kind::State &&
              physicalBeforeBulk.header.code ==
                  modules::physical_input_state_wire::STATE_CODE &&
              physicalBeforeBulk.header.sequence == 35U &&
              fxe::ReadU64LE(physicalBeforeBulk.body.Data()) ==
                  UINT64_C(0x8000000000000001) &&
              fxe::ReadU16LE(physicalBeforeBulk.body.Data() + 8U) ==
                  static_cast<uint16_t>((1U << 3U) | (1U << 11U)),
          "packed physical truth remains ahead of a full reverse Object window");
    const modules::ButtonEvent p1Up{
        modules::UserButton::Up, fxe::Button::Event::Press,
        modules::UiOwner::P1};
    const modules::ButtonEvent p2O{
        modules::UserButton::P2O, fxe::Button::Event::Press,
        modules::UiOwner::P2};
    const auto p1UpBody = modules::button_state_wire_codec::Encode(
        p1Up, true, 101U);
    const auto p2OBody = modules::button_state_wire_codec::Encode(
        p2O, true, 102U);
    Check(modules::button_state_wire_codec::SwitchIndex(p1Up) == 0 &&
              modules::button_state_wire_codec::SwitchIndex(p2O) == 13 &&
              modules::physical_input_state_wire::PackedSwitchIndex(p1Up) ==
                  0 &&
              modules::physical_input_state_wire::PackedSwitchIndex(p2O) ==
                  11 &&
              session.PublishState(
                  modules::button_state_wire_codec::FIRST_STATE_CODE, 13U,
                  p1UpBody.data(), p1UpBody.size()) &&
              session.PublishState(
                  modules::button_state_wire_codec::LAST_STATE_CODE, 14U,
                  p2OBody.data(), p2OBody.size()),
          "simultaneous logical switches retain independent current truth");
    auto firstSwitch = TakeObjectReceipt(
        session, generation, receiptBytes);
    if (firstSwitch.header.kind == wire::Kind::Object) {
        Check(session.Ingress(generation, receiptQueryAll.bytes.data(),
                              receiptQueryAll.size) ==
                  modules::ConnectionSession::Result::Ok,
              "continuous bulk is replenished beside legacy switch truth");
        firstSwitch = TakeObjectReceipt(session, generation, receiptBytes);
    }
    auto secondSwitch = TakeObjectReceipt(
        session, generation, receiptBytes);
    if (secondSwitch.header.kind == wire::Kind::Object) {
        Check(session.Ingress(generation, receiptQueryAll.bytes.data(),
                              receiptQueryAll.size) ==
                  modules::ConnectionSession::Result::Ok,
              "continuous bulk is replenished between legacy switch states");
        secondSwitch = TakeObjectReceipt(session, generation, receiptBytes);
    }
    const bool switchCodesComplete =
        (firstSwitch.header.code ==
             modules::button_state_wire_codec::FIRST_STATE_CODE &&
         secondSwitch.header.code ==
             modules::button_state_wire_codec::LAST_STATE_CODE) ||
        (secondSwitch.header.code ==
             modules::button_state_wire_codec::FIRST_STATE_CODE &&
         firstSwitch.header.code ==
             modules::button_state_wire_codec::LAST_STATE_CODE);
    Check(firstSwitch.header.kind == wire::Kind::State &&
              secondSwitch.header.kind == wire::Kind::State &&
              switchCodesComplete &&
              firstSwitch.body[3U] == 1U && secondSwitch.body[3U] == 1U,
          "each legacy switch state arrives within one bounded bulk interleave");
    const auto deliveredOne = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredTwo = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredThree = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredFour = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredFive = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredSix = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredSeven = TakeObjectReceipt(
        session, generation, receiptBytes);
    const auto deliveredEight = TakeObjectReceipt(
        session, generation, receiptBytes);
    const uint16_t deliveredSequences = static_cast<uint16_t>(
        (1U << (deliveredOne.header.sequence - 1U)) |
        (1U << (deliveredTwo.header.sequence - 1U)) |
        (1U << (deliveredThree.header.sequence - 1U)) |
        (1U << (deliveredFour.header.sequence - 1U)) |
        (1U << (deliveredFive.header.sequence - 1U)) |
        (1U << (deliveredSix.header.sequence - 1U)) |
        (1U << (deliveredSeven.header.sequence - 1U)) |
        (1U << (deliveredEight.header.sequence - 1U)));
    Check(deliveredOne.header.kind == wire::Kind::Object &&
              deliveredTwo.header.kind == wire::Kind::Object &&
              deliveredThree.header.kind == wire::Kind::Object &&
              deliveredFour.header.kind == wire::Kind::Object &&
              deliveredFive.header.kind == wire::Kind::Object &&
              deliveredSix.header.kind == wire::Kind::Object &&
              deliveredSeven.header.kind == wire::Kind::Object &&
              deliveredEight.header.kind == wire::Kind::Object &&
              deliveredSequences == 0x00ffU,
          "eight-slot window publishes each transport sequence once");
    Check(session.HasBulkReverseObject(),
          "native submission retains bulk activity until cumulative acknowledgement");
    std::array<uint8_t, 10U> reverseProgressBody{};
    fxe::WriteU32LE(1U, reverseProgressBody.data());
    fxe::WriteU32LE(9U, reverseProgressBody.data() + 4U);
    fxe::WriteU16LE(0x007eU, reverseProgressBody.data() + 8U);
    const auto reverseHole = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Progress),
          reverseObjectToken, 1U},
         {reverseProgressBody.data(), reverseProgressBody.size()}});
    Check(session.Ingress(generation, reverseHole.bytes.data(),
                          reverseHole.size) ==
              modules::ConnectionSession::Result::Ok,
          "reverse Progress admits a selective missing-sequence report");
    const auto repairedTwo = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(repairedTwo.header.sequence == 2U,
          "only the missing reverse Object is repaired");
    size_t noDuplicateBulkSize = 0U;
    if (session.PeekOutput(generation, receiptBytes.data(),
                           receiptBytes.size(), noDuplicateBulkSize)) {
        wire::PacketView unrelated;
        Check(wire::Codec::Decode(
                  {receiptBytes.data(), noDuplicateBulkSize}, unrelated,
                  512U) == wire::Error::None &&
                  unrelated.header.token != reverseObjectToken,
              "already received reverse Objects are not replayed");
        session.AcceptOutput(generation);
    }
    fxe::WriteU32LE(8U, reverseProgressBody.data());
    fxe::WriteU32LE(16U, reverseProgressBody.data() + 4U);
    fxe::WriteU16LE(0U, reverseProgressBody.data() + 8U);
    const auto reverseConfirmed = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Progress),
          reverseObjectToken, 8U},
         {reverseProgressBody.data(), reverseProgressBody.size()}});
    Check(session.Ingress(generation, reverseConfirmed.bytes.data(),
                          reverseConfirmed.size) ==
                  modules::ConnectionSession::Result::Ok &&
              session.PublishReverseObject(
                  0x8dU, 15U, storeState.data(), storeState.size()),
          "cumulative confirmation frees the prefix and extends reverse credit");
    Check(!session.HasBulkReverseObject(),
          "acknowledged bulk tail retires without treating a new status receipt as bulk");
    const auto sequenceNine = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(sequenceNine.header.sequence == 9U,
          "freed reverse slot carries the next immutable sequence");
    fxe::WriteU32LE(9U, reverseProgressBody.data());
    fxe::WriteU32LE(17U, reverseProgressBody.data() + 4U);
    const auto reverseNineConfirmed = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Progress),
          reverseObjectToken, 9U},
         {reverseProgressBody.data(), reverseProgressBody.size()}});
    Check(session.Ingress(generation, reverseNineConfirmed.bytes.data(),
                          reverseNineConfirmed.size) ==
              modules::ConnectionSession::Result::Ok,
          "next cumulative reverse confirmation retires its exact slot");

    std::array<uint8_t, 502U> largeStoreFragment{};
    largeStoreFragment[0U] = 1U;
    largeStoreFragment[1U] = 3U;
    fxe::WriteU16LE(498U, largeStoreFragment.data() + 2U);
    Check(session.PublishReverseObject(
              0x90U, 16U, largeStoreFragment.data(),
              largeStoreFragment.size()),
          "large Store chunk enters the same reverse Object window");
    const auto largeReceipt = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(largeReceipt.header.sequence == 10U &&
              largeReceipt.body.Size() == 502U &&
              largeReceipt.body[0U] == 1U &&
              fxe::ReadU16LE(largeReceipt.body.Data() + 2U) == 498U,
          "full 512-byte reverse packet remains intact through output");

    const std::array<uint8_t, 10U> storeAuthority = {
        1U, 0U, 62U, 0U, 0U, 0U, 62U, 0U, 0U, 0U};
    Check(session.PublishState(
              modules::ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE,
              62U, storeAuthority.data(), storeAuthority.size()),
          "Store authority is retained beside reverse artwork");
    const auto initialAuthority = Take(session, generation, output);
    Check(initialAuthority.header.kind == wire::Kind::State &&
              initialAuthority.header.code ==
                  modules::ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE,
          "initial Store authority is delivered");
    Check(session.PublishReverseObject(
              0x90U, 17U, largeStoreFragment.data(),
              largeStoreFragment.size()),
          "another reverse artwork packet occupies the output slot");
    size_t blockedBulkSize = 0U;
    Check(session.PeekOutput(generation, receiptBytes.data(),
                             receiptBytes.size(), blockedBulkSize),
          "reverse artwork is selected before simulated notify backpressure");
    wire::PacketView blockedBulk;
    Check(wire::Codec::Decode(
              {receiptBytes.data(), blockedBulkSize}, blockedBulk, 512U) ==
                  wire::Error::None &&
              blockedBulk.header.kind == wire::Kind::Object,
          "unaccepted artwork output models a failed BLE notify");
    const auto authorityQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          stateStreamToken,
          modules::ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE},
         {}});
    Check(session.Ingress(generation, authorityQuery.bytes.data(),
                          authorityQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "post-read authority query rearms retained State");
    const auto priorityAuthority = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(priorityAuthority.header.kind == wire::Kind::State &&
              priorityAuthority.header.code ==
                  modules::ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE,
          "queried authority preempts unsent artwork after notify backpressure");
    const auto resumedBulk = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(resumedBulk.header.kind == wire::Kind::Object &&
              resumedBulk.header.sequence == 11U,
          "preempted artwork remains queued for the next available notify");

    // A previously transmitted tail can be selected for repair while the
    // SDK is busy. Its earlier transmission may still be acknowledged before
    // that repair is accepted; the next upload must not inherit that selection.
    const auto reverseTailQuery = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Query),
          reverseObjectToken, 0U}, {}});
    Check(session.Ingress(generation, reverseTailQuery.bytes.data(),
                          reverseTailQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "snapshot tail repair re-arms retained reverse packets");
    size_t blockedRepairSize = 0U;
    Check(session.PeekOutput(generation, receiptBytes.data(),
                             receiptBytes.size(), blockedRepairSize),
          "tail repair remains selected after simulated notify refusal");
    wire::PacketView blockedRepair;
    Check(wire::Codec::Decode(
              {receiptBytes.data(), blockedRepairSize}, blockedRepair, 512U) ==
                  wire::Error::None &&
              blockedRepair.header.kind == wire::Kind::Object &&
              blockedRepair.header.sequence <= 11U,
          "blocked repair belongs to the preceding snapshot");
    fxe::WriteU32LE(11U, reverseProgressBody.data());
    fxe::WriteU32LE(19U, reverseProgressBody.data() + 4U);
    const auto tailConfirmed = Encode(
        {{wire::Kind::Control, uint8_t(wire::Control::Progress),
          reverseObjectToken, 11U},
         {reverseProgressBody.data(), reverseProgressBody.size()}});
    Check(session.Ingress(generation, tailConfirmed.bytes.data(),
                          tailConfirmed.size) ==
              modules::ConnectionSession::Result::Ok,
          "earlier successful tail transmission is cumulatively confirmed");
    fxe::WriteU32LE(2U, objectBytes.data() + 6U);
    Check(session.Ingress(generation, objectBytes.data(), objectSize,
                          &objectCommand) ==
                  modules::ConnectionSession::Result::Ok &&
              session.AcceptObject(objectToken, 2U) ==
                  modules::ConnectionSession::Result::Ok,
          "next artwork Begin is admitted after the snapshot tail");
    const auto nextUploadProgress = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(nextUploadProgress.header.kind == wire::Kind::Control &&
              nextUploadProgress.header.code == uint8_t(wire::Control::Progress) &&
              nextUploadProgress.header.token == objectToken &&
              nextUploadProgress.header.sequence == 2U,
          "confirmed blocked tail cannot suppress the next upload watermark");
    Check(session.Ingress(generation, objectQuery.bytes.data(),
                          objectQuery.size) ==
              modules::ConnectionSession::Result::Ok,
          "next upload Query remains available after reverse slot retirement");
    const auto nextUploadQuery = TakeObjectReceipt(
        session, generation, receiptBytes);
    Check(nextUploadQuery.header.token == objectToken &&
              nextUploadQuery.header.sequence == 2U,
          "next upload Query publishes its exact admission watermark");

    const std::array<uint8_t, 2U> actionBody = {0x31U, 0x32U};
    const wire::PacketView action{{wire::Kind::Ordered, 9U, streamToken, 1U},
                                  {actionBody.data(), actionBody.size()}};
    std::array<uint8_t, 20U> actionBytes{};
    size_t actionSize = 0U;
    Check(wire::Codec::Encode(action,
                              {actionBytes.data(), actionBytes.size()},
                              actionSize) == wire::Error::None,
          "ordered action encodes");
    Check(session.Ingress(generation, actionBytes.data(), actionSize) ==
              modules::ConnectionSession::Result::Ok,
          "ordered action is admitted");
    modules::ConnectionSession::OrderedCommand command;
    Check(session.BeginNext(command) ==
              modules::ConnectionSession::Result::Ok &&
              command.topic == 41U && command.opcode == 9U &&
              command.sequence == 1U && command.body.Size() == 2U &&
              command.body[0U] == 0x31U,
          "domain sees one typed contiguous command");
    Check(session.BeginNext(command) ==
              modules::ConnectionSession::Result::Unexpected,
          "polling cannot invoke in-flight work twice");
    Check(session.ReadApplying(command) && command.opcode == 9U &&
              command.sequence == 1U && command.body.Size() == 2U && command.body[0U] == 0x31U,
          "denied domain admission re-observes the exact retained applying position");

    using Window = fxe::OrderedReceiveWindow<
        modules::ConnectionSession::ORDERED_CAPACITY,
        modules::ConnectionSession::INTERACTIVE_PACKET_BYTES>;
    const Window::Outcome rejected{Window::Disposition::Rejected, 77U};
    Check(session.Complete(streamToken, 1U, rejected, 12U, 34U) ==
              modules::ConnectionSession::Result::Ok,
          "domain rejection completes the ordered position");
    Check(!session.ReadApplying(command),
          "completed position cannot replay through admission retry");
    std::array<uint8_t, 20U> rejectedOutcomeBytes{};
    std::array<uint8_t, 20U> rejectedCheckpointBytes{};
    std::array<uint8_t, 20U> rejectedProgressBytes{};
    const auto rejectedOutcome =
        Take(session, generation, rejectedOutcomeBytes);
    const auto rejectedCheckpoint =
        Take(session, generation, rejectedCheckpointBytes);
    const auto rejectedProgress =
        Take(session, generation, rejectedProgressBytes);
    Check(rejectedOutcome.header.code == uint8_t(wire::Control::Outcome) &&
              rejectedOutcome.header.sequence == 1U &&
              fxe::ReadU16LE(rejectedOutcome.body.Data() + 1U) == 77U &&
              rejectedCheckpoint.header.code ==
                  uint8_t(wire::Control::Checkpoint) &&
              rejectedCheckpoint.header.sequence == 1U &&
              (fxe::ReadU16LE(rejectedCheckpoint.body.Data() + 8U) & 1U) !=
                  0U &&
              rejectedProgress.header.code ==
                  uint8_t(wire::Control::Progress) &&
              rejectedProgress.header.sequence == 1U,
          "authoritative outcome and checkpoint precede window progress");

    const wire::PacketView query{{wire::Kind::Control,
                                  uint8_t(wire::Control::Query),
                                  streamToken, 0U}, {}};
    const auto queryBytes = Encode(query);
    Check(session.Ingress(generation, queryBytes.bytes.data(),
                          queryBytes.size) ==
              modules::ConnectionSession::Result::Ok,
          "zero query requests retained terminal evidence");
    std::array<uint8_t, 20U> replayedCheckpointBytes{};
    std::array<uint8_t, 20U> queriedProgressBytes{};
    const auto replayedCheckpoint =
        Take(session, generation, replayedCheckpointBytes);
    const auto queriedProgress =
        Take(session, generation, queriedProgressBytes);
    Check(replayedCheckpoint.header.code ==
                  uint8_t(wire::Control::Checkpoint) &&
              replayedCheckpoint.header.sequence == 1U &&
              fxe::ReadU32LE(replayedCheckpoint.body.Data()) == 12U &&
              fxe::ReadU32LE(replayedCheckpoint.body.Data() + 4U) == 34U &&
              queriedProgress.header.code == uint8_t(wire::Control::Progress),
          "query replays a lost checkpoint without re-running the domain");

    const wire::PacketView confirm{{wire::Kind::Control,
                                    uint8_t(wire::Control::Confirm),
                                    streamToken, 1U}, {}};
    std::array<uint8_t, 20U> confirmBytes{};
    size_t confirmSize = 0U;
    Check(wire::Codec::Encode(confirm,
                              {confirmBytes.data(), confirmBytes.size()},
                              confirmSize) == wire::Error::None &&
              session.Ingress(generation, confirmBytes.data(), confirmSize) ==
                  modules::ConnectionSession::Result::Ok,
          "complete outcome receipt frees the retained prefix");
    const auto grant = Take(session, generation, output);
    Check(fxe::ReadU32LE(grant.body.Data()) == 1U &&
              fxe::ReadU32LE(grant.body.Data() + 4U) == 9U,
          "absolute grant advances only after cumulative receipt");

    session.End(generation);
    Check(session.SuccessfulReverseObjectReplays() == 0U,
          "session retirement resets successful reverse replay observations");
    Check(session.Ingress(generation, actionBytes.data(), actionSize) ==
              modules::ConnectionSession::Result::Inactive &&
              session.Ingress(generation - 1U, actionBytes.data(), actionSize) ==
                  modules::ConnectionSession::Result::StaleGeneration,
          "disconnect retires tokens and stale generations");

    Check(session.Begin(generation + 1U, bootNonce, 512U, 41U) &&
              session.Ingress(generation + 1U, hello.bytes.data(), hello.size) ==
                  modules::ConnectionSession::Result::Ok,
          "replacement connection starts a fresh transcript");
    std::array<uint8_t, 20U> replacementWelcomeBytes{};
    const auto replacementWelcome =
        Take(session, generation + 1U, replacementWelcomeBytes);
    std::array<uint8_t, 20U> replacementIdentityBytes{};
    const auto replacementIdentity =
        Take(session, generation + 1U, replacementIdentityBytes);
    Check(replacementWelcome.header.token != welcome.header.token &&
              replacementWelcome.header.token ==
                  replacementIdentity.header.token,
          "replacement connection never reuses a prior stream token");
}

uint32_t ActivateRecoverySession(
    modules::ConnectionSession& session, const uint32_t generation = 7U,
    const uint16_t packetMaximum = 20U) {
    Check(session.Begin(generation, 1234U, packetMaximum, 1U), "recovery fixture begins");
    std::array<uint8_t, 10U> body{};
    fxe::WriteU64LE(5678U, body.data());
    fxe::WriteU16LE(packetMaximum, body.data() + 8U);
    const auto hello = Encode({{wire::Kind::Control, uint8_t(wire::Control::Hello), 0U, 0U},
                               {body.data(), body.size()}});
    Check(session.Ingress(generation, hello.bytes.data(), hello.size) ==
              modules::ConnectionSession::Result::Ok, "recovery Hello admitted");
    std::array<uint8_t, 20U> welcomeBytes{}, identityBytes{};
    const auto welcome = Take(session, generation, welcomeBytes);
    (void)Take(session, generation, identityBytes);
    std::array<uint8_t, 60U> transcript{};
    std::copy_n(hello.bytes.begin(), 20U, transcript.begin());
    std::copy(welcomeBytes.begin(), welcomeBytes.end(), transcript.begin() + 20U);
    std::copy(identityBytes.begin(), identityBytes.end(), transcript.begin() + 40U);
    std::array<uint8_t, 4U> crc{};
    fxe::WriteU32LE(fxe::Crc32(transcript.data(), transcript.size()), crc.data());
    const auto ready = Encode({{wire::Kind::Control, uint8_t(wire::Control::Ready),
                               welcome.header.token, 0U}, {crc.data(), crc.size()}});
    Check(session.Ingress(generation, ready.bytes.data(), ready.size) ==
              modules::ConnectionSession::Result::Ok, "recovery Ready admitted");
    return welcome.header.token;
}

Encoded RecoveryOpen(const uint32_t token, const uint32_t request, const bool object,
                     const uint8_t capacity = 8U, const uint16_t maximumBody = 10U) {
    std::array<uint8_t, 8U> body{};
    fxe::WriteU16LE(1U, body.data());
    body[2U] = object ? 1U : 0U;
    body[3U] = capacity;
    fxe::WriteU16LE(maximumBody, body.data() + 4U);
    return Encode({{wire::Kind::Control, uint8_t(wire::Control::Open), token, request},
                   {body.data(), body.size()}});
}

void OpenPressureDoesNotLeakStream() {
    modules::ConnectionSession session;
    const auto token = ActivateRecoverySession(session);
    const auto open = RecoveryOpen(token, 1U, false, 3U, 2U);
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::NoCapacity && session.StreamToken() == 0U,
          "full lifecycle output cannot leak an unanswered Ordered stream");
    std::array<uint8_t, 20U> bytes{};
    (void)Take(session, 7U, bytes); // Activated.
    (void)Take(session, 7U, bytes); // State Open.
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok,
          "exact Open retries after lifecycle output becomes available");
    const auto opened = Take(session, 7U, bytes);
    Check(opened.header.code == uint8_t(wire::Control::Opened) &&
              fxe::ReadU16LE(opened.body.Data() + 8U) == 0U &&
              fxe::ReadU16LE(opened.body.Data() + 4U) == 2U &&
              opened.body[6U] == 3U && session.StreamToken() != 0U,
          "retried Open exposes one usable token instead of stream busy");
    const auto progress = Take(session, 7U, bytes);
    Check(fxe::ReadU32LE(progress.body.Data() + 4U) == 3U,
          "Ordered Progress grants the selected capacity rather than its storage maximum");
    const auto outside = Encode({{wire::Kind::Ordered, 5U, session.StreamToken(), 4U}, {}});
    const uint8_t tooLarge[]{1U, 2U, 3U};
    const auto oversized = Encode({{wire::Kind::Ordered, 5U, session.StreamToken(), 1U}, tooLarge});
    Check(session.Ingress(7U, outside.bytes.data(), outside.size) ==
              modules::ConnectionSession::Result::NoCapacity &&
              session.Ingress(7U, oversized.bytes.data(), oversized.size) ==
              modules::ConnectionSession::Result::Unexpected,
          "selected Ordered capacity and body bounds reject before admission");
}

void GracefulCloseWaitsForCompleteEvidenceAndReplays() {
    modules::ConnectionSession session;
    const auto token = ActivateRecoverySession(session);
    std::array<uint8_t, 20U> bytes{};
    (void)Take(session, 7U, bytes);
    (void)Take(session, 7U, bytes);
    auto open = RecoveryOpen(token, 1U, false);
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok, "close fixture opens Ordered");
    (void)Take(session, 7U, bytes);
    (void)Take(session, 7U, bytes);
    const auto oldToken = session.StreamToken();
    auto close = Encode({{wire::Kind::Control, uint8_t(wire::Control::Close), oldToken, 0U}, {}});
    Check(session.Ingress(7U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::Ok && session.StreamToken() == 0U,
          "empty Ordered stream closes without retiring connection");
    const auto closed = Take(session, 7U, bytes);
    Check(closed.header.code == uint8_t(wire::Control::Closed) &&
              closed.header.token == oldToken && session.IsActivated(),
          "Closed uses retired stream token and preserves shared connection");
    Check(session.Ingress(7U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::Ok,
          "lost Closed is replayed without reviving stream");
    (void)Take(session, 7U, bytes);
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Unexpected && session.StreamToken() == 0U,
          "old Open replay cannot advertise a retired token");
    open = RecoveryOpen(token, 2U, false);
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok && session.StreamToken() != oldToken,
          "new Open rotates the drained stream without BLE reconnect");
    (void)Take(session, 7U, bytes);
    (void)Take(session, 7U, bytes);
    const auto stream = session.StreamToken();
    const auto action = Encode({{wire::Kind::Ordered, 5U, stream, 1U}, {}});
    Check(session.Ingress(7U, action.bytes.data(), action.size) ==
              modules::ConnectionSession::Result::Ok, "close fixture retains action");
    close = Encode({{wire::Kind::Control, uint8_t(wire::Control::Close), stream, 0U}, {}});
    Check(session.Ingress(7U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::NoCapacity && session.StreamToken() == stream,
          "Close refuses retained and in-flight work instead of cancelling it");
    (void)Take(session, 7U, bytes);
    modules::ConnectionSession::OrderedCommand command;
    Check(session.BeginNext(command) == modules::ConnectionSession::Result::Ok,
          "retained action still reaches domain exactly once");
    using Window = fxe::OrderedReceiveWindow<8U, 20U>;
    Check(session.Complete(stream, 1U, Window::Outcome{}, 4U, 8U) ==
              modules::ConnectionSession::Result::Ok, "action completes before close");
    Check(session.Ingress(7U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::NoCapacity,
          "unconfirmed terminal evidence also prevents close");
    (void)Take(session, 7U, bytes);
    (void)Take(session, 7U, bytes);
    const auto confirm = Encode({{wire::Kind::Control, uint8_t(wire::Control::Confirm), stream, 1U}, {}});
    Check(session.Ingress(7U, confirm.bytes.data(), confirm.size) ==
              modules::ConnectionSession::Result::Ok &&
              session.Ingress(7U, close.bytes.data(), close.size) ==
                  modules::ConnectionSession::Result::Ok && session.StreamToken() == 0U,
          "cumulative confirmation permits graceful retirement");
    (void)Take(session, 7U, bytes);
    open = RecoveryOpen(token, 3U, true);
    Check(session.Ingress(7U, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok, "empty Object stream opens");
    (void)Take(session, 7U, bytes);
    close = Encode({{wire::Kind::Control, uint8_t(wire::Control::Close), session.ObjectStreamToken(), 0U}, {}});
    Check(session.Ingress(7U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::Ok && session.ObjectStreamToken() == 0U,
          "empty Object stream closes safely");
    (void)Take(session, 7U, bytes);
    Check(session.Ingress(6U, close.bytes.data(), close.size) ==
              modules::ConnectionSession::Result::StaleGeneration,
          "retired lifecycle replay is connection generation fenced");
    session.End(7U);
    Check(!session.IsActivated(), "End retires the complete provider lifetime");
}

void ObjectReservationsSurviveDerivedCopiesAndRetireOnce() {
    using Session = modules::ConnectionSession;
    Session session;
    uint32_t connection = ActivateRecoverySession(session);
    std::array<uint8_t, 20U> bytes{};
    size_t size = 0U;
    auto drain = [&] { while (session.PeekOutput(7U, bytes.data(), bytes.size(), size)) session.AcceptOutput(7U); };
    drain();
    auto open = RecoveryOpen(connection, 1U, true, 8U, 4U);
    Check(session.Ingress(7U, open.bytes.data(), open.size) == Session::Result::Ok,
          "credit test opens actual bounded Object stream");
    drain();
    const uint32_t firstToken = session.ObjectStreamToken();
    const uint8_t oversizedBody[]{1U, 2U, 3U, 4U, 5U};
    const auto oversized = Encode({{wire::Kind::Object, 0x40U, firstToken, 1U}, oversizedBody});
    Session::ObjectCommand oversizedCommand;
    Check(session.Ingress(7U, oversized.bytes.data(), oversized.size, &oversizedCommand) ==
              Session::Result::Unexpected && !session.HasPendingObject(),
          "Object body exceeding selected Open bound consumes no reservation");
    const std::array<uint8_t, 1U> body{{0x55U}};
    auto query = [&](const uint32_t accepted, const uint32_t grant) {
        const auto packet = Encode({{wire::Kind::Control, uint8_t(wire::Control::Query), session.ObjectStreamToken(), 0U}, {}});
        Check(session.Ingress(7U, packet.bytes.data(), packet.size) == Session::Result::Ok,
              "Object Query remains live with every reservation held");
        const auto progress = Take(session, 7U, bytes);
        Check(progress.header.sequence == accepted && fxe::ReadU32LE(progress.body.Data()) == accepted &&
                  fxe::ReadU32LE(progress.body.Data() + 4U) == grant,
              "Query replays bounded monotonic grant without minting a reservation");
    };
    for (uint32_t sequence = 1U; sequence <= 8U; ++sequence) {
        const auto packet = Encode({{wire::Kind::Object, 0x40U, firstToken, sequence}, {body.data(), body.size()}});
        Session::ObjectCommand command;
        Check(session.Ingress(7U, packet.bytes.data(), packet.size, &command, 100U + sequence) == Session::Result::Ok &&
                  command.ingressOrdinal == 100U + sequence &&
                  session.AcceptObject(firstToken, sequence, true) == Session::Result::Ok,
              "same-FIFO copy advances admission while retaining its raw reservation");
        drain();
        Check(session.Ingress(7U, packet.bytes.data(), packet.size, &command) == Session::Result::Ok && command.streamToken == 0U,
              "accepted duplicate cannot create another derived event or return credit");
        query(sequence, 8U);
    }
    const auto outside = Encode({{wire::Kind::Object, 0x40U, firstToken, 9U}, {body.data(), body.size()}});
    Session::ObjectCommand command;
    Check(session.Ingress(7U, outside.bytes.data(), outside.size, &command) == Session::Result::NoCapacity,
          "sequential Object beyond actual grant cannot overbook held derived events");
    auto close = Encode({{wire::Kind::Control, uint8_t(wire::Control::Close), firstToken, 0U}, {}});
    Check(session.Ingress(7U, close.bytes.data(), close.size) == Session::Result::NoCapacity && session.ObjectStreamToken() == firstToken,
          "Close preserves live stream while derived events still occupy raw slots");
    drain();
    for (unsigned index = 0U; index < 8U; ++index) session.ReturnObjectCredit(firstToken);
    drain(); query(8U, 16U);
    Check(session.Ingress(7U, close.bytes.data(), close.size) == Session::Result::Ok,
          "draining derived events permits Close without losing acknowledgement");
    drain();
    open = RecoveryOpen(connection, 2U, true);
    Check(session.Ingress(7U, open.bytes.data(), open.size) == Session::Result::Ok && session.ObjectStreamToken() > firstToken,
          "reopen allocates a fresh stream only after old raw reservations drain");
    drain(); session.ReturnObjectCredit(firstToken); query(0U, 8U);
    const uint32_t secondToken = session.ObjectStreamToken();
    session.End(7U);
    connection = ActivateRecoverySession(session);
    drain(); open = RecoveryOpen(connection, 1U, true);
    Check(session.Ingress(7U, open.bytes.data(), open.size) == Session::Result::Ok && session.ObjectStreamToken() > secondToken,
          "Begin/End never reuse an old derived-event stream token");
    drain(); session.ReturnObjectCredit(secondToken); query(0U, 8U);
    modules::ConnectionSessionCreditTestPeer::SetFinalReservation(session);
    const auto final = Encode({{wire::Kind::Object, 0x40U, session.ObjectStreamToken(), wire::LAST_SEQUENCE}, {body.data(), body.size()}});
    Check(session.Ingress(7U, final.bytes.data(), final.size, &command) == Session::Result::Ok &&
              session.AcceptObject(command.streamToken, command.sequence, true) == Session::Result::Ok,
          "final sequence uses its existing bounded reservation");
    drain(); session.ReturnObjectCredit(command.streamToken); drain(); query(wire::LAST_SEQUENCE, wire::LAST_SEQUENCE);
    std::puts("[OBJECT_CREDIT] same_fifo=held query=no_mint duplicate=once close_open=drained stale=no_return saturation=bounded");
}

void FullStateCapacityRetainsStoreAuthority() {
    modules::ConnectionSession session;
    Check(session.Begin(17U, UINT64_C(0x8899aabbccddeeff), 20U, 1U),
          "minimum-MTU capacity fixture begins");
    const std::array<uint8_t, 1U> occupiedState = {1U};
    bool existingKeysRetained = true;
    for (uint8_t code = 1U; code <= 23U; ++code) {
        existingKeysRetained = session.PublishState(
            code, 40U, occupiedState.data(), occupiedState.size()) &&
            existingKeysRetained;
    }
    const std::array<uint8_t, 10U> liveAuthority = {
        1U, 0U, 7U, 0U, 0U, 0U, 9U, 0U, 0U, 0U};
    Check(existingKeysRetained && session.PublishState(
              modules::ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE,
              41U, liveAuthority.data(), liveAuthority.size()) &&
              session.PublishState(
                  modules::ConnectionSession::BOARD_STORAGE_RECOVERY_STATE_CODE,
                  42U, occupiedState.data(), occupiedState.size()) &&
              session.PublishState(39U, 43U, occupiedState.data(),
                                   occupiedState.size()) &&
              session.PublishState(40U, 44U, occupiedState.data(),
                                    occupiedState.size()) &&
              session.PublishState(41U, 44U, occupiedState.data(),
                                    occupiedState.size()) &&
              session.PublishState(42U, 44U, occupiedState.data(),
                                    occupiedState.size()) &&
              session.PublishState(43U, 45U, occupiedState.data(),
                                    occupiedState.size()) &&
              session.PublishState(44U, 46U, occupiedState.data(), occupiedState.size()) &&
              session.PublishState(
                  modules::ConnectionSession::CURRENT_GAME_LINE_STATE_CODE,
                  46U, occupiedState.data(), occupiedState.size()) &&
              session.PublishState(modules::settings_state_wire::LIVE_PALETTE_STATE_CODE,
                  47U, occupiedState.data(), occupiedState.size()) &&
              !session.PublishState(47U, 48U, occupiedState.data(), occupiedState.size()),
          "all thirty-three keys including live palette fit; a thirty-fourth key is refused");
}


// Home and Settings use idle-state payloads accepted by the Dart readers.
// Other keys reuse those payloads to exercise scheduling fairness. Store edits
// use the wire fixture above.
struct RetainedStateFixture {
    uint8_t code;
    uint32_t revision;
    uint8_t size;
    std::array<uint8_t, 10U> body;
};
constexpr std::array<RetainedStateFixture, 32U> RETAINED_STATE_FIXTURES{{
    {1U, 1U, 10U, {0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x0fU, 0xffU, 0x74U, 0x30U, 0x00U}},
    {2U, 1U, 6U, {0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {3U, 2539145382U, 10U, {0x40U, 0x03U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {4U, 2957015923U, 10U, {0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x40U, 0x04U, 0xacU, 0x00U}},
    {5U, 2957015923U, 10U, {0x02U, 0x0aU, 0xb9U, 0x9cU, 0xa0U, 0x77U, 0x77U, 0x77U, 0x77U, 0x00U}},
    {6U, 2957015923U, 10U, {0x08U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {7U, 2957015923U, 10U, {0x00U, 0x00U, 0x02U, 0x20U, 0x00U, 0x11U, 0x11U, 0x11U, 0x11U, 0x04U}},
    {8U, 2957015923U, 10U, {0x53U, 0x36U, 0x40U, 0x1eU, 0xffU, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U}},
    {9U, 2957015923U, 10U, {0x00U, 0x00U, 0x00U, 0xffU, 0xbdU, 0xffU, 0x24U, 0x00U, 0x00U, 0x24U}},
    {10U, 2957015923U, 10U, {0xffU, 0xbdU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x3eU, 0x2dU, 0x06U}},
    {11U, 2957015923U, 10U, {0x15U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {12U, 2957015923U, 10U, {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0xffU, 0xffU, 0xffU, 0xffU}},
    {13U, 2957015923U, 10U, {0xffU, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {14U, 2910568667U, 10U, {0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0xbdU, 0x00U}},
    {15U, 2910568667U, 10U, {0x09U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x00U}},
    {16U, 2910568667U, 10U, {0x08U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U}},
    {17U, 2910568667U, 10U, {0x03U, 0x00U, 0x20U, 0x00U, 0x40U, 0x01U, 0x44U, 0x02U, 0x52U, 0x03U}},
    {18U, 2910568667U, 10U, {0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x7dU, 0x00U}},
    {19U, 2910568667U, 10U, {0x50U, 0x00U, 0x33U, 0x02U, 0x28U, 0x00U, 0x01U, 0x00U, 0xe8U, 0x03U}},
    {20U, 2910568667U, 10U, {0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U}},
    {21U, 2910568667U, 10U, {0x00U, 0x00U, 0x00U, 0x00U, 0xc0U, 0x00U, 0x08U, 0x00U, 0xbdU, 0x09U}},
    {22U, 1U, 10U, {0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x1fU, 0x00U, 0x00U, 0x00U}},
    {23U, 1U, 10U, {0xbdU, 0xffU, 0x24U, 0x00U, 0x00U, 0x24U, 0xffU, 0xbdU, 0x00U, 0x00U}},
    {37U, 1U, 10U, {0x01U, 0x00U, 0x4cU, 0x00U, 0x00U, 0x00U, 0x4cU, 0x00U, 0x00U, 0x00U}},
    {38U, 2U, 3U, {0x01U, 0x01U, 0x00U}},
    {39U, 2910568667U, 2U, {0x01U, 0x00U}},
    {40U, 1U, 8U, {0x01U, 0x01U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {41U, 1U, 8U, {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}},
    {42U, 1U, 8U, {9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U}},
    {43U, 1U, 6U, {1U, 1U, 0U, 0U, 0U, 0U}},
    {44U, 1U, 6U, {1U, 1U, 0U, 0U, 0U, 0U}},
    {modules::ConnectionSession::CURRENT_GAME_LINE_STATE_CODE, 2U, 2U, {1U, 1U}},
}};

struct ReplayCoverage {
    std::array<bool, 256U> seen{};
    uint16_t objects = 0U;
    uint8_t statesSinceBulk = 0U;
};

class StateReplayFixture final {
  public:
    modules::ConnectionSession session;
    uint32_t generation = 7U;

    StateReplayFixture() { Start(); }

    void Start() {
        const uint32_t token = ActivateRecoverySession(session, generation, 512U);
        std::array<uint8_t, 20U> bytes{};
        (void)Take(session, generation, bytes); // Activated.
        for (uint32_t sequence = 1U; sequence <= 2U; ++sequence) {
            session.Update(generation);
            const auto open = Take(session, generation, bytes);
            Check(open.header.code == uint8_t(wire::Control::Open) &&
                      open.header.sequence == sequence,
                  "replay fixture negotiates production reverse streams");
            std::array<uint8_t, 10U> body{};
            fxe::WriteU32LE(0x80000000U + generation * 2U + sequence, body.data());
            fxe::WriteU16LE(sequence == 1U ? 10U : 502U, body.data() + 4U);
            body[6U] = sequence == 1U ? modules::ConnectionSession::STATE_CAPACITY : 8U;
            body[7U] = 1U;
            const auto opened = Encode(
                {{wire::Kind::Control, uint8_t(wire::Control::Opened),
                  token, sequence}, {body.data(), body.size()}});
            Check(session.Ingress(generation, opened.bytes.data(), opened.size) ==
                      modules::ConnectionSession::Result::Ok,
                  "production Opened admits the reverse stream");
        }
    }

    void PublishStates() {
        for (const auto& state : RETAINED_STATE_FIXTURES) {
            Check(session.PublishState(state.code, state.revision,
                                       state.body.data(), state.size),
                  "all production State keys are retained");
        }
    }

    void Query(const uint32_t token, const uint32_t code = 0U) {
        const auto query = Encode(
            {{wire::Kind::Control, uint8_t(wire::Control::Query), token, code}, {}});
        Check(session.Ingress(generation, query.bytes.data(), query.size) ==
                  modules::ConnectionSession::Result::Ok,
              "production Query rearms retained output");
    }

    void KeepBulkPending() {
        std::array<uint8_t, 19U> body{};
        body[0U] = 0x4dU;
        body[1U] = 2U;
        body[2U] = 0x8dU;
        fxe::WriteU16LE(1U, body.data() + 11U);
        Check(session.PublishReverseObject(0x8dU, 1U, body.data(), body.size()),
              "production reverse Object remains eligible beside State replay");
    }

    unsigned Drain(ReplayCoverage& coverage, const unsigned budget,
                   const bool continuousBulk = false) {
        unsigned count = 0U;
        for (; count < budget; ++count) {
            std::array<uint8_t, 512U> bytes{};
            size_t size = 0U;
            if (!session.PeekOutput(generation, bytes.data(), bytes.size(), size)) break;
            wire::PacketView packet;
            Check(wire::Codec::Decode({bytes.data(), size}, packet, 512U) ==
                      wire::Error::None,
                  "every accepted replay output validates through production Codec");
            if (packet.header.kind == wire::Kind::State) {
                coverage.seen[packet.header.code] = true;
                ++coverage.statesSinceBulk;
                const auto expected = std::find_if(
                    RETAINED_STATE_FIXTURES.begin(), RETAINED_STATE_FIXTURES.end(),
                    [&](const auto& state) { return state.code == packet.header.code; });
                Check(expected != RETAINED_STATE_FIXTURES.end() &&
                          packet.body.Size() == expected->size &&
                          std::equal(packet.body.Data(), packet.body.Data() + packet.body.Size(),
                                     expected->body.begin()),
                      "replay preserves the complete captured State fragment bytes");
                if (continuousBulk) {
                    Check(coverage.statesSinceBulk <= 3U,
                          "eligible bulk never waits beyond three accepted State packets");
                }
            } else {
                Check(packet.header.kind == wire::Kind::Object,
                      "only negotiated State and reverse Object traffic is emitted");
                ++coverage.objects;
                coverage.statesSinceBulk = 0U;
            }
            session.AcceptOutput(generation);
            if (continuousBulk && packet.header.kind == wire::Kind::Object) {
                Query(session.ReverseObjectStreamToken());
            }
        }
        return count;
    }
};

bool SettingsComplete(const ReplayCoverage& coverage) {
    for (uint8_t code = 14U; code <= 21U; ++code) {
        if (!coverage.seen[code]) return false;
    }
    return coverage.seen[39U];
}

void CheckHomeAndSettings(const ReplayCoverage& coverage) {
    bool homeComplete = true;
    for (uint8_t code = 4U; code <= 13U; ++code) homeComplete &= coverage.seen[code];
    Check(homeComplete && SettingsComplete(coverage),
          "bounded whole-State recovery completes Home and Settings despite requery");
}

void RepeatedStateQueriesConvergeWithinReaderDeadline() {
    for (unsigned budget = 5U; budget <= 8U; ++budget) {
        StateReplayFixture fixture;
        fixture.PublishStates();
        ReplayCoverage coverage;
        for (uint32_t tick = 1U; tick <= 16U; ++tick) {
            fxe::ManualMillis::SetNowMs(tick * 121U);
            fixture.Query(fixture.session.StateStreamToken());
            fixture.Drain(coverage, budget);
        }
        CheckHomeAndSettings(coverage);
    }

    // Production value adjustment repeats every 19 ms, while BLE services at
    // most four notifications every 20 ms. A live pair must not wait for all
    // nine Settings fragments to survive that continuously replaced snapshot.
    for (const bool homePressure : {false, true}) {
        StateReplayFixture held;
        modules::SettingsStore heldSettings;
        held.PublishStates();
        ReplayCoverage initial;
        held.Drain(initial, modules::ConnectionSession::STATE_CAPACITY);
        uint32_t receivedAtMs = 0U;
        uint32_t maxGapMs = 0U;
        unsigned liveUpdates = 0U;
        for (uint32_t now = 0U; now <= 1000U; ++now) {
            fxe::ManualMillis::SetNowMs(now + 1U);
            if (now % 19U == 0U) {
                const uint32_t revision = heldSettings.GetRevision() + 1U;
                heldSettings.Doc()["P1_END_PIECE_WHEEL"] = static_cast<uint8_t>(revision * 2U);
                heldSettings.Doc()["P2_END_PIECE_WHEEL"] = 170U;
                heldSettings.Doc()["GAME_FIRST_PLAYER"] = 2U;
                heldSettings.MarkDirty();
                modules::settings_state_wire::Payload full{};
                Check(modules::settings_state_wire::Encode(heldSettings, full),
                      "held color uses the production complete Settings encoder");
                const uint32_t fullIdentity = fxe::Crc32(full.data(), full.size());
                for (uint8_t index = 0U; index < modules::settings_state_wire::FRAGMENT_COUNT; ++index) {
                    Check(held.session.PublishState(
                              modules::settings_state_wire::FragmentCode(index),
                              fullIdentity == 0U ? 1U : fullIdentity,
                              full.data() + index * modules::settings_state_wire::FRAGMENT_BYTES,
                              modules::settings_state_wire::FragmentLength(index)),
                          "held color replaces every full Settings fragment identity");
                }
                const auto palette = modules::settings_state_wire::EncodeLivePalette(heldSettings);
                const uint32_t identity = fxe::Crc32(palette.data(), palette.size());
                Check(held.session.PublishState(
                          modules::settings_state_wire::LIVE_PALETTE_STATE_CODE,
                          identity == 0U ? 1U : identity, palette.data(), palette.size()),
                      "live physical pair and assignment fit beside all existing State keys");
                if (homePressure) {
                    for (const auto& state : RETAINED_STATE_FIXTURES) {
                        if (state.code == 2U || (state.code >= 4U && state.code <= 13U)) {
                            Check(held.session.PublishState(state.code, revision,
                                      state.body.data(), state.size),
                                  "held palette shares priority with live Home and health");
                        }
                    }
                }
            }
            if (now % 20U != 0U) continue;
            for (unsigned count = 0U; count < 4U; ++count) {
                std::array<uint8_t, 20U> bytes{};
                size_t size = 0U;
                if (!held.session.PeekOutput(held.generation, bytes.data(), bytes.size(), size)) break;
                wire::PacketView packet;
                Check(wire::Codec::Decode({bytes.data(), size}, packet, bytes.size()) == wire::Error::None,
                      "held color output fits the real minimum-MTU codec");
                if (packet.header.kind == wire::Kind::State &&
                    packet.header.code == modules::settings_state_wire::LIVE_PALETTE_STATE_CODE) {
                    maxGapMs = std::max(maxGapMs, now - receivedAtMs);
                    receivedAtMs = now;
                    ++liveUpdates;
                    Check(packet.body.Size() == 8U && packet.body[0U] == 1U &&
                              packet.body[5U] == static_cast<uint8_t>(fxe::ReadU32LE(packet.body.Data() + 1U) * 2U) &&
                              packet.body[6U] == 170U && packet.body[7U] == 2U,
                          "held color keeps both end colors and assignment together");
                }
                held.session.AcceptOutput(held.generation);
            }
            Check(now - receivedAtMs <= (homePressure ? 100U : 60U),
                  "held color remains live while full Settings snapshots are continuously replaced");
        }
        Check(liveUpdates >= (homePressure ? 10U : 20U),
              "a continuous D-pad hold delivers repeated live colors before release");
        std::printf("[LIVE_PALETTE] home_pressure=%u updates=%u max_gap_ms=%u\n",
                    static_cast<unsigned>(homePressure), liveUpdates, maxGapMs);
    }
}

void StateReplayAndContinuousBulkBothConverge() {
    for (const bool settingsOnly : {true, false}) {
        StateReplayFixture fixture;
        fixture.PublishStates();
        ReplayCoverage initial;
        fixture.Drain(initial, modules::ConnectionSession::STATE_CAPACITY);
        fixture.KeepBulkPending();
        ReplayCoverage coverage;
        for (uint32_t tick = 1U; tick <= 16U; ++tick) {
            fxe::ManualMillis::SetNowMs(tick * 121U);
            if (settingsOnly) {
                for (uint8_t code = 14U; code <= 21U; ++code) {
                    fixture.Query(fixture.session.StateStreamToken(), code);
                }
                fixture.Query(fixture.session.StateStreamToken(), 39U);
            } else {
                fixture.Query(fixture.session.StateStreamToken());
            }
            // A new urgent health revision must still precede ordinary replay
            // promptly, without monopolizing it or reverse bulk.
            const auto& health = RETAINED_STATE_FIXTURES[1U];
            Check(fixture.session.PublishState(2U, tick + 1U, health.body.data(), health.size),
                  "urgent health can change during constrained replay");
            coverage.seen[2U] = false;
            fixture.Drain(coverage, 4U, true);
            if (settingsOnly) {
                Check(coverage.seen[2U], "sole urgent priority key arrives within one bounded bulk burst");
            }
            fixture.Drain(coverage, 4U, true);
        }
        Check(coverage.seen[2U], "rotating priority truth remains observable during repeated replay");
        Check(SettingsComplete(coverage) && coverage.objects > 0U,
              "specific and whole replay cannot starve Settings or continuous reverse bulk");
        if (!settingsOnly) CheckHomeAndSettings(coverage);
    }
}

void FailedNotifyRetainsExactOutputAndRecoveryRestarts() {
    StateReplayFixture fixture;
    fixture.PublishStates();
    std::array<uint8_t, 512U> first{}, retry{};
    size_t firstSize = 0U, retrySize = 0U;
    Check(fixture.session.PeekOutput(fixture.generation, first.data(), first.size(), firstSize),
          "a failed-notify fixture selects actual retained output");
    for (uint32_t tick = 1U; tick <= 5U; ++tick) {
        fxe::ManualMillis::SetNowMs(tick * 121U);
        fixture.Query(fixture.session.StateStreamToken());
        Check(fixture.session.PeekOutput(fixture.generation, retry.data(), retry.size(), retrySize) &&
                  retrySize == firstSize && std::equal(first.begin(), first.begin() + firstSize, retry.begin()),
              "notify false plus repeated Query retains byte-identical selected output");
        // SDK rejection is modelled by withholding AcceptOutput.
    }
    ReplayCoverage coverage;
    for (uint32_t tick = 6U; tick <= 21U; ++tick) {
        fxe::ManualMillis::SetNowMs(tick * 121U);
        fixture.Query(fixture.session.StateStreamToken());
        fixture.Drain(coverage, 5U);
    }
    CheckHomeAndSettings(coverage);
    const auto oldToken = fixture.session.StateStreamToken();
    fixture.session.End(fixture.generation);
    ++fixture.generation;
    fixture.Start();
    const auto stale = Encode({{wire::Kind::Control, uint8_t(wire::Control::Query), oldToken, 0U}, {}});
    Check(fixture.session.Ingress(7U, stale.bytes.data(), stale.size) ==
              modules::ConnectionSession::Result::StaleGeneration,
          "replacement generation rejects the old replay request");
    Check(!fixture.session.PeekOutput(7U, retry.data(), retry.size(), retrySize),
          "retired generation cannot send retained failed output");
    fixture.PublishStates();
    coverage = {};
    fixture.Drain(coverage, modules::ConnectionSession::STATE_CAPACITY);
    CheckHomeAndSettings(coverage);
}

void RestrictedPairingReplayAndRevocationFenceQueuedOutput() {
    StateReplayFixture fixture;
    fixture.PublishStates();
    std::array<uint8_t, 512U> bytes{};
    size_t size = 0U;
    Check(fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size) &&
              bytes[1U] < 40U,
          "a protected State is selected as if native notify had failed");
    fixture.session.SetProductAccess(false);
    ReplayCoverage publicOnly;
    Check(fixture.Drain(publicOnly, 40U) == 4U && publicOnly.seen[2U] &&
              publicOnly.seen[40U] && publicOnly.seen[41U] && publicOnly.seen[42U],
          "revocation permits only safe runtime health, pairing and durable identity");
    fixture.Query(fixture.session.StateStreamToken(), 40U);
    publicOnly = {};
    Check(fixture.Drain(publicOnly, 40U) == 3U &&
              publicOnly.seen[40U] && publicOnly.seen[41U] && publicOnly.seen[42U],
          "one shared Query40 repairs status plus both exact identity halves");
    fixture.Query(fixture.session.StateStreamToken());
    publicOnly = {};
    Check(fixture.Drain(publicOnly, 40U) == 4U && publicOnly.seen[2U] && !publicOnly.seen[1U] && !publicOnly.seen[3U],
          "whole replay cannot disclose retained product State before claim");
    fixture.session.SetProductAccess(true);
    ReplayCoverage resumed;
    fixture.Drain(resumed, 40U);
    CheckHomeAndSettings(resumed);
    fixture.KeepBulkPending();
    Check(fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size) &&
              (bytes[0U] & 0x0fU) == uint8_t(wire::Kind::Object),
          "actual protected reverse Object is selected before access withdrawal");
    fixture.session.SetProductAccess(false);
    Check(!fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size),
          "selected protected Object cannot escape after revocation");
    const auto query = Encode({{wire::Kind::Control, uint8_t(wire::Control::Query),
                               fixture.session.ReverseObjectStreamToken(), 0U}, {}});
    Check(fixture.session.Ingress(fixture.generation, query.bytes.data(), query.size) ==
              modules::ConnectionSession::Result::NotReady,
          "restricted peer cannot rearm protected Object receipts");
    fixture.session.End(fixture.generation);
    ++fixture.generation;
    fixture.Start();
    fixture.PublishStates();
    publicOnly = {};
    Check(fixture.Drain(publicOnly, 40U) == 4U,
          "negotiation restart cannot reset an active access restriction");
}

void PublicSupportAllowlistAndPrivateRevocation() {
    StateReplayFixture fixture;
    fixture.session.SetProductAccess(false);
    const std::array<uint8_t, 8U> body{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    Check(!fixture.session.PublishReverseObject(0x44U, 1U, body.data(), body.size()),
          "unowned peer cannot publish private user actions");
    Check(!fixture.session.PublishReverseObject(0x40U, 1U, body.data(), body.size()),
          "public support permission does not open other Objects");
    Check(fixture.session.PublishReverseObject(0x45U, 1U, body.data(), body.size()),
          "sanitized public support uses existing reverse Object before owner proof");
    std::array<uint8_t, 20U> bytes{};
    const auto publicReply = Take(fixture.session, fixture.generation, bytes);
    Check(publicReply.header.kind == wire::Kind::Object && publicReply.header.code == 0x45U,
          "only exact public support opcode reaches restricted peer");
    fixture.session.SetProductAccess(true);
    Check(fixture.session.PublishReverseObject(0x44U, 2U, body.data(), body.size()),
          "authenticated private support can retain a response");
    size_t size = 0U;
    Check(fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size) && bytes[1U] == 0x44U,
          "private response selected before access is revoked");
    fixture.session.SetProductAccess(false);
    Check(!fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size),
          "selected private response remains hidden after revocation");
    const auto query = Encode({{wire::Kind::Control, uint8_t(wire::Control::Query),
        fixture.session.ReverseObjectStreamToken(), 0U}, {}});
    (void)fixture.session.Ingress(fixture.generation, query.bytes.data(), query.size);
    Check(fixture.session.PeekOutput(fixture.generation, bytes.data(), bytes.size(), size) && bytes[1U] == 0x45U,
          "restricted replay re-arms public response without revealing retained private bytes");
    fixture.session.AcceptOutput(fixture.generation);
    const uint8_t safe = 1U;
    Check(fixture.session.PublishState(2U, 9U, &safe, 1U),
          "safe health can be published while private Object authority is revoked");
    const auto healthReply = Take(fixture.session, fixture.generation, bytes);
    Check(healthReply.header.kind == wire::Kind::State && healthReply.header.code == 2U,
          "safe State2 preempts the hidden private reverse sequence hole");
    fixture.session.End(fixture.generation);
    ++fixture.generation;
    fixture.Start();
    Check(fixture.session.PublishReverseObject(0x45U, 1U, body.data(), body.size()),
          "normal renewed session restores sanitized public support capacity");
    const auto renewed = Take(fixture.session, fixture.generation, bytes);
    Check(renewed.header.kind == wire::Kind::Object && renewed.header.code == 0x45U,
          "fresh unowned session delivers public snapshot without private history");

}

void RestrictedOrderedReceiptsRemainLiveWithoutProductDigests() {
    StateReplayFixture fixture;
    std::array<uint8_t, 10U> body{};
    fxe::WriteU16LE(modules::ConnectionSession::LITCHCRAFT_ORDERED_TOPIC, body.data());
    body[2U] = 0U;
    body[3U] = 8U;
    fxe::WriteU16LE(10U, body.data() + 4U);
    const auto open = Encode({{wire::Kind::Control, uint8_t(wire::Control::Open),
                              fixture.session.ConnectionToken(), 1U}, {body.data(), 8U}});
    Check(fixture.session.Ingress(fixture.generation, open.bytes.data(), open.size) ==
              modules::ConnectionSession::Result::Ok, "pairing uses the existing ordered stream");
    std::array<uint8_t, 20U> bytes{};
    (void)Take(fixture.session, fixture.generation, bytes);
    (void)Take(fixture.session, fixture.generation, bytes);
    for (uint32_t sequence = 1U; sequence <= 2U; ++sequence) {
        const auto command = Encode({{wire::Kind::Ordered, 27U, fixture.session.StreamToken(), sequence},
                                     {body.data(), 5U}});
        Check(fixture.session.Ingress(fixture.generation, command.bytes.data(), command.size) ==
                  modules::ConnectionSession::Result::Ok, "pairing action enters common ordered owner");
        modules::ConnectionSession::OrderedCommand admitted;
        Check(fixture.session.BeginNext(admitted) == modules::ConnectionSession::Result::Ok,
              "Begin and Cancel remain dispatchable without product access");
        if (sequence == 2U) fixture.session.SetProductAccess(false);
        Check(fixture.session.Complete(admitted.streamToken, sequence, {}, 123U, 456U) ==
                  modules::ConnectionSession::Result::Ok, "pairing result receives common completion");
        fixture.session.SetProductAccess(false);
        const auto receipt = Take(fixture.session, fixture.generation, bytes);
        Check(receipt.header.code == uint8_t(wire::Control::Checkpoint) &&
                  receipt.header.sequence == sequence &&
                  fxe::ReadU32LE(receipt.body.Data()) == 0U &&
                  fxe::ReadU32LE(receipt.body.Data() + 4U) == 0U,
              "both queued and newly completed restricted receipts mask protected revision and digest");
        (void)Take(fixture.session, fixture.generation, bytes);
    }
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1U);
    ActivationOrderedOutcomeAndRecovery();
    RepeatedStateQueriesConvergeWithinReaderDeadline();
    StateReplayAndContinuousBulkBothConverge();
    FailedNotifyRetainsExactOutputAndRecoveryRestarts();
    RestrictedPairingReplayAndRevocationFenceQueuedOutput();
    PublicSupportAllowlistAndPrivateRevocation();
    RestrictedOrderedReceiptsRemainLiveWithoutProductDigests();
    FullStateCapacityRetainsStoreAuthority();
    OpenPressureDoesNotLeakStream();
    GracefulCloseWaitsForCompleteEvidenceAndReplays();
    ObjectReservationsSurviveDerivedCopiesAndRetireOnce();
    std::printf("[CONNECTION_SESSION] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
