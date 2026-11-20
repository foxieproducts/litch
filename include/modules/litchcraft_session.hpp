#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "modules/board_led_frame.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/litchcraft_history.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_presentation.hpp"
#include "modules/litchcraft_shake_detector.hpp"
#include "modules/user_input.hpp"
#include "modules/tap_hold_gesture_controller.hpp"
#include "modules/control_led_feedback.hpp"
#include "trace/support_action_history.hpp"

namespace modules {

class LitchcraftPersistence;

using LitchcraftQuickPalette = std::array<Rgb, 8U>;
inline constexpr LitchcraftQuickPalette LITCHCRAFT_DEFAULT_QUICK_PALETTE{{
    {255U, 116U, 48U}, {255U, 52U, 82U}, {255U, 207U, 45U},
    {55U, 215U, 105U}, {34U, 195U, 226U}, {63U, 106U, 255U},
    {174U, 83U, 255U}, {255U, 255U, 255U},
}};

constexpr bool LitchcraftCanvasOwnsFrame(const bool active,
                                         const bool runtimePaused,
                                         const bool identifyOverlayActive) {
    return active && !runtimePaused && !identifyOverlayActive;
}

// V1 wire contract: all integers little-endian; every packet fits ATT MTU 23.
// Logical elements are square (a8=0 .. h1=63) * 4 + Left/Top/Right/Bottom.
// V1/V2 center targets use mask 10. Their output mapper averages Top/Bottom.
class LitchcraftSession final {
  public:
    static constexpr uint8_t MAGIC = 0x4c;
    static constexpr uint8_t BULK_REPLACE = 0x40;
    static constexpr size_t ELEMENT_COUNT = 256U;
    static constexpr uint32_t PHYSICAL_RELEASE_MS = 350U;
    // Face buttons are physically separate controls. Allow a normal human
    // stagger while keeping single-button actions responsive.
    static constexpr uint32_t MODE_CHORD_PRIORITY_MS = 750U;
    static constexpr uint32_t ENTRY_HOLD_MS = 1500U;
    static constexpr uint32_t PICK_ARM_MS = 1000U;
    static constexpr uint32_t PICK_PROGRESS_MS = 1000U;
    static constexpr uint32_t PICK_CONFIRM_MS = 600U;
    static constexpr uint32_t ERASE_WIPE_MS = 480U;
    static constexpr uint32_t ERASE_MARKER_PHASE_MS = 750U;
    static constexpr uint32_t QUICK_PALETTE_IDLE_MS = 2000U;
    static constexpr uint32_t QUICK_PALETTE_FADE_MS = 300U;
    static constexpr uint32_t SHAKE_ERASE_ANIMATION_MS = 1000U;
    static constexpr uint32_t REPLACE_TIMEOUT_MS = 10000U;
    enum class Op : uint8_t { Enter = 1,
                              Exit,
                              Tool,
                              Paint,
                              Undo,
                              Redo,
                              Clear,
                              Snapshot,
                              ReplaceBegin,
                              ReplaceChunk,
                              ReplaceCommit,
                              Inspect,
                              PaletteChunk,
                              ShakeErase };
    enum class Event : uint8_t { State = 0x81,
                                 Tool,
                                 Cell,
                                 SnapshotBegin,
                                 SnapshotChunk,
                                 SnapshotEnd,
                                 ResyncRequired,
                                 ColorPick,
                                 Gesture };
    enum class Status : uint8_t { Ok = 0,
                                  Busy,
                                  StaleSession,
                                  Malformed,
                                  Unsupported,
                                  NoHistory,
                                  Incomplete,
                                  StaleRequest,
                                  BoardNotEmpty,
                                  RevisionConflict,
                                  ResourceLimit,
                                  IntegrityFailure,
                                  EntryPending };
    enum class ModeChordResult : uint8_t {
        Ignored = 0,
        Pending,
        Entered,
        Blocked,
        Replay,
        Cancelled,
    };
    using Canvas = LitchcraftHistoryCanvas;
#if defined(LITCH_ESP32S3)
    LitchcraftSession() = default;
#else
    LitchcraftSession() : m_history() {}
#endif
    void SetSupportActions(trace::SupportActionHistory* actions) { m_supportActions = actions; }
    struct Packet {
        std::array<uint8_t, 20> bytes{};
        uint8_t size = 0;
    };
    struct BulkPacket {
        std::array<uint8_t, 244> bytes{};
        uint16_t size = 0;
    };
    struct AdvancedMutationDiagnostic {
        uint32_t count = 0U;
        uint32_t revision = 0U;
        uint8_t op = 0U;
        uint8_t detail = 0U;
        uint8_t frames = 0U;
    };
    struct PhysicalStoreEdit {
        uint32_t revision = 0U;
        uint32_t generation = 0U;
        uint8_t frame = 0U;
        uint8_t square = 0U;
        uint8_t mask = 0U;
        Rgb color{};
        bool clearEffects = false;
    };
    static constexpr uint8_t PHYSICAL_STORE_BATCH_EVENT = 0x91U;
    static constexpr uint8_t PHYSICAL_STORE_BATCH_CAPACITY = 16U;
    static constexpr uint8_t PHYSICAL_STORE_BATCH_RECORD_BYTES = 21U;
    const AdvancedMutationDiagnostic& LastAdvancedMutation() const {
        return m_lastAdvancedMutation;
    }

    void Configure(uint8_t hardwareRevision, uint32_t bootNonce);
    void AttachAdvancedPersistence(LitchcraftPersistence* persistence);
    void AttachFrameStream(
        litchcraft_frame_stream::FrameStream* stream,
        void (*wakeStorage)(void*, bool, uint32_t, bool) = nullptr,
        void* wakeStorageContext = nullptr);
    void RestoreArtwork(const Canvas& canvas, bool active, uint64_t occupancy,
                        const LitchcraftQuickPalette* quickPalette = nullptr,
                        bool bootOccupancyKnown = false);
    bool ConsumeExitFlushRequested() {
        const bool requested = m_exitFlushRequested;
        m_exitFlushRequested = false;
        return requested;
    }
    void SetMutationsPaused(bool paused);
    uint32_t CanvasGeneration() const { return m_canvasGeneration; }
    uint32_t CanvasChangedAtMs() const { return m_canvasChangedAtMs; }
    uint32_t ModeGeneration() const { return m_modeGeneration; }
    uint32_t ModeChangedAtMs() const { return m_modeChangedAtMs; }
    uint32_t PaletteGeneration() const { return m_paletteGeneration; }
    uint32_t PaletteChangedAtMs() const { return m_paletteChangedAtMs; }
    // Transport lifecycle only: artwork and physical mode remain authoritative.
    void BeginConnection();
    void EndConnection();
    void HandlePacket(const uint8_t* bytes, size_t size, bool entryEligible, uint64_t occupancy);
    Status HandleCompactMode(const uint8_t* bytes, size_t size,
                             bool entryEligible, uint64_t occupancy);
    bool ConsumeOccupiedEntryRejected() {
        const bool rejected = m_occupiedEntryRejected;
        m_occupiedEntryRejected = false;
        return rejected;
    }
    // High-MTU artwork frames arrive through the already allocated OTA data
    // lane. They stage into the same replacement canvas and publish one Ack.
    void HandleBulkPacket(const uint8_t* bytes, size_t size,
                          bool entryEligible, uint64_t occupancy);
    void ObserveOccupancy(uint64_t occupiedMask);
    void ResynchronizeOccupancy(uint64_t occupiedMask);
    void ConfigureShakeErase(bool supported);
    void ObserveMotionSample(int32_t xMg, int32_t yMg, int32_t zMg);
    void SetControlFeedback(ControlLedFeedback* feedback) { m_controlFeedback = feedback; }
    void FillControlFeedbackInputs(ControlLedFeedbackInputs& inputs) const;
    void ObserveButton(const ButtonEvent& event);
    void ObserveButton(const ButtonEvent& event, uint32_t eventTimeMs);
    ModeChordResult ObserveEntryButton(const ButtonEvent& event,
                                       uint32_t eventTimeMs, bool eligible,
                                       bool blockedByOccupancy = false,
                                       uint64_t occupancy = 0U);
    bool TryExpireEntryButton(uint32_t nowMs, ButtonEvent& outEvent,
                              uint32_t& outEventTimeMs);
    // Borrow the solitary buffered edge for presentation without consuming it.
    bool TryGetPendingEntryButton(ButtonEvent& outEvent,
                                  uint32_t& outEventTimeMs) const;
    bool TakeEntryButtonForReplay(ButtonEvent& outEvent,
                                  uint32_t& outEventTimeMs);
    void CancelEntryHold();
    void SetPhysicalButtonTicksAllowed(const bool allowed) { m_physicalButtonTicksAllowed = allowed; }
    void SetPhysicalButtonTickTime(const uint32_t throughMs) {
        m_physicalButtonTickMs = throughMs;
        m_physicalButtonTickTimeValid = true;
    }
    void Update();
    void Exit();
    bool ConsumeExited();
    bool ConsumeActivity();
    bool PeekPacket(Packet& out);
    bool PeekPacket(BulkPacket& out, size_t maximumPayload);
    void AcceptPacket();
    void RequireResync();
    bool IsActive() const {
        return HasStreamedDocument() &&
                m_streamGeneration != m_frameStream->Generation()
            ? m_frameStream->Active() && (m_active || m_modeGeneration == 0U)
            : m_active;
    }
    // Exit acknowledges visually before storage completes. Input authority is
    // retained by IsActive until durable success, or the canvas returns on failure.
    bool IsCanvasVisible() const { return IsActive(); }
    bool IsInitialStoreModePending() const {
        return m_frameStream != nullptr && m_modeGeneration == 0U &&
            m_frameStream->InitialRecoveryPending();
    }
    bool IsEntryHoldActive() const { return !m_active && m_modeChordPending; }
    bool IsExitHoldActive() const {
        return m_active && m_modeChordPending && m_modeChordFormed;
    }
    bool OwnsButton(const ButtonEvent&) const { return IsActive(); }
    bool IsWaitingForLift() const { return m_blockedOccupancy != 0U; }
    uint32_t SessionId() const { return m_session; }
    uint32_t Revision() const { return m_revision; }
    uint64_t Occupancy() const { return m_occupancy; }
    const Canvas& Artwork() const { return m_canvas; }
    uint8_t Target() const { return m_target; }
    Rgb Color() const { return m_color; }
    bool IsErasing() const { return m_erase; }
    uint16_t FrameCount() const;
    bool BeginBootArtworkOnce();
    void UpdateBootArtwork();
    void CancelBootArtwork();
    void NoteBootArtworkPresented();
    bool IsBootArtworkPlaying() const { return (m_bootArtworkFlags & 1U) != 0U; }
    bool BootArtworkFinished() const { return (m_bootArtworkFlags & 2U) != 0U; }
    uint8_t SelectedFrame() const {
        return StoreIsSoleArtworkOwner() && !HasStreamedDocument()
            ? 0U : m_advancedSelectedFrame;
    }
    bool IsAnimationPlaying() const {
        return m_advancedPlaying &&
            (!StoreIsSoleArtworkOwner() || HasStreamedDocument());
    }
    bool BulkTransferActive() const {
        return m_replacing || m_snapshotActive || m_v2Transfer != 0U || m_v2SnapshotActive;
    }
    void SetBulkTransferActive(bool active);
    bool IsPlaybackTransferSuspended() const { return m_bulkTransferActive; }
    uint16_t AnimationFrameElapsedMs() const;
    uint8_t AnimationTopology() const;
    uint64_t AnimationElapsedMs() const { return m_advancedElapsed; }
    const LitchcraftQuickPalette& RecentColors() const { return m_recentColors; }
    bool IsQuickPaletteVisible() const { return m_quickPaletteVisible; }
    bool IsShakeEraseSupported() const { return m_shakeEraseSupported; }
    uint16_t LastShakeEraseToken() const { return m_shakeEraseToken; }
    uint8_t UndoCount() const;
    uint8_t RedoCount() const;
    // Connection-session ordered adapter. It applies only the already-owned
    // atomic paint/effect and exact-history opcodes; lifecycle, ambiguous
    // history recovery and bulk operations stay with their existing owners.
    Status ApplyConnectionOrdered(uint8_t opcode, const uint8_t* payload,
                                  size_t size,
                                  uint32_t connectionEpoch = 0U);
    uint32_t ConnectionRevision() const;
    litchcraft_frame_stream::FrameStream::LiveAuthority
    StoreLiveAuthority() const;
    uint32_t StoreGuardRevision() const { return m_storeGuardRevision; }
    bool IsStoreHistoryPending() const { return m_pendingStoreHistoryOrdered; }
    void CancelPendingStoreHistory() {
        if (m_pendingStoreHistoryOrdered && m_pendingStoreStructural && m_frameStream != nullptr) {
            m_frameStream->CancelStructuralEdit();
            m_pendingStoreStructural = false;
            m_pendingStoreOpcode = 0U;
        }
        if (!m_pendingStoreHistoryOrdered) return;
        if (m_frameStream != nullptr && !m_frameStream->ReplacementPending() &&
            m_advancedSelectedFrame == m_pendingPhysicalHistoryFrame &&
            m_frameStream->LiveRevision() == m_pendingPhysicalHistoryRevision) {
            const uint16_t displayed = m_frameStream->DisplayedIndex();
            if (displayed < m_frameStream->FrameCount()) {
                m_advancedSelectedFrame = static_cast<uint8_t>(displayed);
                (void)m_frameStream->Request(displayed);
            }
        }
        m_pendingStoreHistoryOrdered = false;
        m_pendingStoreOpcode = 0U;
        m_pendingPhysicalHistoryFrame = UINT16_MAX;
    }
    bool CopyPhysicalStoreEdit(PhysicalStoreEdit& out) const;
    bool PeekPhysicalStoreBatch(uint8_t* out, size_t capacity,
                                size_t& outSize);
    void AcceptPhysicalStoreBatch();
    // Samples the advanced playback cursor once, when present, then presents
    // its semantic frame through the existing topology/power pipeline.
    void Compose(BoardLedFrame& board,
                 std::array<Rgb, EDGE_LED_COUNT>& status);
    enum class PresentationAccess : uint8_t { None, Control, Frame };
    PresentationAccess PacketPresentationAccess(const uint8_t* bytes, size_t size) const;
    PresentationAccess OrderedPresentationAccess(uint8_t opcode) const;
    bool PacketWaitsForPresentedFrame(const uint8_t* bytes, size_t size) const;
    PresentationAccess ButtonPresentationAccess(const ButtonEvent& event) const;
    bool PresentationFramePending() const;
    bool CanMutatePresentedFrame() const;
    bool OrderedWaitsForPresentedFrame(uint8_t opcode, const uint8_t* bytes, size_t size) const;
    PresentationAccess UpdatePresentationAccess() const;
    void CapturePresentation(litchcraft_presentation::Inputs& inputs);
    void SetIndependentPresentation(bool enabled);
    bool ApplyPresentationFeedback(const litchcraft_presentation::Feedback& feedback);
    litchcraft_presentation::EntryHold CaptureEntryHoldPresentation() const {
        return {m_modeChordStartedMs, m_modeChordFormed};
    }
    void ComposeEntryStatus(std::array<Rgb, EDGE_LED_COUNT>& status) const;

  private:
    trace::SupportActionHistory* m_supportActions = nullptr;
    uint32_t m_supportStroke = 0U;
    uint64_t m_supportStrokeSquares = 0U;
    friend class LitchcraftPersistence;
    enum class Source : uint8_t { None,
                                  App,
                                  Board };
    struct Receipt {
        // Small control packets remain exact bytes. Long transfer packets use
        // the project's SHA-256 identity, retaining all 32 replay receipts
        // without reserving a maximum-MTU packet body for every command.
        std::array<uint8_t, 32U> identity{};
        uint16_t request = 0U;
        uint16_t size = 0U;
        bool advanced = false;
        Status status = Status::Ok;
        Packet response{};
        static std::array<uint8_t, 32U> Identity(const uint8_t* bytes, size_t size);
        void Remember(const uint8_t* bytes, size_t length,
                      const std::array<uint8_t, 32U>& fingerprint);
        bool Matches(size_t length, const std::array<uint8_t, 32U>& fingerprint) const;
    };
    Packet Header(Event event, uint16_t request, uint32_t revision) const;
    Packet StatePacket(Event event, uint16_t request, Status status) const;
    Packet ToolPacket(uint16_t request) const;
    Packet ColorPickPacket() const;
    Packet GesturePacket(uint8_t action, uint16_t token) const;
    void PublishColorPickStatus();
    void Queue(const Packet& packet);
    void Ack(uint16_t request, Status status = Status::Ok);
    void StartSnapshot(uint16_t request, bool bulk = false);
    void EndStroke();
    void StartStroke(Source source, uint16_t stroke = 0U);
    bool PaintSquare(uint8_t square, uint16_t request);
    void QueueCell(uint8_t square, uint8_t target, Rgb color, uint16_t request);
    void QueueCanvasChanges(const Canvas& previous, uint16_t request);
    void PaintFootprint(uint64_t footprint, uint16_t request);
    void ChangeTool(bool erase, uint8_t target, Rgb color, uint16_t request,
                    bool rememberColor = true);
    void SelectTool(bool erase, uint8_t target, Rgb color, uint16_t request,
                    bool rememberColor = true);
    void RememberColor(Rgb color);
    Status ApplyPaletteChunk(const uint8_t* payload, size_t size);
    void OpenOrStepQuickPalette(const ButtonEvent& event);
    bool SelectQuickPaletteFromOccupancy(uint64_t occupancy);
    void CenterQuickPaletteWindow();
    void CloseQuickPalette();
    uint8_t QuickPaletteSquare(uint8_t index) const;
    uint8_t ColorPickBlendAlpha(uint32_t nowMs) const;
    void BlendColorPickPreview(BoardSquareLeds& square, uint32_t nowMs) const;
    Rgb ColorPickSample(uint32_t nowMs) const;
    void CompleteColorPick(Rgb sample);
    Status Apply(Op op, const uint8_t* payload, size_t size, uint16_t request, bool eligible, uint64_t occupancy);
    Status HandleCompactPacket(const uint8_t* bytes, size_t size,
                               bool entryEligible, uint64_t occupancy);
    void Remember(const uint8_t* bytes, size_t size, Status status);
    static uint16_t Read16(const uint8_t* bytes);
    static uint32_t Read32(const uint8_t* bytes);
    void MarkCanvasChanged();
    void MarkModeChanged();
    void MarkPaletteChanged();
    void FinishExit();
    void EnterFromBoardControls(uint64_t occupancy);
    void ObservePhysicalButton(const ButtonEvent& event);
    void UpdatePhysicalButtons();
    void ResetPhysicalButtons();
    void PerformPhysicalFaceAction(bool xButton, const TapHoldAction& action);
    void ReportPhysicalFeedback(ControlFeedbackAction action, UiOwner owner,
                                ControlRole role, Rgb color = {});
    bool ReplaceCanvas(const Canvas& canvas);
    bool HandleV2Packet(const uint8_t* bytes, size_t size, bool eligible,
                        uint64_t occupancy);
    bool PeekV2Packet(BulkPacket& out, size_t maximumPayload);
    void AcceptV2Packet();
    void RetireV2Transfer();
    void UpdateV2Transport();
    Packet V2State(uint16_t request, Status status) const;
    // Request replies select their wire contract explicitly, including refusals.
    Packet V2State(uint16_t request, Status status, uint8_t operation) const;
    bool HasAdvancedDocument() const;
    PresentationAccess FramePresentationAccess() const;
    litchcraft_presentation::Playback CapturePresentationPlayback() const;
    void ApplyPresentationPlayback(const litchcraft_presentation::Playback& playback);
    bool HasStreamedDocument() const;
    bool StoreIsSoleArtworkOwner() const {
#if defined(LITCH_ESP32S3)
        // Store ownership precedes worker/stream startup. Missing authority
        // must refuse artwork rather than enter an unowned legacy fallback.
        return true;
#else
        return m_frameStream != nullptr && m_advancedPersistence == nullptr;
#endif
    }
    void InitializeAdvancedRuntime();
    void ResetAdvancedPlayback();
    void SampleAdvancedPlayback();
    void SetAnimationPlaying(bool playing);
    uint16_t PlaybackStatusScale256() const;
    void PauseAdvancedPlayback();
    void SelectAdvancedFrame(uint8_t frame);
    void QueueTimeline(uint16_t request);
    void SyncAdvancedCanvas();
    void ResetStreamedPlayback();
    void SampleStreamedPlayback();
    void PinStreamedFrameForEdit();
    void WakeStreamedStorage();
    void AdoptStreamedModeAuthority();
    void RecordAdvancedMutation(uint8_t beforeFrame, bool extend);
    bool CommitAdvancedEdit(uint8_t beforeFrame, bool extend);
    Status ApplyV2Edit(uint8_t op, const uint8_t* payload, size_t size);
    Status ApplyStoreRuntime(const uint8_t* payload, size_t size);
    Status ApplyOptimisticControl(uint16_t request, const uint8_t* payload,
                                  size_t size);
    Status ApplyOptimisticEdit(uint8_t op, uint16_t sequence,
                               const uint8_t* payload, size_t size);
    Packet OptimisticWatermark(Status status) const;
    Status ApplyV2Stroke(uint8_t op, const uint8_t* payload, size_t size);
    Status ApplyV2FrameEdit(const uint8_t* payload, size_t size);
    Status ApplyV2CanvasEdit(const uint8_t* payload, size_t size);
    Status AdvancedUndoRedo(bool redo);
    void PaintAdvancedFootprint(uint64_t footprint);
    void PaintStreamedFootprint(uint64_t footprint);
    void RecordPhysicalStoreEdit(uint64_t footprint, Rgb color, uint8_t mask,
                                 bool clearEffects);
    void RecordPhysicalStoreEdit(
        uint64_t footprint, Rgb color, uint8_t mask, bool clearEffects,
        const litchcraft_frame_stream::FrameStream::LiveAuthority& authority,
        uint8_t frame);
    void CompleteStreamedColorPick(Rgb sample);
    void ResetPhysicalStoreBatch();
    Status ApplyStoreStroke(uint8_t op, const uint8_t* payload, size_t size,
                            uint32_t connectionEpoch);
    Status ApplyStoreCanvas(const uint8_t* payload, size_t size,
                            uint32_t connectionEpoch);
    Status ApplyStoreFrameEdit(const uint8_t* payload, size_t size,
                              uint32_t connectionEpoch);
    Status ApplyStoreNewCanvas(const uint8_t* payload, size_t size,
                              uint32_t connectionEpoch);
    Status CompleteStoreStructural();
    Status ApplyStoreHistory(bool redo, size_t size,
                             uint32_t connectionEpoch);
    Status ApplyPhysicalStoreHistory(bool redo = false, UiOwner owner = UiOwner::None);
    void PublishPhysicalStoreHistoryStep(uint32_t revision);
    void CompleteAdvancedColorPick(Rgb sample);
    bool ShakeEraseEligible() const;
    bool FrameEraseEligible() const;
    bool ApplyFrameErase(bool shake);
    bool ApplyShakeErase();
    void UpdateAdvancedFavorites();
    struct PhysicalEditScope {
        explicit PhysicalEditScope(LitchcraftSession& owner);
        ~PhysicalEditScope();
        LitchcraftSession& session;
    };

    Canvas m_canvas{};
    uint32_t m_canvasGeneration = 0U;
    uint32_t m_canvasChangedAtMs = 0U;
    uint32_t m_modeGeneration = 0U;
    uint32_t m_modeChangedAtMs = 0U;
    uint32_t m_paletteGeneration = 0U;
    uint32_t m_paletteChangedAtMs = 0U;
    bool m_restoreOccupancyPending = false;
    bool m_exitFlushRequested = false;
#if !defined(LITCH_ESP32S3)
    union {
        LitchcraftHistory m_history;
        LitchcraftHistory m_advancedHistory;
    };
#endif
    bool m_advancedRuntime = false;
    bool m_advancedPlaying = false;
    bool m_bulkTransferActive = false;
    uint32_t m_bulkTransferEpoch = 0U;
    bool m_playbackSuspended = false;
    uint16_t m_playbackStatusFrom256 = 256U;
    uint32_t m_playbackStatusChangedAtMs = 0U;
    bool m_advancedPhysicalCommand = false;
    bool m_advancedCommandChanged = false;
    bool m_advancedCommandHasHistory = false;
    uint8_t m_advancedSelectedFrame = 0U;
    uint16_t m_pendingPhysicalHistoryFrame = UINT16_MAX;
    bool m_pendingStoreHistoryOrdered = false;
    bool m_pendingStoreStructural = false;
    uint8_t m_pendingStoreOpcode = 0U;
    uint8_t m_pendingStoreFrameAction = 0U;
    uint32_t m_pendingPhysicalHistoryRevision = 0U;
    uint32_t m_pendingPhysicalHistoryAtMs = 0U;
    uint8_t m_advancedStrokeFrame = 0U;
    // Reuse clock-alignment padding for the two boot-only cursor bytes.
    uint8_t m_bootArtworkFlags = 0U;
    uint8_t m_bootResumeFrame = 0U;
    uint64_t m_advancedElapsed = 0U;
    uint64_t m_advancedEffectElapsed = 0U;
    uint32_t m_advancedClock = 0U;
    litchcraft_presentation::Playback m_presentationCapturedPlayback{};
    litchcraft_presentation::Visuals m_presentationCapturedVisuals{};
    litchcraft_presentation::Context m_presentationCapturedContext{};
    litchcraft_presentation::Playback m_presentationActualPlayback{};
    litchcraft_presentation::Context m_presentationActualContext{};
    uint32_t m_presentationIntentEpoch = 0U;
    uint32_t m_presentationEpoch = 0U;
    uint32_t m_presentationFeedbackSequence = 0U;
    litchcraft_presentation::Source m_presentationSource = litchcraft_presentation::Source::Static;
    bool m_independentPresentation = false;
    bool m_presentationCaptured = false;
    bool m_presentationActual = false;
    uint16_t m_timelineSample = 0U;
    Canvas m_strokeBefore{};
    Source m_strokeSource = Source::None;
    uint16_t m_strokeId = 0U;
    bool m_strokeChanged = false;
    uint32_t m_releaseAtMs = 0U;
    bool m_releasePending = false;
    std::array<Rgb, 4U> m_pickBefore{};
    std::array<litchcraft_frame_store::Emitter, 4U> m_pickBeforeEmitters{};
    uint8_t m_pickSquare = 0U;
    uint32_t m_pickDocumentEpoch = 0U;
    uint32_t m_pickStartedMs = 0U;
    uint32_t m_pickConfirmedMs = 0U;
    bool m_pickPending = false;
    bool m_pickConfirmed = false;
    bool m_pickPriorStrokeChanged = false;
    Canvas m_replacement{};
    std::bitset<ELEMENT_COUNT> m_received{};
    bool m_replacing = false;
    uint32_t m_replaceAtMs = 0U;
    uint32_t m_session = 0U;
    uint32_t m_revision = 0U;
    uint32_t m_nonce = 1U;
    uint8_t m_hardware = 2U;
    // Legacy State byte: index in the original fixed palette, or 255 for a
    // custom/favorite color. New quick-palette ordering is separate.
    uint8_t m_palette = 0U;
    LitchcraftQuickPalette m_recentColors = LITCHCRAFT_DEFAULT_QUICK_PALETTE;
    LitchcraftQuickPalette m_paletteStaging = LITCHCRAFT_DEFAULT_QUICK_PALETTE;
    uint8_t m_paletteStagingCount = 0U;
    LitchcraftQuickPalette m_quickPaletteColors = LITCHCRAFT_DEFAULT_QUICK_PALETTE;
    UiOwner m_quickPaletteOwner = UiOwner::None;
    uint8_t m_quickPaletteIndex = 0U;
    uint8_t m_quickPaletteWheelIndex = 0U;
    uint8_t m_quickPaletteWindowStart = 0U;
    uint32_t m_quickPaletteTouchedMs = 0U;
    bool m_quickPaletteVisible = false;
    uint32_t m_quickPaletteFadeAtMs = 0U;
    bool m_quickPaletteFading = false;
    uint8_t m_colorWheel = 0U;
    Rgb m_color{255U, 116U, 48U};
    uint8_t m_target = 15U;
    bool m_erase = false;
    bool m_eraseCueStarted = false;
    uint32_t m_eraseSelectedMs = 0U;
    LitchcraftShakeDetector m_shakeDetector{};
    uint32_t m_shakeEraseAnimationAtMs = 0U;
    uint16_t m_shakeEraseToken = 0U;
    bool m_shakeEraseSupported = false;
    bool m_shakeEraseAnimationActive = false;
    bool m_active = false;
    bool m_mutationsPaused = false;
    bool m_exited = false;
    bool m_activity = false;
    uint64_t m_occupancy = 0U;
    uint64_t m_blockedOccupancy = 0U;
    ControlLedFeedback* m_controlFeedback = nullptr;
    bool m_physicalButtonTicksAllowed = true;
    bool m_physicalButtonTickTimeValid = false;
    uint32_t m_physicalButtonTickMs = 0U;
    TapHoldGestureController m_physicalX;
    TapHoldGestureController m_physicalO;
    std::array<uint8_t, 2> m_physicalFaceDown{};
    std::array<uint32_t, 2> m_physicalFaceStartedMs{};
    bool m_pendingPhysicalHistoryRedo = false;
    UiOwner m_pendingPhysicalHistoryOwner = UiOwner::None;
    ButtonEvent m_modeChordEvent{};
    bool m_modeChordPending = false;
    bool m_modeChordFormed = false;
    bool m_modeChordReleasePending = false;
    uint32_t m_modeChordStartedMs = 0U;
    static constexpr size_t QUEUE_CAPACITY = 96U;
    std::array<Packet, QUEUE_CAPACITY> m_queue{};
    size_t m_queueHead = 0U;
    size_t m_queueCount = 0U;
    uint8_t m_priorityQueueOffset = 0xffU;
    bool m_connected = false;
    bool m_occupiedEntryRejected = false;
    bool m_canvasInvalidated = false;
    bool m_resyncRequired = false;
    bool m_resyncSent = false;
    bool m_snapshotActive = false;
    bool m_snapshotAck = false;
    Canvas m_snapshot{};
    uint32_t m_snapshotRevision = 0U;
    uint32_t m_snapshotSession = 0U;
    uint16_t m_snapshotRequest = 0U;
    uint16_t m_snapshotCursor = 0U;
    uint8_t m_snapshotPeekElements = 0U;
    bool m_snapshotBulk = false;
    Packet m_snapshotState{};
    Packet m_snapshotTool{};
    std::array<Receipt, 32U> m_receipts{};
    uint8_t m_receiptNext = 0U;
    uint8_t m_receiptCount = 0U;
    uint16_t m_requestHighwater = 0U;
    uint16_t m_bulkRequest = 0U;
    uint16_t m_bulkNextOffset = 0U;
    uint16_t m_bulkCommittedRequest = 0U;
    LitchcraftPersistence* m_advancedPersistence = nullptr;
    litchcraft_frame_stream::FrameStream* m_frameStream = nullptr;
    void (*m_frameStreamWakeStorage)(void*, bool, uint32_t, bool) = nullptr;
    void* m_frameStreamWakeStorageContext = nullptr;
    uint32_t m_streamGeneration = 0U;
    uint32_t m_storeGuardConnectionEpoch = 0U;
    uint32_t m_storeGuardGeneration = 0U;
    uint32_t m_storeGuardRevision = 0U;
    PhysicalStoreEdit m_physicalStoreEdit{};
    std::array<uint8_t,
               PHYSICAL_STORE_BATCH_CAPACITY *
                   PHYSICAL_STORE_BATCH_RECORD_BYTES>
        m_physicalStoreBatch{};
    uint8_t m_physicalStoreBatchHead = 0U;
    uint8_t m_physicalStoreBatchCount = 0U;
    uint8_t m_physicalStoreBatchPeekCount = 0U;
    bool m_physicalStoreBatchOverflow = false;
    bool m_physicalStoreBatchPeekOverflow = false;
    uint32_t m_streamDocumentEpoch = 0U;
    uint32_t m_streamFrameStartedAtMs = 0U;
    uint32_t m_streamPausedAtMs = 0U;
    uint32_t m_streamCorruptStartedAtMs = 0U;
    bool m_streamFrameStarted = false;
    bool m_streamCorruptDwell = false;
    bool m_streamAwaitingInitialFrame = false;
    bool m_streamAwaitingPlaybackFrame = false;
    bool m_streamAwaitingRecoveryFrame = false;
    AdvancedMutationDiagnostic m_lastAdvancedMutation{};
    bool m_v2SnapshotActive = false;
    bool m_v2SnapshotSessionZero = false;
    bool m_v2PacketPeeked = false;
    bool m_v2Subscribed = false;
    bool m_v2EffectClockSubscribed = false;
    uint16_t m_v2PendingRequest = 0U;
    uint16_t m_optimisticEpoch = 0U;
    uint16_t m_optimisticHighestSequence = 0U;
    uint8_t m_optimisticFrame = 0U;
    uint8_t m_optimisticAcceptedSinceWatermark = 0U;
    bool m_optimisticActive = false;
    bool m_optimisticResync = false;
    uint32_t m_v2ObservedRevision = 0U;
    bool m_v2EnvelopeLeased = false;
    bool m_v2StreamedReplace = false;
    uint16_t m_v2Request = 0U;
    uint16_t m_v2Cursor = 0U;
    uint16_t m_v2ChunkBytes = 0U;
    uint16_t m_v2Length = 0U;
    uint32_t m_v2Crc = 0U;
    uint32_t m_v2Revision = 0U;
    uint32_t m_v2Session = 0U;
    uint16_t m_v2Transfer = 0U;
    uint32_t m_v2BaseRevision = 0U;
    uint32_t m_v2StartedAtMs = 0U;
    uint32_t m_v2ProgressAtMs = 0U;
};

} // namespace modules
