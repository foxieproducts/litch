#pragma once
#include "LittleFS.h"
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0;
inline esp_err_t esp_littlefs_format(const char* label) {
    reformat_sdk::Access();
    ++reformat_sdk::formatCalls;
    reformat_sdk::label = label;
    if (reformat_sdk::fault == reformat_sdk::Fault::Format) return -1;
    if (std::string(label) != "spiffs") return -2;
    for (const auto& entry : std::filesystem::directory_iterator(reformat_sdk::root)) {
        std::filesystem::remove_all(entry.path());
    }
    if (reformat_sdk::fault == reformat_sdk::Fault::Write) {
        std::filesystem::create_directory(reformat_sdk::root / "settings.json");
        std::filesystem::create_directory(reformat_sdk::root / "settings.json.tmp");
        std::filesystem::create_directory(reformat_sdk::root / "settings.json.retry");
    }
    return ESP_OK;
}
