#pragma once

#include <array>
#include <cstdint>

#if defined(LITCH_HW_VARIANT_S3)
#include "hardware/pins_esp32s3.hpp"
#else
#include "hardware/pins_esp32c3.hpp"
#endif
