#pragma once

#include <ArduinoJson.h>
#include <cstring>

namespace modules {

// BLE control envelopes are flat objects. Reject NUL-bearing keys/strings
// before C-string command matching can mistake a prefix for the whole token.
inline bool HasSafeBleJsonText(const JsonVariantConst root) {
    if (!root.is<JsonObjectConst>()) {
        return false;
    }
    for (const JsonPairConst field : root.as<JsonObjectConst>()) {
        const JsonString key = field.key();
        const JsonString value = field.value().as<JsonString>();
        if (std::strlen(key.c_str()) != key.size() ||
            (!value.isNull() && std::strlen(value.c_str()) != value.size())) {
            return false;
        }
    }
    return true;
}

} // namespace modules
