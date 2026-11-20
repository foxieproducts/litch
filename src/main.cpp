#ifdef ARDUINO

#include "firmware/firmware_app.hpp"
#include "logger.hpp"

void setup() {
    firmware::Setup();
}

void loop() {
    firmware::Loop();
}

#else

#ifndef PIO_UNIT_TESTING
int main() {
    logger::SetMinLevel(logger::Level::DEBUG);
    logger::Info("Litch native");

    // Placeholder: the future headless emulator/replay runner entrypoint.
    return 0;
}
#endif

#endif
