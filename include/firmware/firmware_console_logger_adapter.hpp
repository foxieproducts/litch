#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/fixed_string.hpp"

namespace firmware_internal {

// Logger callbacks are serialized by fxe::logger. This single-producer,
// single-consumer handoff keeps worker tasks out of Console and its transports.
class FirmwareConsoleLoggerAdapter final {
  public:
    static constexpr size_t LINE_CAPACITY = 256U;
    static constexpr size_t QUEUED_LINE_CAPACITY = 8U;
    static constexpr size_t MAX_DRAIN_LINES_PER_UPDATE = 4U;

    bool Enqueue(const char* line) {
        if (line == nullptr) {
            return false;
        }

        constexpr size_t FRAGMENT_CAPACITY = LINE_CAPACITY - 1U;
        std::string_view remaining(line);
        const size_t required = remaining.empty()
            ? 1U : 1U + (remaining.size() - 1U) / FRAGMENT_CAPACITY;
        if (required > QUEUED_LINE_CAPACITY - Size()) {
            m_pendingDroppedLines.fetch_add(1U, std::memory_order_relaxed);
            m_totalDroppedLines.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        // Publish all fragments together. A busy queue cannot expose a
        // partial JSON record to the console transport.
        size_t tail = m_tail.load(std::memory_order_relaxed);
        for (size_t fragment = 0U; fragment < required; ++fragment) {
            const auto part = remaining.substr(0U, FRAGMENT_CAPACITY);
            (void)m_lines[tail].Assign(part);
            remaining.remove_prefix(part.size());
            m_lineContinues[tail] = !remaining.empty();
            tail = Increment(tail);
        }
        m_tail.store(tail, std::memory_order_release);
        return true;
    }

    size_t DrainTo(fxe::Console& console) {
        size_t drained = 0U;
        fxe::FixedString<LINE_CAPACITY> line;
        for (; drained < MAX_DRAIN_LINES_PER_UPDATE; ++drained) {
            bool continues = false;
            if (!TryDequeue(line, continues)) {
                break;
            }
            if (!continues) {
                console.PrintLn("%s", line.CStr());
                continue;
            }
            console.BeginBulkOutput();
            do {
                console.PrintRaw(line.CStr(), line.Size());
            } while (continues && TryDequeue(line, continues));
            console.PrintRaw("\r\n", 2U);
            console.EndBulkOutput();
        }

        if (Size() == 0U) {
            const uint32_t dropped =
                m_pendingDroppedLines.exchange(0U, std::memory_order_acq_rel);
            if (dropped != 0U) {
                console.PrintLn("console: omitted %lu live log lines (queue full)",
                                static_cast<unsigned long>(dropped));
            }
        }
        return drained;
    }

    size_t Size() const {
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        return tail >= head ? tail - head : SLOT_COUNT - head + tail;
    }

    uint32_t TotalDroppedLines() const {
        return m_totalDroppedLines.load(std::memory_order_acquire);
    }

  private:
    static constexpr size_t SLOT_COUNT = QUEUED_LINE_CAPACITY + 1U;

    static constexpr size_t Increment(const size_t index) {
        return index + 1U == SLOT_COUNT ? 0U : index + 1U;
    }

    bool TryDequeue(fxe::FixedString<LINE_CAPACITY>& outLine, bool& outContinues) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire)) {
            return false;
        }

        outLine = m_lines[head];
        outContinues = m_lineContinues[head];
        m_head.store(Increment(head), std::memory_order_release);
        return true;
    }

    fxe::FixedString<LINE_CAPACITY> m_lines[SLOT_COUNT] = {};
    bool m_lineContinues[SLOT_COUNT] = {};
    std::atomic<size_t> m_head{0U};
    std::atomic<size_t> m_tail{0U};
    std::atomic<uint32_t> m_pendingDroppedLines{0U};
    std::atomic<uint32_t> m_totalDroppedLines{0U};
};

} // namespace firmware_internal
