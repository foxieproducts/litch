#include <stdint.h>

#include "modules/litch_max_search_policy.hpp"

#include "upstream_test.h"

static uint32_t delay_calls;
static uint32_t last_delay_ticks;

void vTaskDelay(const uint32_t ticks)
{
    delay_calls++;
    last_delay_ticks = ticks;
}

int main(void)
{
    const uint32_t interval = 512U;

    modules::CooperateLitchMaxSearch(nullptr, interval - 1u);
    MCUMAX_TEST_CHECK(delay_calls == 0u);

    modules::CooperateLitchMaxSearch(nullptr, interval);
    MCUMAX_TEST_CHECK(delay_calls == 1u);
    MCUMAX_TEST_CHECK(last_delay_ticks == 1u);

    modules::CooperateLitchMaxSearch(nullptr, interval + 1u);
    MCUMAX_TEST_CHECK(delay_calls == 1u);

    modules::CooperateLitchMaxSearch(nullptr, interval * 2u);
    MCUMAX_TEST_CHECK(delay_calls == 2u);
    MCUMAX_TEST_CHECK(last_delay_ticks == 1u);

    MCUMAX_TEST_CHECK(interval == 512U);

    return mcumax_test_finish();
}
