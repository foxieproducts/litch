#pragma once

#include <cstring>
#include <vector>

#include "modules/current_game_log.hpp"

// Shared storage adapter for the real current-line service/worker fixtures.
namespace persistence_test {

struct CurrentLineMemoryStorage {
    using Slot = modules::current_game_log::Slot;
    using Status = modules::current_game_log::StorageStatus;
    std::vector<uint8_t> slots[2U];
    uint32_t beginBatchCalls = 0U;
    uint32_t endBatchCalls = 0U;

    static size_t Index(const Slot slot) {
        return slot == Slot::A ? 0U : 1U;
    }
    static Status Stat(void* context, Slot slot, uint32_t& outSize) {
        auto& bytes = static_cast<CurrentLineMemoryStorage*>(context)
                          ->slots[Index(slot)];
        outSize = static_cast<uint32_t>(bytes.size());
        return bytes.empty() ? Status::Missing : Status::Present;
    }
    static bool Read(void* context, Slot slot, uint32_t offset,
                     uint8_t* out, size_t length) {
        const auto& bytes = static_cast<CurrentLineMemoryStorage*>(context)
                                ->slots[Index(slot)];
        if ((out == nullptr && length != 0U) || offset > bytes.size() ||
            length > bytes.size() - offset) return false;
        if (length != 0U) {
            std::memcpy(out, bytes.data() + offset, length);
        }
        return true;
    }
    static bool Replace(void* context, Slot slot,
                        const uint8_t* bytes, size_t length) {
        if (bytes == nullptr && length != 0U) return false;
        auto& target = static_cast<CurrentLineMemoryStorage*>(context)
                           ->slots[Index(slot)];
        target.clear();
        if (length != 0U) target.assign(bytes, bytes + length);
        return true;
    }
    static bool Append(void* context, Slot slot,
                       const uint8_t* bytes, size_t length) {
        if (bytes == nullptr || length == 0U) return false;
        auto& target = static_cast<CurrentLineMemoryStorage*>(context)
                           ->slots[Index(slot)];
        target.insert(target.end(), bytes, bytes + length);
        return true;
    }
    static bool Truncate(void* context, Slot slot, uint32_t length) {
        auto& target = static_cast<CurrentLineMemoryStorage*>(context)
                           ->slots[Index(slot)];
        if (length > target.size()) return false;
        target.resize(length);
        return true;
    }
    static bool BeginBatch(void* context) {
        if (context == nullptr) return false;
        ++static_cast<CurrentLineMemoryStorage*>(context)->beginBatchCalls;
        return true;
    }
    static void EndBatch(void* context) {
        if (context != nullptr) {
            ++static_cast<CurrentLineMemoryStorage*>(context)->endBatchCalls;
        }
    }
    modules::current_game_log::StorageOps Ops() {
        return {this, &Stat, &Read, &Replace, &Append, &Truncate,
                &BeginBatch, &EndBatch};
    }
};

} // namespace persistence_test
