#pragma once

#include "console.hpp"
#include "modules/board_owner_pairing_runtime.hpp"

namespace modules {

// Registers only in the private owner-pairing preview. Consent requires a
// fresh local-terminal line and the exact live attempt, end, native connection,
// shared session and board-owned prompt generation printed by status.
bool RegisterOwnerPairingConsoleCommands(Console& console,
                                        BoardOwnerPairingRuntime& runtime);

} // namespace modules
