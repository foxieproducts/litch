#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <unistd.h>

#include "bounded_hint_engine.hpp"
#include "firmware/cold_factory_reset.hpp"
#include "firmware/recovery_status.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

using hardware::BoardRevision;
using firmware::RecoveryAction;
using firmware::RecoveryActionFailure;
using firmware::RecoveryControlLevels;
using firmware::RecoveryPolicy;
using firmware::RecoveryState;

constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
constexpr uint64_t LIFTED = OPENING & ~(UINT64_C(1) << 52U);
constexpr uint64_t AFTER_E4 = LIFTED | (UINT64_C(1) << 36U);
int failures = 0;

void Check(const bool ok, const char* message) {
    if (ok) return;
    ++failures;
    std::printf("[FAIL] cold factory reset: %s\n", message);
}

virtual_litch_config Config(const BoardRevision revision) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = revision == BoardRevision::V3_3S3 ? VIRTUAL_LITCH_HARDWARE_V3_3
        : revision == BoardRevision::V3S3 ? VIRTUAL_LITCH_HARDWARE_V3
                                         : VIRTUAL_LITCH_HARDWARE_S3;
    return config;
}

std::string ReadFile(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}

std::string SeedSavedGame(const std::string& path) {
    test_support::BoundedHintSession fixture(Config(BoardRevision::V3_3S3), path.c_str());
    auto& board = fixture.session;
    board.ObserveHallMask(OPENING);
    board.Advance(6200U);
    board.ObserveHallMask(LIFTED);
    board.ObserveHallMask(AFTER_E4);
    board.Advance(2100U);
    Check(board.State().expected_occupied_mask == AFTER_E4, "the saved game contains e2-e4");
    auto& settings = board.SettingsForPersistenceTest();
    settings.Doc()["BRIGHTNESS"] = 2U;
    Check(settings.SaveNow(), "the production settings store writes the game and preference");
    const auto bytes = ReadFile(path);
    Check(!bytes.empty(), "the reset has a real persisted document to protect or erase");
    return bytes;
}

struct Step {
    uint32_t atMs;
    const char* description;
    RecoveryControlLevels held;
};

constexpr RecoveryControlLevels DOWN1{.p1Down = true};
constexpr RecoveryControlLevels DOWN2{.p2Down = true};
constexpr RecoveryControlLevels CHORD1{.p1X = true, .p1O = true};
constexpr RecoveryControlLevels CHORD2{.p2X = true, .p2O = true};
constexpr RecoveryControlLevels SPLIT{.p1X = true, .p2O = true};
constexpr RecoveryControlLevels CIRCLE1{.p1O = true};

struct Restarted {};
enum class Result { Continue, Restart, DeleteFailed };

struct Expected {
    Result result;
    uint32_t finishMs;
    unsigned lostFrame = 0U;
    firmware::ColdFactoryResetBoot boot{true, false};
};

class BoardAtBoot final : public firmware::ColdFactoryResetTarget {
  public:
    BoardAtBoot(RecoveryPolicy& policy, const hardware::BoardHardwareProfile& profile,
                modules::SettingsStore& settings, const std::initializer_list<Step> steps,
                const char* trial, const unsigned lostFrame)
        : ColdFactoryResetTarget(policy, profile), m_profile(profile), m_settings(settings),
          m_step(steps.begin()), m_end(steps.end()), m_trial(trial), m_lostFrame(lostFrame) {}

    RecoveryControlLevels ReadResetControls() override {
        while (m_step != m_end && m_step->atMs <= fxe::Millis()) {
            m_controls = m_step->held;
            m_controls.hasP2Controls = m_profile.hasP2Controls;
            m_input = m_step->description;
            Checkpoint("input sampled");
            ++m_step;
        }
        return m_controls;
    }

    void WaitForResetInput(const uint32_t ms) override {
        const auto state = Policy().GetSnapshot().state;
        if (Policy().IsColdFactoryReset() && m_lastState != state) {
            m_lastState = state;
            if (state == RecoveryState::WaitForRelease) Checkpoint("release Down and X/O");
            if (state == RecoveryState::Ready) Checkpoint("hold X+O at the same end");
            if (state == RecoveryState::DestructiveHold) Checkpoint("keep X+O held for two seconds");
            if (state == RecoveryState::Cancelled) Checkpoint("rejected: released early");
        }
        fxe::ManualMillis::AdvanceMs(ms);
    }

    bool DeliverResetFrame(const hardware::PhysicalLedFrame& frame) override {
        if (++m_frames == m_lostFrame) {
            Checkpoint("rejected: LED delivery failed");
            return false;
        }
        unsigned lit = 0U;
        modules::Rgb color{};
        for (const auto& pixel : frame) {
            if (pixel.r || pixel.g || pixel.b) {
                ++lit;
                color = pixel;
            }
        }
        if (lit) CheckFacePixels(frame, color, lit);
        if (lit && color.r) {
            Check(color.r > color.g && color.g && !color.b, "armed cue is amber");
            m_cue = "all X/O amber";
            if (!amberCount++) {
                Checkpoint("armed: release Down, then hold same-end X+O");
            }
        } else if (lit) {
            Check(!color.r && color.g && !color.b, "successful cue is green");
            Check(deletes == 1U && clears == 1U && m_deleteVerified,
                  "green requires verified deletion and boot guard clear");
            if (greenCount < greenAt.size()) greenAt[greenCount] = fxe::Millis();
            ++greenCount;
            m_cue = "all X/O green";
            Checkpoint("accepted: wait for restart");
        } else if (m_cue[0] != 'd') {
            m_cue = "dark";
            if (amberCount == 1U || greenCount) Checkpoint("blink off");
        }
        return true;
    }

    void PrepareForRetryReboot() override { ++retries; }
    bool DeletePersistedState() override {
        ++deletes;
        deleteAt = fxe::Millis();
        m_deleteVerified = m_settings.DeleteFile();
        Checkpoint(m_deleteVerified ? "settings and game deleted" : "rejected: delete failed");
        return m_deleteVerified;
    }
    void ClearBootGuard() override { ++clears; }
    void Restart() override {
        ++restarts;
        Checkpoint("restart");
        throw Restarted{};
    }

    void Checkpoint(const char* result) {
        const auto snapshot = Policy().GetSnapshot();
        std::printf("[ColdFactoryResetVirtualLitch] %s rev=%u ms=%u hall=%016llx "
                    "input=\"%s\" mode=%s result=\"%s\" display=\"\" LEDs=\"%s\"\n",
                    m_trial, unsigned(m_profile.revision), fxe::Millis(),
                    static_cast<unsigned long long>(AFTER_E4), m_input,
                    firmware::RecoveryStateToken(snapshot.state), result, m_cue);
    }

    void CheckFailureCue() {
        const auto frame = firmware::ComposeRecoveryLedFrame(Policy().GetSnapshot(), fxe::Millis());
        unsigned red = 0U;
        for (const auto& pixel : frame) {
            if (pixel.r && !pixel.g && !pixel.b) ++red;
        }
        Check(red == 8U, "failed deletion renders the red recovery cue");
        m_cue = "eight red perimeter LEDs";
        Checkpoint("recovery: release, then hold Circle for ordinary retry");
    }

    void CheckpointOrdinaryRetry(const char* result) {
        const auto frame = firmware::ComposeRecoveryLedFrame(Policy().GetSnapshot(), fxe::Millis());
        unsigned amber = 0U, blue = 0U, cyan = 0U, green = 0U;
        for (const auto& pixel : frame) {
            if (pixel.r && pixel.g && !pixel.b) ++amber;
            if (!pixel.r && pixel.b > pixel.g) ++blue;
            if (!pixel.r && pixel.g && pixel.b) ++cyan;
            if (!pixel.r && pixel.g > pixel.b) ++green;
        }
        char cue[96]{};
        std::snprintf(cue, sizeof(cue), "perimeter: amber=%u blue=%u cyan=%u green=%u",
                      amber, blue, cyan, green);
        m_cue = cue;
        Checkpoint(result);
        m_cue = "ordinary recovery perimeter";
    }

    unsigned amberCount = 0U, greenCount = 0U, deletes = 0U, clears = 0U, restarts = 0U, retries = 0U;
    uint32_t deleteAt = 0U;
    std::array<uint32_t, 2> greenAt{};

  private:
    void CheckFacePixels(const hardware::PhysicalLedFrame& frame, const modules::Rgb color,
                         const unsigned lit) const {
        const auto revision = m_profile.revision;
        const auto base = hardware::BoardLedTopologyFor(revision).physicalBoardLedCount;
        Check(lit == 4U, "only the four X/O positions light");
        for (const bool secondBank : {false, true}) {
            for (const auto role : {hardware::ControlLedRole::X, hardware::ControlLedRole::O}) {
                const uint8_t offset = revision == BoardRevision::V3_3S3
                    ? hardware::V3_3PhysicalIndexForControl(secondBank, role)
                    : modules::PlayerHomeLedIndex(secondBank ? modules::UiOwner::P2 : modules::UiOwner::P1,
                                                 role == hardware::ControlLedRole::O ? 2U : 5U);
                const auto pixel = frame[base + offset];
                Check(pixel.r == color.r && pixel.g == color.g && pixel.b == color.b,
                      "both physical ends receive the same X/O color");
            }
        }
    }

    const hardware::BoardHardwareProfile& m_profile;
    modules::SettingsStore& m_settings;
    const Step* m_step;
    const Step* m_end;
    const char* m_trial;
    const unsigned m_lostFrame;
    unsigned m_frames = 0U;
    RecoveryControlLevels m_controls{};
    RecoveryState m_lastState = RecoveryState::Inactive;
    bool m_deleteVerified = false;
    const char* m_input = "power on";
    const char* m_cue = "dark";
};

void Trial(const char* name, const BoardRevision revision,
           const std::string& path, const std::string& saved,
           const std::initializer_list<Step> steps, const Expected expected) {
    std::ofstream(path) << saved;
    modules::SettingsStoreConfig storeConfig;
    storeConfig.path = path.c_str();
    modules::SettingsStore settings(storeConfig);
    Check(settings.Load(), "startup loads the saved settings and game before reset");
    if (expected.result == Result::DeleteFailed)
        settings.SetBeforeDirectStorageAccessCallback([]() { return false; });
    RecoveryPolicy policy;
    const auto profile = hardware::BoardHardwareProfileFor(revision);
    BoardAtBoot board(policy, profile, settings, steps, name, expected.lostFrame);
    bool restarted = false;
    {
        fxe::ManualMillis::SetNowMs(0U);
        fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
        try {
            const auto failure = firmware::RunColdFactoryReset(expected.boot, board);
            Check(failure == (expected.result == Result::DeleteFailed
                                  ? RecoveryActionFailure::PersistedStateDeleteFailed
                                  : RecoveryActionFailure::None), "boot reports the exact action result");
        } catch (const Restarted&) {
            restarted = true;
        }
        const bool erased = expected.result == Result::Restart;
        Check(fxe::Millis() == expected.finishMs, "boot exits at the expected deadline");
        Check(restarted == erased && board.restarts == unsigned(erased), "restart only follows verified delete");
        Check(board.deletes == unsigned(expected.result != Result::Continue) && board.clears == unsigned(erased),
              "commit boundary admits at most one delete and clears guard only on success");
        Check(board.greenCount == (erased ? 2U : 0U), "only success blinks green twice");
        const bool showAmber = expected.boot.coldPowerOn && !expected.boot.deepSleepWake &&
            expected.lostFrame != 1U;
        Check((board.amberCount != 0U) == showAmber,
              "amber is delivered only on admitted cold power");
        if (expected.finishMs == 10020U)
            Check(board.amberCount == 20U, "timeout closes without another amber pulse");
        if (erased) {
            Check(board.deleteAt == 2320U && board.greenAt[0] == board.deleteAt &&
                      board.greenAt[1] - board.greenAt[0] == 500U &&
                      fxe::Millis() - board.greenAt[0] == 1000U,
                  "green begins after two seconds of debounced consent and finishes before restart");
            Check(!std::filesystem::exists(path), "success removes the durable settings document");
        } else {
            Check(ReadFile(path) == saved, "the cancelled or refused reset preserves the saved game");
        }
        if (expected.result == Result::DeleteFailed) {
            Check(policy.GetSnapshot().state == RecoveryState::ActionFailed, "failure enters recovery");
            board.CheckFailureCue();
            board.WaitForResetInput(100U);
            policy.Update(firmware::NormalizeSafeRecoveryInput(board.ReadResetControls()), fxe::Millis());
            board.CheckpointOrdinaryRetry("ready: hold Circle for ordinary retry");
            board.WaitForResetInput(100U);
            const auto circle = firmware::NormalizeSafeRecoveryInput(board.ReadResetControls());
            policy.Update(circle, fxe::Millis());
            fxe::ManualMillis::AdvanceMs(750U);
            policy.Update(circle, fxe::Millis());
            board.CheckpointOrdinaryRetry("keep holding Circle");
            fxe::ManualMillis::AdvanceMs(750U);
            const auto retry = policy.Update(circle, fxe::Millis());
            Check(retry.action == RecoveryAction::RetryBoot, "a fresh ordinary Circle hold can retry");
            board.CheckpointOrdinaryRetry("accepted: non-destructive retry");
            bool retried = false;
            try {
                firmware::ExecuteRecoveryActionSequence(retry.action, board);
            } catch (const Restarted&) {
                retried = true;
            }
            Check(retried && board.restarts == 1U && board.retries == 1U && board.deletes == 1U,
                  "ordinary retry reboots without another delete");
        } else if (!erased) {
            board.Checkpoint("normal startup: saved game retained");
        }
    }

    test_support::BoundedHintSession fixture(Config(revision), path.c_str());
    auto& session = fixture.session;
    session.ObserveBootHallMask(AFTER_E4);
    session.Advance(6000U);
    const bool erased = expected.result == Result::Restart;
    const auto state = session.State();
    const auto home = session.HomeStateSnapshot();
    Check(state.physical_occupied_mask == AFTER_E4 &&
              state.expected_occupied_mask == (erased ? OPENING : AFTER_E4),
          "VirtualLitch boots into fresh placement after erase, or resumes the saved game");
    Check(home.placementMode == erased && home.freshSetup == erased &&
              home.action == (erased ? modules::HomeStateAction::Correction
                                     : modules::HomeStateAction::Idle),
          "the restored board owns opening correction after erase or ordinary play after retry");
    Check((session.SettingsForPersistenceTest().ReadValue("BRIGHTNESS", 8U) != 2U) == erased,
          "the same normal boot path restores saved brightness or uses the default");
    const auto& frame = session.SemanticFrame();
    const auto e2 = frame.board[52U][0U];
    const auto e4 = frame.board[36U][0U];
    std::printf("[ColdFactoryResetVirtualLitch] %s resumed hall=%016llx placement=%u "
                "fresh_setup=%u phase=%s action=%s display=\"%.*s\" expected=%016llx "
                "LEDs=\"e2=%u/%u/%u e4=%u/%u/%u\" next=\"%s\"\n", name,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                unsigned(home.placementMode), unsigned(home.freshSetup),
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                modules::HomeStateActionToToken(home.action),
                int(session.DisplayText().size()), session.DisplayText().data(),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                e2.r, e2.g, e2.b, e4.r, e4.g, e4.b,
                home.placementMode ? "return e4 pawn to e2" : "continue the saved game");
}

} // namespace

int main() {
    // Leave room for the production settings owner's .tmp/.bak/.retry suffixes.
    std::string dir = "/tmp/lcr-XXXXXX";
    if (::mkdtemp(dir.data()) == nullptr) return 1;
    const auto saved = SeedSavedGame(dir + "/seed.json");
    const auto path = dir + "/settings.json";
    for (const auto revision : {BoardRevision::V2S3, BoardRevision::V3S3, BoardRevision::V3_3S3}) {
        Trial("P1 confirms", revision, path, saved,
              {{0U, "P1 Down at power", DOWN1}, {100U, "release", {}},
               {300U, "fresh P1 X+O", CHORD1}}, {Result::Restart, 3320U});
    }
    constexpr auto revision = BoardRevision::V3_3S3;
    Trial("P2 confirms", revision, path, saved,
          {{0U, "P2 Down at power", DOWN2}, {100U, "release", {}},
           {300U, "fresh P2 X+O", CHORD2}}, {Result::Restart, 3320U});
    Trial("early release", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}, {100U, "release", {}},
           {300U, "fresh P1 X+O", CHORD1}, {2315U, "release before commit", {}}},
          {Result::Continue, 2340U});
    Trial("split ends time out", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}, {100U, "release", {}},
           {300U, "P1 X with P2 O", SPLIT}, {2900U, "other end X+O", CHORD2}},
          {Result::Continue, 10020U});
    Trial("pre-held chord times out", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}, {100U, "X+O before neutral debounce", CHORD1}},
          {Result::Continue, 10020U});
    Trial("prompt unavailable", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}}, {Result::Continue, 20U, 1U});
    Trial("prompt lost", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}, {100U, "release", {}},
           {300U, "fresh P1 X+O", CHORD1}}, {Result::Continue, 520U, 3U});
    Trial("non-cold restart", revision, path, saved,
          {{0U, "Down held at software restart", DOWN1}},
          {Result::Continue, 0U, 0U, {false, false}});
    Trial("deep sleep wake", revision, path, saved,
          {{0U, "Down held on wake", DOWN1}},
          {Result::Continue, 0U, 0U, {true, true}});
    Trial("storage refuses reset", revision, path, saved,
          {{0U, "P1 Down at power", DOWN1}, {100U, "release", {}},
           {300U, "fresh P1 X+O", CHORD1}, {2420U, "release after error", {}},
           {2520U, "Circle alone retries", CIRCLE1}}, {Result::DeleteFailed, 2320U});
    std::filesystem::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
