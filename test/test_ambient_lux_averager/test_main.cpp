#include <cstdio>

#include "modules/ambient_lux_averager.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

modules::AmbientLuxAveragerConfig MakeConfig() {
    modules::AmbientLuxAveragerConfig cfg;
    cfg.windowMs = 0U;
    cfg.smoothMs = 650U;
    cfg.smoothDownMs = 200U;
    cfg.hysteresis01 = 0.03f;
    cfg.driftMs = 1200U;
    cfg.driftDownMs = 450U;
    cfg.fullLux = 200U;
    cfg.luxOffset = 1.0f;
    cfg.useLogCurve = true;
    cfg.curveGamma = 1.8f;
    cfg.endpointRawEpsilon01 = 0.001f;
    cfg.endpointSnapEpsilon01 = 0.01f;
    return cfg;
}

} // namespace

int main() {
    std::puts("[AMBIENT_LUX_AVERAGER] start");

    {
        modules::AmbientLuxAverager avg(MakeConfig());

        // Start bright and ensure we begin at the max endpoint.
        avg.OnLuxSample(0U, 1000.0f, true);
        float smoothed = avg.ComputeSmoothedAmbientFactor01(0U);
        Check(smoothed == 1.0f, "high lux starts at exact max endpoint");

        // Sustained darkness should not jump immediately to min, but it must
        // eventually converge to exact zero.
        avg.OnLuxSample(50U, 0.0f, true);
        smoothed = avg.ComputeSmoothedAmbientFactor01(50U);
        Check(smoothed > 0.0f, "smoothing remains sticky on first low sample");

        for (uint32_t nowMs = 100U; nowMs <= 10000U; nowMs += 50U) {
            avg.OnLuxSample(nowMs, 0.0f, true);
            smoothed = avg.ComputeSmoothedAmbientFactor01(nowMs);
        }

        Check(smoothed == 0.0f, "sustained low lux converges to exact min endpoint");
    }

    {
        modules::AmbientLuxAverager avg(MakeConfig());

        // Start dark and ensure we begin at the min endpoint.
        avg.OnLuxSample(0U, 0.0f, true);
        float smoothed = avg.ComputeSmoothedAmbientFactor01(0U);
        Check(smoothed == 0.0f, "low lux starts at exact min endpoint");

        // Sustained high lux should not jump immediately to max, but it must
        // eventually converge to exact one.
        avg.OnLuxSample(50U, 1000.0f, true);
        smoothed = avg.ComputeSmoothedAmbientFactor01(50U);
        Check(smoothed < 1.0f, "smoothing remains sticky on first high sample");

        for (uint32_t nowMs = 100U; nowMs <= 10000U; nowMs += 50U) {
            avg.OnLuxSample(nowMs, 1000.0f, true);
            smoothed = avg.ComputeSmoothedAmbientFactor01(nowMs);
        }

        Check(smoothed == 1.0f, "sustained high lux converges to exact max endpoint");
    }

    if (g_failures == 0) {
        std::puts("[AMBIENT_LUX_AVERAGER] ok");
        return 0;
    }

    std::printf("[AMBIENT_LUX_AVERAGER] failures=%d\n", g_failures);
    return 1;
}
