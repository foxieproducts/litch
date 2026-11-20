#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "modules/owner_pairing_session_wire.hpp"

int main() {
    using namespace modules::owner_pairing_session_wire;
    std::ifstream fixture("test/fixtures/owner_pairing_session.csv");
    if (!fixture) return 1;
    std::string line;
    std::getline(fixture, line);
    size_t count = 0U;
    while (std::getline(fixture, line)) {
        std::istringstream row(line);
        std::string name, kind, valid, hex;
        std::getline(row, name, ',');
        std::getline(row, kind, ',');
        std::getline(row, valid, ',');
        std::getline(row, hex, ',');
        std::array<uint8_t, 32> bytes{};
        const size_t size = hex.size() / 2U;
        for (size_t i = 0U; i < size; ++i) {
            bytes[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2U, 2U), nullptr, 16));
        }
        Command command;
        Status status;
        const bool decoded = kind == "command"
            ? DecodeCommand(bytes.data(), size, command)
            : DecodeStatus(bytes.data(), size, status);
        if (decoded != (valid == "1")) {
            std::printf("[FAIL] %s\n", name.c_str());
            return 1;
        }
        ++count;
    }
    std::printf("[OWNER_PAIRING_SESSION_WIRE] %zu shared fixtures passed\n", count);
    return count == 45U ? 0 : 1;
}
