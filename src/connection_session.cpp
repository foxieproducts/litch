#include "modules/connection_session.hpp"
#include "modules/ble_ota_chunk_queue.hpp"

#include <algorithm>

#include "modules/home_state_wire_codec.hpp"
#include "modules/owner_pairing_session_wire.hpp"
#include "modules/settings_state_wire_codec.hpp"
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"

namespace modules {
namespace wire = connection_session_codec;

static_assert(size_t(ConnectionSession::OBJECT_CAPACITY) *
                  BleOtaChunkQueue::MAX_CHUNK_BYTES +
                  BleOtaChunkQueue::MAX_CHUNK_BYTES <=
              BleOtaChunkQueue::GUARANTEED_PAYLOAD_BYTES,
              "Every Object lease plus one raw record must fit the shared byte arena");

bool ConnectionSession::Begin(const uint32_t generation,
                              const uint64_t bootNonce,
                              const uint16_t packetMaximum,
                              const uint16_t orderedTopic) {
    if (generation == 0U || bootNonce == 0U || orderedTopic == 0U ||
        packetMaximum < wire::MIN_PACKET_BYTES ||
        packetMaximum > REVERSE_OBJECT_PACKET_BYTES) {
        return false;
    }
    m_generation = generation;
    m_bootNonce = bootNonce;
    m_packetMaximum = packetMaximum;
    m_orderedTopic = orderedTopic;
    m_stateOutputs = {};
    ResetNegotiation();
    return true;
}

void ConnectionSession::End(const uint32_t generation) {
    if (generation != m_generation) return;
    if (m_streamToken != 0U) (void)m_orderedWindow.Retire(m_streamToken);
    m_state = State::Inactive;
    m_connectionToken = 0U;
    m_streamToken = 0U;
    m_orderedMaximumBody = 0U;
    m_stateStreamToken = 0U;
    m_stateMaximumBody = 0U;
    m_stateCapacity = 0U;
    m_objectStreamToken = 0U;
    m_objectStreamBusy = false;
    m_lastClosedToken = 0U;
    m_objectAcceptedThrough = 0U;
    m_objectGrantThrough = 0U;
    m_objectHeldCredits = 0U;
    m_objectPendingSequence = 0U;
    m_objectPendingOrdinal = 0U;
    m_requestedObjectReceipt = 0U;
    m_objectCapacity = 0U;
    m_objectMaximumBody = 0U;
    m_objectAcceptedSize = 0U;
    m_objectPendingSize = 0U;
    m_reverseObjectOpenSentAtMs = 0U;
    m_reverseObjectStreamToken = 0U;
    m_reverseObjectConfirmedThrough = 0U;
    m_reverseObjectTerminalThrough = 0U;
    m_reverseObjectGrantThrough = 0U;
    m_reverseObjectNextSequence = 1U;
    m_reverseObjectMaximumBody = 0U;
    m_reverseObjectCapacity = 0U;
    m_lifecycleCount = 0U;
    m_progress = {};
    m_objectProgress = {};
    m_reverseObjectOutputs = {};
    m_successfulReverseObjectReplays = 0U;
    m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    m_checkpoint = {};
    m_outcome = {};
    m_stateOutputs = {};
    m_nextStateOutput = 0U;
    m_nextPriorityStateOutput = 0U;
    m_selectedStateOutput = STATE_CAPACITY;
    m_stateOutputBurst = 0U;
    m_selectedStatePriority = false;
    m_selectedOutput = SelectedOutput::None;
}

void ConnectionSession::Update(const uint32_t generation) {
    if (generation != m_generation || m_state == State::Inactive) {
        return;
    }
    if (m_state == State::Active) {
        if (m_stateStreamToken == 0U &&
            (m_stateOpenSentAtMs == 0U ||
             static_cast<uint32_t>(fxe::Millis() - m_stateOpenSentAtMs) >=
                 2000U)) {
            (void)QueueStateOpen();
        }
        if (m_reverseObjectStreamToken == 0U &&
            (m_reverseObjectOpenSentAtMs == 0U ||
             static_cast<uint32_t>(fxe::Millis() -
                                   m_reverseObjectOpenSentAtMs) >= 2000U)) {
            (void)QueueReverseObjectOpen();
        }
        const uint32_t nowMs = fxe::Millis();
        for (auto& output : m_reverseObjectOutputs) {
            if (output.size == 0U || output.pending ||
                output.sendAttempts == 0U) {
                continue;
            }
            const uint8_t shift = static_cast<uint8_t>(std::min<uint8_t>(
                static_cast<uint8_t>(output.sendAttempts - 1U), 3U));
            const uint16_t retryMs = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(REVERSE_OBJECT_RECOVERY_MS) << shift,
                REVERSE_OBJECT_MAX_RECOVERY_MS));
            if (static_cast<uint32_t>(nowMs - output.lastSentAtMs) >= retryMs) {
                output.pending = true;
            }
        }
        return;
    }
    if (static_cast<uint32_t>(fxe::Millis() - m_startedAtMs) >=
        NEGOTIATION_TIMEOUT_MS) {
        ResetNegotiation();
    }
}

bool ConnectionSession::PublishState(const uint8_t code,
                                     const uint32_t revision,
                                     const uint8_t* const body,
                                     const size_t size) {
    const size_t maximumBody = m_stateStreamToken == 0U
        ? STATE_BODY_BYTES
        : m_stateMaximumBody;
    if (code == 0U || revision == 0U || size > maximumBody ||
        (size != 0U && body == nullptr)) {
        return false;
    }
    StateOutput* output = FindState(code);
    if (output != nullptr &&
        output->size == wire::HEADER_BYTES + size &&
        fxe::ReadU32LE(output->bytes.data() + 6U) == revision &&
        (size == 0U ||
         std::equal(body, body + size,
                    output->bytes.begin() + wire::HEADER_BYTES))) {
        return true;
    }
    if (output == nullptr) output = FindEmptyState();
    return output != nullptr &&
           EncodeRetainedState(*output, code, revision, body, size);
}

bool ConnectionSession::StateOutputAllowed(const uint8_t code) const {
    return m_productAccess || code == RUNTIME_HEALTH_STATE_CODE ||
        (code >= owner_pairing_session_wire::STATUS_CODE &&
         code <= owner_pairing_session_wire::IDENTITY_HIGH_CODE);
}

bool ConnectionSession::ReverseObjectOutputAllowed(const uint8_t code) const {
    return m_productAccess || code == PUBLIC_SUPPORT_OBJECT_OPCODE;
}

void ConnectionSession::SetProductAccess(const bool allowed) {
    if (m_productAccess == allowed) return;
    m_productAccess = allowed;
    if (allowed) return;
    // A retained Checkpoint may predate revocation. Keep its delivery receipt,
    // but never disclose a protected domain revision/digest through it.
    if (m_checkpoint.size >= wire::HEADER_BYTES + 8U) {
        std::fill_n(m_checkpoint.bytes.begin() + wire::HEADER_BYTES, 8U, 0U);
    }
    if (m_selectedOutput == SelectedOutput::ReverseObject ||
        (m_selectedOutput == SelectedOutput::State &&
         m_selectedStateOutput < STATE_CAPACITY &&
         !StateOutputAllowed(m_stateOutputs[m_selectedStateOutput].bytes[1U]))) {
        m_selectedOutput = SelectedOutput::None;
        m_selectedStateOutput = STATE_CAPACITY;
        m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    }
}

bool ConnectionSession::HasBulkReverseObject() const {
    for (const auto& output : m_reverseObjectOutputs) {
        if (output.size != 0U && output.bulk &&
            ReverseObjectOutputAllowed(output.bytes[1U])) return true;
    }
    return false;
}

bool ConnectionSession::PublishReverseObject(
    const uint8_t code, const uint32_t request, const uint8_t* const body,
    const size_t size, const bool bulk) {
    if (!ReverseObjectOutputAllowed(code) || m_state != State::Active || m_reverseObjectStreamToken == 0U ||
        m_reverseObjectCapacity == 0U || code == 0U ||
        request == 0U || body == nullptr || size == 0U ||
        size > m_reverseObjectMaximumBody ||
        size + wire::HEADER_BYTES > m_packetMaximum ||
        m_reverseObjectNextSequence > m_reverseObjectGrantThrough ||
        m_reverseObjectNextSequence > wire::LAST_SEQUENCE) {
        return false;
    }
    const uint8_t slot = static_cast<uint8_t>(
        (m_reverseObjectNextSequence - 1U) % m_reverseObjectCapacity);
    auto& output = m_reverseObjectOutputs[slot];
    if (output.size != 0U) return false;
    const wire::PacketView receipt{
        {wire::Kind::Object, code, m_reverseObjectStreamToken,
         m_reverseObjectNextSequence},
        {body, size}};
    size_t encodedSize = 0U;
    if (wire::Codec::Encode(
            receipt,
            {output.bytes.data(), output.bytes.size()},
            encodedSize) != wire::Error::None) {
        return false;
    }
    output.size = static_cast<uint16_t>(encodedSize);
    output.sequence = m_reverseObjectNextSequence++;
    output.bulk = bulk;
    output.pending = true;
    return true;
}

ConnectionSession::Result ConnectionSession::Ingress(
    const uint32_t generation, const uint8_t* const bytes, const size_t size,
    ObjectCommand* const object, const uint32_t ingressOrdinal) {
    if (object != nullptr) *object = {};
    if (generation != m_generation) return Result::StaleGeneration;
    if (m_state == State::Inactive) return Result::Inactive;
    wire::PacketView packet;
    const auto encoded = fxe::ConstByteSpan(bytes, size);
    if (wire::Codec::Decode(encoded, packet, m_packetMaximum) !=
        wire::Error::None) {
        return Result::Malformed;
    }
    if (packet.header.kind == wire::Kind::Control) {
        return HandleControl(packet, encoded);
    }
    if (packet.header.kind == wire::Kind::Object) {
        if (m_state != State::Active || m_objectStreamToken == 0U ||
            packet.header.token != m_objectStreamToken || object == nullptr ||
            size > OBJECT_PACKET_BYTES ||
            packet.body.Size() > m_objectMaximumBody) {
            return Result::Unexpected;
        }
        if (packet.header.sequence == m_objectAcceptedThrough) {
            return size == m_objectAcceptedSize &&
                    std::equal(bytes, bytes + size, m_objectAccepted.begin())
                ? Result::Ok
                : Result::Conflict;
        }
        if (packet.header.sequence != m_objectAcceptedThrough + 1U ||
            packet.header.sequence > m_objectGrantThrough) {
            return Result::NoCapacity;
        }
        if (m_objectPendingSequence != 0U) {
            return packet.header.sequence == m_objectPendingSequence &&
                    size == m_objectPendingSize &&
                    std::equal(bytes, bytes + size, m_objectPending.begin())
                ? Result::NoCapacity
                : Result::Conflict;
        }
        std::copy_n(bytes, size, m_objectPending.begin());
        m_objectPendingSequence = packet.header.sequence;
        m_objectPendingSize = static_cast<uint16_t>(size);
        m_objectPendingOrdinal = ingressOrdinal;
        object->topic = LITCHCRAFT_OBJECT_TOPIC;
        object->streamToken = packet.header.token;
        object->sequence = packet.header.sequence;
        object->opcode = packet.header.code;
        object->body = {m_objectPending.data() + wire::HEADER_BYTES,
                        size - wire::HEADER_BYTES};
        object->ingressOrdinal = ingressOrdinal;
        return Result::Ok;
    }
    if (m_state != State::Active || m_streamToken == 0U ||
        packet.header.kind != wire::Kind::Ordered ||
        packet.header.token != m_streamToken || size > INTERACTIVE_PACKET_BYTES ||
        packet.body.Size() > m_orderedMaximumBody) {
        return Result::Unexpected;
    }
    switch (m_orderedWindow.Ingress(packet.header.token,
                                    packet.header.sequence, encoded)) {
    case Window::Result::Ok:
    case Window::Result::Duplicate:
    case Window::Result::AlreadyConfirmed:
        (void)PublishProgress();
        return Result::Ok;
    case Window::Result::ChangedDuplicate:
        return Result::Conflict;
    case Window::Result::OutsideWindow:
        (void)PublishProgress();
        return Result::NoCapacity;
    default:
        return Result::Unexpected;
    }
}

uint16_t ConnectionSession::TakeRequestedObjectReceipt() {
    const uint16_t request = m_requestedObjectReceipt;
    m_requestedObjectReceipt = 0U;
    return request;
}

ConnectionSession::Result ConnectionSession::AcceptObject(
    const uint32_t streamToken, const uint32_t sequence,
    const bool retainCredit) {
    if (m_state != State::Active || streamToken != m_objectStreamToken) {
        return Result::StaleGeneration;
    }
    if (sequence != m_objectPendingSequence || m_objectPendingSize == 0U) {
        return Result::Unexpected;
    }
    std::copy_n(m_objectPending.begin(), m_objectPendingSize,
                m_objectAccepted.begin());
    m_objectAcceptedSize = m_objectPendingSize;
    m_objectAcceptedThrough = sequence;
    if (retainCredit) ++m_objectHeldCredits;
    else if (m_objectGrantThrough < wire::LAST_SEQUENCE) ++m_objectGrantThrough;
    m_objectPendingSequence = 0U;
    m_objectPendingOrdinal = 0U;
    m_objectPendingSize = 0U;
    (void)PublishObjectProgress();
    return Result::Ok;
}

void ConnectionSession::RejectObject(const uint32_t streamToken,
                                     const uint32_t sequence) {
    if (streamToken == m_objectStreamToken &&
        sequence == m_objectPendingSequence) {
        m_objectPendingSequence = 0U;
        m_objectPendingOrdinal = 0U;
        m_objectPendingSize = 0U;
    }
}

bool ConnectionSession::PeekPendingObject(ObjectCommand& object) const {
    object = {};
    if (m_state != State::Active || m_objectPendingSequence == 0U) return false;
    wire::PacketView packet;
    if (wire::Codec::Decode({m_objectPending.data(), m_objectPendingSize},
                            packet, m_packetMaximum) != wire::Error::None) return false;
    object = {LITCHCRAFT_OBJECT_TOPIC, packet.header.token,
              packet.header.sequence, packet.header.code, packet.body,
              m_objectPendingOrdinal};
    return true;
}

void ConnectionSession::ReturnObjectCredit(const uint32_t streamToken) {
    if (m_state != State::Active || streamToken == 0U ||
        streamToken != m_objectStreamToken || m_objectHeldCredits == 0U) return;
    --m_objectHeldCredits;
    if (m_objectGrantThrough < wire::LAST_SEQUENCE) ++m_objectGrantThrough;
    (void)PublishObjectProgress();
}

bool ConnectionSession::PeekOutput(const uint32_t generation,
                                   uint8_t* const out,
                                   const size_t capacity,
                                   size_t& outSize) {
    outSize = 0U;
    if (generation != m_generation || m_state == State::Inactive ||
        out == nullptr) {
        return false;
    }
    if (m_selectedOutput == SelectedOutput::ReverseObject &&
        SelectStateOutputWithinBurst()) {
        // A failed notify leaves the bulk packet selected. Keep it retained,
        // but let queried State use its bounded share of the next openings.
        m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
        m_selectedOutput = SelectedOutput::State;
    }
    if (m_selectedOutput == SelectedOutput::None) {
        if (m_lifecycleCount != 0U) {
            m_selectedOutput = SelectedOutput::Lifecycle;
        } else if (m_outcome.pending) {
            m_selectedOutput = SelectedOutput::Outcome;
        } else if (m_checkpoint.pending) {
            // A completed ordered command's Checkpoint is its authoritative
            // app-visible receipt. Send it before the redundant window
            // Progress packet so a 30 ms iOS link does not add one full
            // connection event to every interactive acknowledgement.
            m_selectedOutput = SelectedOutput::Checkpoint;
        } else if (m_progress.pending) {
            m_selectedOutput = SelectedOutput::Progress;
        } else if (m_objectProgress.pending) {
            m_selectedOutput = SelectedOutput::ObjectProgress;
        } else if (SelectStateOutputWithinBurst()) {
            m_selectedOutput = SelectedOutput::State;
        } else if (SelectNextReverseObjectOutput()) {
            m_selectedOutput = SelectedOutput::ReverseObject;
        } else {
            // Without eligible bulk, continue State service without resetting
            // either rotation. A Query re-arms keys, never their place in line.
            m_stateOutputBurst = 0U;
            if (!SelectStateOutputWithinBurst()) return false;
            m_selectedOutput = SelectedOutput::State;
        }
    }
    switch (m_selectedOutput) {
    case SelectedOutput::Lifecycle:
        return CopyOutput(m_lifecycle[m_lifecycleHead], out, capacity, outSize);
    case SelectedOutput::Outcome:
        return CopyOutput(m_outcome, out, capacity, outSize);
    case SelectedOutput::Progress:
        return CopyOutput(m_progress, out, capacity, outSize);
    case SelectedOutput::ObjectProgress:
        return CopyOutput(m_objectProgress, out, capacity, outSize);
    case SelectedOutput::ReverseObject:
        return CopyReverseObject(out, capacity, outSize);
    case SelectedOutput::Checkpoint:
        return CopyOutput(m_checkpoint, out, capacity, outSize);
    case SelectedOutput::State:
        return CopyStateOutput(out, capacity, outSize);
    case SelectedOutput::None:
        return false;
    }
    return false;
}

void ConnectionSession::AcceptOutput(const uint32_t generation) {
    if (generation != m_generation) return;
    switch (m_selectedOutput) {
    case SelectedOutput::Lifecycle:
        m_lifecycle[m_lifecycleHead] = {};
        m_lifecycleHead = static_cast<uint8_t>((m_lifecycleHead + 1U) % 2U);
        if (m_lifecycleCount != 0U) --m_lifecycleCount;
        break;
    case SelectedOutput::Outcome:
        m_outcome.pending = false;
        break;
    case SelectedOutput::Progress:
        m_progress.pending = false;
        break;
    case SelectedOutput::ObjectProgress:
        m_objectProgress.pending = false;
        break;
    case SelectedOutput::ReverseObject:
        if (m_selectedReverseObjectOutput < m_reverseObjectCapacity) {
            auto& output =
                m_reverseObjectOutputs[m_selectedReverseObjectOutput];
            if (output.sendAttempts != 0U && m_successfulReverseObjectReplays != UINT32_MAX)
                ++m_successfulReverseObjectReplays;
            output.pending = false;
            output.lastSentAtMs = fxe::Millis();
            if (output.sendAttempts != UINT8_MAX) ++output.sendAttempts;
        }
        m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
        m_stateOutputBurst = 0U;
        break;
    case SelectedOutput::Checkpoint:
        m_checkpoint.pending = false;
        break;
    case SelectedOutput::State:
        if (m_selectedStateOutput < STATE_CAPACITY) {
            m_stateOutputs[m_selectedStateOutput].pending = false;
            auto& next = m_selectedStatePriority ? m_nextPriorityStateOutput
                                                : m_nextStateOutput;
            next = static_cast<uint8_t>(
                (m_selectedStateOutput + 1U) % m_stateCapacity);
        }
        m_selectedStateOutput = STATE_CAPACITY;
        if (m_stateOutputBurst < PRIORITY_STATE_BURST) ++m_stateOutputBurst;
        m_selectedStatePriority = false;
        break;
    case SelectedOutput::None:
        break;
    }
    m_selectedOutput = SelectedOutput::None;
}

ConnectionSession::Result ConnectionSession::BeginNext(OrderedCommand& out) {
    out = {};
    if (m_state != State::Active || m_streamToken == 0U) {
        return Result::Inactive;
    }
    Window::Command command;
    const auto result = m_orderedWindow.BeginNext(m_streamToken, command);
    if (result == Window::Result::NotReady) return Result::NotReady;
    if (result != Window::Result::Ok) return Result::Unexpected;
    wire::PacketView packet;
    if (wire::Codec::Decode(command.payload, packet, m_packetMaximum) !=
        wire::Error::None || packet.header.kind != wire::Kind::Ordered ||
        packet.header.token != command.streamToken ||
        packet.header.sequence != command.sequence) {
        return Result::Malformed;
    }
    out.topic = m_orderedTopic;
    out.streamToken = command.streamToken;
    out.sequence = command.sequence;
    out.opcode = packet.header.code;
    out.body = packet.body;
    return Result::Ok;
}

bool ConnectionSession::ReadApplying(OrderedCommand& out) const {
    out = {};
    if (m_state != State::Active || m_streamToken == 0U) return false;
    Window::Command command;
    if (!m_orderedWindow.ReadApplying(m_streamToken, command)) return false;
    wire::PacketView packet;
    if (wire::Codec::Decode(command.payload, packet, m_packetMaximum) != wire::Error::None ||
        packet.header.kind != wire::Kind::Ordered || packet.header.token != command.streamToken ||
        packet.header.sequence != command.sequence) return false;
    out = {m_orderedTopic, command.streamToken, command.sequence, packet.header.code, packet.body};
    return true;
}

ConnectionSession::Result ConnectionSession::Complete(
    const uint32_t streamToken, const uint32_t sequence,
    const Window::Outcome outcome, const uint32_t domainRevision,
    const uint32_t domainDigest) {
    if (m_state != State::Active || streamToken != m_streamToken) {
        return Result::StaleGeneration;
    }
    if (m_orderedWindow.Complete(streamToken, sequence, outcome) !=
        Window::Result::Ok) {
        return Result::Unexpected;
    }
    (void)PublishProgress();
    (void)PublishCheckpoint(domainRevision, domainDigest);
    if (outcome.disposition != Window::Disposition::Applied) {
        (void)PublishOutcome(sequence, outcome);
    }
    return Result::Ok;
}

void ConnectionSession::ResetNegotiation() {
    if (m_streamToken != 0U) (void)m_orderedWindow.Retire(m_streamToken);
    m_state = State::AwaitHello;
    m_clientNonce = 0U;
    m_connectionToken = 0U;
    m_streamToken = 0U;
    m_orderedMaximumBody = 0U;
    m_stateStreamToken = 0U;
    m_stateMaximumBody = 0U;
    m_stateCapacity = 0U;
    m_objectStreamToken = 0U;
    m_objectStreamBusy = false;
    m_lastClosedToken = 0U;
    m_objectAcceptedThrough = 0U;
    m_objectGrantThrough = 0U;
    m_objectHeldCredits = 0U;
    m_objectPendingSequence = 0U;
    m_objectPendingOrdinal = 0U;
    m_requestedObjectReceipt = 0U;
    m_objectCapacity = 0U;
    m_objectMaximumBody = 0U;
    m_objectAcceptedSize = 0U;
    m_objectPendingSize = 0U;
    m_stateOpenSentAtMs = 0U;
    m_reverseObjectOpenSentAtMs = 0U;
    m_reverseObjectStreamToken = 0U;
    m_reverseObjectConfirmedThrough = 0U;
    m_reverseObjectTerminalThrough = 0U;
    m_reverseObjectGrantThrough = 0U;
    m_reverseObjectNextSequence = 1U;
    m_reverseObjectMaximumBody = 0U;
    m_reverseObjectCapacity = 0U;
    m_lastOpenRequest = 0U;
    m_lastOpenSize = 0U;
    m_lastOpenedSize = 0U;
    m_startedAtMs = fxe::Millis();
    m_lifecycleHead = 0U;
    m_lifecycleCount = 0U;
    m_progress = {};
    m_objectProgress = {};
    m_reverseObjectOutputs = {};
    m_successfulReverseObjectReplays = 0U;
    m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    m_checkpoint = {};
    m_outcome = {};
    for (auto& output : m_stateOutputs) output.pending = false;
    m_nextStateOutput = 0U;
    m_nextPriorityStateOutput = 0U;
    m_selectedStateOutput = STATE_CAPACITY;
    m_stateOutputBurst = 0U;
    m_selectedStatePriority = false;
    m_selectedOutput = SelectedOutput::None;
}

uint32_t ConnectionSession::AllocateToken() {
    if (m_nextToken == 0U || (m_nextToken & 0x80000000U) != 0U) return 0U;
    return m_nextToken++;
}

ConnectionSession::Result ConnectionSession::HandleControl(
    const wire::PacketView& packet, const fxe::ConstByteSpan encoded) {
    switch (static_cast<wire::Control>(packet.header.code)) {
    case wire::Control::Hello:
        return HandleHello(packet, encoded);
    case wire::Control::Ready:
        return HandleReady(packet);
    case wire::Control::Open:
        return HandleOpen(packet, encoded);
    case wire::Control::Opened:
        return HandleOpened(packet);
    case wire::Control::Progress:
        return HandleReverseObjectProgress(packet);
    case wire::Control::Query:
        return HandleQuery(packet);
    case wire::Control::Confirm:
        return HandleConfirm(packet);
    case wire::Control::Close:
        return HandleClose(packet);
    case wire::Control::Heartbeat:
        return m_state == State::Active &&
                       packet.header.token == m_connectionToken
                   ? Result::Ok
                   : Result::Unexpected;
    default:
        return Result::Unexpected;
    }
}

ConnectionSession::Result ConnectionSession::HandleHello(
    const wire::PacketView& packet, const fxe::ConstByteSpan encoded) {
    const uint64_t clientNonce = fxe::ReadU64LE(packet.body.Data());
    if (m_state != State::AwaitHello) {
        if (m_state != State::AwaitReady) return Result::Unexpected;
        if (clientNonce != m_clientNonce || encoded.Size() != m_hello.size() ||
            !std::equal(encoded.Data(), encoded.Data() + encoded.Size(),
                        m_hello.begin())) {
            return Result::Conflict;
        }
        wire::PacketView welcome;
        wire::PacketView identity;
        if (wire::Codec::Decode({m_welcome.data(), m_welcome.size()}, welcome,
                                m_packetMaximum) != wire::Error::None ||
            wire::Codec::Decode({m_identity.data(), m_identity.size()}, identity,
                                m_packetMaximum) != wire::Error::None ||
            !QueueLifecycle(welcome) || !QueueLifecycle(identity)) {
            return Result::NoCapacity;
        }
        return Result::Ok;
    }
    const uint16_t peerMaximum = fxe::ReadU16LE(packet.body.Data() + 8U);
    m_packetMaximum = std::min(m_packetMaximum, peerMaximum);
    m_connectionToken = AllocateToken();
    if (m_connectionToken == 0U) return Result::NoCapacity;
    m_clientNonce = clientNonce;
    std::copy_n(encoded.Data(), encoded.Size(), m_hello.begin());

    std::array<uint8_t, 10U> body{};
    fxe::WriteU64LE(clientNonce, body.data());
    fxe::WriteU16LE(m_packetMaximum, body.data() + 8U);
    const wire::PacketView welcome{{wire::Kind::Control,
                                    uint8_t(wire::Control::Welcome),
                                    m_connectionToken, 0U},
                                   {body.data(), body.size()}};
    size_t size = 0U;
    if (wire::Codec::Encode(welcome, {m_welcome.data(), m_welcome.size()},
                            size) != wire::Error::None ||
        size != m_welcome.size() || !QueueLifecycle(welcome)) {
        return Result::NoCapacity;
    }

    fxe::WriteU64LE(m_bootNonce, body.data());
    fxe::WriteU16LE(FEATURE_ORDERED | FEATURE_STATE | FEATURE_OBJECT |
                        FEATURE_REVERSE_OBJECT | FEATURE_STORE_GUARD_EPOCH | FEATURE_ORDERED_MODE,
                    body.data() + 8U);
    const wire::PacketView identity{{wire::Kind::Control,
                                     uint8_t(wire::Control::Identity),
                                     m_connectionToken, 0U},
                                    {body.data(), body.size()}};
    if (wire::Codec::Encode(identity, {m_identity.data(), m_identity.size()},
                            size) != wire::Error::None ||
        size != m_identity.size() || !QueueLifecycle(identity)) {
        return Result::NoCapacity;
    }
    m_state = State::AwaitReady;
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleReady(
    const wire::PacketView& packet) {
    if ((m_state != State::AwaitReady && m_state != State::Active) ||
        packet.header.token != m_connectionToken) {
        return Result::Unexpected;
    }
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES * 3U> transcript{};
    std::copy(m_hello.begin(), m_hello.end(), transcript.begin());
    std::copy(m_welcome.begin(), m_welcome.end(),
              transcript.begin() + INTERACTIVE_PACKET_BYTES);
    std::copy(m_identity.begin(), m_identity.end(),
              transcript.begin() + INTERACTIVE_PACKET_BYTES * 2U);
    const uint32_t expected = fxe::Crc32(transcript.data(), transcript.size());
    if (fxe::ReadU32LE(packet.body.Data()) != expected) return Result::Conflict;
    const wire::PacketView activated{{wire::Kind::Control,
                                      uint8_t(wire::Control::Activated),
                                      m_connectionToken, 0U},
                                     packet.body};
    if (!QueueLifecycle(activated)) return Result::NoCapacity;
    m_state = State::Active;
    (void)QueueStateOpen();
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleOpen(
    const wire::PacketView& packet, const fxe::ConstByteSpan encoded) {
    if (m_state != State::Active || packet.header.token != m_connectionToken) {
        return Result::Unexpected;
    }
    if (packet.header.sequence == m_lastOpenRequest) {
        if (encoded.Size() != m_lastOpenSize ||
            !std::equal(encoded.Data(), encoded.Data() + encoded.Size(),
                        m_lastOpen.begin())) {
            return Result::Conflict;
        }
        if (m_lastClosedToken != 0U &&
            m_lastOpenedSize >= wire::HEADER_BYTES + 4U &&
            fxe::ReadU32LE(m_lastOpened.data() + wire::HEADER_BYTES) ==
                m_lastClosedToken) {
            return Result::Unexpected;
        }
        wire::PacketView prior;
        if (wire::Codec::Decode(
                {m_lastOpened.data(), m_lastOpenedSize}, prior,
                m_packetMaximum) != wire::Error::None ||
            !QueueLifecycle(prior)) {
            return Result::NoCapacity;
        }
        return Result::Ok;
    }
    if (packet.header.sequence < m_lastOpenRequest) return Result::Unexpected;
    // A new Open must be able to expose its reply before allocating authority.
    // Exact retries above can still reuse an already queued lifecycle reply.
    if (m_lifecycleCount >= 2U) return Result::NoCapacity;
    const auto* const body = packet.body.Data();
    const uint16_t topic = fxe::ReadU16LE(body);
    const uint8_t lane = body[2U];
    const uint8_t requestedCapacity = body[3U];
    const uint16_t requestedBody = fxe::ReadU16LE(body + 4U);
    uint16_t status = 0U;
    uint32_t streamToken = 0U;
    uint16_t maximumBody = 0U;
    uint8_t capacity = 0U;
    const bool ordered = topic == m_orderedTopic && lane == 0U;
    const bool object = topic == LITCHCRAFT_OBJECT_TOPIC && lane == 1U;
    if (!ordered && !object) {
        status = REFUSED_UNSUPPORTED_TOPIC;
    } else if ((ordered && m_streamToken != 0U) ||
               (object && m_objectStreamToken != 0U)) {
        status = REFUSED_STREAM_BUSY;
    } else {
        streamToken = AllocateToken();
        maximumBody = std::min<uint16_t>(requestedBody,
            object ? std::min<size_t>(m_packetMaximum,
                                     OBJECT_PACKET_BYTES) - wire::HEADER_BYTES
                   : INTERACTIVE_PACKET_BYTES - wire::HEADER_BYTES);
        capacity = std::min<uint8_t>(requestedCapacity,
            object ? OBJECT_CAPACITY : ORDERED_CAPACITY);
        if (streamToken == 0U || capacity == 0U || maximumBody == 0U ||
            (ordered && !m_orderedWindow.Start(streamToken, capacity))) {
            status = REFUSED_STREAM_BUSY;
            streamToken = 0U;
            maximumBody = 0U;
            capacity = 0U;
        } else {
            if (ordered) {
                m_streamToken = streamToken;
                m_orderedMaximumBody = maximumBody;
            }
            else {
                m_objectStreamToken = streamToken;
                m_objectMaximumBody = maximumBody;
                m_objectCapacity = capacity;
                m_objectGrantThrough = capacity;
            }
        }
    }
    std::array<uint8_t, 10U> responseBody{};
    fxe::WriteU32LE(streamToken, responseBody.data());
    fxe::WriteU16LE(maximumBody, responseBody.data() + 4U);
    responseBody[6U] = capacity;
    responseBody[7U] = lane;
    fxe::WriteU16LE(status, responseBody.data() + 8U);
    const wire::PacketView opened{{wire::Kind::Control,
                                   uint8_t(wire::Control::Opened),
                                   m_connectionToken, packet.header.sequence},
                                  {responseBody.data(), responseBody.size()}};
    if (!QueueLifecycle(opened)) return Result::NoCapacity;
    size_t openedSize = 0U;
    if (wire::Codec::Encode(
            opened, {m_lastOpened.data(), m_lastOpened.size()},
            openedSize) != wire::Error::None || openedSize > UINT8_MAX) {
        return Result::Unexpected;
    }
    m_lastOpenedSize = static_cast<uint8_t>(openedSize);
    m_lastOpenRequest = packet.header.sequence;
    m_lastOpenSize = static_cast<uint8_t>(encoded.Size());
    std::copy_n(encoded.Data(), encoded.Size(), m_lastOpen.begin());
    if (ordered && m_streamToken != 0U) (void)PublishProgress();
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleOpened(
    const wire::PacketView& packet) {
    if (m_state != State::Active || packet.header.token != m_connectionToken ||
        (packet.header.sequence != 1U && packet.header.sequence != 2U)) {
        return Result::Unexpected;
    }
    const auto* const body = packet.body.Data();
    const uint32_t streamToken = fxe::ReadU32LE(body);
    const uint16_t maximumBody = fxe::ReadU16LE(body + 4U);
    const uint8_t capacity = body[6U];
    const uint8_t lane = body[7U];
    const uint16_t status = fxe::ReadU16LE(body + 8U);
    const bool state = packet.header.sequence == 1U;
    const uint8_t requestedCapacity = state ? STATE_CAPACITY
                                            : REVERSE_OBJECT_CAPACITY;
    const uint16_t requestedBody = state ? STATE_BODY_BYTES
                                         : m_packetMaximum - wire::HEADER_BYTES;
    if (status != 0U || streamToken == 0U ||
        (streamToken & 0x80000000U) == 0U || lane != 1U || capacity == 0U ||
        capacity > requestedCapacity || maximumBody == 0U ||
        maximumBody > requestedBody) {
        return Result::Conflict;
    }
    if (state) {
        if (m_stateStreamToken != 0U) {
            return m_stateStreamToken == streamToken &&
                    m_stateMaximumBody == maximumBody && m_stateCapacity == capacity
                ? Result::Ok : Result::Conflict;
        }
        m_stateStreamToken = streamToken;
        m_stateMaximumBody = maximumBody;
        m_stateCapacity = capacity;
        return RebindRetainedStates() ? Result::Ok : Result::NoCapacity;
    }
    if (m_reverseObjectStreamToken != 0U) {
        return m_reverseObjectStreamToken == streamToken &&
                m_reverseObjectMaximumBody == maximumBody &&
                m_reverseObjectCapacity == capacity
            ? Result::Ok : Result::Conflict;
    }
    m_reverseObjectStreamToken = streamToken;
    m_reverseObjectMaximumBody = maximumBody;
    m_reverseObjectCapacity = capacity;
    m_reverseObjectGrantThrough = capacity;
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleReverseObjectProgress(
    const wire::PacketView& packet) {
    if (m_state != State::Active || m_reverseObjectStreamToken == 0U ||
        packet.header.token != m_reverseObjectStreamToken) {
        return Result::Unexpected;
    }
    const uint32_t confirmed = fxe::ReadU32LE(packet.body.Data());
    const uint32_t grant = fxe::ReadU32LE(packet.body.Data() + 4U);
    const uint16_t receivedAhead =
        fxe::ReadU16LE(packet.body.Data() + 8U);
    if (confirmed < m_reverseObjectConfirmedThrough ||
        packet.header.sequence < m_reverseObjectTerminalThrough ||
        grant < m_reverseObjectGrantThrough ||
        grant - confirmed > m_reverseObjectCapacity ||
        confirmed >= m_reverseObjectNextSequence ||
        packet.header.sequence >= m_reverseObjectNextSequence) {
        return Result::Conflict;
    }
    m_reverseObjectConfirmedThrough = confirmed;
    m_reverseObjectTerminalThrough = packet.header.sequence;
    m_reverseObjectGrantThrough = grant;
    // A delayed receipt can confirm a packet whose repair notify was refused.
    // Retiring its slot must also release the selection, or the empty slot
    // blocks subsequent control replies and forward Object admission progress.
    if (m_selectedOutput == SelectedOutput::ReverseObject &&
        m_selectedReverseObjectOutput < m_reverseObjectCapacity &&
        m_reverseObjectOutputs[m_selectedReverseObjectOutput].sequence <=
            confirmed) {
        m_selectedOutput = SelectedOutput::None;
        m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    }
    for (auto& output : m_reverseObjectOutputs) {
        if (output.size == 0U) continue;
        if (output.sequence <= confirmed) {
            output = {};
            continue;
        }
        bool received = output.sequence <= packet.header.sequence;
        if (!received && output.sequence > packet.header.sequence) {
            const uint32_t offset =
                output.sequence - packet.header.sequence - 1U;
            received = offset < 16U &&
                (receivedAhead & static_cast<uint16_t>(1U << offset)) != 0U;
            // An unreported tail may still be in flight. Only a later
            // retained sequence proves that this missing packet is a hole.
            if (!received && offset < 15U &&
                (receivedAhead >> (offset + 1U)) != 0U) output.pending = true;
        }
    }
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleQuery(
    const wire::PacketView& packet) {
    if (m_state != State::Active) {
        return Result::Unexpected;
    }
    if (packet.header.token == m_stateStreamToken) {
        bool matched = false;
        for (uint8_t index = 0U; index < m_stateCapacity; ++index) {
            auto& output = m_stateOutputs[index];
            if (output.size == 0U || !StateOutputAllowed(output.bytes[1U])) continue;
            if (packet.header.sequence == 0U ||
                output.bytes[1U] == packet.header.sequence ||
                (packet.header.sequence == owner_pairing_session_wire::STATUS_CODE &&
                 output.bytes[1U] >= owner_pairing_session_wire::STATUS_CODE &&
                 output.bytes[1U] <= owner_pairing_session_wire::IDENTITY_HIGH_CODE)) {
                output.pending = true;
                matched = true;
            }
        }
        return matched ? Result::Ok : Result::Unexpected;
    }
    if (packet.header.token == m_objectStreamToken) {
        if (packet.header.sequence == 0U) {
            return PublishObjectProgress() ? Result::Ok : Result::Unexpected;
        }
        if (packet.header.sequence > UINT16_MAX) return Result::Unexpected;
        if (m_requestedObjectReceipt != 0U) {
            return Result::NoCapacity;
        }
        m_requestedObjectReceipt = static_cast<uint16_t>(packet.header.sequence);
        return Result::Ok;
    }
    if (packet.header.token == m_reverseObjectStreamToken) {
        if (packet.header.sequence == 0U) {
            bool matched = false;
            for (auto& output : m_reverseObjectOutputs) {
                if (output.size != 0U && ReverseObjectOutputAllowed(output.bytes[1U])) {
                    output.pending = true;
                    matched = true;
                }
            }
            return matched || m_productAccess ? Result::Ok : Result::NotReady;
        }
        for (auto& output : m_reverseObjectOutputs) {
            if (output.size != 0U && ReverseObjectOutputAllowed(output.bytes[1U]) &&
                output.sequence == packet.header.sequence) {
                output.pending = true;
                return Result::Ok;
            }
        }
        return m_productAccess ? Result::Unexpected : Result::NotReady;
    }
    if (packet.header.token != m_streamToken) return Result::Unexpected;
    if (packet.header.sequence == 0U) {
        // Checkpoint bytes retain the exact domain revision, digest and
        // exception bitmap until superseded. Re-arm the coalesced slot so a
        // lost notification can be recovered without re-running the domain.
        if (m_checkpoint.size != 0U) m_checkpoint.pending = true;
    } else {
        Window::Outcome outcome;
        if (m_orderedWindow.ReadOutcome(m_streamToken, packet.header.sequence,
                                        outcome) &&
            outcome.disposition != Window::Disposition::Applied) {
            (void)PublishOutcome(packet.header.sequence, outcome);
        }
    }
    (void)PublishProgress();
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleConfirm(
    const wire::PacketView& packet) {
    if (m_state != State::Active || packet.header.token != m_streamToken) {
        return Result::Unexpected;
    }
    const auto result = m_orderedWindow.ConfirmThrough(
        m_streamToken, packet.header.sequence);
    if (result != Window::Result::Ok && result != Window::Result::Duplicate) {
        return Result::Unexpected;
    }
    (void)PublishProgress();
    return Result::Ok;
}

ConnectionSession::Result ConnectionSession::HandleClose(
    const wire::PacketView& packet) {
    if (m_state != State::Active) return Result::Unexpected;
    const uint32_t token = packet.header.token;
    const wire::PacketView closed{
        {wire::Kind::Control, uint8_t(wire::Control::Closed), token, 0U}, {}};
    if (token == m_lastClosedToken) {
        return QueueLifecycle(closed) ? Result::Ok : Result::NoCapacity;
    }
    const bool ordered = token == m_streamToken;
    const bool object = token == m_objectStreamToken;
    if (!ordered && !object) return Result::Unexpected;
    if (ordered) {
        Window::Progress progress;
        if (!m_orderedWindow.ReadProgress(token, progress)) return Result::Unexpected;
        if (progress.confirmedThrough != progress.terminalThrough ||
            progress.receivedBits != 0U) {
            (void)PublishProgress();
            return Result::NoCapacity;
        }
    } else if (m_objectPendingSequence != 0U || m_objectStreamBusy ||
               m_objectHeldCredits != 0U) {
        (void)PublishObjectProgress();
        return Result::NoCapacity;
    }
    // Failed notification admission must leave the original stream intact.
    if (!QueueLifecycle(closed)) return Result::NoCapacity;
    m_lastClosedToken = token;
    if (ordered) {
        (void)m_orderedWindow.Retire(token);
        m_streamToken = 0U;
        m_orderedMaximumBody = 0U;
        m_progress = {};
        m_checkpoint = {};
        m_outcome = {};
        if (m_selectedOutput == SelectedOutput::Progress ||
            m_selectedOutput == SelectedOutput::Checkpoint ||
            m_selectedOutput == SelectedOutput::Outcome) {
            m_selectedOutput = SelectedOutput::None;
        }
    } else {
        m_objectStreamToken = 0U;
        m_objectAcceptedThrough = 0U;
        m_objectGrantThrough = 0U;
        m_objectHeldCredits = 0U;
        m_objectAcceptedSize = 0U;
        m_objectCapacity = 0U;
        m_objectMaximumBody = 0U;
        m_requestedObjectReceipt = 0U;
        m_objectProgress = {};
        if (m_selectedOutput == SelectedOutput::ObjectProgress) {
            m_selectedOutput = SelectedOutput::None;
        }
    }
    return Result::Ok;
}

bool ConnectionSession::QueueLifecycle(const wire::PacketView& packet) {
    for (uint8_t offset = 0U; offset < m_lifecycleCount; ++offset) {
        const auto& current =
            m_lifecycle[(m_lifecycleHead + offset) % 2U];
        std::array<uint8_t, INTERACTIVE_PACKET_BYTES> encoded{};
        size_t size = 0U;
        if (wire::Codec::Encode(packet, {encoded.data(), encoded.size()}, size) ==
                wire::Error::None &&
            current.size == size &&
            std::equal(encoded.begin(), encoded.begin() + size,
                       current.bytes.begin())) {
            return true;
        }
    }
    if (m_lifecycleCount >= 2U) return false;
    auto& output = m_lifecycle[(m_lifecycleHead + m_lifecycleCount) % 2U];
    if (!SetOutput(output, packet)) return false;
    ++m_lifecycleCount;
    return true;
}

bool ConnectionSession::QueueStateOpen() {
    if (m_state != State::Active || m_connectionToken == 0U ||
        m_stateStreamToken != 0U) {
        return false;
    }
    std::array<uint8_t, 8U> body{};
    fxe::WriteU16LE(LITCHCRAFT_STATE_TOPIC, body.data());
    body[2U] = 1U;
    body[3U] = STATE_CAPACITY;
    fxe::WriteU16LE(STATE_BODY_BYTES, body.data() + 4U);
    fxe::WriteU16LE(EXPECTED_FEEDBACK_MS, body.data() + 6U);
    const wire::PacketView open{{wire::Kind::Control,
                                 uint8_t(wire::Control::Open),
                                 m_connectionToken, 1U},
                                {body.data(), body.size()}};
    if (!QueueLifecycle(open)) return false;
    m_stateOpenSentAtMs = fxe::Millis();
    return true;
}

bool ConnectionSession::QueueReverseObjectOpen() {
    if (m_state != State::Active || m_connectionToken == 0U ||
        m_reverseObjectStreamToken != 0U) {
        return false;
    }
    std::array<uint8_t, 8U> body{};
    fxe::WriteU16LE(LITCHCRAFT_REVERSE_OBJECT_TOPIC, body.data());
    body[2U] = 1U;
    body[3U] = REVERSE_OBJECT_CAPACITY;
    fxe::WriteU16LE(
        static_cast<uint16_t>(m_packetMaximum - wire::HEADER_BYTES),
        body.data() + 4U);
    fxe::WriteU16LE(EXPECTED_FEEDBACK_MS, body.data() + 6U);
    const wire::PacketView open{{wire::Kind::Control,
                                 uint8_t(wire::Control::Open),
                                 m_connectionToken, 2U},
                                {body.data(), body.size()}};
    if (!QueueLifecycle(open)) return false;
    m_reverseObjectOpenSentAtMs = fxe::Millis();
    return true;
}

bool ConnectionSession::EncodeRetainedState(
    StateOutput& output, const uint8_t code, const uint32_t revision,
    const uint8_t* const body, const size_t size) {
    const uint32_t token = m_stateStreamToken == 0U ? 1U
                                                    : m_stateStreamToken;
    size_t encodedSize = 0U;
    if (wire::Codec::Encode(
            {{wire::Kind::State, code, token, revision}, {body, size}},
            {output.bytes.data(), output.bytes.size()}, encodedSize) !=
            wire::Error::None || encodedSize > UINT16_MAX) {
        return false;
    }
    output.size = static_cast<uint8_t>(encodedSize);
    output.pending = m_stateStreamToken != 0U;
    return true;
}

bool ConnectionSession::RebindRetainedStates() {
    for (uint8_t index = 0U; index < STATE_CAPACITY; ++index) {
        auto& output = m_stateOutputs[index];
        if (output.size == 0U) continue;
        if (index >= m_stateCapacity ||
            output.size - wire::HEADER_BYTES > m_stateMaximumBody) {
            return false;
        }
        fxe::WriteU32LE(m_stateStreamToken, output.bytes.data() + 2U);
        output.pending = true;
    }
    return true;
}

ConnectionSession::StateOutput* ConnectionSession::FindState(
    const uint8_t code) {
    const uint8_t capacity = m_stateStreamToken == 0U ? STATE_CAPACITY
                                                      : m_stateCapacity;
    for (uint8_t index = 0U; index < capacity; ++index) {
        auto& output = m_stateOutputs[index];
        if (output.size != 0U && output.bytes[1U] == code) return &output;
    }
    return nullptr;
}

ConnectionSession::StateOutput* ConnectionSession::FindEmptyState() {
    const uint8_t capacity = m_stateStreamToken == 0U ? STATE_CAPACITY
                                                      : m_stateCapacity;
    for (uint8_t index = 0U; index < capacity; ++index) {
        if (m_stateOutputs[index].size == 0U) return &m_stateOutputs[index];
    }
    return nullptr;
}

bool ConnectionSession::SelectStateOutputWithinBurst() {
    if (m_stateOutputBurst >= PRIORITY_STATE_BURST) return false;
    // Reserve the last State turn for ordinary truth, such as Settings. Whole
    // replay Queries can keep priority keys pending throughout a slow link.
    return (m_stateOutputBurst + 1U == PRIORITY_STATE_BURST &&
            SelectNextStateOutput(StateSelection::Ordinary)) ||
           SelectNextStateOutput(StateSelection::Priority) ||
           SelectNextStateOutput(StateSelection::Ordinary);
}

bool ConnectionSession::SelectNextStateOutput(const StateSelection selection) {
    const bool priorityOnly = selection == StateSelection::Priority;
    const uint8_t next = priorityOnly ? m_nextPriorityStateOutput
                                     : m_nextStateOutput;
    for (uint8_t offset = 0U; offset < m_stateCapacity; ++offset) {
        const uint8_t index = static_cast<uint8_t>(
            (next + offset) % m_stateCapacity);
        const auto& output = m_stateOutputs[index];
        const uint8_t code = output.size >= wire::HEADER_BYTES
            ? output.bytes[1U]
            : 0U;
        const bool priority = code == LITCHCRAFT_STATE_CODE ||
            code == settings_state_wire::LIVE_PALETTE_STATE_CODE ||
            code == RUNTIME_HEALTH_STATE_CODE ||
            (code >= home_state_wire::FIRST_STATE_CODE &&
             code <= home_state_wire::LAST_STATE_CODE) ||
            code == LITCHCRAFT_STORE_EDIT_STATE_CODE ||
            code == LITCHCRAFT_STORE_AUTHORITY_STATE_CODE ||
            (code >= BUTTON_STATE_FIRST_CODE &&
             code <= BUTTON_STATE_LAST_CODE);
        if (output.pending && output.size >= wire::HEADER_BYTES &&
            StateOutputAllowed(code) &&
            priority == priorityOnly) {
            m_selectedStateOutput = index;
            m_selectedStatePriority = priorityOnly;
            return true;
        }
    }
    m_selectedStateOutput = STATE_CAPACITY;
    return false;
}

bool ConnectionSession::SelectNextReverseObjectOutput() {
    m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    for (uint8_t index = 0U; index < m_reverseObjectCapacity; ++index) {
        const auto& output = m_reverseObjectOutputs[index];
        if (!output.pending || !ReverseObjectOutputAllowed(output.bytes[1U])) continue;
        if (m_selectedReverseObjectOutput == REVERSE_OBJECT_CAPACITY ||
            output.sequence < m_reverseObjectOutputs[m_selectedReverseObjectOutput].sequence)
            m_selectedReverseObjectOutput = index;
    }
    return m_selectedReverseObjectOutput != REVERSE_OBJECT_CAPACITY;
}

bool ConnectionSession::SetOutput(Output& output,
                                  const wire::PacketView& packet) {
    size_t size = 0U;
    if (wire::Codec::Encode(packet, {output.bytes.data(), output.bytes.size()},
                            size) != wire::Error::None ||
        size > UINT8_MAX) {
        return false;
    }
    output.size = static_cast<uint8_t>(size);
    output.pending = true;
    return true;
}

bool ConnectionSession::PublishProgress() {
    Window::Progress progress;
    if (!m_orderedWindow.ReadProgress(m_streamToken, progress)) return false;
    std::array<uint8_t, 10U> body{};
    fxe::WriteU32LE(progress.confirmedThrough, body.data());
    fxe::WriteU32LE(progress.grantThrough, body.data() + 4U);
    const uint32_t shift = progress.terminalThrough - progress.confirmedThrough;
    const uint16_t receivedAhead = static_cast<uint16_t>(
        shift >= 64U ? 0U : (progress.receivedBits >> shift));
    fxe::WriteU16LE(receivedAhead, body.data() + 8U);
    return SetOutput(m_progress,
                     {{wire::Kind::Control,
                       uint8_t(wire::Control::Progress), m_streamToken,
                       progress.terminalThrough},
                      {body.data(), body.size()}});
}

bool ConnectionSession::PublishObjectProgress() {
    if (m_objectStreamToken == 0U || m_objectCapacity == 0U) return false;
    std::array<uint8_t, 10U> body{};
    fxe::WriteU32LE(m_objectAcceptedThrough, body.data());
    fxe::WriteU32LE(m_objectGrantThrough, body.data() + 4U);
    fxe::WriteU16LE(0U, body.data() + 8U);
    return SetOutput(m_objectProgress,
                     {{wire::Kind::Control,
                       uint8_t(wire::Control::Progress), m_objectStreamToken,
                       m_objectAcceptedThrough},
                      {body.data(), body.size()}});
}

bool ConnectionSession::PublishCheckpoint(const uint32_t domainRevision,
                                          const uint32_t domainDigest) {
    Window::Progress progress;
    if (!m_orderedWindow.ReadProgress(m_streamToken, progress)) return false;
    uint16_t exceptions = 0U;
    for (uint8_t bit = 0U; bit < ORDERED_CAPACITY &&
                           progress.terminalThrough > bit;
         ++bit) {
        Window::Outcome outcome;
        const uint32_t sequence = progress.terminalThrough - bit;
        if (m_orderedWindow.ReadOutcome(m_streamToken, sequence, outcome) &&
            outcome.disposition != Window::Disposition::Applied) {
            exceptions |= static_cast<uint16_t>(1U << bit);
        }
    }
    std::array<uint8_t, 10U> body{};
    fxe::WriteU32LE(m_productAccess ? domainRevision : 0U, body.data());
    fxe::WriteU32LE(m_productAccess ? domainDigest : 0U, body.data() + 4U);
    fxe::WriteU16LE(exceptions, body.data() + 8U);
    return SetOutput(m_checkpoint,
                     {{wire::Kind::Control,
                       uint8_t(wire::Control::Checkpoint), m_streamToken,
                       progress.terminalThrough},
                      {body.data(), body.size()}});
}

bool ConnectionSession::PublishOutcome(const uint32_t sequence,
                                       const Window::Outcome& outcome) {
    std::array<uint8_t, 3U> body{};
    body[0U] = static_cast<uint8_t>(outcome.disposition);
    fxe::WriteU16LE(outcome.reason, body.data() + 1U);
    return SetOutput(m_outcome,
                     {{wire::Kind::Control,
                       uint8_t(wire::Control::Outcome), m_streamToken,
                       sequence},
                      {body.data(), body.size()}});
}

bool ConnectionSession::CopyOutput(const Output& output, uint8_t* const out,
                                   const size_t capacity,
                                   size_t& outSize) const {
    if (!output.pending || capacity < output.size) return false;
    std::copy_n(output.bytes.begin(), output.size, out);
    outSize = output.size;
    return true;
}

bool ConnectionSession::CopyReverseObject(uint8_t* const out,
                                          const size_t capacity,
                                          size_t& outSize) const {
    if (m_selectedReverseObjectOutput >= m_reverseObjectCapacity) {
        return false;
    }
    const auto& output =
        m_reverseObjectOutputs[m_selectedReverseObjectOutput];
    if (!output.pending || capacity < output.size ||
        !ReverseObjectOutputAllowed(output.bytes[1U])) return false;
    std::copy_n(output.bytes.begin(), output.size, out);
    outSize = output.size;
    return true;
}

bool ConnectionSession::CopyStateOutput(uint8_t* const out,
                                        const size_t capacity,
                                        size_t& outSize) const {
    if (m_selectedStateOutput >= STATE_CAPACITY) return false;
    const auto& output = m_stateOutputs[m_selectedStateOutput];
    if (!output.pending || capacity < output.size ||
        !StateOutputAllowed(output.bytes[1U])) return false;
    std::copy_n(output.bytes.begin(), output.size, out);
    outSize = output.size;
    return true;
}

} // namespace modules
