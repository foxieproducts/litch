#include "hardware/chessboard_hardware.hpp"

#include "foxie_embedded/millis.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace hardware {

namespace {

uint64_t MapOccupancyMask(const uint64_t rawMask,
                          const bool flipSides) {
    uint64_t logicalMask = 0;
    for (uint8_t rawSquare = 0; rawSquare < 64U; ++rawSquare) {
        const uint64_t rawSquareMask = (1ULL << rawSquare);
        if ((rawMask & rawSquareMask) == 0ULL) {
            continue;
        }

        const uint8_t logicalSquare = runtime::MapSquareIndex(rawSquare, flipSides);
        logicalMask |= (1ULL << logicalSquare);
    }
    return logicalMask;
}

} // namespace

void ChessboardHardware::QueuePendingSensorChanges() {
    modules::SquareDelta delta;

    while (m_boardInput.TryReadDelta(delta)) {
        const uint8_t logicalSquare = runtime::MapSquareIndex(delta.square, m_flipSides);
        (void)m_sensorChangeQueue.Enqueue(runtime::SensorChangeEvent{
            .tMs = fxe::Millis(),
            .rawSquare = delta.square,
            .logicalSquare = logicalSquare,
            .nowOccupied = delta.nowOccupied,
        });
    }
}

void ChessboardHardware::DispatchQueuedSensorChanges(runtime::InteractionDispatcher& dispatcher
#ifdef LITCH_TRACE
                                                     ,
                                                     trace::TraceRecorder* recorder
#endif
) {
    runtime::SensorChangeEvent sensorEvent;
    while (m_sensorChangeQueue.TryDequeue(sensorEvent)) {
#ifdef LITCH_TRACE
        if (recorder) {
            // Record raw square (pre flip-sides) so replay can apply settings.
            recorder->RecordSensorDelta(sensorEvent.rawSquare, sensorEvent.nowOccupied);
        }
#endif

        // This remains the current ingress seam for physical board events.
        // Queue first so capture order is explicit before game dispatch.
        dispatcher.DispatchSensorChange(sensorEvent);
        ++m_sensorChangeCount;

        // Invariant: update previous occupancy only after dispatch.
        m_previousOccupancy[sensorEvent.logicalSquare] = sensorEvent.nowOccupied;

        const uint64_t mask = (1ULL << sensorEvent.logicalSquare);
        const uint64_t rawMask = (1ULL << sensorEvent.rawSquare);
        if (sensorEvent.nowOccupied) {
            m_occ64 |= mask;
            m_rawOcc64 |= rawMask;
        } else {
            m_occ64 &= ~mask;
            m_rawOcc64 &= ~rawMask;
        }
    }
}

modules::BoardScanAttemptResult ChessboardHardware::CaptureScanAttempt() {
    if (!m_boardInput.SupportsAtomicObservations()) {
        return {};
    }
    return m_boardInput.CaptureScanAttempt();
}

bool ChessboardHardware::AcceptAndDispatchScan(
    const modules::BoardScanAttemptResult& result,
    runtime::InteractionDispatcher& dispatcher,
    const ObservationDispatchMode mode
#ifdef LITCH_TRACE
    ,
    trace::TraceRecorder* recorder
#endif
) {
    if (result.status != modules::BoardScanAttemptStatus::Succeeded) {
        return false;
    }

    const uint64_t rawChangedMask = result.rawOccupiedMask ^ m_rawOcc64;
    const uint64_t occupiedMask = MapOccupancyMask(
        result.rawOccupiedMask, m_flipSides);
    const uint64_t changedMask = occupiedMask ^ m_occ64;
    if (changedMask == 0ULL &&
        mode == ObservationDispatchMode::ChangedOnly) {
        return false;
    }

    const uint32_t observationSequence = m_observationSequence + 1U;
    const runtime::BoardObservationEvent observation{
        .tMs = fxe::Millis(),
        .sequence = observationSequence,
        .changedMask = changedMask,
        .occupiedMask = occupiedMask,
    };
    uint8_t deferredDuckDestination = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    const uint32_t eventSequence =
        mode == ObservationDispatchMode::Always
            ? dispatcher.DispatchBoardResynchronization(observation)
            : dispatcher.DispatchBoardObservation(observation, &deferredDuckDestination);
    if (eventSequence == 0U) {
        return false;
    }
    m_observationSequence = observationSequence;

#ifdef LITCH_TRACE
    if (recorder && mode != ObservationDispatchMode::Always) {
        // Trace has no runtime-health/resync record yet. Recording this as an
        // ordinary observation would replay it through move inference and
        // misrepresent the correction-only recovery authority.
        recorder->RecordBoardObservation(rawChangedMask,
                                         result.rawOccupiedMask,
                                         eventSequence,
            deferredDuckDestination < 64U
                ? runtime::MapSquareIndex(deferredDuckDestination, m_flipSides)
                : runtime::CHESS_INTERACTION_INVALID_SQUARE);
        const runtime::GameStateMachine& stateMachine =
            dispatcher.GetStateMachine();
        recorder->RecordCheckpointInteraction(
            stateMachine.GetChessInteractionModel(),
            stateMachine.GetInteractionInvariantStatus());
    }
#endif

    for (uint8_t square = 0; square < 64U; ++square) {
        const uint64_t squareMask = (1ULL << square);
        if ((changedMask & squareMask) == 0ULL) {
            continue;
        }
        m_previousOccupancy[square] = (occupiedMask & squareMask) != 0ULL;
        ++m_sensorChangeCount;
    }
    m_rawOcc64 = result.rawOccupiedMask;
    m_occ64 = occupiedMask;
    return true;
}

void ChessboardHardware::PollAndDispatch(runtime::InteractionDispatcher& dispatcher
#ifdef LITCH_TRACE
                                         ,
                                         trace::TraceRecorder* recorder
#endif
) {
    if (m_boardInput.SupportsAtomicObservations()) {
        const modules::BoardScanAttemptResult result = CaptureScanAttempt();
        (void)AcceptAndDispatchScan(
            result,
            dispatcher,
            ObservationDispatchMode::ChangedOnly
#ifdef LITCH_TRACE
            ,
            recorder
#endif
        );
        return;
    }

    QueuePendingSensorChanges();
    DispatchQueuedSensorChanges(dispatcher
#ifdef LITCH_TRACE
                                ,
                                recorder
#endif
    );
}

void ChessboardHardware::PollAndDispatch(runtime::GameStateMachine& sm
#ifdef LITCH_TRACE
                                         ,
                                         trace::TraceRecorder* recorder
#endif
) {
    runtime::InteractionDispatcher dispatcher(sm);
    PollAndDispatch(dispatcher
#ifdef LITCH_TRACE
                    ,
                    recorder
#endif
    );
}

} // namespace hardware
