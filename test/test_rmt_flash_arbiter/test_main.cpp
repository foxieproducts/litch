#include <cstdio>
#include <chrono>
#include <thread>

#include "hardware/rmt_flash_arbiter.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint32_t ConcurrentClock() { return 0U; }

void ConcurrentLeases() {
    fxe::ScopedMillisProviderOverride clock(ConcurrentClock);
    hardware::RmtFlashArbiter arbiter;
    std::atomic<unsigned> flash{0U}, render{0U}, failures{0U};
    std::atomic<bool> done{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const auto wait = [&] {
        if (std::chrono::steady_clock::now() >= deadline) {
            ++failures;
            return false;
        }
        std::this_thread::yield();
        return true;
    };
    const auto physical = [&] {
        if (flash.fetch_add(1U) != 0U || render.load() != 0U) ++failures;
        std::this_thread::yield();
        flash.fetch_sub(1U);
    };
    const auto writer = [&] {
        for (unsigned i = 0U; i < 200U; ++i) {
            arbiter.RequestFlash();
            while (!arbiter.TryBeginRequestedFlash()) {
                if (!wait()) { arbiter.CancelFlashRequest(); return; }
            }
            physical();
            arbiter.EndFlash();
        }
    };
    std::thread renderer([&] {
        while (!done.load()) {
            arbiter.RequestRender();
            if (arbiter.TryBeginRequestedRender()) {
                if (render.fetch_add(1U) != 0U || flash.load() != 0U) ++failures;
                std::this_thread::yield();
                render.fetch_sub(1U);
                arbiter.EndRender();
            } else if (!wait()) { arbiter.CancelRenderRequest(); return; }
        }
        arbiter.CancelRenderRequest();
    });
    std::thread first(writer), second(writer);
    first.join(); second.join();
    done.store(true);
    renderer.join();
    Check(failures.load() == 0U, "concurrent writers and renderer never overlap and make bounded progress");
}

} // namespace

int main() {
    std::puts("[RMT_FLASH_ARBITER] start");
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    hardware::RmtFlashArbiter arbiter;
    const uint32_t postRequestFrame = arbiter.NextRenderGeneration();

    Check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()),
          "current-transfer hardware access does not require a future frame");
    arbiter.EndFlash();
    Check(arbiter.TryBeginRender(), "renderer acquires the idle output");
    Check(!arbiter.TryBeginFlash(postRequestFrame),
          "flash cannot start during an RMT transmission");
    arbiter.EndRender();

    Check(arbiter.CompletedRenderGeneration() == postRequestFrame,
          "completed RMT transmission advances the render generation");
    Check(arbiter.TryBeginFlash(postRequestFrame),
          "flash starts after the requested frame has completed");
    Check(!arbiter.TryBeginRender(),
          "renderer defers while flash owns the shared hardware window");
    arbiter.EndFlash();

    // Reproduce the observed long primitive, separately from time spent
    // waiting for the post-request frame. Also cover millis wrap at runtime.
    hardware::RmtFlashArbiter measured;
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 1120U);
    const auto requestedFrame = measured.NextRenderGeneration();

    fxe::ManualMillis::AdvanceMs(1000U);
    Check(measured.TryBeginRender(), "timing fixture completes requested frame");
    measured.EndRender();
    Check(measured.TryBeginFlash(requestedFrame), "timing fixture acquires after wait");
    fxe::ManualMillis::AdvanceMs(245U);
    Check(!measured.TryBeginRender(), "long primitive reproduces denied LED delivery");
    measured.EndFlash();
    const auto measuredHold = measured.GetFlashDiagnostics();
    Check(measuredHold.accesses == 1U && measuredHold.lastHoldMs == 245U &&
              measuredHold.maxHoldMs == 245U,
          "flash timing excludes the 1000ms frame wait and handles millis wrap");
    Check(measured.TryBeginRender(), "LED delivery recovers after timed primitive");
    measured.EndRender();
    Check(measured.TryBeginFlash(measured.CompletedRenderGeneration()), "short follow-up acquires");
    fxe::ManualMillis::AdvanceMs(3U);
    measured.EndFlash();
    Check(measured.GetFlashDiagnostics().lastHoldMs == 3U &&
              measured.GetFlashDiagnostics().maxHoldMs == 245U,
          "short follow-up does not hide the observed longest primitive");

    const uint32_t interSegmentFrame = arbiter.CompletedRenderGeneration();
    Check(arbiter.TryBeginRender(), "renderer resumes after flash completes");
    arbiter.EndRender();

    const uint32_t generationBeforeFailure =
        arbiter.CompletedRenderGeneration();
    Check(arbiter.TryBeginRender(),
          "renderer acquires output before a failed transfer");
    arbiter.EndRender(false);
    Check(arbiter.CompletedRenderGeneration() == generationBeforeFailure,
          "failed transfer does not publish a completed render generation");
    Check(!arbiter.TryBeginFlash(generationBeforeFailure + 1U),
          "flash waiting for a visible frame stays blocked after failed transfer");
    Check(!arbiter.TryBeginFlash(generationBeforeFailure),
          "even current-generation flash stays quarantined while transfer completion is uncertain");
    Check(arbiter.TryBeginRender(),
          "renderer can retry after a failed transfer releases its lease");
    arbiter.EndRender();
    Check(arbiter.TryBeginFlash(generationBeforeFailure + 1U),
          "completed retry admits the waiting flash access");
    arbiter.EndFlash();
    Check(arbiter.TryBeginFlash(interSegmentFrame),
          "the next flash segment starts after the yielded frame");
    arbiter.EndFlash();

    const uint32_t currentFrame = arbiter.CompletedRenderGeneration();
    Check(arbiter.TryBeginFlash(currentFrame),
          "shutdown flush can use the current completed frame");
    Check(!arbiter.TryBeginFlash(currentFrame),
          "only one flash operation can own the window");
    arbiter.EndFlash();

    hardware::RmtFlashArbiter pending;
    Check(pending.TryBeginRender(), "pending fixture starts one actual transfer");
    pending.RequestFlash();
    Check(!pending.TryBeginRender() && !pending.TryBeginRequestedFlash(),
          "pending flash permits only the existing transfer to finish");
    pending.RequestFlash();
    pending.RequestRender();
    pending.EndRender();
    Check(!pending.TryBeginRequestedRender() &&
              !pending.TryBeginFlash(pending.CompletedRenderGeneration()),
          "new render or an unreserved writer cannot commandeer pending flash");
    Check(pending.TryBeginRequestedFlash() && !pending.TryBeginRequestedFlash(),
          "two legitimate pending writers still have one hardware owner");
    pending.EndFlash();
    Check(!pending.TryBeginRequestedFlash() && pending.TryBeginRequestedRender(),
          "a ready frame gets a physical turn between waiting primitives");
    Check(!pending.TryBeginRequestedFlash(), "queued flash never overlaps the ready frame");
    pending.EndRender();
    pending.RequestRender();
    Check(!pending.TryBeginRequestedRender() && pending.TryBeginRequestedFlash(),
          "one completed frame gives waiting flash its physical turn");
    pending.EndFlash();
    pending.CancelRenderRequest();
    Check(pending.TryBeginRender(), "LED resumes when the last physical primitive ends");
    pending.RequestFlash();
    pending.EndRender(false);
    Check(!pending.TryBeginRequestedFlash() && pending.TryBeginRender(),
          "uncertain hardware remains quarantined and can complete its existing recovery");
    pending.EndRender();
    Check(pending.TryBeginRequestedFlash(), "confirmed hardware recovery releases pending flash");
    pending.EndFlash();
    pending.RequestFlash();
    pending.RequestFlash();
    Check(pending.TryBeginRequestedFlash(), "cancel fixture has one active and one waiting owner");
    pending.CancelFlashRequest();
    Check(!pending.TryBeginRender() && !pending.TryBeginRequestedFlash(),
          "canceling an unconsumed claim cannot release the active owner's hardware");
    pending.EndFlash();
    Check(pending.TryBeginRender(), "cancel removes exactly the caller's pending claim");
    pending.EndRender();
    pending.RequestRender();
    pending.RequestFlash();
    pending.CancelRenderRequest();
    Check(pending.TryBeginRequestedFlash(),
          "a canceled ready frame cannot strand a waiting physical primitive");
    pending.EndFlash();

    ConcurrentLeases();

    if (g_failures == 0) {
        std::puts("[RMT_FLASH_ARBITER] ok");
        return 0;
    }
    std::printf("[RMT_FLASH_ARBITER] failures=%d\n", g_failures);
    return 1;
}
