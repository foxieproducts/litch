#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules {

inline constexpr uint8_t FACTORY_RADIO_PROTOCOL_VERSION = 3U;

enum class FactoryRadioChannel : uint8_t {
    Ble = 1U,
};

enum class FactoryRadioFrameType : uint8_t {
    Hello = 1U,
    Challenge = 2U,
    Response = 3U,
    Verified = 4U,
};

struct FactoryRadioFrame {
    uint8_t version = FACTORY_RADIO_PROTOCOL_VERSION;
    FactoryRadioChannel channel = FactoryRadioChannel::Ble;
    FactoryRadioFrameType type = FactoryRadioFrameType::Hello;
    uint32_t sessionId = 0U;
    uint32_t senderNonce = 0U;
    uint32_t echoNonce = 0U;
};

inline constexpr size_t FACTORY_RADIO_WIRE_FRAME_SIZE = 20U;
using FactoryRadioWireFrame =
    std::array<uint8_t, FACTORY_RADIO_WIRE_FRAME_SIZE>;

bool EncodeFactoryRadioFrame(const FactoryRadioFrame& frame,
                             FactoryRadioWireFrame& out);
bool DecodeFactoryRadioFrame(const uint8_t* bytes,
                             size_t length,
                             FactoryRadioFrame& out);

class FactoryRadioDutHandshake final {
  public:
    bool Start(FactoryRadioChannel channel,
               uint32_t sessionId,
               uint32_t dutNonce,
               FactoryRadioFrame& outHello,
               uint32_t expectedPeerNonce = 0U);

    bool Handle(const FactoryRadioFrame& frame,
                FactoryRadioFrame& outResponse);

    bool IsVerified() const { return m_state == State::Verified; }
    uint32_t GetPeerNonce() const { return m_peerNonce; }

  private:
    enum class State : uint8_t {
        Idle = 0U,
        AwaitChallenge,
        AwaitVerified,
        Verified,
    };

    State m_state = State::Idle;
    FactoryRadioChannel m_channel = FactoryRadioChannel::Ble;
    uint32_t m_sessionId = 0U;
    uint32_t m_dutNonce = 0U;
    uint32_t m_peerNonce = 0U;
    uint32_t m_expectedPeerNonce = 0U;
};

class FactoryRadioVerifierHandshake final {
  public:
    void SetEligible(bool eligible);

    bool Handle(const FactoryRadioFrame& frame,
                uint32_t proposedPeerNonce,
                FactoryRadioFrame& outFrame);

    bool WasHandshakeCompleted() const {
        return m_state == State::Completed;
    }

  private:
    enum class State : uint8_t {
        Idle = 0U,
        AwaitResponse,
        Completed,
    };

    bool StartChallenge(const FactoryRadioFrame& hello,
                        uint32_t peerNonce,
                        FactoryRadioFrame& outChallenge);
    bool CompleteResponse(const FactoryRadioFrame& response,
                          FactoryRadioFrame& outVerified);

    bool m_eligible = false;
    State m_state = State::Idle;
    FactoryRadioChannel m_channel = FactoryRadioChannel::Ble;
    uint32_t m_sessionId = 0U;
    uint32_t m_dutNonce = 0U;
    uint32_t m_peerNonce = 0U;
    uint32_t m_lastCompletedSessionId = 0U;
    uint32_t m_lastCompletedDutNonce = 0U;
};

} // namespace modules
