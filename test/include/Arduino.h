#pragma once

// Minimal SDK timing boundary for native tests that compile Arduino owners.
// The fixture provides the clock; no device API is emulated here.
void delay(unsigned long milliseconds);
