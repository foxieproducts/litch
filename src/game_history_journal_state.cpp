#include "modules/game_history_journal.hpp"

#include <array>
#include <cstring>

#include "foxie_embedded/checksum.hpp"

namespace modules::game_history {
namespace {

constexpr uint8_t STATE_MAGIC[4] = {'L', 'G', 'J', '1'};
constexpr uint8_t STATE_VERSION = 1U;

void WriteLe32(uint8_t* const out, const uint32_t value) {
    for (uint8_t index = 0U; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> static_cast<uint8_t>(index * 8U)) & 0xFFU);
    }
}

void WriteLe64(uint8_t* const out, const uint64_t value) {
    for (uint8_t index = 0U; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(
            (value >> static_cast<uint8_t>(index * 8U)) & 0xFFU);
    }
}

uint32_t ReadLe32(const uint8_t* const bytes) {
    uint32_t value = 0U;
    for (uint8_t index = 0U; index < 4U; ++index) {
        value |= static_cast<uint32_t>(bytes[index]) <<
                 static_cast<uint8_t>(index * 8U);
    }
    return value;
}

uint64_t ReadLe64(const uint8_t* const bytes) {
    uint64_t value = 0U;
    for (uint8_t index = 0U; index < 8U; ++index) {
        value |= static_cast<uint64_t>(bytes[index]) <<
                 static_cast<uint8_t>(index * 8U);
    }
    return value;
}

bool SameStateBytes(const uint8_t* const left,
                    const uint8_t* const right) {
    return std::memcmp(left, right, JOURNAL_STATE_BYTES) == 0;
}

} // namespace

bool GameHistoryJournal::SerializeState(
    const State& state,
    uint8_t out[JOURNAL_STATE_BYTES]) const {
    if (out == nullptr || state.generation == 0U || state.journalId == 0U ||
        state.pending > PendingCompaction::EvictLostGame) {
        return false;
    }
    const uint32_t newest = state.nextSequence == 0U
                                ? UINT32_MAX
                                : state.nextSequence - 1U;
    if (state.durableAck > newest) {
        return false;
    }
    if (state.pending == PendingCompaction::None) {
        if (state.pendingOldest != 0U || state.pendingNext != 0U ||
            state.pendingBytes != 0U) {
            return false;
        }
    } else if (state.pendingOldest == 0U || state.pendingBytes == 0U ||
               (state.pendingNext != 0U &&
                state.pendingNext <= state.pendingOldest)) {
        return false;
    }

    std::memset(out, 0, JOURNAL_STATE_BYTES);
    std::memcpy(out, STATE_MAGIC, sizeof(STATE_MAGIC));
    out[4U] = STATE_VERSION;
    out[5U] = static_cast<uint8_t>(state.pending);
    WriteLe32(out + 8U, state.generation);
    WriteLe64(out + 12U, state.journalId);
    WriteLe32(out + 20U, state.nextSequence);
    WriteLe32(out + 24U, state.durableAck);
    WriteLe32(out + 28U, state.lostGameCount);
    WriteLe32(out + 32U, state.compactionCount);
    WriteLe32(out + 36U, state.recoveryCount);
    WriteLe32(out + 40U, state.pendingOldest);
    WriteLe32(out + 44U, state.pendingNext);
    WriteLe32(out + 48U, state.pendingBytes);
    WriteLe32(out + 52U, fxe::Crc32(out, 52U));
    return true;
}

bool GameHistoryJournal::DeserializeState(const uint8_t* const bytes,
                                          const size_t length,
                                          State& out) const {
    if (bytes == nullptr || length != JOURNAL_STATE_BYTES ||
        std::memcmp(bytes, STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 ||
        bytes[4U] != STATE_VERSION || bytes[6U] != 0U || bytes[7U] != 0U ||
        ReadLe32(bytes + 52U) != fxe::Crc32(bytes, 52U)) {
        return false;
    }
    State candidate;
    candidate.pending = static_cast<PendingCompaction>(bytes[5U]);
    candidate.generation = ReadLe32(bytes + 8U);
    candidate.journalId = ReadLe64(bytes + 12U);
    candidate.nextSequence = ReadLe32(bytes + 20U);
    candidate.durableAck = ReadLe32(bytes + 24U);
    candidate.lostGameCount = ReadLe32(bytes + 28U);
    candidate.compactionCount = ReadLe32(bytes + 32U);
    candidate.recoveryCount = ReadLe32(bytes + 36U);
    candidate.pendingOldest = ReadLe32(bytes + 40U);
    candidate.pendingNext = ReadLe32(bytes + 44U);
    candidate.pendingBytes = ReadLe32(bytes + 48U);
    uint8_t checked[JOURNAL_STATE_BYTES] = {};
    if (!SerializeState(candidate, checked) ||
        !SameStateBytes(bytes, checked)) {
        return false;
    }
    out = candidate;
    return true;
}

JournalOpenResult GameHistoryJournal::ReadState(const JournalArtifact artifact,
                                   State& out) const {
    if (m_ops.stat == nullptr || m_ops.read == nullptr) {
        return JournalOpenResult::IoFailure;
    }
    uint32_t size = 0U;
    const auto status = m_ops.stat(m_ops.context, artifact, size);
    if (status == JournalStorageStatus::IoFailure) {
        return JournalOpenResult::IoFailure;
    }
    if (status != JournalStorageStatus::Present || size != JOURNAL_STATE_BYTES) {
        return JournalOpenResult::Corrupt;
    }
    uint8_t bytes[JOURNAL_STATE_BYTES] = {};
    if (!m_ops.read(m_ops.context, artifact, 0U, bytes, sizeof(bytes))) {
        return JournalOpenResult::IoFailure;
    }
    return DeserializeState(bytes, sizeof(bytes), out)
               ? JournalOpenResult::Ready : JournalOpenResult::Corrupt;
}

JournalOpenResult GameHistoryJournal::LoadState(bool& outMissing) {
    outMissing = false;
    if (m_ops.stat == nullptr || m_ops.read == nullptr) {
        return JournalOpenResult::IoFailure;
    }
    uint32_t sizeA = 0U;
    uint32_t sizeB = 0U;
    const JournalStorageStatus statusA =
        m_ops.stat(m_ops.context, JournalArtifact::StateA, sizeA);
    const JournalStorageStatus statusB =
        m_ops.stat(m_ops.context, JournalArtifact::StateB, sizeB);
    if (statusA == JournalStorageStatus::IoFailure ||
        statusB == JournalStorageStatus::IoFailure) {
        return JournalOpenResult::IoFailure;
    }
    const bool presentA = statusA == JournalStorageStatus::Present;
    const bool presentB = statusB == JournalStorageStatus::Present;
    if (!presentA && !presentB) {
        outMissing = true;
        return JournalOpenResult::Ready;
    }

    State stateA;
    State stateB;
    const auto readA = presentA && sizeA == JOURNAL_STATE_BYTES
                          ? ReadState(JournalArtifact::StateA, stateA)
                          : JournalOpenResult::Corrupt;
    const auto readB = presentB && sizeB == JOURNAL_STATE_BYTES
                          ? ReadState(JournalArtifact::StateB, stateB)
                          : JournalOpenResult::Corrupt;
    if (readA == JournalOpenResult::IoFailure || readB == JournalOpenResult::IoFailure) {
        return JournalOpenResult::IoFailure;
    }
    const bool validA = readA == JournalOpenResult::Ready;
    const bool validB = readB == JournalOpenResult::Ready;
    if (!validA && !validB) {
        return JournalOpenResult::Corrupt;
    }
    if (validA && validB && stateA.generation == stateB.generation) {
        uint8_t bytesA[JOURNAL_STATE_BYTES] = {};
        uint8_t bytesB[JOURNAL_STATE_BYTES] = {};
        if (!SerializeState(stateA, bytesA) ||
            !SerializeState(stateB, bytesB) ||
            !SameStateBytes(bytesA, bytesB)) {
            return JournalOpenResult::Corrupt;
        }
    }
    if (validA && (!validB || stateA.generation >= stateB.generation)) {
        m_state = stateA;
        m_stateArtifact = JournalArtifact::StateA;
    } else {
        m_state = stateB;
        m_stateArtifact = JournalArtifact::StateB;
    }
    return JournalOpenResult::Ready;
}

bool GameHistoryJournal::SaveState(const State& replacement) {
    if (m_ops.replace == nullptr || m_ops.stat == nullptr ||
        m_ops.read == nullptr || m_state.generation == UINT32_MAX) {
        return false;
    }
    State candidate = replacement;
    candidate.generation = m_state.generation + 1U;
    uint8_t bytes[JOURNAL_STATE_BYTES] = {};
    if (!SerializeState(candidate, bytes)) {
        return false;
    }
    const JournalArtifact target = m_state.generation == 0U
                                       ? JournalArtifact::StateA
                                       : (m_stateArtifact ==
                                                  JournalArtifact::StateA
                                              ? JournalArtifact::StateB
                                              : JournalArtifact::StateA);
    if (!m_ops.replace(m_ops.context, target, bytes, sizeof(bytes))) {
        return false;
    }
    State checked;
    if (ReadState(target, checked) != JournalOpenResult::Ready) {
        return false;
    }
    uint8_t checkedBytes[JOURNAL_STATE_BYTES] = {};
    if (!SerializeState(checked, checkedBytes) ||
        !SameStateBytes(bytes, checkedBytes)) {
        return false;
    }
    m_state = candidate;
    m_stateArtifact = target;
    return true;
}

} // namespace modules::game_history
