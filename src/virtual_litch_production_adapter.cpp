#include "virtual_litch_session.hpp"

#ifdef LITCH_NATIVE

#include <array>
#include <cstddef>
#include <cstdio>
#include <utility>

#include "virtual_litch_session_internal.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/button_manager.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/button_event_transform.hpp"
#include "modules/orientation.hpp"
#include "modules/user_input.hpp"
#include "runtime/square_mapping.hpp"

namespace runtime {
namespace {

constexpr size_t CONTROLS_PER_OWNER = 6U;
constexpr size_t CONTROL_COUNT = CONTROLS_PER_OWNER * 2U;

uint64_t MapPhysicalMaskToLogical(const uint64_t physicalMask,
                                  const bool rotate) {
    if (!rotate) {
        return physicalMask;
    }
    uint64_t logicalMask = 0ULL;
    for (uint8_t physicalSquare = 0U;
         physicalSquare < VIRTUAL_LITCH_SQUARE_COUNT;
         ++physicalSquare) {
        if ((physicalMask & (UINT64_C(1) << physicalSquare)) == 0ULL) {
            continue;
        }
        logicalMask |= UINT64_C(1)
                       << MapSquareIndex(physicalSquare, true);
    }
    return logicalMask;
}

modules::UserButton LogicalButtonFor(
    const uint8_t physicalOwner,
    const VirtualLitchPhysicalButton button) {
    switch (button) {
        case VirtualLitchPhysicalButton::Up:
            return modules::UserButton::Up;
        case VirtualLitchPhysicalButton::Down:
            return modules::UserButton::Down;
        case VirtualLitchPhysicalButton::Left:
            return modules::UserButton::Left;
        case VirtualLitchPhysicalButton::Right:
            return modules::UserButton::Right;
        case VirtualLitchPhysicalButton::Press:
            return modules::UserButton::None;
        case VirtualLitchPhysicalButton::X:
            return physicalOwner == VIRTUAL_LITCH_OWNER_P2
                       ? modules::UserButton::P2X
                       : modules::UserButton::P1X;
        case VirtualLitchPhysicalButton::O:
            return physicalOwner == VIRTUAL_LITCH_OWNER_P2
                       ? modules::UserButton::P2O
                       : modules::UserButton::P1O;
    }
    return modules::UserButton::None;
}

fxe::Button::Config ButtonConfig(const modules::UserButton button) {
    fxe::Button::Config config;
    config.debounceMs = 20U;
    config.canRepeat = modules::IsDpadButton(button);
    if (config.canRepeat) {
        config.beforeRepeatMs =
            hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS;
        const bool adjustsValue = button == modules::UserButton::Up ||
                                  button == modules::UserButton::Down;
        config.repeatRateMs =
            adjustsValue
                ? hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS
                : hardware::user_input_timing::DPAD_NAV_REPEAT_RATE_MS;
    }
    return config;
}

} // namespace

class VirtualLitchSession::Impl final {
  public:
    explicit Impl(
        const virtual_litch_config& config,
        const char* settingsPath,
        modules::IEngine* const engineOverride = nullptr,
        modules::AsyncThinkWorker* const asyncThinkWorkerOverride = nullptr,
        const VirtualLitchBootProfile profile = VirtualLitchBootProfile::Normal)
        : session(config,
                  firmware::FactoryBootProfileFor(
                      profile == VirtualLitchBootProfile::Factory, false) !=
                          firmware::FactoryBootProfile::Normal
                      ? virtual_litch_internal::SessionMode::FactoryJourney
                      : virtual_litch_internal::SessionMode::EngineBackedJourney,
                  settingsPath,
                  engineOverride,
                  asyncThinkWorkerOverride) {
        session.SetBenchmarkRawControlsIdleProbe(this, [](void* context) {
            const auto& self = *static_cast<Impl*>(context);
            for (const bool down : self.rawDown) if (down) return false;
            return true;
        });
        ResetRawButtons();
    }

    virtual_litch_result Reset() {
        physicalHallMask = 0ULL;
        const virtual_litch_result result = session.Reset();
        ResetRawButtons();
        return result;
    }

    virtual_litch_result SetButtonDown(
        const uint8_t physicalOwner,
        const VirtualLitchPhysicalButton button,
        const bool down) {
        if (physicalOwner != VIRTUAL_LITCH_OWNER_P1 &&
            physicalOwner != VIRTUAL_LITCH_OWNER_P2) {
            return VIRTUAL_LITCH_INVALID_ENUM;
        }
        if (session.IsC3() &&
            physicalOwner == VIRTUAL_LITCH_OWNER_P2) {
            return VIRTUAL_LITCH_OUT_OF_RANGE;
        }
        if (button == VirtualLitchPhysicalButton::Press) {
            return VIRTUAL_LITCH_OK;
        }
        const uint8_t wireButton = static_cast<uint8_t>(button);
        if (wireButton > static_cast<uint8_t>(VirtualLitchPhysicalButton::O)) {
            return VIRTUAL_LITCH_INVALID_ENUM;
        }
        SyncOrientation();
        const size_t ownerOffset =
            physicalOwner == VIRTUAL_LITCH_OWNER_P2
                ? CONTROLS_PER_OWNER
                : 0U;
        const uint8_t controlIndex = wireButton > 4U ? wireButton - 1U : wireButton;
        rawDown[ownerOffset + controlIndex] = down;
        return PollButtons();
    }

    virtual_litch_result ObserveHallMask(
        const uint64_t nextPhysicalHallMask) {
        physicalHallMask = nextPhysicalHallMask;
        SyncOrientation();
        return session.DispatchBoardObservation(
            session.PhysicalOccupancyMask() ^
                MapPhysicalMaskToLogical(physicalHallMask, hallRotate),
            MapPhysicalMaskToLogical(physicalHallMask, hallRotate));
    }

    virtual_litch_result DispatchRuntimeHealthFact(
        const virtual_litch_runtime_health_fact& fact) {
        return session.DispatchRuntimeHealthFact(fact);
    }

    virtual_litch_result Advance(const uint32_t deltaMs) {
        SyncOrientation();
        const bool testOwned = (session.IsFactoryBootOnly() || session.ManufacturingTest().IsActive());
        const virtual_litch_result step = session.Advance(deltaMs);
        if (step != VIRTUAL_LITCH_OK) {
            return step;
        }
        SyncOrientation();
        if (testOwned && !(session.IsFactoryBootOnly() || session.ManufacturingTest().IsActive())) {
            ConfigureButtons(true);
        }
        return PollButtons();
    }

    virtual_litch_result MutateSettingJson(
        const uint16_t requestId,
        const std::string_view key,
        const std::string_view valueJson,
        modules::ControlMutationOutcome& outOutcome) {
        virtual_litch_setting_request request{};
        virtual_litch_default_setting_request(&request);
        if (key.size() >= sizeof(request.key) ||
            valueJson.size() >= sizeof(request.value_json)) {
            return VIRTUAL_LITCH_OUT_OF_RANGE;
        }
        std::snprintf(request.key,
                      sizeof(request.key),
                      "%.*s",
                      static_cast<int>(key.size()),
                      key.data());
        std::snprintf(request.value_json,
                      sizeof(request.value_json),
                      "%.*s",
                      static_cast<int>(valueJson.size()),
                      valueJson.data());
        request.request_id = requestId;
        return session.MutateSetting(request, outOutcome);
    }

    bool LoadExternalFen(const char* fen) {
        return session.LoadExternalFen(fen);
    }

    void ResetRawButtons() {
        rawDown.fill(false);
        controlsRotate = session.ControlBanksSwapped();
        hallRotate = session.HallRotate();
        ConfigureButtons(false);
    }

    void SyncOrientation() {
        const bool nextControlsRotate = session.ControlBanksSwapped();
        if (nextControlsRotate != controlsRotate) {
            controlsRotate = nextControlsRotate;
            session.ResetQuickSettingsInput();
            ConfigureButtons(true);
        }
        const bool nextHallRotate = session.HallRotate();
        if (nextHallRotate == hallRotate) return;
        hallRotate = nextHallRotate;
        const uint64_t logicalMask =
            MapPhysicalMaskToLogical(physicalHallMask, hallRotate);
        (void)session.DispatchBoardObservation(
            session.PhysicalOccupancyMask() ^ logicalMask,
            logicalMask);
    }

    void ConfigureButtons(const bool seedRawDown) {
        fxe::ScopedMillisProviderOverride timeOverride(
            &fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(session.NowMs());
        for (size_t index = 0U; index < CONTROL_COUNT; ++index) {
            const uint8_t physicalOwner =
                index < CONTROLS_PER_OWNER
                    ? VIRTUAL_LITCH_OWNER_P1
                    : VIRTUAL_LITCH_OWNER_P2;
            const auto physicalButton =
                static_cast<VirtualLitchPhysicalButton>(
                    index % CONTROLS_PER_OWNER < 4U
                        ? index % CONTROLS_PER_OWNER : index % CONTROLS_PER_OWNER + 1U);
            modules::ButtonEvent mapped{
                LogicalButtonFor(physicalOwner, physicalButton),
                fxe::Button::Event::Press,
                physicalOwner == VIRTUAL_LITCH_OWNER_P2
                    ? modules::UiOwner::P2
                    : modules::UiOwner::P1,
            };
            modules::ApplyRotateToButtonEvent(mapped, controlsRotate);
            auto config = ButtonConfig(mapped.button);
            buttons.ConfigureButton(
                index,
                mapped.button,
                mapped.owner,
                config,
                fxe::Button::DetectionFn(
                    [this, index]() { return rawDown[index]; }),
                seedRawDown);
        }
        buttons.Begin();
    }

    virtual_litch_result PollButtons() {
        fxe::ScopedMillisProviderOverride timeOverride(
            &fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(session.NowMs());
        buttons.Update();
        modules::ButtonEvent event;
        while (buttons.TryReadButtonEvent(event)) {
            const bool testOwned = (session.IsFactoryBootOnly() || session.ManufacturingTest().IsActive());
            const virtual_litch_result result = session.DispatchNormalizedButton(
                static_cast<uint8_t>(event.owner),
                static_cast<uint8_t>(event.button),
                static_cast<uint8_t>(event.event));
            if (result != VIRTUAL_LITCH_OK) {
                return result;
            }
            if (testOwned && !(session.IsFactoryBootOnly() || session.ManufacturingTest().IsActive())) {
                // Same Button::SeedRawDown boundary as production GPIO exit.
                ConfigureButtons(true);
                break;
            }
        }
        return VIRTUAL_LITCH_OK;
    }

    virtual_litch_internal::Session session;
    hardware::ButtonManager<CONTROL_COUNT> buttons;
    std::array<bool, CONTROL_COUNT> rawDown{};
    uint64_t physicalHallMask = 0ULL;
    bool controlsRotate = false;
    bool hallRotate = false;
};

VirtualLitchSession::VirtualLitchSession(const virtual_litch_config& config,
                                 const char* settingsPath)
    : m_impl(std::make_unique<Impl>(config, settingsPath)) {}

VirtualLitchSession::VirtualLitchSession(const virtual_litch_config& config,
                                        const VirtualLitchBootProfile profile,
                                        const char* settingsPath)
    : m_impl(std::make_unique<Impl>(config, settingsPath, nullptr, nullptr, profile)) {}

bool VirtualLitchSession::HasNormalRuntimeOwners() const {
    return m_impl->session.HasNormalRuntimeOwners();
}

void VirtualLitchSession::BindOwnerPairingRuntime(
    modules::BoardOwnerPairingRuntime& runtime) {
    m_impl->session.BindOwnerPairingRuntime(runtime);
}

VirtualLitchSession::VirtualLitchSession(
    const virtual_litch_config& config,
    modules::IEngine& engine,
    const char* settingsPath)
    : m_impl(std::make_unique<Impl>(config, settingsPath, &engine, nullptr)) {}

VirtualLitchSession::VirtualLitchSession(
    const virtual_litch_config& config,
    modules::IEngine& engine,
    modules::AsyncThinkWorker& asyncThinkWorker,
    const char* settingsPath)
    : m_impl(std::make_unique<Impl>(
          config, settingsPath, &engine, &asyncThinkWorker)) {}

VirtualLitchSession::~VirtualLitchSession() = default;
VirtualLitchSession::VirtualLitchSession(VirtualLitchSession&&) noexcept = default;
VirtualLitchSession& VirtualLitchSession::operator=(
    VirtualLitchSession&&) noexcept = default;

virtual_litch_result VirtualLitchSession::Reset() {
    return m_impl->Reset();
}

void VirtualLitchSession::RunManufacturingIngress(
    void (*callback)(void*, firmware::HardwareTestMode&, uint32_t), void* context) {
    const uint32_t runBefore = m_impl->session.ManufacturingTest().GetRunId();
    m_impl->session.RunManufacturingIngress(callback, context);
    m_impl->SyncOrientation();
    if (runBefore != m_impl->session.ManufacturingTest().GetRunId()) {
        m_impl->ConfigureButtons(true);
    }
}

const firmware::HardwareTestMode& VirtualLitchSession::ManufacturingTest() const {
    return m_impl->session.ManufacturingTest();
}

void VirtualLitchSession::SetManufacturingHallSnapshot(bool valid, uint64_t occupiedMask) {
    m_impl->session.SetManufacturingHallSnapshot(valid, occupiedMask);
}

virtual_litch_result VirtualLitchSession::DispatchControlAction(
    const virtual_litch_action_request& request,
    modules::ControlMutationOutcome& out) {
    return m_impl->session.DispatchControlAction(request, out);
}

bool VirtualLitchSession::TryTakeControlCompletion(
    modules::ControlMutationOutcome& out) {
    return m_impl->session.TryTakeControlCompletion(out);
}

virtual_litch_result VirtualLitchSession::SetButtonDown(
    const uint8_t physicalOwner,
    const VirtualLitchPhysicalButton button,
    const bool down) {
    return m_impl->SetButtonDown(physicalOwner, button, down);
}

virtual_litch_result VirtualLitchSession::DispatchRuntimeHealthFact(
    const virtual_litch_runtime_health_fact& fact) {
    return m_impl->DispatchRuntimeHealthFact(fact);
}

virtual_litch_result VirtualLitchSession::ObserveHallMask(
    const uint64_t physicalOccupiedMask) {
    return m_impl->ObserveHallMask(physicalOccupiedMask);
}

void VirtualLitchSession::ObserveBootHallMask(const uint64_t physicalOccupiedMask) {
    m_impl->physicalHallMask = physicalOccupiedMask;
    m_impl->SyncOrientation();
    m_impl->session.ObserveBootHallMask(
        MapPhysicalMaskToLogical(physicalOccupiedMask, m_impl->hallRotate));
}

void VirtualLitchSession::ObserveBootArtworkRecovery(const modules::litchcraft_frame_store::Status status) {
    m_impl->session.ObserveBootArtworkRecovery(status);
}
void VirtualLitchSession::BindLedOutputArbiter(hardware::RmtFlashArbiter& arbiter) {
    m_impl->session.BindLedOutputArbiter(arbiter);
}

virtual_litch_result VirtualLitchSession::Advance(const uint32_t deltaMs) {
    return m_impl->Advance(deltaMs);
}

void VirtualLitchSession::ObserveMotionSample(const hardware::Lis2dw12Sample& sample) {
    fxe::ScopedMillisProviderOverride timeOverride(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(m_impl->session.NowMs());
    m_impl->session.ObserveMotionSample(sample);
}

void VirtualLitchSession::ObserveWaterAmbientSample(const float lux,
                                                   const bool available) {
    m_impl->session.ObserveWaterAmbientSample(lux, available);
}

bool VirtualLitchSession::WaterSloshActive() const {
    return m_impl->session.WaterSloshActive();
}

virtual_litch_result VirtualLitchSession::MutateSettingJson(
    const uint16_t requestId,
    const std::string_view key,
    const std::string_view valueJson,
    modules::ControlMutationOutcome& outOutcome) {
    return m_impl->MutateSettingJson(
        requestId, key, valueJson, outOutcome);
}

bool VirtualLitchSession::LoadExternalFen(const char* fen) {
    return m_impl->LoadExternalFen(fen);
}

virtual_litch_result VirtualLitchSession::MutateSetting(
    const virtual_litch_setting_request& request,
    modules::ControlMutationOutcome& out) {
    return m_impl->session.MutateSetting(request, out);
}

void VirtualLitchSession::CaptureStartupFrame(const uint32_t nowMs,
                                          const uint16_t fadeScale256) {
    m_impl->session.CaptureStartupFrame(nowMs, fadeScale256);
}

void VirtualLitchSession::BeginStartupPieceAccent() {
    m_impl->session.BeginStartupPieceAccent();
}

void VirtualLitchSession::BeginBootDisplay() { m_impl->session.BeginBootDisplay(); }

const virtual_litch_state& VirtualLitchSession::State() const {
    return m_impl->session.State();
}

const virtual_litch_semantic_frame& VirtualLitchSession::SemanticFrame() const {
    return m_impl->session.SemanticFrame();
}

const virtual_litch_physical_frame& VirtualLitchSession::PhysicalFrame() const {
    return m_impl->session.PhysicalFrame();
}

uint64_t VirtualLitchSession::SidePrioritySquareMask() const {
    return m_impl->session.SidePrioritySquareMask();
}

bool VirtualLitchSession::QuickSettingsActive() const {
    return m_impl->session.QuickSettingsActive();
}

uint8_t VirtualLitchSession::QuickSettingsOwner() const {
    return m_impl->session.QuickSettingsOwner();
}

uint8_t VirtualLitchSession::QuickSettingsSelectedIndex() const {
    return m_impl->session.QuickSettingsSelectedIndex();
}

bool VirtualLitchSession::FirstPlayerIsP2() const {
    return m_impl->session.FirstPlayerIsP2();
}

bool VirtualLitchSession::BoardRotate() const {
    return m_impl->session.BoardRotate();
}

bool VirtualLitchSession::PlacementModeActive() const {
    return m_impl->session.PlacementModeActive();
}

bool VirtualLitchSession::KingQueenSwapActive() const {
    return m_impl->session.KingQueenSwapActive();
}

bool VirtualLitchSession::HintDisplayRequested() const {
    return m_impl->session.HintDisplayRequested();
}

modules::EdgeLedCueIntent VirtualLitchSession::EdgeCueIntent() const {
    return m_impl->session.EdgeCueIntent();
}

bool VirtualLitchSession::PromotionActive() const {
    return m_impl->session.PromotionActive();
}

modules::PromotionPiece VirtualLitchSession::PromotionSelection() const {
    return m_impl->session.PromotionSelection();
}

bool VirtualLitchSession::IsUndoActionPending() const {
    return m_impl->session.IsUndoActionPending();
}

uint8_t VirtualLitchSession::UndoHistoryMoveCount() const {
    return m_impl->session.UndoHistoryMoveCount();
}

const runtime::GameClock* VirtualLitchSession::GameClock() const {
    return m_impl->session.GameClock();
}

std::string_view VirtualLitchSession::DisplayText() const {
    return m_impl->session.DisplayText();
}

bool VirtualLitchSession::TryGetLastBoardControlOutcome(
    modules::ControlMutationOutcome& out) const {
    return m_impl->session.TryGetLastBoardControlOutcome(out);
}

bool VirtualLitchSession::TryGetHintMoveSquares(
    uint8_t& outFromSquare,
    uint8_t& outToSquare) const {
    return m_impl->session.TryGetHintMoveSquares(
        outFromSquare, outToSquare);
}

bool VirtualLitchSession::TryGetCoachInsight(
    modules::CoachInsight& outInsight) const {
    return m_impl->session.TryGetCoachInsight(outInsight);
}

bool VirtualLitchSession::TryGetHintDuckMoveSquares(
    uint8_t& outFromSquare,
    uint8_t& outToSquare) const {
    return m_impl->session.TryGetHintDuckMoveSquares(
        outFromSquare, outToSquare);
}

modules::GameVariant VirtualLitchSession::GameVariant() const {
    return m_impl->session.GameVariant();
}

uint8_t VirtualLitchSession::DuckSquare() const {
    return m_impl->session.DuckSquare();
}

uint8_t VirtualLitchSession::SetupDuckCandidateSquare() const {
    return m_impl->session.SetupDuckCandidateSquare();
}

bool VirtualLitchSession::CpuThinkingVisible() const {
    return m_impl->session.CpuThinkingVisible();
}

modules::HomeStateNotification VirtualLitchSession::HomeStateSnapshot() const {
    return m_impl->session.HomeStateSnapshot();
}

modules::HomeStateNotificationLine VirtualLitchSession::HomeStateLine() const {
    return m_impl->session.HomeStateLine();
}
void VirtualLitchSession::RunControlIngress(
    void (*callback)(void*, modules::SettingsStore&,
                     modules::ControlMutationService&, modules::GamePersistenceCoordinator&),
    void* context) {
    m_impl->session.RunControlIngress(callback, context);
}

bool VirtualLitchSession::CanPublishHomeState() const {
    return m_impl->session.CanPublishHomeState();
}

modules::BoardHealthLine VirtualLitchSession::BoardHealthStateLine() const {
    return m_impl->session.BoardHealthStateLine();
}

modules::BoardHealthConnectionState VirtualLitchSession::BoardHealthConnectionState() const {
    return m_impl->session.BoardHealthConnectionState();
}

bool VirtualLitchSession::TryPrepareHomePublication(
    modules::HomeStateNotificationPublisher& publisher,
    const bool force, modules::HomeStateNotificationLine& outLine) const {
    return m_impl->session.TryPrepareHomePublication(publisher, force, outLine);
}

modules::SettingsStore& VirtualLitchSession::SettingsForPersistenceTest() {
    return m_impl->session.SettingsForPersistenceTest();
}

bool VirtualLitchSession::SimulateBootPersistenceFailure(
    const modules::PersistenceDegradation degradation) {
    return m_impl->session.SimulateBootPersistenceFailure(degradation);
}

modules::PersistenceDegradation
VirtualLitchSession::PersistenceDegradation() const {
    return m_impl->session.PersistenceDegradation();
}

void VirtualLitchSession::SendLitchcraftPacket(const uint8_t* bytes, const size_t size) {
    m_impl->session.SendLitchcraftPacket(bytes,size);
}
void VirtualLitchSession::ShowIdentifyOverlay(const uint8_t number,
                                              const modules::Rgb color) {
    m_impl->session.ShowIdentifyOverlay(number, color);
}
void VirtualLitchSession::StopIdentifyOverlay() {
    m_impl->session.StopIdentifyOverlay();
}
void VirtualLitchSession::RunIdentifyIngress(
    void (*callback)(void*, modules::BleIdentifyPresentation&, uint32_t), void* context) {
    m_impl->session.RunIdentifyIngress(callback, context);
}
modules::LitchcraftSession& VirtualLitchSession::Litchcraft() {
    return m_impl->session.Litchcraft();
}
modules::LitchcraftPersistence& VirtualLitchSession::ArtPersistence() {
    return m_impl->session.ArtPersistence();
}

} // namespace runtime

#endif
