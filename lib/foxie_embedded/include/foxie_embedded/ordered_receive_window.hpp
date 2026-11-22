#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/span.hpp"

namespace fxe {

// Single-owner semantic admission and outcome retention; not radio credits.
// The retained span must include every semantic field (kind/opcode as well as
// body), e.g. the complete validated wire packet. A body-only comparison cannot
// detect an altered opcode under the same sequence identity.
template <size_t CAPACITY, size_t MAX_PAYLOAD_BYTES>
class OrderedReceiveWindow final {
  public:
    static_assert(CAPACITY > 0U && CAPACITY <= 64U, "Window bitmap holds 1..64 slots");
    static_assert(MAX_PAYLOAD_BYTES > 0U && MAX_PAYLOAD_BYTES <= UINT16_MAX,
                  "Payload length must fit uint16_t");
    static constexpr uint32_t LAST_SEQUENCE = UINT32_MAX - 1U;

    enum class Result : uint8_t {
        Ok, StaleStream, InvalidSequence, InvalidPayload, OutsideWindow,
        Duplicate, ChangedDuplicate, AlreadyConfirmed, NotReady,
        AlreadyApplying, InvalidCompletion, InvalidReceipt, SequenceExhausted,
    };
    enum class Disposition : uint8_t { Applied, Rejected, Cancelled };
    struct Outcome {
        Disposition disposition = Disposition::Applied;
        uint16_t reason = 0U;
    };
    struct Command {
        uint32_t streamToken = 0U;
        uint32_t sequence = 0U;
        ConstByteSpan payload;
    };
    struct Progress {
        uint32_t terminalThrough = 0U;
        uint32_t confirmedThrough = 0U;
        uint32_t grantThrough = 0U;
        uint32_t nextExpected = 1U;
        uint64_t receivedBits = 0U;
        uint64_t missingBits = 0U;
    };

    // Caller guarantees token freshness across all prior streams/boots. A
    // reused older token cannot be detected with a bounded local token ledger.
    bool Start(const uint32_t freshToken, const size_t capacity = CAPACITY) {
        if (freshToken == 0U || freshToken == m_token ||
            capacity == 0U || capacity > CAPACITY) return false;
        m_slots = {};
        m_token = freshToken;
        m_terminal = m_confirmed = 0U;
        m_capacity = static_cast<uint8_t>(capacity);
        m_active = true;
        return true;
    }
    bool Retire(const uint32_t token) {
        if (!Owns(token)) return false;
        m_active = false;
        return true;
    }

    static constexpr uint32_t GrantThroughFor(const uint32_t confirmedThrough,
                                             const size_t capacity = CAPACITY) {
        const uint64_t proposed = uint64_t(confirmedThrough) + capacity;
        return proposed > LAST_SEQUENCE ? LAST_SEQUENCE : uint32_t(proposed);
    }

    Result Ingress(const uint32_t token, const uint32_t sequence,
                   const ConstByteSpan payload) {
        if (!Owns(token)) return Result::StaleStream;
        if (sequence == 0U) return Result::InvalidSequence;
        if (sequence > LAST_SEQUENCE) return Result::SequenceExhausted;
        if (payload.Size() > MAX_PAYLOAD_BYTES ||
            (payload.Size() != 0U && payload.Data() == nullptr)) return Result::InvalidPayload;
        if (sequence <= m_confirmed) return Result::AlreadyConfirmed;
        if (sequence > GrantThroughFor(m_confirmed, m_capacity)) return Result::OutsideWindow;
        auto& slot = SlotFor(sequence);
        if (slot.state != State::Missing) {
            if (slot.size != payload.Size()) return Result::ChangedDuplicate;
            for (size_t i = 0U; i < slot.size; ++i) {
                if (slot.payload[i] != payload[i]) return Result::ChangedDuplicate;
            }
            return Result::Duplicate;
        }
        for (size_t i = 0U; i < payload.Size(); ++i) slot.payload[i] = payload[i];
        slot.size = static_cast<uint16_t>(payload.Size());
        slot.state = State::Received;
        return Result::Ok;
    }

    // The returned payload is borrowed until ConfirmThrough, Retire or Start.
    // Only this transition exposes a command; polling while work is pending
    // cannot dispatch it a second time.
    Result BeginNext(const uint32_t token, Command& out) {
        out = {};
        if (!Owns(token)) return Result::StaleStream;
        if (m_terminal == LAST_SEQUENCE) return Result::SequenceExhausted;
        const uint32_t next = m_terminal + 1U;
        if (next > GrantThroughFor(m_confirmed, m_capacity)) return Result::NotReady;
        auto& slot = SlotFor(next);
        if (slot.state == State::Missing) return Result::NotReady;
        if (slot.state == State::Applying) return Result::AlreadyApplying;
        slot.state = State::Applying;
        out = {m_token, next, ConstByteSpan(slot.payload.data(), slot.size)};
        return Result::Ok;
    }

    Result Complete(const uint32_t token, const uint32_t sequence,
                    const Outcome outcome) {
        if (!Owns(token)) return Result::StaleStream;
        if (m_terminal == LAST_SEQUENCE || sequence != m_terminal + 1U ||
            sequence > GrantThroughFor(m_confirmed, m_capacity) ||
            (outcome.disposition != Disposition::Applied &&
             outcome.disposition != Disposition::Rejected &&
             outcome.disposition != Disposition::Cancelled) ||
            ((outcome.disposition == Disposition::Applied) != (outcome.reason == 0U))) {
            return Result::InvalidCompletion;
        }
        auto& slot = SlotFor(sequence);
        if (slot.state != State::Applying) return Result::InvalidCompletion;
        slot.state = State::Terminal;
        slot.disposition = outcome.disposition;
        slot.reason = outcome.reason;
        m_terminal = sequence;
        return Result::Ok;
    }

    // Re-observe pending work without granting another domain invocation.
    bool ReadApplying(const uint32_t token, Command& out) const {
        out = {};
        if (!Owns(token) || m_terminal == LAST_SEQUENCE) return false;
        const uint32_t next = m_terminal + 1U;
        if (next > GrantThroughFor(m_confirmed, m_capacity)) return false;
        const auto& slot = SlotFor(next);
        if (slot.state != State::Applying) return false;
        out = {m_token, next, ConstByteSpan(slot.payload.data(), slot.size)};
        return true;
    }

    Result ConfirmThrough(const uint32_t token, const uint32_t sequence) {
        if (!Owns(token)) return Result::StaleStream;
        if (sequence > m_terminal) return Result::InvalidReceipt;
        if (sequence <= m_confirmed) return Result::Duplicate;
        for (uint32_t next = m_confirmed + 1U; next <= sequence; ++next) {
            SlotFor(next).state = State::Missing;
        }
        m_confirmed = sequence;
        return Result::Ok;
    }

    bool ReadOutcome(const uint32_t token, const uint32_t sequence, Outcome& out) const {
        out = {};
        if (!Owns(token) || sequence <= m_confirmed || sequence > m_terminal) return false;
        const auto& slot = SlotFor(sequence);
        out = {slot.disposition, slot.reason};
        return true;
    }

    bool ReadProgress(const uint32_t token, Progress& out) const {
        out = {};
        if (!Owns(token)) return false;
        out.terminalThrough = m_terminal;
        out.confirmedThrough = m_confirmed;
        out.grantThrough = GrantThroughFor(m_confirmed, m_capacity);
        out.nextExpected = m_terminal + 1U; // UINT32_MAX means rotate; never wraps.
        for (size_t bit = 0U; bit < out.grantThrough - m_confirmed; ++bit) {
            const auto& slot = SlotFor(m_confirmed + uint32_t(bit) + 1U);
            (slot.state == State::Missing ? out.missingBits : out.receivedBits) |= UINT64_C(1) << bit;
        }
        return true;
    }

  private:
    enum class State : uint8_t { Missing, Received, Applying, Terminal };
    struct Slot {
        std::array<uint8_t, MAX_PAYLOAD_BYTES> payload{};
        uint16_t size = 0U;
        State state = State::Missing;
        Disposition disposition = Disposition::Applied;
        uint16_t reason = 0U;
    };
    bool Owns(const uint32_t token) const { return m_active && token != 0U && token == m_token; }
    Slot& SlotFor(const uint32_t sequence) { return m_slots[(sequence - 1U) % CAPACITY]; }
    const Slot& SlotFor(const uint32_t sequence) const { return m_slots[(sequence - 1U) % CAPACITY]; }

    std::array<Slot, CAPACITY> m_slots{};
    uint32_t m_token = 0U;
    uint32_t m_terminal = 0U;
    uint32_t m_confirmed = 0U;
    uint8_t m_capacity = CAPACITY;
    bool m_active = false;
};

} // namespace fxe
