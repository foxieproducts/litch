#pragma once

void RunLateSaveAfterSessionFallback() {
    const char* path = "build/t_storage_late_success.json";
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) {
        std::filesystem::remove_all(std::string(path) + suffix);
    }
    modules::SettingsStoreConfig config; config.path = path;
    modules::SettingsStore seed(config);
    Check(seed.Load() && modules::WriteGamePersistenceState(seed.Doc(), CurrentLineState(1U, 2U)) &&
              seed.SaveNowBlocking(), "late-save journey has verified played baseline");
    virtual_litch_config boardConfig{}; virtual_litch_default_config(&boardConfig);
    test_support::BoundedHintSession fixtureBoard(boardConfig, path);
    auto& board = fixtureBoard.session;
    auto& source = board.SettingsForPersistenceTest();
    fixture::stopping = false; fixture::notified = false;
    fixture::hold.store(true); fixture::blocked.store(false);
    fixture::holdRetry.store(false); fixture::workerAtWait.store(false);
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    Check((worker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), worker.Begin(source)) && worker.Enqueue(source.Doc()), "real accepted snapshot owns worker");
    fixture::Wait([] { return fixture::blocked.load(); });
    source.DisablePersistenceForSession();
    // Match the waiting worker's mutex so its predicate check cannot miss release.
    {
        std::lock_guard<std::mutex> lock(fixture::mutex);
        fixture::hold.store(false);
    }
    fixture::changed.notify_all();
    fixture::Wait([&] { return worker.GetCompletedSaveCount() == 1U && fixture::workerAtWait.load(); });
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && source.IsDisabled() &&
              source.GetLastError() == fxe::SettingsStoreError::None &&
              std::strstr(board.HomeStateLine().CStr(), "\"ps\"") == nullptr &&
              std::strstr(board.BoardHealthStateLine().CStr(), "\"ps\":\"n\"") != nullptr &&
              std::strstr(board.BoardHealthStateLine().CStr(), "\"sd\":1") != nullptr &&
              !source.HasConfirmedStorageFailure() && !worker.Enqueue(source.Doc()),
          "late accepted save cannot re-enable RAM session or hide unsaved consequences");
    std::puts("[LATE_SAVE_SESSION_ONLY_JOURNEY] accepted_snapshot=saved session_only=1 home_ps=absent board_health_ps=n subsequent_save=refused");
    { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
    fixture::worker.join();
}

void RunRecoveryProtectionSnapshot() {
    StaticJsonDocument<2048U> live;
    StaticJsonDocument<1024U> safe;
    modules::BoardIdentity identity; identity.bytes[0U] = 9U;
    modules::HardwareTestRecord record;
    record.revision = hardware::BoardRevision::V3S3;
    record.radioResult = modules::ManufacturingTestRadioResult::Bypassed;
    Check(modules::WriteBoardIdentity(live, identity) &&
              modules::WriteHardwareTestRecord(live, record), "seed readable hardware facts");
    live["LED_BROWNOUT_LIMIT_LATCH"] = 1U;
    live["LED_BROWNOUT_INCIDENT_COUNT"] = 9U;
    live["RGBW_COOL_TO_WARM_THRESHOLD"] = 180U;
    live["USB_CC_THRESH_500"] = 320U;
    live["USB_CC_THRESH_1500"] = 580U;
    live["USB_CC_THRESH_3000"] = 850U;
    live["BRIGHTNESS"] = 5U; live["GAME_FEN"] = "RAM game";
    live["SAVE_COUNTER"] = 99U;
    const std::string original = [&] { std::string text; serializeJson(live, text); return text; }();
    Check(modules::BuildBoardStorageRecoverySnapshot(live, safe), "bounded safe snapshot builds");
    modules::BoardIdentity recovered;
    Check(modules::ReadBoardIdentity(safe, recovered) == modules::BoardIdentityRead::Current &&
              recovered == identity && safe["HARDWARE_TEST_RECORD"].is<const char*>() &&
              safe["LED_BROWNOUT_LIMIT_LATCH"].as<bool>() &&
              safe["LED_BROWNOUT_INCIDENT_COUNT"].as<unsigned>() == 2U &&
              safe["RGBW_COOL_TO_WARM_THRESHOLD"].as<unsigned>() == 180U &&
              safe["USB_CC_THRESH_3000"].as<unsigned>() == 850U &&
              !safe.containsKey("BRIGHTNESS") && !safe.containsKey("GAME_FEN") &&
              !safe.containsKey("SAVE_COUNTER"), "safe snapshot keeps only readable hardware protection");
    std::string unchanged; serializeJson(live, unchanged);
    Check(unchanged == original, "safe snapshot never changes live RAM game or preferences");
    live["LED_BROWNOUT_LIMIT_LATCH"] = -1;
    live["LED_BROWNOUT_INCIDENT_COUNT"] = true;
    live["USB_CC_THRESH_1500"] = 100U;
    live["RGBW_COOL_TO_WARM_THRESHOLD"] = 256U;
    Check(modules::BuildBoardStorageRecoverySnapshot(live, safe) &&
              safe["LED_BROWNOUT_LIMIT_LATCH"].as<bool>() &&
              safe["LED_BROWNOUT_INCIDENT_COUNT"].as<unsigned>() == 1U &&
              !safe.containsKey("USB_CC_THRESH_500") &&
              !safe.containsKey("RGBW_COOL_TO_WARM_THRESHOLD"),
          "numeric latch remains protected and invalid calibration returns to conservative defaults");
    StaticJsonDocument<32U> tooSmall;
    Check(!modules::BuildBoardStorageRecoverySnapshot(live, tooSmall),
          "snapshot capacity failure cannot claim protected recovery admission");
}

// Uses the real SettingsPersistenceWorker/CurrentGameLogService/admission above.
// Only SDK scheduling and the filesystem failure are controlled. The engine
// wrapper bounds continuous Hint work; this is not a physical search-time gate.
void RunRuntimeStorageNewGameJourney(const bool unavailableWorker) {
    using Result = modules::ControlCoordinatorResult;
    using Kind = modules::GamePersistenceCoordinator::ReplacementKind;
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    constexpr uint64_t played = (opening & ~(UINT64_C(1) << 52U)) |
        (UINT64_C(1) << 36U);
    const char* path = unavailableWorker ? "build/t_storage_no_worker.json"
                                        : "build/t_storage_runtime_failure.json";
    std::filesystem::remove_all(std::string(path) + ".tmp");
    std::filesystem::remove_all(std::string(path) + ".bak");
    std::filesystem::remove_all(std::string(path) + ".retry");
    std::filesystem::remove(path);
    modules::SettingsStoreConfig config; config.path = path;
    modules::SettingsStore seed(config);
    Check(seed.Load(), "runtime storage fixture starts on a real settings file");
    seed.Doc()["BRIGHTNESS"] = 3U;
    seed.Doc()["TIMER"] = 1U;
    seed.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
    Check(modules::WriteGamePersistenceState(seed.Doc(), CurrentLineState(1U, 2U)) &&
              seed.SaveNowBlocking(), "runtime storage fixture seeds a played game");
    virtual_litch_config boardConfig{}; virtual_litch_default_config(&boardConfig);
    boardConfig.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixtureBoard(boardConfig, path);
    auto& board = fixtureBoard.session;
    auto& settings = board.SettingsForPersistenceTest();
    fixture::stopping = false; fixture::notified = false;
    fixture::hold.store(false); fixture::blocked.store(false);
    fixture::holdRetry.store(false); fixture::workerAtWait.store(false);
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    CurrentLineMemoryStorage storage;
    firmware::CurrentGameLogService log(storage.Ops());
    ScratchReadbackFault readbackFault(path);
    firmware::CurrentGameSyncRuntime sync(worker, log, storage.Ops(), settings);
    Check(sync.Configure(settings.Doc()), "runtime storage fixture configures actual log owner");
    struct Wiring {
        firmware::CurrentGameSyncRuntime* sync;
        modules::GamePersistenceCoordinator* coordinator = nullptr;
        uint8_t identity = 0x80U;
    } wiring{&sync};
    board.RunControlIngress([](void* context, modules::SettingsStore&,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
        auto& wiring = *static_cast<Wiring*>(context); wiring.coordinator = &game;
        game.SetGameHistoryIdentityGenerator([](void* context,
            modules::game_history::GameId& out) {
            out = {}; out.bytes[0U] = ++static_cast<Wiring*>(context)->identity;
            return true;
        }, context);
        game.SetReplacementAdmissionCallback([](void* context,
            const modules::GameHistoryPersistenceState& current, const Kind kind) {
            // Equivalent controlled OLD admission fault: keep all production
            // input, New Game, engine, RAM and rendering owners, but reject the
            // session-only replacement as the former durable gate did.
            if (std::getenv("LITCH_STORAGE_ADMISSION_OLD") != nullptr) {
                return Result::StorageUnavailable;
            }
            return static_cast<Wiring*>(context)->sync->CheckReplacementAdmission(current, kind);
        }, context);
    }, &wiring);
    auto& game = *wiring.coordinator;
    fixture::failNextTaskCreate.store(unavailableWorker);
    const bool started = (worker.SetStorageAccessTestHook(&ScratchReadbackFault::Observe, &readbackFault), worker.Begin(settings));
    Check(started != unavailableWorker, "worker availability is the real SDK startup outcome");
    Check(board.ObserveHallMask(played) == VIRTUAL_LITCH_OK &&
              board.Advance(6200U) == VIRTUAL_LITCH_OK,
          "runtime storage fixture settles a restored played board");
    auto snapshot = [&] {
        bool accepted = false;
        board.RunControlIngress([](void* context, modules::SettingsStore& source,
            modules::ControlMutationService&, modules::GamePersistenceCoordinator&) {
            *static_cast<bool*>(context) = source.SaveNow();
        }, &accepted);
        return accepted;
    };
    if (started) {
        Check(snapshot(), "healthy played baseline queues through real worker");
        fixture::Wait([&] { return worker.GetCompletedSaveCount() == 1U &&
                                  fixture::workerAtWait.load(); });
        Check(sync.CheckReplacementAdmission(game.GetGameHistoryPersistenceState(), Kind::NewGame) ==
                  Result::Applied, "healthy unarchived play permits explicit New Game");
        const uint64_t lifted = played & ~(UINT64_C(1) << 12U);
        Check(board.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK &&
                  board.ObserveHallMask(lifted | (UINT64_C(1) << 28U)) == VIRTUAL_LITCH_OK &&
                  board.Advance(2100U) == VIRTUAL_LITCH_OK,
              "runtime failure follows a production black pawn move");
        readbackFault.armed.store(true);
        Check(snapshot(), "real scratch-file readback fault admits then fails a save");
        fixture::Wait([&] { return worker.GetFailureCount() != 0U &&
                                  fixture::workerAtWait.load(); });
        std::printf("[RUNTIME_STORAGE_FAILURE] failed=%u saved=%u result=%u error=%u\n",
            worker.GetFailureCount(), worker.GetCompletedSaveCount(),
            unsigned(log.LastResult()), unsigned(worker.GetLastError()));
        Check(readbackFault.corruptions.load() != 0U &&
                  worker.GetLastError() == fxe::SettingsStoreError::VerifyFailed &&
                  settings.HasConfirmedStorageFailure() && settings.HasTerminalSaveFailure(),
              "actual failed readback establishes confirmed terminal storage evidence");
        readbackFault.armed.store(false);
        for (const char* suffix : {".tmp", ".retry"}) std::filesystem::remove_all(std::string(path) + suffix);
        Check(sync.CheckReplacementAdmission(game.GetGameHistoryPersistenceState(), Kind::NewGame) ==
                  Result::Applied,
              "explicit New Game is admitted before terminal storage failure reconciliation");
    } else {
        Check(sync.CheckReplacementAdmission(game.GetGameHistoryPersistenceState(), Kind::NewGame) ==
                  Result::Applied, "unavailable persistence worker cannot gate explicit New Game");
    }
    const auto before = game.GetGameHistoryPersistenceState();
    const std::string fen = game.GetAuthoritativeFen();
    const auto undo = board.UndoHistoryMoveCount();
    const auto clockSide = board.GameClock()->GetActiveSide();
    const auto clockWhite = board.GameClock()->GetRemainingMs(runtime::ClockSide::White);
    const auto clockBlack = board.GameClock()->GetRemainingMs(runtime::ClockSide::Black);
    modules::SettingsStore disk(config); Check(disk.Load(), "last verified document remains readable");
    const std::string diskFen = disk.ReadValue("GAME_FEN", "");
    // Production boot takes this branch for a worker startup failure. Runtime
    // terminal failure takes the same shared helper on the next main-loop tick.
    if (!started) {
        board.RunControlIngress([](void*, modules::SettingsStore& source,
            modules::ControlMutationService& controls, modules::GamePersistenceCoordinator&) {
            modules::PersistenceDegradation degradation = modules::PersistenceDegradation::None;
            modules::UpdateRuntimePersistenceSession(source, controls, degradation, false);
        }, nullptr);
    }
    Check(board.Advance(0U) == VIRTUAL_LITCH_OK && settings.IsDisabled() &&
              game.GetAuthoritativeFen() == fen &&
              game.GetGameHistoryPersistenceState().gameId == before.gameId &&
              board.UndoHistoryMoveCount() == undo &&
              board.GameClock()->GetActiveSide() == clockSide &&
              board.GameClock()->GetRemainingMs(runtime::ClockSide::White) == clockWhite &&
              board.GameClock()->GetRemainingMs(runtime::ClockSide::Black) == clockBlack &&
              settings.ReadValue("BRIGHTNESS", 0U) == 3U &&
              settings.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false),
          "fallback preserves live game/history/clock/preferences/known protection");
    if (started) {
        // Captured high-MTU CurrentBegin after the actual terminal save failure.
        // BLE Object admission uses this exact callback result; SDK radio is separate HIL.
        std::array<uint8_t, 20U> begin{{76U, 1U, 32U, 0U, 5U, 0U, 0U, 0U,
            9U, 0U, 0U, 0U, 43U, 154U, 242U, 220U, 22U, 36U, 125U, 106U}};
        const uint32_t saves = worker.GetCompletedSaveCount();
        for (uint8_t transaction : {5U, 6U}) {
            begin[4U] = transaction;
            const bool admitted = sync.Admit(begin.data(), begin.size(), 71U, false);
            Check(admitted, "session-only history envelope is admitted instead of starving Object progress");
            if (!admitted) continue;
            std::array<uint8_t, 20U> reply{};
            size_t size = 0U;
            uint32_t token = 0U;
            fixture::Wait([&] { return sync.Peek(reply.data(), reply.size(), size, token, 71U) &&
                                      fixture::workerAtWait.load(); });
            modules::game_history::Frame frame;
            Check(modules::game_history::DecodeFrame(reply.data(), size, frame) &&
                      frame.opcode == static_cast<uint8_t>(modules::game_history::Opcode::Reject) &&
                      frame.transactionId == transaction && frame.payloadLength != 0U &&
                      frame.payload[0U] == static_cast<uint8_t>(modules::game_history::RejectReason::BadState),
                  "history produces the canonical unavailable domain receipt while saving is disabled");
            sync.Accept(token, 71U);
        }
        fixture::Wait([&] { return fixture::workerAtWait.load(); });
        Check(worker.GetCompletedSaveCount() == saves && settings.IsDisabled(),
              "history refusal never resumes saving or acknowledges a durable archive");
    }
    modules::ControlMutationOutcome outcome;
    Check(board.MutateSettingJson(701U, "BRIGHTNESS", "4", outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.persistenceTier == modules::ControlMutationPersistenceTier::None,
          "ordinary brightness remains usable with truthful RAM-only outcome");
    Check(board.MutateSettingJson(702U, "SLEEP_TIMEOUT", "\"30m\"", outcome) == VIRTUAL_LITCH_OK &&
              outcome.status == modules::ControlMutationStatus::Applied,
          "ordinary hardware-category user setting remains usable in RAM");
    Check(board.MutateSettingJson(703U, "BRIGHTNESS", "99", outcome) == VIRTUAL_LITCH_OK &&
              outcome.reason == modules::ControlMutationReason::OutOfRange,
          "RAM settings retain normal validation");
    Check(board.MutateSettingJson(704U, "USB_CC_THRESH_500", "400", outcome) == VIRTUAL_LITCH_OK &&
              outcome.reason == modules::ControlMutationReason::StorageUnavailable,
          "critical calibration remains guarded");
    const auto line = board.HomeStateLine();
    const auto health = board.BoardHealthStateLine();
    Check((started ? std::strstr(line.CStr(), "\"ps\":\"i\"") != nullptr
                   : std::strstr(line.CStr(), "\"ps\"") == nullptr) &&
              std::strstr(health.CStr(), started ? "\"ps\":\"i\"" : "\"ps\":\"n\"") != nullptr &&
              std::strstr(health.CStr(), "\"sd\":1") != nullptr &&
              settings.HasConfirmedStorageFailure() == started,
          "confirmed readback failure and unavailable worker retain distinct truthful session-only evidence");
    auto press = [&] {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
              board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
              board.Advance(40U) == VIRTUAL_LITCH_OK, "raw buttons enter reset hold");
    };
    auto release = [&] {
        Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
              board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
              board.Advance(40U) == VIRTUAL_LITCH_OK, "raw chord fully releases");
    };
    auto checkpoint = [&](const char* stage, const char* next) {
        unsigned green = 0U;
        for (const auto pixel : board.SemanticFrame().status) {
            if (pixel.g > pixel.r && pixel.g > pixel.b) ++green;
        }
        std::printf("[RUNTIME_STORAGE_NEW_GAME_JOURNEY] worker=%u stage=%s t=%u occupancy=%016llx id=%u ply=%u display=\"%.*s\" green=%u cue=%u session_only=%u next=%s\n",
            started, stage, board.State().now_ms,
            static_cast<unsigned long long>(board.State().physical_occupied_mask),
            game.GetGameHistoryPersistenceState().gameId.bytes[0U],
            game.GetGameHistoryPersistenceState().currentPly,
            int(board.DisplayText().size()), board.DisplayText().data(), green,
            unsigned(board.EdgeCueIntent().kind), settings.IsDisabled(), next);
    };
    checkpoint("saving-failed-live-game-preserved", "play-or-hold-new-game");
    press(); Check(board.Advance(800U) == VIRTUAL_LITCH_OK, "partial hold progresses");
    checkpoint("partial-hold", "release-to-cancel");
    release();
    Check(game.GetAuthoritativeFen() == fen &&
              game.GetGameHistoryPersistenceState().gameId == before.gameId,
          "partial cancellation preserves the exact played game");
    press(); Check(board.Advance(1999U) == VIRTUAL_LITCH_OK &&
                  game.GetAuthoritativeFen() == fen, "1999ms does not reset");
    unsigned progressGreen = 0U;
    for (const auto pixel : board.SemanticFrame().status) {
        if (pixel.g > pixel.r && pixel.g > pixel.b) ++progressGreen;
    }
    Check(progressGreen > 0U, "real timed reset hold projects green perimeter consent progress");
    checkpoint("hold-1999", "keep-holding-one-ms");
    Check(board.Advance(1U) == VIRTUAL_LITCH_OK &&
              board.TryGetLastBoardControlOutcome(outcome), "2000ms completes one action");
    const bool applied = outcome.status == modules::ControlMutationStatus::Applied &&
        outcome.action == modules::ControlAction::NewGame &&
        game.GetGameHistoryPersistenceState().gameId != before.gameId &&
        game.GetGameHistoryPersistenceState().currentPly == 0U;
    Check(applied, "full green New Game hold applies despite unavailable saving");
    checkpoint(applied ? "new-game-applied" : "OLD-new-game-failed", "release-and-place-opening");
    const auto newIdentity = game.GetGameHistoryPersistenceState().gameId;
    Check(board.Advance(2500U) == VIRTUAL_LITCH_OK &&
              game.GetGameHistoryPersistenceState().gameId == newIdentity,
          "held terminal chord never repeats New Game");
    release();
    Check(disk.Load() && disk.ReadValue("GAME_FEN", "") == diskFen &&
              !settings.SaveNow(), "RAM replacement never retires or overwrites saved game");
    if (applied) {
        Check(board.ObserveHallMask(opening) == VIRTUAL_LITCH_OK &&
                  board.Advance(6200U) == VIRTUAL_LITCH_OK &&
                  board.State().correction_mask == 0U,
              "opening placement recovers into playable new game");
        const uint64_t lifted = opening & ~(UINT64_C(1) << 52U);
        Check(board.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK &&
                  board.ObserveHallMask(lifted | (UINT64_C(1) << 36U)) == VIRTUAL_LITCH_OK &&
                  board.Advance(2100U) == VIRTUAL_LITCH_OK &&
                  board.UndoHistoryMoveCount() == 1U,
              "next production move commits in RAM after New Game");
        modules::ControlMutationOutcome terminal;
        board.RunControlIngress([](void* context, modules::SettingsStore&,
            modules::ControlMutationService& controls, modules::GamePersistenceCoordinator&) {
            auto& terminal = *static_cast<modules::ControlMutationOutcome*>(context);
            terminal = controls.MutateAction(modules::ControlAction::Undo, 709U);
            if (terminal.status == modules::ControlMutationStatus::Accepted) {
                controls.Update(); (void)controls.TryTakeCompletion(709U, terminal);
            }
        }, &terminal);
        Check(terminal.status == modules::ControlMutationStatus::Applied,
              "Undo remains usable through the production control owner");
        checkpoint("next-move-and-undo", "restore-piece-as-cued");
    }
    if (started) {
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
        fixture::worker.join();
    }
    std::filesystem::remove_all(std::string(path) + ".tmp");
    std::filesystem::remove_all(std::string(path) + ".retry");
}
