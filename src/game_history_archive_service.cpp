#include "firmware/game_history_archive_service.hpp"

#include "logger.hpp"
#include "modules/duck_game_persistence.hpp"

namespace firmware {

bool GameHistoryArchiveService::Configure(const uint64_t newJournalId) {
    if (newJournalId == 0U || m_configured) return false;
    m_newJournalId = newJournalId;
    m_configured = true;
    return true;
}

void GameHistoryArchiveService::OnDurableSettingsSnapshot(
    const JsonDocument& document) {
    if (!m_configured ||
        !document.containsKey(modules::KEY_GAME_HISTORY_V1) ||
        !document.containsKey(modules::KEY_GAME_FEN)) {
        return;
    }

    if (!modules::ReadGamePersistenceState(document, m_snapshot)) {
        ++m_failureCount;
        logger::Warn("history: durable settings snapshot is invalid");
        return;
    }
    modules::DuckGameDurableState variantState;
    if (!modules::DecodeDuckGameDurableState(
            m_snapshot.variantState.CStr(), variantState)) {
        ++m_failureCount;
        logger::Warn("history: durable game variant is invalid");
        return;
    }
    // Setup intentionally stays variant-neutral until the first complete move
    // or a valid Duck placement latches the ruleset. Do not write a Classic
    // base record that would make the later Duck decision look like a branch.
    if (variantState.variant == modules::GameVariant::Undecided) {
        return;
    }
    modules::game_history::GameHistorySnapshotReader reader(m_snapshot);
    modules::game_history::GameHistoryCaptureSnapshot captureSnapshot;
    if (!reader.Build(captureSnapshot)) {
        ++m_failureCount;
        logger::Warn("history: durable game snapshot cannot be replayed");
        return;
    }
    if (!m_journal.IsOpen()) {
        const modules::game_history::JournalOpenResult opened =
            m_journal.Open(m_newJournalId);
        if (opened != modules::game_history::JournalOpenResult::Ready &&
            opened != modules::game_history::JournalOpenResult::Initialized) {
            ++m_failureCount;
            logger::Warn("history: journal open failed result=%u",
                         static_cast<unsigned>(opened));
            return;
        }
    }

    const modules::game_history::GameHistoryCaptureResult result =
        m_capture.Reconcile(captureSnapshot);
    if (result != modules::game_history::GameHistoryCaptureResult::Applied &&
        result != modules::game_history::GameHistoryCaptureResult::NoChange) {
        if (result == modules::game_history::GameHistoryCaptureResult::IoFailure ||
            result == modules::game_history::GameHistoryCaptureResult::Corrupt) {
            // A scan may conservatively quarantine a transient read failure.
            // Rescan on the next durable callback; real invalid bytes stay intact.
            m_journal.Close();
        }
        ++m_failureCount;
        logger::Warn("history: journal reconciliation failed result=%u",
                     static_cast<unsigned>(result));
    }
}

} // namespace firmware
