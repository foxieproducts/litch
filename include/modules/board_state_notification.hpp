#pragma once

#include "modules/board_health_notification.hpp"
#include "modules/settings_store.hpp"

namespace fxe { class Console; }

namespace modules {

// A complete bootstrap record, never a second mutable board-state owner.
// Entries in settings retain the deployed visible descriptor index order.
using BoardStateNotificationLine = fxe::FixedString<2048>;

bool TryFormatBoardStateNotification(
    const HomeStateNotificationLine& home,
    const BoardHealthLine& health,
    const SettingsStore& settings,
    BoardStateNotificationLine& outLine, uint32_t receipt = 0U);

struct BoardStatePublication {
    const HomeStateNotificationLine& home;
    const BoardHealthLine& health;
    const SettingsStore& settings;
    bool force;
    uint32_t receipt = 0U;
};

// Shared firmware/VirtualLitch output path. Empty Home means no Home change;
// health can still publish independently. The existing caller owns admission.
void WriteBoardStatePublication(fxe::Console& console,
                               BoardHealthLine& lastHealth,
                               const BoardStatePublication& publication);

} // namespace modules
