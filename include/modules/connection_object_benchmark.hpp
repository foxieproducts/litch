#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"

namespace modules {

// Diagnostic payload state only. The existing ConnectionSession owns all
// packets, credits, admission, ordering, replay, cancellation and scheduling.
class ConnectionObjectBenchmark final {
  public:
    static constexpr uint8_t REVERSE_CODE = 0x33U;
    static constexpr uint8_t DATA_TAG = 0x11U;
    static constexpr uint8_t END_TAG = 0x12U;
    static constexpr uint32_t MAX_BYTES = 300000U;
    static constexpr size_t DATA_HEADER_BYTES = 3U;
    static constexpr size_t END_BYTES = 10U;
    enum class Direction : uint8_t { None, Forward, Reverse };

    struct Lifetime final {
        uint32_t epoch = 0U;
        uint32_t connection = 0U;
        uint32_t forward = 0U;
        uint32_t reverse = 0U;
    };

    static uint8_t Pattern(const uint32_t offset, const uint16_t seed) {
        return static_cast<uint8_t>(offset * 33U + seed + (offset >> 8U));
    }

    bool Begin(const Lifetime lifetime, const Direction direction,
               const uint16_t run, const uint32_t total, const uint16_t seed) {
        if (m_direction != Direction::None || run == 0U || run <= m_lastRun ||
            total == 0U || total > MAX_BYTES || direction == Direction::None ||
            lifetime.epoch == 0U || lifetime.connection == 0U ||
            lifetime.forward == 0U || lifetime.reverse == 0U) return false;
        m_lifetime = lifetime;
        m_direction = direction;
        m_run = run;
        m_lastRun = run;
        m_total = total;
        m_seed = seed;
        m_cursor = 0U;
        m_crc = 0U;
        m_pending = 0U;
        m_failed = false;
        m_finished = false;
        return true;
    }

    bool Owns(const Lifetime current) const {
        return current.epoch == m_lifetime.epoch &&
            current.connection == m_lifetime.connection &&
            current.forward == m_lifetime.forward &&
            current.reverse == m_lifetime.reverse;
    }

    void Cancel() {
        m_direction = Direction::None;
        m_pending = 0U;
        m_finished = false;
    }

    // Only connection/session retirement permits clearing the last run.
    void EndLifetime() { *this = {}; }

    bool ConsumeForward(const Lifetime current, const uint8_t* const bytes,
                        const size_t size) {
        if (!Owns(current) || m_direction != Direction::Forward || m_failed ||
            bytes == nullptr || size == 0U || size > m_total - m_cursor) {
            m_failed = true;
            return false;
        }
        for (size_t index = 0U; index < size; ++index) {
            if (bytes[index] != Pattern(m_cursor + index, m_seed)) {
                m_failed = true;
                return false;
            }
        }
        m_crc = fxe::Crc32(bytes, size, ~m_crc);
        m_cursor += size;
        return true;
    }

    bool FinishForward(const Lifetime current, const uint32_t expectedCrc) {
        if (!Owns(current) || m_direction != Direction::Forward || m_failed ||
            m_cursor != m_total || m_crc != expectedCrc) return false;
        m_finished = true;
        m_direction = Direction::None;
        return true;
    }

    // The caller borrows its existing notification scratch only until publish.
    // Peek is deterministic; failed native admission does not advance payload.
    bool PeekReverse(const Lifetime current, uint8_t* const out,
                     const size_t maximumBody, size_t& size) {
        size = 0U;
        m_pending = 0U;
        if (!Owns(current) || m_direction != Direction::Reverse || m_failed ||
            out == nullptr || maximumBody < END_BYTES || maximumBody > 502U) return false;
        fxe::WriteU16LE(m_run, out + 1U);
        if (m_cursor == m_total) {
            out[0U] = END_TAG;
            out[3U] = static_cast<uint8_t>(m_total);
            out[4U] = static_cast<uint8_t>(m_total >> 8U);
            out[5U] = static_cast<uint8_t>(m_total >> 16U);
            fxe::WriteU32LE(m_crc, out + 6U);
            size = END_BYTES;
            m_pending = END_BYTES;
            return true;
        }
        out[0U] = DATA_TAG;
        const size_t count = std::min<size_t>(
            maximumBody - DATA_HEADER_BYTES, m_total - m_cursor);
        for (size_t index = 0U; index < count; ++index)
            out[DATA_HEADER_BYTES + index] = Pattern(m_cursor + index, m_seed);
        size = DATA_HEADER_BYTES + count;
        m_pending = size;
        return true;
    }

    // Called exactly once after PublishReverseObject copies the borrowed bytes.
    // Later notify(false), retries and cumulative grants stay Session-owned.
    bool AcceptReverse(const Lifetime current, const uint8_t* const bytes,
                       const size_t size) {
        if (!Owns(current) || m_direction != Direction::Reverse ||
            bytes == nullptr || size != m_pending || m_pending == 0U) return false;
        m_pending = 0U;
        if (bytes[0U] == END_TAG && m_cursor == m_total && size == END_BYTES) {
            m_finished = true;
            m_direction = Direction::None;
            return true;
        }
        if (bytes[0U] != DATA_TAG || size <= DATA_HEADER_BYTES) return false;
        const size_t count = size - DATA_HEADER_BYTES;
        m_crc = fxe::Crc32(bytes + DATA_HEADER_BYTES, count, ~m_crc);
        m_cursor += count;
        return true;
    }

    bool IsActive() const { return m_direction != Direction::None; }
    bool IsForward() const { return m_direction == Direction::Forward; }
    bool IsReverse() const { return m_direction == Direction::Reverse; }
    bool Failed() const { return m_failed; }
    bool Finished() const { return m_finished; }
    uint32_t Count() const { return m_cursor; }
    uint32_t Crc() const { return m_crc; }
    uint16_t Run() const { return m_run; }

  private:
    Lifetime m_lifetime{};
    Direction m_direction = Direction::None;
    uint16_t m_run = 0U;
    uint16_t m_lastRun = 0U;
    uint16_t m_seed = 0U;
    uint32_t m_total = 0U;
    uint32_t m_cursor = 0U;
    uint32_t m_crc = 0U;
    size_t m_pending = 0U;
    bool m_failed = false;
    bool m_finished = false;
};

static_assert(sizeof(ConnectionObjectBenchmark) <= 64U,
              "Diagnostic state must not retain payload or a transport queue");
} // namespace modules
