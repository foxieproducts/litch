namespace {
void SeedBootPages(store::FrameStore& frames, stream::FrameStream& pager) {
    Check(frames.Recover() == store::Status::NoArtwork, "delivery witness starts with a fresh Store");
    store::Manifest manifest{};
    manifest.frameCount = 3U;
    store::TransactionToken token{};
    Check(frames.Begin(manifest, 0U, 81U, token) == store::Status::Ok,
          "delivery witness starts ordinary durable artwork transaction");
    for (uint16_t i = 0U; i < 3U; ++i) {
        store::Frame frame{};
        frame.durationMs = 100U;
        for (auto& emitter : frame.emitters)
            emitter.color = {uint8_t(80U + i * 40U + (i == 2U ? 40U : 0U)), 0U, 0U};
        Check(frames.WriteFrame(token, i, frame) == store::Status::Ok,
              "delivery witness saves distinguishable authored frames");
    }
    Check(frames.Commit(token) == store::Status::Ok &&
              pager.Configure(frames.CurrentManifest(), frames.Generation()),
          "delivery witness adopts the saved root through existing pager");
    bool firstReady = false;
    for (unsigned i = 0U; i < 32U; ++i) {
        if (pager.Service(frames, false) == stream::FrameStream::ServiceResult::LoadedRequested) {
            firstReady = true;
            break;
        }
    }
    Check(firstReady, "only the first page is preloaded; successors require actual pager service");
}
virtual_litch_config BootDeliveryConfig() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.max_led_current_ma = 12000U;
    config.brightness_scale_256 = 256U;
    return config;
}
void StoredArtworkComposeClockBoundary() {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium; store::FrameStore frames(medium.Ops()); stream::FrameStream pager;
    SeedBootPages(frames,pager); Service(pager,frames);
    modules::LitchcraftSession art; art.AttachFrameStream(&pager);
    modules::BootDisplay boot;
    boot.Begin(modules::BootDisplayChoice::StoredArtwork,true,{}); boot.ObserveBoard(true,0ULL);
    modules::BoardLedFrame semantic{};
    std::array<modules::Rgb,modules::EDGE_LED_COUNT> status{};
    for (uint16_t index=0U;index<3U;++index) {
        fxe::ManualMillis::SetNowMs(index*100U);
        boot.UpdateArtwork(art,false); art.Compose(semantic,status);
        Check(art.IsBootArtworkPlaying() && art.SelectedFrame()==index,
              "clock-boundary journey samples each authored frame through the boot owner");
        boot.Presented();art.NoteBootArtworkPresented(); Service(pager,frames);
    }
    fxe::ManualMillis::SetNowMs(299U);boot.UpdateArtwork(art,false);
    fxe::ManualMillis::SetNowMs(300U);art.Compose(semantic,status);
    hardware::PhysicalLedFrame accepted{};
    boot.Apply(accepted,false,true,&semantic);
    Check(art.IsBootArtworkPlaying() && art.SelectedFrame()==2U &&
              semantic[0U].left.r==200U && accepted[0U].r==100U,
          "crossing final deadline inside Compose retains the sampled final frame, never black or saved frame");
    modules::litchcraft_physical_presentation::ModeCrossfade<hardware::MAX_PHYSICAL_LED_COUNT> fade;
    fade.Presented(accepted);
    boot.UpdateArtwork(art,false);
    Check(boot.IsArtworkCrossfading() && !art.IsBootArtworkPlaying(),
          "the next boot-owner sample alone chooses final handoff");
    hardware::PhysicalLedFrame normal{};normal.fill({40U,0U,0U});
    fade.TransitionFromPresented(false,300U);fade.Apply(normal,false,300U);
    Check(normal[0U].r==100U,"final crossfade starts from the accepted authored frame across clock boundary");
    std::puts("[StoredArtworkComposeBoundaryV33] sampled=299ms compose=300ms final=2/200red source=100red fade=owner-only bypass=SDK/radio/optics");
}

void StoredArtworkDelayedAndDenied() {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    stream::FrameStream pager;
    SeedBootPages(frames, pager);
    const auto saved = medium.bytes;
    hardware::RmtFlashArbiter delivery;
    test_support::BoundedHintSession fixture(BootDeliveryConfig());
    auto& board = fixture.session;
    auto& art = board.Litchcraft();
    art.AttachFrameStream(&pager);
    board.SettingsForPersistenceTest().Doc()["BOOT_DISPLAY"] = "artwork";
    board.ObserveBootHallMask(0ULL);
    board.Advance(0U);
    const auto normal = board.PhysicalFrame();
    board.BindLedOutputArbiter(delivery);
    Check(delivery.TryBeginFlash(0U), "production flash owner denies the first render");
    board.BeginBootDisplay();
    board.ObserveHallMask(0ULL);
    board.Advance(1000U);
    Check(art.IsBootArtworkPlaying() && art.SelectedFrame() == 0U &&
              board.PhysicalFrame().frame_hash == normal.frame_hash,
          "denied first transfer cannot acknowledge, skip or consume the first authored dwell");
    delivery.EndFlash();
    board.Advance(0U);
    Check(board.PhysicalFrame().pixels[0U].r == 20U, "first accepted transfer shows authored frame zero");
    const auto first = board.PhysicalFrame();
    board.Advance(100U);
    board.Advance(1000U); // Do not service the requested successor yet.
    Check(art.IsBootArtworkPlaying() && art.SelectedFrame() == 0U &&
              board.PhysicalFrame().pixels[0U].r == first.pixels[0U].r,
          "delayed page holds the first actual frame without inventing a successor or starting the fade");
    Service(pager, frames);
    board.Advance(0U);
    Check(art.SelectedFrame() == 1U && board.PhysicalFrame().pixels[0U].r == 30U,
          "late successor presents in order instead of catching up past authored frames");
    board.Advance(99U);
    Check(art.SelectedFrame() == 1U && art.IsBootArtworkPlaying(), "late successor receives its full accepted dwell");
    const auto penultimate = board.PhysicalFrame();
    Check(delivery.TryBeginFlash(0U), "production flash owner denies final-frame transfer");
    board.Advance(1U);
    board.Advance(1000U);
    Check(art.SelectedFrame() == 2U && art.IsBootArtworkPlaying() &&
              board.PhysicalFrame().frame_hash == penultimate.frame_hash,
          "denied final transfer holds last delivered output and cannot start final dwell or fade");
    delivery.EndFlash();
    board.Advance(0U);
    Check(board.PhysicalFrame().pixels[0U].r == 50U, "accepted final transfer shows the authored final frame");
    board.Advance(99U);
    Check(art.IsBootArtworkPlaying() && art.SelectedFrame() == 2U && board.PhysicalFrame().pixels[0U].r == 50U,
          "final frame lasts its full dwell after actual transfer acceptance");
    board.Advance(1U);
    Check(!art.IsBootArtworkPlaying() && board.PhysicalFrame().pixels[0U].r == 50U,
          "crossfade begins from the final accepted frame only after complete final dwell");
    board.Advance(125U);
    const uint8_t midpoint = board.PhysicalFrame().pixels[0U].r;
    std::printf("[StoredArtworkFadeDeliveryV33] sourceR=50 midpointR=%u destinationR=%u finalDwell=100ms fadeElapsed=125ms\n", midpoint, normal.pixels[0U].r);
    Check(midpoint > std::min<uint8_t>(50U, normal.pixels[0U].r) &&
              midpoint < std::max<uint8_t>(50U, normal.pixels[0U].r),
          "final-frame fade has a real intermediate physical output");
    board.Advance(125U);
    Check(board.PhysicalFrame().pixels[0U].r == normal.pixels[0U].r &&
              !art.IsActive() && !art.IsAnimationPlaying() && medium.bytes == saved,
          "delayed/denied boot finishes at saved Play with unchanged artwork and preferences");
    std::puts(
        "[StoredArtworkDeliveryV33] Hall=empty first=denied1000ms accepted20red "
        "page1=delayed1000ms accepted30red final=denied1000ms accepted50red dwell=100ms fade=250ms "
        "next=Play root=unchanged bypass=SDK/radio/electrical");
}
void StoredArtworkRequestedPageFailure(bool corrupt) {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    Medium medium;
    store::FrameStore frames(medium.Ops());
    stream::FrameStream pager;
    SeedBootPages(frames, pager);
    const auto saved = medium.bytes;
    test_support::BoundedHintSession fixture(BootDeliveryConfig());
    auto& board = fixture.session;
    auto& art = board.Litchcraft();
    art.AttachFrameStream(&pager);
    board.SettingsForPersistenceTest().Doc()["BOOT_DISPLAY"] = "artwork";
    board.ObserveBootHallMask(0ULL);
    board.Advance(0U);
    const uint8_t normalRed = board.PhysicalFrame().pixels[0U].r;
    board.BeginBootDisplay();
    board.ObserveHallMask(0ULL);
    board.Advance(100U);
    medium.failReads = !corrupt;
    medium.corruptReads = corrupt;
    Service(pager, frames);
    Check(corrupt ? pager.RequestedFrameCorrupt() : pager.RequestedFrameFailed(),
          "requested boot page exposes actual Store corruption/I/O failure through existing pager");
    board.Advance(0U);
    Check(!art.IsBootArtworkPlaying() && !art.IsActive() && !art.IsAnimationPlaying() &&
              board.PhysicalFrame().pixels[0U].r == normalRed,
          "failed requested boot page cancels cleanly to saved mode instead of skipping authored frames");
    medium.failReads = false;
    medium.corruptReads = false;
    Service(pager, frames);
    board.Advance(500U);
    Check(!art.IsBootArtworkPlaying() && medium.bytes == saved,
          "page recovery cannot rearm the canceled episode or rewrite artwork");
    std::printf(
        "[StoredArtworkPageFailureV33] Hall=empty requested=1 failure=%s accepted=cancel "
        "next=Play no-rearm=1 root=unchanged bypass=SDK/radio/electrical\n",
        corrupt ? "CRC" : "I/O");
}
} // namespace
