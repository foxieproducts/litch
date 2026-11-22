#include <Arduino.h>

#include "foxie_embedded/console_transport_stream.hpp"

#include "app.hpp"

namespace {
fxe::StreamConsoleTransport g_consoleTransport(Serial);
App g_app(g_consoleTransport);
} // namespace

void setup() {
    Serial.begin(115200);

    // Some boards need a moment for USB-serial to come up.
    delay(50);

    g_app.Begin();
}

void loop() {
    g_app.Update();
}
