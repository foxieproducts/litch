#pragma once

#include "foxie_embedded/console.hpp"

namespace modules {

// Optional, bounded text only. The Console owns its fortune cursor.
bool RegisterFoxConsoleCommand(fxe::Console& console);

} // namespace modules
