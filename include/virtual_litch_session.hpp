#pragma once

#include "firmware/factory_boot_intent.hpp"

#ifdef LITCH_NATIVE

#include <cstdint>
#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_persistence.hpp"
#include <memory>
#include <string_view>

#include "modules/control_mutation_service.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/game_variant.hpp"
#include "modules/home_state_notification.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/promotion_ux.hpp"
#include "runtime/virtual_litch_session.h"

namespace firmware { class HardwareTestMode; }
namespace hardware { struct Lis2dw12Sample; class RmtFlashArbiter; }

namespace modules {
class AsyncThinkWorker;
class GamePersistenceCoordinator;
class BoardOwnerPairingRuntime;
class BleIdentifyPresentation;
struct CoachInsight;
class IEngine;
} // namespace modules

namespace runtime {

class GameClock;

// Physical controls as seen from either end of the board. The adapter maps
// these through the same player-end/orientation semantics used by hardware.
enum class VirtualLitchPhysicalButton : uint8_t {
    Up = 0,
    Down,
    Left,
    Right,
    Press, // Reserved numeric identity; raw input is ignored.
    X,
    O,
};

// A native-only production-fidelity seam. Callers provide raw held levels and
// complete physical Hall occupancy; the adapter owns debounce and mapping and
// feeds the existing firmware control/coordinator/render owners.
using VirtualLitchBootProfile = firmware::FactoryBootProfile;

class VirtualLitchSession final {
  public:
    explicit VirtualLitchSession(const virtual_litch_config& config,
                              const char* settingsPath = nullptr);
    // Factory boot bypasses the same normal restoration/control owners. Native
    // SDK tasks, persistence receipts/NVS and reset delivery remain external.
    VirtualLitchSession(const virtual_litch_config& config,
                        VirtualLitchBootProfile profile,
                        const char* settingsPath = nullptr);
    bool HasNormalRuntimeOwners() const;
    // Synchronous native journey seam. The injected engine must outlive the
    // session; absence of a worker preserves the fixture's deterministic poll.
    VirtualLitchSession(const virtual_litch_config& config,
                        modules::IEngine& engine,
                        const char* settingsPath = nullptr);
    // Native journey seam for deterministic search-boundary coverage. The
    // engine and existing worker must both outlive this session.
    VirtualLitchSession(const virtual_litch_config& config,
                     modules::IEngine& engine,
                     modules::AsyncThinkWorker& asyncThinkWorker,
                     const char* settingsPath = nullptr);
    ~VirtualLitchSession();

    VirtualLitchSession(const VirtualLitchSession&) = delete;
    VirtualLitchSession& operator=(const VirtualLitchSession&) = delete;
    VirtualLitchSession(VirtualLitchSession&&) noexcept;
    VirtualLitchSession& operator=(VirtualLitchSession&&) noexcept;

    virtual_litch_result Reset();
    virtual_litch_result SetButtonDown(
        uint8_t physicalOwner,
        VirtualLitchPhysicalButton button,
        bool down);
    virtual_litch_result ObserveHallMask(uint64_t physicalOccupiedMask);
    // Initial sensor snapshot follows firmware startup's corrective admission.
    void ObserveBootHallMask(uint64_t physicalOccupiedMask);
    // Injects a sampled sensor value, using production mapping and liquid owner.
    void ObserveMotionSample(const hardware::Lis2dw12Sample& sample);
    // Injects a cached, freshness-qualified mean; no ambient sensor I/O or C ABI.
    void ObserveWaterAmbientSample(float lux, bool available);
    bool WaterSloshActive() const;
    void SendLitchcraftPacket(const uint8_t* bytes, size_t size);
    // Injects the accepted BLE Identify presentation at the same compositor
    // boundary used by firmware, without replacing Litchcraft mode ownership.
    void ShowIdentifyOverlay(uint8_t number, modules::Rgb color);
    void StopIdentifyOverlay();
    // Runs actual admitted BLE consumption at the authoritative render clock.
    void RunIdentifyIngress(void (*callback)(void*, modules::BleIdentifyPresentation&, uint32_t),
                            void* context);
    modules::LitchcraftSession& Litchcraft();
    modules::LitchcraftPersistence& ArtPersistence();
    virtual_litch_result DispatchRuntimeHealthFact(
        const virtual_litch_runtime_health_fact& fact);
    virtual_litch_result Advance(uint32_t deltaMs);
    // The production physical owner is supplied by the native fixture's
    // existing persistence adapter and must outlive this session.
    void BindOwnerPairingRuntime(modules::BoardOwnerPairingRuntime& runtime);
    virtual_litch_result MutateSettingJson(
        uint16_t requestId,
        std::string_view key,
        std::string_view valueJson,
        modules::ControlMutationOutcome& outOutcome);

    // Exercise the same bounded external-position import used by the
    // production console, then continue through ordinary Hall observations.
    bool LoadExternalFen(const char* fen);
    virtual_litch_result MutateSetting(
        const virtual_litch_setting_request& request,
        modules::ControlMutationOutcome& out);
    virtual_litch_result DispatchControlAction(
        const virtual_litch_action_request& request,
        modules::ControlMutationOutcome& out);
    bool TryTakeControlCompletion(modules::ControlMutationOutcome& out);

    // Startup-output seam. Time/transfer acceptance remains with the production
    // StartupLedSequence supplied by the journey; this does not boot hardware.
    void CaptureStartupFrame(uint32_t nowMs, uint16_t fadeScale256);
    // Model the real service handoff; subsequent Hall/frames gate this episode.
    void BeginStartupPieceAccent();
    // Same service-ready boundary as normal firmware, before the first Hall fact.
    void BeginBootDisplay();
    // Supply actual worker recovery facts; Ok can precede stream publication.
    void ObserveBootArtworkRecovery(modules::litchcraft_frame_store::Status status);
    // Native transfer acceptance uses the production flash/render lease owner.
    // The caller-owned arbiter must outlive this session.
    void BindLedOutputArbiter(hardware::RmtFlashArbiter& arbiter);
    // Fixture facts enter the existing manufacturing owner at session time.
    // Buttons/Halls continue through raw production-equivalent input above.
    void RunManufacturingIngress(
        void (*callback)(void*, firmware::HardwareTestMode&, uint32_t), void* context);
    // Set raw IBoardInput facts without observing them yet. The next
    // RunManufacturingIngress delivers sensors after any queued button input.
    void SetManufacturingHallSnapshot(bool valid, uint64_t occupiedMask);
    const firmware::HardwareTestMode& ManufacturingTest() const;

    const virtual_litch_state& State() const;
    const virtual_litch_semantic_frame& SemanticFrame() const;
    const virtual_litch_physical_frame& PhysicalFrame() const;
    // Effective compositor-output ownership before final square rotation.
    uint64_t SidePrioritySquareMask() const;

    bool QuickSettingsActive() const;
    uint8_t QuickSettingsOwner() const;
    uint8_t QuickSettingsSelectedIndex() const;
    bool FirstPlayerIsP2() const;
    bool BoardRotate() const;
    bool PlacementModeActive() const;
    bool KingQueenSwapActive() const;
    bool HintDisplayRequested() const;
    modules::EdgeLedCueIntent EdgeCueIntent() const;
    bool PromotionActive() const;
    modules::PromotionPiece PromotionSelection() const;
    bool IsUndoActionPending() const;
    // Read-only access to the existing production clock for timed journeys.
    const runtime::GameClock* GameClock() const;
    uint8_t UndoHistoryMoveCount() const;
    std::string_view DisplayText() const;
    bool TryGetLastBoardControlOutcome(
        modules::ControlMutationOutcome& out) const;
    bool TryGetHintMoveSquares(uint8_t& outFromSquare,
                               uint8_t& outToSquare) const;
    bool TryGetCoachInsight(modules::CoachInsight& outInsight) const;
    bool TryGetHintDuckMoveSquares(uint8_t& outFromSquare,
                                   uint8_t& outToSquare) const;
    modules::GameVariant GameVariant() const;
    uint8_t DuckSquare() const;
    uint8_t SetupDuckCandidateSquare() const;
    bool CpuThinkingVisible() const;
    modules::HomeStateNotification HomeStateSnapshot() const;
    modules::HomeStateNotificationLine HomeStateLine() const;
    bool CanPublishHomeState() const;
    modules::BoardHealthLine BoardHealthStateLine() const;
    modules::BoardHealthConnectionState BoardHealthConnectionState() const;
    // Native transport wiring executes at the session's authoritative clock.
    // The callback receives existing owners, never a substitute controller.
    void RunControlIngress(
        void (*callback)(void*, modules::SettingsStore&,
                         modules::ControlMutationService&, modules::GamePersistenceCoordinator&),
        void* context);
    bool TryPrepareHomePublication(
        modules::HomeStateNotificationPublisher& publisher,
        bool force, modules::HomeStateNotificationLine& outLine) const;
    modules::SettingsStore& SettingsForPersistenceTest();
    bool SimulateBootPersistenceFailure(
        modules::PersistenceDegradation degradation);
    modules::PersistenceDegradation PersistenceDegradation() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace runtime

#endif
