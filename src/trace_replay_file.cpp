#include "trace/trace_replay.hpp"

#ifndef ARDUINO

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void SetFailure(trace::ReplayResult& outResult, const uint32_t line, const char* msg) {
    outResult.ok = false;
    outResult.failureLine = line;
    std::snprintf(outResult.message, sizeof(outResult.message), "%s", msg ? msg : "?");
}

} // namespace

namespace trace {

bool ReplayJsonlFile(const char* path,
                     runtime::ManualClock& clock,
                     runtime::GameStateMachine& sm,
                     ReplayResult& outResult,
                     modules::GamePersistenceCoordinator* const gamePersistence,
                     modules::IEngine* const engine) {
    if (!path || std::strlen(path) == 0) {
        SetFailure(outResult, 0, "missing_path");
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        SetFailure(outResult, 0, "file_open_failed");
        return false;
    }

    std::vector<std::string> ownedLines;
    ownedLines.reserve(1024);

    std::string line;
    while (std::getline(in, line)) {
        ownedLines.push_back(line);
    }

    std::vector<const char*> ptrs;
    ptrs.reserve(ownedLines.size());
    for (const auto& s : ownedLines) {
        ptrs.push_back(s.c_str());
    }

    return ReplayJsonLines(ptrs.data(), ptrs.size(), clock, sm, outResult, gamePersistence, engine);
}

} // namespace trace

#endif
