#pragma once

#include <cstddef>

#include "modules/game_persistence_coordinator.hpp"
#include "trace/trace_jsonl_event_reader.hpp"

namespace trace {

inline bool TraceContainsExternalizedInteractionEvents(
    const char* const* lines,
    const size_t lineCount) {
    for (size_t i = 0; i < lineCount; ++i) {
        if (lines[i] == nullptr) {
            continue;
        }
        JsonlEventRecord record;
        if (!TryParseJsonlEventRecord(lines[i], record)) {
            continue;
        }
        if (record.type == JsonlEventType::PositionAcknowledged ||
            record.type == JsonlEventType::ActionResult ||
            record.type == JsonlEventType::AdapterResult) {
            return true;
        }
    }
    return false;
}

class ExternalizedInteractionEventsGuard final {
  public:
    explicit ExternalizedInteractionEventsGuard(
        modules::GamePersistenceCoordinator* coordinator,
        const bool externalize)
        : m_coordinator(coordinator),
          m_previous(coordinator != nullptr &&
                     coordinator->AreInteractionEventsExternalized()) {
        if (m_coordinator != nullptr && externalize) {
            m_coordinator->SetInteractionEventsExternalized(true);
        }
    }

    ~ExternalizedInteractionEventsGuard() {
        if (m_coordinator != nullptr) {
            m_coordinator->SetInteractionEventsExternalized(m_previous);
        }
    }

    ExternalizedInteractionEventsGuard(
        const ExternalizedInteractionEventsGuard&) = delete;
    ExternalizedInteractionEventsGuard& operator=(
        const ExternalizedInteractionEventsGuard&) = delete;

  private:
    modules::GamePersistenceCoordinator* m_coordinator = nullptr;
    bool m_previous = false;
};

} // namespace trace
