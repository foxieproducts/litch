#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>

#include "console.hpp"
#include "virtual_litch_session.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "modules/game_console_commands.hpp"

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/edge_led_constants.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/assistance_preferences.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/cpu_thinking.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/initial_fen.hpp"
#include "modules/orientation.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/vs_cpu_difficulty.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "../../src/virtual_litch_session_internal.hpp"
#include "../test_litchcraft_persistence/allocation_probe.hpp"

namespace modules {

// Native diagnostic and worker-lifetime access. Diagnostic mutations happen
// only at the existing parked Poll gate or quiescence; no writer overlaps.
struct DiagnosticSnapshotTestAccess {
    static void JoinNaturalCompletion(AsyncThinkWorker& worker) {
        if (worker.m_worker.joinable()) worker.m_worker.join();
    }
    static ThinkDiagnostics Cached(const GamePersistenceCoordinator& owner) {
        return owner.m_cpuDiagnostics;
    }
    static void RestoreCache(GamePersistenceCoordinator& owner, const ThinkDiagnostics& saved) {
        owner.m_cpuDiagnostics = saved;
    }
    static void Refresh(GamePersistenceCoordinator& owner) { owner.RefreshThinkDiagnostics(); }
    static void SeedPriorDiagnosticHistory(GamePersistenceCoordinator& owner, AsyncThinkWorker& worker) {
        ThinkConfig config;
        worker.PrepareDiagnostics(800U, ThinkRequestKind::Cpu, config, SideToMove::Black, false, 255U);
        worker.RecordSynchronousResult(ThinkState::Done, 77U, true);
        (void)worker.TryGetDiagnostics(owner.m_cpuDiagnostics);
        owner.m_hintDiagnostics = owner.m_cpuDiagnostics;
        owner.m_hintDiagnostics.kind = ThinkRequestKind::Hint;
        owner.m_hintDiagnostics.requestId = 799U;
        owner.m_hintDiagnostics.firstAcceptedHintValid = true;
        owner.m_diagnosticRequestId = 800U;
        owner.m_diagnosticFen = owner.m_cachedFen;
        owner.m_diagnosticVariant = owner.m_gameVariant;
    }

    static uint32_t HoldCopyUnavailable(AsyncThinkWorker& worker) {
        const uint32_t revision = worker.m_diagnosticRevision.load();
        worker.m_diagnosticRevision.store(revision | 1U);
        return revision;
    }
    static void ReleaseCopy(AsyncThinkWorker& worker, const uint32_t revision) {
        worker.m_diagnosticRevision.store(revision);
    }
};

} // namespace modules

namespace {

using Session = runtime::virtual_litch_internal::Session;
using Results = fxe::test_support::TestResults;
enum class PendingBoundary { None, Undo, Timeout };
enum class CpuExpiry { None, BeforeRoot, AfterFullRoot };
std::atomic<uint32_t> s_nowMs{0U};
uint32_t AtomicMillis() { return s_nowMs.load(std::memory_order_acquire); }

template <typename Predicate>
bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::yield();
    }
    return false;
}

// Only scheduling and node-observation cadence are controlled. Position,
// legality, actual search/results, and physical move application delegate to
// the production engine. Session already accepts this engine/worker seam.
class GatedEngine final : public modules::IEngine {
  public:
    explicit GatedEngine(modules::AsyncThinkWorker& worker) : m_worker(worker) {}
    ~GatedEngine() override {
        // The worker is declared before this engine. Retire any automatically
        // rearmed held search before the delegated engine member is destroyed.
        if (!m_worker.IsActive()) return;
        m_engine.CancelThink();
        releaseStats.store(true, std::memory_order_release);
        releaseAllCandidates.store(true, std::memory_order_release);
        releasePoll.store(true, std::memory_order_release);
        if (!WaitFor([&] { return !m_worker.IsActive(); })) std::abort();
    }

    void Reset() override { CheckLane(); m_engine.Reset(); }
    bool SetFen(const char* fen) override { CheckLane(); return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override {
        CheckLane(); return m_engine.GetFen(fen, size);
    }
    bool ApplyUciMove(const char* move) override {
        CheckLane(); return m_engine.ApplyUciMove(move);
    }
    modules::SideToMove GetSideToMove() const override {
        CheckLane(); return m_engine.GetSideToMove();
    }
    bool IsInCheck(modules::SideToMove side) const override {
        CheckLane(); return m_engine.IsInCheck(side);
    }
    bool IsInCheckmate(modules::SideToMove side) const override {
        CheckLane(); return m_engine.IsInCheckmate(side);
    }
    bool IsInStalemate(modules::SideToMove side) const override {
        CheckLane(); return m_engine.IsInStalemate(side);
    }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        CheckLane(); return m_engine.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override {
        // Controlled admission failure covers the production pending projection;
        // the paired Arduino fixture injects the actual worker allocation fault.
        if (rejectNextHintStart && config.continuousAnalysis) {
            rejectNextHintStart = false;
            return false;
        }
        if (admissionObserver != nullptr) admissionObserver(admissionContext);
        return m_engine.StartThink(Prepare(config));
    }
    bool StartDuckThink(const char* fen, uint8_t duck,
                        const modules::ThinkConfig& config) override {
        return m_engine.StartDuckThink(fen, duck, Prepare(config));
    }
    void CancelThink() override { ++cancels; m_engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress,
                                          modules::UciMove& best) override {
        pollEntered.store(true, std::memory_order_release);
        while (!releasePoll.load(std::memory_order_acquire)) {
            const uint32_t request = sampleRequest.load(std::memory_order_acquire);
            if (request != sampleAccepted.load(std::memory_order_relaxed)) {
                originalObserver.Publish(sampleNodes.load(std::memory_order_relaxed));
                sampleAccepted.store(request, std::memory_order_release);
            }
            std::this_thread::yield();
        }
        calculationStartedAtMs = fxe::Millis();
        return m_engine.PollThinkProgress(progress, best);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        statsEntered.store(true, std::memory_order_release);
        while (!releaseStats.load(std::memory_order_acquire)) std::this_thread::yield();
        return m_engine.GetLastThinkStats(stats);
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        CheckLane(); return m_engine.GetLastClassicThinkResult(result);
    }
    bool GetLastDuckThinkResult(modules::DuckThinkResult& result) const override {
        CheckLane(); return m_engine.GetLastDuckThinkResult(result);
    }

    bool PublishSample(uint32_t nowMs, uint32_t nodes) {
        // No Session method is running here. The worker acknowledges this
        // callback before Session temporarily installs its own clock provider.
        s_nowMs.store(nowMs, std::memory_order_release);
        sampleNodes.store(nodes, std::memory_order_relaxed);
        const uint32_t request = sampleRequest.fetch_add(1U, std::memory_order_release) + 1U;
        return WaitFor([&] { return sampleAccepted.load(std::memory_order_acquire) == request; });
    }
    bool Finish(uint32_t nowMs) {
        s_nowMs.store(nowMs, std::memory_order_release);
        releaseAllCandidates.store(true, std::memory_order_release);
        releasePoll.store(true, std::memory_order_release);
        return WaitFor([&] { return !m_worker.IsActive(); });
    }

    bool FinishFiniteSearch(uint32_t nowMs) {
        s_nowMs.store(nowMs, std::memory_order_release);
        releaseAllCandidates.store(true, std::memory_order_release);
        releasePoll.store(true, std::memory_order_release);
        // Bound the existing natural-completion seam before joining. On a
        // failed check, release/cancel all controlled gates before destruction.
        const bool finished = WaitFor([&] { return !m_worker.IsActive(); });
        if (!finished) {
            CancelThink();
            releaseStats.store(true, std::memory_order_release);
            if (!Finish(nowMs)) std::abort();
        }
        modules::DiagnosticSnapshotTestAccess::JoinNaturalCompletion(m_worker);
        return finished;
    }

    bool ReleaseCandidate(uint8_t depth, uint32_t nowMs) {
        s_nowMs.store(nowMs, std::memory_order_release);
        allowedCandidateDepth.store(depth, std::memory_order_release);
        return WaitFor([&] { return publishedCandidateDepth.load() >= depth; });
    }

    // Declared controlled-rate fixture, charged during actual progress before
    // any desired root. This is an assumption, never hardware throughput.
    void (*admissionObserver)(void*) = nullptr;
    void* admissionContext = nullptr;
    uint32_t controlledNodesPerSecond = 0U;
    CpuExpiry cpuExpiry = CpuExpiry::None;
    std::atomic<uint8_t> cpuCandidates{0U};
    std::atomic<uint8_t> cpuLastDepth{0U};
    std::atomic<uint32_t> cpuCompletedFullNodes{0U};
    std::atomic<bool> cpuDeadlineForced{false};
    modules::ThinkConfig lastConfig{};
    uint32_t starts = 0U;
    uint32_t startedAtMs = 0U;
    uint32_t holdStart = 1U;
    uint32_t budget = 0U;
    bool continuousHint = false;
    bool gateCandidates = false;
    bool rejectNextHintStart = false;
    modules::ThinkCandidate completedCandidates[32]{};
    std::atomic<uint8_t> readyCandidateDepth{0U};
    std::atomic<uint8_t> allowedCandidateDepth{0U};
    std::atomic<uint8_t> publishedCandidateDepth{0U};
    std::atomic<bool> releaseAllCandidates{false};
    mutable std::atomic<uint32_t> unsafeReads{0U};
    std::atomic<uint32_t> cancels{0U};
    std::atomic<uint32_t> actualSamples{0U};
    std::atomic<bool> wrongCallbackThread{false};
    std::atomic<bool> pollEntered{false};
    std::atomic<bool> releasePoll{false};
    mutable std::atomic<bool> statsEntered{false};
    std::atomic<bool> releaseStats{true};
    modules::ThinkProgressObserver originalObserver{};

  private:
    void CheckLane() const {
        if (m_worker.IsActive()) ++unsafeReads;
    }
    modules::ThinkConfig Prepare(const modules::ThinkConfig& config) {
        CheckLane();
        ++starts;
        lastConfig = config;
        startedAtMs = fxe::Millis();
        budget = config.maxNodes;
        continuousHint = config.continuousAnalysis;
        originalObserver = config.progressObserver;
        originalCandidateObserver = config.candidateObserver;
        cpuCandidates.store(0U, std::memory_order_release);
        cpuLastDepth.store(0U, std::memory_order_release);
        cpuCompletedFullNodes.store(0U, std::memory_order_release);
        cpuDeadlineForced.store(false, std::memory_order_release);
        pollEntered.store(false, std::memory_order_release);
        statsEntered.store(false, std::memory_order_release);
        releasePoll.store(holdStart != UINT32_MAX && starts != holdStart,
                          std::memory_order_release);
        modules::ThinkConfig observed = config;
        // This fixture freezes the clock inside a real synchronous search.
        // Bound its intervening human Hint, preserving the CPU policy under test.
        if (observed.continuousAnalysis) {
            observed.maxNodes = gateCandidates ? 1800000U : 4096U;
            if (gateCandidates) {
                observed.maxDepth = 8U;
                originalCandidateObserver = config.candidateObserver;
                observed.candidateObserver = {[](void* context,
                        const modules::ThinkCandidate& candidate) {
                    auto& self = *static_cast<GatedEngine*>(context);
                    self.completedCandidates[candidate.completedDepth] = candidate;
                    // These geometry journeys deliberately hold the selected
                    // deeper Classic snapshot; shallower scored roots remain
                    // legitimate and are coalesced by this controlled seam.
                    if (!candidate.isDuck && candidate.completedDepth < 5U) return;
                    self.readyCandidateDepth.store(candidate.completedDepth,
                                                  std::memory_order_release);
                    while (self.allowedCandidateDepth.load(std::memory_order_acquire) <
                               candidate.completedDepth &&
                           !self.releaseAllCandidates.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    self.originalCandidateObserver.Publish(candidate);
                    self.publishedCandidateDepth.store(candidate.completedDepth,
                                                       std::memory_order_release);
                }, this};
            }
        }
        if (!observed.continuousAnalysis && cpuExpiry != CpuExpiry::None) {
            // Advance only the test clock after actual engine work. Keep the
            // production factory deadline and every original callback intact.
            observed.candidateObserver = {[](void* context,
                    const modules::ThinkCandidate& candidate) {
                auto& self = *static_cast<GatedEngine*>(context);
                self.originalCandidateObserver.Publish(candidate);
                self.cpuLastDepth.store(candidate.completedDepth, std::memory_order_release);
                self.cpuCandidates.fetch_add(1U);
                if (self.cpuExpiry == CpuExpiry::AfterFullRoot &&
                    candidate.rootKind == modules::ThinkRootKind::ClassicFull && candidate.completedDepth == 1U &&
                    !self.cpuDeadlineForced.exchange(true, std::memory_order_acq_rel)) {
                    self.cpuCompletedFullNodes.store(candidate.completedNodes, std::memory_order_release);
                    s_nowMs.store(self.startedAtMs + 2000U, std::memory_order_release);
                    self.cpuDeadlineForced.store(true, std::memory_order_release);
                }
            }, this};
        }
        if (observed.continuousAnalysis && !gateCandidates) {
            observed.candidateObserver = {[](void* context, const modules::ThinkCandidate& candidate) {
                static_cast<GatedEngine*>(context)->originalCandidateObserver.Publish(candidate);
            }, this};
        }
        if (observed.candidateObserver.context == this) {
            observed.candidateObserver.completedRoot = [](void* context, const modules::ThinkCandidate& candidate) {
                auto& self = *static_cast<GatedEngine*>(context);
                // The existing clock is frozen during synchronous calculation.
                // Charge its declared500ms at real completed-root work, never
                // admission/queued idle, so production first-reveal gating can
                // complete without an unbounded release hold at teardown.
                if (self.lastConfig.continuousAnalysis && self.controlledNodesPerSecond == 0U &&
                    fxe::Millis() - self.calculationStartedAtMs < self.lastConfig.minimumCalculationMs) {
                    s_nowMs.store(self.calculationStartedAtMs + self.lastConfig.minimumCalculationMs,
                                  std::memory_order_release);
                }
                self.originalCandidateObserver.RootCompleted(candidate);
            };
            observed.candidateObserver.effectiveProfile = [](void* context, const modules::ThinkProfile& profile) {
                static_cast<GatedEngine*>(context)->originalCandidateObserver.ProfileResolved(profile);
            };
        }
        if (originalObserver.publish != nullptr) {
            observed.progressObserver = {[](void* context, uint32_t nodes) {
                auto& self = *static_cast<GatedEngine*>(context);
                ++self.actualSamples;
                if (self.controlledNodesPerSecond != 0U) {
                    const uint32_t elapsed = static_cast<uint32_t>(static_cast<uint64_t>(nodes) * 1000U /
                                                                 self.controlledNodesPerSecond);
                    s_nowMs.store(self.startedAtMs + elapsed, std::memory_order_release);
                }
                if (std::this_thread::get_id() == self.m_mainThread) {
                    self.wrongCallbackThread.store(true, std::memory_order_release);
                }
                if (!self.lastConfig.continuousAnalysis &&
                    self.cpuExpiry == CpuExpiry::BeforeRoot &&
                    !self.cpuDeadlineForced.exchange(true, std::memory_order_acq_rel)) {
                    s_nowMs.store(self.startedAtMs + 2000U, std::memory_order_release);
                }
                self.originalObserver.Publish(nodes);
            }, this};
        }
        return observed;
    }
    modules::AsyncThinkWorker& m_worker;
    modules::LitchMaxEngine m_engine;
    const std::thread::id m_mainThread = std::this_thread::get_id();
    std::atomic<uint32_t> sampleRequest{0U};
    std::atomic<uint32_t> sampleAccepted{0U};
    std::atomic<uint32_t> sampleNodes{0U};
    modules::ThinkCandidateObserver originalCandidateObserver{};
    uint32_t calculationStartedAtMs = 0U;
};

template <typename SessionType>
bool ReadDiagnostics(SessionType& session, const modules::ThinkRequestKind kind,
                     modules::ThinkDiagnostics& data) {
    struct Read { modules::ThinkRequestKind kind; modules::ThinkDiagnostics* out; bool found = false; } read{kind, &data};
    session.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                                modules::GamePersistenceCoordinator& coordinator) {
        auto& read = *static_cast<Read*>(context);
        read.found = coordinator.TryGetThinkDiagnostics(read.kind, *read.out);
    }, &read);
    return read.found;
}

std::string ReadDiagnosticConsole(Session& session) {
    std::string text;
    session.RunControlIngress([](void* context, modules::SettingsStore& settings,
                                modules::ControlMutationService& mutation,
                                modules::GamePersistenceCoordinator& coordinator) {
        MemoryConsoleTransport transport; Console console(transport);
        fxe::test_support::ConsoleOutputCapture capture; capture.Attach(transport);
        modules::GameConsoleCommandContext command;
        // Only the diagnostic command is executed; its unused reducer slot is
        // fixture-local. Search/cache/time are the real Session coordinator.
        runtime::GameStateMachine unusedState;
        modules::RegisterGameConsoleCommands(console, settings, coordinator, unusedState, command, &mutation);
        fxe::test_support::RunCommand(console, transport, "game hint stats");
        *static_cast<std::string*>(context) = capture.Str();
    }, &text);
    return text;
}

void CheckEstimate(Results& results) {
    modules::CpuThinkingEstimate estimate;
    s_nowMs.store(UINT32_MAX - 200U);
    estimate.Start('b', 3000U);
    results.Check(estimate.Snapshot().side == 'b' &&
                      estimate.Snapshot().remainingSeconds == 0U,
                  "CPU estimate starts unknown without a completion percentage");
    s_nowMs.fetch_add(250U);
    estimate.Update(300U);
    s_nowMs.fetch_add(249U);
    estimate.Update(599U);
    results.Check(estimate.Snapshot().remainingSeconds == 0U,
                  "two samples do not publish an estimate before 500 ms");
    s_nowMs.fetch_add(1U);
    estimate.Update(600U);
    const auto first = estimate.Snapshot();
    results.Check(first.elapsedHalfSeconds == 1U && first.remainingSeconds == 4U,
                  "wrap-safe current-job throughput adds two seconds of cushion");
    s_nowMs.fetch_add(499U);
    estimate.Update(900U);
    results.Check(estimate.Snapshot() == first,
                  "live progress cannot republish the estimate faster than 2 Hz");
    s_nowMs.fetch_add(1U);
    estimate.Update(900U);
    results.Check(estimate.Snapshot().elapsedHalfSeconds == 2U &&
                      estimate.Snapshot().remainingSeconds == 5U,
                  "next publication uses the latest observed sample");
    s_nowMs.fetch_add(1000U);
    estimate.Update(900U);
    results.Check(estimate.Snapshot().remainingSeconds == 4U,
                  "an unchanged sample does not extend its previous estimate");
    s_nowMs.fetch_add(1000U);
    estimate.Update(900U);
    results.Check(estimate.Snapshot().remainingSeconds == 0U,
                  "two seconds without progress return to unknown");
    estimate.Update(3000U);
    results.Check(estimate.Snapshot().remainingSeconds == 0U,
                  "budget exhaustion never reports search completion");
    estimate.Start('w', 0U);
    estimate.Update(1U);
    s_nowMs.fetch_add(500U);
    estimate.Update(2U);
    results.Check(estimate.Snapshot().remainingSeconds == 0U,
                  "a missing budget never fabricates remaining seconds");
    estimate.Clear();
    results.Check(estimate.Snapshot().side == '\0', "CPU estimate clears its owner");

    for (uint32_t turn = 0U; turn < 3U; ++turn) {
        const auto config = modules::MakeThinkConfigForVsCpuTurn(11U, turn, 1U);
        s_nowMs.store(UINT32_MAX - 200U);
        estimate.Start('w', config.maxNodes, config.maxThinkMs);
        s_nowMs.fetch_add(250U);
        estimate.Update(750U);
        s_nowMs.fetch_add(250U);
        estimate.Update(1500U);
        results.Check(estimate.Snapshot().remainingSeconds == 62U,
                      "selected deadline caps every opening-ramp estimate across millis wrap");
        s_nowMs.fetch_add(1000U);
        estimate.Update(4500U);
        results.Check(estimate.Snapshot().remainingSeconds <= 61U,
                      "new progress cannot extend the selected search deadline");
        s_nowMs.fetch_add(500U);
        estimate.Update(4500U);
        results.Check(estimate.Snapshot().remainingSeconds <= 60U,
                      "unchanged fresh progress still respects the shrinking deadline");
        s_nowMs.fetch_add(1500U);
        estimate.Update(4500U);
        results.Check(estimate.Snapshot().remainingSeconds == 0U,
                      "time limit cannot fabricate an estimate from stale progress");
        s_nowMs.store(UINT32_MAX - 200U + 60000U);
        estimate.Update(5000U);
        results.Check(estimate.Snapshot().remainingSeconds == 0U,
                      "deadline exhaustion reports unknown until the worker actually completes");
    }
    s_nowMs.store(1000U);
    estimate.Start('b', UINT32_MAX, 60000U);
    s_nowMs.fetch_add(250U);
    estimate.Update(1U);
    s_nowMs.fetch_add(250U);
    estimate.Update(2U);
    results.Check(estimate.Snapshot().remainingSeconds == 62U,
                  "time bound clamps a large node projection before wire-size overflow");
}

constexpr uint64_t CHESS_OCCUPANCY = (UINT64_C(1) << 4U) |
    (UINT64_C(1) << 11U) | (UINT64_C(1) << 52U) | (UINT64_C(1) << 60U);
constexpr uint8_t DUCK_FROM = 24U;

void RemoveFiles(const char* path) {
    (void)std::remove(path);
    char sidecar[192];
    for (const char* suffix : {".bak", ".tmp"}) {
        std::snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffix);
        (void)std::remove(sidecar);
    }
}

bool SaveFixture(const char* path, bool duck, bool black, bool firstP2, bool rotate,
                 const char* overrideFen = nullptr, uint32_t timer = 0U,
                 uint32_t level = 3U, bool completeInitialHistory = false, uint16_t historyPly = 0U) {
    RemoveFiles(path);
    modules::SettingsStoreConfig config;
    config.path = path;
    modules::SettingsStore settings(config);
    if (!settings.Load()) return false;
    modules::GamePersistenceState game;
    const char* fen = overrideFen != nullptr ? overrideFen
        : black ? "4k3/3p4/8/8/8/8/4P3/4K3 b - - 0 1"
                : "4k3/3p4/8/8/8/8/4P3/4K3 w - - 0 1";
    if (!game.fen.Assign(fen) || !game.undoBaseFen.Assign(fen)) return false;
    if (completeInitialHistory) {
        game.gameHistory.present = true;
        game.gameHistory.gameId.bytes[0] = 41U;
        game.gameHistory.lineRevision = 1U;
        game.gameHistory.currentPly = historyPly;
    }
    modules::DuckGameDurableState variant;
    variant.variant = duck ? modules::GameVariant::Duck : modules::GameVariant::Classic;
    variant.duckSquare = duck ? DUCK_FROM : modules::INVALID_DUCK_SQUARE;
    char encoded[32]{};
    if (!modules::EncodeDuckGameDurableState(variant, encoded, sizeof(encoded)) ||
        !game.variantState.Assign(encoded) ||
        !modules::WriteGamePersistenceState(settings.Doc(), game)) return false;
    settings.Doc()["VS_CPU_LEVEL"] = level;
    settings.Doc()["VS_CPU_PLAYER"] = black ? 2U : 1U;
    settings.Doc()["GAME_FIRST_PLAYER"] = firstP2 ? 2U : 1U;
    settings.Doc()["BOARD_ROTATE"] = rotate;
    settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    settings.Doc()["WHITE_SQUARES_ON"] = false;
    settings.Doc()["BRIGHTNESS"] = 6U;
    settings.Doc()["TIMER"] = timer;
    settings.MarkDirty();
    return settings.SaveNow();
}

template <typename SessionType>
bool ReadHome(const SessionType& session, StaticJsonDocument<1536>& home) {
    const auto line = session.HomeStateLine();
    return !deserializeJson(home, line.CStr());
}

template <typename SessionType>
bool Set(SessionType& session, uint16_t id, const char* key, const char* json) {
    virtual_litch_setting_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = id;
    std::snprintf(request.key, sizeof(request.key), "%s", key);
    std::snprintf(request.value_json, sizeof(request.value_json), "%s", json);
    modules::ControlMutationOutcome outcome;
    return session.MutateSetting(request, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

bool CompleteAction(Session& session, const virtual_litch_action_request& request,
                    modules::ControlMutationOutcome& out) {
    if (session.DispatchControlAction(request, out) != VIRTUAL_LITCH_OK ||
        out.status != modules::ControlMutationStatus::Accepted ||
        out.requestId != request.request_id ||
        session.Advance(1U) != VIRTUAL_LITCH_OK) {
        return false;
    }
    return session.TryTakeControlCompletion(out) &&
           out.requestId == request.request_id;
}


uint16_t CueEnergy(const virtual_litch_color color) {
    return color.r + color.g + color.b + color.w;
}

bool CueEqual(const virtual_litch_color a, const virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}

bool CueSameHue(const virtual_litch_color a, const virtual_litch_color b) {
    if (CueEnergy(a) == 0U || CueEnergy(b) == 0U) return false;
    const uint8_t left[]{a.r, a.g, a.b}, right[]{b.r, b.g, b.b};
    for (uint8_t i = 0U; i < 3U; ++i) {
        if ((left[i] == 0U) != (right[i] == 0U)) return false;
        for (uint8_t j = 0U; j < 3U; ++j) {
            const int delta = static_cast<int>(left[i]) * right[j] - static_cast<int>(left[j]) * right[i];
            if (delta < -510 || delta > 510) return false;
        }
    }
    return true;
}

bool ActiveHintLanding(const Session& session, const uint8_t target,
                       const uint8_t neighbor, const modules::Rgb available,
                       const bool knight = false) {
    const auto ledCount = session.PhysicalFrame().led_count;
    const auto revision = ledCount == 300U ? hardware::BoardRevision::V3_3S3
        : ledCount == 224U ? hardware::BoardRevision::V2S3 : hardware::BoardRevision::V3S3;
    const auto rgb = modules::ScaleRgbSaturatingBy256(available, 320U);
    const virtual_litch_color expected{rgb.r, rgb.g, rgb.b, 0U};
    // The directional head may crossfade either rail toward the mover's
    // color. Required landing guidance retains its visible available-color
    // floor; a full-strength stationary opposite rail is not its contract.
    for (const uint8_t slot : {0U, 2U}) {
        const auto actual = session.SemanticFrame().board[target][slot];
        const std::array<uint8_t, 3U> channels{{actual.r, actual.g, actual.b}};
        const std::array<uint8_t, 3U> full{{expected.r, expected.g, expected.b}};
        if (CueEnergy(actual) == 0U) return false;
        for (uint8_t channel = 0U; channel < channels.size(); ++channel) {
            if (!knight && (channels[channel] < full[channel] * 96U / 256U ||
                            channels[channel] > full[channel])) return false;
        }
    }
    if (CueEnergy(session.SemanticFrame().board[target][1U]) == 0U) return false;
    for (const auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Piece,
                           hardware::SquareLedSlot::Square2}) {
        const auto mapped = hardware::MapSemanticSquareLedTargets(revision, target, slot, session.HallRotate());
        if (mapped.count == 0U) return false;
        for (uint8_t i = 0U; i < mapped.count; ++i)
            if (CueEnergy(session.PhysicalFrame().pixels[mapped.indices[i]]) == 0U) return false;
    }
    const bool legalNeighbor = (session.State().valid_destination_mask & (UINT64_C(1) << neighbor)) != 0ULL;
    const auto ordinary = legalNeighbor ? expected : virtual_litch_color{};
    for (const uint8_t slot : {0U, 2U})
        if (!CueEqual(session.SemanticFrame().board[neighbor][slot], ordinary)) return false;
    return CueEnergy(session.SemanticFrame().board[neighbor][1U]) == 0U;
}

bool CapturedSlotLit(const Session& session, uint8_t square,
                     hardware::SquareLedSlot slot, bool lit = true) {
    const auto semantic = session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
    if ((CueEnergy(semantic) != 0U) != lit) return false;
    const auto ledCount = session.PhysicalFrame().led_count;
    const auto revision = ledCount == 300U ? hardware::BoardRevision::V3_3S3
        : ledCount == 224U ? hardware::BoardRevision::V2S3 : hardware::BoardRevision::V3S3;
    const auto targets = hardware::MapSemanticSquareLedTargets(
        revision, square, slot, session.HallRotate());
    if (targets.count == 0U) return false;
    for (uint8_t i = 0U; i < targets.count; ++i) {
        if ((CueEnergy(session.PhysicalFrame().pixels[targets.indices[i]]) != 0U) != lit) return false;
    }
    return true;
}

bool CapturedSquareLit(const Session& session, uint8_t square, bool lit = true) {
    return CapturedSlotLit(session, square, hardware::SquareLedSlot::Square0, lit) &&
           CapturedSlotLit(session, square, hardware::SquareLedSlot::Square2, lit);
}

bool CapturedHintSourceCadence(const Session& session, uint8_t square) {
    // The same production token owns CPU and idle Hint movers. Check both
    // semantic output and every mapped V3 piece emitter in either phase.
    const bool on = modules::animation::IsBlinkOn(
        modules::animation::AnimationContext{session.NowMs()},
        modules::animation::LED_FRAME_TOKENS.moverPieceBlink);
    return CapturedSlotLit(session, square, hardware::SquareLedSlot::Piece, on);
}

bool CapturedSquareSplit(const Session& session, uint8_t square) {
    const auto left = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Square0, session.HallRotate());
    const auto right = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Square2, session.HallRotate());
    return !CueEqual(session.SemanticFrame().board[square][0U], session.SemanticFrame().board[square][2U]) &&
           !CueEqual(session.PhysicalFrame().pixels[left.indices[0U]],
                     session.PhysicalFrame().pixels[right.indices[0U]]);
}

bool CapturedSamePieceWithinTravelFloor(const Session& session, uint8_t square, uint8_t reference) {
    const auto withinFloor = [](virtual_litch_color actual, virtual_litch_color ordinary) {
        const std::array<uint8_t,4U> a{{actual.r,actual.g,actual.b,actual.w}};
        const std::array<uint8_t,4U> b{{ordinary.r,ordinary.g,ordinary.b,ordinary.w}};
        for (uint8_t channel=0U;channel<4U;++channel) {
            if (b[channel]==0U ? a[channel]!=0U :
                a[channel]<b[channel]*96U/256U || a[channel]>b[channel]) return false;
        }
        return CueEnergy(actual)>0U;
    };
    // This fixture's Black is pure red, White is blue. Preserve that exact
    // identity in semantic output and BOTH mapped emitters; only the approved
    // source floor/head gain may vary during the ON interval.
    if (!withinFloor(session.SemanticFrame().board[square][1U],
                     session.SemanticFrame().board[reference][1U])) return false;
    const auto a = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Piece, session.HallRotate());
    const auto b = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, reference, hardware::SquareLedSlot::Piece, session.HallRotate());
    for (uint8_t i=0U;i<a.count;++i) if (!withinFloor(session.PhysicalFrame().pixels[a.indices[i]],
        session.PhysicalFrame().pixels[b.indices[i]])) return false;
    return a.count!=0U && a.count==b.count;
}

// Victim removal is WHITE; correction/error markers below retain their own red.
bool AmberTint(const virtual_litch_color color) {
    return color.r > color.g && color.g > 0U && color.b == 0U && color.w == 0U;
}

bool CapturedAmberSquare(const Session& session, uint8_t square) {
    if (!CapturedSquareLit(session, square) || CapturedSquareSplit(session, square)) return false;
    for (auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Square2}) {
        if (!AmberTint(session.SemanticFrame().board[square][static_cast<uint8_t>(slot)])) return false;
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, square, slot, session.HallRotate());
        for (uint8_t i = 0U; i < targets.count; ++i)
            if (!AmberTint(session.PhysicalFrame().pixels[targets.indices[i]])) return false;
    }
    return true;
}

bool CapturedStatusRed(const Session& session) {
    constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
    for (uint8_t i = 0U; i < 32U; ++i) {
        const auto semantic = session.SemanticFrame().status[i];
        const auto mapped = session.PhysicalFrame().pixels[topology.physicalBoardLedCount + i];
        if (semantic.r == 0U || semantic.g != 0U || semantic.b != 0U ||
            mapped.r == 0U || mapped.g != 0U || mapped.b != 0U || mapped.w != 0U) return false;
    }
    return true;
}

bool CapturedRemovalCue(const Session& session,uint8_t square){
    const auto left=session.SemanticFrame().board[square][0U],right=session.SemanticFrame().board[square][2U];
    const uint16_t sum=left.r+right.r;
    if(left.r!=left.g || left.g!=left.b || right.r!=right.g || right.g!=right.b ||
        left.w!=0U || right.w!=0U || sum<254U || sum>257U ||
        !CapturedSlotLit(session,square,hardware::SquareLedSlot::Piece,false))return false;
    for(const auto slot:{hardware::SquareLedSlot::Square0,hardware::SquareLedSlot::Square2}){
        const auto targets=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3S3,square,slot,session.HallRotate());
        const auto semantic=session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
        for(uint8_t i=0U;i<targets.count;++i){
            const auto mapped=session.PhysicalFrame().pixels[targets.indices[i]];
            if(semantic.r==0U && CueEnergy(mapped)!=0U)return false;
            if(semantic.r==255U && CueEnergy(mapped)==0U)return false;
        }
    }
    return true;
}

std::array<virtual_litch_color, 4U> CapturedCorners(const Session& session) {
    std::array<virtual_litch_color, 4U> colors{};
    const auto indices = modules::PlayerCornerLedIndices(modules::UiOwner::P1);
    constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
    for (uint8_t i = 0U; i < indices.size(); ++i)
        colors[i] = session.PhysicalFrame().pixels[topology.physicalBoardLedCount + indices[i]];
    return colors;
}

bool CapturedCornersWhite(const Session& session) {
    const auto physical = CapturedCorners(session);
    const auto indices = modules::PlayerCornerLedIndices(modules::UiOwner::P1);
    for (uint8_t i = 0U; i < indices.size(); ++i) {
        const auto semantic = session.SemanticFrame().status[indices[i]];
        const auto mapped = physical[i];
        if (semantic.r == 0U || semantic.r != semantic.g || semantic.g != semantic.b ||
            mapped.r == 0U || mapped.r != mapped.g || mapped.g != mapped.b || mapped.w == 0U) return false;
    }
    return true;
}

bool SameCapturedCorners(const std::array<virtual_litch_color, 4U>& a,
                         const std::array<virtual_litch_color, 4U>& b) {
    for (uint8_t i = 0U; i < a.size(); ++i) if (!CueEqual(a[i], b[i])) return false;
    return true;
}

bool UniformCapturedCorners(const Session& session) {
    const auto colors = CapturedCorners(session);
    for (uint8_t i = 1U; i < colors.size(); ++i) if (!CueEqual(colors[0U], colors[i])) return false;
    return true;
}

void CapturedCueCheckpoint(const Session& session, const char* run, const char* checkpoint) {
    const auto& state = session.State();
    uint8_t from = 64U, to = 64U;
    const bool hint = session.TryGetHintMoveSquares(from, to);
    std::printf("[CAPTURED_LED_CUES] run=%s checkpoint=%s t=%lu topology=V3-RGBW "
                "phase=%s required=%u physical=%016llx expected=%016llx hint=%u:%u-%u squares=[",
                run, checkpoint, static_cast<unsigned long>(session.NowMs()),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.required_action,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask), hint, from, to);
    for (const uint8_t square : {42U, 27U, 12U, 20U, 21U, 11U, 45U}) {
        std::printf("%c%c{", 'a' + square % 8U, '8' - square / 8U);
        for (auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Piece,
                          hardware::SquareLedSlot::Square2}) {
            const auto semantic = session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
            std::printf("slot%u:semantic=%u,%u,%u,%u/mapped=", static_cast<unsigned>(slot),
                        semantic.r, semantic.g, semantic.b, semantic.w);
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, square, slot, session.HallRotate());
            for (uint8_t i = 0U; i < targets.count; ++i) {
                const auto mapped = session.PhysicalFrame().pixels[targets.indices[i]];
                std::printf("%u:%u,%u,%u,%u;", targets.indices[i], mapped.r, mapped.g, mapped.b, mapped.w);
            }
        }
        std::printf("}");
    }
    std::printf("] P1_home_corners=[");
    const auto indices = modules::PlayerCornerLedIndices(modules::UiOwner::P1);
    const auto physical = CapturedCorners(session);
    for (uint8_t i = 0U; i < indices.size(); ++i) {
        const auto semantic = session.SemanticFrame().status[indices[i]];
        std::printf("%u:semantic=%u,%u,%u/mapped=%u,%u,%u,%u;", indices[i],
                    semantic.r, semantic.g, semantic.b,
                    physical[i].r, physical[i].g, physical[i].b, physical[i].w);
    }
    std::puts("]");
}

template <typename SessionType>
bool ControlBankLit(const SessionType& session, bool physicalP2, bool lit) {
    const bool v3 = session.PhysicalFrame().led_count == 288U;
    const auto range = physicalP2 ? hardware::V3_3_P2_CONTROL_LED_RANGE : hardware::V3_3_P1_CONTROL_LED_RANGE;
    bool any = false;
    for (uint8_t i = 0U; i < (v3 ? 4U : range.count); ++i) {
        const uint8_t index = v3 ? modules::PlayerHomeLedIndex(
            physicalP2 ? modules::UiOwner::P2 : modules::UiOwner::P1, 2U+i) : range.first+i;
        any = any || CueEnergy(session.PhysicalFrame().pixels[256U + index]) != 0U;
    }
    return any == lit;
}

template <typename SessionType>
void ControlCheckpoint(const SessionType& session, const char* run, const char* stage, const char* next) {
    if (session.PhysicalFrame().led_count != 300U && session.PhysicalFrame().led_count != 288U) return;
    std::printf("[CONTROL_LED_JOURNEY] run=%s stage=%s t=%lu occupancy=%016llx expected=%016llx phase=%s action=%u text=%.*s next=%s cpu_visible=%u hint_requested=%u pixels=[",
        run, stage, static_cast<unsigned long>(session.State().now_ms),
        static_cast<unsigned long long>(session.State().physical_occupied_mask),
        static_cast<unsigned long long>(session.State().expected_occupied_mask),
        runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(session.State().interaction_phase)),
        session.State().required_action, static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
        next, session.CpuThinkingVisible(), session.HintDisplayRequested());
    const uint8_t statusCount = session.PhysicalFrame().led_count - 256U;
    for (uint8_t index = 0U; index < statusCount; ++index) {
        const auto c=session.PhysicalFrame().pixels[256U+index];
        std::printf("raw%u/pixel%u:%u,%u,%u,%u;",index,256U+index,c.r,c.g,c.b,c.w);
    }
    std::printf("] brightness65535=%u current_ma=%u limit=%u\n",session.PhysicalFrame().applied_brightness_scale_65535,
        session.PhysicalFrame().applied_led_current_ma,session.PhysicalFrame().power_limited);
}

void Checkpoint(Results& results, const Session& session, const GatedEngine& engine,
                const char* run, const char* name, const char* next) {
    ControlCheckpoint(session, run, name, next);
    StaticJsonDocument<1536> home;
    results.Check(ReadHome(session, home), "Session Home projection remains serializable");
    const char* fen = home["fen"] | "";
    const char* mover = std::strchr(fen, ' ');
    const bool hasThinking = home["th"].is<JsonArray>();
    results.Check(!hasThinking || (mover != nullptr &&
                      (home["th"][0] | "")[0] == mover[1]),
                  "Home thinking side follows the admitted FEN mover across end mapping");
    const auto& state = session.State();
    const bool visible = session.CpuThinkingVisible();
    std::printf("[CPU_THINKING_JOURNEY] run=%s checkpoint=%s t=%lu "
                "physical=%016llx expected=%016llx phase=%s required=%u "
                "searches=%lu worker_samples=%lu unsafe=%lu control_cue=%u "
                "edge=%016llx physical_frame=%016llx text=%.*s next=%s home=%s status_rgb=[",
                run, name, static_cast<unsigned long>(session.NowMs()),
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.required_action, static_cast<unsigned long>(engine.starts),
                static_cast<unsigned long>(engine.actualSamples.load()),
                static_cast<unsigned long>(engine.unsafeReads.load()), visible,
                static_cast<unsigned long long>(session.SemanticFrame().status_hash),
                static_cast<unsigned long long>(session.PhysicalFrame().frame_hash),
                static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
                next, session.HomeStateLine().CStr());
    for (uint8_t index = 0U; index < VIRTUAL_LITCH_STATUS_LED_COUNT; ++index) {
        const auto& color = session.SemanticFrame().status[index];
        std::printf("%s%02x%02x%02x", index == 0U ? "" : ",",
                    static_cast<unsigned>(color.r), static_cast<unsigned>(color.g),
                    static_cast<unsigned>(color.b));
    }
    const auto& physical = session.PhysicalFrame();
    std::printf("] scale65535=%u current_est_ma=%u current_applied_ma=%u limited=%u\n",
                static_cast<unsigned>(physical.applied_brightness_scale_65535),
                static_cast<unsigned>(physical.estimated_led_current_ma),
                static_cast<unsigned>(physical.applied_led_current_ma),
                static_cast<unsigned>(physical.power_limited));
    if (std::strncmp(run, "Classic/captured-V3/", 20U) == 0)
        CapturedCueCheckpoint(session, run, name);
    std::fflush(stdout);
}

bool Observe(Session& session, uint64_t occupancy) {
    return session.DispatchBoardObservation(
        session.PhysicalOccupancyMask() ^ occupancy, occupancy) == VIRTUAL_LITCH_OK;
}

bool Move(Session& session, uint64_t& occupancy, uint8_t from, uint8_t to) {
    if (from >= 64U || to >= 64U) return false;
    occupancy &= ~(UINT64_C(1) << from);
    if (!Observe(session, occupancy)) return false;
    occupancy |= UINT64_C(1) << to;
    return Observe(session, occupancy) && session.Advance(2600U) == VIRTUAL_LITCH_OK;
}

void CheckPendingBoundary(Results& results, Session& session, GatedEngine& engine,
                          modules::AsyncThinkWorker& worker, const char* run,
                          bool duck, uint64_t& occupancy,
                          const litch_max::ParsedUciMove& priorMove,
                          PendingBoundary boundary) {
    // The preceding journey committed an actual White CPU turn. Its Black
    // human Hint is the existing handoff owner, and history/clock are real.
    for (uint8_t i = 0U; i < 30U && engine.starts == 1U; ++i) {
        (void)session.Advance(100U);
    }
    results.Check(engine.starts == 2U && worker.IsActive() &&
                      engine.continuousHint && engine.originalObserver.publish != nullptr,
                  "committed turn starts the next human Hint before CPU takeover");
    results.Check(Set(session, 811U, "VS_CPU_PLAYER", "2") &&
                      engine.Finish(session.NowMs()),
                  "acknowledged CPU-seat change retires the intervening Hint");
    for (uint8_t i = 0U; i < 30U && engine.starts < 3U; ++i) (void)session.Advance(100U);
    const bool admitted = engine.starts == 3U &&
        WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && session.CpuThinkingVisible() &&
                      engine.originalObserver.publish != nullptr,
                  "next CPU turn admits a fresh real worker with actual committed history");
    if (!admitted) return;
    const uint32_t start = session.NowMs();
    const auto* clock = session.GameClock();
    const uint32_t blackBefore = clock->GetRemainingMs(runtime::ClockSide::Black);
    results.Check(engine.PublishSample(start + 250U, 300U) &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK &&
                      engine.PublishSample(start + 500U, 600U) &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK,
                  "pending-boundary CPU obtains two worker-owned measurements");
    StaticJsonDocument<1536> home;
    results.Check(ReadHome(session, home) && home["th"].is<JsonArray>() &&
                      std::strcmp(home["th"][0] | "", "b") == 0 &&
                      home["th"][2] == 3U && session.UndoHistoryMoveCount() == 1U,
                  "pending-boundary precondition has measured Black search and one committed turn");
    modules::FenString committedFen;
    results.Check(committedFen.Assign(home["fen"] | ""), "committed boundary FEN fits cache");
    Checkpoint(results, session, engine, run, "next-search-measured", "wait-for-computer");

    if (boundary == PendingBoundary::Timeout) {
        results.Check(clock->IsEnabled() && clock->IsRunning() && !clock->IsPaused() &&
                          clock->GetActiveSide() == runtime::ClockSide::Black &&
                          blackBefore >= 500U &&
                          clock->GetRemainingMs(runtime::ClockSide::Black) == blackBefore - 500U,
                      "real CPU clock runs while the measured worker search is active");
        const uint32_t untilTimeout = clock->GetRemainingMs(runtime::ClockSide::Black);
        results.Check(untilTimeout != 0U && session.Advance(untilTimeout) == VIRTUAL_LITCH_OK &&
                          clock->HasTimedOut() &&
                          clock->GetTimedOutSide() == runtime::ClockSide::Black &&
                          ReadHome(session, home) && std::strcmp(home["tw"] | "", "w") == 0 &&
                          home["th"].isNull() && !session.CpuThinkingVisible() && worker.IsActive(),
                      "exact clock zero immediately retires Home/scanner while cancellation still owns worker");
        Checkpoint(results, session, engine, run, "timeout-draining", "game-over-start-New-Game");
        results.Check(engine.PublishSample(session.NowMs() + 250U, 900U) &&
                          session.Advance(250U) == VIRTUAL_LITCH_OK &&
                          ReadHome(session, home) && home["th"].isNull() &&
                          std::strcmp(home["tw"] | "", "w") == 0,
                      "late canceled progress cannot replace the authoritative timeout winner");
        results.Check(engine.Finish(session.NowMs()), "timed-out search reaches worker quiescence");
        (void)session.Advance(1U);
        results.Check(ReadHome(session, home) && home["th"].isNull() && home["c"].isNull() &&
                          std::strcmp(home["tw"] | "", "w") == 0 &&
                          committedFen.Equals(home["fen"] | "") &&
                          session.UndoHistoryMoveCount() == 1U &&
                          session.PhysicalOccupancyMask() == occupancy &&
                          !session.CpuThinkingVisible() && !worker.IsActive(),
                      "timeout cleanup preserves physical board/history and never queues the late CPU move");
        Checkpoint(results, session, engine, run, "timeout-settled", "game-over-start-New-Game");
        return;
    }

    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = 812U;
    request.action = VIRTUAL_LITCH_CONTROL_UNDO;
    request.undo_move_count = 1U;
    modules::ControlMutationOutcome outcome;
    results.Check(CompleteAction(session, request, outcome) &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      session.IsUndoActionPending() && worker.IsActive() &&
                      ReadHome(session, home) && home["up"] == 1U && home["th"].isNull() &&
                      !session.CpuThinkingVisible() && committedFen.Equals(home["fen"] | "") &&
                      session.UndoHistoryMoveCount() == 1U,
                  "canonical pending Undo immediately retires progress and retains committed authority during cancellation");
    Checkpoint(results, session, engine, run, "undo-draining",
               duck ? "return-Duck-to-yellow-source" : "return-last-moved-piece");
    results.Check(engine.PublishSample(session.NowMs() + 250U, 900U) &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK &&
                      ReadHome(session, home) && home["th"].isNull() &&
                      !session.CpuThinkingVisible() && session.IsUndoActionPending(),
                  "late canceled sample cannot reclaim the pending Undo display");
    results.Check(engine.Finish(session.NowMs()), "Undo cancellation reaches worker quiescence");
    (void)session.Advance(1U);
    if (duck) {
        results.Check(Move(session, occupancy, session.DuckSquare(), DUCK_FROM),
                      "raw fixed Duck return advances committed Undo to the chess inverse");
        Checkpoint(results, session, engine, run, "undo-chess", "return-last-moved-piece");
    }
    results.Check(Move(session, occupancy, priorMove.toSquare, priorMove.fromSquare),
                  "raw inverse chess lift/place completes the requested Undo");
    (void)session.Advance(1U);
    results.Check(ReadHome(session, home) && home["th"].isNull() &&
                      !session.CpuThinkingVisible() && session.UndoHistoryMoveCount() == 0U &&
                      session.PhysicalOccupancyMask() ==
                          (CHESS_OCCUPANCY | (duck ? UINT64_C(1) << DUCK_FROM : 0U)),
                  "completed Undo restores original physical position and removes one real history turn without stale search");
    Checkpoint(results, session, engine, run, "undo-complete", "human-may-move");
}

void CheckSession(Results& results, bool duck, bool black, bool firstP2, bool rotate,
                  bool cancel, PendingBoundary boundary = PendingBoundary::None,
                  uint32_t level = 3U) {
    constexpr const char* PATH = "build/test_cpu_thinking_session.json";
    char run[96];
    std::snprintf(run, sizeof(run), "%s/cpu-%c/p1-end-%u/rotate-%u/%s",
                  duck ? "Duck" : "Classic", black ? 'b' : 'w', firstP2 ? 2U : 1U,
                  rotate, cancel ? "cancel-restart" :
                  boundary == PendingBoundary::Undo ? "pending-Undo" :
                  boundary == PendingBoundary::Timeout ? "clock-timeout" : "complete");
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, duck, black, firstP2, rotate, nullptr,
                              boundary == PendingBoundary::Timeout ? 1U : 0U, level),
                  "CPU journey restores a real saved position and CPU settings");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = UINT32_MAX;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    uint64_t occupancy = CHESS_OCCUPANCY | (duck ? UINT64_C(1) << DUCK_FROM : 0U);
    results.Check(Set(session, 769U, "HINT_THINKING_STYLE", "\"foxie_sweep\""),
                  "legacy20ms sweep journey explicitly selects Foxie through the settings owner");
    if (boundary == PendingBoundary::Timeout) {
        const auto* clock = session.GameClock();
        results.Check(clock->IsRunning() && clock->IsPaused(),
                      "restored clock waits for a healthy full board scan");
        virtual_litch_runtime_health_fact fact{};
        virtual_litch_default_runtime_health_fact(&fact);
        fact.board_scan = VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
        results.Check(session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK &&
                          session.Advance(100U) == VIRTUAL_LITCH_OK &&
                          session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK &&
                          session.DispatchBoardObservation(occupancy, occupancy) ==
                              VIRTUAL_LITCH_POLICY_REJECTED &&
                          session.PhysicalOccupancyMask() == 0U && clock->IsPaused(),
                      "unhealthy full board ingress cannot authorize the restored clock");
        fact.board_scan = VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE;
        results.Check(session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK &&
                          session.Advance(100U) == VIRTUAL_LITCH_OK &&
                          session.DispatchRuntimeHealthFact(fact) == VIRTUAL_LITCH_OK &&
                          clock->IsPaused(),
                      "healthy runtime evidence still waits for the full occupancy scan");
    }
    results.Check(Observe(session, occupancy), "raw Hall observation restores the saved board");
    if (boundary == PendingBoundary::Timeout) {
        Checkpoint(results, session, engine, run, "clock-full-scan", "wait-for-stable-board");
        if (!duck) {
            // The empty Classic board entered placement during the health
            // pause. Its real stability deadline still governs clock resume.
            constexpr uint32_t stableMs =
                runtime::GameStateMachineConfig{}.placementStableRequiredMs;
            results.Check(session.Advance(stableMs) == VIRTUAL_LITCH_OK,
                          "ordinary tick reaches the actual placement stability deadline");
        }
        results.Check(session.GameClock()->IsRunning() && !session.GameClock()->IsPaused(),
                      "healthy full board scan resumes the real clock once placement is stable");
        Checkpoint(results, session, engine, run, "clock-resumed", "wait-for-computer");
    }
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && engine.starts == 1U && engine.originalObserver.publish != nullptr &&
                      session.CpuThinkingVisible(),
                  "stable restored CPU turn admits one real worker and final scanner");
    if (!admitted) { engine.releaseStats.store(true); (void)engine.Finish(session.NowMs()); return; }
    const bool physicalP2 = black != modules::ComputeControlBankSwap(rotate, firstP2, false);
    const uint32_t start = session.NowMs();
    (void)session.Advance(20U);
    results.Check(ControlBankLit(session, physicalP2, true) && ControlBankLit(session, !physicalP2, false),
                  "actual CPU admission begins fading only its mapped control bank within the first20ms");
    StaticJsonDocument<1536> home;
    Checkpoint(results, session, engine, run, "admitted", "wait-for-computer");
    results.Check(ReadHome(session, home) && home["th"][1] == 0U && home["th"][2].isNull(),
                  "admitted search starts with unknown ETA");
    const uint32_t firstNodes = level == 11U ? 750U : 300U;
    const uint32_t finalNodes = level == 11U ? 1500U : 600U;
    results.Check(engine.PublishSample(start + 250U, firstNodes), "first node sample runs on worker");
    (void)session.Advance(230U); // Preserve the 250 ms boundary after the fade sample.
    results.Check(worker.PublishedNodeCount() == firstNodes && ReadHome(session, home) &&
                      home["th"][2].isNull(), "one node sample keeps ETA unknown");
    results.Check(engine.PublishSample(start + 499U, finalNodes - 1U), "worker receives sub-cadence sample");
    (void)session.Advance(249U);
    results.Check(worker.PublishedNodeCount() == firstNodes && ReadHome(session, home) &&
                      home["th"][1] == 0U, "worker publication waits 250 ms and Home waits 500 ms");
    results.Check(engine.PublishSample(start + 500U, finalNodes), "second bounded node sample runs on worker");
    (void)session.Advance(1U);
    // Controlled throughput reproduces an excessive node-only projection.
    // Selected level11/3 deadlines cap it at62/3 seconds including the existing
    // two-second ETA cushion. This is not measured device latency.
    const uint32_t expectedSeconds = level == 11U ? 62U : 3U;
    results.Check(ReadHome(session, home) && home["th"][1] == 1U &&
                      home["th"][2] == expectedSeconds,
                  "actual Home estimate respects the selected CPU time bound after 500 ms");
    std::printf("[CPU_ETA_BOUND] level=%u elapsed_ms=500 nodes=%u remaining=%u expected=%u\n",
                level, finalNodes, home["th"][2].as<unsigned>(), expectedSeconds);
    std::fflush(stdout);
    const auto cachedLine = session.HomeStateLine();
    s_nowMs.store(start + 900U);
    results.Check(cachedLine.Equals(session.HomeStateLine().View()),
                  "repeated Home getters cannot advance cached thinking time");
    Checkpoint(results, session, engine, run, "measured", "wait-for-computer");

    if (!duck && level == 3U) {
        results.Check(Set(session, 770U, "HINT_THINKING_STYLE", "\"player_clockwise\"") &&
                          ControlBankLit(session, physicalP2, true),
                      "acknowledged style mutation immediately changes actual pending CPU controls");
        ControlCheckpoint(session, run, "player-clockwise", "wait-for-computer");
        const uint8_t bank = black ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
        const auto startsBeforeEdit = engine.starts;
        const char* editedPalette=(black!=firstP2)?"P2_END_PIECE_WHEEL":"P1_END_PIECE_WHEEL";
        results.Check(Set(session,772U,editedPalette,"84"),"palette fixture selects the real step immediately before pure green");
        results.Check(session.DispatchButton(bank, VIRTUAL_LITCH_BUTTON_UP, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OK &&
                          session.CpuThinkingVisible() && worker.IsActive(),
                      "V3.3 Up acknowledges only its direction while actual CPU thinking continues");
        const auto previewIndex = hardware::V3_3PhysicalIndexForControl(physicalP2, hardware::ControlLedRole::Up);
        const auto preview=session.PhysicalFrame().pixels[256U+previewIndex];
        results.Check(preview.g>0U&&preview.r==0U&&preview.b==0U&&
                          session.SettingsForPersistenceTest().ReadValue(editedPalette,0U)==85U&&engine.starts==startsBeforeEdit,
                      "the accepted Up control shows the actual new pure-green palette without restarting search");
        ControlCheckpoint(session, run, "Up-color-edit", "release-Up");
        results.Check(session.DispatchButton(bank, VIRTUAL_LITCH_BUTTON_UP, VIRTUAL_LITCH_BUTTON_RELEASE_EVENT) == VIRTUAL_LITCH_OK &&
                          session.CpuThinkingVisible(),
                      "normalized release restores player-clockwise feedback without a search restart");
        results.Check(session.DispatchButton(bank, VIRTUAL_LITCH_BUTTON_DOWN, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OK &&
                          session.CpuThinkingVisible() &&
                          session.DispatchButton(bank, VIRTUAL_LITCH_BUTTON_DOWN, VIRTUAL_LITCH_BUTTON_RELEASE_EVENT) == VIRTUAL_LITCH_OK &&
                          session.CpuThinkingVisible(),
                      "V3.3 Down has the same pressed-direction acknowledgement on both ends and orientations");
        results.Check(Set(session, 771U, "HINT_THINKING_STYLE", "\"foxie_sweep\""), "Foxie style restores through existing setting mutation");
        ControlCheckpoint(session, run, "edit-release", "wait-for-computer");
    }

    if (level == 11U) {
        results.Check(Set(session, 800U, "VS_CPU_PLAYER", black ? "1" : "2") &&
                          ReadHome(session, home) && home["th"].isNull(),
                      "deadline-bounded CPU cancellation clears the same Home estimate");
        results.Check(engine.Finish(session.NowMs()),
                      "deadline estimate fixture retires the canceled real worker");
        results.Check(engine.unsafeReads.load() == 0U,
                      "deadline estimate never reads the active engine from the main lane");
        return;
    }

    if (cancel) {
        results.Check(Set(session, 801U, "VS_CPU_PLAYER", black ? "1" : "2"),
                      "canonical CPU-owner change is acknowledged while search is held");
        results.Check(ReadHome(session, home) && home["th"].isNull() &&
                          !session.CpuThinkingVisible() && worker.IsActive(),
                      "cancel immediately clears Home/scanner before worker cleanup");
        modules::ThinkDiagnostics retired;
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, retired) && retired.retired && retired.active &&
                          !retired.firstAcceptedHintValid,
                      "CPU ownership change retires old request evidence while actual worker still drains");
        const uint32_t oldRequest = retired.requestId;
        results.Check(retired.configuredCpuLevel == level,
                      "retired CPU diagnostics preserve configured level from admission, not current settings");
        results.Check(engine.PublishSample(session.NowMs() + 500U, 900U),
                      "old canceled worker can still finish its bounded callback");
        (void)session.Advance(500U);
        results.Check(ReadHome(session, home) && home["th"].isNull() && engine.starts == 1U,
                      "late canceled progress neither resurrects presentation nor overlaps a new search");
        Checkpoint(results, session, engine, run, "cancel-draining", "wait-for-safe-owner-handoff");
        engine.holdStart = 2U;
        results.Check(engine.Finish(session.NowMs()), "canceled worker reaches real quiescence");
        (void)session.Advance(1U);
        (void)session.Advance(1U);
        results.Check(WaitFor([&] { return engine.pollEntered.load(); }) && engine.starts == 2U &&
                          engine.continuousHint && engine.originalObserver.publish != nullptr &&
                          ReadHome(session, home) && home["th"].isNull() &&
                          !session.CpuThinkingVisible(),
                      "replacement human Hint records progress without a CPU presentation");
        modules::ThinkDiagnostics replacement;
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Hint, replacement) &&
                          replacement.requestId != oldRequest && replacement.active && !replacement.firstAcceptedHintValid,
                      "replacement Hint diagnostics start a distinct request without inherited timestamps");
        Checkpoint(results, session, engine, run, "hint-only", "human-may-move");
        results.Check(Set(session, 802U, "VS_CPU_PLAYER", black ? "2" : "1"),
                      "canonical setting gives the same side back to CPU");
        engine.holdStart = 3U;
        results.Check(engine.Finish(session.NowMs()), "Hint retires before CPU resumes");
        for (uint8_t i = 0U; i < 30U && engine.starts < 3U; ++i) (void)session.Advance(100U);
        results.Check(WaitFor([&] { return engine.pollEntered.load(); }) && engine.starts == 3U &&
                          worker.PublishedNodeCount() == 0U &&
                          ReadHome(session, home) && home["th"][2].isNull() &&
                          session.CpuThinkingVisible(),
                      "new CPU job starts unknown without the prior job's node sample");
        Checkpoint(results, session, engine, run, "cpu-restarted", "wait-for-new-computer-search");
        virtual_litch_action_request request{};
        request.struct_size = sizeof(request);
        request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
        request.request_id = 803U;
        request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
        modules::ControlMutationOutcome outcome;
        results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Accepted &&
                          session.Advance(1U) == VIRTUAL_LITCH_OK &&
                          !session.TryTakeControlCompletion(outcome) &&
                          ReadHome(session, home) && home["th"].isNull() &&
                          !session.CpuThinkingVisible() && worker.IsActive(),
                      "one New Game request remains pending while immediately clearing canceled CPU progress");
        Checkpoint(results, session, engine, run, "new-game-draining", "finish-worker-and-complete-New-Game");
        results.Check(engine.Finish(session.NowMs()), "New Game's canceled CPU worker exits");
        results.Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
                          session.TryTakeControlCompletion(outcome) &&
                          outcome.requestId == request.request_id &&
                          outcome.status == modules::ControlMutationStatus::Applied &&
                          ReadHome(session, home) && home["th"].isNull() &&
                          std::strcmp(home["fen"] | "", modules::INITIAL_FEN) == 0 &&
                          !session.CpuThinkingVisible(),
                      "the accepted New Game completes without a second gesture or stale CPU progress");
        results.Check(!ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, retired) && retired.requestId == 0U,
                      "authoritative New Game position/variant rejects old CPU diagnostics instead of exposing stale progress");
        Checkpoint(results, session, engine, run, "new-game-ready", "arrange-starting-pieces");
        results.Check(engine.Finish(session.NowMs()), "New Game leaves no worker using the retired position");
        results.Check(engine.unsafeReads.load() == 0U, "all cancellation ownership reads stay safe");
        return;
    }

    const uint8_t lifted = black ? 52U : 11U;
    const uint64_t corrected = occupancy & ~(UINT64_C(1) << lifted);
    results.Check(Observe(session, corrected) && session.Advance(100U) == VIRTUAL_LITCH_OK,
                  "wrong physical pickup reaches the production correction reducer");
    results.Check(ReadHome(session, home) && std::strcmp(home["a"] | "", "corr") == 0 &&
                      home["th"].is<JsonArray>() && !session.CpuThinkingVisible() &&
                      worker.IsActive() && engine.cancels.load() == 0U,
                  "correction hides scanner while the same CPU search continues");
    Checkpoint(results, session, engine, run, "correction", "return-lifted-piece");
    results.Check(Observe(session, occupancy) && session.Advance(250U) == VIRTUAL_LITCH_OK &&
                      session.CpuThinkingVisible() && engine.starts == 1U,
                  "returning the piece restores the existing search scanner");
    Checkpoint(results, session, engine, run, "corrected", "wait-for-computer");
    (void)Observe(session, corrected);
    engine.releaseStats.store(false);
    s_nowMs.store(session.NowMs());
    engine.releasePoll.store(true);
    const bool stats = WaitFor([&] { return engine.statsEntered.load(); });
    results.Check(stats && worker.IsActive() && worker.PublishedNodeCount() == 0U,
                  "Poll completion clears measurement before held statistics cleanup");
    if (!stats) { engine.releaseStats.store(true); (void)engine.Finish(session.NowMs()); return; }
    results.Check(ReadHome(session, home) && home["th"][2].isNull() &&
                      home["th"].is<JsonArray>(), "cleanup immediately projects unknown without a main-loop engine read");
    (void)session.Advance(500U);
    Checkpoint(results, session, engine, run, "cleanup", "return-lifted-piece");
    engine.releaseStats.store(true);
    results.Check(engine.Finish(session.NowMs()), "actual search and cleanup both finish");
    (void)session.Advance(1U);
    results.Check(ReadHome(session, home) && home["th"].isNull() && !session.CpuThinkingVisible(),
                  "completed selection waiting for correction clears CPU presentation immediately");
    Checkpoint(results, session, engine, run, "ready-awaiting-correction", "return-lifted-piece");
    (void)Observe(session, occupancy);
    for (uint8_t i = 0U; i < 20U; ++i) {
        if (ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0) break;
        (void)session.Advance(100U);
    }
    modules::UciMove uci;
    const char* from = home["c"][0] | "";
    const char* to = home["c"][1] | "";
    litch_max::ParsedUciMove move;
    const bool selected = std::strcmp(home["a"] | "", "cpu") == 0 &&
        uci.Format("%s%s", from, to) && litch_max::TryParseUciMove(uci.CStr(), move);
    results.Check(selected && home["th"].isNull() && !session.CpuThinkingVisible(),
                  "real search result replaces progress with the CPU physical move prompt");
    if (duck && !black && !firstP2 && !rotate && boundary == PendingBoundary::None) {
        modules::ThinkDiagnostics diagnostic;
        modules::DuckThinkResult actual;
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, diagnostic) &&
                          diagnostic.isDuck && diagnostic.effectiveValid && diagnostic.effective.maxDepth <= 15U &&
                          diagnostic.latestPublication.rootKind == modules::ThinkRootKind::DuckCompound &&
                          diagnostic.latestPublication.scoreUnits == modules::ThinkScoreUnits::DuckPawn1000 &&
                          diagnostic.latestPublication.scorePerspective == modules::SideToMove::White &&
                          engine.GetLastDuckThinkResult(actual) && diagnostic.latestPublication.score == actual.score &&
                          diagnostic.completedRootCount != 0U && diagnostic.finalValid && diagnostic.quiescentValid &&
                          !diagnostic.firstAcceptedHintValid && !diagnostic.active,
                      "real Duck CPU compound result preserves completed turns, raw score scale, perspective and terminal availability");
    }
    Checkpoint(results, session, engine, run, "selected", "move-indicated-computer-piece");
    if (selected) {
        results.Check(Move(session, occupancy, move.fromSquare, move.toSquare),
                      "raw Hall lift/place acknowledges the real selected chess move");
        if (duck) {
            results.Check(ReadHome(session, home) && std::strcmp(home["a"] | "", "duck") == 0,
                          "CPU chess move advances to its fixed Duck placement prompt");
            const char* relocation = home["dr"] | "";
            uint8_t duckTo = 64U;
            if (std::strlen(relocation) == 5U) {
                (void)litch_max::TryParseUciSquare(relocation + 3U, duckTo);
            }
            Checkpoint(results, session, engine, run, "duck-placement", "move-Duck-to-yellow-target");
            results.Check(Move(session, occupancy, DUCK_FROM, duckTo),
                          "raw fixed Duck relocation completes the compound CPU turn");
        }
        results.Check(session.UndoHistoryMoveCount() == 1U &&
                          ReadHome(session, home) && home["th"].isNull(),
                      "physical acknowledgement commits exactly one turn without stale progress");
        Checkpoint(results, session, engine, run, "committed", "human-may-move");
        if (boundary != PendingBoundary::None) {
            CheckPendingBoundary(results, session, engine, worker, run, duck,
                                 occupancy, move, boundary);
        }
    }
    results.Check(engine.Finish(session.NowMs()) && engine.unsafeReads.load() == 0U &&
                      !engine.wrongCallbackThread.load(),
                  "actual engine search callbacks and all lifecycle reads respect worker ownership");
}

void DeadlineCheckpoint(Results& results, const Session& session,
                        const GatedEngine& engine, const char* run,
                        const char* name, const char* next) {
    Checkpoint(results, session, engine, run, name, next);
    std::printf("[CPU_DEADLINE_V3_CUES] run=%s checkpoint=%s mover=%s action_token=%lu history=%lu squares=[",
                run, name, session.State().side_to_move_black ? "black" : "white",
                static_cast<unsigned long>(session.State().pending_action_token),
                static_cast<unsigned long>(session.UndoHistoryMoveCount()));
    for (const uint8_t square : {22U, 30U}) {
        std::printf("%c%c{", 'a' + square % 8U, '8' - square / 8U);
        for (auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Piece,
                          hardware::SquareLedSlot::Square2}) {
            const auto color = session.SemanticFrame().board[square][static_cast<uint8_t>(slot)];
            std::printf("slot%u:semantic=%u,%u,%u,%u/mapped=", static_cast<unsigned>(slot),
                        color.r, color.g, color.b, color.w);
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, square, slot, session.HallRotate());
            for (uint8_t i = 0U; i < targets.count; ++i) {
                const auto mapped = session.PhysicalFrame().pixels[targets.indices[i]];
                std::printf("%u:%u,%u,%u,%u;", targets.indices[i], mapped.r, mapped.g, mapped.b, mapped.w);
            }
        }
        std::printf("}");
    }
    constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
    std::printf("] mapped_status=[");
    for (uint8_t i = 0U; i < VIRTUAL_LITCH_STATUS_LED_COUNT; ++i) {
        const auto color = session.PhysicalFrame().pixels[topology.physicalBoardLedCount + i];
        std::printf("%u:%u,%u,%u,%u;", i, color.r, color.g, color.b, color.w);
    }
    std::puts("]");
    std::fflush(stdout);
}

void CheckCpuDeadlineJourney(Results& results, bool retainFullRoot) {
    constexpr const char* PATH = "build/test_cpu_deadline_journey.json";
    constexpr const char* FEN = "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31";
    const char* run = retainFullRoot ? "Classic/V3/CPU4/deadline-retained-Full"
                                    : "Classic/V3/CPU4/deadline-ranked-Seed-recovery";
    // Saved imported FEN is setup only; Hall observations, worker terminal
    // publication, GPC selection, rendering and physical ACK remain production.
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, true, false, false, FEN, 0U, 4U),
                  "deadline journey restores imported position with selected CPU4");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = UINT32_MAX;
    engine.cpuExpiry = retainFullRoot ? CpuExpiry::AfterFullRoot : CpuExpiry::BeforeRoot;
    engine.releaseStats.store(false);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    uint64_t occupancy = session.State().expected_occupied_mask;
    results.Check(Observe(session, occupancy), "full physical scan reconciles the imported CPU position");
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && engine.starts == 1U && engine.lastConfig.difficulty == 4U &&
                      engine.lastConfig.maxThinkMs == 1400U && session.CpuThinkingVisible(),
                  "production selected CPU4 factory reaches the real async worker and thinking cues");
    if (!admitted) { engine.releaseStats.store(true); (void)engine.Finish(session.NowMs()); return; }
    DeadlineCheckpoint(results, session, engine, run, "admitted", "wait-for-computer");
    s_nowMs.store(session.NowMs());
    engine.releasePoll.store(true, std::memory_order_release);
    const bool cleanup = WaitFor([&] { return engine.statsEntered.load(); });
    modules::ThinkDiagnostics cleanupDiagnostic;
    results.Check(cleanup && worker.IsActive() && !worker.IsDone() &&
                      worker.PublishedNodeCount() <= engine.budget && engine.cpuDeadlineForced.load() &&
                      ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, cleanupDiagnostic) &&
                      !cleanupDiagnostic.finalValid && !cleanupDiagnostic.quiescentValid,
                  "actual expiry finishes polling but holds terminal publication until worker cleanup");
    if (!cleanup) { engine.releaseStats.store(true); (void)engine.Finish(session.NowMs()); return; }
    results.Check(session.Advance(engine.lastConfig.maxThinkMs) == VIRTUAL_LITCH_OK,
                  "session rendering reaches the configured CPU4 expiry while cleanup is held");
    StaticJsonDocument<1536> home;
    results.Check(ReadHome(session, home) && home["th"].is<JsonArray>() &&
                      std::strcmp(home["a"] | "", "cpu") != 0 && session.UndoHistoryMoveCount() == 0U,
                  "held cleanup cannot publish a ready prompt, partial move or history entry");
    DeadlineCheckpoint(results, session, engine, run, "cleanup-held", "wait-for-worker-cleanup");
    engine.releaseStats.store(true, std::memory_order_release);
    results.Check(engine.Finish(session.NowMs()) && !worker.IsActive() && worker.IsDone(),
                  "real worker publishes terminal outcome only after quiescent cleanup");
    uint8_t progress = 0U;
    modules::UciMove best;
    uint32_t elapsed = 0U, nodes = 0U;
    const auto terminal = worker.GetResult(progress, best, elapsed, nodes);
    modules::ClassicThinkResult analyzed;
    const bool haveAnalysis = engine.GetLastClassicThinkResult(analyzed);
    std::printf("[CPU_DEADLINE_TERMINAL] run=%s factory_ms=%u state=%u elapsed=%u nodes=%u move=%s candidates=%u analyzed=%u preliminary=%u depth=%u\n",
                run, engine.lastConfig.maxThinkMs, static_cast<unsigned>(terminal), elapsed, nodes,
                best.CStr(), engine.cpuCandidates.load(), haveAnalysis,
                haveAnalysis && analyzed.preliminaryOnly, engine.cpuLastDepth.load());
    results.Check(session.Advance(1U) == VIRTUAL_LITCH_OK && ReadHome(session, home) &&
                      home["th"].isNull() && !session.CpuThinkingVisible(),
                  "coordinator consumes the terminal result and clears stale thinking presentation");
    if (retainFullRoot) {
        results.Check(terminal == modules::ThinkState::Done && best.Equals("g6g5") &&
                          engine.cpuCompletedFullNodes.load() != 0U &&
                          nodes == engine.cpuCompletedFullNodes.load() && engine.cpuCandidates.load() == 3U && haveAnalysis &&
                          analyzed.bestScoreValid && !analyzed.preliminaryOnly && engine.cpuLastDepth.load() == 1U,
                      "deadline retains the real completed Full root without fabricated selection");
        modules::UciMove queued;
        litch_max::ParsedUciMove move;
        const bool ready = std::strcmp(home["a"] | "", "cpu") == 0 &&
            queued.Format("%s%s", home["c"][0] | "", home["c"][1] | "") &&
            queued.Equals(best.View()) && litch_max::TryParseUciMove(queued.CStr(), move);
        results.Check(ready && CapturedSquareLit(session, 22U) && CapturedSquareLit(session, 30U),
                      "retained analyzed queue names its legal source/target and lights every mapped V3 prompt LED");
        DeadlineCheckpoint(results, session, engine, run, "ready", "move-g6-pawn-to-g5");
        if (ready) {
            results.Check(Move(session, occupancy, move.fromSquare, move.toSquare) &&
                              session.UndoHistoryMoveCount() == 1U && !session.State().side_to_move_black,
                          "normal physical lift/place ACK commits exactly one CPU move and hands over to human");
            DeadlineCheckpoint(results, session, engine, run, "committed", "human-white-may-move");
            results.Check(Observe(session, occupancy) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                              session.UndoHistoryMoveCount() == 1U && ReadHome(session, home) && home["th"].isNull(),
                          "repeated settled scan cannot commit twice or resurrect CPU thinking");
        }
    } else {
        litch_max::ParsedUciMove seedMove;
        modules::ThinkDiagnostics seedDiagnostic;
        modules::UciMove queued;
        const bool seedReady = litch_max::TryParseUciMove(best.CStr(), seedMove) &&
            std::strcmp(home["a"] | "", "cpu") == 0 &&
            queued.Format("%s%s", home["c"][0] | "", home["c"][1] | "") && queued.Equals(best.View());
        results.Check(terminal == modules::ThinkState::Done && nodes == 128U &&
                          haveAnalysis && analyzed.source == modules::ThinkCandidateSource::Seed &&
                          engine.cpuCandidates.load() == 1U && engine.cpuLastDepth.load() == 0U &&
                          ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, seedDiagnostic) &&
                          seedDiagnostic.completedRootCount == 0U &&
                          seedDiagnostic.latestPublication.rootKind == modules::ThinkRootKind::Seed &&
                          !seedDiagnostic.firstAcceptedHintValid && seedReady,
                      "expiry before a root retains only the current ranked legal Seed with truthful depth-zero provenance");
        DeadlineCheckpoint(results, session, engine, run, "ranked-Seed-ready", "follow-the-legal-Seed-or-change-owner");
        const uint32_t selectedAt = session.NowMs();
        const uint64_t wrong = occupancy & ~(UINT64_C(1) << 49U);
        results.Check(Observe(session, wrong) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                          session.State().physical_occupied_mask == wrong &&
                          session.State().cpu_illegal_mask == (UINT64_C(1) << 49U) &&
                          ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0 &&
                          queued.Format("%s%s", home["c"][0] | "", home["c"][1] | "") && queued.Equals(best.View()) &&
                          engine.starts == 1U && !worker.IsActive() && session.UndoHistoryMoveCount() == 0U,
                      "wrong-piece input latches only b2 correction while the same legal Seed plan waits without acknowledgement");
        results.Check(session.Advance(modules::CORRECTION_QUIET_MS - 100U) == VIRTUAL_LITCH_OK &&
                          CapturedAmberSquare(session, 49U),
                      "the existing300ms correction boundary shows b2 amber on semantic and mapped LEDs");
        DeadlineCheckpoint(results, session, engine, run, "Seed-correction", "return-b2-pawn");
        results.Check(Observe(session, occupancy) && session.Advance(250U) == VIRTUAL_LITCH_OK &&
                          session.Advance(selectedAt + 999U - session.NowMs()) == VIRTUAL_LITCH_OK &&
                          session.State().cpu_illegal_mask == 0ULL &&
                          engine.starts == 1U && !worker.IsActive() && session.UndoHistoryMoveCount() == 0U,
                      "returning the piece restores the retained Seed without duplicate search or premature move acknowledgement");
        results.Check(Set(session, 891U, "VS_CPU_PLAYER", "1") &&
                          ReadHome(session, home) && home["th"].isNull() &&
                          std::strcmp(home["a"] | "", "cpu") != 0,
                      "explicit owner change retires the pending Seed without changing game history");
        engine.cpuExpiry = CpuExpiry::None;
        engine.holdStart = 2U;
        if (seedReady) {
            if ((occupancy & (UINT64_C(1) << seedMove.toSquare)) != 0ULL) {
                occupancy &= ~(UINT64_C(1) << seedMove.toSquare);
                results.Check(Observe(session, occupancy), "physical capture removes the Seed target before placement");
            }
            results.Check(Move(session, occupancy, seedMove.fromSquare, seedMove.toSquare) &&
                              session.UndoHistoryMoveCount() == 1U && !session.State().side_to_move_black,
                          "the now-human owner acknowledges the same legal move through real Hall input exactly once");
            for (uint8_t tick = 0U; tick < 30U && engine.starts < 2U; ++tick) (void)session.Advance(100U);
            results.Check(WaitFor([&] { return engine.pollEntered.load(); }) && engine.starts == 2U &&
                              worker.IsActive() && session.CpuThinkingVisible() && worker.PublishedNodeCount() == 0U,
                          "the next real CPU turn starts a fresh bounded job after explicit Seed retirement");
            DeadlineCheckpoint(results, session, engine, run, "next-turn-admitted", "wait-for-computer-or-cancel");
            results.Check(Set(session, 892U, "VS_CPU_PLAYER", "2") &&
                              ReadHome(session, home) && home["th"].isNull() && !session.CpuThinkingVisible(),
                          "normal owner change cancels the next job and clears its presentation immediately");
            results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                              ReadHome(session, home) && home["th"].isNull() &&
                              std::strcmp(home["a"] | "", "cpu") != 0 && session.UndoHistoryMoveCount() == 1U,
                          "canceled next-turn work cleans up without another move or stale CPU prompt");
        }
        DeadlineCheckpoint(results, session, engine, run, "canceled-human-turn", "human-white-may-move");
    }
    engine.releaseStats.store(true);
    results.Check(engine.Finish(session.NowMs()) && engine.unsafeReads.load() == 0U &&
                      !engine.wrongCallbackThread.load(),
                  "deadline lifecycle leaves no worker or forbidden engine read on the main lane");
    std::printf("[CPU_DEADLINE_JOURNEY_SEAMS] run=%s imported_saved_FEN=1 forced_clock_ms=2000 "
                "real_search_worker_coordinator_physical_ACK=1 mapped_V3=1 optical_S3_timing_claim=0\n", run);
}

void CheckRealSearchCallbacks(Results& results, bool duck) {
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = 0U;
    results.Check(engine.SetFen(modules::INITIAL_FEN), "real callback fixture installs initial FEN");
    modules::ThinkConfig config;
    config.maxNodes = 8000U;
    config.maxDepth = 8U;
    config.rngSeed = 7U;
    config.progressObserver = worker.PrepareProgressObserver();
    const bool started = duck ? engine.StartDuckThink(modules::INITIAL_FEN, 24U, config)
                              : engine.StartThink(config);
    results.Check(started && worker.Start(engine) && engine.Finish(100000U),
                  "production Classic/Duck search executes on the real async worker");
    results.Check(engine.actualSamples.load() != 0U && !engine.wrongCallbackThread.load() &&
                      worker.PublishedNodeCount() == 0U && engine.unsafeReads.load() == 0U,
                  "actual engine publishes bounded scalar progress on worker and clears it at completion");
    uint8_t progress = 0U;
    modules::UciMove best;
    uint32_t elapsed = 0U;
    uint32_t nodes = 0U;
    const auto state = worker.GetResult(progress, best, elapsed, nodes);
    results.Check(state == modules::ThinkState::Done && !best.Empty() && nodes != 0U,
                  "progress observer preserves the engine's actual completed move and node statistics");
    std::printf("[CPU_THINKING_REAL_SEARCH] variant=%s callbacks=%lu nodes=%lu best=%s state=%u unsafe=%lu\n",
                duck ? "Duck" : "Classic", static_cast<unsigned long>(engine.actualSamples.load()),
                static_cast<unsigned long>(nodes), best.CStr(), static_cast<unsigned>(state),
                static_cast<unsigned long>(engine.unsafeReads.load()));
}

void CheckIncompleteOpeningContext(Results& results) {
    constexpr const char* PATH = "build/test_book_unknown_prefix.json";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, false, false, false, modules::INITIAL_FEN,
                      0U, 0U, true, 4U), "mismatched complete-prefix fixture preserves acknowledged ply identity");
    modules::AsyncThinkWorker worker; GatedEngine engine(worker); engine.holdStart = 0U;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    results.Check(Observe(session, session.State().expected_occupied_mask),
                  "actual Hall scan admits a restored initial-looking position with missing played prefix");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    results.Check(engine.starts == 1U && !engine.lastConfig.openingPrefix.fromCanonicalStart &&
                      engine.lastConfig.reversibleHalfmoves == UINT16_MAX,
                  "actual GPC cannot mistake lost-prefix repetition or draw-clock history for a fresh Book opening");
    engine.CancelThink();
    results.Check(engine.Finish(session.NowMs()) && engine.unsafeReads.load() == 0U,
                  "unknown-prefix context drains its ordinary search without lane violations");
}

void CheckBookHintFirstRevealJourney(Results& results) {
    constexpr const char* PATH = "build/test_book_hint_reveal.json";
    constexpr const char* RUN = "Classic/acknowledged-opening/Book-orders-scored-Hint";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, false, false, false, modules::INITIAL_FEN,
                      0U, 0U, true), "Book journey restores complete acknowledged canonical history");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker); engine.holdStart = 0U; engine.gateCandidates = true;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    const uint64_t occupancy = session.State().expected_occupied_mask;
    results.Check(Observe(session, occupancy), "raw Hall scan admits acknowledged opening history");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    const bool fullReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
    results.Check(fullReady && engine.starts == 1U && worker.IsActive(),
                  "real scored roots reach the selected deeper geometry publication gate");
    if (!fullReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    const auto first = engine.completedCandidates[5U];
    const uint32_t start = engine.startedAtMs;
    modules::ThinkCandidate candidate;
    modules::ThinkDiagnostics diagnostic;
    results.Check(engine.lastConfig.openingPrefix.fromCanonicalStart &&
                      engine.lastConfig.openingPrefix.moveCount == 0U &&
                      engine.lastConfig.reversibleHalfmoves == 0U && engine.lastConfig.minimumCalculationMs == 500U &&
                      !worker.TryGetPublishedCandidate(candidate) &&
                      ReadDiagnostics(session, modules::ThinkRequestKind::Hint, diagnostic) &&
                      !diagnostic.firstPublicationValid && !diagnostic.firstAcceptedHintValid &&
                      diagnostic.firstFull3RootValid && first.scoreValid,
                  "opening knowledge orders actual scored work; neither Seed nor Book bypasses first reveal");
    const auto bookConsole = ReadDiagnosticConsole(session);
    results.Check(bookConsole.find("first_full3_ms=" + std::to_string(
                      diagnostic.firstFull3RootAtMs - diagnostic.requestAtMs)) != std::string::npos,
                  "completed Full3 work is reported independently of the held first publication");
    virtual_litch_action_request request{}; request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION; request.request_id = 860U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT; modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied && session.HintDisplayRequested(),
                  "typed Hint retains pending intent without restarting opening analysis");
    (void)session.Advance(start + 499U - session.NowMs());
    uint8_t from = 64U, to = 64U;
    results.Check(!session.TryGetHintMoveSquares(from, to),
                  "manual opening Hint cannot reveal Book or Seed at499ms");
    results.Check(Observe(session, occupancy & ~(UINT64_C(1) << first.fromSquare)) &&
                      session.State().selected_square == first.fromSquare,
                  "physical pickup uses the actual scored source and ordinary legal destinations");
    results.Check(engine.ReleaseCandidate(5U, start + 500U) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      worker.TryGetPublishedCandidate(candidate) && candidate.source == modules::ThinkCandidateSource::Search &&
                      candidate.completedDepth == 5U && session.TryGetHintMoveSquares(from, to) &&
                      from == first.fromSquare && to == first.toSquare,
                  "scored search guidance becomes visible through the real mailbox and Hall phase at500ms");
    StaticJsonDocument<1536> home;
    std::string prefix = "1l";
    prefix += static_cast<char>('a' + first.fromSquare % 8U);
    prefix += static_cast<char>('8' - first.fromSquare / 8U);
    std::string destination;
    destination += static_cast<char>('a' + first.toSquare % 8U);
    destination += static_cast<char>('8' - first.toSquare / 8U);
    char expectedAssistance[24]{};
    std::snprintf(expectedAssistance, sizeof(expectedAssistance), "%s%016llX%s",
                  prefix.c_str(), static_cast<unsigned long long>(session.State().valid_destination_mask),
                  destination.c_str());
    results.Check(ReadHome(session, home) && std::strcmp(home["as"] | "", expectedAssistance) == 0,
                  "Home projects the exact scored source, legal mask and target suffix for the same physical pickup");
    results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Hint, diagnostic) &&
                      diagnostic.firstPublicationValid && diagnostic.firstPublicationAtMs == start + 500U &&
                      diagnostic.firstAcceptedHintValid && diagnostic.firstAcceptedHintAtMs == start + 500U &&
                      diagnostic.latestPublication.rootKind == modules::ThinkRootKind::ClassicFull,
                  "first publication and acceptance record scored calculation after500ms, with no earlier Seed acceptance");
    const uint32_t firstPublicationAt = diagnostic.firstPublicationAtMs;
    const uint32_t firstAcceptedAt = diagnostic.firstAcceptedHintAtMs;
    // In the immutable canonical opening only b1/g1 are White knights.
    // Another file two columns from the target is outside their long-leg-first
    // route and the same-file pawn route; all initial landing ranks are empty.
    const bool knight = first.fromSquare == 57U || first.fromSquare == 62U;
    const uint8_t neighbor = static_cast<uint8_t>(first.toSquare / 8U * 8U +
        (first.toSquare % 8U + 2U) % 8U);
    const auto available = modules::ui_color_wheel::WheelToRgb(
        static_cast<uint8_t>(modules::player_end_palette::P1_END_DEFAULT_WHEEL + 16U));
    uint32_t sampled = 0U;
    for (const uint32_t elapsed : {0U, 80U, 200U, 450U}) {
        results.Check(session.Advance(elapsed - sampled) == VIRTUAL_LITCH_OK &&
                          session.TryGetHintMoveSquares(from, to) && from == first.fromSquare && to == first.toSquare &&
                          ActiveHintLanding(session, first.toSquare, neighbor, available, knight) &&
                          worker.IsActive() &&
                          engine.starts == 1U && engine.unsafeReads.load() == 0U,
                      "actual opening landing retains rails, Piece pair and every mapped emitter while the offroute neighbor keeps ordinary guidance");
        sampled = elapsed;
    }
    const bool refined = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(refined && engine.ReleaseCandidate(6U, session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      session.TryGetHintMoveSquares(from, to) && from == first.fromSquare && to == first.toSquare,
                  "a real deeper scored root cannot retarget the revealed held piece");
    results.Check(Observe(session, occupancy), "replacing the scored source restores normal Idle");
    (void)session.Advance(300U);
    results.Check(session.TryGetHintMoveSquares(from, to) && from == engine.completedCandidates[6U].fromSquare &&
                      to == engine.completedCandidates[6U].toSquare &&
                      ReadDiagnostics(session, modules::ThinkRequestKind::Hint, diagnostic) &&
                      diagnostic.firstPublicationAtMs == firstPublicationAt && diagnostic.firstAcceptedHintAtMs == firstAcceptedAt &&
                      diagnostic.latestPublication.completedDepth == 6U,
                  "stable Idle adopts the pending actual refinement while first timestamps remain stable");
    const uint8_t other = first.fromSquare == 48U ? 55U : 48U;
    results.Check(Observe(session, occupancy & ~(UINT64_C(1) << other)) &&
                      ((session.State().valid_destination_mask >> (other - 8U)) & 1ULL) != 0ULL,
                  "another physical piece retains its own ordinary legal destinations");
    results.Check(Observe(session, occupancy), "returning the other piece recovers Idle");
    Checkpoint(results, session, engine, RUN, "scored-opening-Hint", "consider-current-search-guidance");
    engine.CancelThink();
    results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      engine.unsafeReads.load() == 0U && engine.starts == 1U,
                  "real cancellation drains one worker without a restart or mutable engine read");
    std::printf("[BOOK_HINT_JOURNEY] actual_gpc=1 actual_search=1 raw_hall=1 home=1 Book=ordering-only Seed=hidden first_ms=500 selected_geometry_depth=5 schedule_controlled=1 physical_timing_claim=0\n");
}

void CheckHintFirstRevealJourney(Results& results, bool refinementBeforeReveal,
                                bool lateManualRequest = false) {
    constexpr const char* PATH = "build/test_hint_first_reveal.json";
    constexpr const char* FEN =
        "r2n1rk1/ppp2p2/5p1p/2bPp3/2P5/5PP1/PP2N2P/2KR1B1R b - - 2 14";
    const char* run = lateManualRequest ? "Classic/real-root/first-manual-Hint-after-pickup"
        : refinementBeforeReveal
        ? "Classic/real-root/refinement-before-first-reveal"
        : "Classic/real-root/refinement-after-first-reveal";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, true, false, false, FEN, 0U, 0U),
                  "first-reveal journey restores the actual retained human corpus position");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = 0U;
    engine.gateCandidates = true;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    const uint64_t occupancy = session.State().expected_occupied_mask;
    results.Check(Observe(session, occupancy), "raw Hall full scan admits the saved human turn");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    const bool firstReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
    results.Check(firstReady && engine.starts == 1U && worker.IsActive(),
                  "production Full search completes its first root on the owned worker");
    if (!firstReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    results.Check(!engine.lastConfig.openingPrefix.fromCanonicalStart &&
                      engine.lastConfig.reversibleHalfmoves == UINT16_MAX,
                  "actual imported-position GPC context declines unknown clock and opening provenance");
    const uint32_t start = engine.startedAtMs + 450U;
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = 850U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    if (!lateManualRequest&&!refinementBeforeReveal) results.Check(
                      session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      session.HintDisplayRequested(),
                  "typed manual Hint acknowledges the reveal without interrupting search");
    results.Check(ControlBankLit(session, true, false) && ControlBankLit(session, false, false),
                  "explicit Hint starts its fade from dark; unrequested background analysis stays quiet");
    ControlCheckpoint(session, run, "Hint-request-pending", "wait-for-Hint");
    const auto hintIs = [&](uint8_t expectedTo) {
        uint8_t from = 64U, to = 64U;
        return session.TryGetHintMoveSquares(from, to) && from == 8U && to == expectedTo;
    };
    const auto noHint = [&] {
        uint8_t from = 64U, to = 64U;
        return !session.TryGetHintMoveSquares(from, to);
    };
    results.Check(engine.completedCandidates[5U].fromSquare == 8U &&
                      engine.completedCandidates[5U].toSquare == 16U &&
                      engine.ReleaseCandidate(5U, start + 50U) &&
                      session.Advance(start + 50U - session.NowMs()) == VIRTUAL_LITCH_OK &&
                      hintIs(16U) &&
                      session.HintDisplayRequested() == (!lateManualRequest && !refinementBeforeReveal),
                  "selected scored geometry root is prepared at500ms while only the accepted manual request reveals it");
    ControlCheckpoint(session, run, "first-root-cached-before-reveal", "wait-for-Hint");
    modules::ThinkDiagnostics diagnostic;
    results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Hint, diagnostic) &&
                      diagnostic.firstPublicationValid && diagnostic.firstPublicationAtMs == start + 50U &&
                      diagnostic.firstAcceptedHintValid && diagnostic.firstAcceptedHintAtMs == start + 50U &&
                      diagnostic.fullDepth >= 5U &&
                      diagnostic.completedRootCount >= 5U && diagnostic.firstPublication.rootKind == modules::ThinkRootKind::ClassicFull &&
                      diagnostic.firstPublication.scoreUnits == modules::ThinkScoreUnits::ClassicPawn100 &&
                      diagnostic.firstPublication.scorePerspective == modules::SideToMove::Black,
                  "diagnostics record acceptance of the scored prepared result at500ms independently of display intent");
    const uint32_t firstPublication = diagnostic.firstPublicationAtMs;
    (void)session.Advance(50U);
    results.Check(Observe(session, occupancy & ~(UINT64_C(1) << 8U)) &&
                      session.State().selected_square == 8U &&
                      hintIs(16U) &&
                      session.HintDisplayRequested() == (!lateManualRequest && !refinementBeforeReveal),
                  "a7 pickup retains the actual already shown target and never invents a background request");
    Checkpoint(results, session, engine, run, "pickup-before-first-reveal", "choose-legal-a7-destination");
    // The smaller production cache retains a7a6 at Full6; Full7 changes to a7a5.
    // Admit that real unchanged intermediate root before gating the refinement.
    const bool intermediateReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(intermediateReady && engine.completedCandidates[6U].fromSquare == 8U &&
                      engine.completedCandidates[6U].toSquare == 16U &&
                      engine.ReleaseCandidate(6U, start + 100U),
                  "actual a7a6 depth6 preserves the first target before the distinct depth7 root");
    if (!intermediateReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    const bool refinedReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 7U; });
    results.Check(refinedReady && engine.completedCandidates[7U].fromSquare == 8U &&
                      engine.completedCandidates[7U].toSquare == 24U,
                  "same real search completes the different a7a5 depth7 refinement");
    if (!refinedReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    if (refinementBeforeReveal) {
        results.Check(engine.ReleaseCandidate(7U, start + 400U) &&
                          session.Advance(300U) == VIRTUAL_LITCH_OK &&
                          hintIs(24U) && !session.HintDisplayRequested(),
                      "refinement before first reveal remains hidden until the same first-request boundary after the calculation minimum");
    }
    (void)session.Advance(start + 499U - session.NowMs());
    results.Check(hintIs(refinementBeforeReveal ? 24U : 16U) &&
                      session.HintDisplayRequested() == (!lateManualRequest && !refinementBeforeReveal),
                  "requested guidance remains stable while later unrequested refinements stay hidden");
    (void)session.Advance(1U);
    if(refinementBeforeReveal)results.Check(session.DispatchControlAction(request,outcome)==VIRTUAL_LITCH_OK&&
                      outcome.status==modules::ControlMutationStatus::Applied&&session.HintDisplayRequested(),
                  "first manual O consumes the latest scored background refinement after the calculation minimum");
    const uint32_t firstAcceptedElapsed = 500U;
    const uint8_t initialTarget = refinementBeforeReveal ? 24U : 16U;
    results.Check(hintIs(initialTarget) && worker.IsActive() &&
                      session.HintDisplayRequested() != lateManualRequest &&
                      ((session.State().valid_destination_mask >> initialTarget) & 1ULL) != 0ULL,
                  "first reveal uses the latest actual completed result, not the unseen cached older target");
    results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Hint, diagnostic) &&
                      diagnostic.firstPublicationAtMs == firstPublication && diagnostic.firstAcceptedHintValid &&
                      diagnostic.firstAcceptedHintAtMs - diagnostic.requestAtMs == firstAcceptedElapsed,
                  "accepted useful Hint records its actual manual or background publication branch and preserves first worker timestamp");
    const auto console = ReadDiagnosticConsole(session);
    results.Check(console.find("source=ClassicFull") != std::string::npos &&
                      console.find("first_worker_ms=500") != std::string::npos &&
                      console.find("first_hint_ms="+std::to_string(firstAcceptedElapsed)) != std::string::npos &&
                      console.find("depth_ceiling=8") != std::string::npos,
                  "existing Console stats reads cached active real Hint and reports ceiling separately from actual depth");
    if (refinementBeforeReveal) std::printf("[ACTIVE_HINT_DIAGNOSTIC_CONSOLE]\n%s", console.c_str());
    Checkpoint(results, session, engine, run, "latest-first-visible", "consider-visible-a7-target");
    if (!refinementBeforeReveal) {
        results.Check(engine.ReleaseCandidate(7U, start + 600U) &&
                          session.Advance(100U) == VIRTUAL_LITCH_OK &&
                          hintIs(lateManualRequest ? 24U : 16U),
                      "actual refinement updates a hidden target but preserves an already requested held target");
    }
    if (lateManualRequest) {
        (void)session.Advance(100U);
        results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Applied &&
                          session.HintDisplayRequested() && hintIs(24U),
                      "first late manual request reveals the latest actual matching result during pickup");
        Checkpoint(results, session, engine, run, "first-manual-request", "consider-a7a5");
    }
    const uint8_t firstTarget = refinementBeforeReveal || lateManualRequest ? 24U : 16U;
    const auto available = modules::ui_color_wheel::WheelToRgb(
        static_cast<uint8_t>(modules::player_end_palette::P2_END_DEFAULT_WHEEL + 16U));
    uint32_t sampled = 0U;
    for (const uint32_t elapsed : {0U, 80U, 200U, 450U}) {
        results.Check(session.Advance(elapsed - sampled) == VIRTUAL_LITCH_OK &&
                          hintIs(firstTarget) && ActiveHintLanding(session, firstTarget,
                              static_cast<uint8_t>(firstTarget + 1U), available) &&
                          worker.IsActive() && engine.starts == 1U && engine.unsafeReads.load() == 0U,
                      "first held target keeps its full opposite rail and actual landing emitters while nonroute neighbor stays ordinary during the same search");
        sampled = elapsed;
    }
    results.Check(Observe(session, occupancy), "returning a7 reaches normal recovery to Idle");
    (void)session.Advance(300U);
    results.Check(hintIs(24U), "stable Idle adopts the actual pending depth7 result");
    Checkpoint(results, session, engine, run, "returned-to-Idle", "consider-latest-global-Hint");
    const bool subsequentReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 8U; });
    results.Check(subsequentReady && worker.IsActive(),
                  "later actual root reaches a held publication boundary before position-changing cancellation");
    if (!subsequentReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    request.request_id = 851U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    const bool dispatched = session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK;
    (void)session.Advance(1U);
    Checkpoint(results, session, engine, run, "new-game-draining", "wait-for-safe-owner-handoff");
    results.Check(dispatched &&
                      outcome.status == modules::ControlMutationStatus::Accepted &&
                      !session.TryTakeControlCompletion(outcome) && worker.IsActive(),
                  "accepted position-changing action waits for the actual worker publication owner");
    results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      session.TryTakeControlCompletion(outcome) && outcome.requestId == 851U &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      !session.TryTakeControlCompletion(outcome) && noHint() &&
                      std::strcmp(session.HomeStateSnapshot().fen, modules::INITIAL_FEN) == 0 &&
                      engine.unsafeReads.load() == 0U,
                  "one New Game completes exactly once without old-position publication or live engine reads");
    Checkpoint(results, session, engine, run, "new-game-acknowledged", "arrange-starting-pieces");
    std::printf("[HINT_FIRST_REVEAL] actual_search=1 schedule_controlled=1 physical_timing_claim=0 first_depth=5 refined_depth=7 refinement_before_reveal=%u late_manual_request=%u first_target=%u\n",
                refinementBeforeReveal, lateManualRequest, firstTarget);
}



void HintRefinementFrame(Results& results, const Session& session,
                         const GatedEngine& engine, const char* stage,
                         const char* run = "Classic/real-root/visible-Hint-refinement") {
    Checkpoint(results, session, engine, run,
               stage, "consider-one-legal-current-position-Hint");
    std::printf("[HINT_REFINEMENT_FRAME] {\"stage\":\"%s\",\"t\":%lu,\"board\":[",
                stage, static_cast<unsigned long>(session.NowMs()));
    for (uint8_t square = 0U; square < 64U; ++square) {
        std::printf("%s[", square == 0U ? "" : ",");
        for (uint8_t slot = 0U; slot < 3U; ++slot) {
            const auto c = session.SemanticFrame().board[square][slot];
            std::printf("%s[%u,%u,%u,%u]", slot == 0U ? "" : ",", c.r, c.g, c.b, c.w);
        }
        std::printf("]");
    }
    std::printf("],\"mapped_rgbw\":[");
    for (uint16_t i = 0U; i < session.PhysicalFrame().led_count; ++i) {
        const auto c = session.PhysicalFrame().pixels[i];
        std::printf("%s[%u,%u,%u,%u]", i == 0U ? "" : ",", c.r, c.g, c.b, c.w);
    }
    std::puts("]}");
}

void CheckVisibleHintRefinement(Results& results, Session& session,
                                GatedEngine& engine, uint64_t occupancy,
                                uint32_t start, uint8_t scenario) {
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = 856U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied,
                  "visible refinement starts with the actual typed manual Hint acknowledgement");
    (void)session.Advance(start + 500U - session.NowMs());
    const auto oldSource = session.SemanticFrame().board[48U][0U];
    results.Check(!CueEqual(oldSource, session.SemanticFrame().board[50U][0U]) &&
                      CueEnergy(session.SemanticFrame().board[46U][0U]) == 0U &&
                      CapturedSquareLit(session, 48U),
                  "first500ms reveal has full a2a4 guidance and ordinary unhinted g3");
    HintRefinementFrame(results, session, engine, "visible-a2a4-before");
    const bool ready = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(ready && engine.completedCandidates[6U].fromSquare == 54U &&
                      engine.completedCandidates[6U].toSquare == 46U,
                  "same owned real search completes changed-source g2g3 Full6");
    if (!ready) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    results.Check(engine.ReleaseCandidate(6U, start + 600U) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK,
                  "actual completed-root mailbox admits visible refinement at stable Idle");
    HintRefinementFrame(results, session, engine, "replacement-0ms");
    results.Check(CueEqual(session.SemanticFrame().board[48U][0U], oldSource) &&
                      CueEnergy(session.SemanticFrame().board[46U][0U]) == 0U &&
                      !CapturedSquareLit(session, 46U),
                  "visible replacement starts on departing cue instead of abruptly jumping both source and target");
    (void)session.Advance(60U);
    HintRefinementFrame(results, session, engine, "replacement-60ms");
    results.Check(!CueEqual(session.SemanticFrame().board[48U][0U], oldSource) &&
                      !CueEqual(session.SemanticFrame().board[48U][0U], session.SemanticFrame().board[50U][0U]) &&
                      CueEnergy(session.SemanticFrame().board[46U][0U]) == 0U,
                  "old guidance fades toward ordinary board before any new actionable target appears");
    if (scenario == 2U || scenario == 3U) {
        if (scenario == 2U) {
            results.Check(Observe(session, occupancy & ~(UINT64_C(1) << 54U)) &&
                              session.State().selected_square == 54U &&
                              ((session.State().valid_destination_mask >> 46U) & 1ULL) != 0ULL &&
                              !CapturedSquareLit(session, 48U),
                          "pickup during fade-out discards the old cue and immediately admits g3 for the held pawn");
            HintRefinementFrame(results, session, engine, "interrupted-held-at60ms");
            results.Check(Observe(session, occupancy) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                              !CapturedSquareLit(session, 48U) && CapturedSquareLit(session, 46U),
                          "rapid pickup-return cannot revive a discarded departing target");
            HintRefinementFrame(results, session, engine, "interrupted-return-at61ms");
            results.Check(Observe(session, occupancy & ~(UINT64_C(1) << 4U)) &&
                              !CapturedSquareLit(session, 46U),
                          "invalid enemy-king pickup immediately suppresses optional Hint presentation");
            HintRefinementFrame(results, session, engine, "invalid-pickup");
            results.Check(Observe(session, occupancy) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                              !CapturedSquareLit(session, 48U) && CapturedSquareLit(session, 46U),
                          "invalid pickup recovery restores only current guidance");
        } else {
            results.Check(Set(session, 857U, "MINIMAL_ANIMATIONS", "true") &&
                              session.Advance(1U) == VIRTUAL_LITCH_OK &&
                              !CapturedSquareLit(session, 48U) && CapturedSquareLit(session, 46U),
                          "actual typed Minimal setting shows current tuple immediately during transition");
            HintRefinementFrame(results, session, engine, "minimal-enabled-at61ms");
            results.Check(Set(session, 858U, "MINIMAL_ANIMATIONS", "false") &&
                              session.Advance(1U) == VIRTUAL_LITCH_OK &&
                              !CapturedSquareLit(session, 48U) && CapturedSquareLit(session, 46U),
                          "turning Minimal off before240ms cannot revive old guidance");
            HintRefinementFrame(results, session, engine, "minimal-disabled-at62ms");
        }
        session.RunControlIngress([](void*, modules::SettingsStore&,
                modules::ControlMutationService&, modules::GamePersistenceCoordinator& owner) {
            owner.SetBoardSettingsActive(true);
        }, nullptr);
        (void)session.Advance(1U);
        uint8_t shownFrom = 64U, shownTo = 64U;
        results.Check(session.TryGetHintMoveSquares(shownFrom, shownTo) && shownFrom == 54U && shownTo == 46U,
                      "board settings preserve the existing rendered tuple and held-target contract");
        session.RunControlIngress([](void*, modules::SettingsStore&,
                modules::ControlMutationService&, modules::GamePersistenceCoordinator& owner) {
            owner.SetBoardSettingsActive(false);
        }, nullptr);
        (void)session.Advance(1U);
        results.Check(CapturedSquareLit(session, 46U) && !CapturedSquareLit(session, 48U),
                      "menu close restores current cache immediately without a new initial window or old replay");
        HintRefinementFrame(results, session, engine, "menu-close-active-search");
        engine.CancelThink();
        results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK,
                      "real retained search quiesces without replacing the completed Hint");
        session.RunControlIngress([](void*, modules::SettingsStore&,
                modules::ControlMutationService&, modules::GamePersistenceCoordinator& owner) {
            owner.SetBoardSettingsActive(true);
            owner.SetBoardSettingsActive(false);
        }, nullptr);
        (void)session.Advance(1U);
        results.Check(CapturedSquareLit(session, 46U) && !CapturedSquareLit(session, 48U),
                      "finished-search menu close reuses completed current guidance immediately");
        HintRefinementFrame(results, session, engine, "menu-close-finished-search");
        return;
    }
    (void)session.Advance(60U);
    HintRefinementFrame(results, session, engine, "replacement-120ms");
    results.Check(CueEqual(session.SemanticFrame().board[48U][0U], session.SemanticFrame().board[50U][0U]) &&
                      CueEqual(session.SemanticFrame().board[48U][1U], session.SemanticFrame().board[50U][1U]) &&
                      CueEnergy(session.SemanticFrame().board[46U][0U]) == 0U,
                  "midpoint restores ordinary source piece illumination and never shows two move choices");
    (void)session.Advance(60U);
    const uint16_t midway = CueEnergy(session.SemanticFrame().board[46U][0U]);
    HintRefinementFrame(results, session, engine, "replacement-180ms");
    (void)session.Advance(60U);
    HintRefinementFrame(results, session, engine, "replacement-240ms");
    results.Check(midway > 0U && midway < CueEnergy(session.SemanticFrame().board[46U][0U]) &&
                      CapturedSquareLit(session, 46U),
                  "new g2g3 target fades in and reaches full semantic and mapped guidance within240ms");
    results.Check(Observe(session, occupancy & ~(UINT64_C(1) << 54U)) &&
                      session.State().selected_square == 54U &&
                      ((session.State().valid_destination_mask >> 46U) & 1ULL) != 0ULL,
                  "raw g2 pickup preserves every ordinary legal destination and the revealed target");
    HintRefinementFrame(results, session, engine, "matching-source-held");
    results.Check(Observe(session, occupancy) && session.Advance(1U) == VIRTUAL_LITCH_OK,
                  "raw source return recovers the same current-position Hint");
    HintRefinementFrame(results, session, engine, "source-returned");
    ++request.request_id;
    modules::ThinkDiagnostics replayDiagnostic;
    uint8_t restoredFrom = 64U, restoredTo = 64U;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      !session.HintDisplayRequested() && !CapturedSquareLit(session,46U) &&
                      ReadDiagnostics(session,modules::ThinkRequestKind::Hint,replayDiagnostic) && replayDiagnostic.active,
                  "manual secondHint hides the shown tuple while preserving current-position analysis");
    ++request.request_id;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      session.HintDisplayRequested() && CapturedSquareLit(session,46U) &&
                      session.TryGetHintMoveSquares(restoredFrom,restoredTo) &&
                      restoredFrom==54U && restoredTo==46U && engine.starts==1U,
                  "manual thirdHint restores the same current tuple without another search");
    HintRefinementFrame(results, session, engine, "manual-off-on-current-tuple");
    engine.CancelThink();
    results.Check(engine.Finish(session.NowMs()) && engine.unsafeReads.load() == 0U,
                  "visible refinement uses immutable results without mutable engine reads");
}



void CheckDuckHintMinimalToggleJourney(Results& results, bool resultWhileMinimal) {
    constexpr const char* PATH = "build/test_duck_hint_minimal_toggle.json";
    const char* RUN = resultWhileMinimal
        ? "Duck/real-root/result-while-Minimal-no-replay"
        : "Duck/real-root/Minimal-no-refinement-replay";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, true, false, false, false, nullptr, 0U, 0U),
                  "Duck Minimal journey restores an ongoing human position and real Duck square");
    modules::AsyncThinkWorker worker; GatedEngine engine(worker);
    engine.holdStart = 0U; engine.gateCandidates = true;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    results.Check(Observe(session, session.State().expected_occupied_mask),
                  "raw Hall scan admits Duck Hint through the shared real coordinator");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    const bool firstReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 1U; });
    results.Check(firstReady && engine.starts == 1U && worker.IsActive(),
                  "actual Duck engine completes a gated legal compound root on the existing worker");
    if (!firstReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    const auto first = engine.completedCandidates[1U];
    const uint32_t start = engine.startedAtMs + 450U;
    results.Check(first.isDuck && !first.capturedKing && first.duckDestination < 64U &&
                      engine.ReleaseCandidate(1U, start + 50U) &&
                      session.Advance(start + 50U - session.NowMs()) == VIRTUAL_LITCH_OK,
                  "real scored compound result reaches the mailbox after the500ms calculation minimum");
    virtual_litch_action_request request{}; request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION; request.request_id = 865U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT; modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied,
                  "typed manual Hint acknowledges the Duck suggestion without restarting search");
    (void)session.Advance(start + 500U - session.NowMs());
    results.Check(CapturedSquareLit(session, first.duckDestination),
                  "first500ms reveal includes semantic and actual V3 mapped Duck destination");
    HintRefinementFrame(results, session, engine, "duck-first-visible", RUN);
    modules::ThinkCandidate current;
    uint8_t selectedDepth = 0U;
    for (uint8_t depth = 2U; depth <= 5U; ++depth) {
        const bool ready = WaitFor([&] { return engine.readyCandidateDepth.load() >= depth; });
        if (!ready) break;
        current = engine.completedCandidates[depth];
        std::printf("[DUCK_MINIMAL_ROOT] depth=%u chess=%u,%u duck=%u nodes=%lu\n",
                    depth, current.fromSquare, current.toSquare, current.duckDestination,
                    static_cast<unsigned long>(current.completedNodes));
        if (current.duckDestination != first.duckDestination &&
            first.duckDestination != current.fromSquare &&
            first.duckDestination != current.toSquare) {
            selectedDepth = depth; break;
        }
        (void)engine.ReleaseCandidate(depth, session.NowMs());
    }
    results.Check(selectedDepth != 0U && current.isDuck && !current.capturedKing,
                  "same real Duck search completes a changed legal compound destination");
    if (selectedDepth == 0U) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    if (resultWhileMinimal) {
        results.Check(Set(session, 864U, "MINIMAL_ANIMATIONS", "true"),
                      "typed Minimal is already enabled when the changed compound result arrives");
    }
    results.Check(engine.ReleaseCandidate(selectedDepth, start + 600U) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK,
                  "shared Duck result pipeline applies both chess and Duck setters at stable Idle");
    uint8_t from = 64U, to = 64U;
    results.Check(session.TryGetHintMoveSquares(from, to) && from == current.fromSquare &&
                      to == current.toSquare &&
                      CapturedSquareLit(session, first.duckDestination, !resultWhileMinimal) &&
                      CapturedSquareLit(session, current.duckDestination, resultWhileMinimal),
                  "authoritative compound tuple updates with a fade only when Minimal is disabled");
    HintRefinementFrame(results, session, engine, "duck-refinement-0ms", RUN);
    if (!resultWhileMinimal) {
        (void)session.Advance(60U);
        results.Check(Set(session, 866U, "MINIMAL_ANIMATIONS", "true") &&
                          session.Advance(1U) == VIRTUAL_LITCH_OK &&
                          CapturedSquareLit(session, first.duckDestination, false) &&
                          CapturedSquareLit(session, current.duckDestination) &&
                          CapturedSquareLit(session, current.toSquare),
                      "Duck typed Minimal on immediately shows the current chess and Duck semantic/mapped cues");
    }
    HintRefinementFrame(results, session, engine, resultWhileMinimal
        ? "duck-result-while-minimal-on-at0ms" : "duck-minimal-on-at61ms", RUN);
    results.Check(Set(session, 867U, "MINIMAL_ANIMATIONS", "false") &&
                      session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      session.TryGetHintMoveSquares(from, to) &&
                      from == current.fromSquare && to == current.toSquare &&
                      session.TryGetHintDuckMoveSquares(from, to) &&
                      from == DUCK_FROM && to == current.duckDestination &&
                      CapturedSquareLit(session, first.duckDestination, false) &&
                      CapturedSquareLit(session, current.duckDestination) &&
                      CapturedSquareLit(session, current.toSquare) &&
                      session.SemanticFrame().board[current.duckDestination][0U].r >= 96U &&
                      session.SemanticFrame().board[current.duckDestination][0U].r ==
                          session.SemanticFrame().board[current.duckDestination][0U].g &&
                      session.SemanticFrame().board[current.duckDestination][0U].b == 0U,
                  "Duck typed Minimal off before240ms retains current mapped guidance and never revives departing tuple");
    HintRefinementFrame(results, session, engine, resultWhileMinimal
        ? "duck-minimal-off-at1ms" : "duck-minimal-off-at62ms", RUN);
    (void)session.Advance(240U);
    results.Check(CapturedSquareLit(session, first.duckDestination, false) &&
                      CapturedSquareLit(session, current.duckDestination) &&
                      session.TryGetHintMoveSquares(from, to) && from == current.fromSquare && to == current.toSquare,
                  "Duck recovery expires the discarded episode with current tuple/cache still present");
    HintRefinementFrame(results, session, engine, "duck-after-old-expiry", RUN);
    engine.CancelThink();
    results.Check(engine.Finish(session.NowMs()) && engine.starts == 1U && engine.unsafeReads.load() == 0U,
                  "Duck Minimal toggle never erases cache, restarts search or reads the mutable active engine");
    std::printf("[DUCK_MINIMAL_JOURNEY_SEAMS] actual_search_worker_mailbox_GPC=1 raw_Hall=1 typed_settings=1 "
                "paired_chess_Duck_setters=1 semantic_mapped_V3=1 schedule_controlled=1 physical_timing_claim=0 "
                "first=%u,%u,%u current=%u,%u,%u depth=%u\n", first.fromSquare, first.toSquare,
                first.duckDestination, current.fromSquare, current.toSquare, current.duckDestination, selectedDepth);
}

void CheckHintRefinementBoundaries(Results& results) {
    using namespace modules::led_frame_composer::internal;
    // Supporting render-owner test: controlled legal tuples, no claimed engine
    // result. The named Session journey above owns actual search/physical proof.
    s_nowMs.store(UINT32_MAX - 100U);
    runtime::GameStateMachine machine;
    runtime::ChessPositionState position;
    position.occupiedMask = (UINT64_C(1) << 48U) | (UINT64_C(1) << 54U) |
        (UINT64_C(1) << 52U) | (UINT64_C(1) << 28U) | (UINT64_C(1) << 36U) |
        (UINT64_C(1) << 24U) | (UINT64_C(1) << 27U) | (UINT64_C(1) << 29U);
    position.moverOccupiedMask = (UINT64_C(1) << 48U) | (UINT64_C(1) << 54U) | (UINT64_C(1) << 52U) | (UINT64_C(1) << 28U) | (UINT64_C(1) << 36U);
    position.opponentOccupiedMask = (UINT64_C(1) << 27U) | (UINT64_C(1) << 29U);
    position.pawnMask = position.moverOccupiedMask | position.opponentOccupiedMask;
    position.enPassantTargetSquare = 21U;
    position.enPassantCaptureSquare = 29U;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    machine.SetHintDisplayRequested(true);
    machine.SetHintMoveSquares(48U, 32U);
    machine.SetHintDuckMoveSquares(24U, 25U);
    machine.SetHintMoveSquares(54U, 46U);
    machine.SetHintDuckMoveSquares(24U, 26U);
    modules::SettingsStoreConfig config; config.path = "build/test_refinement_render_settings.json";
    RemoveFiles(config.path);
    modules::SettingsStore settings(config); (void)settings.Load();
    modules::EngineDerivedState derived;
    runtime::InteractionModelSnapshot snapshot;
    modules::led_frame_composer::ComposeInputs in;
    const auto compose = [&] {
        snapshot = runtime::BuildInteractionModelSnapshot(
            {.stateMachine = &machine, .derived = &derived, .variant = modules::GameVariant::Duck,
             .duckSquare = 24U});
        in.settings = &settings; in.interactionSnapshot = &snapshot;
        ComposeContext ctx;
        results.Check(BuildComposeContext(in, ctx), "real immutable snapshot builds refinement compose context");
        return ctx;
    };
    s_nowMs.fetch_add(180U);
    machine.Tick();
    auto ctx = compose();
    const auto renderedTarget = [&](const ComposeContext& context, const uint8_t square) {
        modules::Rgb color = modules::RGB_BLACK;
        BoardSquarePromptState prompt;
        ApplyHintMovePrompt(context, BuildBoardComposeRuntime(context), square, color, prompt);
        return color;
    };
    const auto halfVisible = renderedTarget(ctx, 46U);
    const auto latched = machine.GetRenderState().hintRefinement.incoming;
    const auto began = machine.GetRenderState().hintRefinement.elapsed.StartMs();
    const auto feedbackBegan = machine.GetRenderState().hintFeedbackElapsed.StartMs();
    const uint8_t alpha = ctx.hintRefinementAlpha255;
    machine.SetHintMoveSquares(52U, 44U);
    ctx = compose();
    results.Check(ctx.hintFromSquare == 54U && ctx.hintToSquare == 46U &&
                      ctx.hintRefinementAlpha255 == alpha &&
                      machine.GetRenderState().hintRefinement.incoming == latched &&
                      machine.GetRenderState().hintRefinement.elapsed.StartMs() == began &&
                      machine.GetRenderState().hintFeedbackElapsed.StartMs() == feedbackBegan,
                  "changed tuple at180ms cannot snap the half-visible move or restart either presentation clock");
    const auto retained = renderedTarget(ctx, 46U);
    const auto queued = renderedTarget(ctx, 44U);
    results.Check(retained.r == halfVisible.r && retained.g == halfVisible.g &&
                      retained.b == halfVisible.b && queued.r == 0U && queued.g == 0U && queued.b == 0U,
                  "observable half-visible target RGB stays unchanged and queued destination stays ordinary");
    std::printf("[HINT_REFINEMENT_RGB] age=180 target46=%u,%u,%u queued44=%u,%u,%u\n",
                retained.r, retained.g, retained.b, queued.r, queued.g, queued.b);
    std::printf("[HINT_REFINEMENT_BOUNDARY] stage=rapid-at180ms wrap=1 shown=%u-%u latest=52-44 alpha=%u start=%lu\n",
                ctx.hintFromSquare, ctx.hintToSquare, alpha, static_cast<unsigned long>(began));
    s_nowMs.fetch_add(60U); machine.Tick(); ctx = compose();
    results.Check(ctx.hintFromSquare == 54U && ctx.hintToSquare == 46U && ctx.hintRefinementAlpha255 == 255U,
                  "initial240ms episode ends on its fully visible latched tuple before the shorter handoff");
    s_nowMs.fetch_add(60U); machine.Tick(); ctx = compose();
    results.Check(ctx.hintFromSquare == 52U && ctx.hintToSquare == 44U && ctx.hintRefinementAlpha255 == 0U,
                  "queued rapid update crosses ordinary board in the120ms follow-up");
    s_nowMs.fetch_add(60U); machine.Tick(); ctx = compose();
    results.Check(ctx.hintFromSquare == 52U && ctx.hintToSquare == 44U && ctx.hintRefinementAlpha255 == 255U &&
                      !machine.GetRenderState().hintRefinement.elapsed.Started(),
                  "queued latest tuple is fully visible180ms after its update across millis wrap");
    machine.SetHintMoveSquares(52U, 44U);
    results.Check(!machine.GetRenderState().hintRefinement.elapsed.Started(),
                  "same tuple greater-depth publication cannot restart visual refinement");
    machine.SetHintMoveSquares(28U, 21U); // EP's empty landing differs from its victim29.
    s_nowMs.fetch_add(180U); machine.Tick(); ctx = compose();
    results.Check(ctx.hintCaptureSquare == 29U && ctx.hintToSquare == 21U,
                  "immutable EP tuple preserves the off-destination victim during fade-in");
    BoardComposeRuntime boardRuntime = BuildBoardComposeRuntime(ctx);
    boardRuntime.illegalAnim.elapsedMs = 0U;
    modules::Rgb victim = modules::RGB_BLACK;
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 29U, victim);
    auto fullContext=ctx;fullContext.hintRefinementAlpha255=255U;
    modules::Rgb fullWhite=modules::RGB_BLACK;
    ApplyPhysicalRemovalPrompt(fullContext,boardRuntime,false,29U,fullWhite);
    results.Check(victim.r>0U && victim.r<fullWhite.r && victim.r==victim.g && victim.g==victim.b,
                  "Hint-only EP WHITE removal retains the tuple's partial fade instead of appearing full strength");
    s_nowMs.fetch_add(60U); machine.Tick();
    machine.SetHintMoveSquares(36U, 27U); // e4xd5 replaces e5xf6 EP on the same board.
    s_nowMs.fetch_add(60U); machine.Tick(); ctx = compose();
    boardRuntime = BuildBoardComposeRuntime(ctx); boardRuntime.illegalAnim.elapsedMs = 0U;
    modules::Rgb oldVictim = modules::RGB_BLACK, newVictim = modules::RGB_BLACK;
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 29U, oldVictim);
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 27U, newVictim);
    results.Check(ctx.hintCaptureSquare == 29U && oldVictim.r > 0U && oldVictim.r < fullWhite.r &&
                      oldVictim.r==oldVictim.g && oldVictim.g==oldVictim.b &&
                      newVictim.r == 0U && newVictim.g == 0U && newVictim.b == 0U,
                  "departing EP WHITE fades out without prematurely showing the new capture victim");
    s_nowMs.fetch_add(120U); machine.Tick(); ctx = compose();
    boardRuntime = BuildBoardComposeRuntime(ctx); boardRuntime.illegalAnim.elapsedMs = 0U;
    oldVictim = newVictim = modules::RGB_BLACK;
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 29U, oldVictim);
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 27U, newVictim);
    results.Check(ctx.hintCaptureSquare == 27U && oldVictim.r == 0U && newVictim.r > 0U && newVictim.r < fullWhite.r &&
                      newVictim.r==newVictim.g && newVictim.g==newVictim.b,
                  "only incoming ordinary capture victim fades in after the EP cue retires");
    ctx.hintRefinementAlpha255 = 0U;
    ctx.cpuTargetOccValid = true;
    snapshot.interaction.taggedState.plan.removalRequired = true;
    snapshot.interaction.taggedState.plan.removalSquare = 29U;
    modules::Rgb required = modules::RGB_BLACK;
    ApplyPhysicalRemovalPrompt(ctx, boardRuntime, false, 29U, required);
    results.Check(required.r==fullWhite.r && required.g==fullWhite.g && required.b==fullWhite.b,
                  "required active-plan removal is never faded by optional Hint presentation");
    runtime::GameStateMachineRenderState state;
    runtime::SetHintDisplayRequested(state, true); runtime::SetHintMoveSquares(state, 48U, 27U);
    runtime::SetHintMoveSquares(state, 54U, 46U);
    runtime::ClearHintMove(state);
    results.Check(!state.hintRefinement.elapsed.Started() && !state.hintRefinementReady && !state.hintMoveValid,
                  "position clear retires tuple, transition and resume marker together");
    RemoveFiles(config.path);
    std::printf("[HINT_REFINEMENT_RESOURCES] state_bytes=%zu tuple_bytes=%zu snapshot_render_bytes=%zu max_initial_ms=240 max_followup_ms=120 allocation_per_frame=0\n",
                sizeof(runtime::HintRefinementPresentation), sizeof(runtime::HintPresentationTuple), sizeof(runtime::RenderStateView));
}

void CheckHiddenHintSourceChangeJourney(Results& results, uint8_t visibleRefinement = 0U) {
    constexpr const char* PATH = "build/test_hint_source_change.json";
    constexpr const char* FEN =
        "r2qk2r/pb2p2p/1ppb2p1/8/3P4/1B3P2/PBP3PP/R2Q1RK1 w kq - 1 16";
    constexpr const char* RUN = "Classic/real-root/hidden-Hint-source-change";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, true, false, false, FEN, 0U, 0U),
                  "source-changing refinement restores a retained human corpus position");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = 0U;
    engine.gateCandidates = true;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = visibleRefinement ? VIRTUAL_LITCH_HARDWARE_V3
                                        : VIRTUAL_LITCH_HARDWARE_S3;
    config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    const uint64_t occupancy = session.State().expected_occupied_mask;
    results.Check(Observe(session, occupancy), "raw Hall scan admits the source-change human turn");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    const bool firstReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
    results.Check(firstReady && engine.starts == 1U && worker.IsActive(),
                  "source-change journey completes the first actual root on its owned worker");
    if (!firstReady) {
        Checkpoint(results, session, engine, RUN, "first-root-unavailable", "inspect-search-admission");
        engine.CancelThink();
        (void)engine.Finish(session.NowMs());
        return;
    }
    const uint32_t start = engine.startedAtMs + 450U;
    const auto hintIs = [&](uint8_t expectedFrom, uint8_t expectedTo) {
        uint8_t from = 64U, to = 64U;
        return session.TryGetHintMoveSquares(from, to) &&
               from == expectedFrom && to == expectedTo;
    };
    const auto noHint = [&] {
        uint8_t from = 64U, to = 64U;
        return !session.TryGetHintMoveSquares(from, to);
    };
    results.Check(engine.completedCandidates[5U].fromSquare == 48U &&
                      engine.completedCandidates[5U].toSquare == 32U &&
                      engine.ReleaseCandidate(5U, start + 50U) &&
                      session.Advance(start + 50U - session.NowMs()) == VIRTUAL_LITCH_OK &&
                      hintIs(48U, 32U) && !session.HintDisplayRequested(),
                  "actual a2a4 depth5 result is prepared at500ms without requesting display");
    if (visibleRefinement) {
        CheckVisibleHintRefinement(results, session, engine, occupancy, start, visibleRefinement);
        return;
    }
    (void)session.Advance(50U);
    results.Check(Observe(session, occupancy & ~(UINT64_C(1) << 48U)),
                  "raw a2 pickup keeps its ordinary legal guidance while Hint is off");
    (void)session.Advance(start + 500U - session.NowMs());
    results.Check(!session.HintDisplayRequested() && hintIs(48U, 32U),
                  "the unrequested a2a4 target is prepared during pickup");
    const bool refinedReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(refinedReady && engine.completedCandidates[6U].fromSquare == 54U &&
                      engine.completedCandidates[6U].toSquare == 46U,
                  "actual depth6 root changes the global Hint source to g2g3");
    if (!refinedReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    results.Check(engine.ReleaseCandidate(6U, start + 600U) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK && noHint() &&
                      session.State().selected_square == 48U &&
                      ((session.State().valid_destination_mask >> 32U) & 1ULL) != 0ULL,
                  "unseen old target retires when refinement changes source without losing legal guidance");
    Checkpoint(results, session, engine, RUN, "hidden-source-refined", "choose-legal-a2-destination");
    (void)session.Advance(100U);
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = 852U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      session.HintDisplayRequested() && noHint(),
                  "first typed request cannot reveal or freeze the superseded hidden source");
    StaticJsonDocument<1536> home;
    results.Check(ReadHome(session, home) &&
                      std::strstr(home["as"] | "", "a2a4") == nullptr,
                  "Home and board both omit the superseded held-piece target");
    Checkpoint(results, session, engine, RUN, "first-request-other-source-held", "return-to-Idle-for-global-Hint");
    results.Check(Observe(session, occupancy), "returning a2 restores the full board");
    (void)session.Advance(300U);
    results.Check(hintIs(54U, 46U) && ReadHome(session, home) &&
                      std::strcmp(home["as"] | "", "3rg2g3") == 0 &&
                      session.HomeStateSnapshot().assistance.presentation == modules::HomeAssistancePresentation::HintRefreshing,
                  "stable Idle publishes retained g2g3 consistently to board and Home while analysis continues");
    Checkpoint(results, session, engine, RUN, "returned-to-Idle", "consider-g2g3");
    engine.CancelThink();
    results.Check(engine.Finish(session.NowMs()) && engine.unsafeReads.load() == 0U,
                  "source-change journey quiesces real analysis without reading the mutable worker");
    std::printf("[HINT_SOURCE_CHANGE] actual_search=1 schedule_controlled=1 physical_timing_claim=0 first=a2a4 refined=g2g3\n");
}


void CheckCapturedKnightToCpuJourney(Results& results) {
    constexpr const char* PATH = "build/test_captured_knight_cpu.json";
    constexpr const char* BEFORE_CAPTURE =
        "rn1qkb1r/pp1bpp2/3p1n1p/3p2p1/4P3/2NB1N1P/PPP2PP1/R1BQ1RK1 w kq - 0 1";
    constexpr const char* AFTER_CAPTURE =
        "rn1qkb1r/pp1bpp2/3p1n1p/3N2p1/4P3/3B1N1P/PPP2PP1/R1BQ1RK1 b kq - 0 1";
    constexpr const char* RUN = "Classic/captured-V3/Hint-c3xd5-CPU4-reply";
    constexpr uint64_t BEFORE = UINT64_C(0x6D67AC1048A83BBB);
    constexpr uint64_t LIFT = UINT64_C(0x6D67A81048A83BBB);
    constexpr uint64_t REMOVE_CAPTURE = UINT64_C(0x6D67A81040A83BBB);
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, true, false, false, BEFORE_CAPTURE, 0U, 4U),
                  "captured V3 journey restores White move with BlackCPU4 seat2");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = 2U;
    engine.gateCandidates = true;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,
                    PATH, &engine, &worker);
    results.Check(Set(session, 869U, "HINT_THINKING_STYLE", "\"foxie_sweep\""),
                  "legacy captured corner-retirement journey explicitly selects Foxie before admission");
    virtual_litch_action_request coaching{};
    coaching.struct_size = sizeof(coaching);
    coaching.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    coaching.request_id = 870U;
    coaching.action = static_cast<uint8_t>(modules::ControlAction::Coaching);
    coaching.owner = static_cast<uint8_t>(modules::UiOwner::P1);
    coaching.coaching_mutation = static_cast<uint8_t>(modules::CoachingMutation::Enable);
    modules::ControlMutationOutcome coachingOutcome;
    results.Check(session.DispatchControlAction(coaching, coachingOutcome) == VIRTUAL_LITCH_OK &&
                      coachingOutcome.status == modules::ControlMutationStatus::Applied,
                  "typed production action restores witnessed White Coaching state");
    results.Check(Observe(session, BEFORE), "captured full raw Hall occupancy matches saved FEN");
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool hintReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
    results.Check(hintReady && engine.continuousHint && engine.starts == 1U,
                  "human turn starts the real Full5 Hint before the captured gesture");
    if (!hintReady) { engine.CancelThink(); (void)engine.Finish(session.NowMs()); return; }
    const uint32_t hintAt = engine.startedAtMs;
    results.Check(engine.ReleaseCandidate(5U, hintAt + 500U) &&
                      session.Advance(hintAt + 500U - session.NowMs()) == VIRTUAL_LITCH_OK,
                  "completed real Full5 candidate reaches the normal500ms reveal gate");
    StaticJsonDocument<1536> hintHome;
    uint8_t hintFrom = 64U, hintTo = 64U;
    results.Check(ReadHome(session, hintHome) && hintHome["co"] == 1U &&
                      session.TryGetHintMoveSquares(hintFrom, hintTo) &&
                      hintFrom == 42U && hintTo == 27U,
                  "witnessed Coaching exposes actual Full5 c3d5 consistently before pickup");
    results.Check(session.PhysicalFrame().led_count == 288U && !session.HallRotate() &&
                      CapturedSquareLit(session, 42U) &&
                      CapturedHintSourceCadence(session, 42U) &&
                      CapturedCornersWhite(session),
                  "current c3d5 Hint exposes White source and neutral corners through actual V3RGBW mapping");
    Checkpoint(results, session, engine, RUN, "human-Hint-ready", "lift-white-knight-c3");
    uint16_t sourceLow = UINT16_MAX, sourceHigh = 0U;
    bool captureVisible = true; uint16_t captureLow = UINT16_MAX, captureHigh = 0U;
    for (uint8_t frame = 0U; frame <= 9U; ++frame) {
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, 42U, hardware::SquareLedSlot::Piece, false);
        const uint16_t energy = CueEnergy(session.PhysicalFrame().pixels[targets.indices[0U]]);
        sourceLow = std::min(sourceLow, energy); sourceHigh = std::max(sourceHigh, energy);
        const bool removal = CapturedRemovalCue(session, 27U);
        captureVisible = captureVisible && removal && CapturedSlotLit(session,27U,hardware::SquareLedSlot::Piece,false);
        const uint16_t intensity = session.SemanticFrame().board[27U][0U].r;
        captureLow = std::min(captureLow,intensity); captureHigh = std::max(captureHigh,intensity);
        results.Check(session.TryGetHintMoveSquares(hintFrom, hintTo) &&
                          hintFrom == 42U && hintTo == 27U && CapturedSquareLit(session, 42U) &&
                          CapturedHintSourceCadence(session, 42U),
                      "CPU source blink and capture-removal cadence retain exact current Hint authority");
        CapturedCueCheckpoint(session, RUN, removal ? "c3-source-d5-removal-on" : "c3-source-d5-removal-off");
        if (frame != 9U) (void)session.Advance(100U);
    }
    results.Check(sourceLow == 0U && sourceHigh > 0U && captureVisible && captureHigh > captureLow,
                  "White Hint source fully blinks while Black d5 Piece LEDs stay zero and WHITE brackets crossfade through zero/full");
    results.Check(Observe(session, LIFT) && session.State().selected_square == 42U,
                  "captured raw Hall prefix lifts the actual White knight atc3");
    results.Check((session.State().physical_occupied_mask & (UINT64_C(1) << 42U)) == 0ULL &&
                      CapturedSquareLit(session, 42U),
                  "held c3 source remains physically empty with side guidance while the directional head leaves it");
    // The existing capture-draining checkpoint below checks the empty source's
    // Piece pair is dark after the permitted route head has retired.
    Checkpoint(results, session, engine, RUN, "c3-lift", "remove-black-pawn-d5");
    results.Check(Observe(session, REMOVE_CAPTURE) &&
                      CapturedSlotLit(session, 27U, hardware::SquareLedSlot::Piece) &&
                      !CapturedRemovalCue(session, 27U),
                  "captured pawn removal retires WHITE removal and exposes both actual d5 landing emitters");
    Checkpoint(results, session, engine, RUN, "d5-victim-removed", "place-held-white-knight-d5");
    results.Check(Observe(session, LIFT) &&
                      CapturedSlotLit(session, 42U, hardware::SquareLedSlot::Piece, false) &&
                      CapturedSlotLit(session, 27U, hardware::SquareLedSlot::Piece),
                  "captured raw Hall placement moves the visible White identity from c3to d5");
    const bool heldRefinement = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(heldRefinement && worker.IsActive() && engine.starts == 1U,
                  "real old-position refinement remains held through the physical capture");
    const uint32_t cancelsBeforeCommit = engine.cancels.load();
    StaticJsonDocument<1536> home;
    results.Check(session.Advance(2600U) == VIRTUAL_LITCH_OK &&
                      engine.cancels.load() > cancelsBeforeCommit && worker.IsActive() &&
                      ReadHome(session, home) && std::strcmp(home["fen"] | "", BEFORE_CAPTURE) == 0 &&
                      session.UndoHistoryMoveCount() == 0U && engine.starts == 1U &&
                      !session.CpuThinkingVisible() && engine.unsafeReads.load() == 0U,
                  "production capture commit requests cancellation and preserves unclaimed move while Hint cleanup drains");
    results.Check(session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::AwaitAcknowledgement) &&
                      CapturedSlotLit(session, 42U, hardware::SquareLedSlot::Piece, false) &&
                      !CapturedRemovalCue(session, 27U) && UniformCapturedCorners(session),
                  "capture draining awaits acknowledgement and suppresses retired c3d5 capture and traveling Hint feedback");
    Checkpoint(results, session, engine, RUN, "capture-cancel-draining", "wait-for-safe-capture-commit");
    results.Check(session.Advance(100U) == VIRTUAL_LITCH_OK && worker.IsActive() &&
                      ReadHome(session, home) && std::strcmp(home["fen"] | "", BEFORE_CAPTURE) == 0 &&
                      session.UndoHistoryMoveCount() == 0U && engine.starts == 1U &&
                      engine.unsafeReads.load() == 0U,
                  "repeated capture-commit tick cannot mutate or admit CPU while old publication is held");
    engine.holdStart = UINT32_MAX;
    results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK,
                  "production-canceled Hint releases its gate before the retryable capture is claimed");
    results.Check(ReadHome(session, home) && std::strcmp(home["fen"] | "", AFTER_CAPTURE) == 0 &&
                      session.UndoHistoryMoveCount() == 1U &&
                      !session.TryGetHintMoveSquares(hintFrom, hintTo),
                  "authoritative coordinator claims the captured move exactly once and clears stale Hint output");
    Checkpoint(results, session, engine, RUN, "human-capture-committed", "wait-for-black-CPU");
    // Only the first Hint candidate publication was held. CPU scheduling stays
    // controlled at Poll, with the real profile and actual search unchanged.
    for (uint8_t i = 0U; i < 30U && engine.starts < 2U; ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.starts == 2U && engine.pollEntered.load(); });
    results.Check(admitted && session.UndoHistoryMoveCount() == 1U &&
                      worker.IsActive() && !engine.continuousHint &&
                      session.CpuThinkingVisible() && ReadHome(session, home) &&
                      std::strcmp(home["th"][0] | "", "b") == 0,
                  "same human gesture automatically admits CPU4 and publishes Black thinking");
    Checkpoint(results, session, engine, RUN, "CPU-admitted", "wait-for-analyzed-reply");
    results.Check(engine.Finish(session.NowMs()), "captured CPU4 real search and stats finish");
    (void)session.Advance(1U);
    for (uint8_t i = 0U; i < 30U; ++i) {
        if (ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0) break;
        (void)session.Advance(100U);
    }
    results.Check(ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0 &&
                      home["c"].is<JsonArray>() && home["th"].isNull() &&
                      !session.CpuThinkingVisible() && engine.unsafeReads.load() == 0U,
                  "analyzed actual reply replaces CPU thinking with the physical move prompt");
    results.Check(session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) &&
                      CapturedSquareLit(session, 12U) && CapturedSquareLit(session, 20U) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece),
                  "authoritative e7e6 CPU prompt lights source sides and both actual empty landing emitters");
    Checkpoint(results, session, engine, RUN, "reply-prompt", "lift-black-pawn-e7-then-place-e6");
    bool cpuOn = false, cpuOff = false;
    for (uint8_t frame = 0U; frame <= 8U; ++frame) {
        const bool on = session.NowMs() % 800U < 600U;
        cpuOn = cpuOn || on; cpuOff = cpuOff || !on;
        results.Check(CapturedSlotLit(session, 12U, hardware::SquareLedSlot::Piece, on) &&
                          (!on || CapturedSamePieceWithinTravelFloor(session, 12U, 11U)) &&
                          CapturedSquareLit(session, 12U) && CapturedSquareLit(session, 20U) &&
                          CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece) &&
                          ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0 &&
                          std::strcmp(home["c"][0U] | "", "e7") == 0 &&
                          std::strcmp(home["c"][1U] | "", "e6") == 0,
                      "CPU source independently obeys600ms on200ms off with Black identity and both e6 landing emitters visible");
        CapturedCueCheckpoint(session, RUN, on ? "e7-CPU-piece-on-e6-target" : "e7-CPU-piece-off-e6-target");
        if (frame != 8U) (void)session.Advance(100U);
    }
    results.Check(cpuOn && cpuOff, "CPU next-action playback samples both complete source blink states");
    modules::UciMove cpuMove;
    litch_max::ParsedUciMove parsed;
    results.Check(cpuMove.Format("%s%s", home["c"][0] | "", home["c"][1] | "") &&
                      cpuMove.Equals("e7e6") && litch_max::TryParseUciMove(cpuMove.CStr(), parsed),
                  "captured board journey selects the exact observed e7e6 reply");
    engine.holdStart = 3U;
    engine.readyCandidateDepth.store(0U);
    engine.publishedCandidateDepth.store(0U);
    engine.allowedCandidateDepth.store(0U);
    engine.releaseAllCandidates.store(false);
    uint64_t occupancy = LIFT;
    occupancy &= ~(UINT64_C(1) << parsed.fromSquare);
    results.Check(Observe(session, occupancy) &&
                      session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary) &&
                      CapturedSlotLit(session, 12U, hardware::SquareLedSlot::Piece, false) &&
                      CapturedSquareLit(session, 20U),
                  "CPU e7 lift immediately retires its piece marker and asks to place on e6");
    Checkpoint(results, session, engine, RUN, "CPU-e7-held", "place-black-pawn-e6");
    occupancy |= UINT64_C(1) << parsed.toSquare;
    results.Check(Observe(session, occupancy) && session.Advance(2600U) == VIRTUAL_LITCH_OK &&
                      occupancy == UINT64_C(0x6D67A81048B82BBB),
                  "real Hall lift and placement acknowledge the CPU e7e6 move");
    results.Check(ReadHome(session, home) && std::strcmp(home["fen"] | "",
                      "rn1qkb1r/pp1b1p2/3ppn1p/3N2p1/4P3/3B1N1P/PPP2PP1/R1BQ1RK1 w kq - 0 2") == 0,
                  "Home shows White to play move two after Black's e7e6");
    results.Check(session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::None) &&
                      CapturedSlotLit(session, 12U, hardware::SquareLedSlot::Piece, false) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece) &&
                      !session.TryGetHintMoveSquares(hintFrom, hintTo) &&
                      home["c"].isNull() && home["th"].isNull() && UniformCapturedCorners(session),
                  "physical e6 acknowledgement retires CPU prompt and stale Hint while retaining the placed Black marker");
    Checkpoint(results, session, engine, RUN, "CPU-physically-acknowledged", "wait-for-settle-then-request-White-Hint");
    coaching.request_id = 871U;
    coaching.coaching_mutation = static_cast<uint8_t>(modules::CoachingMutation::Disable);
    results.Check(session.DispatchControlAction(coaching, coachingOutcome) == VIRTUAL_LITCH_OK &&
                      coachingOutcome.status == modules::ControlMutationStatus::Applied,
                  "typed Coaching disable preserves the actual next-position analysis context");
    virtual_litch_action_request request{};
    request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id = 872U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      ReadHome(session, home) && std::strcmp(home["as"] | "", "1x") == 0 &&
                      home["co"] == 0U && home["th"].isNull(),
                  "Hint request during physical settling is retained with optional feedback suppressed");
    Checkpoint(results, session, engine, RUN, "human-Hint-requested-during-settle", "wait-for-stable-human-turn");
    engine.rejectNextHintStart = true;
    for (uint8_t i = 0U; i < 30U; ++i) {
        (void)session.Advance(100U);
        if (ReadHome(session, home) && std::strcmp(home["as"] | "", "3p") == 0) break;
    }
    results.Check(ReadHome(session, home) && std::strcmp(home["as"] | "", "3p") == 0 &&
                      !worker.IsActive() && engine.starts == 2U && home["th"].isNull() &&
                      !session.CpuThinkingVisible(),
                  "eligible requested Hint reports pending after controlled admission failure, not calculating");
    results.Check(!CapturedCornersWhite(session), "requested pending Hint no longer replaces ordinary Turn with neutral-white corner bounce");
    Checkpoint(results, session, engine, RUN, "human-Hint-requested-pending", "wait-for-Hint-retry");
    results.Check(session.Advance(100U) == VIRTUAL_LITCH_OK && !CapturedCornersWhite(session) &&
                      !session.TryGetHintMoveSquares(hintFrom, hintTo) &&
                      ReadHome(session, home) && std::strcmp(home["as"] | "", "3p") == 0,
                  "retired pending corner bounce stays absent while actual failed Hint admission remains pending");
    Checkpoint(results, session, engine, RUN, "human-Hint-pending-corner-cadence", "wait-for-Hint-retry");
    // Same-position retries retain the first admitted attempt's reveal window.
    const uint32_t nextWindowStarted = session.NowMs() - 100U;
    for (uint8_t i = 0U; i < 30U && engine.starts < 3U; ++i) (void)session.Advance(100U);
    const bool nextAdmitted = WaitFor([&] { return engine.starts == 3U && engine.pollEntered.load(); });
    results.Check(nextAdmitted && worker.IsActive() && engine.continuousHint &&
                      ReadHome(session, home) && std::strcmp(home["as"] | "", "3t") == 0 &&
                      home["th"].isNull() && !session.CpuThinkingVisible(),
                  "same requested humanHint progresses to true thinking on its own owner");
    const uint32_t nextStarted = engine.startedAtMs;
    results.Check(engine.PublishSample(nextStarted + 100U, 128U) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK &&
                      worker.PublishedNodeCount() == 128U,
                  "actual worker mailbox reports new-position progress without stale CPUcounts");
    Checkpoint(results, session, engine, RUN, "human-Hint-thinking", "wait-for-completed-Full5");
    engine.releasePoll.store(true);
    const bool nextFull5 = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
    results.Check(nextFull5 && engine.ReleaseCandidate(5U, nextWindowStarted + 500U) &&
                      session.Advance(nextWindowStarted + 500U - session.NowMs()) == VIRTUAL_LITCH_OK,
                  "real next-position Full5 root completes before controlled first-reveal boundary");
    uint8_t from = 64U, to = 64U;
    results.Check(session.TryGetHintMoveSquares(from,to)&&from==engine.completedCandidates[5U].fromSquare&&
                      to==engine.completedCandidates[5U].toSquare,
                  "accepted new-position manual O reveals only its genuine current Full5 at the500ms calculation boundary");
    (void)session.Advance(1U);
    results.Check(session.TryGetHintMoveSquares(from, to) && ReadHome(session, home) &&
                      std::strncmp(home["as"] | "", "3r", 2U) == 0 &&
                      session.HomeStateSnapshot().assistance.presentation == modules::HomeAssistancePresentation::HintRefreshing &&
                      from == engine.completedCandidates[5U].fromSquare &&
                      to == engine.completedCandidates[5U].toSquare &&
                      worker.IsActive() && engine.starts == 3U,
                  "selected completed Full5 remains current-White guidance while the same search refreshes it");
    results.Check(from == 27U && to == 21U && CapturedSquareLit(session, 27U) &&
                      CapturedHintSourceCadence(session, 27U) &&
                      CapturedCornersWhite(session),
                  "real current WhiteFull5d5f6 exposes its White source and neutral ready acknowledgement");
    Checkpoint(results, session, engine, RUN, "human-Hint-ready-refining", "lift-white-knight-d5-and-remove-black-knight-f6");
    const auto readyCorners = CapturedCorners(session);
    const uint32_t readyCueAt = session.NowMs();
    uint16_t hintLow = UINT16_MAX, hintHigh = 0U;
    bool hintRemovalVisible = true; uint16_t hintRemovalLow = UINT16_MAX, hintRemovalHigh = 0U;
    for (uint8_t frame = 0U; frame <= 9U; ++frame) {
        const auto targets = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V3S3, 27U, hardware::SquareLedSlot::Piece, false);
        const uint16_t energy = CueEnergy(session.PhysicalFrame().pixels[targets.indices[0U]]);
        hintLow = std::min(hintLow, energy); hintHigh = std::max(hintHigh, energy);
        const bool removal = CapturedRemovalCue(session, 21U);
        hintRemovalVisible = hintRemovalVisible && removal && CapturedSlotLit(session,21U,hardware::SquareLedSlot::Piece,false);
        const uint16_t intensity = session.SemanticFrame().board[21U][0U].r;
        hintRemovalLow = std::min(hintRemovalLow,intensity); hintRemovalHigh = std::max(hintRemovalHigh,intensity);
        results.Check(session.TryGetHintMoveSquares(from, to) && from == 27U && to == 21U &&
                          CapturedSquareLit(session, 27U) &&
                          CapturedHintSourceCadence(session, 27U),
                      "same-position refining Hint preserves d5source/f6victim across its visible cadence");
        CapturedCueCheckpoint(session, RUN, removal ? "d5-source-f6-removal-on" : "d5-source-f6-removal-off");
        if (frame != 9U) (void)session.Advance(100U);
    }
    results.Check(hintLow == 0U && hintHigh > 0U && hintRemovalVisible && hintRemovalHigh > hintRemovalLow &&
                      !SameCapturedCorners(readyCorners, CapturedCorners(session)),
                  "real Hint keeps the CPU source blink, dark victim Piece pair, breathing removal brackets and bounded corner acknowledgement");
    const auto checkRetiredHintCorners = [&] {
        constexpr auto topology = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
        for (const auto index : modules::PlayerCornerLedIndices(modules::UiOwner::P1)) {
            const auto semantic = session.SemanticFrame().status[index];
            const auto mapped = session.PhysicalFrame().pixels[topology.physicalBoardLedCount + index];
            // Ordinary corners are off in this fixed captured fixture. Checking
            // each actual pixel rejects even one stuck neutral acknowledgement;
            // equality of two late frames cannot establish that retirement.
            results.Check(CueEnergy(semantic) == 0U,
                          "each semantic P1 corner returns to ordinary off after Hint acknowledgement");
            results.Check(CueEnergy(mapped) == 0U && mapped.w == 0U,
                          "each mapped V3RGBW P1 corner retires all Hint light including the white channel");
        }
    };
    results.Check(session.Advance(300U) == VIRTUAL_LITCH_OK &&
                      session.NowMs() - readyCueAt == 1200U &&
                      session.TryGetHintMoveSquares(from, to) && from == 27U && to == 21U,
                  "exact1200ms acknowledgement deadline retains the current completed d5f6 Hint");
    checkRetiredHintCorners();
    Checkpoint(results, session, engine, RUN, "human-Hint-corners-retired-1200ms", "consider-current-White-capture");
    const auto expiredCorners = CapturedCorners(session);
    results.Check(session.Advance(200U) == VIRTUAL_LITCH_OK &&
                      session.NowMs() - readyCueAt == 1400U &&
                      SameCapturedCorners(expiredCorners, CapturedCorners(session)) &&
                      session.TryGetHintMoveSquares(from, to) && from == 27U && to == 21U,
                  "exact1400ms corner retirement stays stable while the completed d5f6 Hint remains available");
    checkRetiredHintCorners();
    Checkpoint(results, session, engine, RUN, "human-Hint-corners-retired", "consider-current-White-capture");
    const bool refining = WaitFor([&] { return engine.readyCandidateDepth.load() >= 6U; });
    results.Check(refining, "same new-position search reaches a real deeper refinement");
    const uint32_t cancelsBeforeNewGame = engine.cancels.load();
    request.request_id = 873U;
    request.action = VIRTUAL_LITCH_CONTROL_NEW_GAME;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Accepted &&
                      !session.TryTakeControlCompletion(outcome) && worker.IsActive() &&
                      session.TryGetHintMoveSquares(from, to) &&
                      from == engine.completedCandidates[5U].fromSquare &&
                      to == engine.completedCandidates[5U].toSquare,
                  "accepted NewGame defers position mutation until cleanup and retains the still-current Hint");
    // Accepted queues the action. Run its production update while the real
    // refinement is still held, so cancellation precedes worker cleanup.
    results.Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      engine.cancels.load() > cancelsBeforeNewGame &&
                      worker.IsActive() && !session.TryTakeControlCompletion(outcome) &&
                      session.TryGetHintMoveSquares(from, to) &&
                      from == engine.completedCandidates[5U].fromSquare &&
                      to == engine.completedCandidates[5U].toSquare &&
                      engine.unsafeReads.load() == 0U,
                  "queued NewGame requests cancellation before held refinement cleanup and completion");
    Checkpoint(results, session, engine, RUN, "human-Hint-cancel-draining", "wait-for-safe-new-game");
    results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      session.TryTakeControlCompletion(outcome) && outcome.requestId == 873U &&
                      outcome.status == modules::ControlMutationStatus::Applied &&
                      !session.TryTakeControlCompletion(outcome) &&
                      !session.TryGetHintMoveSquares(from, to) &&
                      std::strcmp(session.HomeStateSnapshot().fen, modules::INITIAL_FEN) == 0 &&
                      engine.unsafeReads.load() == 0U,
                  "NewGame completes once after real cancellation without stale Hint or active-engine reads");
    const uint32_t resetAt = session.NowMs();
    results.Check(session.PlacementModeActive() &&
                      session.State().expected_occupied_mask == UINT64_C(0xFFFF00000000FFFF) &&
                      (session.State().physical_occupied_mask &
                       ~session.State().expected_occupied_mask & (UINT64_C(1) << 20U)) != 0ULL &&
                      CapturedSquareLit(session, 20U, false) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece, false) &&
                      CapturedSquareLit(session, 27U, false) &&
                      CapturedSquareLit(session, 21U, false) &&
                      ReadHome(session, home) && std::strcmp(home["a"] | "", "corr") == 0 &&
                      std::strcmp(home["as"] | "", "0x") == 0,
                  "NewGame immediately owns correction and suppresses old Hint through the contractual quiet stage");
    Checkpoint(results, session, engine, RUN, "new-game-exit-acknowledged", "remove-extra-pieces-then-arrange-starting-position");
    // Ordinary extra-piece correction is deliberately graded: quiet <300ms,
    // local amber through1499ms, then shared rainbow/red. Replacement and
    // stale Hint suppression are immediate throughout these presentation stages.
    results.Check(session.Advance(modules::CORRECTION_QUIET_MS - 1U) == VIRTUAL_LITCH_OK &&
                      session.NowMs() - resetAt == 299U &&
                      CapturedSquareLit(session, 20U, false) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece, false) &&
                      !CapturedStatusRed(session) && !session.TryGetHintMoveSquares(from, to) &&
                      !CapturedRemovalCue(session, 21U),
                  "reset recovery remains quiet through299ms without reviving Hint removal or corner feedback");
    Checkpoint(results, session, engine, RUN, "new-game-quiet-299ms", "remove-extra-pieces-then-arrange-starting-position");
    results.Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      CapturedAmberSquare(session, 20U) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece, false) &&
                      !CapturedStatusRed(session) && !session.TryGetHintMoveSquares(from, to),
                  "at300ms extra e6 receives the local amber bracket in semantic and mapped output");
    Checkpoint(results, session, engine, RUN, "new-game-local-amber-300ms", "remove-extra-piece-e6-and-other-leftovers");
    results.Check(session.Advance(modules::CORRECTION_STRONG_MS - modules::CORRECTION_QUIET_MS - 1U) == VIRTUAL_LITCH_OK &&
                      CapturedAmberSquare(session, 20U) && !CapturedStatusRed(session),
                  "local amber stays steady through1499ms without a premature global red fault");
    Checkpoint(results, session, engine, RUN, "new-game-local-amber-1499ms", "remove-extra-piece-e6-and-other-leftovers");
    results.Check(session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      CapturedSquareLit(session, 20U) && CapturedSquareSplit(session, 20U) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece) &&
                      !CueEqual(session.SemanticFrame().board[20U][1U], session.SemanticFrame().board[20U][0U]) &&
                      CapturedStatusRed(session) && CapturedSquareLit(session, 12U) &&
                      CapturedSlotLit(session, 12U, hardware::SquareLedSlot::Piece, false) &&
                      !CapturedSquareSplit(session, 12U) &&
                      !session.TryGetHintMoveSquares(from, to) && session.UndoHistoryMoveCount() == 0U,
                  "at1500ms e6extra keeps rainbow/red correction alongside e7 placement sides and its dark empty center");
    const auto rainbowTargets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, 20U, hardware::SquareLedSlot::Square0, session.HallRotate());
    const auto rainbowLeft = session.PhysicalFrame().pixels[rainbowTargets.indices[0U]];
    Checkpoint(results, session, engine, RUN, "new-game-rainbow-red-1500ms", "remove-extra-piece-e6-and-other-leftovers");
    results.Check(session.Advance(100U) == VIRTUAL_LITCH_OK &&
                      CapturedSquareSplit(session, 20U) &&
                      !CueEqual(rainbowLeft, session.PhysicalFrame().pixels[rainbowTargets.indices[0U]]) &&
                      !session.TryGetHintMoveSquares(from, to),
                  "actual mapped extra-piece rainbow rotates without reviving the old d5f6 tuple");
    Checkpoint(results, session, engine, RUN, "new-game-rainbow-cadence", "remove-extra-piece-e6-and-other-leftovers");
    occupancy &= UINT64_C(0xFFFF00000000FFFF);
    results.Check(Observe(session, occupancy) && session.Advance(100U) == VIRTUAL_LITCH_OK &&
                      session.PlacementModeActive() &&
                      CapturedSquareLit(session, 20U, false) &&
                      CapturedSlotLit(session, 20U, hardware::SquareLedSlot::Piece, false) &&
                      CapturedSquareLit(session, 27U, false) && CapturedSquareLit(session, 21U, false) &&
                      CapturedSquareLit(session, 12U) &&
                      CapturedSlotLit(session, 12U, hardware::SquareLedSlot::Piece, false) &&
                      !CapturedSquareSplit(session, 12U) && !CapturedStatusRed(session) &&
                      !session.TryGetHintMoveSquares(from, to),
                  "removing actual extras retires rainbow; undetected e7 shows owner sides with its center dark");
    Checkpoint(results, session, engine, RUN, "new-game-extras-removed", "place-detectable-missing-starting-pieces");
    // The paired actual ARDUINO fixture proves the8180-byte allocation fault.
    // This journey proves production Hall/settling/turn/Hint/CPU/Home/render
    // integration; its native scheduler does not emulate ESP-IDF allocation.
    std::printf("[CAPTURED_CPU_STALL_JOURNEY] actual_search=1 actual_hall_prefix=1 "
                "hint_pending_thinking_full5_ready=1 allocation_fault_in_paired_arduino_fixture=1 "
                "physical_timing_claim=0\n");
}

} // namespace

void CheckBenchmarkDiagnosticIsolation(Results& results) {
    constexpr const char* PATH = "build/test_benchmark_diagnostic_isolation.json";
    (void)std::remove(PATH);
    modules::SettingsStoreConfig config;
    config.path = PATH;
    modules::SettingsStore settings(config);
    results.Check(settings.Load(), "benchmark cache fixture loads");
    settings.Doc()["GAME_FEN"] = "8/8/8/k3p3/7P/Q1p3P1/5P2/1R4K1 b - - 0 1";
    settings.Doc()["TIMER"] = 0U;
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine state;
    modules::AsyncThinkWorker worker;
    modules::GamePersistenceCoordinator owner(settings, engine, state, nullptr, &worker);
    owner.SetBenchmarkControlsIdleProbe(nullptr, [](void*) { return true; });
    results.Check(owner.LoadOrInitialize(), "benchmark cache owner loads terminal position");
    runtime::InteractionDispatcher dispatcher(state, owner);
    const auto expected = state.GetExpectedOccupancyMask();
    (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U, expected, expected});
    owner.Update();
    // Supporting cache fixture only: the full107 production journey is separate.
    modules::DiagnosticSnapshotTestAccess::SeedPriorDiagnosticHistory(owner, worker);
    modules::ThinkDiagnostics cpu, hint;
    results.Check(owner.TryGetThinkDiagnostics(modules::ThinkRequestKind::Cpu, cpu) &&
        owner.TryGetThinkDiagnostics(modules::ThinkRequestKind::Hint, hint), "prior typed histories available");
    results.Check(owner.StartBenchmark(1U, 1U), "benchmark starts without retiring live histories");
    results.Check(WaitFor([&] { return !worker.IsActive(); }), "benchmark actual search finishes before controlled failed-copy seam");
    const auto revision = modules::DiagnosticSnapshotTestAccess::HoldCopyUnavailable(worker);
    modules::EngineBenchmarkStatus unavailable;
    results.Check(!owner.TryGetBenchmarkStatus(unavailable) && unavailable.caseId == 0U,
        "current benchmark failed coherent copy cannot invent cached activity/state");
    modules::DiagnosticSnapshotTestAccess::ReleaseCopy(worker, revision);
    owner.Update();
    modules::ThinkDiagnostics afterCpu, afterHint;
    results.Check(owner.TryGetThinkDiagnostics(modules::ThinkRequestKind::Cpu, afterCpu) &&
        owner.TryGetThinkDiagnostics(modules::ThinkRequestKind::Hint, afterHint) &&
        std::memcmp(&cpu, &afterCpu, sizeof(cpu)) == 0 && std::memcmp(&hint, &afterHint, sizeof(hint)) == 0,
        "benchmark refresh preserves both quiescent live cache identities and accepted Hint timing");
    modules::EngineBenchmarkStatus status;
    results.Check(owner.TryGetBenchmarkStatus(status) && status.diagnostics.kind == modules::ThinkRequestKind::Benchmark &&
        !status.diagnostics.firstAcceptedHintValid, "benchmark keeps its distinct cached request after cleanup");
}

void CheckFull3RootDiagnostics(Results& results) {
    modules::AsyncThinkWorker worker;
    modules::ThinkConfig config;
    s_nowMs.store(0U);
    worker.PrepareDiagnostics(701U, modules::ThinkRequestKind::Hint, config,
                              modules::SideToMove::White, false, 255U);
    auto observer = worker.PrepareCandidateObserver();
    modules::ThinkCandidate full3;
    full3.source = modules::ThinkCandidateSource::Search;
    full3.rootKind = modules::ThinkRootKind::ClassicFull;
    full3.completedDepth = 3U;
    modules::ThinkDiagnostics data;
    for (uint8_t provenance = 0U; provenance < 5U; ++provenance) {
        auto excluded = full3;
        if (provenance == 0U) excluded.source = modules::ThinkCandidateSource::Book;
        if (provenance == 1U) excluded.replayed = true;
        if (provenance == 2U) excluded.rootKind = modules::ThinkRootKind::ClassicPreliminary;
        if (provenance == 3U) {
            excluded.rootKind = modules::ThinkRootKind::DuckCompound;
            excluded.isDuck = true;
        }
        if (provenance == 4U) excluded.rootKind = modules::ThinkRootKind::Unavailable;
        observer.RootCompleted(excluded);
        results.Check(worker.TryGetDiagnostics(data) && !data.firstFull3RootValid &&
                          data.firstFull3RootAtMs == 0U,
                      "Book/replay/preliminary/Duck/unknown completion cannot claim fresh Full3 time");
    }
    observer.RootCompleted(full3);
    results.Check(worker.TryGetDiagnostics(data) && data.firstFull3RootValid &&
                      data.firstFull3RootAtMs == 0U && data.stopReason == modules::ThinkStopReason::Unavailable,
                  "actual Full3 observer records a valid zero timestamp with initialized word-copy tail");
    s_nowMs.store(25U);
    observer.RootCompleted(full3);
    auto full5 = full3; full5.completedDepth = 5U;
    observer.RootCompleted(full5);
    results.Check(worker.TryGetDiagnostics(data) && data.fullDepth == 5U &&
                      data.firstFull3RootValid && data.firstFull3RootAtMs == 0U,
                  "duplicate Full3 and deeper refinement cannot replace the first timestamp");
    s_nowMs.store(UINT32_MAX - 50U);
    worker.PrepareDiagnostics(702U, modules::ThinkRequestKind::Cpu, config,
                              modules::SideToMove::Black, false, 255U);
    observer = worker.PrepareCandidateObserver();
    results.Check(worker.TryGetDiagnostics(data) && data.kind == modules::ThinkRequestKind::Cpu &&
                      !data.firstFull3RootValid && data.firstFull3RootAtMs == 0U,
                  "new CPU request resets prior Hint Full3 timestamp and validity");
    auto full4 = full3; full4.completedDepth = 4U;
    observer.RootCompleted(full4);
    observer.RootCompleted(full3);
    results.Check(worker.TryGetDiagnostics(data) && data.fullDepth == 4U &&
                      !data.firstFull3RootValid,
                  "skipped depth3 and later out-of-order root do not infer its completion time");
    worker.PrepareDiagnostics(703U, modules::ThinkRequestKind::Hint, config,
                              modules::SideToMove::White, false, 255U);
    observer = worker.PrepareCandidateObserver();
    s_nowMs.store(24U);
    observer.RootCompleted(full3);
    results.Check(worker.TryGetDiagnostics(data) && data.kind == modules::ThinkRequestKind::Hint &&
                      data.firstFull3RootValid && data.firstFull3RootAtMs == 24U &&
                      data.firstFull3RootAtMs - data.requestAtMs == 75U,
                  "fresh Hint Full3 elapsed time remains correct across millis wrap");
}

void CheckDiagnosticFreshness(Results& results) {
    using Access = modules::DiagnosticSnapshotTestAccess;
    constexpr const char* PATH = "build/test_diagnostic_freshness.json";
    constexpr const char* FEN = "2r1kb1r/p2b1p2/1pnppq1p/6p1/P3P3/2PBBN1P/1P3PP1/R2Q1RK1 w k - 0 1";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, false, false, false, FEN, 0U, 1U),
                  "freshness fixture saves real current26 CPU position");
    modules::AsyncThinkWorker worker; GatedEngine engine(worker);
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, PATH, &engine, &worker);
    struct Admission {
        modules::GamePersistenceCoordinator* owner = nullptr;
        modules::ThinkDiagnostics saved;
    } admission;
    session.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                                modules::GamePersistenceCoordinator& owner) {
        static_cast<Admission*>(context)->owner = &owner;
    }, &admission);
    engine.admissionContext = &admission;
    engine.admissionObserver = [](void* context) {
        auto& capture = *static_cast<Admission*>(context);
        capture.saved = Access::Cached(*capture.owner);
    };
    results.Check(Observe(session, session.State().expected_occupied_mask), "Hall observation admits freshness CPU");
    for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && worker.IsActive() && admission.saved.requestId != 0U &&
                      admission.saved.state == modules::ThinkState::Idle && !admission.saved.active,
                  "actual admission captures a valid Idle cache before worker dispatch");
    if (!admitted) { (void)engine.Finish(session.NowMs()); return; }
    engine.admissionObserver = nullptr;
    // Controlled retired-cache formatting only; the real CPU remains parked at Poll.
    auto longIntervals = admission.saved;
    longIntervals.requestId = UINT32_MAX;
    longIntervals.requestAtMs = UINT32_MAX;
    longIntervals.preflightValid = longIntervals.startValid = longIntervals.pollValid = false;
    longIntervals.firstPublicationValid = longIntervals.firstFullRootValid = false;
    longIntervals.firstFull3RootValid = longIntervals.firstAcceptedHintValid = false;
    longIntervals.finalValid = false;
    longIntervals.quiescentValid = true;
    longIntervals.quiescentAtMs = UINT32_MAX - 1U;
    longIntervals.active = false;
    longIntervals.retired = true;
    Access::RestoreCache(*admission.owner, longIntervals);
    const auto longConsole = ReadDiagnosticConsole(session);
    const std::string longTiming = "search: kind=cpu request_at_ms=4294967295 preflight_ms=unavailable dispatch_ms=unavailable poll_ms=unavailable first_worker_ms=unavailable first_full_ms=unavailable first_hint_ms=unavailable final_ms=unavailable quiescent_ms=4294967295";
    results.Check(longConsole.find(longTiming) != std::string::npos &&
                      longConsole.find("search: kind=cpu first_full3_ms=unavailable") != std::string::npos,
                  "bounded actual Console retains original timing tail and separate unavailable Full3 row");
    Access::RestoreCache(*admission.owner, admission.saved);
    uint32_t revision = Access::HoldCopyUnavailable(worker);
    modules::ThinkDiagnostics data; data.requestId = UINT32_MAX;
    modules::ThinkDiagnostics rejected_admission;
    results.Check((revision & 1U) == 0U && !worker.TryGetDiagnostics(rejected_admission),
                  "admission fault deterministically rejects both bounded worker copy attempts");
    const bool idleFound = ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, data);
    results.Check(!idleFound && data.requestId == 0U && !data.firstFull3RootValid &&
                      data.firstFull3RootAtMs == 0U,
                  "DIAG-FRESHNESS admission copy failure cannot expose cached Idle as current inactive");
    const auto idleConsole = ReadDiagnosticConsole(session);
    results.Check(idleConsole.find("search: kind=cpu active=unavailable status=unavailable") != std::string::npos &&
                      idleConsole.find("status=Idle") == std::string::npos,
                  "DIAG-FRESHNESS admission Console makes both activity and state unknown");
    std::printf("[DIAG_FRESHNESS] stage=admission cached=Idle actual_active=1 copy_unavailable=1 getter_found=%u physical_timing_claim=0\n", idleFound ? 1U : 0U);
    Access::ReleaseCopy(worker, revision);
    results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, data) &&
                      data.requestId == admission.saved.requestId && data.active && data.state == modules::ThinkState::Thinking,
                  "fresh successful copy recovers actual Thinking instead of the admission cache");
    Access::Refresh(*admission.owner);
    const auto thinking = Access::Cached(*admission.owner);
    results.Check(thinking.active && thinking.state == modules::ThinkState::Thinking,
                  "actual successful coordinator refresh stages a valid Thinking cache");
    results.Check(engine.Finish(session.NowMs() + 1U) && !worker.IsActive(), "real CPU completes before coordinator consumes terminal snapshot");
    revision = Access::HoldCopyUnavailable(worker);
    modules::ThinkDiagnostics rejected_terminal;
    results.Check((revision & 1U) == 0U && !worker.TryGetDiagnostics(rejected_terminal),
                  "terminal fault deterministically rejects both bounded worker copy attempts");
    const bool terminalFound = ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, data);
    results.Check(!terminalFound && data.requestId == 0U && !data.firstFull3RootValid &&
                      data.firstFull3RootAtMs == 0U,
                  "DIAG-FRESHNESS terminal copy failure cannot expose cached Thinking as current active");
    const auto terminalConsole = ReadDiagnosticConsole(session);
    results.Check(terminalConsole.find("search: kind=cpu active=unavailable status=unavailable") != std::string::npos &&
                      terminalConsole.find("status=Thinking") == std::string::npos,
                  "DIAG-FRESHNESS terminal Console makes activity and state unknown after cleanup");
    std::printf("[DIAG_FRESHNESS] stage=terminal cached=Thinking actual_active=0 copy_unavailable=1 getter_found=%u physical_timing_claim=0\n", terminalFound ? 1U : 0U);
    Access::ReleaseCopy(worker, revision);
    results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, data) &&
                      !data.active && data.state == modules::ThinkState::Done && data.finalValid && data.quiescentValid,
                  "fresh terminal recovery reports actual result and quiescence");
    const auto completed = data;
    engine.holdStart = 2U;
    results.Check(Set(session, 2201U, "VS_CPU_PLAYER", "2"), "canonical CPU-seat change retires finished request");
    for (uint8_t i = 0U; i < 30U && engine.starts < 2U; ++i) (void)session.Advance(100U);
    const bool replacement = engine.starts == 2U && WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(replacement && engine.continuousHint && worker.IsActive(), "actual human Hint takes over the shared worker");
    if (replacement) {
        revision = Access::HoldCopyUnavailable(worker);
        modules::ThinkDiagnostics rejected_takeover;
        results.Check((revision & 1U) == 0U && !worker.TryGetDiagnostics(rejected_takeover),
                      "takeover fault deterministically rejects both bounded worker copy attempts");
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, data) && data.retired &&
                          !data.active && data.quiescentValid && data.requestId == completed.requestId &&
                          data.latestPublication.rootKind == completed.latestPublication.rootKind &&
                          data.latestPublication.score == completed.latestPublication.score &&
                          data.firstPublicationAtMs == completed.firstPublicationAtMs && data.finalNodes == completed.finalNodes &&
                          data.firstFull3RootValid == completed.firstFull3RootValid &&
                          data.firstFull3RootAtMs == completed.firstFull3RootAtMs,
                      "confirmed per-kind retired history survives an unreadable current Hint snapshot");
        results.Check(!ReadDiagnostics(session, modules::ThinkRequestKind::Hint, data) && data.requestId == 0U,
                      "current Hint cannot borrow activity from old completed CPU history");
        const auto historyConsole = ReadDiagnosticConsole(session);
        results.Check(historyConsole.find("search: kind=hint active=unavailable status=unavailable") != std::string::npos &&
                          historyConsole.find("active=0 status=Done retired=1") != std::string::npos,
                      "Console separates confirmed completed CPU history from unavailable current Hint");
        Access::ReleaseCopy(worker, revision);
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Hint, data) && data.active &&
                          data.state == modules::ThinkState::Thinking && data.requestId != completed.requestId &&
                          !data.firstFull3RootValid && data.firstFull3RootAtMs == 0U,
                      "fresh replacement Hint recovers its own request and activity");
    }
    results.Check(engine.Finish(session.NowMs()), "freshness fixture drains the real replacement worker");
    results.Check(engine.unsafeReads.load() == 0U, "failed copies, Console and recovery never read an active engine");
}

void CheckCpuDiagnosticRates(Results& results) {
    constexpr const char* PATH = "build/test_cpu_diagnostic_rates.json";
    constexpr const char* POSITIONS[] = {
        "2r1kb1r/p2b1p2/1pnppq1p/6p1/P3P3/2PBBN1P/1P3PP1/R2Q1RK1 w k - 0 1",
        "4r1k1/r3q2p/p5p1/1pp5/4p1bN/4Q1P1/1P1N1PB1/R3R1K1 b - - 5 31"};
    for (uint8_t position = 0U; position < 2U; ++position) for (const uint32_t level : {1U, 4U}) {
        s_nowMs.store(1000U);
        results.Check(SaveFixture(PATH, false, position == 1U, false, false, POSITIONS[position], 0U, level),
                      "controlled-rate diagnostics saves exact current26 or heavy real position");
        modules::AsyncThinkWorker worker; GatedEngine engine(worker);
        engine.controlledNodesPerSecond = 3000U;
        engine.releaseStats.store(false);
        virtual_litch_config config{}; virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3; config.initial_time_ms = 1000U;
        Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, PATH, &engine, &worker);
        results.Check(Observe(session, session.State().expected_occupied_mask), "full Hall observation admits CPU diagnostic fixture");
        for (uint8_t i = 0U; i < 30U && engine.starts == 0U; ++i) (void)session.Advance(100U);
        const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
        results.Check(admitted && worker.IsActive(), "actual factory CPU request reaches held worker");
        if (!admitted) { engine.releaseStats.store(true); (void)engine.Finish(session.NowMs()); continue; }
        s_nowMs.store(engine.startedAtMs); engine.releasePoll.store(true);
        results.Check(WaitFor([&] { return engine.statsEntered.load(); }), "clock charged during real progress reaches actual deadline before terminal publication");
        modules::ThinkDiagnostics diagnostic;
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, diagnostic) &&
                          diagnostic.configuredCpuLevel == level &&
                          diagnostic.requested.maxThinkMs == (level == 1U ? 900U : 1400U) &&
                          diagnostic.effectiveValid && diagnostic.preliminaryDepth == (level == 1U ? 1U : 2U) &&
                          diagnostic.firstPublicationValid &&
                          diagnostic.firstPublication.source == modules::ThinkCandidateSource::Seed &&
                          diagnostic.firstPublication.rootKind == modules::ThinkRootKind::Seed &&
                          diagnostic.firstPublication.completedDepth == 0U &&
                          diagnostic.firstPublicationAtMs == diagnostic.startAtMs &&
                          diagnostic.completedRootCount > 0U && diagnostic.latestPublication.scoreValid &&
                          diagnostic.latestPublication.source == modules::ThinkCandidateSource::Search &&
                          !diagnostic.firstAcceptedHintValid && !diagnostic.finalValid,
                      "CPU admission Seed stays distinct from completed novice or reply-aware scored work and human Hint timing");
        if (position == 1U) results.Check(diagnostic.fullDepth == 0U && !diagnostic.firstFullRootValid &&
                                             diagnostic.latestPublication.rootKind == modules::ThinkRootKind::ClassicPreliminary,
                                         "heavy position keeps preliminary result without inventing unfinished Full work");
        engine.releaseStats.store(true); results.Check(WaitFor([&] { return !worker.IsActive(); }), "actual worker retires after held final statistics");
        results.Check(ReadDiagnostics(session, modules::ThinkRequestKind::Cpu, diagnostic) &&
                          diagnostic.finalValid && diagnostic.quiescentValid && diagnostic.finalNodesValid &&
                          diagnostic.state == modules::ThinkState::Done && !diagnostic.active,
                      "deadline terminal snapshot truthfully retains usable completed preliminary or Full result");
        const uint32_t observedTime = diagnostic.finalAtMs - diagnostic.startAtMs;
        std::printf("[CPU_DIAGNOSTIC_CONTROLLED_RATE] position=%u level=%lu assumed_nps=3000 first_ms=%lu final_ms=%lu nodes=%lu full_depth=%u preliminary_depth=%u roots=%lu\n",
            position, static_cast<unsigned long>(level), static_cast<unsigned long>(diagnostic.firstPublicationAtMs - diagnostic.startAtMs),
            static_cast<unsigned long>(observedTime), static_cast<unsigned long>(diagnostic.finalNodes),
            diagnostic.fullDepth, diagnostic.preliminaryDepth, static_cast<unsigned long>(diagnostic.completedRootCount));
        const auto console = ReadDiagnosticConsole(session);
        modules::ClassicThinkResult actual;
        results.Check(engine.GetLastClassicThinkResult(actual) && actual.preliminaryOnly ==
                          (diagnostic.latestPublication.rootKind == modules::ThinkRootKind::ClassicPreliminary),
                      "real final engine result independently agrees with cached preliminary versus Full provenance");
        results.Check(console.find("first_hint_ms=unavailable") != std::string::npos &&
                          console.find(actual.preliminaryOnly ? "source=ClassicPreliminary" : "source=ClassicFull") != std::string::npos,
                      "existing Console preserves actual terminal provenance and explicit unavailable Hint timing");
        if (position == 1U && level == 4U) std::printf("[TERMINAL_CPU_DIAGNOSTIC_CONSOLE]\n%s", console.c_str());
        (void)session.Advance(observedTime + 1U);
        StaticJsonDocument<1536> home;
        results.Check(ReadHome(session, home) && std::strcmp(home["a"] | "", "cpu") == 0 && engine.unsafeReads.load() == 0U,
                      "real coordinator queues the completed legal result after cleanup without an active engine read");
        results.Check(engine.Finish(session.NowMs()), "rate fixture leaves no active worker");
    }
}

// On V3 these LEDs also carry ordinary Turn and higher-priority guidance.
// Changing style at the same authoritative timestamp must not change them
// when cosmetics have yielded; requiring darkness would erase that guidance.
bool ThinkingControlsAbsent(runtime::VirtualLitchSession& session) {
    const auto before=session.PhysicalFrame();
    static uint16_t requestId=10000U;
    const std::string current=session.SettingsForPersistenceTest().ReadValue("HINT_THINKING_STYLE","foxie_sweep");
    const bool clockwise=current=="player_clockwise";
    if(!Set(session,++requestId,"HINT_THINKING_STYLE",clockwise?"\"foxie_sweep\"":"\"player_clockwise\""))return false;
    const auto after=session.PhysicalFrame();
    char saved[48]{};std::snprintf(saved,sizeof(saved),"\"%s\"",current.c_str());
    const bool restored=Set(session,++requestId,"HINT_THINKING_STYLE",saved);
    bool same=restored;
    if(session.PhysicalFrame().led_count==300U) {
        for(bool p2:{false,true})for(uint8_t role=0U;role<6U;++role) {
            const auto index=256U+hardware::V3_3PhysicalIndexForControl(p2,static_cast<hardware::ControlLedRole>(role));
            same&=CueEqual(before.pixels[index],after.pixels[index]);
        }
    } else {
        for(uint8_t index:{2U,3U,4U,5U,18U,19U,20U,21U})same&=CueEqual(before.pixels[256U+index],after.pixels[256U+index]);
    }
    return same&&!session.CpuThinkingVisible();
}

void CheckRawThinkingControlJourney(Results& results, bool cpu, bool black, bool firstP2, bool rotate, bool v3, bool visibleWitness=false) {
    constexpr const char* PATH = "build/test_raw_thinking_controls.json";
    char run[100];
    std::snprintf(run,sizeof(run),"%s/raw-%s/%c/end%u/rotate%u/%s",v3?"V3":"V3.3",cpu?"CPU":"Hint",black?'b':'w',firstP2?2U:1U,rotate,visibleWitness?"visible-witness":"10mA-safety");
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH,false,black,firstP2,rotate,nullptr,0U,cpu?3U:0U),"raw control journey saves real position/settings precondition");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);engine.holdStart=UINT32_MAX;
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=v3?VIRTUAL_LITCH_HARDWARE_V3:VIRTUAL_LITCH_HARDWARE_V3_3;config.initial_time_ms=1000U;
    config.max_led_current_ma=visibleWitness?1000U:10U;
    runtime::VirtualLitchSession session(config,engine,worker,PATH);
    results.Check(Set(session, 908U, "HINT_THINKING_STYLE", "\"foxie_sweep\""),
                  "Litchon-specific raw playback explicitly selects its retained token");
    const bool swap=modules::ComputeControlBankSwap(rotate,firstP2,false);
    const bool physicalP2=black!=swap;
    const uint8_t bank=physicalP2?VIRTUAL_LITCH_OWNER_P2:VIRTUAL_LITCH_OWNER_P1;
    uint64_t physicalOccupancy=0U;
    for(uint8_t square=0U;square<64U;++square)
        if((CHESS_OCCUPANCY&(UINT64_C(1)<<square))!=0U)
            physicalOccupancy|=UINT64_C(1)<<modules::MapSquareToPhysical(square,swap);
    results.Check(session.ObserveHallMask(physicalOccupancy)==VIRTUAL_LITCH_OK,"raw Hall scan passes production orientation/input adapter");
    for(uint8_t i=0U;i<30U&&!worker.IsActive();++i)(void)session.Advance(100U);
    const bool admitted=WaitFor([&]{return engine.pollEntered.load();});
    results.Check(admitted&&engine.starts==1U&&worker.IsActive(),"raw journey admits actual owned engine worker before controls");
    if(!admitted){(void)engine.Finish(session.State().now_ms);return;}
    if(!cpu){
        results.Check(ThinkingControlsAbsent(session),"background Hint analysis alone stays quiet on both control banks");
        results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::O,true)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK&&
                      session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::O,false)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK&&session.HintDisplayRequested(),
                      "raw physical O debounce/release reaches actual acknowledged Hint owner");
        modules::ControlMutationOutcome outcome;
        results.Check(session.TryGetLastBoardControlOutcome(outcome)&&outcome.status==modules::ControlMutationStatus::Applied,
                      "physical Hint action has its real applied domain acknowledgement");
    }
    // The 10 mA output envelope can quantize the first fade samples to zero.
    // Check the bank mapping at full presentation strength with search held.
    results.Check(session.Advance(200U) == VIRTUAL_LITCH_OK,
                  "raw control journey reaches the full thinking fade inside its current envelope");
    results.Check(ControlBankLit(session,physicalP2,true)&&(v3||ControlBankLit(session,!physicalP2,false)),"thinking uses same physical bank as raw mover button input");
    results.Check((visibleWitness||session.PhysicalFrame().power_limited) && session.PhysicalFrame().applied_led_current_ma <= config.max_led_current_ma,
                  "actual control pixels remain inside the existing configured current envelope");
    ControlCheckpoint(session,run,"pending-search","wait-for-move");
    if (cpu) {
        virtual_litch_action_request coaching{};
        coaching.struct_size = sizeof(coaching);
        coaching.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION;
        coaching.request_id = 909U;
        coaching.action = VIRTUAL_LITCH_CONTROL_COACHING;
        coaching.owner = black ? VIRTUAL_LITCH_OWNER_P1 : VIRTUAL_LITCH_OWNER_P2;
        coaching.coaching_mutation = static_cast<uint8_t>(modules::CoachingMutation::Enable);
        modules::ControlMutationOutcome outcome;
        results.Check(session.DispatchControlAction(coaching, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Applied &&
                          session.Advance(1800U) == VIRTUAL_LITCH_OK,
                      "Coaching enters through the acknowledged owner while actual CPU search stays held");
        const auto owner = black ? modules::UiOwner::P1 : modules::UiOwner::P2;
        bool badges = true;
        for (const auto index : modules::PlayerCornerLedIndices(owner))
            badges &= CueEnergy(session.SemanticFrame().status[index]) > 0U;
        results.Check(badges && session.CpuThinkingVisible() &&
                          ControlBankLit(session, physicalP2, true) && worker.IsActive(),
                      "CPU thinking bank coexists with ordinary Turn and the other player's Coaching corner badges");
        ControlCheckpoint(session, run, "CPU-Turn-Coaching-coexist", "wait-for-move");
    }
    const auto firstFoxieFrame = session.PhysicalFrame();
    results.Check(session.Advance(1200U) == VIRTUAL_LITCH_OK,
                  "same owned raw request advances one complete Foxie sweep");
    bool foxieColorChanged = false;
    const auto foxieRange = physicalP2 ? hardware::V3_3_P2_CONTROL_LED_RANGE
                                     : hardware::V3_3_P1_CONTROL_LED_RANGE;
    for (uint8_t position = 0U; position < (v3 ? 4U : foxieRange.count); ++position) {
        const uint8_t index = v3 ? modules::PlayerHomeLedIndex(
            physicalP2 ? modules::UiOwner::P2 : modules::UiOwner::P1, 2U + position)
            : foxieRange.first + position;
        foxieColorChanged |= !CueEqual(firstFoxieFrame.pixels[256U + index],
                                      session.PhysicalFrame().pixels[256U + index]);
    }
    results.Check(foxieColorChanged && engine.starts == 1U && worker.IsActive(),
                  "Foxie colors travel between physical positions on repeated sweeps without restarting real search");
    ControlCheckpoint(session, run, "Foxie-next-color-pass", "wait-for-move");
    const auto otherBankHasNoThinking=[&] {
        if(v3)return true; // Older topology also carries Turn and Coaching corners.
        for(uint8_t role=1U;role<6U;++role) {
            const auto index=256U+hardware::V3_3PhysicalIndexForControl(!physicalP2,static_cast<hardware::ControlLedRole>(role));
            if(CueEnergy(session.PhysicalFrame().pixels[index])!=0U)return false;
        }
        const auto o=session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(!physicalP2,hardware::ControlLedRole::O)];
        const auto logicalOther=black?modules::UiOwner::P1:modules::UiOwner::P2;
        const bool effective=modules::assistance_preferences::AutomaticHintsEffective(
            session.SettingsForPersistenceTest(),logicalOther);
        // Use the production effective CPU/Coach policy and semantic owner,
        // rather than the Home mask of saved latches or a permissive hue test.
        modules::ControlLedFeedback expectedOwner;
        modules::ControlLedFeedbackInputs expectedInputs;
        expectedInputs.coaching[black?0U:1U]=effective;
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay expectedOverlay;
        expectedOwner.Compose(expectedInputs,expectedOverlay);
        const auto semantic=expectedOverlay.controlPixels[(black?0U:6U)];
        const auto intended=effective?modules::ScaleRgbSaturatingBy256(
            modules::Rgb{150U,50U,255U},8U):modules::RGB_BLACK;
        if(semantic.r!=intended.r||semantic.g!=intended.g||semantic.b!=intended.b)return false;
        // The frame reports the final brightness including the V3.3 gain.
        // Replay just this canonical emitter through the existing production
        // pipeline at both ends of that16-bit report's half-code interval.
        const auto gain=hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3).ledOutputScale256;
        const auto mapped=[&](float reportOffset) {
            runtime::virtual_litch_internal::CaptureLedRenderer renderer(1U,false);
            modules::PowerBudgetPolicy unlimited({});
            modules::BudgetAwareLedPipelineConfig output;
            output.outputBrightnessScale256=gain;
            const float reported=(float(session.PhysicalFrame().applied_brightness_scale_65535)+reportOffset)/65535.0F;
            output.brightnessScale01=std::max(0.0F,reported)*256.0F/gain;
            modules::BudgetAwareLedPipeline pipeline(renderer,unlimited,output);
            pipeline.RenderFrame(&semantic,1U);
            return renderer.Pixels()[0U];
        };
        const auto low=mapped(-0.5F),high=mapped(0.5F);
        return o.r>=low.r&&o.r<=high.r&&o.g>=low.g&&o.g<=high.g&&
            o.b>=low.b&&o.b<=high.b&&o.w==0U;
    };
    for(uint8_t step=0U;step<7U;++step){
        results.Check(session.Advance(200U)==VIRTUAL_LITCH_OK&&ControlBankLit(session,physicalP2,true)&&otherBankHasNoThinking(),
            "real tick keeps Foxie on its owner while the other bank retains only its actual optional violet Coaching O");
        ControlCheckpoint(session,run,"Foxie-timed-200ms","wait-for-move");
    }
    const auto foxieFrame = session.PhysicalFrame();
    results.Check(Set(session,910U,"HINT_THINKING_STYLE","\"player_clockwise\""),"raw journey changes style through existing acknowledged settings owner");
    bool styleChanged = false;
    for (uint16_t index = 256U; index < session.PhysicalFrame().led_count; ++index)
        styleChanged |= !CueEqual(foxieFrame.pixels[index], session.PhysicalFrame().pixels[index]);
    results.Check(styleChanged, "accepted style choice changes actual mapped output at the same request phase");
    ControlCheckpoint(session, run, "clockwise-setting-applied", "wait-for-move");
    for(uint8_t step=0U;step<5U;++step){
        results.Check(session.Advance(200U)==VIRTUAL_LITCH_OK&&ControlBankLit(session,physicalP2,true),"clockwise style runs on actual mapped player bank");
        ControlCheckpoint(session,run,"clockwise-timed-200ms","wait-for-move");
    }
    results.Check(Set(session,911U,"HINT_THINKING_STYLE","\"beachball\"") &&
                      std::strcmp(session.SettingsForPersistenceTest().ReadValue("HINT_THINKING_STYLE", ""), "beachball") == 0,
                  "Beachball is accepted and read back through the same production settings owner during real search");
    const auto rainbowStart = session.PhysicalFrame();
    const auto rainbowRange = physicalP2 ? hardware::V3_3_P2_CONTROL_LED_RANGE : hardware::V3_3_P1_CONTROL_LED_RANGE;
    for (uint8_t step = 0U; step < 4U; ++step) {
        results.Check(session.Advance(137U) == VIRTUAL_LITCH_OK && worker.IsActive() && engine.starts == 1U,
                      "Beachball advances at the existing renderer cadence without restarting engine work");
        for (uint8_t position = 0U; position < 4U; ++position) {
            const uint8_t index = v3 ? modules::PlayerHomeLedIndex(
                physicalP2 ? modules::UiOwner::P2 : modules::UiOwner::P1, 2U + position)
                : rainbowRange.first + 1U + position;
            results.Check(CueEnergy(session.PhysicalFrame().pixels[256U + index]) != 0U,
                          "all four D-pad rainbow positions reach actual mapped V3.3/V3 RGBW under the configured current envelope");
        }
        ControlCheckpoint(session,run,"Beachball-pending-timed-137ms","wait-or-cancel-current-search");
    }
    bool rainbowMoved = false;
    for (uint16_t index = 256U; index < session.PhysicalFrame().led_count; ++index)
        rainbowMoved |= !CueEqual(rainbowStart.pixels[index], session.PhysicalFrame().pixels[index]);
    results.Check(rainbowMoved, "the pending Beachball changes real physical output over time");
    const char* editedPalette=(black!=firstP2)?"P2_END_PIECE_WHEEL":"P1_END_PIECE_WHEEL";
    results.Check(Set(session,912U,editedPalette,"84"),"raw palette journey starts one real step before pure green");
    results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::Up,true)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK,"raw held Up debounces through existing physical adapter");
    const auto range=physicalP2?hardware::V3_3_P2_CONTROL_LED_RANGE:hardware::V3_3_P1_CONTROL_LED_RANGE;
    const uint8_t first=v3?modules::PlayerHomeLedIndex(physicalP2?modules::UiOwner::P2:modules::UiOwner::P1,2U):range.first;
    const auto color=session.PhysicalFrame().pixels[256U+first];
    if(v3) {
        for(uint8_t i=0U;i<4U;++i) {
            const uint8_t index=modules::PlayerHomeLedIndex(physicalP2?modules::UiOwner::P2:modules::UiOwner::P1,2U+i);
            results.Check(CueEnergy(color)!=0U&&CueEqual(color,session.PhysicalFrame().pixels[256U+index]),
                "olderV3 raw Up retains its four-pixel held preview in the actual edited end color");
        }
    } else {
        const uint8_t up=hardware::V3_3PhysicalIndexForControl(physicalP2,hardware::ControlLedRole::Up);
        const auto upColor=session.PhysicalFrame().pixels[256U+up];
        if(visibleWitness)for(const auto role:{hardware::ControlLedRole::Right,hardware::ControlLedRole::Down,hardware::ControlLedRole::Left}) {
            const auto index=256U+hardware::V3_3PhysicalIndexForControl(physicalP2,role);
            results.Check(CueEnergy(session.PhysicalFrame().pixels[index])>0U,
                "raw-Beachball-Up/other-three-Dpad-visible: accepted palette flash preserves the simultaneous physical thinking cue");
        }
        results.Check(upColor.g>0U&&upColor.r==0U&&upColor.b==0U&&
                session.SettingsForPersistenceTest().ReadValue(editedPalette,0U)==85U,
            "V3.3 raw Up shows its actual new pure-green end color on only the pressed direction");
    }
    results.Check(worker.IsActive()&&(!cpu||(v3?!session.CpuThinkingVisible():session.CpuThinkingVisible())),
        "palette feedback preserves the actual search and topology-specific thinking ownership");
    ControlCheckpoint(session,run,"raw-Up-preview","release-Up");
    results.Check(session.Advance(700U)==VIRTUAL_LITCH_OK&&session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::Up,false)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK,
                      "held Up repeats through production timing and physical release");
    results.Check(!cpu||session.CpuThinkingVisible(),"debounced release restores still-current CPU cue");
    results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::Down,true)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK&&
                  session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::Down,false)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK,
                  "raw Down uses same priority and release path");
    ControlCheckpoint(session,run,"raw-edit-release","wait-for-move");
    results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::X,true)==VIRTUAL_LITCH_OK&&session.Advance(500U)==VIRTUAL_LITCH_OK&&
                  ThinkingControlsAbsent(session),"raw X hold foreground owns presentation above thinking");
    ControlCheckpoint(session,run,"X-hold-foreground","release-X");
    results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::X,false)==VIRTUAL_LITCH_OK&&session.Advance(20U)==VIRTUAL_LITCH_OK,"raw X release exits hold foreground");
    // Contradictory Hall input suppresses cosmetics while keeping search owned.
    // A pawn remains an ordinary contradictory pickup; acknowledged king
    // holds now have their separate proportional resignation cue journey.
    const uint64_t lifted=physicalOccupancy&~(UINT64_C(1)<<modules::MapSquareToPhysical(11U,swap));
    results.Check(session.ObserveHallMask(lifted)==VIRTUAL_LITCH_OK&&session.Advance(700U)==VIRTUAL_LITCH_OK&&
                  ThinkingControlsAbsent(session),"raw incorrect pickup hides control animation during correction");
    ControlCheckpoint(session,run,"wrong-pickup-correction","return-lifted-piece");
    results.Check(session.ObserveHallMask(physicalOccupancy)==VIRTUAL_LITCH_OK&&session.Advance(700U)==VIRTUAL_LITCH_OK,"raw correction restoration reaches recovery");
    ControlCheckpoint(session,run,"correction-restored","wait-for-move");
    virtual_litch_runtime_health_fact health{};virtual_litch_default_runtime_health_fact(&health);
    health.board_scan=VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
    results.Check(session.DispatchRuntimeHealthFact(health)==VIRTUAL_LITCH_OK&&session.Advance(100U)==VIRTUAL_LITCH_OK&&session.DispatchRuntimeHealthFact(health)==VIRTUAL_LITCH_OK&&
                  ThinkingControlsAbsent(session),"production paused health suppresses control feedback");
    ControlCheckpoint(session,run,"health-paused","restore-board-health");
    health.board_scan=VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE;
    results.Check(session.DispatchRuntimeHealthFact(health)==VIRTUAL_LITCH_OK&&session.Advance(100U)==VIRTUAL_LITCH_OK&&session.DispatchRuntimeHealthFact(health)==VIRTUAL_LITCH_OK,"production health facts recover existing owner");
    // Cancel through the existing acknowledged New Game owner before allowing late worker data.
    virtual_litch_action_request request{};request.struct_size=sizeof(request);request.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id=912U;request.action=VIRTUAL_LITCH_CONTROL_NEW_GAME;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request,outcome)==VIRTUAL_LITCH_OK&&outcome.status==modules::ControlMutationStatus::Accepted,"existing New Game acknowledges cancellation while worker drains");
    engine.holdStart=0U;
    results.Check(engine.Finish(session.State().now_ms)&&session.Advance(1U)==VIRTUAL_LITCH_OK&&engine.unsafeReads.load()==0U,"late worker cleanup stays on its real ownership lane");
    results.Check(ThinkingControlsAbsent(session),"canceled old request leaves no stale control flash");
    results.Check(std::strcmp(session.SettingsForPersistenceTest().ReadValue("HINT_THINKING_STYLE", ""), "beachball") == 0,
                  "palette, correction, health and cancellation never rewrite the selected Beachball preference");
    ControlCheckpoint(session,run,"Beachball-canceled-cleanup","human-next-action");
}

void CheckCapturedWorkspaceAllocation(Results& results, const bool duck,
                                      const bool transientPressure = false) {
    constexpr const char* PATH = "build/test_captured_workspace_allocation.json";
    constexpr const char* CAPTURED_CPU_FEN =
        "rnbqkbnr/pppppppp/8/8/5P2/8/PPPPP1PP/RNBQKBNR b KQkq f3 0 1";
    const char* run = duck ? "CapturedWorkspace/Duck/raw-O" : transientPressure
        ? "CapturedWorkspace/Classic/CPU5/transient-pressure" : "CapturedWorkspace/Classic/CPU5";
    const size_t priorLimit = litchcraft_allocation_probe::maximumBytes;
    const size_t priorTotalLimit = litchcraft_allocation_probe::maximumTotalBytes;
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, duck, !duck, false, false,
        duck ? modules::INITIAL_FEN : CAPTURED_CPU_FEN, 0U, duck ? 0U : 5U),
        "workspace journey restores authoritative position and player ownership");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = UINT32_MAX;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.initial_time_ms = 1000U;
    runtime::VirtualLitchSession session(config, engine, worker, PATH);
    const uint64_t occupancy = duck ? UINT64_C(0xffff00000100ffff)
                                   : UINT64_C(0xffdf00200000ffff);
    results.Check(session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK, "captured workspace journey uses real Hall occupancy");
    for (uint8_t tick = 0U; tick < 30U && !worker.IsActive(); ++tick)
        (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted, "actual coordinator admits search to the real worker");
    if (duck) {
        results.Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
            session.Advance(20U) == VIRTUAL_LITCH_OK &&
            session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
            session.Advance(20U) == VIRTUAL_LITCH_OK && session.HintDisplayRequested(),
            "physical O has an acknowledged Duck Hint request before search completion");
    }
    (void)session.Advance(500U);
    ControlCheckpoint(session, run, "request-admitted", "wait-for-calculated-move");
    // Apply the historically captured largest-block/total limits, or deny
    // all nonthrowing heap requests, only while the real worker calculates.
    const size_t searchBudget = transientPressure ? 0U : 26000U;
    size_t allocationCalls = 0U;
    const auto finishSearch = [&] {
        using namespace litchcraft_allocation_probe;
        maximumBytes = 13300U;
        maximumTotalBytes = searchBudget;
        acceptedBytes = calls = largestRequest = 0U;
        const bool done = engine.Finish(session.State().now_ms);
        allocationCalls = calls;
        maximumBytes = priorLimit;
        maximumTotalBytes = priorTotalLimit;
        return done;
    };
    const bool finished = admitted && finishSearch();
    results.Check(finished && !worker.IsActive(), "actual search publishes terminal cleanup");
    (void)session.Advance(0U);
    modules::ThinkDiagnostics data;
    const auto kind = duck ? modules::ThinkRequestKind::Hint : modules::ThinkRequestKind::Cpu;
    const bool diagnostic = ReadDiagnostics(session, kind, data);
    uint8_t from = 64U, to = 64U, duckFrom = 64U, duckTo = 64U;
    const bool useful = duck ? session.TryGetHintMoveSquares(from, to) &&
        session.TryGetHintDuckMoveSquares(duckFrom, duckTo) :
        session.State().required_action != 0U;
    results.Check(diagnostic && data.state == modules::ThinkState::Done &&
                      data.finalNodesValid && data.finalNodes > 0U && data.completedRootCount > 0U &&
                      useful && allocationCalls == 0U,
                  "fixed scratch supplies a calculated CPU move or compound Hint without a heap request");
    std::printf("[CAPTURED_WORKSPACE] variant=%s total_limit=%zu allocations=%zu nodes=%u roots=%u useful=%u\n",
        duck ? "Duck" : "Classic", searchBudget, allocationCalls, data.finalNodes,
        data.completedRootCount, useful);
    ControlCheckpoint(session, run, "calculation-terminal", useful ? "follow-calculated-move" : "no-calculated-next-action");
    if (transientPressure) {
        StaticJsonDocument<1536> home;
        modules::UciMove queued;
        litch_max::ParsedUciMove scoredMove;
        const bool scoredReady = data.latestPublication.source == modules::ThinkCandidateSource::Search &&
            data.latestPublication.scoreValid && data.latestPublication.completedDepth > 0U && ReadHome(session, home) &&
            queued.Format("%s%s", home["c"][0] | "", home["c"][1] | "") &&
            litch_max::TryParseUciMove(queued.CStr(), scoredMove) &&
            scoredMove.fromSquare == data.latestPublication.fromSquare &&
            scoredMove.toSquare == data.latestPublication.toSquare;
        results.Check(scoredReady && session.Advance(1100U) == VIRTUAL_LITCH_OK && engine.starts == 1U,
                      "the scored CPU move stays queued once, without a speculative retry");
        if (scoredReady) {
            uint64_t physical = occupancy;
            if ((physical & (UINT64_C(1) << scoredMove.toSquare)) != 0ULL) {
                physical &= ~(UINT64_C(1) << scoredMove.toSquare);
                results.Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
                              "physical acknowledgement removes the scored capture victim first");
            }
            physical &= ~(UINT64_C(1) << scoredMove.fromSquare);
            results.Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK,
                          "physical acknowledgement lifts the scored move source");
            physical |= UINT64_C(1) << scoredMove.toSquare;
            results.Check(session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
                              session.Advance(2600U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U,
                          "the scored CPU decision receives exactly one physical move acknowledgement");
        }
        engine.CancelThink();
        const uint32_t priorStarts = engine.starts;
        results.Check(engine.Finish(session.State().now_ms) && session.LoadExternalFen(CAPTURED_CPU_FEN) &&
                          session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK,
                      "explicit position replacement retires old work before proving scratch reuse on the restored CPU root");
        for (uint8_t tick = 0U; tick < 30U && engine.starts == priorStarts; ++tick) (void)session.Advance(100U);
        results.Check(WaitFor([&] { return engine.pollEntered.load(); }) &&
                          engine.starts == priorStarts + 1U && finishSearch(),
                      "the restored CPU job reuses the same workspace while new heap requests remain unavailable");
        (void)session.Advance(0U);
        const bool recovered = ReadDiagnostics(session, kind, data) &&
            data.state == modules::ThinkState::Done && data.finalNodes > 0U && data.completedRootCount > 0U &&
            data.latestPublication.source == modules::ThinkCandidateSource::Search &&
            session.State().required_action != 0U &&
            allocationCalls == 0U;
        results.Check(recovered, "restored root produces another scored CPU decision with the same workspace");
        ControlCheckpoint(session, run, "restored-root", recovered ? "follow-calculated-move" : "no-calculated-next-action");
    }
    if (worker.IsActive()) (void)engine.Finish(session.State().now_ms);
}

void CheckClassicHintRequestHandoff(Results& results) {
    constexpr const char* PATH = "build/test_classic_hint_request_handoff.json";
    constexpr const char* RUN = "Classic/prior-White-Hint/e2e3/Black-background";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, false, false, false, false, modules::INITIAL_FEN, 0U, 0U),
                  "Classic comparison restores the same human e2e3 opening");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker); engine.holdStart = UINT32_MAX;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, PATH, &engine, &worker);
    uint64_t occupancy = UINT64_C(0xffff00000000ffff);
    results.Check(Observe(session, occupancy), "Classic raw Hall scan reaches the production opening");
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted, "Classic automatic White worker starts on its real engine lane");
    if (!admitted) return;
    virtual_litch_action_request request{}; request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION; request.request_id = 890U;
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied && session.HintDisplayRequested() &&
                      engine.Finish(session.NowMs()) && session.Advance(600U) == VIRTUAL_LITCH_OK,
                  "Classic comparison has a prior explicit White Hint and a quiescent worker");
    results.Check(Move(session, occupancy, 52U, 44U), "Classic e2e3 commits through real physical input");
    for (uint8_t i = 0U; i < 30U && engine.starts < 2U; ++i) (void)session.Advance(100U);
    results.Check(engine.starts == 2U && WaitFor([&] { return engine.pollEntered.load(); }) &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK && session.State().side_to_move_black == 1U &&
                      !session.HintDisplayRequested() && ControlBankLit(session, false, false) && ControlBankLit(session, true, false),
                  "existing Classic commit already retires the outgoing Hint before Black background analysis");
    Checkpoint(results, session, engine, RUN, "Black-background-request-retired", "Black-chooses-own-move");
    results.Check(engine.Finish(session.NowMs()) && session.Advance(1U) == VIRTUAL_LITCH_OK && engine.unsafeReads.load() == 0U,
                  "Classic comparison reaps its controlled worker without engine-lane violations");
}

void CheckDuckHintRequestHandoff(Results& results, const bool blackCoaching) {
    constexpr const char* PATH = "build/test_duck_hint_request_handoff.json";
    const char* RUN = blackCoaching
        ? "Duck/prior-White-Hint/e2e3/h4-h5/Black-Coaching"
        : "Duck/prior-White-Hint/e2e3/h4-h5/Black-background";
    constexpr uint8_t FROM_DUCK = 39U, TO_DUCK = 31U;
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH, true, false, false, false, modules::INITIAL_FEN, 0U, 0U),
                  "handoff restores a human Duck opening without Coaching");
    {
        modules::SettingsStoreConfig settingsConfig; settingsConfig.path = PATH;
        modules::SettingsStore settings(settingsConfig);
        modules::DuckGameDurableState variant; variant.variant = modules::GameVariant::Duck;
        variant.duckSquare = FROM_DUCK;
        char encoded[32]{};
        results.Check(settings.Load() && modules::EncodeDuckGameDurableState(variant, encoded, sizeof(encoded)),
                      "handoff fixture uses the captured h4 Duck precondition");
        settings.Doc()[modules::KEY_GAME_VARIANT_V1] = encoded;
        settings.MarkDirty();
        results.Check(settings.SaveNow(), "handoff fixture persists the exact initial Duck square");
    }
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker); engine.holdStart = UINT32_MAX;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, PATH, &engine, &worker);
    uint64_t occupancy = UINT64_C(0xffff00000000ffff) | (UINT64_C(1) << FROM_DUCK);
    results.Check(Observe(session, occupancy), "raw Hall occupancy reaches real Duck game owners");
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && worker.IsActive() && !session.HintDisplayRequested(),
                  "automatic White analysis starts with no requested Hint");
    if (!admitted) return;
    results.Check(Set(session, 891U, "HINT_THINKING_STYLE", "\"foxie_sweep\"") &&
                      Set(session, 892U, "HINT_THINKING_STYLE", "\"player_clockwise\"") &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK && !session.HintDisplayRequested() &&
                      ControlBankLit(session, false, false) && ControlBankLit(session, true, false),
                  "style changes during automatic background analysis never request Hint or acquire either control bank");
    Checkpoint(results, session, engine, RUN, "background-style-change-quiet", "play-or-request-Hint");
    virtual_litch_action_request request{}; request.struct_size = sizeof(request);
    request.abi_version = VIRTUAL_LITCH_CONTROL_ABI_VERSION; request.request_id = 893U;
    modules::ControlMutationOutcome outcome;
    if (blackCoaching) {
        request.action = VIRTUAL_LITCH_CONTROL_COACHING;
        request.owner = VIRTUAL_LITCH_OWNER_P2;
        request.coaching_mutation = static_cast<uint8_t>(modules::CoachingMutation::Enable);
        results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Applied && !session.HintDisplayRequested(),
                      "incoming Black Coaching is enabled without revealing White background guidance");
        ++request.request_id;
    }
    request.action = VIRTUAL_LITCH_CONTROL_HINT;
    request.owner = VIRTUAL_LITCH_OWNER_NONE;
    results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                      outcome.status == modules::ControlMutationStatus::Applied && session.HintDisplayRequested(),
                  "prior White Hint is an explicit acknowledged action");
    results.Check(engine.Finish(session.NowMs()) && session.Advance(600U) == VIRTUAL_LITCH_OK,
                  "actual White search completes before the physical combined turn");
    results.Check(Move(session, occupancy, 52U, 44U) && session.HintDisplayRequested(),
                  "e2e3 preserves White requested guidance only through the Duck half");
    Checkpoint(results, session, engine, RUN, "White-chess-half-complete", "move-Duck-h4-h5");
    results.Check(Move(session, occupancy, FROM_DUCK, TO_DUCK),
                  "actual h4h5 Hall choreography commits the combined turn");
    for (uint8_t i = 0U; i < 30U && engine.starts < 2U; ++i) (void)session.Advance(100U);
    const bool blackAdmitted = engine.starts == 2U && WaitFor([&] { return engine.pollEntered.load(); });
    StaticJsonDocument<1536> home;
    results.Check(blackAdmitted && worker.IsActive() && ReadHome(session, home) && home["co"].as<unsigned>() == (blackCoaching ? 2U : 0U) &&
                      session.State().side_to_move_black == 1U && session.DuckSquare() == TO_DUCK &&
                      std::strcmp(home["fen"] | "", "rnbqkbnr/pppppppp/8/8/8/4P3/PPPP1PPP/RNBQKBNR b KQkq - 0 1") == 0,
                  "captured Black e3/h5 position has real analysis and the intended owner-local Coaching latch");
    const bool requestMatchesIncomingOwner = session.HintDisplayRequested() == blackCoaching;
    const bool advancedBlack = session.Advance(250U) == VIRTUAL_LITCH_OK;
    results.Check(requestMatchesIncomingOwner && advancedBlack &&
                      ControlBankLit(session, false, false) && ControlBankLit(session, true, blackCoaching),
                  "complete Duck handoff retires White manual Hint and only Black Coaching may request incoming thinking");
    Checkpoint(results, session, engine, RUN,
               blackCoaching ? "Black-Coaching-requested" : "Black-background-no-request",
               blackCoaching ? "wait-for-own-Coaching-Hint" : "Black-chooses-own-move");
    results.Check(engine.Finish(session.NowMs()) && session.Advance(600U) == VIRTUAL_LITCH_OK &&
                      session.Advance(250U) == VIRTUAL_LITCH_OK && ReadHome(session, home),
                  "actual Black search finishes and its existing200ms thinking envelope retires");
    uint8_t from = 64U, to = 64U;
    const bool revealed = session.HintDisplayRequested() && session.TryGetHintMoveSquares(from, to);
    results.Check(revealed == blackCoaching && (blackCoaching || std::strcmp(home["as"] | "", "2o") == 0) &&
                      ControlBankLit(session, false, false) && ControlBankLit(session, true, blackCoaching),
                  "completed Black result retires thinking while retaining only actual Coaching status");
    bool expectedControls=true;
    for(uint8_t role=0U;role<6U;++role) {
        const auto c=session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(true,static_cast<hardware::ControlLedRole>(role))];
        expectedControls&=blackCoaching&&role==0U?c.r>0U&&c.b>c.g:CueEnergy(c)==0U;
    }
    results.Check(expectedControls,"completed Black Coaching leaves only violet O; all former thinking directions are dark");
    Checkpoint(results, session, engine, RUN, "Black-analysis-Done", blackCoaching ? "consider-own-Coaching-Hint" : "Black-chooses-own-move");
    if (!blackCoaching) {
        ++request.request_id;
        results.Check(session.DispatchControlAction(request, outcome) == VIRTUAL_LITCH_OK &&
                          outcome.status == modules::ControlMutationStatus::Applied && session.HintDisplayRequested() &&
                          session.TryGetHintMoveSquares(from, to),
                      "new Black O-equivalent Hint action can reveal its own completed cached suggestion");
        Checkpoint(results, session, engine, RUN, "Black-new-Hint-request", "consider-own-requested-Hint");
    }
    results.Check(engine.unsafeReads.load() == 0U, "handoff never reads the active worker engine lane");
}

void CheckThinkingControlFadeJourney(Results& results, const bool clockwise) {
    constexpr const char* PATH = "build/test_thinking_control_fade.json";
    constexpr const char* FEN = "r2n1rk1/ppp2p2/5p1p/2bPp3/2P5/5PP1/PP2N2P/2KR1B1R b - - 2 14";
    const char* run = clockwise ? "Classic/real-root/clockwise-thinking-fade" : "Classic/real-root/Foxie-thinking-fade";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH,false,true,false,false,FEN,0U,0U), "fade journey restores a real human turn");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker); engine.holdStart = 0U; engine.gateCandidates = true;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3; config.initial_time_ms = 1000U;
    Session session(config, runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, PATH, &engine, &worker);
    results.Check(Set(session,880U,"HINT_THINKING_STYLE", clockwise ? "\"player_clockwise\"" : "\"foxie_sweep\""),
                  "fade playback explicitly selects its named style without depending on the fresh default");
    results.Check(Observe(session,session.State().expected_occupied_mask), "production Hall occupancy enters the real saved turn");
    for (uint8_t i=0U;i<30U && engine.starts==0U;++i) (void)session.Advance(100U);
    const bool ready = WaitFor([&] {return engine.readyCandidateDepth.load()>=5U;});
    results.Check(ready && worker.IsActive(), "actual first completed root is held at its publication seam");
    if (!ready) {engine.CancelThink();(void)engine.Finish(session.NowMs());return;}
    const uint32_t start=engine.startedAtMs;
    results.Check(ControlBankLit(session,true,false), "hidden background search owns no thinking lights");
    virtual_litch_action_request request{};request.struct_size=sizeof(request);request.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    request.request_id=881U;request.action=VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome outcome;
    results.Check(session.DispatchControlAction(request,outcome)==VIRTUAL_LITCH_OK && outcome.status==modules::ControlMutationStatus::Applied,
        "actual typed Hint request acknowledges the current calculation");
    const auto energy=[&] {uint32_t sum=0U;for (uint8_t i=4U;i<10U;++i) {const auto c=session.PhysicalFrame().pixels[256U+i];sum+=c.r+c.g+c.b+c.w;}return sum;};
    results.Check(energy()==0U, "first requested frame starts from dark instead of snapping on");
    ControlCheckpoint(session,run,"request-zero","wait-for-current-position-Hint");
    (void)session.Advance(50U);const auto quarter=energy();
    ControlCheckpoint(session,run,"fade50ms","wait-for-current-position-Hint");
    (void)session.Advance(50U);const auto half=energy();
    ControlCheckpoint(session,run,"fade100ms","wait-for-current-position-Hint");
    (void)session.Advance(100U);const auto full=energy();
    results.Check(quarter>0U && half>quarter && full>half, "mapped controls fade in progressively over200ms");
    ControlCheckpoint(session,run,"fade200ms","wait-for-current-position-Hint");
    results.Check(engine.ReleaseCandidate(5U,start+500U) && session.Advance(start+500U-session.NowMs())==VIRTUAL_LITCH_OK,
        "real Full5 publication reaches its existing500ms reveal boundary");
    uint8_t from=64U,to=64U;
    results.Check(session.HintDisplayRequested() && session.TryGetHintMoveSquares(from,to) && worker.IsActive() && energy()>0U,
        "first usable move leaves the thinking controls active while analysis improves");
    ControlCheckpoint(session,run,"usable-Hint-search-active","consider-Hint-or-wait-for-refinement");
    const bool next=WaitFor([&] {return engine.readyCandidateDepth.load()>=6U;});
    results.Check(next,"real Full6 improvement reaches publication seam");
    if (!next) {engine.CancelThink();(void)engine.Finish(session.NowMs());return;}
    results.Check(engine.ReleaseCandidate(6U,start+600U) && session.Advance(100U)==VIRTUAL_LITCH_OK && energy()>0U && worker.IsActive(),
        "later actual improvement cannot end or restart the thinking cue");
    ControlCheckpoint(session,run,"better-Hint-search-active","consider-latest-Hint");
    const bool parked=WaitFor([&] {return engine.readyCandidateDepth.load()>=7U;});
    results.Check(parked,"real subsequent root waits while the position window reaches its ceiling");
    if (!parked) {engine.CancelThink();(void)engine.Finish(session.NowMs());return;}
    // Controlled dropped-release fault at the existing normalized event seam.
    // Raw held/repeat/release coverage remains in CheckRawThinkingControlJourney.
    results.Check(Set(session,884U,"P2_END_PIECE_WHEEL","84"),"dropped-release fixture starts one actual palette step before pure green");
    results.Check(session.DispatchNormalizedButton(VIRTUAL_LITCH_OWNER_P2,
                      VIRTUAL_LITCH_BUTTON_UP, VIRTUAL_LITCH_BUTTON_PRESS_EVENT) == VIRTUAL_LITCH_OK,
                  "production palette owner accepts a press whose release will be lost");
    results.Check(Set(session, 882U, "HINT_THINKING_STYLE", "\"none\""),
                  "None isolates the finite V3.3 palette acknowledgement from the background thinking animation");
    const auto upIndex=256U+hardware::V3_3PhysicalIndexForControl(true,hardware::ControlLedRole::Up);
    const auto onlyUp=[&] {
        const auto upColor=session.PhysicalFrame().pixels[upIndex];
        bool okay=upColor.g>0U&&upColor.r==0U&&upColor.b==0U&&
            session.SettingsForPersistenceTest().ReadValue("P2_END_PIECE_WHEEL",0U)==85U;
        for(uint8_t role=0U;role<6U;++role) {
            const auto index=256U+hardware::V3_3PhysicalIndexForControl(true,static_cast<hardware::ControlLedRole>(role));
            if(index!=upIndex)okay&=CueEnergy(session.PhysicalFrame().pixels[index])==0U;
        }
        return okay;
    };
    results.Check(onlyUp()&&session.Advance(75U)==VIRTUAL_LITCH_OK&&onlyUp(),
                  "a lost V3.3 release retains only the accepted Up acknowledgement through its finite fade");
    ControlCheckpoint(session,run,"lost-release-ack75ms","wait-for-bounded-recovery");
    results.Check(session.Advance(75U)==VIRTUAL_LITCH_OK&&!ControlBankLit(session,true,true)&&worker.IsActive(),
                  "at150ms the lost release cannot latch any V3.3 control light");
    results.Check(Set(session,883U,"HINT_THINKING_STYLE",clockwise?"\"player_clockwise\"":"\"foxie_sweep\"")&&
                      session.Advance(750U)==VIRTUAL_LITCH_OK&&energy()>0U&&worker.IsActive(),
                  "the still-current thinking request resumes without restarting its engine or presentation lease");
    ControlCheckpoint(session,run,"lost-release-recovered900ms","consider-latest-Hint");
    (void)session.Advance(start+89999U-session.NowMs());
    results.Check(energy()>0U && worker.IsActive(), "thinking stays visible through the last millisecond of its90s position window");
    results.Check(engine.Finish(start+90000U) && session.Advance(1U)==VIRTUAL_LITCH_OK, "actual engine terminates at the configured position deadline");
    modules::ThinkDiagnostics diagnostic;
    results.Check(ReadDiagnostics(session,modules::ThinkRequestKind::Hint,diagnostic) && !diagnostic.active &&
        diagnostic.state==modules::ThinkState::Done && energy()>0U, "terminal result begins a visible fade without dropping usable guidance");
    const auto terminal=energy();
    ControlCheckpoint(session,run,"terminal-fade-start","follow-final-Hint");
    (void)session.Advance(100U);const auto outgoing=energy();
    results.Check(outgoing>0U && outgoing<terminal, "terminal cue fades halfway while move guidance remains available");
    (void)session.Advance(100U);
    results.Check(energy()==0U && session.TryGetHintMoveSquares(from,to) && session.HintDisplayRequested(),
        "thinking retires after200ms without removing the final move guidance");
    ControlCheckpoint(session,run,"terminal-fade-complete","follow-final-Hint");
    results.Check(engine.unsafeReads.load()==0U,"fade and completion never read the live worker engine lane");
    std::printf("[THINKING_FADE] run=%s actual_engine=1 controlled_root_gate=1 deadline_ms=90000 hidden=quiet fade_in=200 fade_out=200 physical_timing_claim=0\n",run);
}

void CheckSearchCancellation(Results& results) {
    for (const bool benchmark : {false, true}) {
        modules::LitchMaxEngine engine;
        engine.Reset();
        results.Check(engine.SetFen(modules::INITIAL_FEN), "search cancellation uses a valid production root");
        struct Cancellation {
            modules::LitchMaxEngine& engine;
            uint32_t firstNodes = 0U;
        } cancellation{engine};
        auto config = modules::MakeThinkConfigForVsCpuLevel(8U, 1U);
        config.progressObserver = {[](void* context, const uint32_t nodes) {
            auto& request = *static_cast<Cancellation*>(context);
            if (nodes > 0U && request.firstNodes == 0U) {
                request.firstNodes = nodes;
                request.engine.CancelThink();
            }
        }, &cancellation};
        results.Check(benchmark ? engine.StartBenchmark(1U, config) : engine.StartThink(config),
                      "real normal or benchmark search starts before cancellation");
        uint8_t progress = 0U;
        modules::UciMove move;
        const auto state = engine.PollThinkProgress(progress, move);
        modules::ThinkStats stats;
        results.Check(cancellation.firstNodes > 0U && state == modules::ThinkState::Canceled &&
                          move.Empty() && engine.GetLastThinkStats(stats) &&
                          stats.stopReason == modules::ThinkStopReason::Canceled &&
                          stats.nodeCount >= cancellation.firstNodes,
                      "in-search cancellation discards the move and reports actual completed work");
    }
}

void CheckUrgentDuckNewClassic(Results& results, bool fragmented, bool afterDuck) {
    constexpr const char* PATH="build/urgent_fragmented_classic.json";
    constexpr const char* FEN="rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq d3 0 1";
    const char* run=fragmented?"urgent/Duck-NewClassic/fragmented":"urgent/Duck-NewClassic/contiguous-control";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH,afterDuck,false,false,false,modules::INITIAL_FEN,0U,0U,!afterDuck),"urgent real Duck setup persisted");
    modules::AsyncThinkWorker worker;GatedEngine engine(worker);engine.holdStart=UINT32_MAX;
    virtual_litch_config config{};virtual_litch_default_config(&config);config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.initial_time_ms=1000U;
    Session session(config,runtime::virtual_litch_internal::SessionMode::EngineBackedJourney,PATH,&engine,&worker);
    results.Check(Observe(session,session.State().expected_occupied_mask),"urgent raw Duck occupancy");
    if (afterDuck) {
    for(uint8_t i=0U;i<30U&&!worker.IsActive();++i)(void)session.Advance(100U);
    results.Check(WaitFor([&]{return engine.pollEntered.load();})&&engine.Finish(session.NowMs()),"urgent actual Duck Run ends before NewGame");
    (void)session.Advance(0U);ControlCheckpoint(session,run,"Duck-search-cleaned","NewGame");
    virtual_litch_action_request request{};request.struct_size=sizeof(request);request.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;request.request_id=920U;request.action=VIRTUAL_LITCH_CONTROL_NEW_GAME;
    modules::ControlMutationOutcome outcome;
    results.Check(CompleteAction(session,request,outcome)&&outcome.status==modules::ControlMutationStatus::Applied,"urgent production NewGame applied once");
    }
    results.Check(Set(session,921U,"VS_CPU_LEVEL","8")&&Set(session,922U,"VS_CPU_PLAYER","2"),"urgent CPU8 black selected via typed owner");
    results.Check(Observe(session,0U)&&Observe(session,UINT64_C(0xffff00000000ffff))&&session.Advance(2200U)==VIRTUAL_LITCH_OK,"urgent raw 32-piece fresh setup is accepted before variant latch");
    uint64_t occupancy=UINT64_C(0xffff00000000ffff)&~(UINT64_C(1)<<51U);
    results.Check(Observe(session,occupancy),"urgent physical d2 pickup");
    if(worker.IsActive()) results.Check(engine.Finish(session.NowMs()),"urgent old human analysis drains after pickup");
    (void)session.Advance(1U);
    occupancy|=UINT64_C(1)<<35U;
    results.Check(Observe(session,occupancy),"urgent physical d4 placement");
    for(uint8_t i=0U;i<50U;++i) {
        (void)session.Advance(100U);
        if(worker.IsActive()&&!engine.continuousHint)break;
        if(worker.IsActive())(void)engine.Finish(session.NowMs());
    }
    const bool admitted=WaitFor([&]{return worker.IsActive()&&engine.pollEntered.load()&&!engine.continuousHint;});
    results.Check(admitted&&std::strcmp(session.HomeStateSnapshot().fen,FEN)==0&&occupancy==UINT64_C(0xfff700080000ffff)&&session.GameVariant()==modules::GameVariant::Classic,"urgent real CPU request matches witnessed d2d4 FEN and occupancy");
    if (!admitted) {
        engine.CancelThink();
        (void)engine.FinishFiniteSearch(session.NowMs());
        return;
    }
    ControlCheckpoint(session,run,"CPU8-admitted","wait-for-calculated-reply");
    litchcraft_allocation_probe::ResetTopology(fragmented);
    const bool finished=engine.FinishFiniteSearch(session.NowMs());
    litchcraft_allocation_probe::topologyEnabled=false;
    results.Check(finished&&!worker.IsActive(),"urgent Run reaches quiescence");
    (void)session.Advance(0U);modules::ThinkDiagnostics data;
    const bool diagnostic=ReadDiagnostics(session,modules::ThinkRequestKind::Cpu,data);
    const bool useful=session.State().required_action!=0U;
    results.Check(diagnostic&&data.effectiveValid&&data.pollValid&&data.finalNodesValid,"urgent actual Poll and profile diagnostics available");
    results.Check(data.state==modules::ThinkState::Done&&data.finalNodes>0U&&data.completedRootCount>0U&&useful,
                  "fresh Classic produces a calculated physical CPU reply with fragmented or contiguous heap");
    if (!afterDuck) results.Check(data.requested.difficulty==3U&&data.requested.maxThinkMs==10500U&&data.requested.maxNodes==180000U&&data.requested.maxDepth==4U,
        "fresh retained Classic opening matches the exact physical CPU8 ramp profile");
    std::printf("[URGENT_RESULT] fragmented=%u after_Duck=%u actual_raw_d2d4_CPU8=1 controlled_holes=1 firmware_heap_claim=0 state=%u nodes=%u roots=%u publications=%u first_publication=%u poll=%u effective=%u allocations=%zu accepted_payload=%zu max_request=%zu requested_level=%u time=%u nodecap=%u depth=%u useful=%u unsafe_reads=%u\n",
        fragmented,afterDuck,static_cast<unsigned>(data.state),data.finalNodes,data.completedRootCount,data.publicationCount,data.firstPublicationValid,data.pollValid,data.effectiveValid,
        litchcraft_allocation_probe::calls,litchcraft_allocation_probe::acceptedBytes,litchcraft_allocation_probe::largestRequest,
        data.requested.difficulty,data.requested.maxThinkMs,data.requested.maxNodes,data.requested.maxDepth,useful,engine.unsafeReads.load());
    ControlCheckpoint(session,run,"CPU-terminal",useful?"follow-calculated-reply":"no-calculated-reply");
    results.Check(engine.unsafeReads.load()==0U,"urgent no live-engine read or mutation");
}

void CheckKingResignationSearchJourney(Results& results, const bool cpu) {
    constexpr const char* path = "build/king_resignation_owned_search.json";
    s_nowMs.store(1000U);
    results.Check(SaveFixture(path, false, false, false, false, nullptr, 0U, cpu ? 3U : 0U),
                  "resignation worker journey restores real acknowledged position");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    engine.holdStart = UINT32_MAX;
    engine.releaseStats.store(false);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.initial_time_ms = 1000U;
    runtime::VirtualLitchSession session(config, engine, worker, path);
    results.Check(session.ObserveHallMask(CHESS_OCCUPANCY) == VIRTUAL_LITCH_OK,
                  "resignation search journey enters through raw full Hall scan");
    for (uint8_t i = 0U; i < 30U && !worker.IsActive(); ++i) (void)session.Advance(100U);
    const bool admitted = WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && worker.IsActive(), "actual Hint or CPU worker owns engine before king lift");
    if (!admitted) { engine.releaseStats.store(true); (void)engine.Finish(session.State().now_ms); return; }
    const uint64_t lifted = CHESS_OCCUPANCY & ~(UINT64_C(1) << 4U);
    results.Check(session.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK && session.Advance(5000U) == VIRTUAL_LITCH_OK,
                  "off-turn black king arms while actual worker remains parked");
    const uint8_t circle = hardware::V3_3PhysicalIndexForControl(true, hardware::ControlLedRole::O);
    bool onlyCircle = true;
    for (bool p2 : {false, true}) {
        const auto range = p2 ? hardware::V3_3_P2_CONTROL_LED_RANGE : hardware::V3_3_P1_CONTROL_LED_RANGE;
        for (uint8_t i = 0U; i < range.count; ++i)
            onlyCircle &= (CueEnergy(session.PhysicalFrame().pixels[256U + range.first + i]) > 0U) == (range.first + i == circle);
    }
    results.Check(onlyCircle && !session.CpuThinkingVisible(),
                  "armed actual black Circle owns physical controls above worker thinking cues");
    ControlCheckpoint(session, cpu ? "resign/CPU" : "resign/Hint", "armed", "fresh-black-Circle");
    results.Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P2, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
                      session.Advance(20U) == VIRTUAL_LITCH_OK &&
                      session.SetButtonDown(VIRTUAL_LITCH_OWNER_P2, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
                      session.Advance(20U) == VIRTUAL_LITCH_OK && session.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK,
                  "raw owning Circle then fresh unchanged Hall scan confirms resignation");
    results.Check(session.HomeStateSnapshot().outcome.reason == modules::VariantOutcomeReason::Resigned &&
                      session.HomeStateSnapshot().outcome.winner == modules::GameWinner::White && worker.IsActive() && engine.unsafeReads.load() == 0U,
                  "explicit resignation latches without reading the owned mutable engine");
    engine.releasePoll.store(true);
    results.Check(WaitFor([&] { return engine.statsEntered.load(); }) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      worker.IsActive() && engine.unsafeReads.load() == 0U,
                  "final statistics publication keeps terminal cleanup deferred and engine lane safe");
    engine.releaseStats.store(true);
    results.Check(engine.Finish(session.State().now_ms) && session.Advance(1U) == VIRTUAL_LITCH_OK &&
                      !session.HomeStateSnapshot().physicalMoveValid && !session.HintDisplayRequested() && engine.starts == 1U && engine.unsafeReads.load() == 0U,
                  "actual worker retirement abandons late results without restarted search or executable move");
    modules::GamePersistenceState durable;
    modules::DuckGameDurableState variant;
    results.Check(modules::ReadGamePersistenceState(session.SettingsForPersistenceTest().Doc(), durable) &&
                      modules::DecodeDuckGameDurableState(durable.variantState.CStr(), variant) &&
                      variant.outcome.reason == modules::VariantOutcomeReason::Resigned && !durable.cpuMoveValid && durable.interactionState.Empty(),
                  "quiesced resignation is durable with no executable worker or interaction continuation");
    ControlCheckpoint(session, cpu ? "resign/CPU" : "resign/Hint", "quiesced", "Undo-or-New-Game");
}

enum class FollowupPlacementTiming : uint8_t { AfterAck, BeforeAckE5, BeforeAckIllegalF5 };

void CheckRapidSettleSearchHandoff(Results& results, const bool duck,
                                  const char* policy, const uint32_t duration,
                                  const char* style,
                                  const FollowupPlacementTiming timing = FollowupPlacementTiming::AfterAck) {
    constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
    constexpr uint64_t E7_DESTINATIONS = (UINT64_C(1) << 20U) | (UINT64_C(1) << 28U);
    const bool beforeAck = timing != FollowupPlacementTiming::AfterAck;
    const uint8_t target = timing == FollowupPlacementTiming::BeforeAckIllegalF5 ? 29U : 28U;
    s_nowMs.store(1000U);
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.initial_time_ms = 1000U;
    runtime::VirtualLitchSession session(config, engine, worker);
    uint64_t occupancy = OPENING | (duck ? UINT64_C(1) << 24U : 0ULL);
    const auto observe = [&](const uint64_t next) {
        occupancy = next;
        return session.ObserveHallMask(next) == VIRTUAL_LITCH_OK;
    };
    const auto redPerimeter = [&](const bool physical) {
        for (uint8_t index = 0U; index < 32U; ++index) {
            const auto color = physical ? session.PhysicalFrame().pixels[
                256U + hardware::V3_3PhysicalIndexForSemanticStatus(index)]
                : session.SemanticFrame().status[index];
            if (color.r == 0U || color.g != 0U || color.b != 0U || color.w != 0U) return false;
        }
        return true;
    };
    const auto healthy = [&] {
        StaticJsonDocument<1536> home;
        return session.State().human_illegal_mask == 0ULL &&
            session.State().correction_mask == 0ULL &&
            !redPerimeter(false) && !redPerimeter(true) &&
            ReadHome(session, home) && std::strcmp(home["a"] | "", "corr") != 0;
    };
    const auto moverPalette = [&] {
        const auto piece = session.SemanticFrame().board[target][1U];
        if (CueEnergy(piece) == 0U || !CueSameHue(piece, session.SemanticFrame().board[11U][1U])) return false;
        for (uint8_t slot : {1U, 3U}) {
            if (!CueSameHue(session.PhysicalFrame().pixels[target * 4U + slot],
                          session.PhysicalFrame().pixels[11U * 4U + slot])) return false;
        }
        return true;
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        const auto& state = session.State();
        const auto square = session.SemanticFrame().board[target][0U];
        const auto piece = session.SemanticFrame().board[target][1U];
        std::printf("[RAPID_ASYNC_SETTLE] variant=%s policy=%s style=%s stage=%s t=%u "
                    "occupancy=%016llx expected=%016llx phase=%s action=%u token=%u "
                    "side=%u history=%u illegal=%016llx correction=%016llx "
                    "target=%u square=%u/%u/%u piece=%u/%u/%u text=%.*s next=%s home=%s\n",
                    duck ? "Duck" : "Classic", policy, style, stage, state.now_ms,
                    static_cast<unsigned long long>(state.physical_occupied_mask),
                    static_cast<unsigned long long>(state.expected_occupied_mask),
                    runtime::ChessInteractionPhaseToString(
                        static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                    state.required_action, state.pending_action_token, state.side_to_move_black,
                    session.UndoHistoryMoveCount(),
                    static_cast<unsigned long long>(state.human_illegal_mask),
                    static_cast<unsigned long long>(state.correction_mask),
                    target, square.r, square.g, square.b, piece.r, piece.g, piece.b,
                    static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
                    next, session.HomeStateLine().CStr());
    };
    results.Check(session.Reset() == VIRTUAL_LITCH_OK && observe(occupancy) &&
                      Set(session, 920U, "MOVE_CONFIRM", policy) &&
                      Set(session, 921U, "SETTLE_ANIM_STYLE", style) &&
                      session.Advance(200U) == VIRTUAL_LITCH_OK,
                  "rapid async journey starts from a raw Classic/Duck opening and real settings");
    const bool admitted = engine.starts == 1U &&
        WaitFor([&] { return engine.pollEntered.load(); });
    results.Check(admitted && worker.IsActive(),
                  "rapid async journey holds the actual background Hint worker at Poll");
    if (!admitted) { engine.CancelThink(); (void)engine.Finish(session.State().now_ms); return; }
    results.Check(observe(occupancy & ~(UINT64_C(1) << 52U)) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK &&
                      observe(occupancy | (UINT64_C(1) << 36U)) &&
                      session.Advance(100U) == VIRTUAL_LITCH_OK &&
                      session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling),
                  "raw e2 pickup and e4 placement begin the full configured human settle");
    const uint8_t interruptSource = duck ? 24U : 12U;
    results.Check(observe(occupancy & ~(UINT64_C(1) << interruptSource)) &&
                      session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::AwaitingAction) &&
                      session.UndoHistoryMoveCount() == 0U && healthy(),
                  "early next-action pickup remains neutral while prior move awaits worker cancellation");
    checkpoint("pickup-awaiting-cancel", duck ? "place-required-Duck-after-ack" : "place-e7-on-e6-or-e5-after-ack");
    const uint32_t token = session.State().pending_action_token;
    for (uint8_t frame = 0U; frame < 5U; ++frame) {
        results.Check(session.Advance(20U) == VIRTUAL_LITCH_OK && healthy() &&
                          session.State().pending_action_token == token && worker.IsActive(),
                      "every held cancellation frame preserves one action without false correction");
    }
    engine.releaseStats.store(false);
    engine.releasePoll.store(true);
    const bool cleaning = WaitFor([&] { return engine.statsEntered.load(); });
    results.Check(cleaning && session.Advance(20U) == VIRTUAL_LITCH_OK &&
                      worker.IsActive() && healthy() && session.UndoHistoryMoveCount() == 0U,
                  "worker statistics cleanup also preserves the pending pickup without old-turn rejection");
    checkpoint("pickup-awaiting-cleanup", "continue-held-physical-action");
    if (beforeAck) {
        results.Check(observe(occupancy | (UINT64_C(1) << target)),
                      "complete next move is physically observed before the previous acknowledgement");
        for (uint8_t frame = 0U; frame < 100U; ++frame) {
            const bool advanced = session.Advance(20U) == VIRTUAL_LITCH_OK;
            bool noAmberFault = true;
            for (const uint8_t slot : {0U, 2U}) {
                noAmberFault &= !AmberTint(session.SemanticFrame().board[target][slot]) &&
                    !AmberTint(session.PhysicalFrame().pixels[target * 4U + slot]);
            }
            results.Check(advanced && healthy() && noAmberFault && worker.IsActive() &&
                              session.State().pending_action_token == token && session.UndoHistoryMoveCount() == 0U,
                          "complete next-side observation stays neutral beyond amber/red thresholds while cleanup owns the engine");
            if (frame == 14U || frame == 74U || frame == 99U) {
                checkpoint("destination-before-ack-held", "wait-for-authoritative-ack");
            }
        }
    }
    engine.releaseStats.store(true);
    results.Check(engine.Finish(session.State().now_ms) && session.Advance(1U) == VIRTUAL_LITCH_OK,
                  "actual worker retirement permits exactly one authoritative move acknowledgement");
    if (duck) {
        results.Check(session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::DuckRelocation) &&
                          session.UndoHistoryMoveCount() == 0U && healthy() &&
                          observe(occupancy | (UINT64_C(1) << 25U)) &&
                          session.Advance(100U) == VIRTUAL_LITCH_OK &&
                          session.UndoHistoryMoveCount() == 0U &&
                          observe(occupancy & ~(UINT64_C(1) << 12U)),
                      "Duck requires its chess-half acknowledgement and legal Duck placement before next-side pickup");
    }
    if (!beforeAck) {
        results.Check(session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                          session.State().selected_square == 12U && session.State().side_to_move_black != 0U &&
                          session.State().valid_destination_mask == E7_DESTINATIONS &&
                          session.UndoHistoryMoveCount() == 1U && healthy(),
                      "acknowledged e2e4 replays e7 once and immediately admits both e6/e5 legal destinations");
        checkpoint("acknowledged-e7-held", "choose-e6-or-e5");
        const bool held = session.Advance(100U) == VIRTUAL_LITCH_OK;
        for (uint8_t slot : {0U, 2U}) {
            const auto semantic = session.SemanticFrame().board[20U][slot];
            const auto physical = session.PhysicalFrame().pixels[4U * 20U + slot];
            results.Check(AmberTint(semantic) && AmberTint(physical),
                          "the first reveal band lights both mapped rails of the nearer e6 destination");
        }
        const bool placed = observe(occupancy | (UINT64_C(1) << 28U));
        results.Check(held && placed && healthy() && moverPalette() &&
                          session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling),
                      "Black can place on legal e5 during the reveal and starts its own settle without stale e7/e5 errors");
    } else {
        results.Check(session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Settling) &&
                          session.State().side_to_move_black != 0U && session.UndoHistoryMoveCount() == 1U &&
                          healthy() && moverPalette(),
                      "acknowledgement replays the complete next move into its own ordinary confirmation lifecycle");
    }
    checkpoint("next-destination-owned", duck ? "move-required-Duck" : "wait-for-Black-settle");
    if (timing == FollowupPlacementTiming::BeforeAckIllegalF5) {
        for (uint32_t elapsed = 0U; elapsed < duration; elapsed += 20U) {
            results.Check(session.Advance(20U) == VIRTUAL_LITCH_OK,
                          "invalid next destination reaches the ordinary authoritative action boundary");
        }
        StaticJsonDocument<1536> rejectedHome;
        results.Check(session.UndoHistoryMoveCount() == 1U && session.State().side_to_move_black != 0U &&
                          session.State().correction_mask == ((UINT64_C(1) << 12U) | (UINT64_C(1) << target)) &&
                          session.State().human_illegal_mask != 0ULL && ReadHome(session, rejectedHome) &&
                          std::strcmp(rejectedHome["fen"] | "", "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1") == 0,
                      "engine rejects illegal e7f5 after acknowledgement without committing a second move");
        checkpoint("illegal-next-destination-rejected", "return-f5-piece-to-e7");
        const bool liftedInvalid = observe(occupancy & ~(UINT64_C(1) << target));
        const bool returnedSource = observe(occupancy | (UINT64_C(1) << 12U));
        results.Check(liftedInvalid && returnedSource && session.Advance(200U) == VIRTUAL_LITCH_OK &&
                          healthy() && session.UndoHistoryMoveCount() == 1U && engine.unsafeReads.load() == 0U,
                      "restoring the truly illegal destination recovers the acknowledged Black turn without a duplicate move");
        checkpoint("illegal-next-destination-recovered", "Black-next-move");
        engine.CancelThink();
        results.Check(engine.Finish(session.State().now_ms), "illegal followup journey drains the background worker");
        return;
    }
    if (duck) {
        for (uint8_t frame = 0U; frame < 5U; ++frame) {
            results.Check(session.Advance(20U) == VIRTUAL_LITCH_OK && healthy() && moverPalette(),
                          "Duck's chess destination retains Black's palette until required Duck pickup ends that settle");
        }
        results.Check(observe(occupancy & ~(UINT64_C(1) << 25U)) && healthy() &&
                          observe(occupancy | (UINT64_C(1) << 24U)) && healthy(),
                      "Black also completes the mandatory Duck relocation without bypassing the compound turn");
    }
    for (uint32_t elapsed = 0U; elapsed < duration; elapsed += 20U) {
        results.Check(session.Advance(20U) == VIRTUAL_LITCH_OK && healthy(),
                      "all post-e5 frames through bloom, settling and expiry remain free of false correction");
        if (!duck && session.UndoHistoryMoveCount() < 2U) {
            results.Check(moverPalette(), "e5 keeps the same mover palette as the other Black pieces throughout settling");
        }
    }
    StaticJsonDocument<1536> home;
    results.Check(session.UndoHistoryMoveCount() == 2U &&
                      session.State().side_to_move_black == 0U &&
                      session.State().expected_occupied_mask == occupancy && healthy() &&
                      ReadHome(session, home) &&
                      std::strcmp(home["fen"] | "", "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2") == 0 &&
                      engine.unsafeReads.load() == 0U,
                  "both exact e2e4/e7e5 turns commit once with correct FEN, occupancy and engine-lane ownership");
    checkpoint("both-turns-committed", "White-next-move");
    const bool invalidPickup = observe(occupancy & ~(UINT64_C(1) << 1U));
    const bool rejected = session.State().human_illegal_mask != 0ULL &&
                          session.UndoHistoryMoveCount() == 2U;
    results.Check(session.Advance(modules::CORRECTION_STRONG_MS) == VIRTUAL_LITCH_OK &&
                      redPerimeter(false) && redPerimeter(true),
                  "a real invalid pickup still reaches strong red feedback on semantic and mapped perimeter LEDs");
    checkpoint("real-invalid-pickup-strong-red", "return-b8");
    const bool restored = observe(occupancy | (UINT64_C(1) << 1U));
    const bool recovered = session.Advance(200U) == VIRTUAL_LITCH_OK && healthy();
    results.Check(invalidPickup && rejected && restored && recovered,
                  "ordinary wrong-side b8 pickup is still rejected and physically returning it recovers");
    checkpoint("real-invalid-pickup-recovered", "White-next-move");
    engine.CancelThink();
    results.Check(engine.Finish(session.State().now_ms), "rapid async journey drains its background worker");
}

enum class CauldronContext { ManualOnly, CoachingAndPairing };

void CheckCauldronBrewingJourney(Results& results, const bool black, const bool rotate,
                               const CauldronContext context = CauldronContext::ManualOnly) {
    constexpr const char* PATH="build/cauldron_brewing_owned_search.json";
    constexpr const char* WHITE="r2n1rk1/ppp2p2/5p1p/2bPp3/2P5/5PP1/PP2N2P/2KR1B1R w - - 2 14";
    constexpr const char* BLACK="r2n1rk1/ppp2p2/5p1p/2bPp3/2P5/5PP1/PP2N2P/2KR1B1R b - - 2 14";
    fxe::ScopedMillisProviderOverride clock(&AtomicMillis);
    s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH,false,black,false,rotate,black?BLACK:WHITE,0U,0U),
        "Cauldron restores a genuine human position with factory/unset Hint style");
    modules::AsyncThinkWorker worker;
    GatedEngine engine(worker);engine.holdStart=UINT32_MAX;engine.gateCandidates=true;
    // Normal authenticated-owner facts enter the existing runtime seam. No
    // native radio or pairing-persistence mutation is needed in this journey.
    struct PairedPersistence final : modules::BoardOwnerPersistence {
        unsigned saves = 0U;
        bool SaveOwnerRecord(modules::BoardOwnerRecord, Receipt&) override { ++saves; return false; }
        Outcome GetOutcome(Receipt) const override { return Outcome::Unknown; }
        bool Release(Receipt) override { return false; }
    } pairedPersistence;
    modules::BoardOwnerPairingRuntime pairing(pairedPersistence);
    constexpr modules::BoardOwnerPairingRuntime::Connection OWNER{10U, 20U, 30U};
    const bool combined = context == CauldronContext::CoachingAndPairing;
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.initial_time_ms=1000U;
    runtime::VirtualLitchSession session(config,engine,worker,PATH);
    if (combined) {
        results.Check(!black && !rotate, "the additional combined context is exactly P1/unrotated");
        pairing.Boot(modules::BoardOwnerRecord::Active, {true, 1U}, true);
        results.Check(pairing.RegisterOwnerConnection(OWNER, {{true, 1U}, true, true, true, true}) &&
            pairing.OwnerReady(OWNER), "existing checked owner-registration seam supplies a normal paired runtime");
        session.BindOwnerPairingRuntime(pairing);
    }
    const bool physicalP2=black!=rotate;
    const uint8_t bank=physicalP2?VIRTUAL_LITCH_OWNER_P2:VIRTUAL_LITCH_OWNER_P1;
    const auto rolePixel=[&](hardware::ControlLedRole role) {
        return session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(physicalP2,role)];
    };
    const auto edge=[&](runtime::VirtualLitchPhysicalButton button,bool down) {
        results.Check(session.SetButtonDown(bank,button,down)==VIRTUAL_LITCH_OK&&session.Advance(30U)==VIRTUAL_LITCH_OK,
            "Cauldron uses raw player-bank levels, production debounce and current-turn authority");
    };
    const auto tap=[&] {edge(runtime::VirtualLitchPhysicalButton::O,true);edge(runtime::VirtualLitchPhysicalButton::O,false);};
    const auto onlyO=[&](bool lit) {
        bool okay=true;
        for(bool p2:{false,true})for(uint8_t role=0U;role<6U;++role) {
            const auto color=session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(p2,static_cast<hardware::ControlLedRole>(role))];
            const bool expected=lit&&p2==physicalP2&&role==0U;
            okay=okay&&(expected?CueEnergy(color)>0U:CueEnergy(color)==0U);
        }
        return okay;
    };
    const auto checkpoint=[&](const char* stage,const char* next) {
        const auto color=rolePixel(hardware::ControlLedRole::O);
        std::printf("[CAULDRON_ASYNC_JOURNEY] black=%u rotate=%u stage=%s t=%u physical_bank=%u occupancy=%016llx requested=%u worker=%u starts=%u O=%u/%u/%u next=%s\n",
            black,rotate,stage,session.State().now_ms,bank,
            static_cast<unsigned long long>(session.State().physical_occupied_mask),session.HintDisplayRequested(),
            worker.IsActive(),engine.starts,color.r,color.g,color.b,next);
        ControlCheckpoint(session,"Cauldron/raw-O/real-async",stage,next);
    };
    uint64_t occupancy=0ULL;
    for(uint8_t square=0U;square<64U;++square)
        if((session.State().expected_occupied_mask&(UINT64_C(1)<<square))!=0ULL)
            occupancy|=UINT64_C(1)<<modules::MapSquareToPhysical(square,rotate);
    results.Check(session.ObserveHallMask(occupancy)==VIRTUAL_LITCH_OK,"Cauldron raw Hall input matches saved position and orientation");
    for(uint8_t i=0U;i<30U&&!worker.IsActive();++i)(void)session.Advance(100U);
    const bool parked=WaitFor([&]{return engine.pollEntered.load();});
    results.Check(parked&&worker.IsActive()&&onlyO(false),"real hidden background Hint owns no control light before manual O");
    if(!parked){engine.CancelThink();(void)engine.Finish(session.State().now_ms);return;}
    results.Check(Set(session,939U,"BRIGHTNESS","8")&&Set(session,939U,"AUTO_BRIGHTNESS","false"),
        "Cauldron timed production output uses the actual full selected-brightness policy");
    virtual_litch_color coaching{};
    if (combined) {
        results.Check(Set(session,941U,"P1_AUTO_HINTS","true") && session.Advance(220U) == VIRTUAL_LITCH_OK,
            "Coaching is enabled through the real typed setting owner while the first Hint result remains held");
        coaching = rolePixel(hardware::ControlLedRole::O);
        uint8_t from = 64U, to = 64U;
        results.Check(onlyO(true) && coaching.b > coaching.r && coaching.b > coaching.g &&
            session.HintDisplayRequested() && !session.TryGetHintMoveSquares(from, to) &&
            worker.IsActive() && pairing.OwnerReady(OWNER) && !pairing.Presentation().active,
            "automatic Coaching owns the selected Cauldron O during analysis; normal pairing adds no cue");
        checkpoint("coaching-paired-no-manual-request", "short-O-for-current-Hint");
    }
    const uint32_t starts=engine.starts;
    tap();const uint32_t requestedAt=session.State().now_ms;
    results.Check(session.HintDisplayRequested(),"raw short O admits the real current-position Hint request");
    if (combined) {
        modules::ControlMutationOutcome outcome;
        uint8_t from = 64U, to = 64U;
        results.Check(session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.action == modules::ControlAction::Hint && outcome.status == modules::ControlMutationStatus::Applied &&
            !session.TryGetHintMoveSquares(from, to),
            "the raw O is an accepted manual Hint with no cached result, not an injected thinking flag");
    }
    (void)session.Advance(220U);
    results.Check(onlyO(true),"pending Cauldron lights only the requesting player's O after local ack and fade-in");
    const auto floor=rolePixel(hardware::ControlLedRole::O);
    (void)session.Advance(requestedAt+800U-session.State().now_ms);
    const auto peak=rolePixel(hardware::ControlLedRole::O);
    results.Check(CueEnergy(peak)>CueEnergy(floor)&&onlyO(true),"mapped O gently breathes rather than blinking or moving through other LEDs");
    checkpoint("pending-crest","wait-for-current-Hint-or-cancel");
    uint16_t minimum = 765U, maximum = 0U, largestStep = 0U, previous = CueEnergy(peak);
    uint8_t brightestChannel = 0U;
    for (uint32_t elapsed = 820U; elapsed <= 2420U; elapsed += 20U) {
        results.Check(session.Advance(20U) == VIRTUAL_LITCH_OK && onlyO(true) &&
            session.HintDisplayRequested() && worker.IsActive() && engine.starts == starts,
            "pending real Hint retains phase and current O authority at every renderer cadence");
        if (combined) {
            uint8_t from = 64U, to = 64U;
            results.Check(!session.TryGetHintMoveSquares(from, to) && pairing.OwnerReady(OWNER) &&
                !pairing.Presentation().active && pairedPersistence.saves == 0U &&
                session.State().physical_occupied_mask == occupancy &&
                session.State().expected_occupied_mask == occupancy,
                "every combined-context sample retains real pending Hint, normal owner authority and exact physical occupancy");
        }
        const auto color = rolePixel(hardware::ControlLedRole::O);
        const auto energy = CueEnergy(color);
        minimum = std::min(minimum, energy); maximum = std::max(maximum, energy);
        largestStep = std::max<uint16_t>(largestStep, energy > previous ? energy - previous : previous - energy);
        brightestChannel = std::max({brightestChannel, color.r, color.g, color.b});
        previous = energy;
        if (elapsed % 400U == 0U) checkpoint("pending-pulse-sample", "wait-for-current-Hint-or-cancel");
    }
    const uint8_t selectedCeiling = modules::ScaleU8SaturatingBy256(
        modules::brightness_scale_policy::USER_MAX_EFFECTIVE_SCALE256,
        hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3).ledOutputScale256);
    results.Check(brightestChannel == selectedCeiling && maximum > minimum * 8U && largestStep <= 48U,
        "actual pending Hint gently rises to full selected brightness and returns without resetting phase");
    std::printf("[CAULDRON_PULSE] black=%u rotate=%u samples=81 interval_ms=20 period_ms=1600 min_energy=%u max_energy=%u peak_channel=%u selected_ceiling=%u max_step=%u next=wait-or-cancel\n",
        black, rotate, minimum, maximum, brightestChannel, selectedCeiling, largestStep);

    if (combined) {
        engine.releasePoll.store(true);
        const bool rootReady = WaitFor([&] { return engine.readyCandidateDepth.load() >= 5U; });
        results.Check(rootReady, "combined context obtains a genuine completed first root through the existing held publication seam");
        if (rootReady) {
            results.Check(engine.ReleaseCandidate(5U, session.State().now_ms) && session.Advance(0U) == VIRTUAL_LITCH_OK,
                "the production owner publishes the real Hint result without a cosmetic delay");
            uint8_t from = 64U, to = 64U;
            results.Check(session.HintDisplayRequested() && session.TryGetHintMoveSquares(from, to),
                "the completed result becomes an actual displayed and cached Hint");
            for (unsigned sample = 0U; sample < 5U; ++sample) {
                results.Check(session.Advance(220U) == VIRTUAL_LITCH_OK && onlyO(true) &&
                    CueSameHue(rolePixel(hardware::ControlLedRole::O), coaching) &&
                    session.HintDisplayRequested() && worker.IsActive() && engine.starts == starts &&
                    pairing.OwnerReady(OWNER) && !pairing.Presentation().active && pairedPersistence.saves == 0U &&
                    session.State().physical_occupied_mask == occupancy,
                    "ready Hint retains selected thinking feedback while the same analysis and owner remain active");
            }
            checkpoint("coaching-paired-ready-steady", "follow-the-shown-Hint");
            tap(); (void)session.Advance(50U);
            uint8_t replayFrom = 64U, replayTo = 64U;
            results.Check(session.TryGetHintMoveSquares(replayFrom, replayTo) && replayFrom == from && replayTo == to &&
                !CueEqual(rolePixel(hardware::ControlLedRole::O), coaching) && engine.starts == starts,
                "a real cached-Hint retap gives only the finite local acknowledgement without restarting search");
            (void)session.Advance(220U);
            results.Check(onlyO(true) && worker.IsActive() && engine.starts == starts,
                "cached-Hint acknowledgement returns to the same continuing analysis feedback");
            std::printf("[CAULDRON_COMBINED] owner=P1 rotate=0 coaching=1 pairing=normal samples=81 pending_min=%u pending_max=%u ready_O=%u/%u/%u cache_retap=finite occupancy=%016llx next=follow-shown-Hint\n",
                minimum, maximum, coaching.r, coaching.g, coaching.b, static_cast<unsigned long long>(occupancy));
        }
        engine.CancelThink(); results.Check(engine.Finish(session.State().now_ms), "combined context drains its actual Hint worker");
        (void)session.Advance(0U);
        (void)session.Advance(220U);
        const auto steady = rolePixel(hardware::ControlLedRole::O);
        results.Check(onlyO(true) && steady.r > steady.g && steady.b > steady.r,
            "only completed analysis retires Cauldron and restores the steady violet Coaching badge");
        results.Check(engine.unsafeReads.load() == 0U, "combined context preserves production engine-lane ownership");
        RemoveFiles(PATH);
        return;
    }

    session.ShowIdentifyOverlay(1U,{255U,255U,255U});(void)session.Advance(20U);
    results.Check(onlyO(false),"Identify preempts the entire brew and its local ack");
    session.StopIdentifyOverlay();(void)session.Advance(20U);
    results.Check(onlyO(true),"Identify exit restores only still-authoritative pending work");
    const char* palette=black?"P2_END_PIECE_WHEEL":"P1_END_PIECE_WHEEL";
    results.Check(Set(session,940U,palette,"85"),"current selected player color changes through the real typed setting owner");
    (void)session.Advance(20U);
    const auto green=rolePixel(hardware::ControlLedRole::O);
    results.Check(green.g>0U&&green.r==0U&&green.b==0U&&onlyO(true),"ongoing brew follows current selected green on the same physical O");
    checkpoint("current-color-changed","wait-or-cancel");
    tap();(void)session.Advance(200U);
    results.Check(!session.HintDisplayRequested()&&onlyO(false)&&worker.IsActive()&&engine.starts==starts,
        "pending second O retires manual brew and bounded ack while ordinary background analysis stays quiet");
    checkpoint("pending-cancelled","fresh-O-or-ordinary-play");
    edge(runtime::VirtualLitchPhysicalButton::O,true);(void)session.Advance(1000U);edge(runtime::VirtualLitchPhysicalButton::O,false);
    (void)session.Advance(1200U);
    results.Check(session.SettingsForPersistenceTest().ReadValue(black?"P2_AUTO_HINTS":"P1_AUTO_HINTS",false),
        "long O retains the existing automatic-Coaching setting action instead of short Hint");
    const auto coached=rolePixel(hardware::ControlLedRole::O);
    results.Check(onlyO(true) && coached.g > coached.r && coached.g > coached.b && worker.IsActive(),
        "automatic Coaching uses the same selected Cauldron while its actual analysis continues");
    edge(runtime::VirtualLitchPhysicalButton::O,true);(void)session.Advance(1000U);edge(runtime::VirtualLitchPhysicalButton::O,false);
    (void)session.Advance(1200U);
    tap();(void)session.Advance(220U);
    results.Check(session.HintDisplayRequested()&&onlyO(true)&&engine.starts==starts,"fresh short O resumes manual feedback without a cosmetic search restart");
    engine.releasePoll.store(true);
    const bool rootReady=WaitFor([&]{return engine.readyCandidateDepth.load()>=5U;});
    results.Check(rootReady,"the production engine calculates a genuine completed root at the held publication seam");
    if(!rootReady){engine.CancelThink();(void)engine.Finish(session.State().now_ms);return;}
    results.Check(engine.ReleaseCandidate(5U,session.State().now_ms)&&session.Advance(0U)==VIRTUAL_LITCH_OK,
        "a real root becomes immediately publishable through the ordinary owner");
    uint8_t from=64U,to=64U;
    results.Check(session.HintDisplayRequested()&&session.TryGetHintMoveSquares(from,to),"ready move appears without a brew delay");
    (void)session.Advance(220U);
    results.Check(onlyO(true) && session.HintDisplayRequested() && worker.IsActive(),
        "a ready suggestion retains selected thinking feedback while actual analysis continues");
    checkpoint("ready-fade-complete","follow-the-shown-move");
    const bool nextRoot=WaitFor([&]{return engine.readyCandidateDepth.load()>=6U;});
    if(nextRoot)(void)engine.ReleaseCandidate(6U,session.State().now_ms);
    edge(runtime::VirtualLitchPhysicalButton::O, true);
    edge(runtime::VirtualLitchPhysicalButton::O, false);
    (void)session.Advance(220U);
    uint8_t cachedFrom=64U,cachedTo=64U;
    results.Check(!session.HintDisplayRequested() && session.TryGetHintMoveSquares(cachedFrom,cachedTo) &&
                      onlyO(false) && worker.IsActive() && engine.starts==starts,
                  "manual secondO hides guidance and its retiring cue while retaining the same active search and current cache");
    edge(runtime::VirtualLitchPhysicalButton::O, true);
    edge(runtime::VirtualLitchPhysicalButton::O, false);
    uint8_t shownFrom=64U,shownTo=64U;
    results.Check(session.HintDisplayRequested() && session.TryGetHintMoveSquares(shownFrom,shownTo) &&
                      shownFrom==cachedFrom && shownTo==cachedTo && worker.IsActive() && engine.starts==starts,
                  "manual thirdO restores the current qualified tuple without another worker or lost cache");
    results.Check(session.Advance(50U) == VIRTUAL_LITCH_OK &&
                      rolePixel(hardware::ControlLedRole::O).g>0U && onlyO(true),
                  "revealing the current cached move gives the selected-color cue during its finite nonzero fade");
    checkpoint("manual-off-on","follow-the-current-move");
    engine.CancelThink();results.Check(engine.Finish(session.State().now_ms),"Cauldron fixture releases actual worker cleanup");
    (void)session.Advance(0U);
    results.Check(session.LoadExternalFen(black?WHITE:BLACK),"a real external-position change invalidates the old request at the existing mutation owner");
    results.Check(!session.HintDisplayRequested()&&onlyO(false),"position invalidation wins immediately over replay and old O acknowledgement");
    checkpoint("position-invalidated","request-a-new-current-position-Hint");
    results.Check(engine.unsafeReads.load()==0U,"all real Hint search/result work retains the existing shared engine lane");
    RemoveFiles(PATH);
}

void CheckCauldronCpuThinking(Results& results,const bool black,const bool rotate) {
    constexpr const char* PATH="build/cauldron_cpu_thinking.json";
    fxe::ScopedMillisProviderOverride clock(&AtomicMillis);s_nowMs.store(1000U);
    results.Check(SaveFixture(PATH,false,black,false,rotate),"Cauldron CPU journey restores an actual CPU position with unset default style");
    modules::AsyncThinkWorker worker;GatedEngine engine(worker);engine.holdStart=UINT32_MAX;
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.initial_time_ms=1000U;
    runtime::VirtualLitchSession session(config,engine,worker,PATH);
    uint64_t occupancy=0ULL;
    for(uint8_t square=0U;square<64U;++square)
        if(session.State().expected_occupied_mask&(UINT64_C(1)<<square))
            occupancy|=UINT64_C(1)<<modules::MapSquareToPhysical(square,rotate);
    results.Check(session.ObserveHallMask(occupancy)==VIRTUAL_LITCH_OK,"CPU Cauldron uses production oriented Hall input");
    for(uint8_t i=0U;i<30U&&!worker.IsActive();++i)(void)session.Advance(100U);
    const bool admitted=WaitFor([&]{return engine.pollEntered.load();});
    results.Check(admitted&&worker.IsActive(),"default Cauldron CPU admits its actual worker");
    if(!admitted){engine.CancelThink();(void)engine.Finish(session.State().now_ms);return;}
    const bool physicalP2=black!=rotate;
    const uint8_t bank=physicalP2?VIRTUAL_LITCH_OWNER_P2:VIRTUAL_LITCH_OWNER_P1;
    const auto oPixel=[&] {
        return session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(physicalP2,hardware::ControlLedRole::O)];
    };
    const auto onlyO=[&](const bool lit) {
        bool okay=true;
        for(bool p2:{false,true})for(uint8_t role=0U;role<6U;++role) {
            const auto color=session.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(p2,static_cast<hardware::ControlLedRole>(role))];
            const bool expected=lit&&p2==physicalP2&&role==0U;
            okay&=expected?CueEnergy(color)>0U:CueEnergy(color)==0U;
        }
        return okay;
    };
    results.Check(Set(session,950U,"BRIGHTNESS","8") && Set(session,951U,"AUTO_BRIGHTNESS","false") &&
        Set(session,952U,black?"P2_END_PIECE_WHEEL":"P1_END_PIECE_WHEEL","85"),
        "CPU brewing uses the current selected player color and production brightness policy");
    (void)session.Advance(250U);const auto rising=oPixel();
    results.Check(onlyO(true) && rising.g>0U && rising.r==0U && rising.b==0U,
        "CPU Cauldron lights only its real mapped O in the current selected color without D-pad or other-bank leakage");
    (void)session.Advance(550U);const auto peak=oPixel();
    (void)session.Advance(800U);const auto trough=oPixel();
    results.Check(onlyO(true) && CueEnergy(peak)>CueEnergy(rising) && CueEnergy(peak)>CueEnergy(trough)*8U &&
        worker.IsActive() && engine.starts==1U && !session.HintDisplayRequested(),
        "actual CPU work follows the selected Cauldron1600ms pulse rather than the former Beachball fallback");
    const auto beforeRejected=oPixel();
    results.Check(session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::O,true)==VIRTUAL_LITCH_OK && session.Advance(30U)==VIRTUAL_LITCH_OK &&
        session.SetButtonDown(bank,runtime::VirtualLitchPhysicalButton::O,false)==VIRTUAL_LITCH_OK && session.Advance(30U)==VIRTUAL_LITCH_OK && session.Advance(75U)==VIRTUAL_LITCH_OK,
        "CPU O attempt traverses real debounce and request rejection");
    results.Check(onlyO(true) && !session.HintDisplayRequested() && worker.IsActive() && engine.starts==1U,
        "rejected CPU O retains only authoritative CPU brewing without manual Hint intent or another search");
    (void)session.Advance(1465U);
    results.Check(onlyO(true) && CueEqual(oPixel(),beforeRejected),
        "rejected O does not reset the active CPU brewing phase one complete cycle later");
    ControlCheckpoint(session,"Cauldron/CPU-thinking/raw-rejected-O","rejected-with-CPU-brew-retained","wait-for-computer");
    (void)session.Advance(800U);
    results.Check(Set(session,953U,"VS_CPU_LEVEL","0") &&
        session.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL",3U)==0U,
        "real typed CPU-off setting accepts cancellation while the owned worker is held");
    const uint32_t cancelledAt=session.State().now_ms;
    uint32_t sampled=0U,atCancellation=0U,atHalf=0U;
    for(const uint32_t elapsed:{0U,20U,50U,100U,150U,199U,200U,220U}) {
        results.Check(session.Advance(elapsed-sampled)==VIRTUAL_LITCH_OK,
            "CPU retirement advances the existing presentation clock without completing the held search");
        sampled=elapsed;const auto energy=CueEnergy(oPixel());
        results.Check(elapsed>=200U ? onlyO(false) : onlyO(true)||onlyO(false),
            "CPU cancellation never leaks beyond its outgoing O and reaches the existing200ms dark endpoint");
        if(elapsed<=100U) results.Check(energy>0U,"CPU cancellation retains visible O through its first100ms before the quantized tail");
        if(elapsed==0U)atCancellation=energy;
        if(elapsed==100U)atHalf=energy;
        results.Check(!session.CpuThinkingVisible() && !session.HintDisplayRequested() &&
            session.State().physical_occupied_mask==session.State().expected_occupied_mask,
            "retiring lights do not resurrect CPU work, Hint intent or a physical correction");
        std::printf("[CAULDRON_CPU_RETIREMENT] black=%u rotate=%u t=%u elapsed=%u accepted_cpu_off=1 requested=0 O=%u/%u/%u energy=%u expiry_ms=200 next=ordinary-human-play\n",
            black,rotate,session.State().now_ms,session.State().now_ms-cancelledAt,oPixel().r,oPixel().g,oPixel().b,energy);
        ControlCheckpoint(session,"Cauldron/CPU-cancel/existing-fade","retirement-frame","ordinary-human-play-after-fade");
    }
    results.Check(atCancellation>atHalf && atHalf>0U,
        "CPU cancellation preserves a diminishing Cauldron O before its bounded dark endpoint");
    engine.CancelThink();results.Check(engine.Finish(session.State().now_ms),"CPU cancellation fixture retires its actual held worker");
    results.Check(engine.unsafeReads.load()==0U,"CPU retirement never reads the live engine lane");
    RemoveFiles(PATH);
}

void CheckCpuThinkingJourneys(Results& results) {
    fxe::ScopedMillisProviderOverride clock(&AtomicMillis);
    CheckCauldronBrewingJourney(results, false, false, CauldronContext::CoachingAndPairing);
    for(bool black:{false,true})for(bool rotate:{false,true}) {
        CheckCauldronBrewingJourney(results,black,rotate);
        CheckCauldronCpuThinking(results,black,rotate);
    }
    CheckClassicHintRequestHandoff(results);
    CheckDuckHintRequestHandoff(results, false);
    CheckDuckHintRequestHandoff(results, true);
    CheckThinkingControlFadeJourney(results,false);
    CheckThinkingControlFadeJourney(results,true);
    CheckRawThinkingControlJourney(results,false,false,false,false,false,true);
    CheckRawThinkingControlJourney(results,true,true,false,false,false,true);
    for (const bool duck : {false, true}) {
        for (const char* style : {"\"ripple\"", "\"mini_orbit\"", "\"landing_flash\""}) {
            CheckRapidSettleSearchHandoff(results, duck, "\"settle_2s\"", 2000U, style);
            CheckRapidSettleSearchHandoff(results, duck, "\"settle_5s\"", 5000U, style);
        }
    }
    CheckRapidSettleSearchHandoff(results, false, "\"settle_2s\"", 2000U, "\"ripple\"", FollowupPlacementTiming::BeforeAckE5);
    CheckRapidSettleSearchHandoff(results, false, "\"settle_2s\"", 2000U, "\"ripple\"", FollowupPlacementTiming::BeforeAckIllegalF5);
    CheckKingResignationSearchJourney(results, false);
    CheckKingResignationSearchJourney(results, true);
    for(bool cpu:{false,true})
        for(bool black:{false,true})
            for(bool firstP2:{false,true})
                for(bool rotate:{false,true})
                    for(bool v3:{false,true})
                        CheckRawThinkingControlJourney(results,cpu,black,firstP2,rotate,v3);
    CheckSearchCancellation(results);
    for (bool afterDuck : {false,true})
        for (bool fragmented : {true,false}) CheckUrgentDuckNewClassic(results,fragmented,afterDuck);
    CheckCapturedWorkspaceAllocation(results, false);
    CheckCapturedWorkspaceAllocation(results, true);
    CheckCapturedWorkspaceAllocation(results, false, true);
    CheckEstimate(results);
    CheckFull3RootDiagnostics(results);
    CheckBenchmarkDiagnosticIsolation(results);
    CheckDiagnosticFreshness(results);
    CheckCpuDiagnosticRates(results);
    CheckCpuDeadlineJourney(results, true);
    CheckCpuDeadlineJourney(results, false);
    CheckCapturedKnightToCpuJourney(results);
    CheckBookHintFirstRevealJourney(results);
    CheckIncompleteOpeningContext(results);
    CheckHintFirstRevealJourney(results, true);
    CheckHintFirstRevealJourney(results, false);
    CheckHintFirstRevealJourney(results, false, true);
    CheckHiddenHintSourceChangeJourney(results);
    CheckHiddenHintSourceChangeJourney(results, 1U);
    CheckHiddenHintSourceChangeJourney(results, 2U);
    CheckHiddenHintSourceChangeJourney(results, 3U);
    CheckHintRefinementBoundaries(results);
    CheckDuckHintMinimalToggleJourney(results, false);
    CheckDuckHintMinimalToggleJourney(results, true);
    for (bool duck : {false, true}) {
        CheckRealSearchCallbacks(results, duck);
        for (bool black : {false, true}) {
            for (bool firstP2 : {false, true}) {
                for (bool rotate : {false, true}) {
                    CheckSession(results, duck, black, firstP2, rotate, false);
                }
            }
        }
        CheckSession(results, duck, false, false, false, true);
        CheckSession(results, duck, false, false, false, false, PendingBoundary::Undo);
        CheckSession(results, duck, false, false, false, false, PendingBoundary::Timeout);
        CheckSession(results, duck, false, false, false, false,
                     PendingBoundary::None, 11U);
    }
}
