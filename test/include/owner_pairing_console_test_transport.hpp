#pragma once

#include <string>

#include "firmware/hybrid_console_transport.hpp"

namespace test_support {

struct OwnerPairingSerialEndpoint {
    std::string input;
    size_t offset = 0U;
    std::string output;
    int available() const { return static_cast<int>(input.size() - offset); }
    int read() { return available() > 0 ? input[offset++] : -1; }
    explicit operator bool() const { return true; }
    int availableForWrite() const { return 4096; }
    size_t write(const uint8_t* data, size_t size) {
        output.append(reinterpret_cast<const char*>(data), size);
        return size;
    }
};

struct OwnerPairingBleEndpoint {
    uint64_t generation = (uint64_t{31U} << 32U) | 71U;
    uint64_t ConsoleInputGeneration() const { return generation; }
    std::string input;
    size_t offset = 0U;
    std::string output;
    bool TryReadConsoleInputChar(char& value) {
        if (offset == input.size()) return false;
        value = input[offset++];
        return true;
    }
    void WriteConsoleOutput(const char* data, size_t size) { output.append(data, size); }
};

struct OwnerPairingConsoleTransport {
    OwnerPairingSerialEndpoint usb;
    OwnerPairingBleEndpoint ble;
    firmware_internal::HybridConsoleTransport<OwnerPairingSerialEndpoint,
        OwnerPairingBleEndpoint> hybrid{usb, ble};
};

} // namespace test_support
