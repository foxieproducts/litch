#pragma once

#include <atomic>
#include <cstdint>
#include <variant>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

#include "firmware/led_animation_renderer.hpp"

namespace firmware {

enum class LedCommandUrgency : uint8_t { Normal, Required };
enum class ArtworkMutation : uint8_t { Frame, Control };

// Controls need only the confirmed Hint identity, not the full LED/power/
// artwork receipt. Keep this projection small on nested input/Undo call paths.
struct HintPresentationObservation {
    uint64_t positionRevision = 0U;
    uint32_t hintEpoch = 0U;
    runtime::HintPresentationTuple visibleHint{};
    bool visibleHintValid = false;
    bool completed = false;
};
static_assert(sizeof(HintPresentationObservation) <= 24U,
              "Hint observation must fit the bounded control-path stack budget");

class LedAnimationTask final {
  public:
    static constexpr uint32_t FRAME_PERIOD_MS = 33U;
    static constexpr uint32_t COMMAND_TIMEOUT_MS = 1000U;
    // The S3 render/composer/output chain already approaches 4 KiB before
    // SDK driver, failure logging and task-context overhead. Keep a separate
    // bounded stack; perf reports its SDK low-water mark for physical HIL.
    static constexpr uint32_t TASK_STACK_BYTES = 8192U;

    void Bind(hardware::NeoPixelLedRenderer& output,
              const hardware::BoardHardwareProfile& profile,
              const modules::litchcraft_presentation::StreamPort& artwork);
    void InitializeBoot(const modules::BootDisplay& boot);
    void InitializeGameBackground(const GameLedPresentation& game);
    void CancelBoot();
    void InitializeBrightness(const modules::AmbientLuxAverager& ambient,
                              const modules::UpwardOutputSlew& source);
    bool Start();
    bool StartAttempted() const { return m_startAttempted; }
    uint32_t ContextEpoch() const { return m_contextEpoch.load(std::memory_order_acquire); }
    HintPresentationObservation ObserveHint(bool retire);
    bool IsStarted() const { return m_started.load(std::memory_order_acquire); }
    LedAnimationPresentation* BeginCapture();
    uint32_t FinishCapture();
    uint32_t Publish(const LedAnimationPresentation& presentation);
    bool WaitForPresentation(uint32_t sequence);
    bool Submit(const PhysicalLedCommand& command,
                LedCommandUrgency urgency = LedCommandUrgency::Normal);
    bool Submit(const RawDieLedCommand& command);
    bool Quiesce();
    void Resume();
    bool IsQuiescent() const { return m_quiescent.load(std::memory_order_acquire); }
    LedAnimationReceipt Receipt() const;
    std::array<modules::Rgb, 64> MenuBackground() const;
    bool OutputEnabled() const;
    uint32_t TryBeginArtworkMutation(ArtworkMutation kind,
        modules::litchcraft_presentation::Feedback& shown);
    bool EndArtworkMutation(uint32_t lease);

  private:
#if defined(LITCH_NATIVE)
    friend struct LedAnimationTaskTestPeer;
#endif
    using Command = std::variant<PhysicalLedCommand, RawDieLedCommand>;
    bool SubmitCommand(const Command& command, LedCommandUrgency urgency);
    void Lock() const;
    void Unlock() const;
    void Wake();
    void Run();
    void PublishResult(LedAnimationFrameResult& result);
    uint32_t CadenceMs() const;
    bool TakePublication();
    bool ProcessCommand();
    bool TakeCommand(Command& command, uint32_t& sequence, LedCommandUrgency& urgency);
    static void TaskEntry(void* context);

    LedAnimationRenderer m_renderer{};
    modules::litchcraft_presentation::StreamPort m_artworkPort{};
    // The short publication mutex covers copies only. It never spans
    // composition, driver acquisition, RMT, filesystem work or game logic.
    LedAnimationPresentation m_pending{};
    LedAnimationPresentation m_current{};
    struct PresentedState {
        LedAnimationReceipt receipt{};
        std::array<modules::Rgb, 64> menuBackground{};
    } m_presented{};
    uint32_t m_artworkMutationLease = 0U;
    uint32_t m_nextLocalMutation = 0U;
    uint32_t m_nextSequence = 0U;
    uint32_t m_pendingSequence = 0U;
    uint32_t m_currentSequence = 0U;
    uint32_t m_menuInputRevision = 0U;
    uint32_t m_nextCommandSequence = 0U;
    uint32_t m_pendingCommandSequence = 0U;
    uint32_t m_completedCommandSequence = 0U;
    uint32_t m_attemptedSequence = 0U;
    bool m_attemptCompleted = false;
    bool m_commandPending = false;
    bool m_capturePending = false;
    bool m_completedCommandSucceeded = false;
    LedCommandUrgency m_commandUrgency = LedCommandUrgency::Normal;
    std::atomic<uint32_t> m_contextEpoch{1U};
    bool m_frameInFlight = false;
    std::atomic<uint32_t> m_bootCancelEpoch{0U};
    uint32_t m_appliedBootCancelEpoch = 0U;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_quiescent{false};
    bool m_startAttempted = false;
    bool m_havePresentation = false;
    bool m_haveFrameStart = false;
    bool m_retryPending = false;
    uint32_t m_lastFrameStartMs = 0U;
#if defined(ARDUINO)
    mutable StaticSemaphore_t m_mutexStorage{};
    mutable SemaphoreHandle_t m_mutex = nullptr;
    StaticTask_t m_taskControl{};
    StackType_t m_taskStack[TASK_STACK_BYTES / sizeof(StackType_t)]{};
    TaskHandle_t m_task = nullptr;
#endif
};

} // namespace firmware
