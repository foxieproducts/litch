inline bool FenToOccupancyMask(const char* fen, uint64_t& outOcc64) {
    outOcc64 = 0;
    if (fen == nullptr) {
        return false;
    }

    uint8_t rank = 0;
    uint8_t file = 0;

    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            break;
        }

        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            const uint8_t n = static_cast<uint8_t>(c - '0');
            file = static_cast<uint8_t>(file + n);
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!isPiece) {
            return false;
        }

        if (rank >= 8 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>((rank * 8) + file);
        outOcc64 |= (1ULL << sq);
        ++file;
    }

    return (rank == 7) && (file == 8);
}

inline bool GetEngineOcc(modules::IEngine& engine, uint64_t& outOcc64) {
    char buf[128] = {};
    if (!engine.GetFen(buf, sizeof(buf))) {
        return false;
    }
    return FenToOccupancyMask(buf, outOcc64);
}

inline bool GetEngineFen(modules::IEngine& engine,
                         char* outBuf,
                         const size_t outCapacity) {
    if (outBuf == nullptr || outCapacity == 0) {
        return false;
    }
    outBuf[0] = '\0';
    return engine.GetFen(outBuf, outCapacity);
}

inline bool FenSideToMoveIsBlack(const char* fen) {
    if (fen == nullptr) {
        return false;
    }

    const char* p = fen;
    while (*p != '\0' && *p != ' ') {
        ++p;
    }
    if (*p != ' ') {
        return false;
    }
    ++p;
    return *p == 'b';
}
