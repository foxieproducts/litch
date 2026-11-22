#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/ordered_receive_window.hpp"
#include "modules/connection_session_codec.hpp"

namespace modules {

// Board-side owner for one negotiated connection and one initial interactive
// ordered stream. Domain mutation remains outside this class.
class ConnectionSession final {
  public:
    static constexpr uint8_t ORDERED_CAPACITY = 8U;
    static constexpr size_t INTERACTIVE_PACKET_BYTES = 20U;
    static constexpr size_t STATE_BODY_BYTES =
        INTERACTIVE_PACKET_BYTES - connection_session_codec::HEADER_BYTES;
    static constexpr size_t OBJECT_PACKET_BYTES = 512U;
    // Retain the existing callback FIFO byte budget as packets grow. Every
    // leased record must also fit the shared artwork/OTA byte arena.
    static constexpr uint8_t OBJECT_CAPACITY = 26U;
    static constexpr size_t REVERSE_OBJECT_PACKET_BYTES = 512U;
    static constexpr uint8_t STATE_CAPACITY = 33U;
    static constexpr uint16_t FEATURE_ORDERED = 0x0001U;
    static constexpr uint16_t FEATURE_STATE = 0x0002U;
    static constexpr uint16_t FEATURE_OBJECT = 0x0008U;
    static constexpr uint16_t FEATURE_REVERSE_OBJECT = 0x0010U;
    static constexpr uint16_t FEATURE_STORE_GUARD_EPOCH = 0x0020U;
    static constexpr uint16_t FEATURE_ORDERED_MODE = 0x0040U;
    static constexpr uint8_t LITCHCRAFT_MODE_ORDERED_OPCODE = 1U;
    static constexpr uint16_t LITCHCRAFT_ORDERED_TOPIC = 1U;
    static constexpr uint16_t LITCHCRAFT_OBJECT_TOPIC = 1U;
    static constexpr uint16_t LITCHCRAFT_REVERSE_OBJECT_TOPIC = 2U;
    static constexpr uint8_t REVERSE_OBJECT_CAPACITY = 8U;
    static constexpr uint8_t SUPPORT_OBJECT_OPCODE = 0x44U;
    static constexpr uint8_t PUBLIC_SUPPORT_OBJECT_OPCODE = 0x45U;
    static constexpr uint8_t GAME_HISTORY_OBJECT_OPCODE = 0x30U;
    static constexpr uint8_t GAME_HISTORY_RECEIPT_CODE = 0x30U;
    static constexpr uint8_t OTA_DATA_OBJECT_OPCODE = 0x31U;
    static constexpr uint8_t OTA_CONTROL_OBJECT_OPCODE = 0x32U;
    static constexpr uint8_t OTA_STATUS_RECEIPT_CODE = 0x32U;
    static constexpr uint8_t LITCHCRAFT_OBJECT_OPCODE = 0x40U;
    static constexpr uint8_t LITCHCRAFT_RECEIPT_CODE = 0x40U;
    static constexpr uint16_t LITCHCRAFT_STATE_TOPIC = 1U;
    static constexpr uint8_t LITCHCRAFT_STATE_CODE = 1U;
    static constexpr uint8_t RUNTIME_HEALTH_STATE_CODE = 2U;
    static constexpr uint8_t BOARD_HEALTH_CONTEXT_STATE_CODE = 3U;
    static constexpr uint8_t LITCHCRAFT_STORE_EDIT_STATE_CODE = 22U;
    static constexpr uint8_t BUTTON_STATE_FIRST_CODE = 23U;
    static constexpr uint8_t BUTTON_STATE_LAST_CODE = 36U;
    static constexpr uint8_t LITCHCRAFT_STORE_AUTHORITY_STATE_CODE = 37U;
    static constexpr uint8_t BOARD_STORAGE_RECOVERY_STATE_CODE = 38U;
    static constexpr uint8_t CURRENT_GAME_LINE_STATE_CODE = 45U;
    static constexpr uint16_t REFUSED_UNSUPPORTED_TOPIC = 1U;
    static constexpr uint16_t REFUSED_STREAM_BUSY = 2U;
    static constexpr uint32_t NEGOTIATION_TIMEOUT_MS = 10000U;
    static constexpr uint16_t EXPECTED_FEEDBACK_MS = 40U;
    static constexpr uint16_t REVERSE_OBJECT_RECOVERY_MS =
        EXPECTED_FEEDBACK_MS * 3U + 1U;
    static constexpr uint16_t REVERSE_OBJECT_MAX_RECOVERY_MS = 968U;
    static constexpr uint8_t PRIORITY_STATE_BURST = 3U;

    enum class Result : uint8_t {
        Ok,
        StaleGeneration,
        Inactive,
        Malformed,
        Unexpected,
        Conflict,
        NoCapacity,
        NotReady,
    };

    struct OrderedCommand final {
        uint16_t topic = 0U;
        uint32_t streamToken = 0U;
        uint32_t sequence = 0U;
        uint8_t opcode = 0U;
        fxe::ConstByteSpan body{};
    };

    struct ObjectCommand final {
        uint16_t topic = 0U;
        uint32_t streamToken = 0U;
        uint32_t sequence = 0U;
        uint8_t opcode = 0U;
        fxe::ConstByteSpan body{};
        uint32_t ingressOrdinal = 0U;
    };

    bool Begin(uint32_t generation, uint64_t bootNonce,
               uint16_t packetMaximum, uint16_t orderedTopic);
    void End(uint32_t generation);
    void Update(uint32_t generation);

    Result Ingress(uint32_t generation, const uint8_t* bytes, size_t size,
                   ObjectCommand* object = nullptr, uint32_t ingressOrdinal = 0U);
    uint16_t TakeRequestedObjectReceipt();
    Result AcceptObject(uint32_t streamToken, uint32_t sequence,
                        bool retainCredit = false);
    void RejectObject(uint32_t streamToken, uint32_t sequence);
    bool PeekPendingObject(ObjectCommand& object) const;
    bool HasPendingObject() const { return m_objectPendingSequence != 0U; }
    void ReturnObjectCredit(uint32_t streamToken);
    // Accepted fragment prefixes remain owned by the existing domain adapter.
    void SetObjectStreamBusy(bool busy) { m_objectStreamBusy = busy; }
    // Revocation also fences an output already selected after a failed notify.
    // Negotiation and Ordered receipts remain live for pairing/cancellation;
    // only pairing status and identity State keys are public while restricted.
    void SetProductAccess(bool allowed);
    bool PublishState(uint8_t code, uint32_t revision, const uint8_t* body,
                      size_t size);
    bool PublishReverseObject(uint8_t code, uint32_t request,
                              const uint8_t* body, size_t size, bool bulk = false);
    bool HasBulkReverseObject() const;
    [[nodiscard]] uint32_t LastPublishedReverseObjectSequence() const {
        return m_reverseObjectNextSequence - 1U;
    }
    // FIFO-copy admission is handed off only after native notify accepts its
    // retained Progress. Domain work can block without consuming this output.
    bool HasPendingObjectAdmission() const { return m_objectProgress.pending; }
    bool PeekOutput(uint32_t generation, uint8_t* out, size_t capacity,
                    size_t& outSize);
    void AcceptOutput(uint32_t generation);

    Result BeginNext(OrderedCommand& out);
    bool ReadApplying(OrderedCommand& out) const;
    Result Complete(uint32_t streamToken, uint32_t sequence,
                    fxe::OrderedReceiveWindow<ORDERED_CAPACITY,
                                              INTERACTIVE_PACKET_BYTES>::Outcome outcome,
                    uint32_t domainRevision, uint32_t domainDigest);

    bool IsActivated() const { return m_state == State::Active; }
    uint32_t Generation() const { return m_generation; }
    uint32_t ConnectionToken() const { return m_connectionToken; }
    uint32_t StreamToken() const { return m_streamToken; }
    uint32_t StateStreamToken() const { return m_stateStreamToken; }
    uint32_t ObjectStreamToken() const { return m_objectStreamToken; }
    uint32_t ReverseObjectStreamToken() const {
        return m_reverseObjectStreamToken;
    }
    uint16_t PacketMaximum() const { return m_packetMaximum; }
    uint16_t ObjectMaximumBody() const { return m_objectMaximumBody; }
    uint16_t ReverseObjectMaximumBody() const { return m_reverseObjectMaximumBody; }
    uint32_t SuccessfulReverseObjectReplays() const { return m_successfulReverseObjectReplays; }
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    struct LiveObjectDiagnostics {
        uint32_t generation, accepted, grant, pending, reverseConfirmed, reverseGrant;
    };
    // Cooperative-loop owner only; never a callback or worker-thread read.
    LiveObjectDiagnostics GetLiveObjectDiagnostics() const {
        return {m_generation, m_objectAcceptedThrough, m_objectGrantThrough,
                m_objectPendingSequence, m_reverseObjectConfirmedThrough,
                m_reverseObjectGrantThrough};
    }
#endif

  private:
    friend struct ConnectionSessionCreditTestPeer;
    using Window = fxe::OrderedReceiveWindow<ORDERED_CAPACITY,
                                             INTERACTIVE_PACKET_BYTES>;
    enum class State : uint8_t { Inactive, AwaitHello, AwaitReady, Active };
    enum class SelectedOutput : uint8_t {
        None,
        Lifecycle,
        Outcome,
        Progress,
        ObjectProgress,
        ReverseObject,
        Checkpoint,
        State,
    };
    struct Output final {
        std::array<uint8_t, INTERACTIVE_PACKET_BYTES> bytes{};
        uint8_t size = 0U;
        bool pending = false;
    };
    struct ReverseObjectOutput final {
        std::array<uint8_t, REVERSE_OBJECT_PACKET_BYTES> bytes{};
        uint16_t size = 0U;
        uint32_t sequence = 0U;
        uint32_t lastSentAtMs = 0U;
        uint8_t sendAttempts = 0U;
        bool bulk = false;
        bool pending = false;
    };
    struct StateOutput final {
        std::array<uint8_t, INTERACTIVE_PACKET_BYTES> bytes{};
        uint8_t size = 0U;
        bool pending = false;
    };
    static_assert(sizeof(StateOutput) == 22U, "Retained State key RAM budget");
    enum class StateSelection : uint8_t { Priority, Ordinary };

    void ResetNegotiation();
    uint32_t AllocateToken();
    Result HandleControl(const connection_session_codec::PacketView& packet,
                         fxe::ConstByteSpan encoded);
    Result HandleHello(const connection_session_codec::PacketView& packet,
                       fxe::ConstByteSpan encoded);
    Result HandleReady(const connection_session_codec::PacketView& packet);
    Result HandleOpen(const connection_session_codec::PacketView& packet,
                      fxe::ConstByteSpan encoded);
    Result HandleOpened(const connection_session_codec::PacketView& packet);
    Result HandleQuery(const connection_session_codec::PacketView& packet);
    Result HandleConfirm(const connection_session_codec::PacketView& packet);
    Result HandleClose(const connection_session_codec::PacketView& packet);
    Result HandleReverseObjectProgress(
        const connection_session_codec::PacketView& packet);
    bool QueueLifecycle(const connection_session_codec::PacketView& packet);
    bool QueueStateOpen();
    bool QueueReverseObjectOpen();
    bool EncodeRetainedState(StateOutput& output, uint8_t code,
                             uint32_t revision, const uint8_t* body,
                             size_t size);
    bool RebindRetainedStates();
    StateOutput* FindState(uint8_t code);
    StateOutput* FindEmptyState();
    bool SelectStateOutputWithinBurst();
    bool SelectNextStateOutput(StateSelection selection);
    bool SelectNextReverseObjectOutput();
    bool ReverseObjectOutputAllowed(uint8_t code) const;
    bool StateOutputAllowed(uint8_t code) const;
    bool SetOutput(Output& output,
                   const connection_session_codec::PacketView& packet);
    bool PublishProgress();
    bool PublishObjectProgress();
    bool PublishCheckpoint(uint32_t domainRevision, uint32_t domainDigest);
    bool PublishOutcome(uint32_t sequence, const Window::Outcome& outcome);
    bool CopyOutput(const Output& output, uint8_t* out, size_t capacity,
                    size_t& outSize) const;
    bool CopyReverseObject(uint8_t* out, size_t capacity,
                           size_t& outSize) const;
    bool CopyStateOutput(uint8_t* out, size_t capacity,
                         size_t& outSize) const;

    State m_state = State::Inactive;
    uint32_t m_generation = 0U;
    uint64_t m_bootNonce = 0U;
    uint64_t m_clientNonce = 0U;
    uint16_t m_packetMaximum = 0U;
    uint16_t m_orderedTopic = 0U;
    uint32_t m_startedAtMs = 0U;
    uint32_t m_nextToken = 1U;
    uint32_t m_connectionToken = 0U;
    uint32_t m_streamToken = 0U;
    uint16_t m_orderedMaximumBody = 0U;
    uint32_t m_stateStreamToken = 0U;
    uint8_t m_stateMaximumBody = 0U;
    uint8_t m_stateCapacity = 0U;
    uint32_t m_objectStreamToken = 0U;
    bool m_objectStreamBusy = false;
    bool m_productAccess = true;
    uint32_t m_lastClosedToken = 0U;
    uint32_t m_objectAcceptedThrough = 0U;
    uint32_t m_objectGrantThrough = 0U;
    uint32_t m_objectPendingSequence = 0U;
    uint32_t m_objectPendingOrdinal = 0U;
    uint16_t m_requestedObjectReceipt = 0U;
    uint8_t m_objectCapacity = 0U;
    uint16_t m_objectMaximumBody = 0U;
    uint16_t m_objectAcceptedSize = 0U;
    uint16_t m_objectPendingSize = 0U;
    uint8_t m_objectHeldCredits = 0U;
    uint32_t m_stateOpenSentAtMs = 0U;
    uint32_t m_reverseObjectOpenSentAtMs = 0U;
    uint32_t m_reverseObjectStreamToken = 0U;
    uint32_t m_reverseObjectConfirmedThrough = 0U;
    uint32_t m_reverseObjectTerminalThrough = 0U;
    uint32_t m_reverseObjectGrantThrough = 0U;
    uint32_t m_reverseObjectNextSequence = 1U;
    uint32_t m_successfulReverseObjectReplays = 0U;
    uint16_t m_reverseObjectMaximumBody = 0U;
    uint8_t m_reverseObjectCapacity = 0U;
    uint32_t m_lastOpenRequest = 0U;
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES> m_hello{};
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES> m_welcome{};
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES> m_identity{};
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES> m_lastOpen{};
    std::array<uint8_t, INTERACTIVE_PACKET_BYTES> m_lastOpened{};
    std::array<uint8_t, OBJECT_PACKET_BYTES> m_objectAccepted{};
    std::array<uint8_t, OBJECT_PACKET_BYTES> m_objectPending{};
    uint8_t m_lastOpenSize = 0U;
    uint8_t m_lastOpenedSize = 0U;
    Output m_lifecycle[2]{};
    uint8_t m_lifecycleHead = 0U;
    uint8_t m_lifecycleCount = 0U;
    Output m_progress{};
    Output m_objectProgress{};
    std::array<ReverseObjectOutput, REVERSE_OBJECT_CAPACITY>
        m_reverseObjectOutputs{};
    uint8_t m_selectedReverseObjectOutput = REVERSE_OBJECT_CAPACITY;
    Output m_checkpoint{};
    Output m_outcome{};
    std::array<StateOutput, STATE_CAPACITY> m_stateOutputs{};
    uint8_t m_nextStateOutput = 0U;
    uint8_t m_nextPriorityStateOutput = 0U;
    uint8_t m_selectedStateOutput = STATE_CAPACITY;
    uint8_t m_stateOutputBurst = 0U;
    bool m_selectedStatePriority = false;
    SelectedOutput m_selectedOutput = SelectedOutput::None;
    Window m_orderedWindow{};
};

} // namespace modules
