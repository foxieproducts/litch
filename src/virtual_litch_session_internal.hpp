#pragma once
#include "modules/physical_palette_shortcut.hpp"
#include "modules/board_style_preview.hpp"

#include "modules/board_led_frame.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/ble_identify_presentation.hpp"
#include "modules/water_slosh_session.hpp"
#include "hardware/lis2dw12.hpp"

#ifdef LITCH_NATIVE

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "firmware/local_control_requests.hpp"
#include "firmware/hardware_test_mode.hpp"
#include "firmware/sleep_transition.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/startup_piece_accent.hpp"
#include "modules/boot_display.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/game_reset_hold_policy.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/hint_button_policy.hpp"
#include "modules/home_state_notification.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/menu_level_overlay.hpp"
#include "modules/orientation.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/reset_hold_gesture_controller.hpp"
#include "modules/settings_store.hpp"
#include "runtime/virtual_litch_session.h"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/runtime_health.hpp"

namespace hardware { class RmtFlashArbiter; }

namespace runtime::virtual_litch_internal {

// Native profile selection reuses production topology and capability metadata.
inline hardware::BoardHardwareProfile HardwareProfile(const virtual_litch_config& config) {
    const auto revision = config.hardware == VIRTUAL_LITCH_HARDWARE_C3
        ? hardware::BoardRevision::V1C3
        : config.hardware == VIRTUAL_LITCH_HARDWARE_V3_3
            ? hardware::BoardRevision::V3_3S3
        : config.hardware == VIRTUAL_LITCH_HARDWARE_V3
            ? hardware::BoardRevision::V3S3 : hardware::BoardRevision::V2S3;
    return hardware::BoardHardwareProfileFor(revision);
}

inline uint16_t PhysicalLedCount(const virtual_litch_config& config) {
    const auto leds = HardwareProfile(config).leds;
    return static_cast<uint16_t>(leds.physicalBoardLedCount + leds.physicalStatusLedCount);
}

enum class SessionMode : uint8_t {
    ReducerOnly = 0,
    EngineBackedJourney,
    FactoryJourney,
};

class CaptureLedRenderer final : public modules::ILedRenderer {
  public:
    CaptureLedRenderer(uint16_t ledCount, bool rgbw);

    uint16_t GetLedCount() const override { return m_ledCount; }
    void Clear() override;
    void SetLedRgb(uint16_t ledIndex,
                   uint8_t r,
                   uint8_t g,
                   uint8_t b) override;
    void Show() override {}
    void SetRgbwWhiteMixScale256(const uint16_t scale256) {
        m_rgbwWhiteMixScale256 = scale256 > 256U ? 256U : scale256;
    }

    const std::array<virtual_litch_color,
                     VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT>&
    Pixels() const {
        return m_pixels;
    }

  private:
    uint16_t m_ledCount = 0U;
    bool m_rgbw = false;
    uint16_t m_rgbwWhiteMixScale256 = 256U;
    std::array<virtual_litch_color,
               VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT>
        m_pixels{};
};

class Session final {
  public:
    explicit Session(const virtual_litch_config& config,
                     SessionMode mode = SessionMode::ReducerOnly,
                     const char* settingsPath = nullptr,
                     modules::IEngine* engineOverride = nullptr,
                     modules::AsyncThinkWorker* asyncThinkWorkerOverride = nullptr);

    virtual_litch_result Reset();
    bool IsFactoryBootOnly() const { return m_mode == SessionMode::FactoryJourney; }
    bool HasNormalRuntimeOwners() const {
        return m_engine != nullptr || m_gamePersistence != nullptr ||
            m_dispatcher != nullptr || m_controlMutation.IsConfigured();
    }
    void ObserveBootHallMask(uint64_t logicalOccupiedMask);
    virtual_litch_result DispatchPosition(
        const virtual_litch_position& position);
    virtual_litch_result DispatchBoardObservation(
        uint64_t changedMask,
        uint64_t occupiedMask);
    virtual_litch_result DispatchSensorDelta(uint8_t square,
                                                bool nowOccupied);
    bool LoadExternalFen(const char* fen);
    virtual_litch_result DispatchButton(uint8_t owner,
                                           uint8_t button,
                                           uint8_t event);
    // For the production adapter after it validates the fitted physical bank
    // and maps that bank to its current logical owner.
    virtual_litch_result DispatchNormalizedButton(uint8_t owner,
                                                  uint8_t button,
                                                  uint8_t event);
    // Match the GPIO owner boundary before the adapter reseeds held buttons.
    void ResetQuickSettingsInput() {
        m_quickSettingsOverlayController.Reset();
        m_hintButtonController.Reset();
        m_resetHoldController.Reset();
        m_gameResetHoldActive = false;
        m_gameResetHoldTriggered = false;
        m_controlFeedback.Reset();
        if (m_gamePersistence) m_gamePersistence->Litchcraft().CancelEntryHold();
        if (m_ownerPairing) m_ownerPairing->ReseedPhysicalInput();
        if (m_gamePersistence) m_gamePersistence->ResetKingResignationInput();
    }
    virtual_litch_result StepUntil(uint32_t targetTimeMs);
    virtual_litch_result Advance(uint32_t deltaMs);
    void ObserveMotionSample(const hardware::Lis2dw12Sample& sample);
    void ObserveWaterAmbientSample(float lux, bool available);
    bool WaterSloshActive() const { return m_waterSlosh.IsActive(); }
    void SendLitchcraftPacket(const uint8_t* bytes, size_t size);
    void ShowIdentifyOverlay(uint8_t number, modules::Rgb color);
    void StopIdentifyOverlay();
    void RunIdentifyIngress(void (*callback)(void*, modules::BleIdentifyPresentation&, uint32_t),
                            void* context);
    modules::LitchcraftSession& Litchcraft() { return m_gamePersistence->Litchcraft(); }
    modules::LitchcraftPersistence& ArtPersistence() { return m_gamePersistence->ArtPersistence(); }

    uint64_t PhysicalOccupancyMask() const;
    uint32_t NowMs() const { return m_nowMs; }
    const virtual_litch_state& State() const { return m_current.state; }
    const virtual_litch_semantic_frame& SemanticFrame() const {
        return m_current.semantic;
    }
    const virtual_litch_physical_frame& PhysicalFrame() const {
        return m_current.physical;
    }
    uint64_t SidePrioritySquareMask() const { return m_sidePrioritySquareMask; }
    size_t HistoryCount() const { return m_historyCount; }
    bool TryGetHistory(size_t chronologicalIndex,
                       virtual_litch_history_frame& out) const;
    const virtual_litch_sleep_lifecycle& SleepLifecycle() const {
        return m_sleepLifecycle;
    }
    virtual_litch_result RequestSleep(uint8_t origin);
    virtual_litch_result DispatchValidatedMotionOrTap();
    virtual_litch_result ResolveSleepPreflights(bool persistenceSucceeded,
                                                   bool wakeSourceSucceeded);
    virtual_litch_result SimulateSleepWake();
    virtual_litch_result ReportWakeButtonsReleased();
    virtual_litch_result MutateSetting(
        const virtual_litch_setting_request& request,
        modules::ControlMutationOutcome& out);
    virtual_litch_result DispatchControlAction(
        const virtual_litch_action_request& request,
        modules::ControlMutationOutcome& out);
    virtual_litch_result DispatchRuntimeHealthFact(
        const virtual_litch_runtime_health_fact& fact);
    bool TryTakeControlCompletion(modules::ControlMutationOutcome& out);
    bool TryGetLastBoardControlOutcome(
        modules::ControlMutationOutcome& out) const;
    bool TryGetHintMoveSquares(uint8_t& outFromSquare,
                               uint8_t& outToSquare) const {
        return m_stateMachine.TryGetHintMoveSquares(
            outFromSquare, outToSquare);
    }
    bool TryGetCoachInsight(modules::CoachInsight& outInsight) const {
        return m_gamePersistence != nullptr &&
               m_gamePersistence->TryGetCurrentCoachInsight(outInsight);
    }
    bool TryGetHintDuckMoveSquares(uint8_t& outFromSquare,
                                   uint8_t& outToSquare) const {
        return m_stateMachine.TryGetHintDuckMoveSquares(
            outFromSquare, outToSquare);
    }
    const virtual_litch_runtime_health& RuntimeHealth() const {
        return m_runtimeHealthCapture;
    }
    uint8_t UndoHistoryMoveCount() const {
        return m_gamePersistence == nullptr
                   ? 0U : m_gamePersistence->GetUndoHistoryMoveCount();
    }
    const runtime::GameClock* GameClock() const {
        return m_gamePersistence == nullptr
                   ? nullptr
                   : &m_gamePersistence->GetGameClock();
    }
    bool IsUndoActionPending() const {
        return m_gamePersistence != nullptr &&
               m_gamePersistence->IsUndoActionPending();
    }
    modules::GameVariant GameVariant() const {
        return m_gamePersistence == nullptr
                   ? modules::GameVariant::Classic
                   : m_gamePersistence->GetGameVariant();
    }
    uint8_t DuckSquare() const {
        return m_gamePersistence == nullptr
                   ? modules::INVALID_DUCK_SQUARE
                   : m_gamePersistence->GetDuckSquare();
    }
    uint8_t SetupDuckCandidateSquare() const {
        return m_gamePersistence == nullptr
                   ? modules::INVALID_DUCK_SQUARE
                   : m_gamePersistence->GetInitialDuckCandidateSquare();
    }
    void CaptureStartupFrame(uint32_t nowMs, uint16_t fadeScale256);
    void BeginStartupPieceAccent();
    void BeginBootDisplay();
    void ObserveBootArtworkRecovery(modules::litchcraft_frame_store::Status status) {
        m_bootArtworkUnavailable = modules::BootArtworkUnavailableAfterRecovery(status);
    }
    void BindLedOutputArbiter(hardware::RmtFlashArbiter& arbiter) { m_ledOutputArbiter = &arbiter; }
    void RunManufacturingIngress(
        void (*callback)(void*, firmware::HardwareTestMode&, uint32_t), void* context);
    void SetManufacturingHallSnapshot(bool valid, uint64_t occupiedMask);
    const firmware::HardwareTestMode& ManufacturingTest() const { return m_hardwareTest; }
    bool CpuThinkingVisible() const { return m_cpuThinkingVisible; }
    modules::HomeStateNotification HomeStateSnapshot() const;
    modules::HomeStateNotificationLine HomeStateLine() const;
    bool CanPublishHomeState() const;
    modules::BoardHealthLine BoardHealthStateLine() const;
    modules::BoardHealthConnectionState BoardHealthConnectionState() const;
    // Native transport wiring executes at the session's authoritative clock.
    // The callback receives existing owners, never a substitute controller.
    void SetBenchmarkRawControlsIdleProbe(void* context, bool (*probe)(void*)) {
        m_benchmarkRawControlsContext = context;
        m_benchmarkRawControlsIdle = probe;
    }
    void RunControlIngress(
        void (*callback)(void*, modules::SettingsStore&,
                         modules::ControlMutationService&, modules::GamePersistenceCoordinator&),
        void* context);
    bool TryPrepareHomePublication(
        modules::HomeStateNotificationPublisher& publisher,
        bool force, modules::HomeStateNotificationLine& outLine) const;
    modules::SettingsStore& SettingsForPersistenceTest() { return m_settings; }
    bool SimulateBootPersistenceFailure(
        modules::PersistenceDegradation degradation);
    modules::PersistenceDegradation PersistenceDegradation() const {
        return m_persistenceDegradation;
    }
    bool BoardOverlayActive() const;
    modules::EdgeLedCueIntent EdgeCueIntent() const {
        return m_edgeCue.Current(m_nowMs);
    }
    bool QuickSettingsActive() const {
        return m_quickSettingsOverlayActive;
    }
    void BindOwnerPairingRuntime(modules::BoardOwnerPairingRuntime& runtime) {
        m_ownerPairing = &runtime;
    }
    uint8_t QuickSettingsOwner() const {
        return static_cast<uint8_t>(m_quickSettingsOverlayOwner);
    }
    uint8_t QuickSettingsSelectedIndex() const {
        return m_quickSettingsOverlaySelectedIndex;
    }
    bool FirstPlayerIsP2() const {
        return m_settings.ReadValue(
                   "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
    }
    bool BoardRotate() const {
        return m_settings.ReadValue("BOARD_ROTATE", false);
    }
    bool PlacementModeActive() const {
        return m_stateMachine.IsPlacementModeActive();
    }
    bool ControlBanksSwapped() const {
        return !m_hardwareTest.IsActive() && modules::ComputeControlBankSwap(
            BoardRotate(), FirstPlayerIsP2(),
            m_gamePersistence && m_gamePersistence->Litchcraft().IsActive());
    }
    bool HallRotate() const {
        return !m_hardwareTest.IsActive() && modules::ComputeSquareRotate(
            BoardRotate(), FirstPlayerIsP2(),
            m_gamePersistence && m_gamePersistence->Litchcraft().IsActive());
    }
    bool KingQueenSwapActive() const {
        return m_stateMachine.IsKingQueenSwapPromptActive();
    }
    bool PromotionActive() const {
        return m_stateMachine.IsPromotionActive();
    }
    modules::PromotionPiece PromotionSelection() const {
        return m_stateMachine.GetPromotionSelection();
    }
    bool HintDisplayRequested() const {
        return m_stateMachine.IsHintDisplayRequested();
    }
    bool IsC3() const {
        return m_config.hardware == VIRTUAL_LITCH_HARDWARE_C3;
    }
    std::string_view DisplayText() const {
        if (m_hardwareTest.IsActive()) { return {}; }
        const std::string_view text = m_menuOverlay.Test_GetLastText();
        return text.empty() ? m_displayLabel.View() : text;
    }

  private:
    void SyncOwnerPairingContext();
    modules::BoardOwnerPairingRuntime* m_ownerPairing = nullptr;
    virtual_litch_result DispatchNormalizedButtonAt(
        uint8_t owner, uint8_t button, uint8_t event, uint32_t eventTimeMs,
        bool litchcraftEntryArbitrated);
    void CaptureFrame();
    void CaptureWaterSlosh(const modules::led_frame_composer::ComposeInputs& inputs);
    void CaptureManufacturingFrame();
    void ObserveManufacturingBoard(uint64_t occupiedMask);
    void CaptureSemanticFrame();
    void CapturePhysicalFrame(uint16_t scaleOverride = UINT16_MAX,
                              modules::BudgetAwareLedPipeline* startupPipeline = nullptr,
                              bool initialStoreModePending = false);
    void CaptureState();
    void CaptureRuntimeHealth();
    void CaptureSleepLifecycle();
    void AppendHistory();
    void SyncBeginnerModePolicy();
    void ComposeControlFeedback(const modules::led_frame_composer::ComposeInputs& presentation);
    void UpdateProductionCoordinator();
    void RecordBoardControlOutcome(
        const modules::ControlMutationOutcome& outcome,
        bool showFeedback = true);
    void OpenQuickSettings(modules::UiOwner owner);
    void CloseQuickSettings();
    void ShowQuickSettingsSelection();
    void HandleQuickSettingsAction(
        const modules::QuickSettingsAction& action);
    SetupLifecycle CurrentSetupLifecycle() const;
    bool ConsumeSleepForLocalActivity(
        firmware::SleepTransitionCause cause);
    static modules::ControlCoordinatorResult CheckStartOnly(
        void* context,
        const char* canonicalKey);
    static modules::ControlCoordinatorResult RequestHint(void* context,
                                                         uint16_t requestId);
    static modules::ControlCoordinatorResult RequestUndo(void* context,
                                                         uint16_t requestId,
                                                         uint8_t moveCount);
    static modules::ControlCoordinatorResult RequestNewGame(void* context,
                                                            uint16_t requestId);
    static modules::ControlCoordinatorResult PollControlAction(
        void* context,
        uint16_t requestId);
    static void OnCoachingChanged(void* context,
                                   modules::UiOwner owner,
                                   bool enabled);

    virtual_litch_config m_config{};
    SessionMode m_mode = SessionMode::ReducerOnly;
    firmware::HardwareTestMode m_hardwareTest;
    uint64_t m_manufacturingOccupancy = 0ULL;
    bool m_manufacturingHallValid = true;
    modules::SettingsStore m_settings;
    bool m_loadPersistedSettings = false;
    modules::PersistenceDegradation m_persistenceDegradation =
        modules::PersistenceDegradation::None;
    GameStateMachine m_stateMachine;
    AppEventQueue m_eventQueue;
    runtime::RuntimeHealthReducer m_runtimeHealth;
    std::unique_ptr<modules::LitchMaxEngine> m_ownedEngine;
    modules::IEngine* m_engine = nullptr;
    modules::AsyncThinkWorker* m_asyncThinkWorkerOverride = nullptr;
    std::unique_ptr<modules::GamePersistenceCoordinator> m_gamePersistence;
    std::unique_ptr<InteractionDispatcher> m_dispatcher;
    modules::BeginnerModeController m_beginnerMode{m_settings};
    bool m_beginnerModeForcedHintDisplay = false;
    modules::ControlMutationService m_controlMutation;
    firmware_internal::LocalControlRequestTracker m_localControlRequests;
    void* m_benchmarkRawControlsContext = nullptr;
    bool (*m_benchmarkRawControlsIdle)(void*) = nullptr;
    modules::physical_palette_shortcut::EditPreview m_paletteEditPreview;
    modules::BoardStylePreview m_boardStylePreview;
    modules::HintButtonGestureController m_hintButtonController;
    modules::ControlLedFeedback m_controlFeedback;
    modules::QuickSettingsOverlayController m_quickSettingsOverlayController;
    modules::ResetHoldGestureController m_resetHoldController{
        modules::ResetHoldGestureControllerConfig{2000U}};
    bool m_gameResetHoldActive = false;
    bool m_gameResetHoldTriggered = false;
    bool m_gameResetHoldRejected = false;
    uint32_t m_gameResetHoldStartMs = 0U;
    modules::UiOwner m_gameResetHoldOwner = modules::UiOwner::None;
    bool m_quickSettingsOverlayActive = false;
    fxe::ElapsedTime m_quickSettingsOverlayIdleElapsed;
    modules::UiOwner m_quickSettingsOverlayOwner = modules::UiOwner::None;
    uint8_t m_quickSettingsOverlayPhysicalEnd = 0U;
    uint8_t m_quickSettingsOverlaySelectedIndex =
        modules::quick_settings_items::CPU_DIFFICULTY;
    modules::MenuLevelOverlay m_menuOverlay;
    modules::BleIdentifyPresentation m_bleIdentify;
    bool m_identifyOverlayActive = false;
    fxe::FixedString<8> m_displayLabel;
    modules::EdgeLedCueController m_edgeCue;
    modules::ControlMutationOutcome m_lastBoardControlOutcome{};
    bool m_lastBoardControlOutcomeValid = false;
    modules::EngineDerivedState m_derived;
    CaptureLedRenderer m_renderer;
    modules::litchcraft_physical_presentation::ModeCrossfade<
        hardware::MAX_PHYSICAL_LED_COUNT> m_litchcraftCrossfade;
    modules::PowerBudgetPolicy m_powerPolicy;
    modules::BudgetAwareLedPipeline m_pipeline;
    hardware::RmtFlashArbiter* m_ledOutputArbiter = nullptr;
    bool m_bootArtworkUnavailable = true; // Ordinary native fixtures have completed recovery.
    firmware::SleepTransition m_sleep;
    SetupLifecycle m_setupLifecycle = SetupLifecycle::Gameplay;
    modules::WaterSloshSession m_waterSlosh;
    hardware::Lis2dw12Sample m_waterSample{};
    bool m_waterSamplePending = false;
    float m_waterAmbientLux = 0.0F;
    bool m_waterAmbientAvailable = false;
    uint32_t m_nowMs = 0U;
    uint32_t m_boardObservationSequence = 0U;
    uint32_t m_setupPresentationObservationSequence = 0U;
    bool m_setupPresentationObservationValid = false;
    uint32_t m_frameSequence = 0U;
    bool m_cpuThinkingVisible = false;
    uint64_t m_sidePrioritySquareMask = 0ULL;
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay
        m_fixedPhysicalStatusOverlay{};
    uint64_t m_ordinaryPieceMask = 0ULL;
    uint64_t m_captureRemovalMask = 0ULL;
    uint64_t m_duckReminderMask = 0ULL;
    uint16_t m_startupPieceScale256 = 256U;
    modules::StartupPieceAccent m_startupPieceAccent;
    modules::BootDisplay m_bootDisplay;
    InteractionModelSnapshot m_snapshot{};
    modules::BoardLedFrame m_semanticLedFrame{};
    modules::GridForegroundLayer m_menuForeground{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> m_statusFrame{};
    std::array<modules::Rgb, VIRTUAL_LITCH_SQUARE_COUNT> m_gridFrame{};
    std::array<modules::Rgb, VIRTUAL_LITCH_SQUARE_COUNT>
        m_gridFrameBeforeMenuOverlay{};
    virtual_litch_history_frame m_current{};
    virtual_litch_runtime_health m_runtimeHealthCapture{};
    virtual_litch_sleep_lifecycle m_sleepLifecycle{};
    std::vector<virtual_litch_history_frame> m_history;
    size_t m_historyCount = 0U;
    size_t m_historyWriteIndex = 0U;
};

bool IsValidConfig(const virtual_litch_config& config);
virtual_litch_config DefaultConfig();

} // namespace runtime::virtual_litch_internal

#endif
