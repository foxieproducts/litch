// Included by the existing actual-worker fixture; no second storage owner.
namespace {
void RunBootArtworkRecoveryRaceWorkerLane(const bool craft = false, const bool wakeMode = false) {
    (void)::mkdir("build", 0777);
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    fxe::ManualMillis::SetNowMs(0U);
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "boot recovery starts from its own clean Store");
    store::FrameStore seed(files.StoreOps());
    store::Manifest manifest{};
    manifest.frameCount = 2U;
    manifest.active = craft;
    store::TransactionToken token{};
    Check(seed.Recover() == store::Status::NoArtwork &&
              seed.Begin(manifest, 0U, 701U, token) == store::Status::Ok,
          "boot recovery commits ordinary saved artwork");
    store::Frame frame{};
    frame.durationMs = 100U;
    for (auto& emitter : frame.emitters)
        emitter.color = {80U, 0U, 0U};
    Check(seed.WriteFrame(token, 0U, frame) == store::Status::Ok,
          "boot recovery writes first frame");
    for (auto& emitter : frame.emitters)
        emitter.color = {160U, 0U, 0U};
    Check(seed.WriteFrame(token, 1U, frame) == store::Status::Ok &&
              seed.Commit(token) == store::Status::Ok,
          "boot recovery publishes final frame");
    const auto generation = seed.Generation();
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_litchcraft_boot_recovery_worker.json";
    fxe::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "boot recovery loads normal settings");
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(settingsConfig);
    worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter);
    Check(worker.Begin(settings), "boot recovery starts the actual production worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); }, "boot worker waiting for runtime");
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = 256U;
    config.max_led_current_ma = 12000U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    auto& session = board.Litchcraft();
    struct WakeContext {
        Worker& worker;
        modules::LitchcraftSession& session;
    } wake{worker, session};
    session.AttachFrameStream(&worker.LitchcraftFrameStream(), [](void* context, const bool playing, const uint32_t id, const bool active) {
            auto& owner = *static_cast<WakeContext*>(context);
            if (owner.session.IsBootArtworkPlaying()) owner.worker.NotifyLitchcraftFramePlayback(playing);
            else owner.worker.NotifyLitchcraftFrameSession(playing,id,active); }, &wake);
    board.SettingsForPersistenceTest().Doc()["BOOT_DISPLAY"] = "artwork";
    board.ObserveBootArtworkRecovery(store::Status::NotConfigured);
    board.ObserveBootHallMask(0ULL);
    board.Advance(0U);
    if (!wakeMode) board.BeginBootDisplay();
    board.ObserveHallMask(0ULL);
    const auto boardIsBlack = [&] {
        const auto& output = board.PhysicalFrame();
        for (uint16_t index = 0U; index < 256U; ++index) {
            const auto& pixel = output.pixels[index];
            if (pixel.r != 0U || pixel.g != 0U || pixel.b != 0U || pixel.w != 0U) return false;
        }
        return true;
    };
    const uint32_t firstFrameSequence = board.PhysicalFrame().frame_sequence;
    if (wakeMode) Check(boardIsBlack(), "fresh wake owner must not display Chess before initial Store recovery");
    struct Barrier {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    } barrier;
    worker.SetLitchcraftFrameBeforeSyncTestHook([](void* context) {
        auto& gate = *static_cast<Barrier*>(context);
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (gate.released)
            return;
        gate.entered = true;
        gate.changed.notify_all();
        if (!gate.changed.wait_for(lock, std::chrono::seconds(3), [&] { return gate.released; }))
            std::abort();
    },
                                                &barrier);
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker.NotifyLitchcraftFramePlayback();
    {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        Check(barrier.changed.wait_for(lock, std::chrono::seconds(3), [&] { return barrier.entered; }),
              "actual worker reaches post-recovery/pre-publication checkpoint");
    }
    const auto recovery = static_cast<store::Status>(worker.GetDiagnostics().frameStoreRecovery);
    Check(recovery == store::Status::Ok && worker.LitchcraftFrameStream().Generation() == 0U,
          "actual worker has recovered valid bytes before publishing stream authority");
    board.ObserveBootArtworkRecovery(recovery);
    board.Advance(1000U); // Run real boot/render ownership while the worker is held.
    Check(!session.IsBootArtworkPlaying(), "pre-publication render keeps services running without invented artwork");
    if (wakeMode) {
        Check(boardIsBlack(), "recovery Ok before Configure must not reveal Chess during wake");
        bool statusLit = false;
        const auto& output = board.PhysicalFrame();
        for (uint16_t index = 256U; index < output.led_count; ++index) {
            const auto pixel = output.pixels[index];
            statusLit = statusLit || pixel.r != 0U || pixel.g != 0U || pixel.b != 0U;
        }
        Check(output.frame_sequence > firstFrameSequence && statusLit,
              "unresolved mode still completes LED output with authoritative status");
    }
    {
        std::lock_guard<std::mutex> lock(barrier.mutex);
        barrier.released = true;
        barrier.changed.notify_all();
    }
    worker_fixture::Wait([&] {
        return worker.LitchcraftFrameStream().FrameCount() == 2U &&
            (!wakeMode || worker.LitchcraftFrameStream().TryPresentRequested()) && worker_fixture::atWait.load();
    },
                         "actual worker publishes and pages recovered artwork");
    // No session Update has run since the worker published. The existing
    // generation-mismatch fast path must expose the manifest before capture.
    if (wakeMode) Check(session.IsCanvasVisible() == craft &&
                            !session.IsInitialStoreModePending(),
        "publication after the last session update already exposes the saved mode");
    board.ObserveBootArtworkRecovery(static_cast<store::Status>(worker.GetDiagnostics().frameStoreRecovery));
    board.Advance(0U);
    if (wakeMode) {
        Check(session.IsActive() == craft && !session.IsBootArtworkPlaying(),
              "fresh wake adopts the saved mode directly without a boot episode");
        Check(craft ? board.PhysicalFrame().pixels[0U].r == 20U : !boardIsBlack(),
              "first resolved wake frame is saved Craft or normal inactive Chess");
        if (!craft) {
            Check(board.ObserveHallMask(1ULL << 52U) == VIRTUAL_LITCH_OK &&
                      board.Advance(1U) == VIRTUAL_LITCH_OK && !boardIsBlack() &&
                      !session.IsActive() && !session.IsInitialStoreModePending(),
                  "inactive restoration releases occupied Chess correction output");
        }
        { std::lock_guard<std::mutex> lock(worker_fixture::mutex);
          worker_fixture::stopping = true; worker_fixture::changed.notify_all(); }
        worker_fixture::worker.join();
        Check(files.Remove(), "wake mode journey removes its own Store");
        std::printf("[WakeModeRecoveryV33] saved=%s held=1000ms pending=black resolved=%s next=normal-mode bypass=SDK/electrical/sleep-reset-hardware\n", craft ? "Craft" : "Chess", craft ? "Craft" : "Chess");
        return;
    }
    const auto checkpoint = [&](const char* stage, const char* next) {
        const auto& output = board.PhysicalFrame();
        const auto text = board.DisplayText();
        const auto pixel = output.pixels[0U];
        std::printf("[BootArtworkWorkerRecoveryV33] stage=%s t=%u Hall=empty mode=%s selected=%u playing=%u sequence=%u board0=%u/%u/%u/%u text=\"%.*s\" next=%s\n",
            stage, fxe::Millis(), session.IsActive() ? "Craft" : "Chess", unsigned(session.SelectedFrame()),
            unsigned(session.IsBootArtworkPlaying()), output.frame_sequence,
            unsigned(pixel.r), unsigned(pixel.g), unsigned(pixel.b), unsigned(pixel.w),
            static_cast<int>(text.size()), text.empty() ? "" : text.data(), next);
    };
    Check(!session.IsBootArtworkPlaying() && boardIsBlack(),
          "resolved stored-artwork admission keeps the pending physical board black");
    checkpoint("pending", "wait-for-first-artwork-frame");
    const uint32_t captureAtMs = fxe::Millis();
    bool firstFramePresented = false;
    for (uint8_t capture = 0U; capture < 3U && !firstFramePresented; ++capture) {
        const uint32_t waitEntries = worker_fixture::waitEntries.load();
        const uint32_t sequence = board.PhysicalFrame().frame_sequence;
        Check(board.Advance(0U) == VIRTUAL_LITCH_OK && fxe::Millis() == captureAtMs,
              "next artwork owner capture does not consume visible dwell");
        firstFramePresented = session.IsBootArtworkPlaying() && session.SelectedFrame() == 0U &&
            board.PhysicalFrame().pixels[0U].r == 20U &&
            board.PhysicalFrame().frame_sequence > sequence;
        if (!firstFramePresented && capture + 1U < 3U) {
            worker_fixture::Wait([&] {
                return worker_fixture::atWait.load() && worker_fixture::waitEntries.load() > waitEntries;
            }, "first artwork page reaches the next owner capture");
        }
    }
    Check(session.IsBootArtworkPlaying() && session.SelectedFrame() == 0U &&
              board.PhysicalFrame().pixels[0U].r == 20U && firstFramePresented,
          "intermediate recovery success cannot permanently skip the saved first frame");
    const uint32_t firstFrameAcceptedMs = fxe::Millis();
    checkpoint("frame0", "hold-first-frame-100ms");
    board.Advance(99U);
    Check(session.IsBootArtworkPlaying() && session.SelectedFrame() == 0U &&
              board.PhysicalFrame().pixels[0U].r == 20U &&
              fxe::Millis() - firstFrameAcceptedMs == 99U,
          "worker-backed first frame retains its complete accepted dwell");
    checkpoint("frame0-plus99", "finish-first-frame-dwell");
    const uint32_t waitsBeforeSuccessor = worker_fixture::waitEntries.load();
    board.Advance(1U);
    worker_fixture::Wait([&] {
        return worker_fixture::atWait.load() && worker_fixture::waitEntries.load() > waitsBeforeSuccessor;
    }, "boot worker successor is serviced");
    board.Advance(0U);
    Check(session.SelectedFrame() == 1U && board.PhysicalFrame().pixels[0U].r == 40U &&
              fxe::Millis() - firstFrameAcceptedMs == 100U,
          "actual worker successor reaches physical output in order");
    checkpoint("frame1", "hold-final-frame-100ms");
    board.Advance(99U);
    Check(session.IsBootArtworkPlaying(), "worker-backed final frame holds full dwell before fade");
    board.Advance(1U);
    board.Advance(250U);
    Check(!session.IsBootArtworkPlaying() && session.IsActive() == craft && !session.IsAnimationPlaying(),
          "worker boot episode returns to saved mode and paused preference");
    checkpoint("complete", craft ? "continue-saved-Craft" : "continue-Chess");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    store::FrameStore reboot(files.StoreOps());
    Check(reboot.Recover() == store::Status::Ok && reboot.Generation() == generation &&
              reboot.CurrentManifest().active == craft && !reboot.CurrentManifest().playbackOnEntry,
          "actual worker cosmetic playback preserves saved root, mode and preference");
    Check(files.Remove(), "boot worker removes its temporary artwork banks");
    std::printf(
        "[BootArtworkWorkerRecoveryV33] Hall=empty recovery=Ok stream=unpublished held=1000ms "
        "first=0/20red final=1/40red dwell=100ms fade=250ms mode=%s root=unchanged bypass=SDK/radio/electrical\n",
        craft ? "Litchcraft" : "Play");
}
void RunWakeModeTerminalWorkerLane(const bool corrupt, const bool disabled = false) {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    (void)::mkdir("build", 0777);
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    firmware::LitchcraftFrameFile files;
    Check(files.Remove(), "terminal wake fixture owns clean Store files");
    if (corrupt) {
        std::array<uint8_t, store::ROOT_BYTES> invalid{};
        invalid[0U] = 0xa5U; // A zero-filled root is canonical blank, not corrupt.
        auto* file = std::fopen(firmware::LitchcraftFrameFile::DEFAULT_ROOT0_PATH, "wb");
        Check(file != nullptr, "terminal wake fixture opens the real root file");
        if (file != nullptr) {
            Check(std::fwrite(invalid.data(), 1U, invalid.size(), file) == invalid.size(),
                  "terminal wake fixture writes a complete invalid root");
            std::fclose(file);
        }
    }
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_litchcraft_wake_terminal.json";
    fxe::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "terminal wake loads normal settings");
    if (disabled) settings.DisablePersistenceForSession();
    hardware::ArtworkTestArbiter arbiter;
    Worker worker(settingsConfig);
    worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter);
    Check(worker.Begin(settings), "terminal wake starts the actual worker");
    worker_fixture::Wait([&] { return worker_fixture::atWait.load(); });
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    board.Litchcraft().AttachFrameStream(&worker.LitchcraftFrameStream());
    board.ObserveBootHallMask(0ULL);
    board.Advance(0U);
    Check(board.Litchcraft().IsInitialStoreModePending() == !disabled,
          "disabled storage releases initial presentation without waiting for recovery");
    worker.NotifyLitchcraftFrameRuntimeReady();
    worker_fixture::Wait([&] {
        return !worker.LitchcraftFrameStream().InitialRecoveryPending() && worker_fixture::atWait.load();
    }, "terminal wake result releases boot presentation");
    board.Advance(0U);
    bool lit = false;
    for (uint16_t index = 0U; index < 256U; ++index) {
        const auto pixel = board.PhysicalFrame().pixels[index];
        lit = lit || pixel.r != 0U || pixel.g != 0U || pixel.b != 0U;
    }
    Check(lit && !board.Litchcraft().IsActive(),
          "absent, failed or disabled Store releases normal Chess output");
    Check(disabled || worker.GetDiagnostics().frameStoreRecovery ==
              static_cast<uint8_t>(corrupt ? store::Status::Corrupt : store::Status::NoArtwork),
          "terminal wake follows the actual Store recovery result");
    { std::lock_guard<std::mutex> lock(worker_fixture::mutex);
      worker_fixture::stopping = true; worker_fixture::changed.notify_all(); }
    worker_fixture::worker.join();
    Check(files.Remove(), "terminal wake removes its own Store files");
    std::printf("[WakeModeRecoveryV33] terminal=%s resolved=Chess next=normal-mode\n",
                disabled ? "disabled" : corrupt ? "corrupt" : "absent");
}
void InitialStoreModeExplicitChoice() {
    modules::litchcraft_frame_stream::FrameStream stream;
    modules::LitchcraftSession session;
    session.AttachFrameStream(&stream);
    Check(!session.IsInitialStoreModePending(), "unavailable worker cannot hold normal presentation");
    stream.BeginInitialRecovery();
    Check(session.IsInitialStoreModePending(), "actual stream boot fact owns unresolved mode");
    session.Configure(static_cast<uint8_t>(hardware::BoardRevision::V3_3S3), 1U);
    const auto enter = LegacyLitchcraftRequest(modules::LitchcraftSession::Op::Enter, 0U, 1U);
    Check(session.HandleCompactMode(enter.bytes.data(), enter.size, true, 0ULL) ==
              modules::LitchcraftSession::Status::Ok, "explicit mode enters through the canonical compact owner");
    Check(!session.IsInitialStoreModePending(), "new explicit mode decision supersedes initial recovery hold");
}
} // namespace
