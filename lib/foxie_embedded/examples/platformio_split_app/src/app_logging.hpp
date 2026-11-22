#pragma once

#include "foxie_embedded/console.hpp"

// Route fxe::Info/Warn/Error output to a console instance so the prompt remains stable.
void ConfigureLoggerToConsole(fxe::Console& console);
