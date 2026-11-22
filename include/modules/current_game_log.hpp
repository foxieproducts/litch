#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/game_history_protocol.hpp"

namespace modules::current_game_log {

inline constexpr uint16_t MAX_PLIES = 4096U;
inline constexpr uint16_t MAX_LINE_RECORDS = MAX_PLIES + 2U;
inline constexpr uint32_t MAX_SLOT_BYTES =
    static_cast<uint32_t>(game_history::LINE_BASE_RECORD_BYTES) +
    static_cast<uint32_t>(MAX_PLIES) *
        static_cast<uint32_t>(game_history::MAX_COMMITTED_PLY_RECORD_BYTES) +
    static_cast<uint32_t>(game_history::SEAL_RECORD_BYTES);

enum class Slot : uint8_t {
    A = 0U,
    B = 1U,
};

enum class StorageStatus : uint8_t {
    Missing = 0U,
    Present,
    IoFailure,
};

struct CommitPointer {
    bool present = false;
    Slot slot = Slot::A;
    uint32_t generation = 0U;
    uint32_t byteLength = 0U;
    uint32_t prefixCrc32 = 0U;
    game_history::GameId gameId{};
    uint64_t lineRevision = 0U;
    uint16_t currentPly = 0U;
    bool archiveRequired = false;
};

// An undecided setup cannot produce a game-history line. Its prior empty
// opening may be retired, but a played or unarchived line must remain durable.
inline bool CanRetireUndecidedOpening(const CommitPointer& authority,
                                     const uint16_t currentPly,
                                     const bool variantUndecided) {
    return variantUndecided && currentPly == 0U &&
        (!authority.present ||
         (authority.currentPly == 0U && authority.lineRevision == 1U &&
          !authority.archiveRequired));
}

struct StorageOps {
    void* context = nullptr;
    StorageStatus (*stat)(void*, Slot, uint32_t&) = nullptr;
    bool (*read)(void*, Slot, uint32_t, uint8_t*, size_t) = nullptr;
    bool (*replace)(void*, Slot, const uint8_t*, size_t) = nullptr;
    bool (*append)(void*, Slot, const uint8_t*, size_t) = nullptr;
    // Drops only bytes beyond a previously validated committed prefix. A
    // failed call must never shorten below the requested committed boundary.
    bool (*truncate)(void*, Slot, uint32_t) = nullptr;
    // Optional operation batch. Production storage uses this to amortize file
    // opens and render handoffs while still yielding at bounded intervals.
    bool (*beginBatch)(void*) = nullptr;
    void (*endBatch)(void*) = nullptr;
};

struct SettingsOps {
    using Receipt = uint32_t;

    void* context = nullptr;
    // Admission writes this pointer into the caller-owned exact game snapshot.
    // The caller retains the returned receipt until the existing settings
    // worker reports a terminal outcome and successfully releases it.
    bool (*enqueueTracked)(void*, const CommitPointer&, Receipt&) = nullptr;
};

struct LineSource {
    void* context = nullptr;
    uint16_t recordCount = 0U;
    bool (*readRecord)(void*, uint16_t, game_history::Record&) = nullptr;
};

// Structural variant continuity shared by durable-log validation and the
// read-only sync source. General move legality remains with the game engine.
bool IsValidCommittedPlyForVariant(
    game_history::Variant variant,
    const game_history::Position& prior,
    const game_history::Record& record);

enum class Result : uint8_t {
    Applied = 0U,
    Ready,
    Missing,
    Bounds,
    Corrupt,
    IoFailure,
    StorageFailure,
    ReadbackMismatch,
    SettingsRejected,
    BadState,
};

class CurrentGameLog final {
  public:
    CurrentGameLog(StorageOps storage, SettingsOps settings)
        : m_storage(storage), m_settings(settings) {}

    Result Open(const CommitPointer& authority);
    // Returns the last validated record for the currently open authority
    // without rescanning the already verified prefix.
    Result ReadLatest(const CommitPointer& authority,
                      game_history::Record& out) const;
    Result Initialize(const game_history::Record& base,
                      bool archiveRequired,
                      CommitPointer& outCandidate,
                      SettingsOps::Receipt& outReceipt);
    // Initializes one already-reconstructed complete line with a single
    // settings promotion. This is used when the first worker-owned snapshot
    // contains more than the untouched base position.
    Result InitializeLine(const LineSource& source,
                          bool archiveRequired,
                          CommitPointer& outCandidate,
                          SettingsOps::Receipt& outReceipt);
    Result Append(const CommitPointer& authority,
                  const game_history::Record& record,
                  CommitPointer& outCandidate,
                  SettingsOps::Receipt& outReceipt);
    Result Rewrite(const CommitPointer& authority,
                   const LineSource& source,
                   CommitPointer& outCandidate,
                   SettingsOps::Receipt& outReceipt);
    // Replaces an acknowledged game with a complete new line in the inactive
    // slot. The old authority remains readable until settings promotion.
    Result ReplaceLine(const CommitPointer& authority,
                       const LineSource& source,
                       bool archiveRequired,
                       CommitPointer& outCandidate,
                       SettingsOps::Receipt& outReceipt);
    Result AcknowledgeArchived(const CommitPointer& authority,
                               CommitPointer& outCandidate,
                               SettingsOps::Receipt& outReceipt);
    // Call only after the tracked settings receipt reports Saved. This advances
    // the trusted session without rescanning bytes already proven before the
    // matching settings admission. A new process must still call Open.
    bool ConfirmPromotion(const CommitPointer& authority,
                          SettingsOps::Receipt receipt);
    // Call only after the tracked receipt reports Failed and has been
    // released. The prior open authority remains trusted; any appended suffix
    // is uncommitted and will be truncated by the next append.
    bool RejectPromotion(SettingsOps::Receipt receipt);

  private:
    Result Validate(const CommitPointer& authority,
                    game_history::Record* outLast,
                    game_history::Variant* outVariant = nullptr) const;
    Result WriteLine(Slot slot,
                     const LineSource& source,
                     CommitPointer& outCandidate,
                     game_history::Record& outLast) const;
    Result Enqueue(const CommitPointer& candidate,
                   const game_history::Record& latest,
                   game_history::Variant variant,
                   CommitPointer& outCandidate,
                   SettingsOps::Receipt& outReceipt);
    bool IsOpenAuthority(const CommitPointer& authority) const;

    StorageOps m_storage;
    SettingsOps m_settings;
    CommitPointer m_openAuthority{};
    game_history::Record m_latest{};
    CommitPointer m_pendingAuthority{};
    game_history::Record m_pendingLatest{};
    SettingsOps::Receipt m_pendingReceipt = 0U;
    bool m_open = false;
    game_history::Variant m_openVariant = game_history::Variant::Classic;
    game_history::Variant m_pendingVariant = game_history::Variant::Classic;
};

} // namespace modules::current_game_log
