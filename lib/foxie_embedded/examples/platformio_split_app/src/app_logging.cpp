#include "app_logging.hpp"

#include "foxie_embedded/logger.hpp"

void ConfigureLoggerToConsole(fxe::Console& console) {
    fxe::SetOutputLineCallback([&console](const char* line) {
        if (!line) {
            return;
        }
        console.PrintLn("%s", line);
    });
}
