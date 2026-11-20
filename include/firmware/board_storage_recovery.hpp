#pragma once

#include <array>
#include <cstdint>

namespace firmware {
// Main-owned publication state distinguishes accepted attempts even when a
// fast worker finishes before a Pending/Running frame can be published.
struct BoardStorageRecoveryPublication {
    uint32_t attempt = 0U;
    uint32_t publishedAttempt = 0U;
    uint32_t revision = 0U;
    std::array<uint8_t, 3U> body{};
};
} // namespace firmware

#ifdef ARDUINO
#include "console.hpp"
#include "modules/control_mutation_service.hpp"
namespace firmware_internal {
struct FirmwareSystem;
fxe::Console::StorageReformatStatus BoardStorageRecoveryStatus(FirmwareSystem& sys);
fxe::Console::StorageReformatStatus ConfirmBoardStorageRecovery(FirmwareSystem& sys);
modules::ControlCoordinatorResult RequestBoardStorageRecovery(void* context, uint16_t request);
modules::ControlCoordinatorResult PollBoardStorageRecovery(void* context, uint16_t request);
void PublishBoardStorageRecovery(FirmwareSystem& sys);
bool BoardStorageRecoveryBlocksOta(void* context);
} // namespace firmware_internal
#endif
