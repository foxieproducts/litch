#include "trace/trace_recorder.hpp"

#include <cstdio>

#include "logger.hpp"

namespace {

constexpr size_t TRACE_LINE_MAX = 384;

uint32_t Fnv1a32UpdateByte(uint32_t h, const uint8_t b) {
    h ^= b;
    return h * 16777619u;
}

uint32_t Fnv1a32UpdateU32(uint32_t h, const uint32_t v) {
    h = Fnv1a32UpdateByte(h, static_cast<uint8_t>((v >> 0) & 0xFFu));
    h = Fnv1a32UpdateByte(h, static_cast<uint8_t>((v >> 8) & 0xFFu));
    h = Fnv1a32UpdateByte(h, static_cast<uint8_t>((v >> 16) & 0xFFu));
    h = Fnv1a32UpdateByte(h, static_cast<uint8_t>((v >> 24) & 0xFFu));
    return h;
}

uint32_t ComputeSettingsHash(const trace::TraceSettingsSnapshot& s) {
    // Deterministic, stable across toolchains: hash only canonical key order + values.
    uint32_t h = 2166136261u;
    h = Fnv1a32UpdateU32(h, s.brightness);
    h = Fnv1a32UpdateU32(h, s.gameFlipSides);
    h = Fnv1a32UpdateU32(h, s.gameFirstPlayer);
    h = Fnv1a32UpdateU32(h, s.vsCpuLevel);
    h = Fnv1a32UpdateU32(h, s.vsCpuPlayer);
    h = Fnv1a32UpdateU32(h, s.timer);
    h = Fnv1a32UpdateU32(h, s.takebackGraceSeconds);
    return h;
}

size_t AppendJsonEscaped(const char* s, char* out, const size_t outCap) {
    if (!out || outCap == 0) {
        return 0;
    }

    if (!s) {
        out[0] = '\0';
        return 0;
    }

    size_t w = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);

        const auto appendChar = [&](const char ch) {
            if (w + 1 >= outCap) {
                return;
            }
            out[w++] = ch;
        };

        const auto appendPair = [&](const char a, const char b) {
            if (w + 2 >= outCap) {
                return;
            }
            out[w++] = a;
            out[w++] = b;
        };

        if (c == '\\') {
            appendPair('\\', '\\');
        } else if (c == '"') {
            appendPair('\\', '"');
        } else if (c == '\n') {
            appendPair('\\', 'n');
        } else if (c == '\r') {
            appendPair('\\', 'r');
        } else if (c == '\t') {
            appendPair('\\', 't');
        } else if (c < 0x20) {
            // Control characters must be escaped; keep it simple and deterministic.
            appendChar('?');
        } else {
            appendChar(static_cast<char>(c));
        }

        if (w + 1 >= outCap) {
            break;
        }
    }

    out[w] = '\0';
    return w;
}

void WriteEscapedStringEventLine(char* line,
                                 const size_t lineCap,
                                 const char* prefixFmt,
                                 const uint32_t nowMs,
                                 const char* value) {
    char escaped[TRACE_LINE_MAX];
    (void)AppendJsonEscaped(value, escaped, sizeof(escaped));

    const int prefixN = std::snprintf(line, lineCap, prefixFmt,
                                      static_cast<unsigned long>(nowMs));
    size_t pos = (prefixN < 0) ? 0U : static_cast<size_t>(prefixN);
    if (pos >= lineCap) {
        pos = lineCap - 1;
    }

    const size_t room = (pos < lineCap) ? (lineCap - pos) : 0U;
    const size_t minSuffix = 3;
    const size_t copyCap = (room > minSuffix) ? (room - minSuffix) : 0U;

    size_t copied = 0;
    while (escaped[copied] != '\0' && copied < copyCap) {
        line[pos + copied] = escaped[copied];
        ++copied;
    }
    pos += copied;

    if (pos + 2 < lineCap) {
        line[pos++] = '"';
        line[pos++] = '}';
        line[pos] = '\0';
    } else if (pos + 1 < lineCap) {
        line[pos++] = '}';
        line[pos] = '\0';
    } else {
        line[lineCap - 1] = '\0';
    }
}

} // namespace

namespace trace {

void TraceRecorder::Start(const char* buildId,
                          const char* boardId,
                          const uint32_t rngSeed,
                          const TraceSettingsSnapshot& settings) {
    m_elapsedSinceStart.Reset();
    m_active = true;
    m_recordCount = 0;
    m_inputEventCount = 0;
    m_frameCheckpointCount = 0;
    m_engineCheckpointCount = 0;

    const uint32_t settingsHash = ComputeSettingsHash(settings);
    char settingsHashHex[9];
    std::snprintf(settingsHashHex, sizeof(settingsHashHex), "%08lx",
                  static_cast<unsigned long>(settingsHash));

    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"header\",\"ver\":1,\"build\":\"%s\","
                  "\"board\":\"%s\",\"t0_ms\":0,\"rng_seed\":%lu,"
                  "\"settings_hash\":\"%s\",\"schema\":{\"square_index\":"
                  "\"0=a8\"}}",
                  buildId ? buildId : "unknown", boardId ? boardId : "unknown",
                  static_cast<unsigned long>(rngSeed), settingsHashHex);
    WriteJsonLine(line);
}

void TraceRecorder::Stop() {
    if (!m_active) {
        return;
    }

    const uint32_t tEndMs = NowTraceMs();

    const uint32_t events = m_inputEventCount;
    const uint32_t frames = m_frameCheckpointCount;
    const uint32_t engine = m_engineCheckpointCount;

    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"end\",\"t_ms\":%lu,\"stats\":{\"events\":%lu,\"frames\":%lu,\"engine\":%lu}}",
                  static_cast<unsigned long>(tEndMs),
                  static_cast<unsigned long>(events),
                  static_cast<unsigned long>(frames),
                  static_cast<unsigned long>(engine));
    WriteJsonLine(line);

    m_active = false;
}

void TraceRecorder::RecordSettings(const TraceSettingsSnapshot& settings) {
    if (!m_active) {
        return;
    }

    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"settings\",\"t_ms\":%lu,\"json\":{"
                  "\"BRIGHTNESS\":%lu,\"BOARD_ROTATE\":%lu,\"GAME_FIRST_PLAYER\":%lu,"
                  "\"VS_CPU_LEVEL\":%lu,\"VS_CPU_PLAYER\":%lu,\"TIMER\":%lu,"
                  "\"TAKEBACK_GRACE_SECONDS\":%lu}}",
                  static_cast<unsigned long>(NowTraceMs()),
                  static_cast<unsigned long>(settings.brightness),
                  static_cast<unsigned long>(settings.gameFlipSides),
                  static_cast<unsigned long>(settings.gameFirstPlayer),
                  static_cast<unsigned long>(settings.vsCpuLevel),
                  static_cast<unsigned long>(settings.vsCpuPlayer),
                  static_cast<unsigned long>(settings.timer),
                  static_cast<unsigned long>(
                      settings.takebackGraceSeconds));
    WriteJsonLine(line);
}

void TraceRecorder::RecordTick() {
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;
    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line), "{\"type\":\"tick\",\"t_ms\":%lu}",
                  static_cast<unsigned long>(NowTraceMs()));
    WriteJsonLine(line);
}

void TraceRecorder::RecordSensorDelta(const uint8_t square,
                                      const bool nowOccupied) {
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;
    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"sensor\",\"t_ms\":%lu,\"square\":%u,"
                  "\"occ\":%u}",
                  static_cast<unsigned long>(NowTraceMs()), square,
                  nowOccupied ? 1U : 0U);
    WriteJsonLine(line);
}

void TraceRecorder::RecordBoardObservation(const uint64_t changedMask,
                                           const uint64_t occupiedMask,
                                           const uint32_t sequence,
                                           const uint8_t deferredDuckDestinationSquare) {
    if (changedMask != 0U) {
        const uint8_t lifted = static_cast<uint8_t>(__builtin_popcountll(changedMask & ~occupiedMask));
        const uint8_t placed = static_cast<uint8_t>(__builtin_popcountll(changedMask & occupiedMask));
        const auto action = m_userActions.Begin(SupportActionKind::PiecesChanged,
            SupportActionOrigin::Board, lifted, placed);
        m_userActions.Finish(action, SupportActionOutcome::Observed);
    }
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;
    char line[TRACE_LINE_MAX] = {};
    char deferredDuck[16] = {};
    if (deferredDuckDestinationSquare < 64U) {
        std::snprintf(deferredDuck, sizeof(deferredDuck),
                      ",\"dd\":%u", deferredDuckDestinationSquare);
    }
    std::snprintf(
        line,
        sizeof(line),
        "{\"type\":\"board_observation\",\"t_ms\":%lu,\"seq\":%lu,\"changed_mask\":\"0x%016llx\",\"occupied_mask\":\"0x%016llx\"%s}",
        static_cast<unsigned long>(NowTraceMs()),
        static_cast<unsigned long>(sequence),
        static_cast<unsigned long long>(changedMask),
        static_cast<unsigned long long>(occupiedMask),
        deferredDuck);
    WriteJsonLine(line);
}

void TraceRecorder::RecordButton(const char* id, const char* evt) {
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;
    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"button\",\"t_ms\":%lu,\"id\":\"%s\","
                  "\"evt\":\"%s\"}",
                  static_cast<unsigned long>(NowTraceMs()), id ? id : "?",
                  evt ? evt : "?");
    WriteJsonLine(line);
}

void TraceRecorder::RecordControl(const uint32_t sequence,
                                  const char* control,
                                  const uint8_t count) {
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;
    char line[TRACE_LINE_MAX] = {};
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"control\",\"t_ms\":%lu,\"seq\":%lu,"
                  "\"control\":\"%s\",\"count\":%u}",
                  static_cast<unsigned long>(NowTraceMs()),
                  static_cast<unsigned long>(sequence),
                  control ? control : "?",
                  static_cast<unsigned>(count));
    WriteJsonLine(line);
}

void TraceRecorder::RecordPositionAcknowledged(const uint32_t sequence,
                                                const uint64_t occupiedMask,
                                                const uint64_t kingMask,
                                                const uint64_t rookMask,
                                                const uint64_t moverMask,
                                                const uint64_t opponentMask,
                                                const uint64_t pawnMask,
                                                const uint8_t enPassantTargetSquare,
                                                const uint8_t enPassantCaptureSquare,
                                                const bool sideToMoveValid,
                                                const bool sideToMoveBlack,
                                                const bool legalCaptureTargetsValid,
                                                const uint64_t legalCaptureTargetsMask,
                                                const bool legalCastlingTargetsValid,
                                                const uint64_t legalCastlingKingTargetsMask,
                                                const runtime::CaptureLegality& captureLegality) {
    if (!m_active) {
        return;
    }
    ++m_inputEventCount;
    // Two masks followed by one fixed-width word per nonempty source row.
    constexpr size_t POSITION_ACK_LINE_MAX = 256U + 20U + (2U + 64U) * 16U;
    char line[POSITION_ACK_LINE_MAX] = {};
    const uint8_t castlingFlags =
        ((legalCastlingKingTargetsMask & (1ULL << 62U)) != 0ULL ? 16U : 0U) |
        ((legalCastlingKingTargetsMask & (1ULL << 58U)) != 0ULL ? 32U : 0U) |
        ((legalCastlingKingTargetsMask & (1ULL << 6U)) != 0ULL ? 64U : 0U) |
        ((legalCastlingKingTargetsMask & (1ULL << 2U)) != 0ULL ? 128U : 0U);
    const uint8_t flags =
        (sideToMoveValid ? 1U : 0U) |
        (sideToMoveBlack ? 2U : 0U) |
        (legalCaptureTargetsValid ? 4U : 0U) |
        (legalCastlingTargetsValid ? 8U : 0U) |
        castlingFlags;
    const int baseSize = std::snprintf(
        line,
        sizeof(line),
        "{\"type\":\"position_ack\",\"t\":%lu,\"s\":%lu,\"o\":\"0x%016llx\",\"k\":\"0x%016llx\",\"r\":\"0x%016llx\",\"m\":\"0x%016llx\",\"n\":\"0x%016llx\",\"p\":\"0x%016llx\",\"e\":%u,\"c\":%u,\"f\":%u,\"l\":\"0x%016llx\"}",
        static_cast<unsigned long>(NowTraceMs()),
        static_cast<unsigned long>(sequence),
        static_cast<unsigned long long>(occupiedMask),
        static_cast<unsigned long long>(kingMask),
        static_cast<unsigned long long>(rookMask),
        static_cast<unsigned long long>(moverMask),
        static_cast<unsigned long long>(opponentMask),
        static_cast<unsigned long long>(pawnMask),
        static_cast<unsigned>(enPassantTargetSquare),
        static_cast<unsigned>(enPassantCaptureSquare),
        static_cast<unsigned>(flags),
        static_cast<unsigned long long>(legalCaptureTargetsMask));
    if (captureLegality.knownSources != 0ULL && baseSize > 0 &&
        static_cast<size_t>(baseSize) < sizeof(line)) {
        uint64_t nonemptySources = 0ULL;
        for (uint8_t source = 0U; source < 64U; ++source) {
            if (captureLegality.landingsBySource[source] != 0ULL) {
                nonemptySources |= 1ULL << source;
            }
        }
        nonemptySources &= captureLegality.knownSources;
        size_t cursor = static_cast<size_t>(baseSize) - 1U;
        cursor += std::snprintf(line + cursor, sizeof(line) - cursor,
            ",\"capture_rows\":\"%016llx%016llx",
            static_cast<unsigned long long>(captureLegality.knownSources),
            static_cast<unsigned long long>(nonemptySources));
        for (uint8_t source = 0U; source < 64U; ++source) {
            if ((nonemptySources & (1ULL << source)) != 0ULL) {
                cursor += std::snprintf(line + cursor, sizeof(line) - cursor, "%016llx",
                    static_cast<unsigned long long>(captureLegality.landingsBySource[source]));
            }
        }
        std::snprintf(line + cursor, sizeof(line) - cursor, "\"}");
    }
    WriteJsonLine(line);
}

void TraceRecorder::RecordActionResult(const uint32_t sequence,
                                       const uint32_t token,
                                       const bool succeeded) {
    if (!m_active) {
        return;
    }
    ++m_inputEventCount;
    char line[TRACE_LINE_MAX] = {};
    std::snprintf(
        line,
        sizeof(line),
        "{\"type\":\"action_result\",\"t_ms\":%lu,\"seq\":%lu,\"token\":%lu,\"ok\":%u}",
        static_cast<unsigned long>(NowTraceMs()),
        static_cast<unsigned long>(sequence),
        static_cast<unsigned long>(token),
        succeeded ? 1U : 0U);
    WriteJsonLine(line);
}

void TraceRecorder::RecordAdapterResult(const uint32_t sequence,
                                        const char* adapter,
                                        const bool succeeded) {
    if (!m_active) {
        return;
    }
    ++m_inputEventCount;
    char line[TRACE_LINE_MAX] = {};
    std::snprintf(
        line,
        sizeof(line),
        "{\"type\":\"adapter_result\",\"t_ms\":%lu,\"seq\":%lu,\"adapter\":\"%s\",\"ok\":%u}",
        static_cast<unsigned long>(NowTraceMs()),
        static_cast<unsigned long>(sequence),
        adapter != nullptr ? adapter : "?",
        succeeded ? 1U : 0U);
    WriteJsonLine(line);
}

void TraceRecorder::RecordCmd(const char* cmd) {
    if (!m_active) {
        return;
    }

    ++m_inputEventCount;

    char line[TRACE_LINE_MAX];
    WriteEscapedStringEventLine(
        line, sizeof(line),
        "{\"type\":\"cmd\",\"t_ms\":%lu,\"cmd\":\"",
        NowTraceMs(), cmd);

    WriteJsonLine(line);
}

void TraceRecorder::RecordCheckpointState(const char* const stateLabel,
                                          const bool placementModeActive,
                                          const bool cpuThinking,
                                          const bool humanIllegal,
                                          const bool cpuIllegal) {
    if (!m_active) {
        return;
    }

    char line[TRACE_LINE_MAX];
    std::snprintf(
        line, sizeof(line),
        "{\"type\":\"checkpoint_state\",\"t_ms\":%lu,\"game_state\":\"%s\",\"placement\":%u,\"cpu\":%u,\"human_illegal\":%u,\"cpu_illegal\":%u}",
        static_cast<unsigned long>(NowTraceMs()),
        stateLabel != nullptr ? stateLabel : "?",
        placementModeActive ? 1U : 0U,
        cpuThinking ? 1U : 0U,
        humanIllegal ? 1U : 0U,
        cpuIllegal ? 1U : 0U);
    WriteJsonLine(line);
}

void TraceRecorder::RecordCheckpointEngine(const char* fen) {
    if (!m_active) {
        return;
    }

    ++m_engineCheckpointCount;

    char line[TRACE_LINE_MAX];
    WriteEscapedStringEventLine(
        line, sizeof(line),
        "{\"type\":\"checkpoint_engine\",\"t_ms\":%lu,\"fen\":\"",
        NowTraceMs(), fen);

    WriteJsonLine(line);
}

void TraceRecorder::RecordCheckpointFrame(const uint64_t frameHash, const char* kind) {
    if (!m_active) {
        return;
    }

    ++m_frameCheckpointCount;

    const unsigned long long v = static_cast<unsigned long long>(frameHash);
    const char* k = (kind && kind[0] != '\0') ? kind : "all";

    char line[TRACE_LINE_MAX];
    std::snprintf(line, sizeof(line),
                  "{\"type\":\"checkpoint_frame\",\"t_ms\":%lu,\"frame_hash\":\"0x%016llx\",\"kind\":\"%s\"}",
                  static_cast<unsigned long>(NowTraceMs()), v, k);
    WriteJsonLine(line);
}

uint32_t TraceRecorder::NowTraceMs() const {
    return m_elapsedSinceStart.ElapsedMs();
}

void TraceRecorder::WriteJsonLine(const char* jsonLine) {
    ++m_recordCount;
    logger::RawLine(jsonLine);
}

} // namespace trace
