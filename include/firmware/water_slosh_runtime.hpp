#pragma once

#include <cstdint>

namespace modules::led_frame_composer { struct ComposeInputs; }
namespace firmware_internal {

struct FirmwareSystem;
void SuppressWaterSlosh(FirmwareSystem& sys);
void PollWaterSloshMotion(FirmwareSystem& sys);
uint8_t UpdateWaterSlosh(
    FirmwareSystem& sys,
    const modules::led_frame_composer::ComposeInputs& inputs);

} // namespace firmware_internal
