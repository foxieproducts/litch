#pragma once

#include <array>
#include <vector>
#include <cstring>
#include "modules/game_history_journal.hpp"

namespace game_history_test {
using namespace modules::game_history;

struct Artifact {
    bool present = false;
    std::vector<uint8_t> bytes;
};

struct FakeStorage {
    std::array<Artifact, 4U> artifacts{};
    unsigned journalAppends = 0U;
    unsigned journalReads = 0U;
    unsigned failJournalRead = 0U;
    unsigned failJournalAppend = 0U;
    bool writeBeforeFailure = false;

    static size_t Index(const JournalArtifact artifact) {
        return static_cast<size_t>(artifact);
    }

    static JournalStorageStatus Stat(void* const context,
                                     const JournalArtifact artifact,
                                     uint32_t& outSize) {
        const auto& value = static_cast<FakeStorage*>(context)
                                ->artifacts[Index(artifact)];
        if (!value.present) {
            outSize = 0U;
            return JournalStorageStatus::Missing;
        }
        outSize = static_cast<uint32_t>(value.bytes.size());
        return JournalStorageStatus::Present;
    }

    static bool Read(void* const context,
                     const JournalArtifact artifact,
                     const uint32_t offset,
                     uint8_t* const out,
                     const size_t length) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (artifact == JournalArtifact::Journal &&
            ++self.journalReads == self.failJournalRead) return false;
        const auto& value = self.artifacts[Index(artifact)];
        if (!value.present || out == nullptr ||
            offset > value.bytes.size() ||
            length > value.bytes.size() - offset) {
            return false;
        }
        std::memcpy(out, value.bytes.data() + offset, length);
        return true;
    }

    static bool Replace(void* const context,
                        const JournalArtifact artifact,
                        const uint8_t* const bytes,
                        const size_t length) {
        if (bytes == nullptr && length != 0U) return false;
        auto& value = static_cast<FakeStorage*>(context)
                          ->artifacts[Index(artifact)];
        value.present = true;
        if (length == 0U) {
            value.bytes.clear();
        } else {
            value.bytes.assign(bytes, bytes + length);
        }
        return true;
    }

    static bool Append(void* const context,
                       const JournalArtifact artifact,
                       const uint8_t* const bytes,
                       const size_t length) {
        if (bytes == nullptr || length == 0U) return false;
        auto& self = *static_cast<FakeStorage*>(context);
        const bool fail = artifact == JournalArtifact::Journal &&
                          ++self.journalAppends == self.failJournalAppend;
        if (fail && !self.writeBeforeFailure) return false;
        auto& value = self.artifacts[Index(artifact)];
        value.present = true;
        value.bytes.insert(value.bytes.end(), bytes, bytes + length);
        return !fail;
    }

    static bool Promote(void* const context,
                        const JournalArtifact from,
                        const JournalArtifact to) {
        auto& self = *static_cast<FakeStorage*>(context);
        if (!self.artifacts[Index(from)].present) return false;
        self.artifacts[Index(to)] = self.artifacts[Index(from)];
        self.artifacts[Index(from)] = {};
        return true;
    }

    GameHistoryJournal Journal() {
        return GameHistoryJournal(
            {this, &Stat, &Read, &Replace, &Append, &Promote});
    }
};

} // namespace game_history_test
