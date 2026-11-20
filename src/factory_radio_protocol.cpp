#include "modules/factory_radio_protocol.hpp"

namespace modules {

namespace {

uint16_t Crc16(const uint8_t* bytes, const size_t length) {
    uint16_t crc = 0xFFFFU;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= static_cast<uint16_t>(bytes[index]) << 8U;
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                      ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                      : static_cast<uint16_t>(crc << 1U);
        }
    }
    return crc;
}

void WriteU32(FactoryRadioWireFrame& out,
              const size_t offset,
              const uint32_t value) {
    for (uint8_t byte = 0U; byte < 4U; ++byte) {
        out[offset + byte] = static_cast<uint8_t>(value >> (byte * 8U));
    }
}

uint32_t ReadU32(const uint8_t* bytes, const size_t offset) {
    uint32_t value = 0U;
    for (uint8_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<uint32_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

bool HasKnownType(const FactoryRadioFrameType type) {
    return type == FactoryRadioFrameType::Hello ||
           type == FactoryRadioFrameType::Challenge ||
           type == FactoryRadioFrameType::Response ||
           type == FactoryRadioFrameType::Verified;
}

bool HasKnownChannel(const FactoryRadioChannel channel) {
    return channel == FactoryRadioChannel::Ble;
}

bool HasValidEnvelope(const FactoryRadioFrame& frame) {
    return frame.version == FACTORY_RADIO_PROTOCOL_VERSION &&
           HasKnownChannel(frame.channel) && HasKnownType(frame.type) &&
           frame.sessionId != 0U && frame.senderNonce != 0U;
}

FactoryRadioFrame MakeFrame(const FactoryRadioChannel channel,
                            const FactoryRadioFrameType type,
                            const uint32_t sessionId,
                            const uint32_t senderNonce,
                            const uint32_t echoNonce) {
    return FactoryRadioFrame{
        FACTORY_RADIO_PROTOCOL_VERSION,
        channel,
        type,
        sessionId,
        senderNonce,
        echoNonce,
    };
}

} // namespace

bool EncodeFactoryRadioFrame(const FactoryRadioFrame& frame,
                             FactoryRadioWireFrame& out) {
    if (!HasValidEnvelope(frame)) {
        return false;
    }
    out = {};
    out[0] = 'L';
    out[1] = 'F';
    out[2] = 'T';
    out[3] = frame.version;
    out[4] = static_cast<uint8_t>(frame.channel);
    out[5] = static_cast<uint8_t>(frame.type);
    WriteU32(out, 6U, frame.sessionId);
    WriteU32(out, 10U, frame.senderNonce);
    WriteU32(out, 14U, frame.echoNonce);
    const uint16_t crc = Crc16(out.data(), 18U);
    out[18] = static_cast<uint8_t>(crc);
    out[19] = static_cast<uint8_t>(crc >> 8U);
    return true;
}

bool DecodeFactoryRadioFrame(const uint8_t* bytes,
                             const size_t length,
                             FactoryRadioFrame& out) {
    if (bytes == nullptr || length != FACTORY_RADIO_WIRE_FRAME_SIZE ||
        bytes[0] != 'L' || bytes[1] != 'F' || bytes[2] != 'T') {
        return false;
    }
    const uint16_t expectedCrc = static_cast<uint16_t>(
        bytes[18] | (static_cast<uint16_t>(bytes[19]) << 8U));
    if (Crc16(bytes, 18U) != expectedCrc) {
        return false;
    }
    const FactoryRadioFrame candidate{
        bytes[3],
        static_cast<FactoryRadioChannel>(bytes[4]),
        static_cast<FactoryRadioFrameType>(bytes[5]),
        ReadU32(bytes, 6U),
        ReadU32(bytes, 10U),
        ReadU32(bytes, 14U),
    };
    if (!HasValidEnvelope(candidate)) {
        return false;
    }
    out = candidate;
    return true;
}

bool FactoryRadioDutHandshake::Start(const FactoryRadioChannel channel,
                                     const uint32_t sessionId,
                                     const uint32_t dutNonce,
                                     FactoryRadioFrame& outHello,
                                     const uint32_t expectedPeerNonce) {
    if (!HasKnownChannel(channel) || sessionId == 0U || dutNonce == 0U) {
        return false;
    }
    m_channel = channel;
    m_sessionId = sessionId;
    m_dutNonce = dutNonce;
    m_peerNonce = 0U;
    m_expectedPeerNonce = expectedPeerNonce;
    m_state = State::AwaitChallenge;
    outHello = MakeFrame(channel, FactoryRadioFrameType::Hello,
                         sessionId, dutNonce, 0U);
    return true;
}

bool FactoryRadioDutHandshake::Handle(const FactoryRadioFrame& frame,
                                      FactoryRadioFrame& outResponse) {
    if (!HasValidEnvelope(frame) || frame.channel != m_channel ||
        frame.sessionId != m_sessionId) {
        return false;
    }
    if (m_state == State::AwaitChallenge &&
        frame.type == FactoryRadioFrameType::Challenge &&
        frame.echoNonce == m_dutNonce &&
        frame.senderNonce != m_dutNonce &&
        (m_expectedPeerNonce == 0U ||
         frame.senderNonce == m_expectedPeerNonce)) {
        m_peerNonce = frame.senderNonce;
        outResponse = MakeFrame(m_channel, FactoryRadioFrameType::Response,
                                m_sessionId, m_dutNonce, m_peerNonce);
        m_state = State::AwaitVerified;
        return true;
    }
    if (m_state == State::AwaitVerified &&
        frame.type == FactoryRadioFrameType::Challenge &&
        frame.senderNonce == m_peerNonce &&
        frame.echoNonce == m_dutNonce) {
        outResponse = MakeFrame(m_channel, FactoryRadioFrameType::Response,
                                m_sessionId, m_dutNonce, m_peerNonce);
        return true;
    }
    if (m_state == State::AwaitVerified &&
        frame.type == FactoryRadioFrameType::Verified &&
        frame.senderNonce == m_peerNonce &&
        frame.echoNonce == m_dutNonce) {
        m_state = State::Verified;
        return true;
    }
    return false;
}

void FactoryRadioVerifierHandshake::SetEligible(const bool eligible) {
    m_eligible = eligible;
    if (!eligible) {
        m_state = State::Idle;
    }
}

bool FactoryRadioVerifierHandshake::StartChallenge(
    const FactoryRadioFrame& hello,
    const uint32_t peerNonce,
    FactoryRadioFrame& outChallenge) {
    if (peerNonce == 0U || peerNonce == hello.senderNonce ||
        (hello.sessionId == m_lastCompletedSessionId &&
         hello.senderNonce == m_lastCompletedDutNonce)) {
        return false;
    }
    m_channel = hello.channel;
    m_sessionId = hello.sessionId;
    m_dutNonce = hello.senderNonce;
    m_peerNonce = peerNonce;
    m_state = State::AwaitResponse;
    outChallenge = MakeFrame(m_channel, FactoryRadioFrameType::Challenge,
                             m_sessionId, m_peerNonce, m_dutNonce);
    return true;
}

bool FactoryRadioVerifierHandshake::CompleteResponse(
    const FactoryRadioFrame& response,
    FactoryRadioFrame& outVerified) {
    if (response.channel != m_channel ||
        response.sessionId != m_sessionId ||
        response.senderNonce != m_dutNonce ||
        response.echoNonce != m_peerNonce) {
        return false;
    }
    outVerified = MakeFrame(m_channel, FactoryRadioFrameType::Verified,
                            m_sessionId, m_peerNonce, m_dutNonce);
    m_lastCompletedSessionId = m_sessionId;
    m_lastCompletedDutNonce = m_dutNonce;
    m_state = State::Completed;
    return true;
}

bool FactoryRadioVerifierHandshake::Handle(
    const FactoryRadioFrame& frame,
    const uint32_t proposedPeerNonce,
    FactoryRadioFrame& outFrame) {
    if (!m_eligible || !HasValidEnvelope(frame)) {
        return false;
    }
    if (frame.type == FactoryRadioFrameType::Hello) {
        if (m_state == State::AwaitResponse) {
            if (frame.channel != m_channel ||
                frame.sessionId != m_sessionId ||
                frame.senderNonce != m_dutNonce) {
                return false;
            }
            outFrame = MakeFrame(m_channel,
                                 FactoryRadioFrameType::Challenge,
                                 m_sessionId,
                                 m_peerNonce,
                                 m_dutNonce);
            return true;
        }
        return StartChallenge(frame, proposedPeerNonce, outFrame);
    }
    if (m_state == State::AwaitResponse &&
        frame.type == FactoryRadioFrameType::Response) {
        return CompleteResponse(frame, outFrame);
    }
    if (m_state == State::Completed &&
        frame.type == FactoryRadioFrameType::Response &&
        frame.channel == m_channel &&
        frame.sessionId == m_sessionId &&
        frame.senderNonce == m_dutNonce &&
        frame.echoNonce == m_peerNonce) {
        outFrame = MakeFrame(m_channel,
                             FactoryRadioFrameType::Verified,
                             m_sessionId,
                             m_peerNonce,
                             m_dutNonce);
        return true;
    }
    return false;
}

} // namespace modules
