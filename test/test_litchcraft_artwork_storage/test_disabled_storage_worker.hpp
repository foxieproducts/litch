#pragma once

namespace {
void RunDisabledStorageWorkerLane() {
    using Worker = firmware::ArtworkSettingsPersistenceWorker;
    namespace store = modules::litchcraft_frame_store;
    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = true;
    worker_fixture::atWait.store(false);
    fxe::ManualMillis::SetNowMs(30000U);
    firmware::LitchcraftFrameFile frameFiles;
    Check(frameFiles.Remove(), "disabled storage starts with no frame banks");
    fxe::SettingsStoreConfig config;
    config.path = "build/test_disabled_storage_worker.json";
    fxe::SettingsStore settings(config);
    settings.Doc()["GAME_RAM"] = "preserved";
    hardware::ArtworkTestArbiter arbiter;
    MemoryStorage memory;
    Worker worker(config);
    Check(worker.SetLitchcraftArtworkStorageOps(memory.ChunkedOps()) &&
              (worker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), worker.Begin(settings)), "disabled storage starts existing worker");
    std::array<uint8_t, 32U> artwork{};
    artwork.fill(7U);
    uint32_t receipt = 0U;
    Check(worker.EnqueueLitchcraftArtworkTracked(artwork.data(), artwork.size(), receipt),
          "artwork is admitted before degradation");
    settings.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed);
    settings.DisablePersistenceForSession();
    worker.NotifyLitchcraftFrameSession(false, 700U, true);
    worker.NotifyLitchcraftFrameRuntimeReady();
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::paused = false;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::Wait([&] {
        return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed &&
            worker_fixture::atWait.load();
    }, "disabled queued artwork cancellation");
    Check(memory.replaceCalls == 0U && worker.ReleaseSaveReceipt(receipt),
          "queued artwork fails exactly without writing or claiming Saved");
    Check(worker.IsIdle() && !worker.LitchcraftFrameModeSavePending() &&
              settings.Doc()["GAME_RAM"].as<std::string>() == "preserved",
          "disabled mode retains RAM game and releases automatic storage barrier");
    Check(!worker.EnqueueLitchcraftArtworkTracked(artwork.data(), artwork.size(), receipt) &&
              !worker.RequestLitchcraftFrameClear() && !worker.RequestSettingsReadDiagnostic(),
          "disabled storage rejects fresh artwork, clear and benchmark writes");
    store::FrameStore persisted(frameFiles.StoreOps());
    Check(persisted.Recover() == store::Status::NoArtwork,
          "disabled active mode never creates an automatic blank filesystem root");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();

    worker_fixture::notified = false;
    worker_fixture::stopping = false;
    worker_fixture::paused = false;
    worker_fixture::atWait.store(false);
    store::Manifest manifest{};
    manifest.active = true;
    store::TransactionToken token{};
    store::Frame first{};
    first.emitters[0U].color = {1U, 2U, 3U};
    Check(persisted.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
              persisted.WriteFrame(token, 0U, first) == store::Status::Ok &&
              persisted.Commit(token) == store::Status::Ok,
          "degradation journey seeds a real durable frame");
    fxe::SettingsStore healthy(config);
    Worker paintWorker(config);
    Check((paintWorker.SetStorageAccessTestHook(&hardware::ArtworkTestArbiter::ObserveStorage, &arbiter), paintWorker.Begin(healthy)), "paint degradation starts healthy worker");
    paintWorker.NotifyLitchcraftFrameRuntimeReady();
    auto& stream = paintWorker.LitchcraftFrameStream();
    worker_fixture::Wait([&] {
        return stream.FrameCount() == 1U && stream.TryPresentRequested() &&
            worker_fixture::atWait.load();
    }, "paint degradation presents durable frame");
    Check(stream.PaintDisplayed(0U, 0U, 1U, {91U, 92U, 93U}, true),
          "human brush edit is acknowledged in RAM");
    healthy.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed);
    healthy.DisablePersistenceForSession();
    fxe::ManualMillis::SetNowMs(40000U);
    paintWorker.NotifyLitchcraftFramePlayback(false);
    worker_fixture::Wait([&] {
        return paintWorker.IsIdle() && worker_fixture::atWait.load();
    }, "dirty RAM paint releases disabled automatic writer");
    store::Frame displayed{};
    uint16_t index = 99U;
    Check(stream.HasUnpersistedEdit() && stream.CopyDisplayed(displayed, index) &&
              displayed.emitters[0U].color.r == 91U,
          "session-only painting retains visible unsaved RAM edit");
    store::FrameStore reboot(frameFiles.StoreOps());
    store::Frame old{};
    Check(reboot.Recover() == store::Status::Ok && reboot.ReadFrame(0U, old) == store::Status::Ok &&
              old.emitters[0U].color.r == 1U,
          "disabled edit and mode service never replace the last durable artwork");
    {
        std::lock_guard<std::mutex> lock(worker_fixture::mutex);
        worker_fixture::stopping = true;
        worker_fixture::changed.notify_all();
    }
    worker_fixture::worker.join();
    Check(frameFiles.Remove(), "disabled storage journey removes test banks");
    std::puts("[DISABLED_STORAGE_WORKER_JOURNEY] queued_artwork=failed writes=0 "
              "mode_root=absent dirty_paint=RAM durable_paint=old idle=1");
}
} // namespace
