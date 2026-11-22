#include <cstdint>
#include <cstdio>

#include "foxie_embedded/deadline_timer.hpp"
#include "foxie_embedded/deadline_timer_manager.hpp"
#include "foxie_embedded/millis.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqU32(const uint32_t got, const uint32_t want, const char* msg) {
    if (got == want) {
        return;
    }

    std::printf("[FAIL] %s (got=%u want=%u)\n", msg, got, want);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[DEADLINE_TIMER] start");

    fxe::ManualMillis::SetNowMs(0);
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    {
        fxe::DeadlineTimer timer;
        uint32_t fired = 0;
        timer.SetCallback([&]() { ++fired; });
        timer.Start(10);

        Check(!timer.IsExpired(), "timer not expired at start");
        Check(!timer.Update(), "Update false before deadline");
        CheckEqU32(fired, 0u, "callback not fired early");

        fxe::ManualMillis::AdvanceMs(9);
        Check(!timer.Update(), "Update false at 9ms");
        CheckEqU32(fired, 0u, "callback not fired at 9ms");

        fxe::ManualMillis::AdvanceMs(1);
        Check(timer.Update(), "Update true at 10ms");
        CheckEqU32(fired, 1u, "callback fired once at 10ms");
        Check(!timer.Update(), "Update false after firing");
        CheckEqU32(fired, 1u, "callback fires once");
    }

    {
        fxe::DeadlineTimer timer;
        uint32_t fired = 0;
        timer.SetCallback([&]() { ++fired; });
        timer.Start(5);
        timer.Stop();

        fxe::ManualMillis::AdvanceMs(5);
        Check(!timer.Update(), "stopped timer does not fire");
        CheckEqU32(fired, 0u, "stopped timer did not fire callback");
    }

    {
        fxe::DeadlineTimerManager<2> manager;
        manager.Reset();

        uint32_t firedA = 0;
        uint32_t firedB = 0;
        uint32_t firedC = 0;

        const int idA = manager.Start(3, [&]() { ++firedA; });
        const int idB = manager.Start(7, [&]() { ++firedB; });
        const int idC = manager.Start(1, [&]() { ++firedC; });
        Check(idC == -1, "manager rejects over-capacity start");

        fxe::ManualMillis::AdvanceMs(3);
        manager.Update();
        CheckEqU32(firedA, 1u, "manager fired A at 3ms");
        CheckEqU32(firedB, 0u, "manager did not fire B at 3ms");

        const int idC2 = manager.Start(2, [&]() { ++firedC; });
        Check(idC2 >= 0, "manager reused freed slot");

        fxe::ManualMillis::AdvanceMs(2);
        manager.Update();
        CheckEqU32(firedC, 1u, "manager fired C at 5ms");
        CheckEqU32(firedB, 0u, "manager still waiting for B");

        fxe::ManualMillis::AdvanceMs(2);
        manager.Update();
        CheckEqU32(firedB, 1u, "manager fired B at 7ms");
    }

    if (g_failures == 0) {
        std::puts("[DEADLINE_TIMER] ok");
        return 0;
    }

    std::printf("[DEADLINE_TIMER] failures=%d\n", g_failures);
    return 1;
}
