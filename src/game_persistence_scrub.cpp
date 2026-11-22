#include "modules/game_persistence_scrub.hpp"

#include "modules/game_persistence.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace modules {
namespace {

GamePersistenceScrubReport g_report;

bool IsValidFenJsonString(const JsonVariantConst v) {
    const JsonString s = v.as<JsonString>();
    if (s.isNull()) {
        return false;
    }

    const size_t n = s.size();
    if (n == 0) {
        return true;
    }
    // Keep this bounded; FEN should be small.
    if (n >= 192) {
        return false;
    }

    const char* p = s.c_str();
    if (p == nullptr) {
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == 0) {
            return false;
        }
        // Disallow control chars and anything outside ASCII.
        if (c < 32 || c > 126) {
            return false;
        }
        // FEN should never contain backslashes. Seeing them usually indicates a
        // double-escaped JSON string, which then shows up as "\\u0000" garbage.
        if (c == '\\') {
            return false;
        }

        // Conservative allowlist for FEN text.
        const bool ok = (c == ' ') || (c == '/') || (c == '-') || (c == '.') || (c == ':') ||
                        (c >= '0' && c <= '9') || (c >= 'a' && c <= 'h') ||
                        (c >= 'A' && c <= 'H') || (c == 'w') || (c == 'b') ||
                        (c == 'p') || (c == 'r') || (c == 'n') || (c == 'b') || (c == 'q') ||
                        (c == 'k') || (c == 'P') || (c == 'R') || (c == 'N') || (c == 'B') ||
                        (c == 'Q') || (c == 'K');
        if (!ok) {
            return false;
        }
    }

    return true;
}

bool ScrubFenSettingKey(JsonDocument& doc, const char* key, bool* outRemoved, fxe::SettingsRemovedKeys* removed) {
    if (outRemoved) {
        *outRemoved = false;
    }

    if (!doc.containsKey(key)) {
        return false;
    }

    const JsonVariantConst v = doc[key];
    if (!IsValidFenJsonString(v)) {
        if (removed != nullptr && !removed->Record(key)) return false;
        (void)doc.remove(key);
        if (outRemoved) {
            *outRemoved = true;
        }
        return true;
    }

    return false;
}

} // namespace

const GamePersistenceScrubReport& GetLastGamePersistenceScrubReport() {
    return g_report;
}

void ResetGamePersistenceScrubReport() {
    g_report = GamePersistenceScrubReport{};
}

bool ScrubGamePersistenceSettings(JsonDocument& doc, fxe::SettingsRemovedKeys* removed) {
    ++g_report.runCount;

    bool scrubbed = false;

    bool removedGameFen = false;
    scrubbed = ScrubFenSettingKey(doc, KEY_GAME_FEN, &removedGameFen, removed) || scrubbed;
    g_report.removedGameFen = g_report.removedGameFen || removedGameFen;

    bool removedUndoBaseFen = false;
    scrubbed = ScrubFenSettingKey(doc, KEY_GAME_UNDO_BASE_FEN, &removedUndoBaseFen, removed) || scrubbed;
    g_report.removedUndoBaseFen = g_report.removedUndoBaseFen || removedUndoBaseFen;

    return scrubbed;
}

} // namespace modules
