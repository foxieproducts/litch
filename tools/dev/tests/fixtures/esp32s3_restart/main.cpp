#include "fixture_sdk.hpp"
#include <cstdio>

namespace {
enum class Outcome { Restarted, Orphaned, AcquiredAfterStall };
int g_core;
unsigned g_options;
unsigned g_stall;
int g_owner;
int g_depth;
int g_acquisitions;
int g_restartCalls;
bool g_peerStalled;
}

int cpu_hal_get_core_id() { return g_core; }
unsigned ReadRegister(unsigned address) {
    return address == RTC_CNTL_OPTIONS0_REG ? g_options : g_stall;
}
void regi2c_enter_critical() {
    ++g_acquisitions;
    if (g_peerStalled) throw Outcome::AcquiredAfterStall;
    if (g_owner == 1 - g_core) {
        // The live peer can finish its critical section before teardown.
        g_owner = -1;
        g_depth = 0;
    }
    g_owner = g_core;
    ++g_depth;
}
extern "C" [[noreturn]] void __real_esp_restart_noos() {
    ++g_restartCalls;
    // SDK boundary model: stop the peer, then switch XTAL through the same
    // recursive lock. This models the observed orphan, not the whole SDK/SoC.
    g_peerStalled = true;
    if (g_owner == 1 - g_core) throw Outcome::Orphaned;
    if (g_owner == g_core) {
        ++g_depth;
        --g_depth;
    }
    throw Outcome::Restarted;
}
extern "C" [[noreturn]] void __wrap_esp_restart_noos();

int main() {
    int cases = 0;
    for (int core = 0; core < 2; ++core) {
        for (bool stalled : {false, true}) {
            for (int owner : {1 - core, -1, core}) {
                g_core = core;
                g_options = stalled ? (core == 0 ? 2U : 8U) : 0U;
                g_stall = stalled ? (core == 0 ? 0x02100000U : 0x84000000U) : 0U;
                g_owner = owner;
                g_depth = owner == -1 ? 0 : 2;
                g_acquisitions = g_restartCalls = 0;
                g_peerStalled = stalled;
                Outcome outcome = Outcome::Orphaned;
                try {
#if defined(LITCH_WITHOUT_RESTART_ORDERING_GUARD)
                    __real_esp_restart_noos();
#else
                    __wrap_esp_restart_noos();
#endif
                } catch (Outcome value) {
                    outcome = value;
                }
                const auto expected = stalled && owner == 1 - core
                    ? Outcome::Orphaned : Outcome::Restarted;
                if (outcome != expected || g_restartCalls != 1 ||
                    g_acquisitions != (stalled ? 0 : 1) ||
                    (!stalled && (g_owner != core ||
                        g_depth != (owner == core ? 3 : 1)))) {
                    std::printf("FAIL core=%d peer_stalled=%d owner=%d outcome=%d: normal restart abandoned peer lock or panic path changed\n",
                                core, stalled, owner, static_cast<int>(outcome));
                    return 1;
                }
                ++cases;
            }
        }
    }
    std::printf("PASS %d restart ordering/recursive ownership/panic cases\n", cases);
    return 0;
}
