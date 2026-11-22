#include "firmware/led_animation_task.hpp"

#include <type_traits>

#include "foxie_embedded/millis.hpp"
#include "runtime/progress_watchdog.hpp"

namespace firmware {
namespace {
#if defined(ARDUINO)
constexpr UBaseType_t ANIMATION_PRIORITY = 2U;
#if defined(LITCH_ESP32S3)
constexpr BaseType_t ANIMATION_CORE = 1;
#else
constexpr BaseType_t ANIMATION_CORE = 0;
#endif
void WaitOneTick() { vTaskDelay(1U); }
#else
void WaitOneTick() {}
#endif
} // namespace

void LedAnimationTask::Lock() const {
#if defined(ARDUINO)
    if (m_mutex != nullptr) (void)xSemaphoreTake(m_mutex, portMAX_DELAY);
#endif
}
void LedAnimationTask::Unlock() const {
#if defined(ARDUINO)
    if (m_mutex != nullptr) (void)xSemaphoreGive(m_mutex);
#endif
}
void LedAnimationTask::Wake() {
#if defined(ARDUINO)
    if (m_task != nullptr) xTaskNotifyGive(m_task);
#endif
}
void LedAnimationTask::Bind(hardware::NeoPixelLedRenderer& output,
    const hardware::BoardHardwareProfile& profile,
    const modules::litchcraft_presentation::StreamPort& artwork) {
    if (m_startAttempted) return;
#if defined(ARDUINO)
    if (m_mutex == nullptr) m_mutex = xSemaphoreCreateMutexStatic(&m_mutexStorage);
#endif
    m_artworkPort = artwork;
    m_renderer.Bind(output, profile, artwork);
}
void LedAnimationTask::InitializeBoot(const modules::BootDisplay& boot) {
    if (!m_startAttempted) m_renderer.InitializeBoot(boot);
}
void LedAnimationTask::InitializeGameBackground(const GameLedPresentation& game) {
    if (!m_startAttempted) m_renderer.InitializeGameBackground(game);
}
void LedAnimationTask::CancelBoot() {
    m_bootCancelEpoch.fetch_add(1U, std::memory_order_release);
    Wake();
}
void LedAnimationTask::InitializeBrightness(
    const modules::AmbientLuxAverager& ambient,
    const modules::UpwardOutputSlew& source) {
    if (!m_startAttempted) m_renderer.InitializeBrightness(ambient, source);
}
bool LedAnimationTask::Start() {
    if (IsStarted()) return true;
    if (m_startAttempted) return false;
    m_startAttempted = true;
#if defined(ARDUINO)
    if (m_mutex == nullptr) return false;
    m_task = xTaskCreateStaticPinnedToCore(&LedAnimationTask::TaskEntry,
        "led_animation", TASK_STACK_BYTES, this, ANIMATION_PRIORITY,
        m_taskStack, &m_taskControl, ANIMATION_CORE);
    if (m_task == nullptr) return false;
    m_started.store(true, std::memory_order_release);
    Wake();
    return true;
#else
    // Native users call the same renderer directly; firmware never falls back
    // to cooperative rendering after a failed task start.
    return false;
#endif
}
HintPresentationObservation LedAnimationTask::ObserveHint(const bool retire) {
    if (m_stopRequested.load(std::memory_order_acquire)) return {};
    const uint32_t startedMs = fxe::Millis();
    do {
        Lock();
        if (!retire || !m_frameInFlight) {
            const auto& receipt = m_presented.receipt;
            const HintPresentationObservation shown{
                receipt.positionRevision, receipt.hintEpoch, receipt.visibleHint,
                receipt.visibleHintValid, receipt.completed};
            if (retire) m_contextEpoch.fetch_add(1U, std::memory_order_release);
            Unlock();
            return shown;
        }
        Unlock();
        WaitOneTick();
    } while (fxe::Millis() - startedMs < COMMAND_TIMEOUT_MS);
    // An unresolved transfer is never evidence that a newer tuple appeared.
    return {};
}
LedAnimationPresentation* LedAnimationTask::BeginCapture() {
    Lock();
    if (m_commandPending || m_capturePending) { Unlock(); return nullptr; }
    m_capturePending = true;
    Unlock();
    return &m_pending;
}
uint32_t LedAnimationTask::FinishCapture() {
    Lock();
    if (!m_capturePending) { Unlock(); return 0U; }
    m_pending.sequence = ++m_nextSequence;
    m_pendingSequence = m_pending.sequence;
    m_capturePending = false;
    const uint32_t sequence = m_pendingSequence;
    Unlock();
    Wake();
    return sequence;
}
uint32_t LedAnimationTask::Publish(const LedAnimationPresentation& presentation) {
    auto* target = BeginCapture();
    if (target == nullptr) return 0U;
    *target = presentation;
    return FinishCapture();
}
bool LedAnimationTask::WaitForPresentation(const uint32_t sequence) {
    if (sequence == 0U || !IsStarted()) return false;
    const uint32_t startedMs = fxe::Millis();
    do {
        Lock();
        const bool matching = m_attemptedSequence == sequence;
        const bool completed = matching && m_attemptCompleted;
        Unlock();
        if (matching) return completed;
        WaitOneTick();
    } while (fxe::Millis() - startedMs < COMMAND_TIMEOUT_MS);
    return false;
}
bool LedAnimationTask::TakePublication() {
    Lock();
    if (m_pendingSequence == m_currentSequence || m_commandPending || m_capturePending) {
        Unlock();
        return false;
    }
    auto menu = m_current.menu;
    const bool sameMenu = m_havePresentation &&
        m_menuInputRevision == m_pending.menu.Revision();
    m_current = m_pending;
    if (sameMenu) {
        menu.SetOwner(m_pending.menu.GetOwner());
        m_current.menu = menu;
    }
    m_menuInputRevision = m_pending.menu.Revision();
    m_currentSequence = m_pendingSequence;
    m_havePresentation = true;
    Unlock();
    return true;
}
bool LedAnimationTask::Submit(const PhysicalLedCommand& command,
                              const LedCommandUrgency urgency) {
    if (!IsStarted()) {
        return !m_startAttempted && m_renderer.RenderPhysical(command);
    }
    return SubmitCommand(command, urgency);
}
bool LedAnimationTask::Submit(const RawDieLedCommand& command) {
    if (!IsStarted()) {
        return !m_startAttempted && m_renderer.RenderRawDie(command);
    }
    return SubmitCommand(command, LedCommandUrgency::Required);
}
bool LedAnimationTask::SubmitCommand(const Command& command,
                                     const LedCommandUrgency urgency) {
    const uint32_t startedMs = fxe::Millis();
    uint32_t sequence = 0U;
    do {
        Lock();
        // Consume an already published scene before temporarily using its
        // mailbox for a physical diagnostic command. There is one producer.
        if (!m_commandPending && !m_capturePending && m_pendingSequence == m_currentSequence) {
            std::visit([this](const auto& value) { m_pending.scene = value; }, command);
            sequence = ++m_nextCommandSequence;
            m_pendingCommandSequence = sequence;
            m_commandUrgency = urgency;
            m_commandPending = true;
        }
        Unlock();
        Wake();
        if (sequence != 0U) break;
        WaitOneTick();
    } while (fxe::Millis() - startedMs < COMMAND_TIMEOUT_MS);
    if (sequence == 0U) return false;
    do {
        Lock();
        const bool matching = m_completedCommandSequence == sequence;
        const bool succeeded = matching && m_completedCommandSucceeded;
        Unlock();
        if (matching) return succeeded;
        WaitOneTick();
    } while (fxe::Millis() - startedMs < COMMAND_TIMEOUT_MS);
    // Only an unconsumed command can be cancelled here. An active transfer
    // stays owned by the task until its bounded driver operation resolves.
    Lock();
    if (m_commandPending && m_pendingCommandSequence == sequence) m_commandPending = false;
    Unlock();
    return false;
}
bool LedAnimationTask::TakeCommand(Command& command, uint32_t& sequence,
                                   LedCommandUrgency& urgency) {
    Lock();
    if (!m_commandPending || m_capturePending) { Unlock(); return false; }
    urgency = m_commandUrgency;
    const bool due = !m_haveFrameStart ||
        fxe::Millis() - m_lastFrameStartMs >= FRAME_PERIOD_MS;
    if (!due && urgency != LedCommandUrgency::Required) { Unlock(); return false; }
    if (const auto* frame = std::get_if<PhysicalLedCommand>(&m_pending.scene)) command = *frame;
    else if (const auto* raw = std::get_if<RawDieLedCommand>(&m_pending.scene)) command = *raw;
    else { Unlock(); return false; }
    sequence = m_pendingCommandSequence;
    m_commandPending = false;
    m_frameInFlight = true;
    Unlock();
    return true;
}
bool LedAnimationTask::Quiesce() {
    if (!IsStarted()) return m_renderer.Quiesce();
    m_stopRequested.store(true, std::memory_order_release);
    Wake();
    const uint32_t startedMs = fxe::Millis();
    do {
        if (IsQuiescent()) return true;
        WaitOneTick();
    } while (fxe::Millis() - startedMs < COMMAND_TIMEOUT_MS);
    return false;
}
void LedAnimationTask::Resume() {
    if (!IsStarted()) return;
    m_quiescent.store(false, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    Wake();
}
void LedAnimationTask::PublishResult(LedAnimationFrameResult& result) {
    Lock();
    result.receipt.bootCancelEpoch = m_appliedBootCancelEpoch;
    m_attemptedSequence = result.receipt.sequence;
    m_attemptCompleted = result.receipt.completed;
    m_frameInFlight = false;
    if (result.receipt.completed) {
        // Confirm the exact Store token and publish its confirmed-output receipt as one
        // metadata transaction. Main admission uses this same lock order.
        m_renderer.NoteArtworkPresented(result.receipt.artwork);
        const auto& previous = m_presented.receipt;
        if (result.retainPriorHint && previous.visibleHintValid &&
            previous.positionRevision == result.receipt.positionRevision &&
            previous.hintEpoch == result.receipt.hintEpoch &&
            previous.contextEpoch == result.receipt.contextEpoch) {
            result.receipt.visibleHint = previous.visibleHint;
            result.receipt.visibleHintValid = true;
        }
        m_presented.receipt = result.receipt;
        m_presented.menuBackground = result.menuBackground;
    }
    else {
        m_presented.receipt.delivery = result.receipt.delivery;
        m_presented.receipt.stackFreeBytes = result.receipt.stackFreeBytes;
    }
    Unlock();
}
LedAnimationReceipt LedAnimationTask::Receipt() const {
    Lock();
    const auto receipt = m_presented.receipt;
    Unlock();
    return receipt;
}
std::array<modules::Rgb, 64> LedAnimationTask::MenuBackground() const {
    Lock();
    const auto background = m_presented.menuBackground;
    Unlock();
    return background;
}
bool LedAnimationTask::OutputEnabled() const {
    return IsStarted() ? !IsQuiescent() && Receipt().outputEnabled
                       : m_renderer.OutputEnabled();
}
uint32_t LedAnimationTask::TryBeginArtworkMutation(const ArtworkMutation kind,
    modules::litchcraft_presentation::Feedback& shown) {
    if (!IsStarted()) return 0U;
    Lock();
    if (m_frameInFlight || m_artworkMutationLease != 0U) { Unlock(); return 0U; }
    if (m_artworkPort.stream == nullptr) {
        m_artworkMutationLease = ++m_nextLocalMutation;
        if (m_artworkMutationLease == 0U) m_artworkMutationLease = ++m_nextLocalMutation;
        m_contextEpoch.fetch_add(1U, std::memory_order_release);
        shown = m_presented.receipt.artwork;
        const uint32_t lease = m_artworkMutationLease;
        Unlock();
        return lease;
    }
    const uint32_t lease = kind == ArtworkMutation::Frame
        ? m_artworkPort.stream->TryBeginPresentationMutation()
        : m_artworkPort.stream->TryBeginPresentationControl();
    if (lease != 0U) {
        m_artworkMutationLease = lease;
        shown = m_presented.receipt.artwork;
    }
    Unlock();
    return lease;
}
bool LedAnimationTask::EndArtworkMutation(const uint32_t lease) {
    if (lease == 0U) return false;
    Lock();
    const bool matching = lease == m_artworkMutationLease;
    const bool ended = matching && (m_artworkPort.stream == nullptr ||
        m_artworkPort.stream->EndPresentationMutation(lease));
    if (ended) m_artworkMutationLease = 0U;
    Unlock();
    Wake();
    return ended;
}
uint32_t LedAnimationTask::CadenceMs() const {
    const auto* ota = std::get_if<OtaLedScene>(&m_current.scene);
    return modules::RuntimeLedRefreshMs(ota != nullptr ? ota->presentation
        : modules::OtaLedPresentation{}, m_current.bulkTransferActive, FRAME_PERIOD_MS);
}
bool LedAnimationTask::ProcessCommand() {
    Command command;
    uint32_t commandSequence = 0U;
    LedCommandUrgency urgency = LedCommandUrgency::Normal;
    if (!TakeCommand(command, commandSequence, urgency)) return false;
    m_lastFrameStartMs = fxe::Millis();
    m_haveFrameStart = true;
    m_havePresentation = false;
    m_renderer.CancelArtwork();
    const bool completed = std::visit([this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PhysicalLedCommand>) return m_renderer.RenderPhysical(value);
        else return m_renderer.RenderRawDie(value);
    }, command);
    Lock();
    m_frameInFlight = false;
    m_completedCommandSequence = commandSequence;
    m_completedCommandSucceeded = completed;
    m_presented.receipt.delivery = m_renderer.Delivery();
    m_presented.receipt.power = m_renderer.PowerStats();
    m_presented.receipt.currentLimitMa = m_renderer.CurrentLimitMa();
#if defined(ARDUINO)
    m_presented.receipt.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
#endif
    if (completed) {
        m_presented.receipt.boot = m_renderer.BootState();
        m_presented.receipt.startupComplete = m_renderer.StartupComplete();
        m_presented.receipt.visibleHintValid = false;
        m_presented.receipt.artwork.ready = false;
    }
    Unlock();
    return true;
}
void LedAnimationTask::TaskEntry(void* context) {
    static_cast<LedAnimationTask*>(context)->Run();
}
void LedAnimationTask::Run() {
#if defined(ARDUINO)
    // Subscribe this independent task to the SDK watchdog without replacing
    // main/engine/storage/OTA's four retained breadcrumb lanes. Delivery
    // telemetry and the coherent task receipt own animation diagnostics.
    runtime::WatchCurrentTask(4U, runtime::ProgressStage::Render);
    for (;;) {
        if (m_stopRequested.load(std::memory_order_acquire)) {
            if (!IsQuiescent()) {
                m_lastFrameStartMs = fxe::Millis();
                m_haveFrameStart = true;
                Lock();
                m_frameInFlight = true;
                Unlock();
                const bool safe = m_renderer.Quiesce();
                Lock();
                m_frameInFlight = false;
                m_presented.receipt.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
                m_presented.receipt.delivery = m_renderer.Delivery();
                Unlock();
                if (safe) {
                    m_renderer.CancelArtwork();
                    Lock();
                    m_presented.receipt.visibleHintValid = false;
                    m_presented.receipt.artwork.ready = false;
                    m_presented.receipt.boot = m_renderer.BootState();
                    m_presented.receipt.startupComplete = m_renderer.StartupComplete();
                    m_presented.receipt.quiescent = true;
                    Unlock();
                    m_quiescent.store(true, std::memory_order_release);
                }
            }
            runtime::FeedProgressWatchdog();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FRAME_PERIOD_MS));
            continue;
        }
        const uint32_t bootCancel = m_bootCancelEpoch.load(std::memory_order_acquire);
        if (bootCancel != m_appliedBootCancelEpoch) {
            m_renderer.CancelBoot();
            m_appliedBootCancelEpoch = bootCancel;
        }
        (void)TakePublication();
        if (ProcessCommand()) {
            // The command owns this frame-start slot.
        } else if (m_havePresentation && (!m_haveFrameStart ||
                   fxe::Millis() - m_lastFrameStartMs >= (m_retryPending ? 1U : CadenceMs()))) {
            Lock();
            const bool current = m_current.contextEpoch == ContextEpoch() &&
                m_artworkMutationLease == 0U;
            m_frameInFlight = current;
            Unlock();
            if (current) {
                m_lastFrameStartMs = fxe::Millis();
                m_haveFrameStart = true;
                LedAnimationFrameResult result;
                result.menuBackground = m_presented.menuBackground;
                m_renderer.Render(m_current, result);
                result.receipt.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
                m_retryPending = result.attempted && !result.receipt.completed &&
                    result.receipt.outputEnabled;
                PublishResult(result);
            }
        }
        runtime::FeedProgressWatchdog();
        const uint32_t elapsed = m_haveFrameStart ? fxe::Millis() - m_lastFrameStartMs : 0U;
        const uint32_t cadence = m_retryPending ? 1U : CadenceMs();
        // Deadline is measured from frame START, including composition/Show.
        // An overrun yields once and then samples current absolute time; no
        // catch-up frames or delay-after-Show cadence stretch are introduced.
        const uint32_t remaining = m_haveFrameStart && elapsed < cadence ? cadence - elapsed : 1U;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remaining) == 0U ? 1U : pdMS_TO_TICKS(remaining));
    }
#endif
}

} // namespace firmware
