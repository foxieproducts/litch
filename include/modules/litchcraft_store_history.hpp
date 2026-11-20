#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_store_history {

// One selected-frame stroke is one reversible operation. The journal stores
// only changed emitters; undo swaps its preimage with the displayed value, so
// the same bytes also serve redo without a second document or transport copy.
class StoreHistory final {
  public:
    static constexpr size_t ARENA_BYTES = 4096U;
    static constexpr size_t MAX_OPERATIONS = 64U;

    enum class Kind : uint8_t { Emitters, Duration, AddCopy, Delete };
    struct StructuralStep final {
        // AddCopy/Delete are wire actions; Insert restores a deleted frame.
        uint8_t action = 0U;
        uint16_t source = 0U;
    };
    void Clear();
    void Begin(uint16_t frame, const litchcraft_frame_store::Frame& displayed);
    void End(uint16_t frame, const litchcraft_frame_store::Frame& displayed);
    bool Step(bool redo, uint16_t frame,
              litchcraft_frame_store::Frame& displayed);
    void EndDuration(uint16_t frame, const litchcraft_frame_store::Frame& displayed);
    void CommitStructural(Kind kind, uint16_t frame);
    void CancelStaged() { m_staging = false; }
    bool NextStructural(bool redo, StructuralStep& out) const;
    bool ReadRemovedFrame(bool redo, litchcraft_frame_store::Frame& out) const;
    bool CommitStructuralStep(bool redo);
    bool SteppedNeedsRepair() const;
    uint16_t TargetFrame(bool redo) const;
    size_t SteppedEmitterCount() const;
    bool ReadSteppedEmitter(size_t ordinal,
                            const litchcraft_frame_store::Frame& displayed,
                            uint8_t& index,
                            litchcraft_frame_store::Emitter& emitter) const;
    uint8_t UndoCount() const { return m_cursor; }
    uint8_t RedoCount() const {
        return static_cast<uint8_t>(m_count - m_cursor);
    }

  private:
    static constexpr size_t RECORD_BYTES = 7U;
    struct Operation final {
        uint16_t offset = 0U;
        uint16_t length = 0U;
        uint16_t frame = 0U;
    };
    void EvictOldest();
    uint16_t Append(Kind kind, uint16_t frame, uint16_t length);
    static Kind OperationKind(const Operation& operation) {
        return static_cast<Kind>(operation.frame >> 8U);
    }
    static uint16_t OperationFrame(const Operation& operation) {
        return operation.frame & 0xffU;
    }
    static bool Same(const litchcraft_frame_store::Emitter& first,
                     const litchcraft_frame_store::Emitter& second);
    static void WriteEmitter(uint8_t* target,
                             const litchcraft_frame_store::Emitter& emitter);
    static litchcraft_frame_store::Emitter ReadEmitter(const uint8_t* source);

    std::array<uint8_t, ARENA_BYTES> m_bytes{};
    std::array<Operation, MAX_OPERATIONS> m_operations{};
    litchcraft_frame_store::Frame m_before{};
    uint16_t m_stagedFrame = 0U;
    uint16_t m_used = 0U;
    uint8_t m_count = 0U;
    uint8_t m_cursor = 0U;
    uint8_t m_lastStep = 0U;
    bool m_staging = false;
    bool m_lastStepValid = false;
};

} // namespace modules::litchcraft_store_history
