#include "modules/game_persistence.hpp"

#include <cstring>

#include "logger.hpp"

namespace modules {

namespace game_clock_persistence {

static constexpr uint32_t FLAG_ENABLED = 1U << 0U;
static constexpr uint32_t FLAG_STARTED = 1U << 1U;
static constexpr uint32_t FLAG_RUNNING = 1U << 2U;
static constexpr uint32_t FLAG_PAUSED = 1U << 3U;
static constexpr uint32_t FLAG_TIMED_OUT = 1U << 4U;
static constexpr uint32_t FLAG_MASK = FLAG_ENABLED | FLAG_STARTED |
                                      FLAG_RUNNING | FLAG_PAUSED |
                                      FLAG_TIMED_OUT;
static constexpr uint32_t NO_TIMEOUT_SIDE = 2U;

inline bool AddU32(JsonArray& record, const uint32_t value) {
    return record.add(value);
}

inline bool ReadU32(const JsonArrayConst& record,
                    const size_t index,
                    uint32_t& out) {
    if (index >= record.size()) {
        return false;
    }
    const JsonVariantConst value = record[index];
    if (value.is<bool>() || !value.is<uint32_t>()) {
        return false;
    }
    out = value.as<uint32_t>();
    return true;
}

inline uint32_t SideToU32(const runtime::ClockSide side) {
    return side == runtime::ClockSide::Black ? 1U : 0U;
}

inline bool U32ToSide(const uint32_t value, runtime::ClockSide& out) {
    if (value > 1U) {
        return false;
    }
    out = value == 1U ? runtime::ClockSide::Black
                      : runtime::ClockSide::White;
    return true;
}

inline bool Validate(const GameClockPersistenceState& state) {
    if (state.sessionId == 0U || state.durableEpoch == 0U ||
        state.timerSetting > 6U) {
        return false;
    }

    const runtime::TimerPreset preset =
        runtime::TimerPresetForTimerSetting(state.timerSetting);
    const uint32_t expectedBaseMs = preset.baseSeconds * 1000U;
    const uint32_t expectedIncrementMs = preset.incrementSeconds * 1000U;
    const runtime::GameClockState& clock = state.clock;
    if (!state.terminalDuckChessMove.Empty() && !clock.timedOut) {
        return false;
    }
    if (state.baseMs != expectedBaseMs ||
        clock.incrementMs != expectedIncrementMs ||
        clock.enabled != (state.timerSetting != 0U)) {
        return false;
    }
    if (clock.activeSide != runtime::ClockSide::White &&
        clock.activeSide != runtime::ClockSide::Black) {
        return false;
    }
    if (clock.timedOutSide != runtime::ClockSide::White &&
        clock.timedOutSide != runtime::ClockSide::Black) {
        return false;
    }

    if (!clock.enabled) {
        return !state.started && !clock.running && !clock.paused &&
               !clock.timedOut && clock.remainingMsWhite == 0U &&
               clock.remainingMsBlack == 0U;
    }
    if (clock.running && !state.started) {
        return false;
    }
    if (clock.paused && !clock.running) {
        return false;
    }
    if (!state.started &&
        (clock.running || clock.paused || clock.timedOut ||
         clock.remainingMsWhite != expectedBaseMs ||
         clock.remainingMsBlack != expectedBaseMs)) {
        return false;
    }

    if (clock.timedOut) {
        const uint32_t losingRemaining =
            clock.timedOutSide == runtime::ClockSide::Black
                ? clock.remainingMsBlack
                : clock.remainingMsWhite;
        const uint32_t winningRemaining =
            clock.timedOutSide == runtime::ClockSide::Black
                ? clock.remainingMsWhite
                : clock.remainingMsBlack;
        return state.started && !clock.running && !clock.paused &&
               clock.activeSide == clock.timedOutSide &&
               losingRemaining == 0U && winningRemaining != 0U;
    }

    return clock.remainingMsWhite != 0U &&
           clock.remainingMsBlack != 0U;
}

bool WriteTerminalPresentation(JsonDocument& doc,
                                const GameClockPersistenceState& state) {
    if (state.terminalDuckChessMove.Empty()) {
        (void)doc.remove(KEY_GAME_CLOCK_TERMINAL_V1);
        return true;
    }
    const char* canonicalFen = doc[KEY_GAME_FEN] | "";
    if (canonicalFen[0] == '\0') {
        return false;
    }
    JsonArray record = doc[KEY_GAME_CLOCK_TERMINAL_V1].to<JsonArray>();
    return !record.isNull() && record.add(1U) &&
           record.add(state.sessionId) && record.add(state.durableEpoch) &&
           record.add(const_cast<char*>(canonicalFen)) &&
           record.add(const_cast<char*>(state.terminalDuckChessMove.CStr())) &&
           !doc.overflowed();
}

void ReadTerminalPresentation(const JsonDocument& doc,
                               GameClockPersistenceState& state) {
    if (!state.clock.timedOut || !doc.containsKey(KEY_GAME_CLOCK_TERMINAL_V1)) {
        return;
    }
    const JsonArrayConst record =
        doc[KEY_GAME_CLOCK_TERMINAL_V1].as<JsonArrayConst>();
    uint32_t version = 0U;
    uint32_t session = 0U;
    uint32_t epoch = 0U;
    if (record.isNull() || record.size() != 5U ||
        !ReadU32(record, 0U, version) || version != 1U ||
        !ReadU32(record, 1U, session) || !ReadU32(record, 2U, epoch) ||
        !record[3U].is<const char*>() || !record[4U].is<const char*>()) {
        logger::Warn("clock: ignored malformed terminal presentation");
        return;
    }
    // An older firmware can preserve this unknown key while rewriting the
    // ordinary v1 clock or starting a new game. Never reuse that old display.
    const char* canonicalFen = doc[KEY_GAME_FEN] | "";
    if (session != state.sessionId || epoch != state.durableEpoch ||
        std::strcmp(record[3U].as<const char*>(), canonicalFen) != 0) {
        return;
    }
    if (!state.terminalDuckChessMove.Assign(record[4U].as<const char*>())) {
        logger::Warn("clock: ignored oversized terminal presentation move");
        state.terminalDuckChessMove.Clear();
    }
}

} // namespace game_clock_persistence

bool WriteGameClockPersistenceState(
    JsonDocument& doc,
    const GameClockPersistenceState& state) {
    if (!game_clock_persistence::Validate(state)) {
        return false;
    }

    const runtime::GameClockState& clock = state.clock;
    uint32_t flags = 0U;
    flags |= clock.enabled ? game_clock_persistence::FLAG_ENABLED : 0U;
    flags |= state.started ? game_clock_persistence::FLAG_STARTED : 0U;
    flags |= clock.running ? game_clock_persistence::FLAG_RUNNING : 0U;
    flags |= clock.paused ? game_clock_persistence::FLAG_PAUSED : 0U;
    flags |= clock.timedOut ? game_clock_persistence::FLAG_TIMED_OUT : 0U;

    JsonArray record = doc[KEY_GAME_CLOCK_V1].to<JsonArray>();
    if (record.isNull()) {
        return false;
    }
    record.clear();
    const uint32_t timeoutSide = clock.timedOut
        ? game_clock_persistence::SideToU32(clock.timedOutSide)
        : game_clock_persistence::NO_TIMEOUT_SIDE;
    return game_clock_persistence::AddU32(record, GAME_CLOCK_RECORD_VERSION) &&
           game_clock_persistence::AddU32(record, state.sessionId) &&
           game_clock_persistence::AddU32(record, state.durableEpoch) &&
           game_clock_persistence::AddU32(record, state.timerSetting) &&
           game_clock_persistence::AddU32(record, state.baseMs) &&
           game_clock_persistence::AddU32(record, clock.incrementMs) &&
           game_clock_persistence::AddU32(record, flags) &&
           game_clock_persistence::AddU32(
               record,
               game_clock_persistence::SideToU32(clock.activeSide)) &&
           game_clock_persistence::AddU32(record, clock.remainingMsWhite) &&
           game_clock_persistence::AddU32(record, clock.remainingMsBlack) &&
           game_clock_persistence::AddU32(record, timeoutSide) &&
           game_clock_persistence::WriteTerminalPresentation(doc, state) &&
           !doc.overflowed();
}

bool ReadGameClockPersistenceState(
    const JsonDocument& doc,
    GameClockPersistenceState& out) {
    out = GameClockPersistenceState{};
    if (!doc.containsKey(KEY_GAME_CLOCK_V1)) {
        return false;
    }
    const JsonVariantConst value = doc[KEY_GAME_CLOCK_V1];
    if (!value.is<JsonArrayConst>()) {
        return false;
    }
    const JsonArrayConst record = value.as<JsonArrayConst>();
    if (record.size() != GAME_CLOCK_RECORD_FIELD_COUNT) {
        return false;
    }

    uint32_t version = 0U;
    uint32_t flags = 0U;
    uint32_t activeSide = 0U;
    uint32_t timeoutSide = 0U;
    if (!game_clock_persistence::ReadU32(record, 0U, version) ||
        !game_clock_persistence::ReadU32(record, 1U, out.sessionId) ||
        !game_clock_persistence::ReadU32(record, 2U, out.durableEpoch) ||
        !game_clock_persistence::ReadU32(record, 3U, out.timerSetting) ||
        !game_clock_persistence::ReadU32(record, 4U, out.baseMs) ||
        !game_clock_persistence::ReadU32(record, 5U, out.clock.incrementMs) ||
        !game_clock_persistence::ReadU32(record, 6U, flags) ||
        !game_clock_persistence::ReadU32(record, 7U, activeSide) ||
        !game_clock_persistence::ReadU32(record, 8U, out.clock.remainingMsWhite) ||
        !game_clock_persistence::ReadU32(record, 9U, out.clock.remainingMsBlack) ||
        !game_clock_persistence::ReadU32(record, 10U, timeoutSide) ||
        version != GAME_CLOCK_RECORD_VERSION ||
        (flags & ~game_clock_persistence::FLAG_MASK) != 0U ||
        !game_clock_persistence::U32ToSide(
            activeSide, out.clock.activeSide)) {
        return false;
    }

    out.clock.enabled =
        (flags & game_clock_persistence::FLAG_ENABLED) != 0U;
    out.started =
        (flags & game_clock_persistence::FLAG_STARTED) != 0U;
    out.clock.running =
        (flags & game_clock_persistence::FLAG_RUNNING) != 0U;
    out.clock.paused =
        (flags & game_clock_persistence::FLAG_PAUSED) != 0U;
    out.clock.timedOut =
        (flags & game_clock_persistence::FLAG_TIMED_OUT) != 0U;

    if (out.clock.timedOut) {
        if (!game_clock_persistence::U32ToSide(
                timeoutSide, out.clock.timedOutSide)) {
            return false;
        }
    } else {
        if (timeoutSide != game_clock_persistence::NO_TIMEOUT_SIDE) {
            return false;
        }
        out.clock.timedOutSide = runtime::ClockSide::White;
    }
    if (!game_clock_persistence::Validate(out)) {
        return false;
    }
    game_clock_persistence::ReadTerminalPresentation(doc, out);
    return true;
}

} // namespace modules
