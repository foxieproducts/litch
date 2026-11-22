#pragma once
#include "driver/rmt.h"
#include <vector>

using esp_err_t = int;
using ledc_mode_t = unsigned;
using ledc_timer_t = unsigned;
using ledc_channel_t = unsigned;
constexpr unsigned LEDC_LOW_SPEED_MODE = 0U, LEDC_TIMER_3 = 3U;
constexpr unsigned LEDC_TIMER_14_BIT = 14U, LEDC_USE_APB_CLK = 1U;
constexpr unsigned LEDC_INTR_DISABLE = 0U;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
struct ledc_timer_config_t {
    unsigned speed_mode{}, duty_resolution{}, timer_num{}, freq_hz{}, clk_cfg{};
};
struct ledc_channel_config_t {
    int gpio_num{};
    unsigned speed_mode{}, channel{}, intr_type{}, timer_sel{}, duty{};
    int hpoint{};
    struct { unsigned output_invert{}; } flags;
};
namespace fixture::pwm {
// SDK boundary only: records configuration, pause/duty state and errors. It
// does not simulate PWM edges, MOSFET response, voltage or current.
enum class Api { Timer, Stop, Duty, Update, Resume, Channel, Pause };
struct Call { Api api; unsigned value; uint64_t atUs; };
struct State {
    unsigned calls = 0U, failAt = 0U, frequency = 4882U, duty = 37U;
    unsigned idle = 0U, badAttach = 0U, badConfig = 0U;
    bool paused = true, stopped = true;
    uint64_t zeroAtUs = 0U;
    std::vector<Call> history;
};
inline State state;
inline void Reset(unsigned failure = 0U) { state = State{}; state.failAt = failure; }
inline int Record(Api api, unsigned value = 0U) {
    state.history.push_back({api, value, fixture::nowUs});
    return ++state.calls == state.failAt ? -1 : 0;
}
}
inline int ledc_timer_config(const ledc_timer_config_t* c) {
    auto& s = fixture::pwm::state;
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Timer);
    if (result) return result;
    if (c->speed_mode != LEDC_LOW_SPEED_MODE || c->timer_num != 3U ||
        c->duty_resolution != 14U || c->freq_hz != 4882U || c->clk_cfg != LEDC_USE_APB_CLK)
        ++s.badConfig;
    s.frequency = c->freq_hz;
    // Pinned SDK resets the timer counter but preserves its paused state.
    return 0;
}
inline int ledc_stop(unsigned, unsigned channel, unsigned idle) {
    auto& s = fixture::pwm::state;
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Stop, idle);
    if (result) return result;
    if (channel != 7U) ++s.badConfig;
    s.stopped = true; s.idle = idle; return 0;
}
inline int ledc_set_duty(unsigned, unsigned channel, unsigned duty) {
    auto& s = fixture::pwm::state;
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Duty, duty);
    if (result) return result;
    if (channel != 7U) ++s.badConfig;
    s.duty = duty; return 0;
}
inline int ledc_update_duty(unsigned, unsigned) {
    auto& s = fixture::pwm::state;
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Update);
    if (result) return result;
    s.stopped = false;
    if (s.duty == 0U) s.zeroAtUs = fixture::nowUs;
    return 0;
}
inline int ledc_timer_resume(unsigned, unsigned) {
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Resume);
    if (!result) {
        fixture::pwm::state.paused = false;
        if (fixture::pwm::state.duty == 0U) fixture::pwm::state.zeroAtUs = fixture::nowUs;
    }
    return result;
}
inline unsigned ledc_get_freq(unsigned, unsigned) { return fixture::pwm::state.frequency; }
inline unsigned ledc_get_duty(unsigned, unsigned) { return fixture::pwm::state.duty; }
inline int ledc_channel_config(const ledc_channel_config_t* c) {
    auto& s = fixture::pwm::state;
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Channel);
    if (result) return result;
    if (s.paused || s.duty != 0U || c->duty != 0U ||
        fixture::nowUs - s.zeroAtUs < 5000U) ++s.badAttach;
    if (c->channel != 7U || c->timer_sel != 3U || c->speed_mode != LEDC_LOW_SPEED_MODE ||
        c->intr_type != LEDC_INTR_DISABLE || c->hpoint != 0 || c->flags.output_invert != 0U)
        ++s.badConfig;
    return 0;
}
inline int ledc_timer_pause(unsigned, unsigned) {
    const auto result = fixture::pwm::Record(fixture::pwm::Api::Pause);
    if (!result) fixture::pwm::state.paused = true;
    return result;
}
