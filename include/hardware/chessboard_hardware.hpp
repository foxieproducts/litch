#pragma once

#include <cstdint>

#include "modules/board_input.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/sensor_change_event_queue.hpp"
#include "runtime/square_mapping.hpp"

#ifdef LITCH_TRACE
#include "trace/trace_recorder.hpp"
#endif

namespace hardware {

// Hardware coordinator: consumes board-input deltas, applies flip-sides mapping,
// dispatches into the core state machine, and maintains a canonical occupancy map.
class ChessboardHardware final {
  public:
    enum class ObservationDispatchMode : uint8_t {
        ChangedOnly = 0,
        Always,
    };

    explicit ChessboardHardware(modules::IBoardInput& boardInput)
        : m_boardInput(boardInput) {}

    void SetFlipSides(const bool flipSides) { m_flipSides = flipSides; }

    void Resync() {
        m_boardInput.Resync();
        m_sensorChangeQueue.Clear();

        m_rawOcc64 = 0ULL;
        m_occ64 = 0;
        for (uint8_t i = 0; i < 64; ++i) {
            m_previousOccupancy[i] = false;
        }

        uint64_t rawOcc64 = 0;
        if (!m_boardInput.TryReadSnapshot(rawOcc64)) {
            return;
        }

        m_rawOcc64 = rawOcc64;

        for (uint8_t rawSquare = 0; rawSquare < 64; ++rawSquare) {
            const bool occupied = (rawOcc64 & (1ULL << rawSquare)) != 0;
            if (!occupied) {
                continue;
            }

            const uint8_t logicalSquare = runtime::MapSquareIndex(rawSquare, m_flipSides);
            m_previousOccupancy[logicalSquare] = true;
            m_occ64 |= (1ULL << logicalSquare);
        }
    }

    void PollAndDispatch(runtime::InteractionDispatcher& dispatcher
#ifdef LITCH_TRACE
                         ,
                         trace::TraceRecorder* recorder
#endif
    );

    // Capture does not mutate accepted occupancy or dispatch any observation.
    modules::BoardScanAttemptResult CaptureScanAttempt();

    // Accept one successful capture through the canonical observation path.
    // Always is reserved for an explicitly selected fresh recovery snapshot.
    bool AcceptAndDispatchScan(
        const modules::BoardScanAttemptResult& result,
        runtime::InteractionDispatcher& dispatcher,
        ObservationDispatchMode mode = ObservationDispatchMode::ChangedOnly
#ifdef LITCH_TRACE
        ,
        trace::TraceRecorder* recorder = nullptr
#endif
    );

    void PollAndDispatch(runtime::GameStateMachine& sm
#ifdef LITCH_TRACE
                         ,
                         trace::TraceRecorder* recorder
#endif
    );

    bool GetOccupied(const uint8_t logicalSquare) const {
        if (!runtime::IsValidSquareIndex(logicalSquare)) {
            return false;
        }
        return (m_occ64 & (1ULL << logicalSquare)) != 0;
    }

    uint32_t GetSensorChangeCount() const {
        return m_sensorChangeCount;
    }

  private:
    void QueuePendingSensorChanges();
    void DispatchQueuedSensorChanges(runtime::InteractionDispatcher& dispatcher
#ifdef LITCH_TRACE
                                     ,
                                     trace::TraceRecorder* recorder
#endif
    );

    modules::IBoardInput& m_boardInput;
    bool m_flipSides = false;

    bool m_previousOccupancy[64] = {};
    uint64_t m_rawOcc64 = 0;
    uint64_t m_occ64 = 0;
    uint32_t m_sensorChangeCount = 0;
    uint32_t m_observationSequence = 0;
    runtime::SensorChangeEventQueue m_sensorChangeQueue = runtime::SensorChangeEventQueue();
};

} // namespace hardware
