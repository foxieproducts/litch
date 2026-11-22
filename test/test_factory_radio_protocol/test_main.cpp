#include <cstdio>

#include "modules/factory_radio_protocol.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[FACTORY_RADIO_PROTOCOL] start");

    constexpr uint32_t SESSION = 0x10203040U;
    constexpr uint32_t DUT_NONCE = 0x55667788U;
    constexpr uint32_t PEER_NONCE = 0xA1B2C3D4U;
    Check(modules::FACTORY_RADIO_PROTOCOL_VERSION == 3U,
          "BLE-only qualification has an explicit incompatible protocol epoch");

    modules::FactoryRadioDutHandshake dut;
    modules::FactoryRadioFrame hello;
    Check(dut.Start(modules::FactoryRadioChannel::Ble,
                    SESSION,
                    DUT_NONCE,
                    hello),
          "DUT starts with a bounded BLE hello");

    modules::FactoryRadioVerifierHandshake verifier;
    verifier.SetEligible(true);
    modules::FactoryRadioFrame challenge;
    Check(verifier.Handle(hello, PEER_NONCE, challenge),
          "eligible normal Litch answers a fresh DUT hello");
    Check(challenge.type == modules::FactoryRadioFrameType::Challenge &&
              challenge.echoNonce == DUT_NONCE,
          "verifier proves it received the DUT nonce");

    modules::FactoryRadioFrame retriedChallenge;
    Check(verifier.Handle(hello, 0xDEADBEEFU, retriedChallenge) &&
              retriedChallenge.type ==
                  modules::FactoryRadioFrameType::Challenge &&
              retriedChallenge.senderNonce == PEER_NONCE,
          "lost challenge can be retransmitted without changing its nonce");

    modules::FactoryRadioFrame response;
    Check(dut.Handle(challenge, response),
          "DUT answers the verifier challenge");
    Check(response.echoNonce == PEER_NONCE,
          "DUT response proves bidirectional receive");

    modules::FactoryRadioFrame retriedResponse;
    Check(dut.Handle(retriedChallenge, retriedResponse) &&
              retriedResponse.type == modules::FactoryRadioFrameType::Response &&
              retriedResponse.echoNonce == PEER_NONCE,
          "lost response can be retransmitted for the same challenge");

    modules::FactoryRadioFrame verified;
    Check(verifier.Handle(response, 0U, verified),
          "verifier completes only after its nonce returns");
    Check(verifier.WasHandshakeCompleted(),
          "verifier exposes completed bidirectional exchange");

    modules::FactoryRadioFrame retriedVerified;
    Check(verifier.Handle(retriedResponse, 0U, retriedVerified) &&
              retriedVerified.type == modules::FactoryRadioFrameType::Verified,
          "lost terminal proof can be retransmitted for the same response");
    Check(dut.Handle(verified, response),
          "DUT accepts the matching terminal verification");
    Check(dut.IsVerified(),
          "DUT reports radio verified only after terminal proof");

    modules::FactoryRadioFrame ignored;
    Check(!verifier.Handle(hello, PEER_NONCE, ignored),
          "completed session hello cannot be replayed");

    modules::FactoryRadioVerifierHandshake ineligible;
    ineligible.SetEligible(false);
    Check(!ineligible.Handle(hello, PEER_NONCE, ignored),
          "ordinary unqualified Litch never answers factory traffic");

    const auto unsupportedChannel = static_cast<modules::FactoryRadioChannel>(2U);
    modules::FactoryRadioDutHandshake strictDut;
    Check(!strictDut.Start(unsupportedChannel, SESSION, DUT_NONCE, hello),
          "unsupported transport cannot start a factory proof");
    Check(strictDut.Start(modules::FactoryRadioChannel::Ble,
                          SESSION, DUT_NONCE, hello, PEER_NONCE),
          "fresh BLE proof can bind an expected verifier nonce");
    auto wrongChannel = challenge;
    wrongChannel.channel = unsupportedChannel;
    Check(!strictDut.Handle(wrongChannel, ignored) && !strictDut.IsVerified(),
          "unsupported channel cannot satisfy BLE proof");
    auto oldEpoch = challenge;
    oldEpoch.version = 2U;
    Check(!strictDut.Handle(oldEpoch, ignored) && !strictDut.IsVerified(),
          "prior dual-radio epoch cannot satisfy BLE proof");
    auto wrongPeer = challenge;
    wrongPeer.senderNonce = PEER_NONCE + 1U;
    Check(!strictDut.Handle(wrongPeer, ignored) && strictDut.GetPeerNonce() == 0U,
          "wrong peer nonce cannot satisfy bound BLE proof");
    auto wrongSession = challenge;
    ++wrongSession.sessionId;
    Check(!strictDut.Handle(wrongSession, ignored),
          "proof cannot cross manufacturing sessions");
    auto wrongEcho = challenge;
    ++wrongEcho.echoNonce;
    Check(!strictDut.Handle(wrongEcho, ignored),
          "challenge must echo the actual DUT nonce");
    Check(!strictDut.Handle(verified, ignored) && !strictDut.IsVerified(),
          "terminal packet without a challenge never grants proof");
    Check(strictDut.Handle(challenge, response) && !strictDut.IsVerified(),
          "matching challenge remains pending until terminal proof");
    Check(strictDut.Handle(verified, response) && strictDut.IsVerified(),
          "complete matching BLE nonce exchange grants proof");

    modules::FactoryRadioVerifierHandshake strictVerifier;
    strictVerifier.SetEligible(true);
    auto unsupportedHello = hello;
    unsupportedHello.channel = unsupportedChannel;
    auto oldHello = hello;
    oldHello.version = 2U;
    Check(!strictVerifier.Handle(unsupportedHello, PEER_NONCE, ignored) &&
              !strictVerifier.Handle(oldHello, PEER_NONCE, ignored) &&
              !strictVerifier.WasHandshakeCompleted(),
          "direct verifier input rejects old epoch and unsupported channel");

    modules::FactoryRadioWireFrame wire{};
    Check(modules::EncodeFactoryRadioFrame(verified, wire),
          "BLE proof retains its bounded twenty-byte wire frame");
    modules::FactoryRadioFrame decoded;
    Check(modules::DecodeFactoryRadioFrame(wire.data(), wire.size(), decoded) &&
              decoded.type == modules::FactoryRadioFrameType::Verified &&
              decoded.sessionId == SESSION,
          "wire frame round trips across BLE GATT");
    const modules::FactoryRadioWireFrame oldEpochWire{
        0x4CU, 0x46U, 0x54U, 0x02U, 0x01U, 0x04U, 0x40U, 0x30U, 0x20U, 0x10U,
        0xD4U, 0xC3U, 0xB2U, 0xA1U, 0x88U, 0x77U, 0x66U, 0x55U, 0xDCU, 0x38U};
    const modules::FactoryRadioWireFrame unsupportedChannelWire{
        0x4CU, 0x46U, 0x54U, 0x03U, 0x02U, 0x04U, 0x40U, 0x30U, 0x20U, 0x10U,
        0xD4U, 0xC3U, 0xB2U, 0xA1U, 0x88U, 0x77U, 0x66U, 0x55U, 0x83U, 0x1BU};
    Check(!modules::DecodeFactoryRadioFrame(oldEpochWire.data(), oldEpochWire.size(), decoded) &&
              !modules::DecodeFactoryRadioFrame(unsupportedChannelWire.data(), unsupportedChannelWire.size(), decoded),
          "valid CRC cannot admit legacy epoch or unsupported channel bytes");
    Check(!modules::EncodeFactoryRadioFrame(oldEpoch, wire) &&
              !modules::EncodeFactoryRadioFrame(wrongChannel, wire),
          "encoder cannot publish a legacy epoch or unsupported channel");
    wire[9] ^= 0x40U;
    Check(!modules::DecodeFactoryRadioFrame(wire.data(), wire.size(), decoded),
          "damaged radio packet is rejected before state changes");

    if (g_failures == 0) {
        std::puts("[FACTORY_RADIO_PROTOCOL] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
