#pragma once

#include <cstdint>

#include "modules/board_owner_pairing_controller.hpp"
#include "modules/owner_pairing_session_wire.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace modules {

class MenuLevelOverlay;

// Physical consent, timing and shared-session fencing around the single custody
// controller. Native effects remain addressed acknowledgements from the adapter.
class BoardOwnerPairingRuntime final {
  public:
    using Controller = BoardOwnerPairingController;
    using Command = owner_pairing_session_wire::Command;
    using WireStatus = owner_pairing_session_wire::Status;
    using State = owner_pairing_session_wire::State;
    using Reason = Controller::Reason;
    static constexpr uint32_t CLAIM_WINDOW_MS = 10000U;
    static constexpr uint32_t CLEAR_HOLD_MS = 2000U;
    static constexpr uint32_t CLEAR_FEEDBACK_MS = 2000U;
    static constexpr uint32_t PRESENTATION_FADE_MS = 200U;

    struct Connection {
        uint16_t handle = 0U;
        uint32_t epoch = 0U;
        uint32_t sessionToken = 0U;
    };
    struct PendingConsent {
        Connection connection{};
        uint16_t attempt = 0U;
        uint8_t physicalEnd = 0U;
        uint32_t prompt = 0U;
    };
    struct PhysicalButton {
        uint8_t physicalEnd = 0U;
        bool isX = false;
        bool down = false;
        uint32_t eventTimeMs = 0U;
    };
    enum class PhysicalCue : uint8_t { None, Claim, Clear, Waiting, Restart, Clearing, Cleared, Confirmed };
    struct PhysicalPresentation {
        State state = State::Unavailable;
        PhysicalCue cue = PhysicalCue::None;
        bool active = false;
        uint8_t physicalEnd = 0U;
        uint16_t progress256 = 0U;
        const char* text = "";
        const char* nextAction = "ordinary-gameplay";
    };

    struct CueSnapshot {
        PhysicalCue cue = PhysicalCue::None;
        PhysicalCue previous = PhysicalCue::None;
        uint32_t startedMs = 0U;
        uint32_t previousStartedMs = 0U;
        uint32_t clearStartedMs = 0U;
        uint32_t clearSucceededMs = 0U;
        uint32_t consentStartedMs = 0U;
        uint32_t promptStartedMs = 0U;
        uint32_t holdStartedMs = 0U;
        uint8_t end = 0U;
        bool active = false;
        bool holdingClear = false;
        bool pairPrompt = false;
    };
    CueSnapshot CaptureCue() const;
    static void ComposePhysicalCue(const CueSnapshot& cue,
        status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
        bool controlBanksSwapped);
    static bool SyncPhysicalMessage(const CueSnapshot& cue, bool& wasVisible,
        MenuLevelOverlay& overlay, const std::array<Rgb, 64>& background,
        bool controlBanksSwapped, bool allowed);

    explicit BoardOwnerPairingRuntime(BoardOwnerPersistence& persistence)
        : m_custody(persistence), m_persistence(persistence) {}

    void Boot(BoardOwnerRecord record, Controller::BondInventory bonds,
              bool identityDurable);
    // Supply freshly read durable facts from the existing persistence owner.
    void UpdateDurableState(BoardOwnerRecord record, bool identityDurable);
    bool Begin(const Command& command, Connection connection,
               const BoardOwnerAddBaseline& baseline = {});
    bool Cancel(const Command& command, Connection connection);
    // Consent adapters must address the exact still-armed claim and name one
    // actual bank when its pending end is EITHER_END. This starts
    // native security; it grants no ownership or durable-save acknowledgement.
    PendingConsent ReadPendingConsent() const;
    bool ConfirmPendingConsent(PendingConsent consent);
    // Observe every debounced X/O Press/Release before normal game routing.
    // Return true for input consumed by a pairing context or its held tail.
    bool ObserveButton(PhysicalButton button);
    bool LocalClearMenuCompleted() const {
        return m_closeLocalMenuOnSuccess && m_clearFeedbackSucceeded;
    }
    // Only after the input adapter seeds held controls until their release.
    void ReseedPhysicalInput();
    void SetPhysicalButtonTicksAllowed(const bool allowed) { m_physicalButtonTicksAllowed = allowed; }
    void SetPhysicalButtonTickTime(const uint32_t throughMs) {
        m_physicalButtonTickMs = throughMs;
        m_physicalButtonTickTimeValid = true;
    }
    bool SelectLocalClear(uint16_t attempt, uint8_t physicalEnd);
    // Trusted local consent: pairing only, using the existing checked clear.
    bool RequestLocalPairingClear();
    // Existing authenticated owner consent, fenced to the exact live session.
    // Starts the same durable Clearing/native cleanup/Absent operation.
    bool RequestOwnerClear(Connection connection);
    // The Console reset retains Waiting through replacement settings and
    // fallback qualification. Failure retains app status and releases local play.
    void CompleteOwnerSettingsReset(bool saved);
    void LeaveLocalClear();
    void SyncLocalClearContext(bool selected, uint8_t physicalEnd);
    void Poll();

    void Retire(Connection connection);
    // Called only after the native disconnect has actually occurred.
    void Disconnected(Connection connection);
    bool NeedsDisconnect() const { return m_needsDisconnect; }
    Connection DisconnectTarget() const { return m_claim; }
    bool ClaimLeaseMatches(Connection connection, Controller::Effect effect) const;
    bool PhoneInitiatesSecurity() const { return m_claimValid && m_phoneInitiatesSecurity; }
    bool RegisterOwnerConnection(Connection connection,
                                 Controller::ConnectionSecurity security);
    bool OwnerReady(Connection connection) const;
    WireStatus Status(Connection connection) const;
    WireStatus Status() const { return Status(Connection{}); }
    PhysicalPresentation Presentation() const;
    // Physical-end intent uses the existing profile mapper. This converts its
    // logical-bank input once, independently of game orientation/chess side.
    void ComposePhysicalCue(
        status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
        bool controlBanksSwapped = false) const;
    // Reuse the existing text owner; refresh only after a state/message change.
    // Higher-priority rendering passes false before taking over the display.
    bool SyncPhysicalMessage(MenuLevelOverlay& overlay,
                             const std::array<Rgb, 64>& background,
                             bool controlBanksSwapped, bool allowed);

    // Adapter lifetime matches this runtime. Acknowledge only freshly checked
    // native effects; security additionally requires ClaimLeaseMatches.
    Controller& Custody() { return m_custody; }
    const Controller& Custody() const { return m_custody; }

  private:
    static bool SameConnection(Connection left, Connection right);
    bool PairPrompt() const;
    bool ClearPrompt() const;
    bool AlreadyClear() const;
    void ArmSelectedLocalClear();
    bool AnyButtonDown() const;
    bool ConfirmClear(uint16_t attempt, uint8_t physicalEnd);
    void RetainHeldTail();
    void UpdatePhysicalCue();
    static void ComposePerimeterCue(const CueSnapshot& cue,
        status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay);

    enum class SettingsReset : uint8_t { None, Pending, Failed };
    SettingsReset m_settingsReset = SettingsReset::None;
    Controller m_custody;
    BoardOwnerPersistence& m_persistence;
    BoardOwnerRecord m_record = BoardOwnerRecord::Invalid;
    Connection m_claim{};
    Connection m_owner{};
    uint16_t m_attempt = 0U;
    uint8_t m_end = 0U;
    uint8_t m_promptEnd = 0U;
    uint32_t m_startedMs = 0U;
    uint32_t m_holdStartedMs = 0U;
    bool m_down[2][2]{};
    bool m_fresh[2][2]{};
    bool m_freshPress[2][2]{};
    bool m_identityDurable = false;
    bool m_claimValid = false;
    bool m_phoneInitiatesSecurity = false;
    bool m_localClear = false;
    bool m_localClearAlreadyAbsent = false;
    bool m_holdingClear = false;
    bool m_physicalButtonTicksAllowed = true;
    bool m_physicalButtonTickTimeValid = false;
    uint32_t m_physicalButtonTickMs = 0U;
    bool m_consumeTail = false;
    bool m_needsDisconnect = false;
    bool m_clearContextSelected = false;
    uint8_t m_clearContextEnd = 0U;
    uint16_t m_clearContextAttempt = 0U;
    bool m_restartMessageVisible = false;
    bool m_closeLocalMenuOnSuccess = false;
    bool m_clearFeedbackActive = false;
    bool m_clearFeedbackSucceeded = false;
    uint32_t m_clearFeedbackStartedMs = 0U;
    uint32_t m_clearFeedbackSucceededMs = 0U;
    PhysicalCue m_physicalCue = PhysicalCue::None;
    PhysicalCue m_previousPhysicalCue = PhysicalCue::None;
    bool m_consentAcknowledgmentActive = false;
    uint32_t m_consentAcknowledgmentStartedMs = 0U;
    uint32_t m_physicalCueStartedMs = 0U;
    uint32_t m_previousPhysicalCueStartedMs = 0U;
    // Never reused within this runtime, including Boot reconciliation and
    // repeated client attempts on the same shared session.
    uint32_t m_consentPrompt = 0U;
};

} // namespace modules
