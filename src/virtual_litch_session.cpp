#include "virtual_litch_session_internal.hpp"

#ifdef LITCH_NATIVE

#include <ArduinoJson.h>

#include <cstring>
#include <limits>

#include "foxie_embedded/millis.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/beginner_mode_policy.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/physical_palette_shortcut.hpp"
#include "modules/control_led_feedback_projection.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/home_state_snapshot.hpp"
#include "modules/move_confirm_setting.hpp"
#include "modules/game_persistence_scrub.hpp"
#include "modules/settings_registry.hpp"
#include "modules/vs_cpu_turn.hpp"

namespace runtime::virtual_litch_internal {
namespace {

constexpr uint16_t DEFAULT_HISTORY_CAPACITY = 32U;
constexpr uint16_t MAX_HISTORY_CAPACITY = 256U;
constexpr uint16_t C3_LED_CURRENT_LIMIT_MA =
    modules::S3_MAX_LED_CURRENT_MA;
constexpr uint64_t INITIAL_CHESS_OCCUPANCY = 0xFFFF00000000FFFFULL;
constexpr uint64_t INITIAL_WHITE_OCCUPANCY = 0xFFFF000000000000ULL;
constexpr uint64_t INITIAL_BLACK_OCCUPANCY = 0x000000000000FFFFULL;
constexpr uint64_t INITIAL_KING_MASK = (1ULL << 4U) | (1ULL << 60U);
constexpr uint64_t INITIAL_ROOK_MASK = (1ULL << 0U) | (1ULL << 7U) |
                                       (1ULL << 56U) | (1ULL << 63U);
constexpr uint64_t INITIAL_PAWN_MASK = (0xFFULL << 8U) | (0xFFULL << 48U);

class SessionTimeScope final {
  public:
    explicit SessionTimeScope(const uint32_t nowMs)
        : m_override(&fxe::ManualMillisProvider) {
        fxe::ManualMillis::SetNowMs(nowMs);
    }

  private:
    fxe::ScopedMillisProviderOverride m_override;
};

modules::SettingsStoreConfig MakeSettingsConfig(const char* settingsPath) {
    modules::SettingsStoreConfig config;
    config.path = settingsPath != nullptr ? settingsPath : "virtual_litch_session_unused.json";
    config.migrateFunc = [](JsonDocument& document) {
        return modules::settings_registry::CanonicalizeLoadedSettings(document);
    };
    config.scrubFunc = [](JsonDocument& document, fxe::SettingsRemovedKeys* removed) {
        const bool gameChanged = modules::ScrubGamePersistenceSettings(document, removed);
        const bool artChanged = modules::ScrubLitchcraftRecord(document, removed);
        return gameChanged || artChanged;
    };
    return config;
}

modules::PowerBudgetPolicyConfig MakePowerPolicyConfig(
    const virtual_litch_config& config) {
    modules::PowerBudgetPolicyConfig policy;
    if (config.hardware == VIRTUAL_LITCH_HARDWARE_C3) {
        const uint16_t limit = config.max_led_current_ma == 0U
                                   ? C3_LED_CURRENT_LIMIT_MA
                                   : config.max_led_current_ma;
        policy.maxLedCurrentMa = limit;
        policy.maxTotalCurrentMa = limit;
        return policy;
    }

    const modules::LedPowerEnvelope envelope =
        modules::SelectS3LedPowerEnvelope(
            config.recovery_power_limit != 0U);
    policy.maxLedCurrentMa = config.max_led_current_ma == 0U
                                 ? envelope.maxLedCurrentMa
                                 : config.max_led_current_ma;
    policy.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    policy.maxTotalCurrentMa = config.max_led_current_ma == 0U
                                   ? envelope.maxTotalCurrentMa
                                   : static_cast<uint16_t>(
                                         policy.fixedSystemCurrentMa +
                                         policy.maxLedCurrentMa);
    return policy;
}

modules::BudgetAwareLedPipelineConfig MakePipelineConfig(
    const virtual_litch_config& config) {
    modules::BudgetAwareLedPipelineConfig pipeline;
    pipeline.outputBrightnessScale256 = HardwareProfile(config).ledOutputScale256;
    pipeline.channelMaxCurrentMa = modules::S3_CHANNEL_MAX_CURRENT_MA;
    if (!HardwareProfile(config).isS3) {
        return pipeline;
    }

    if (HardwareProfile(config).revision == hardware::BoardRevision::V3_3S3) {
        pipeline.channelMaxCurrentMa = 20U;
        pipeline.redChannelMaxCurrentMaTimes100 = 2000U;
        pipeline.greenChannelMaxCurrentMaTimes100 = 2000U;
        pipeline.blueChannelMaxCurrentMaTimes100 = 2000U;
        return pipeline;
    }

    pipeline.enableRgbwAutoMapping = true;
    pipeline.enableS3OpticalColorCalibration = true;
    pipeline.rgbwCoolToWarmThreshold = 32U;
    pipeline.redChannelMaxCurrentMaTimes100 =
        modules::S3_RED_CURRENT_MA_TIMES100;
    pipeline.greenChannelMaxCurrentMaTimes100 =
        modules::S3_GREEN_CURRENT_MA_TIMES100;
    pipeline.blueChannelMaxCurrentMaTimes100 =
        modules::S3_BLUE_CURRENT_MA_TIMES100;
    pipeline.whiteChannelMaxCurrentMaTimes100 =
        modules::S3_WHITE_CURRENT_MA_TIMES100;
    return pipeline;
}

bool IsBooleanByte(const uint8_t value) {
    return value <= 1U;
}

bool HasValidBooleanFields(const virtual_litch_config& config) {
    return IsBooleanByte(config.board_rotate) &&
           IsBooleanByte(config.square_rotate_for_output) &&
           IsBooleanByte(config.first_player_is_p2) &&
           IsBooleanByte(config.flip_sides) &&
           IsBooleanByte(config.strong_power_source) &&
           IsBooleanByte(config.recovery_power_limit);
}

bool IsValidButtonOwner(const uint8_t owner) {
    return owner <= VIRTUAL_LITCH_OWNER_P2;
}

bool IsP2Button(const uint8_t button) {
    return button == VIRTUAL_LITCH_BUTTON_P2_X ||
           button == VIRTUAL_LITCH_BUTTON_P2_O;
}

bool AllowsBoardIngress(const runtime::RuntimeHealthSnapshot& health) {
    return health.advice.boardInputAllowed &&
           health.advice.moveAcceptanceAllowed;
}

static_assert(static_cast<uint8_t>(runtime::RuntimeHealthState::Healthy) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthState::DegradedUsable) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_DEGRADED_USABLE);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthState::InputPaused) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthState::ProtectionLatched) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_PROTECTION_LATCHED);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::OneAmbientSensor) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_ONE_AMBIENT_SENSOR);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::NoAmbientSensors) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_NO_AMBIENT_SENSORS);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::BoardScanUncertain) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BOARD_SCAN_UNCERTAIN);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::SourceUnstable) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_SOURCE_UNSTABLE);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::BrownoutLatched) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_BROWNOUT_LATCHED);
static_assert(runtime::RuntimeHealthCauseMask(
                  runtime::RuntimeHealthCause::SevereProtectionLatched) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_CAUSE_SEVERE_PROTECTION_LATCHED);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::BoardScan) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::AmbientSensorCount) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_AMBIENT_SENSOR_COUNT);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::PowerSource) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_POWER_SOURCE);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::PowerEnvelope) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_POWER_ENVELOPE);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::Brownout) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BROWNOUT);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::ResetLatchedProtection) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_RESET_LATCHED_PROTECTION);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthFactKind::LedBudget) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_LED_BUDGET);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthBoardScan::Stable) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthBoardScan::Uncertain) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthSource::Stable) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_STABLE);
static_assert(static_cast<uint8_t>(runtime::RuntimeHealthSource::Unstable) ==
              VIRTUAL_LITCH_RUNTIME_HEALTH_SOURCE_UNSTABLE);

} // namespace

virtual_litch_config DefaultConfig() {
    virtual_litch_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = VIRTUAL_LITCH_ABI_VERSION;
    config.history_capacity = DEFAULT_HISTORY_CAPACITY;
    config.brightness_scale_256 = 256U;
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    config.strong_power_source = 1U;
    return config;
}

bool IsValidConfig(const virtual_litch_config& config) {
    const bool hardwareValid =
        config.hardware == VIRTUAL_LITCH_HARDWARE_C3 ||
        config.hardware == VIRTUAL_LITCH_HARDWARE_S3 ||
        config.hardware == VIRTUAL_LITCH_HARDWARE_V3 ||
        config.hardware == VIRTUAL_LITCH_HARDWARE_V3_3;
    const bool s3OverrideFits =
        !HardwareProfile(config).isS3 ||
        config.max_led_current_ma == 0U ||
        config.max_led_current_ma <=
            std::numeric_limits<uint16_t>::max() -
                modules::S3_FIXED_SYSTEM_CURRENT_MA;
    return config.struct_size == sizeof(config) &&
           config.abi_version == VIRTUAL_LITCH_ABI_VERSION &&
           hardwareValid && HasValidBooleanFields(config) &&
           config.history_capacity <= MAX_HISTORY_CAPACITY &&
           config.brightness_scale_256 <= 256U && s3OverrideFits &&
           config.reserved == 0U;
}

Session::Session(
    const virtual_litch_config& config,
    const SessionMode mode,
    const char* settingsPath,
    modules::IEngine* const engineOverride,
    modules::AsyncThinkWorker* const asyncThinkWorkerOverride)
    : m_config(config),
      m_mode(mode),
      m_settings(MakeSettingsConfig(settingsPath)),
      m_loadPersistedSettings(settingsPath != nullptr),
      m_stateMachine(),
      m_eventQueue(),
      m_renderer(PhysicalLedCount(config),
                 HardwareProfile(config).isS3 &&
                     HardwareProfile(config).revision !=
                         hardware::BoardRevision::V3_3S3),
      m_powerPolicy(MakePowerPolicyConfig(config)),
      m_pipeline(m_renderer, m_powerPolicy, MakePipelineConfig(config)),
      m_history(config.history_capacity) {
    if (m_mode == SessionMode::EngineBackedJourney) {
        m_asyncThinkWorkerOverride = asyncThinkWorkerOverride;
        if (engineOverride != nullptr) {
            m_engine = engineOverride;
        } else {
            m_ownedEngine = std::make_unique<modules::LitchMaxEngine>();
            m_engine = m_ownedEngine.get();
        }
    } else if (m_mode != SessionMode::FactoryJourney) {
        m_dispatcher = std::make_unique<InteractionDispatcher>(
            m_stateMachine, m_eventQueue);
    }
    if (m_mode == SessionMode::FactoryJourney) {
        (void)Reset();
        return;
    }
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = this;
    callbacks.allowVolatileUserSettings = true;
    callbacks.beforeSettingMutation = [](void* context, const char* key) {
        static_cast<Session*>(context)->m_bootDisplay.Cancel();
        if (std::strcmp(key, "VS_CPU_LEVEL") == 0 ||
            std::strcmp(key, "VS_CPU_PLAYER") == 0) {
            auto& gestures = static_cast<Session*>(context)->m_hintButtonController;
            gestures.Cancel(modules::UiOwner::P1);
            gestures.Cancel(modules::UiOwner::P2);
        }
    };
    callbacks.settingProjected = [](void* context, const char* key) {
        auto& self = *static_cast<Session*>(context);
        self.m_boardStylePreview.SettingAccepted(self.m_settings, key);
        const std::string_view canonicalKey = key != nullptr ? key : "";
        if (canonicalKey == "VS_CPU_LEVEL" && self.m_quickSettingsOverlayActive &&
            self.m_quickSettingsOverlaySelectedIndex == modules::quick_settings_items::CPU_DIFFICULTY) {
            modules::RefreshQuickSettingsCpuDifficulty(
                self.m_settings, self.m_menuOverlay, self.m_gridFrameBeforeMenuOverlay);
        }
        if (canonicalKey == "MINIMAL_ANIMATIONS" &&
            self.m_settings.ReadValue("MINIMAL_ANIMATIONS", false)) {
            self.m_stateMachine.CancelHintRefinement();
        }
        if (canonicalKey == "BOARD_ROTATE" || canonicalKey == "GAME_FIRST_PLAYER")
            self.ResetQuickSettingsInput();
    };
    callbacks.checkStartOnlyGate = &Session::CheckStartOnly;
    callbacks.cancelCpuWork = [](void* context) {
        auto& self = *static_cast<Session*>(context);
        self.m_bootDisplay.Cancel();
        if (self.m_gamePersistence != nullptr) {
            self.m_gamePersistence->RequestCancelCpuWork();
        }
    };
    callbacks.cancelHintWorkAndRestart = [](void* context) {
        auto& self = *static_cast<Session*>(context);
        self.m_bootDisplay.Cancel();
        if (self.m_gamePersistence != nullptr) {
            self.m_gamePersistence->RequestCancelHintWorkAndRestart();
        }
    };
    callbacks.requestHint = &Session::RequestHint;
    callbacks.requestUndo = &Session::RequestUndo;
    callbacks.requestNewGame = &Session::RequestNewGame;
    callbacks.pollAction = &Session::PollControlAction;
    callbacks.getActionState = [](void* context) {
        const auto& self = *static_cast<Session*>(context);
        return self.IsUndoActionPending()
                   ? modules::ControlActionState::UndoPending
                   : modules::ControlActionState::Idle;
    };
    callbacks.coachingChanged = &Session::OnCoachingChanged;
    m_controlMutation.Configure(m_settings, callbacks);
    m_controlMutation.SetActionCompletedCallback([](void* context, const modules::ControlMutationOutcome& outcome,
        modules::UiOwner owner, modules::ControlActionState initialState) {
        auto& self = *static_cast<Session*>(context);
        modules::ObserveControlLedActionFeedback(self.m_controlFeedback, outcome, owner, initialState);
    });

    (void)Reset();
}

virtual_litch_result Session::Reset() {
    m_controlFeedback.Reset();
    m_paletteEditPreview.SetFeedback(&m_controlFeedback);
    m_waterSlosh = {};
    m_boardStylePreview.Cancel();
    m_waterAmbientLux = 0.0F;
    m_waterAmbientAvailable = false;
    m_waterSamplePending = false;
    m_startupPieceAccent.Cancel();
    m_ordinaryPieceMask = 0ULL;
    m_startupPieceScale256 = 256U;
    m_hardwareTest.Stop();
    m_manufacturingOccupancy = 0ULL;
    m_manufacturingHallValid = true;
    m_nowMs = m_config.initial_time_ms;
    SessionTimeScope timeScope(m_nowMs);
    if (m_mode == SessionMode::FactoryJourney) {
        if (m_loadPersistedSettings) (void)m_settings.Load();
        m_hardwareTest.Enter(HardwareProfile(m_config).revision, m_nowMs);
        m_powerPolicy.Reset();
        m_frameSequence = m_historyCount = m_historyWriteIndex = 0U;
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    m_stateMachine.Reset();
    m_eventQueue.Reset();
    m_runtimeHealth = runtime::RuntimeHealthReducer{};
    m_persistenceDegradation = modules::PersistenceDegradation::None;
    if (m_mode == SessionMode::EngineBackedJourney) {
        if (m_loadPersistedSettings) {
            m_persistenceDegradation = modules::QuarantineUnavailableSettings(
                m_settings, m_settings.Load());
        } else {
        m_settings.Doc().clear();
        m_settings.Doc()["VS_CPU_LEVEL"] = 0U;
        m_settings.Doc()["TIMER"] = 0U;
        m_settings.Doc()["GAME_FIRST_PLAYER"] =
            m_config.first_player_is_p2 != 0U ? 2U : 1U;
        m_settings.Doc()["BOARD_ROTATE"] =
            m_config.board_rotate != 0U;
        }
        m_gamePersistence =
            std::make_unique<modules::GamePersistenceCoordinator>(
                m_settings,
                *m_engine,
                m_stateMachine,
                &m_eventQueue,
                m_asyncThinkWorkerOverride);
        m_gamePersistence->Litchcraft().SetControlFeedback(&m_controlFeedback);
        m_gamePersistence->SetBenchmarkControlsIdleProbe(this, [](void* context) {
            const auto& self = *static_cast<Session*>(context);
            modules::UiOwner owner = modules::UiOwner::None;
            uint8_t progress = 0U;
            return self.m_benchmarkRawControlsIdle != nullptr &&
                self.m_benchmarkRawControlsIdle(self.m_benchmarkRawControlsContext) &&
                !self.m_hardwareTest.IsActive() && !self.m_gameResetHoldActive &&
                !self.m_quickSettingsOverlayActive &&
                !self.m_quickSettingsOverlayController.TryGetOpenHoldProgress(owner, progress) &&
                !self.m_hintButtonController.IsHeld(modules::UiOwner::P1) &&
                !self.m_hintButtonController.IsHeld(modules::UiOwner::P2);
        });
        m_dispatcher = std::make_unique<InteractionDispatcher>(
            m_stateMachine, *m_gamePersistence, m_eventQueue);
        if (!modules::LoadGameForPersistenceSession(
                m_settings, *m_gamePersistence, m_persistenceDegradation)) {
            return VIRTUAL_LITCH_POLICY_REJECTED;
        }
        m_gamePersistence->SetRuntimeHealthSnapshot(
            m_runtimeHealth.Snapshot());
        if (m_loadPersistedSettings) {
            m_gamePersistence->Litchcraft().Configure(
                static_cast<uint8_t>(HardwareProfile(m_config).revision),
                0x4c430001U ^ m_config.initial_time_ms);
            m_gamePersistence->Litchcraft().ConfigureShakeErase(
                hardware::HasFourLedSquares(HardwareProfile(m_config).revision));
            (void)m_gamePersistence->ArtPersistence().Restore();
        }
    }
    m_derived.Reset();
    m_powerPolicy.Reset();
    m_beginnerModeForcedHintDisplay = false;
    m_controlMutation.ResetTransientState();
    m_localControlRequests =
        firmware_internal::LocalControlRequestTracker{};
    m_resetHoldController.Reset();
    m_gameResetHoldActive = false;
    m_gameResetHoldTriggered = false;
    m_hintButtonController.Reset();
    m_paletteEditPreview.Reset();
    m_quickSettingsOverlayController.Reset();
    m_quickSettingsOverlayActive = false;
    m_quickSettingsOverlayOwner = modules::UiOwner::None;
    m_quickSettingsOverlaySelectedIndex =
        modules::quick_settings_items::CPU_DIFFICULTY;
    m_menuOverlay.Stop();
    m_bleIdentify = {};
    m_identifyOverlayActive = false;
    m_displayLabel.Clear();
    m_edgeCue.Clear();
    m_lastBoardControlOutcome = modules::ControlMutationOutcome{};
    m_lastBoardControlOutcomeValid = false;
    m_sleep.Reset();
    m_setupLifecycle = m_mode == SessionMode::EngineBackedJourney
                           ? CurrentSetupLifecycle()
                           : SetupLifecycle::Gameplay;
    m_boardObservationSequence = 0U;
    m_setupPresentationObservationSequence = 0U;
    m_setupPresentationObservationValid = false;
    m_frameSequence = 0U;
    m_litchcraftCrossfade.Reset();
    m_bootDisplay = modules::BootDisplay{};
    // Ordinary sessions explicitly select no boot episode. The boot journey
    // re-arms the selected display at the actual service-ready boundary.
    m_bootDisplay.Begin(modules::BootDisplayChoice::Off, false, {});
    m_historyCount = 0U;
    m_historyWriteIndex = 0U;
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

void Session::ObserveBootHallMask(const uint64_t logicalOccupiedMask) {
    SessionTimeScope timeScope(m_nowMs);
    m_bootDisplay.ObserveBoard(
        !m_gamePersistence || !m_gamePersistence->Litchcraft().IsInitialStoreModePending(), logicalOccupiedMask,
        !m_gamePersistence || !m_gamePersistence->Litchcraft().IsActive());
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
        m_gamePersistence->Litchcraft().ResynchronizeOccupancy(logicalOccupiedMask);
        CaptureFrame();
        return;
    }
    m_stateMachine.ApplyPhysicalOccupancySnapshot(logicalOccupiedMask);
    if (!m_stateMachine.IsPhysicalOccupancyConsistentWithInteraction() &&
        m_gamePersistence != nullptr &&
        !m_gamePersistence->ShouldDeferInitialPlacementForVariantDetection(
            logicalOccupiedMask)) {
        (void)m_stateMachine.ForceEnterPlacementMode();
    }
    CaptureFrame();
}

modules::HomeStateNotification Session::HomeStateSnapshot() const {
    SessionTimeScope timeScope(m_nowMs);
    if (m_gamePersistence != nullptr) {
        return modules::BuildHomeStateSnapshot(
            m_stateMachine, *m_gamePersistence, m_settings, m_beginnerMode,
            m_persistenceDegradation,
            {
                .menuOverlayActive = m_menuOverlay.IsActive(),
                .quickSettingsOverlayActive =
                    m_quickSettingsOverlayActive,
                .coachingForcedHintDisplay =
                    m_beginnerModeForcedHintDisplay,
            });
    }
    return {};
}

modules::HomeStateNotificationLine Session::HomeStateLine() const {
    modules::HomeStateNotificationLine line;
    auto snapshot = HomeStateSnapshot();
    snapshot.sequence = 1U;
    (void)modules::TryFormatHomeStateNotification(snapshot, line);
    return line;
}

void Session::RunControlIngress(
    void (*callback)(void*, modules::SettingsStore&,
                     modules::ControlMutationService&, modules::GamePersistenceCoordinator&),
    void* context) {
    SessionTimeScope timeScope(m_nowMs);
    callback(context, m_settings, m_controlMutation, *m_gamePersistence);
    UpdateProductionCoordinator();
    CaptureFrame();
}

bool Session::CanPublishHomeState() const {
    return m_gamePersistence != nullptr && modules::ShouldPublishHomeStateNotification(
        m_gamePersistence->GetGameVariantStateSnapshot(),
        m_stateMachine.GetPhysicalOccupancyMask(),
        m_gamePersistence->IsInitialDuckRetirementObservation());
}

modules::BoardHealthLine Session::BoardHealthStateLine() const {
    const auto degradation = m_gamePersistence == nullptr ? m_persistenceDegradation :
        modules::BuildHomeStateSnapshot(m_stateMachine, *m_gamePersistence,
            m_settings, m_beginnerMode, m_persistenceDegradation, {})
            .persistenceDegradation;
    return modules::FormatBoardHealth(m_runtimeHealth.Snapshot(),
        degradation, runtime::GetProgressSnapshot(), m_settings.IsDisabled());
}

modules::BoardHealthConnectionState Session::BoardHealthConnectionState() const {
    const auto degradation = m_gamePersistence == nullptr
        ? m_persistenceDegradation
        : modules::BuildHomeStateSnapshot(
              m_stateMachine, *m_gamePersistence, m_settings,
              m_beginnerMode, m_persistenceDegradation, {})
              .persistenceDegradation;
    return modules::EncodeBoardHealthConnectionState(
        m_runtimeHealth.Snapshot(), degradation,
        runtime::GetProgressSnapshot(), m_settings.IsDisabled());
}

bool Session::TryPrepareHomePublication(
    modules::HomeStateNotificationPublisher& publisher,
    const bool force, modules::HomeStateNotificationLine& outLine) const {
    return m_gamePersistence != nullptr &&
           modules::TryPrepareHomeStatePublication(
               m_stateMachine, *m_gamePersistence, m_settings, m_beginnerMode,
               m_persistenceDegradation,
               {
                   .menuOverlayActive = m_menuOverlay.IsActive(),
                   .quickSettingsOverlayActive = m_quickSettingsOverlayActive,
                   .coachingForcedHintDisplay = m_beginnerModeForcedHintDisplay,
               },
               publisher, force, outLine);
}

bool Session::SimulateBootPersistenceFailure(
    const modules::PersistenceDegradation degradation) {
    if (m_mode != SessionMode::EngineBackedJourney ||
        m_gamePersistence == nullptr ||
        degradation == modules::PersistenceDegradation::None) {
        return false;
    }
    SessionTimeScope timeScope(m_nowMs);
    if (degradation ==
        modules::PersistenceDegradation::QuarantinedRecord) {
        m_settings.Doc()["GAME_FEN"] = "not-a-fen";
        if (m_gamePersistence->LoadOrInitialize()) {
            return false;
        }
    }
    if (!modules::BeginVolatilePersistenceSession(
            m_settings, *m_gamePersistence, degradation)) {
        return false;
    }
    m_persistenceDegradation = degradation;
    m_setupLifecycle = CurrentSetupLifecycle();
    CaptureFrame();
    return true;
}

virtual_litch_result Session::DispatchPosition(
    const virtual_litch_position& position) {
    SessionTimeScope timeScope(m_nowMs);
    m_stateMachine.HandleInteractionPositionAcknowledged(
        position.occupied_mask,
        position.king_mask,
        position.rook_mask,
        position.mover_occupied_mask,
        position.opponent_occupied_mask,
        position.pawn_mask,
        position.en_passant_target_square,
        position.en_passant_capture_square,
        position.side_to_move_valid != 0U,
        position.side_to_move_black != 0U,
        position.legal_capture_targets_valid != 0U,
        position.legal_capture_targets_mask,
        position.legal_castling_targets_valid != 0U,
        position.legal_castling_king_targets_mask);
    if (position.side_to_move_valid != 0U) {
        m_derived.sideToMove = position.side_to_move_black != 0U
                                   ? modules::SideToMove::Black
                                   : modules::SideToMove::White;
    }
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

bool Session::LoadExternalFen(const char* fen) {
    if (m_mode != SessionMode::EngineBackedJourney ||
        m_gamePersistence == nullptr) {
        return false;
    }
    SessionTimeScope timeScope(m_nowMs);
    const bool loaded = m_gamePersistence->LoadExternalFen(fen);
    CaptureFrame();
    return loaded;
}

virtual_litch_result Session::DispatchBoardObservation(
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    if (m_mode == SessionMode::FactoryJourney || m_hardwareTest.IsActive()) {
        m_manufacturingHallValid = true;
        ObserveManufacturingBoard(occupiedMask);
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    if (!AllowsBoardIngress(m_runtimeHealth.Snapshot())) {
        return VIRTUAL_LITCH_POLICY_REJECTED;
    }
    SessionTimeScope timeScope(m_nowMs);
    m_bootDisplay.ObserveBoard(
        !m_gamePersistence || !m_gamePersistence->Litchcraft().IsInitialStoreModePending(), occupiedMask,
        !m_gamePersistence || !m_gamePersistence->Litchcraft().IsActive());
    const uint32_t requestedSequence = m_boardObservationSequence + 1U;
    if (m_mode == SessionMode::EngineBackedJourney) {
        modules::move_confirm_setting::Apply(m_stateMachine, m_settings);
    }
    m_boardObservationSequence =
        m_dispatcher->DispatchBoardObservation(BoardObservationEvent{
            m_nowMs,
            requestedSequence,
            changedMask,
            occupiedMask,
        });
    if (m_mode == SessionMode::EngineBackedJourney) {
        (void)m_gamePersistence->AuthorizeRestoredClockResumeAfterBoot(
            true, m_runtimeHealth.Snapshot());
        m_gamePersistence->ObserveFreshAcceptedBoardScan(occupiedMask);
    }
    // Match firmware PollSensorsAndDispatch: Hall truth reaches the ordinary
    // interaction owner, then its accepted change cancels pending sleep.
    if (changedMask != 0U) {
        (void)m_sleep.Cancel(firmware::SleepTransitionCause::BoardObservationChanged);
    }
    UpdateProductionCoordinator();
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::DispatchSensorDelta(
    const uint8_t square,
    const bool nowOccupied) {
    if (square >= VIRTUAL_LITCH_SQUARE_COUNT) {
        return VIRTUAL_LITCH_OUT_OF_RANGE;
    }
    if (!AllowsBoardIngress(m_runtimeHealth.Snapshot())) {
        return VIRTUAL_LITCH_POLICY_REJECTED;
    }

    const bool wasOccupied =
        (PhysicalOccupancyMask() & (UINT64_C(1) << square)) != 0U;
    SessionTimeScope timeScope(m_nowMs);
    if (m_mode == SessionMode::EngineBackedJourney) {
        modules::move_confirm_setting::Apply(m_stateMachine, m_settings);
    }
    m_dispatcher->DispatchSensorChange(SensorChangeEvent{
        m_nowMs,
        square,
        square,
        nowOccupied,
    });
    if (wasOccupied != nowOccupied) {
        (void)m_sleep.Cancel(firmware::SleepTransitionCause::BoardObservationChanged);
    }
    UpdateProductionCoordinator();
    m_boardObservationSequence =
        m_stateMachine.GetLastBoardObservationSequence();
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::DispatchButton(
    const uint8_t owner,
    const uint8_t button,
    const uint8_t event) {
    if (!IsValidButtonOwner(owner) ||
        button > VIRTUAL_LITCH_BUTTON_P2_O ||
        event > VIRTUAL_LITCH_BUTTON_LONG_HOLD_EVENT) {
        return VIRTUAL_LITCH_INVALID_ENUM;
    }

    const bool p2Unavailable =
        m_config.hardware == VIRTUAL_LITCH_HARDWARE_C3 &&
        (owner == VIRTUAL_LITCH_OWNER_P2 || IsP2Button(button));
    if (p2Unavailable) {
        return VIRTUAL_LITCH_OUT_OF_RANGE;
    }

    return DispatchNormalizedButton(owner, button, event);
}

virtual_litch_result Session::DispatchNormalizedButton(
    const uint8_t owner,
    const uint8_t button,
    const uint8_t event) {
    return DispatchNormalizedButtonAt(
        owner, button, event, m_nowMs, false);
}

virtual_litch_result Session::DispatchNormalizedButtonAt(
    const uint8_t owner,
    const uint8_t button,
    const uint8_t event,
    const uint32_t eventTimeMs,
    const bool litchcraftEntryArbitrated) {
    if (!IsValidButtonOwner(owner) ||
        button > VIRTUAL_LITCH_BUTTON_P2_O ||
        event > VIRTUAL_LITCH_BUTTON_LONG_HOLD_EVENT) {
        return VIRTUAL_LITCH_INVALID_ENUM;
    }

    if (button == VIRTUAL_LITCH_BUTTON_PRESS) return VIRTUAL_LITCH_OK;
    if (m_gamePersistence) m_gamePersistence->StopBenchmark();
    if (m_mode == SessionMode::FactoryJourney && !m_hardwareTest.IsActive()) return VIRTUAL_LITCH_OK;
    if (m_hardwareTest.IsActive()) {
        m_hardwareTest.OnButtonEvent({static_cast<modules::UserButton>(button),
            static_cast<fxe::Button::Event>(event), static_cast<modules::UiOwner>(owner)}, eventTimeMs);
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    if (m_sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ButtonPress)) {
        m_resetHoldController.Reset();
        m_gameResetHoldActive = false;
        m_hintButtonController.Reset();
        m_quickSettingsOverlayController.Reset();
        if (m_ownerPairing) m_ownerPairing->ReseedPhysicalInput();
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    SessionTimeScope pairingTimeScope(m_nowMs);
    m_boardStylePreview.Cancel();
    SyncOwnerPairingContext();
    const auto userButton = static_cast<modules::UserButton>(button);
    if (m_bootDisplay.ConsumeButton({userButton, static_cast<fxe::Button::Event>(event),
            static_cast<modules::UiOwner>(owner)})) {
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    if (m_ownerPairing && !litchcraftEntryArbitrated &&
        (modules::IsXButton(userButton) || modules::IsOButton(userButton))) {
        const uint8_t physicalEnd = static_cast<uint8_t>(
            (owner == VIRTUAL_LITCH_OWNER_P2) != ControlBanksSwapped());
        if (m_ownerPairing->ObserveButton({physicalEnd, modules::IsXButton(userButton),
                event != VIRTUAL_LITCH_BUTTON_RELEASE_EVENT, eventTimeMs})) {
            if (m_quickSettingsOverlayActive) m_quickSettingsOverlayIdleElapsed.Reset();
            if (m_gamePersistence) m_gamePersistence->Litchcraft().CancelEntryHold();
            m_resetHoldController.Reset();
            m_gameResetHoldActive = false;
            m_hintButtonController.Reset();
            m_quickSettingsOverlayController.Reset();
            CaptureFrame();
            return VIRTUAL_LITCH_OK;
        }
    }
    if (event == VIRTUAL_LITCH_BUTTON_PRESS_EVENT &&
        ConsumeSleepForLocalActivity(
            firmware::SleepTransitionCause::ButtonPress)) {
        return VIRTUAL_LITCH_OK;
    }

    SessionTimeScope timeScope(m_nowMs);
    if (m_mode == SessionMode::EngineBackedJourney) {
        modules::move_confirm_setting::Apply(m_stateMachine, m_settings);
    }
    const ButtonInputEvent buttonEvent{
        eventTimeMs,
        static_cast<modules::UserButton>(button),
        static_cast<fxe::Button::Event>(event),
        static_cast<modules::UiOwner>(owner),
        litchcraftEntryArbitrated,
    };
    if (m_gamePersistence && m_gamePersistence->Litchcraft().OwnsButton(
            {buttonEvent.button,buttonEvent.event,buttonEvent.owner})) {
        m_gamePersistence->ObserveLitchcraftButton(
            {buttonEvent.button,buttonEvent.event,buttonEvent.owner},
            eventTimeMs);
        CaptureFrame(); return VIRTUAL_LITCH_OK;
    }
    const bool playFaceChordAvailable =
        !m_menuOverlay.IsActive() && !m_quickSettingsOverlayActive &&
        !m_stateMachine.IsTextOverlayActive() &&
        !m_stateMachine.IsPromotionActive() &&
        !m_stateMachine.IsUndoPromptActive() &&
        !m_stateMachine.IsKingQueenSwapPromptActive() &&
        m_controlMutation.GetActionState() !=
            modules::ControlActionState::UndoPending;
    if (m_gamePersistence && !buttonEvent.litchcraftEntryArbitrated &&
        playFaceChordAvailable &&
        (modules::IsXButton(buttonEvent.button) || modules::IsOButton(buttonEvent.button)) &&
        (m_gamePersistence->Litchcraft().IsEntryHoldActive() ||
         buttonEvent.event == fxe::Button::Event::Press)) {
        const auto result = m_gamePersistence->ObserveLitchcraftEntryButton(
            {buttonEvent.button, buttonEvent.event, buttonEvent.owner},
            eventTimeMs);
        if (result == modules::LitchcraftSession::ModeChordResult::Blocked) {
            m_stateMachine.TriggerAllPiecesFlash(1000U);
            m_edgeCue.Show(modules::EdgeLedCueKind::OccupiedBoardBlocked,
                           modules::UiOwner::None, eventTimeMs);
            m_resetHoldController.Reset();
            m_gameResetHoldActive = false;
            m_gameResetHoldTriggered = false;
            m_hintButtonController.Reset();
            m_paletteEditPreview.Reset();
            m_quickSettingsOverlayController.Reset();
            CaptureFrame();
            return VIRTUAL_LITCH_OK;
        }
        if (result == modules::LitchcraftSession::ModeChordResult::Replay ||
            result == modules::LitchcraftSession::ModeChordResult::Blocked) {
            modules::ButtonEvent fallbackEvent;
            uint32_t fallbackTimeMs = 0U;
            if (m_gamePersistence->Litchcraft().TakeEntryButtonForReplay(
                    fallbackEvent, fallbackTimeMs)) {
                const auto firstResult = DispatchNormalizedButtonAt(
                    static_cast<uint8_t>(fallbackEvent.owner),
                    static_cast<uint8_t>(fallbackEvent.button),
                    static_cast<uint8_t>(fallbackEvent.event),
                    fallbackTimeMs, true);
                if (firstResult != VIRTUAL_LITCH_OK) return firstResult;
                return DispatchNormalizedButtonAt(
                    owner, button, event, eventTimeMs, true);
            }
        }
        if (result == modules::LitchcraftSession::ModeChordResult::Entered) {
            m_resetHoldController.Reset();
            m_gameResetHoldActive = false;
            m_gameResetHoldTriggered = false;
            m_hintButtonController.Reset();
            m_paletteEditPreview.Reset();
            m_quickSettingsOverlayController.Reset();
        }
        if (result != modules::LitchcraftSession::ModeChordResult::Ignored) {
            CaptureFrame();
            return VIRTUAL_LITCH_OK;
        }
    }
    if (m_gamePersistence && m_gamePersistence->HandleKingResignationButton(
            {buttonEvent.button, buttonEvent.event, buttonEvent.owner}, eventTimeMs)) {
        m_hintButtonController.Reset();
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    m_waterSlosh.ObserveControl(
        {buttonEvent.button, buttonEvent.event, buttonEvent.owner}, eventTimeMs);
    if (m_mode == SessionMode::EngineBackedJourney) {
        const bool resetHoldEligible = modules::IsGameResetHoldEligible({
            .menuVisualActive = false,
            .menuOverlayActive = m_menuOverlay.IsActive(),
            .quickSettingsOverlayActive = m_quickSettingsOverlayActive,
            .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
            .promotionActive = m_stateMachine.IsPromotionActive(),
        });
        if (!resetHoldEligible) {
            m_resetHoldController.Reset();
            m_gameResetHoldActive = false;
            m_gameResetHoldTriggered = false;
        } else {
            modules::ResetHoldAction resetAction;
            const bool consumedByResetHold =
                m_resetHoldController.HandleButtonEvent(
                    {buttonEvent.button, buttonEvent.event, buttonEvent.owner},
                    resetAction,
                    eventTimeMs);
            if (resetAction.type == modules::ResetHoldActionType::StartHold) {
                m_gameResetHoldActive = true;
                m_gameResetHoldTriggered = false;
                m_gameResetHoldRejected = false;
                m_gameResetHoldStartMs = eventTimeMs;
                m_gameResetHoldOwner = resetAction.owner;
                m_hintButtonController.Reset();
                m_paletteEditPreview.Reset();
                m_quickSettingsOverlayController.Reset();
                CaptureFrame();
                return VIRTUAL_LITCH_OK;
            }
            if (resetAction.type == modules::ResetHoldActionType::CancelHold) {
                m_gameResetHoldActive = false;
                m_gameResetHoldTriggered = false;
                m_hintButtonController.Reset();
                m_paletteEditPreview.Reset();
                m_quickSettingsOverlayController.Reset();
                CaptureFrame();
                return VIRTUAL_LITCH_OK;
            }
            if (consumedByResetHold) {
                CaptureFrame();
                return VIRTUAL_LITCH_OK;
            }
        }
        const modules::ButtonEvent quickSettingsEvent{
            buttonEvent.button,
            buttonEvent.event,
            buttonEvent.owner,
        };
        modules::QuickSettingsAction quickAction;
        const bool directQuickSettingsEntryAllowed =
            modules::IsQuickSettingsDirectEntryAllowed({
                .quickSettingsOverlayActive = m_quickSettingsOverlayActive,
                .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
                .placementActive = m_stateMachine.IsPlacementModeActive(),
                .kingQueenSwapActive =
                    m_stateMachine.IsKingQueenSwapPromptActive(),
                .undoPromptActive = m_stateMachine.IsUndoPromptActive(),
                .promotionActive = m_stateMachine.IsPromotionActive(),
            });
        if (m_quickSettingsOverlayController.HandleButtonEvent(
                quickSettingsEvent,
                m_quickSettingsOverlayActive,
                m_controlMutation.GetActionState() ==
                    modules::ControlActionState::UndoPending,
                directQuickSettingsEntryAllowed,
                quickAction,
                eventTimeMs)) {
            if (m_quickSettingsOverlayActive ||
                quickAction.type ==
                    modules::QuickSettingsActionType::OpenOverlay) {
                HandleQuickSettingsAction(quickAction);
            } else if (quickAction.type ==
                       modules::QuickSettingsActionType::UndoTap) {
                const modules::led_frame_composer::internal::RenderIntent
                    renderIntent =
                        modules::led_frame_composer::internal::BuildRenderIntent({
                            .snapshot = &m_snapshot,
                            .menuRequested = m_menuOverlay.IsActive(),
                            .hintMoveValid = m_snapshot.render.hintMoveValid,
                        });
                const bool initialKingAssistOwnsX =
                    modules::led_frame_composer::internal::
                        CanInitialKingPlacementAssistOwnBoard(
                            m_settings, m_snapshot, renderIntent);
                const uint8_t undoCount = modules::GetVsCpuUndoPlyCount(
                    m_gamePersistence->GetEngineDerivedState().sideToMove,
                    m_settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U)),
                    FirstPlayerIsP2(),
                    m_settings.ReadValue("VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U);
                bool trackingFailed = false;
                const modules::ControlMutationOutcome outcome =
                    firmware_internal::RunBoardUndoTap(
                        m_stateMachine,
                        initialKingAssistOwnsX,
                        m_controlMutation,
                        m_localControlRequests,
                        undoCount,
                        trackingFailed, quickAction.owner);
                RecordBoardControlOutcome(outcome);
            }
            CaptureFrame();
            return VIRTUAL_LITCH_OK;
        }

        const runtime::TaggedInteractionState& interactionPhase =
            m_stateMachine.GetChessInteractionModel().phase;
        const bool humanDuckRelocation =
            m_gamePersistence->GetGameVariant() ==
                modules::GameVariant::Duck &&
            modules::IsHumanDuckRelocation(interactionPhase);
        const bool canTrackHintGesture =
            (interactionPhase.tag == runtime::ChessInteractionPhase::Idle ||
             humanDuckRelocation) &&
            !m_stateMachine.IsMoveSettlingActive() &&
            !m_stateMachine.HasQueuedCpuMove() &&
            !m_stateMachine.IsPromotionActive();
        const modules::ButtonEvent hintEvent{
            buttonEvent.button,
            buttonEvent.event,
            buttonEvent.owner,
        };
        if (modules::CanTrackCoachingGesture(m_stateMachine)) {
            const modules::UiOwner activeOwner =
                modules::led_frame_composer::OwnerForSideToMove(
                    m_gamePersistence->GetEngineDerivedState().sideToMove,
                    FirstPlayerIsP2());
            const modules::HintButtonRoutingDecision hintRouting =
                modules::ResolveHintButtonRouting(
                    true,
                    modules::IsOButton(hintEvent.button),
                    hintEvent.owner,
                    activeOwner,
                    canTrackHintGesture && !m_stateMachine.IsPlacementModeActive() &&
                        m_stateMachine.IsPhysicalOccupancyConsistentWithInteraction() &&
                        !modules::assistance_preferences::CpuOwns(m_settings, hintEvent.owner));
            if (hintRouting.shouldTrackGesture) {
                modules::HintButtonAction hintAction;
                if (m_hintButtonController.HandleButtonEvent(
                        hintEvent, hintAction, eventTimeMs)) {
                    if (hintAction.type ==
                            modules::HintButtonActionType::ToggleHint &&
                        hintRouting.shouldRequestHintOnRelease) {
                        bool trackingFailed = false;
                        const modules::ControlMutationOutcome outcome =
                            firmware_internal::RunBoardHintTap(
                                m_controlMutation,
                                m_localControlRequests,
                                trackingFailed, hintAction.owner);
                        RecordBoardControlOutcome(outcome);
                    }
                    CaptureFrame();
                    return VIRTUAL_LITCH_OK;
                }
            }
        } else if (modules::IsOButton(hintEvent.button) &&
                   m_hintButtonController.IsHeld(hintEvent.owner)) {
            m_hintButtonController.Cancel(hintEvent.owner);
        }
        const bool pressLike =
            buttonEvent.event == fxe::Button::Event::Press ||
            buttonEvent.event == fxe::Button::Event::Repeat;
        if (modules::physical_palette_shortcut::TryApply(
                hintEvent,
                pressLike,
                m_settings,
                m_controlMutation, &m_paletteEditPreview)) {
            CaptureFrame();
            return VIRTUAL_LITCH_OK;
        }
    }
    m_dispatcher->DispatchButtonEvent(buttonEvent);
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::StepUntil(const uint32_t targetTimeMs) {
    if (targetTimeMs < m_nowMs) {
        return VIRTUAL_LITCH_TIME_REVERSED;
    }

    m_nowMs = targetTimeMs;
    SessionTimeScope timeScope(m_nowMs);
    SyncOwnerPairingContext();
    if (m_ownerPairing) m_ownerPairing->Poll();
    if (m_mode == SessionMode::FactoryJourney) {
        m_hardwareTest.Tick(m_nowMs);
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    modules::UpdateRuntimePersistenceSession(
        m_settings, m_controlMutation, m_persistenceDegradation);
    if (m_hardwareTest.IsActive()) {
        m_hardwareTest.Tick(m_nowMs);
        CaptureFrame();
        return VIRTUAL_LITCH_OK;
    }
    m_sleep.Update(m_nowMs);
    if (m_gamePersistence) {
        modules::ButtonEvent fallbackEvent;
        uint32_t fallbackTimeMs = 0U;
        if (m_gamePersistence->Litchcraft().TryExpireEntryButton(
                m_nowMs, fallbackEvent, fallbackTimeMs)) {
            const auto result = DispatchNormalizedButtonAt(
                static_cast<uint8_t>(fallbackEvent.owner),
                static_cast<uint8_t>(fallbackEvent.button),
                static_cast<uint8_t>(fallbackEvent.event),
                fallbackTimeMs, true);
            if (result != VIRTUAL_LITCH_OK) return result;
        }
    }
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
        m_controlMutation.Update();
        m_gamePersistence->UpdateLitchcraft();
        CaptureFrame(); return VIRTUAL_LITCH_OK;
    }
    if (m_mode == SessionMode::EngineBackedJourney) {
        modules::move_confirm_setting::Apply(m_stateMachine, m_settings);
    }
    m_dispatcher->DispatchTick(m_nowMs);
    if (m_mode == SessionMode::EngineBackedJourney) {
        const bool resetHoldEligible = modules::IsGameResetHoldEligible({
            .menuVisualActive = false,
            .menuOverlayActive = m_menuOverlay.IsActive(),
            .quickSettingsOverlayActive = m_quickSettingsOverlayActive,
            .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
            .promotionActive = m_stateMachine.IsPromotionActive(),
        });
        if (resetHoldEligible) {
            modules::ResetHoldAction resetAction;
            while (m_resetHoldController.Tick(resetAction)) {
                if (resetAction.type !=
                        modules::ResetHoldActionType::TriggerReset ||
                    m_gameResetHoldTriggered) {
                    continue;
                }
                bool trackingFailed = false;
                const modules::ControlMutationOutcome outcome =
                    firmware_internal::RunLocalControlAction(
                        m_controlMutation,
                        m_localControlRequests,
                        firmware_internal::LocalControlRequestOrigin::Board,
                        modules::ControlAction::NewGame,
                        resetAction.owner,
                        modules::CoachingMutation::Toggle,
                        1U,
                        trackingFailed);
                RecordBoardControlOutcome(outcome);
                m_gameResetHoldTriggered = true;
            }
        } else {
            m_resetHoldController.Reset();
            m_gameResetHoldActive = false;
            m_gameResetHoldTriggered = false;
        }
    }
    if (m_mode == SessionMode::EngineBackedJourney &&
        !m_quickSettingsOverlayActive && !m_gameResetHoldActive) {
        modules::QuickSettingsAction quickAction;
        const bool directQuickSettingsEntryAllowed =
            modules::IsQuickSettingsDirectEntryAllowed({
                .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
                .placementActive = m_stateMachine.IsPlacementModeActive(),
                .kingQueenSwapActive =
                    m_stateMachine.IsKingQueenSwapPromptActive(),
                .undoPromptActive = m_stateMachine.IsUndoPromptActive(),
                .promotionActive = m_stateMachine.IsPromotionActive(),
            });
        if (m_quickSettingsOverlayController.Tick(
                quickAction, directQuickSettingsEntryAllowed)) {
            HandleQuickSettingsAction(quickAction);
        }
        modules::HintButtonAction hintAction;
        if (!modules::CanTrackCoachingGesture(m_stateMachine)) {
            m_hintButtonController.Cancel(modules::UiOwner::P1);
            m_hintButtonController.Cancel(modules::UiOwner::P2);
        }
        while (m_hintButtonController.Tick(hintAction)) {
            if (hintAction.type !=
                modules::HintButtonActionType::BeginnerModeHold) {
                continue;
            }
            bool trackingFailed = false;
            const modules::ControlMutationOutcome outcome =
                firmware_internal::RunLocalControlAction(
                    m_controlMutation,
                    m_localControlRequests,
                    firmware_internal::LocalControlRequestOrigin::Board,
                    modules::ControlAction::Coaching,
                    hintAction.owner,
                    modules::CoachingMutation::Toggle,
                    1U,
                    trackingFailed);
            if (outcome.status == modules::ControlMutationStatus::Rejected)
                modules::ObserveControlLedActionFeedback(m_controlFeedback, outcome,
                    hintAction.owner, m_controlMutation.GetActionState());
            RecordBoardControlOutcome(outcome);
        }
    }
    if (m_quickSettingsOverlayActive && m_quickSettingsOverlayIdleElapsed.Started() &&
        m_quickSettingsOverlayIdleElapsed.HasElapsed(modules::QUICK_SETTINGS_IDLE_CLOSE_MS)) {
        CloseQuickSettings();
    }
    m_controlMutation.Update();
    firmware_internal::LocalControlCompletion completion;
    while (firmware_internal::TryTakeNextLocalControlCompletion(
        m_controlMutation, m_localControlRequests, completion)) {
        RecordBoardControlOutcome(completion.outcome);
    }
    UpdateProductionCoordinator();
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::Advance(const uint32_t deltaMs) {
    if (deltaMs > std::numeric_limits<uint32_t>::max() - m_nowMs) {
        return VIRTUAL_LITCH_TIME_OVERFLOW;
    }
    return StepUntil(m_nowMs + deltaMs);
}

uint64_t Session::PhysicalOccupancyMask() const {
    if (m_hardwareTest.IsActive()) { return m_manufacturingOccupancy; }
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive())
        return m_gamePersistence->Litchcraft().Occupancy();
    return m_stateMachine.GetPhysicalOccupancyMask();
}

bool Session::BoardOverlayActive() const {
    SessionTimeScope timeScope(m_nowMs);
    return m_quickSettingsOverlayActive || m_menuOverlay.IsActive();
}

void Session::UpdateProductionCoordinator() {
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
        m_gamePersistence->UpdateLitchcraft(); return;
    }
    if (m_mode != SessionMode::EngineBackedJourney) {
        return;
    }
    const uint32_t boardObservationSequence =
        m_stateMachine.GetLastBoardObservationSequence();
    const bool setupCompletionActive =
        m_edgeCue.Current(m_nowMs).kind ==
        modules::EdgeLedCueKind::SetupComplete;
    if (((m_menuOverlay.IsActive() &&
          m_menuOverlay.PreservesTurnIndicator()) ||
         setupCompletionActive) &&
        m_setupPresentationObservationValid &&
        boardObservationSequence !=
            m_setupPresentationObservationSequence) {
        if (m_stateMachine.GetLastBoardObservationChangedMask() != 0ULL) {
            m_menuOverlay.Stop();
            m_edgeCue.Clear();
            m_displayLabel.Clear();
            m_gamePersistence->CancelSetupPresentationEvents();
            m_setupPresentationObservationValid = false;
        } else {
            m_setupPresentationObservationSequence =
                boardObservationSequence;
        }
    } else if (!m_menuOverlay.IsActive() && !setupCompletionActive) {
        m_setupPresentationObservationValid = false;
        m_displayLabel.Clear();
    }
    const bool processCommittedMoveImmediately =
        m_stateMachine.ShouldProcessCommittedMoveImmediately();
    if (processCommittedMoveImmediately) {
        m_gamePersistence->Update();
    }
    m_gamePersistence->UpdateLegalDestinationsMask();
    if (!processCommittedMoveImmediately) {
        m_gamePersistence->Update();
    }
    if (!m_menuOverlay.IsActive() && m_bootDisplay.IsComplete() &&
        !m_startupPieceAccent.IsArmed()) {
        modules::SetupPresentationEvent event;
        if (m_gamePersistence->TryTakeSetupPresentationEvent(event)) {
            if (event == modules::SetupPresentationEvent::Ready) {
                m_edgeCue.Show(modules::EdgeLedCueKind::SetupComplete,
                               modules::UiOwner::None,
                               m_nowMs);
                m_setupPresentationObservationSequence =
                    boardObservationSequence;
                m_setupPresentationObservationValid = true;
            }
        }
    }
}

void Session::SyncBeginnerModePolicy() {
    if (m_mode != SessionMode::EngineBackedJourney ||
        m_gamePersistence == nullptr) {
        return;
    }
    const runtime::TaggedInteractionState& interactionPhase =
        m_stateMachine.GetChessInteractionModel().phase;
    modules::BeginnerModePolicyInputs inputs;
    inputs.variant = m_gamePersistence->GetGameVariant();
    inputs.setupCompleteActive = !m_bootDisplay.IsComplete() ||
        m_startupPieceAccent.IsArmed() || m_edgeCue.Current(m_nowMs).kind ==
        modules::EdgeLedCueKind::SetupComplete;
    inputs.quickSettingsOverlayActive = m_quickSettingsOverlayActive;
    inputs.textOverlayActive = m_stateMachine.IsTextOverlayActive();
    inputs.placementModeActive = m_stateMachine.IsPlacementModeActive();
    inputs.kingQueenSwapPromptActive =
        m_stateMachine.IsKingQueenSwapPromptActive();
    inputs.undoPromptActive = m_stateMachine.IsUndoPromptActive();
    inputs.promotionActive = m_stateMachine.IsPromotionActive();
    inputs.moveSettlingActive = m_stateMachine.IsMoveSettlingActive();
    inputs.queuedCpuMove = m_stateMachine.HasQueuedCpuMove();
    inputs.sideToMove =
        m_gamePersistence->GetEngineDerivedState().sideToMove;
    inputs.vsCpuLevel = m_settings.ReadValue(
        "VS_CPU_LEVEL", static_cast<uint32_t>(0U));
    inputs.firstPlayerIsP2 = FirstPlayerIsP2();
    inputs.cpuPlayerIsP2 = m_settings.ReadValue(
        "VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
    (void)m_stateMachine.TryGetHintMoveSquares(inputs.hintFromSquare, inputs.hintToSquare);
    const modules::UiOwner activeOwner =
        modules::ActiveBeginnerModeOwnerForTurn(
            m_beginnerMode, inputs, interactionPhase);
    m_gamePersistence->SetCoachingHintActive(
        modules::BeginnerModeOwnerForHumanTurn(
            m_beginnerMode, inputs) != modules::UiOwner::None);
    runtime::SyncBeginnerModeHintDisplay(
        m_stateMachine,
        activeOwner,
        m_beginnerModeForcedHintDisplay);
}

void Session::RecordBoardControlOutcome(
    const modules::ControlMutationOutcome& outcome,
    const bool showFeedback) {
    if (outcome.action == modules::ControlAction::NewGame &&
        outcome.status == modules::ControlMutationStatus::Rejected) {
        m_gameResetHoldRejected = true;
    }
    m_lastBoardControlOutcome = outcome;
    m_lastBoardControlOutcomeValid = true;
    if (!showFeedback) {
        return;
    }
    const firmware_internal::BoardControlFeedback feedback =
        firmware_internal::ClassifyBoardControlFeedback(
            firmware_internal::LocalControlRequestOrigin::Board,
            outcome);
    if (feedback.visible) {
        if (feedback.flashAllPieces) {
            m_stateMachine.TriggerAllPiecesFlash(1000U);
        }
        m_edgeCue.Show(
            feedback.edgeCue, modules::UiOwner::None, m_nowMs);
    }
}

bool Session::TryGetLastBoardControlOutcome(
    modules::ControlMutationOutcome& out) const {
    if (!m_lastBoardControlOutcomeValid) {
        return false;
    }
    out = m_lastBoardControlOutcome;
    return true;
}

SetupLifecycle Session::CurrentSetupLifecycle() const {
    return ClassifySetupLifecycle(
        m_gamePersistence->GetGameVariant(),
        m_gamePersistence->GetDuckTurnPhase(),
        m_stateMachine.GetMovedSquaresMask());
}

bool Session::TryGetHistory(
    const size_t chronologicalIndex,
    virtual_litch_history_frame& out) const {
    if (chronologicalIndex >= m_historyCount || m_history.empty()) {
        return false;
    }
    const size_t oldest = m_historyCount < m_history.size()
                              ? 0U
                              : m_historyWriteIndex;
    const size_t slot =
        (oldest + chronologicalIndex) % m_history.size();
    out = m_history[slot];
    return true;
}

virtual_litch_result Session::RequestSleep(const uint8_t origin) {
    m_bootDisplay.Cancel();
    if (origin > VIRTUAL_LITCH_SLEEP_ORIGIN_APP &&
        origin != VIRTUAL_LITCH_SLEEP_ORIGIN_NONE) {
        return VIRTUAL_LITCH_INVALID_ENUM;
    }
    SessionTimeScope timeScope(m_nowMs);
    const bool accepted = m_sleep.Request(
        static_cast<firmware::SleepRequestOrigin>(origin), m_nowMs,
        origin == VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE);
    if (accepted && m_sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ButtonPress)) {
        m_resetHoldController.Reset();
        m_gameResetHoldActive = false;
        m_hintButtonController.Reset();
        m_quickSettingsOverlayController.Reset();
        if (m_ownerPairing) m_ownerPairing->ReseedPhysicalInput();
    }
    CaptureFrame();
    return accepted ? VIRTUAL_LITCH_OK : VIRTUAL_LITCH_POLICY_REJECTED;
}

virtual_litch_result Session::DispatchValidatedMotionOrTap() {
    SessionTimeScope timeScope(m_nowMs);
    const bool cancelled = m_sleep.Cancel(
        firmware::SleepTransitionCause::ValidatedMotionOrTap);
    CaptureFrame();
    return cancelled ? VIRTUAL_LITCH_OK : VIRTUAL_LITCH_POLICY_REJECTED;
}

virtual_litch_result Session::ResolveSleepPreflights(
    const bool persistenceSucceeded,
    const bool wakeSourceSucceeded) {
    SessionTimeScope timeScope(m_nowMs);
    bool accepted = m_sleep.ReportPersistencePreflight(persistenceSucceeded);
    if (accepted && persistenceSucceeded) {
        accepted = m_sleep.ReportWakeSourcePreflight(wakeSourceSucceeded);
    }
    if (accepted && persistenceSucceeded && wakeSourceSucceeded) {
        accepted = m_sleep.CommitSleep() && m_sleep.ConsumeSleepAuthorization();
    }
    CaptureFrame();
    return accepted ? VIRTUAL_LITCH_OK : VIRTUAL_LITCH_POLICY_REJECTED;
}

virtual_litch_result Session::SimulateSleepWake() {
    SessionTimeScope timeScope(m_nowMs);
    const bool accepted = m_sleep.BeginWakeGuard(m_nowMs);
    CaptureFrame();
    return accepted ? VIRTUAL_LITCH_OK : VIRTUAL_LITCH_POLICY_REJECTED;
}

virtual_litch_result Session::ReportWakeButtonsReleased() {
    SessionTimeScope timeScope(m_nowMs);
    const bool accepted = m_sleep.ReportWakeButtonsReleased();
    CaptureFrame();
    return accepted ? VIRTUAL_LITCH_OK : VIRTUAL_LITCH_POLICY_REJECTED;
}

bool Session::ConsumeSleepForLocalActivity(
    const firmware::SleepTransitionCause cause) {
    SessionTimeScope timeScope(m_nowMs);
    if (!m_sleep.ShouldIgnoreActivity(cause) && !m_sleep.Cancel(cause)) {
        return false;
    }
    CaptureFrame();
    return true;
}

virtual_litch_result Session::MutateSetting(
    const virtual_litch_setting_request& request,
    modules::ControlMutationOutcome& out) {
    SessionTimeScope timeScope(m_nowMs);
    StaticJsonDocument<128> value;
    if (deserializeJson(value, request.value_json)) {
        return VIRTUAL_LITCH_INVALID_CONFIG;
    }
    m_bootDisplay.Cancel();
    out = m_controlMutation.MutateSetting(
        request.key, value.as<JsonVariantConst>(), request.request_id);
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::DispatchControlAction(
    const virtual_litch_action_request& request,
    modules::ControlMutationOutcome& out) {
    SessionTimeScope timeScope(m_nowMs);
    m_bootDisplay.Cancel();
    out = m_controlMutation.MutateAction(
        static_cast<modules::ControlAction>(request.action),
        request.request_id,
        static_cast<modules::UiOwner>(request.owner),
        static_cast<modules::CoachingMutation>(request.coaching_mutation),
        request.undo_move_count);
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

virtual_litch_result Session::DispatchRuntimeHealthFact(
    const virtual_litch_runtime_health_fact& fact) {
    SessionTimeScope timeScope(m_nowMs);
    runtime::RuntimeHealthFact runtimeFact;
    runtimeFact.tMs = m_nowMs;
    runtimeFact.kind =
        static_cast<runtime::RuntimeHealthFactKind>(fact.kind);
    runtimeFact.boardScan =
        static_cast<runtime::RuntimeHealthBoardScan>(fact.board_scan);
    runtimeFact.ambientSensorCount = fact.ambient_sensor_count;
    runtimeFact.source =
        static_cast<runtime::RuntimeHealthSource>(fact.source);
    runtimeFact.powerEnvelopePercent = fact.power_envelope_percent;
    runtimeFact.brownoutDetected = fact.brownout_detected != 0U;
    (void)m_runtimeHealth.Apply(runtimeFact);
    if (m_gamePersistence != nullptr) {
        m_gamePersistence->SetRuntimeHealthSnapshot(
            m_runtimeHealth.Snapshot());
    }
    CaptureFrame();
    return VIRTUAL_LITCH_OK;
}

bool Session::TryTakeControlCompletion(
    modules::ControlMutationOutcome& out) {
    return m_controlMutation.TryTakeCompletion(out);
}

modules::ControlCoordinatorResult Session::CheckStartOnly(
    void* context,
    const char* canonicalKey) {
    auto& self = *static_cast<Session*>(context);
    if (self.m_mode == SessionMode::EngineBackedJourney) {
        return self.m_gamePersistence->AdmitControlStartOnlyMutation(
            canonicalKey);
    }
    if (self.m_stateMachine.GetMovedSquaresMask() != 0ULL) {
        return modules::ControlCoordinatorResult::NotAtStart;
    }
    const bool orientationMutation =
        canonicalKey != nullptr &&
        (std::strcmp(canonicalKey, "BOARD_ROTATE") == 0 ||
         std::strcmp(canonicalKey, "GAME_FIRST_PLAYER") == 0);
    const bool cancelableOrientationPrompt =
        orientationMutation &&
        self.m_stateMachine.IsKingQueenSwapPromptCancelable();
    if (self.m_stateMachine.GetChessInteractionModel().phase.tag !=
            ChessInteractionPhase::Idle ||
        self.m_stateMachine.IsMoveSettlingActive() ||
        self.m_stateMachine.HasQueuedCpuMove() ||
        self.m_stateMachine.IsPromotionActive() ||
        self.m_stateMachine.IsPlacementModeActive() ||
        (self.m_stateMachine.IsKingQueenSwapPromptActive() &&
         !cancelableOrientationPrompt) ||
        self.m_stateMachine.IsUndoPromptActive() ||
        self.m_stateMachine.IsTextOverlayActive()) {
        return modules::ControlCoordinatorResult::Busy;
    }
    return modules::ControlCoordinatorResult::Applied;
}

modules::ControlCoordinatorResult Session::RequestHint(void* context,
                                                       uint16_t requestId) {
    auto& self = *static_cast<Session*>(context);
    self.m_bootDisplay.Cancel();
    if (self.m_mode == SessionMode::EngineBackedJourney) {
        return self.m_gamePersistence->RequestControlHint(requestId);
    }
    self.m_stateMachine.HandleHintButtonTap();
    self.m_stateMachine.NoteManualHintRequest();
    return modules::ControlCoordinatorResult::Applied;
}

modules::ControlCoordinatorResult Session::RequestUndo(void* context,
                                                       uint16_t requestId,
                                                       uint8_t moveCount) {
    auto& self = *static_cast<Session*>(context);
    self.m_bootDisplay.Cancel();
    if (self.m_mode == SessionMode::EngineBackedJourney) {
        return self.m_gamePersistence->RequestControlUndo(
            requestId, moveCount);
    }
    return modules::ControlCoordinatorResult::Unsupported;
}

modules::ControlCoordinatorResult Session::RequestNewGame(void* context,
                                                          uint16_t requestId) {
    auto& self = *static_cast<Session*>(context);
    self.m_bootDisplay.Cancel();
    if (self.m_mode == SessionMode::EngineBackedJourney) {
        return self.m_gamePersistence->RequestControlNewGame(requestId);
    }
    self.m_stateMachine.ResetForExternalEngineMutation();
    self.m_eventQueue.Reset();
    self.m_derived.Reset();
    // The production New Game path acknowledges the canonical engine position
    // before arming the fresh-setup king identity cue. Preserve that ordering
    // in the host session so virtual verification observes the same board
    // contract instead of an empty reset-only approximation.
    self.m_stateMachine.HandleInteractionPositionAcknowledged(
        INITIAL_CHESS_OCCUPANCY,
        INITIAL_KING_MASK,
        INITIAL_ROOK_MASK,
        INITIAL_WHITE_OCCUPANCY,
        INITIAL_BLACK_OCCUPANCY,
        INITIAL_PAWN_MASK,
        UINT8_MAX,
        UINT8_MAX,
        true,
        false,
        false,
        0ULL,
        true,
        0ULL);
    self.m_stateMachine.TriggerInitialKingPlacementPrompt();
    self.m_setupLifecycle = SetupLifecycle::FreshSetup;
    return modules::ControlCoordinatorResult::Applied;
}

modules::ControlCoordinatorResult Session::PollControlAction(
    void* context,
    const uint16_t requestId) {
    auto& self = *static_cast<Session*>(context);
    return self.m_mode == SessionMode::EngineBackedJourney
               ? self.m_gamePersistence->PollControlAction(requestId)
               : modules::ControlCoordinatorResult::Unsupported;
}

void Session::OnCoachingChanged(void* context,
                               modules::UiOwner,
                               bool) {
    auto& session = *static_cast<Session*>(context);
    session.m_bootDisplay.Cancel();
    session.m_waterSlosh.NoteControl(session.m_nowMs);
}

void Session::SendLitchcraftPacket(const uint8_t* bytes,const size_t size) {
    SessionTimeScope timeScope(m_nowMs);
    if (!m_gamePersistence) return;
    auto& art=m_gamePersistence->Litchcraft();
    if (art.SessionId()==0U) art.Configure(
        static_cast<uint8_t>(HardwareProfile(m_config).revision), 0x4c430001U);
    m_gamePersistence->HandleLitchcraftPacket(bytes,size,!m_hardwareTest.IsActive() &&
        !m_quickSettingsOverlayActive && !m_menuOverlay.IsActive() &&
        AllowsBoardIngress(m_runtimeHealth.Snapshot()));
    CaptureFrame();
}

void Session::ShowIdentifyOverlay(const uint8_t number, const modules::Rgb color) {
    SessionTimeScope timeScope(m_nowMs);
    m_bleIdentify.Start(number > 99U ? 99U : number, color, false, 3000U, m_nowMs);
    CaptureFrame();
}

void Session::StopIdentifyOverlay() {
    SessionTimeScope timeScope(m_nowMs);
    m_bleIdentify.Stop();
    CaptureFrame();
}

void Session::RunIdentifyIngress(
    void (*callback)(void*, modules::BleIdentifyPresentation&, uint32_t), void* context) {
    SessionTimeScope timeScope(m_nowMs);
    callback(context, m_bleIdentify, m_nowMs);
    CaptureFrame();
}

} // namespace runtime::virtual_litch_internal

#endif
