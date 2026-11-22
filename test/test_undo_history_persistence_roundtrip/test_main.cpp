#include <array>
#include <cstdio>
#include <cstring>

#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/assistance_preferences.hpp"
#include "modules/game_persistence.hpp"
#include "modules/hardware_test_record.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/settings_registry.hpp"
#include "modules/settings_store.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace {

void FillPattern(uint8_t* out, const uint16_t count) {
    for (uint16_t i = 0; i < count; ++i) {
        // Deterministic byte pattern; keep values in 0..255.
        out[i] = static_cast<uint8_t>((i * 37U + 11U) & 0xFFU);
    }
}

bool IsGameStringKey(const char* key) {
    return std::strcmp(key, modules::KEY_GAME_FEN) == 0 ||
           std::strcmp(key, modules::KEY_GAME_MOVED_SQUARES) == 0 ||
           std::strcmp(key, modules::KEY_GAME_UNDO_BASE_FEN) == 0 ||
           std::strcmp(key, modules::KEY_GAME_UNDO_BASE_MOVED_SQUARES) == 0 ||
           std::strcmp(key, modules::KEY_GAME_UNDO_HISTORY) == 0;
}

void FillAllPersistedSettings(JsonDocument& doc) {
    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors =
        modules::settings_registry::EnumerateAll(descriptorCount);
    for (size_t i = 0; i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (!descriptor.IsPersisted() || IsGameStringKey(descriptor.key)) {
            continue;
        }

        switch (descriptor.type) {
            case fxe::SettingType::UInt32:
                doc[descriptor.key] = descriptor.max;
                break;
            case fxe::SettingType::Bool:
                // The longer JSON spelling bounds both boolean values.
                doc[descriptor.key] = false;
                break;
            case fxe::SettingType::Enum: {
                const char* longest = "";
                for (size_t value = 0; value < descriptor.allowedCount; ++value) {
                    if (std::strlen(descriptor.allowedValues[value]) > std::strlen(longest)) {
                        longest = descriptor.allowedValues[value];
                    }
                }
                doc[descriptor.key].set(const_cast<char*>(longest));
                break;
            }
            case fxe::SettingType::String: {
                // BleService consumes this through FixedString<32>.
                char longestDeviceName[32];
                std::memset(longestDeviceName, 'n', sizeof(longestDeviceName) - 1U);
                longestDeviceName[sizeof(longestDeviceName) - 1U] = '\0';
                doc[descriptor.key].set(longestDeviceName);
                break;
            }
        }
    }
}

void FillMaximumGameState(modules::GamePersistenceState& state) {
    char longestFen[litch_max::FEN_BUFFER_CAPACITY];
    std::memset(longestFen, 'f', sizeof(longestFen) - 1U);
    longestFen[sizeof(longestFen) - 1U] = '\0';
    (void)state.fen.Assign(longestFen);
    (void)state.undoBaseFen.Assign(longestFen);

    for (uint8_t square = 0; square < runtime::BOARD_SQUARE_COUNT; ++square) {
        state.movedSquares[square] = square;
        state.undoBaseMovedSquares[square] = square;
    }
    state.movedSquaresCount = runtime::BOARD_SQUARE_COUNT;
    state.undoBaseMovedSquaresCount = runtime::BOARD_SQUARE_COUNT;

    FillPattern(state.undoHistory, sizeof(state.undoHistory));
    state.undoHistoryCount = sizeof(state.undoHistory);
    state.cpuMoveValid = true;
    state.cpuMoveFrom = 63U;
    state.cpuMoveTo = 62U;
    state.cpuMoveCaptureSquareValid = true;
    state.cpuMoveCaptureSquare = 61U;
    state.cpuMovePromotionSuffixLower = 'q';

    char longestInteraction[128];
    std::memset(longestInteraction, 'i', sizeof(longestInteraction) - 1U);
    longestInteraction[sizeof(longestInteraction) - 1U] = '\0';
    (void)state.interactionState.Assign(longestInteraction);

    state.clockStatePresent = true;
    state.clockState.sessionId = UINT32_MAX;
    state.clockState.durableEpoch = UINT32_MAX;
    state.clockState.timerSetting = 6U;
    state.clockState.baseMs = 1800000U;
    state.clockState.started = true;
    state.clockState.clock = runtime::GameClockState{
        .enabled = true,
        .running = true,
        .paused = true,
        .timedOut = false,
        .activeSide = runtime::ClockSide::Black,
        .timedOutSide = runtime::ClockSide::White,
        .remainingMsWhite = UINT32_MAX,
        .remainingMsBlack = UINT32_MAX,
        .incrementMs = 0U,
    };
}

enum class SnapshotSchema { Current, LegacyWakeKeys };

void CheckMaximumRoundTrip(fxe::test_support::TestResults& results,
                           const SnapshotSchema schema) {
    const bool legacyWakeKeys = schema == SnapshotSchema::LegacyWakeKeys;
    const char* const schemaName = legacyWakeKeys ? "legacy_wake" : "current";

    // Write a settings file containing a long undo history, then reload it and
    // ensure the undo history is preserved.
    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_undo_history_persistence_roundtrip.json";
    cfg.migrateFunc = [](JsonDocument& doc) {
        return modules::settings_registry::CanonicalizeLoadedSettings(doc);
    };
    (void)std::remove(cfg.path);
    (void)std::remove("build/test_undo_history_persistence_roundtrip.json.bak");
    (void)std::remove("build/test_undo_history_persistence_roundtrip.json.tmp");

    {
        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load (writer)");

        FillAllPersistedSettings(settings.Doc());
        if (legacyWakeKeys) {
            settings.Doc()["P1_X_DEEP_WAKE_ENABLED"] = false;
            settings.Doc()["P1_X_WAKE_HOLD_MS"] = 5000U;
        }
        results.Check(settings.Doc().containsKey("P1_X_DEEP_WAKE_ENABLED") == legacyWakeKeys &&
                          settings.Doc().containsKey("P1_X_WAKE_HOLD_MS") == legacyWakeKeys,
                      "maximum fixture contains retired wake keys only for migration coverage");
        settings.Doc()["PLAYER_PALETTE_V1"] = 1U;
        modules::BoardIdentity boardIdentity;
        boardIdentity.bytes.fill(0xffU);
        results.Check(modules::WriteBoardIdentity(settings.Doc(), boardIdentity),
                      "maximum snapshot includes production board identity");
        modules::HardwareTestRecord hardwareRecord;
        hardwareRecord.revision = hardware::BoardRevision::V3_3S3;
        hardwareRecord.radioResult = modules::ManufacturingTestRadioResult::Verified;
        hardwareRecord.factoryVerifierEnabled = true;
        hardwareRecord.radioChannelMask = modules::FACTORY_RADIO_BLE_CHANNEL;
        hardwareRecord.automatedDurationMs = UINT32_MAX;
        hardwareRecord.operatorDurationMs = UINT32_MAX;
        hardwareRecord.radioDurationMs = UINT32_MAX;
        results.Check(modules::WriteHardwareTestRecord(settings.Doc(), hardwareRecord),
                      "maximum snapshot includes production manufacturing record");

        modules::GamePersistenceState s;
        FillMaximumGameState(s);
        s.gameHistory.present = true;
        s.gameHistory.gameId.bytes.fill(0xffU);
        s.gameHistory.lineRevision = modules::game_history::MAX_LINE_REVISION;
        s.gameHistory.currentPly = 4096U;
        results.Check(s.fen.Size() == litch_max::FEN_BUFFER_CAPACITY - 1U,
                      "maximum FEN storage populated");
        results.Check(s.undoBaseFen.Size() == litch_max::FEN_BUFFER_CAPACITY - 1U,
                      "maximum undo-base FEN storage populated");
        results.Check(s.interactionState.Size() == 127U,
                      "maximum interaction-state storage populated");

        // Exercise the maximum supported undo history through the persisted format.
        static constexpr uint16_t MOVES_TO_PERSIST = modules::UNDO_HISTORY_MOVE_CAPACITY;
        static constexpr uint16_t FIELD_COUNT =
            static_cast<uint16_t>(modules::UNDO_HISTORY_FIELDS_PER_MOVE * MOVES_TO_PERSIST);
        static_assert(FIELD_COUNT <= sizeof(s.undoHistory), "field count within capacity");
        FillPattern(s.undoHistory, FIELD_COUNT);
        s.undoHistoryCount = FIELD_COUNT;

        results.Check(modules::WriteGamePersistenceState(settings.Doc(), s), "WriteGamePersistenceState");
        modules::LitchcraftRecord art;
        art.active = true;
        for (size_t i = 0U; i < art.canvas.size(); ++i) {
            art.canvas[i] = {uint8_t(i), uint8_t(i * 37U), uint8_t(255U - i)};
        }
        modules::LitchcraftEncodedCanvas artStorage;
        modules::LitchcraftEncodedQuickPalette artPaletteStorage;
        results.Check(modules::WriteLitchcraftRecord(
                          settings.Doc(), art, artStorage, artPaletteStorage),
                      "maximum snapshot includes real versioned RGB24 art codec");
        std::printf("[SETTINGS_CAPACITY] schema=%s writer_memory=%zu writer_json=%zu capacity=%zu snapshot_capacity=%zu\n",
                    schemaName,
                    settings.Doc().memoryUsage(),
                    measureJson(settings.Doc()),
                    settings.Doc().capacity(),
                    fxe::SETTINGS_STORE_SNAPSHOT_BUFFER_CAPACITY);
        std::array<char, fxe::SETTINGS_STORE_SNAPSHOT_BUFFER_CAPACITY> snapshot{};
        const size_t snapshotBytes = measureJson(settings.Doc());
        results.Check(
            snapshotBytes < snapshot.size(),
            "maximum persisted firmware state fits worker snapshot buffer");
        const size_t written = serializeJson(
            settings.Doc(), snapshot.data(), snapshot.size());
        results.Check(written == snapshotBytes && written < snapshot.size() &&
                          snapshot[written] == '\0',
                      "maximum worker snapshot is complete and terminated");
        results.Check(settings.SaveNow(), "SaveNow");
    }

    {
        modules::SettingsStore settings(cfg);
        results.Check(settings.Load(), "settings load (reader)");
        results.Check(!settings.Doc().containsKey("P1_X_DEEP_WAKE_ENABLED") &&
                          !settings.Doc().containsKey("P1_X_WAKE_HOLD_MS"),
                      "ordinary load retires only obsolete wake controls in the maximum saved snapshot");
        results.Check(settings.Doc()["PLAYER_PALETTE_V1"].as<uint32_t>() == 1U,
                      "maximum settings snapshot retains private palette provenance");

        for (const char* key : modules::assistance_preferences::KEYS) {
            results.Check(settings.Doc()[key].is<bool>() &&
                              !settings.Doc()[key].as<bool>(),
                          "maximum snapshot retains each assistance preference");
        }

        modules::HardwareTestRecord hardwareRecord;
        results.Check(modules::DecodeHardwareTestRecord(settings.ReadValue(
                          modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), hardwareRecord) &&
                          hardwareRecord.automatedDurationMs == UINT32_MAX &&
                          hardwareRecord.operatorDurationMs == UINT32_MAX &&
                          hardwareRecord.radioDurationMs == UINT32_MAX,
                      "maximum manufacturing durations roundtrip");

        modules::GamePersistenceState out;
        results.Check(modules::ReadGamePersistenceState(settings.Doc(), out), "ReadGamePersistenceState");
        modules::LitchcraftRecord art;
        results.Check(modules::ReadLitchcraftRecord(settings.Doc(), art) == modules::LitchcraftRecordStatus::Valid &&
                          art.active && art.canvas[255].r == 255U && art.canvas[255].b == 0U,
                      "maximum snapshot reload preserves versioned artwork");
        std::printf("[SETTINGS_CAPACITY] schema=%s loaded_memory=%zu loaded_json=%zu capacity=%zu\n",
                    schemaName,
                    settings.Doc().memoryUsage(),
                    measureJson(settings.Doc()),
                    settings.Doc().capacity());
        results.Check(!settings.Doc().overflowed() &&
                          settings.Doc().memoryUsage() < settings.Doc().capacity(),
                      "maximum snapshot including legacy migration fits the settings document");
        if (!legacyWakeKeys) {
            // The reserve belongs to the current persisted schema. Retired keys
            // are not persisted; their separate migration case keeps the same
            // content/staging checks, but ArduinoJson removal cannot reclaim its
            // transient arena allocation until the document is reloaded.
            static constexpr size_t REQUIRED_SETTINGS_HEADROOM = 2048U;
            results.Check(
                settings.Doc().memoryUsage() + REQUIRED_SETTINGS_HEADROOM <=
                    settings.Doc().capacity(),
                "maximum persisted firmware state reserves two KiB settings headroom");
        }

        static constexpr uint16_t MOVES_TO_PERSIST = modules::UNDO_HISTORY_MOVE_CAPACITY;
        static constexpr uint16_t FIELD_COUNT =
            static_cast<uint16_t>(modules::UNDO_HISTORY_FIELDS_PER_MOVE * MOVES_TO_PERSIST);
        results.Check(out.undoHistoryCount == FIELD_COUNT, "undo history count roundtrips");
        results.Check(out.clockStatePresent &&
                          out.clockState.sessionId == UINT32_MAX &&
                          out.clockState.durableEpoch == UINT32_MAX &&
                          out.clockState.clock.remainingMsWhite == UINT32_MAX &&
                          out.clockState.clock.remainingMsBlack == UINT32_MAX,
                      "maximum fixed clock record roundtrips");

        uint8_t expected[FIELD_COUNT] = {};
        FillPattern(expected, FIELD_COUNT);
        results.Check(std::memcmp(out.undoHistory, expected, FIELD_COUNT) == 0, "undo history bytes roundtrip");
    }
}

} // namespace

int main() {
    fxe::test_support::TestResults results("UNDO_HISTORY_PERSISTENCE_ROUNDTRIP");
    results.Start();
    CheckMaximumRoundTrip(results, SnapshotSchema::Current);
    CheckMaximumRoundTrip(results, SnapshotSchema::LegacyWakeKeys);
    return results.Finish();
}
