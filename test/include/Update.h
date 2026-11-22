#pragma once

#include <cstddef>
#include <cstdint>

// Match the pinned Arduino Update call signatures used by the S3 adapter.
inline constexpr size_t UPDATE_SIZE_UNKNOWN = 0xFFFFFFFFU;
inline constexpr int U_FLASH = 0;
class UpdateClass {
  public:
    bool begin(size_t size = UPDATE_SIZE_UNKNOWN, int command = U_FLASH,
               int ledPin = -1, uint8_t ledOn = 0U, const char* label = nullptr);
    size_t write(uint8_t* data, size_t len);
    bool end(bool evenIfRemaining = false);
    void abort();
    bool hasError();
};
extern UpdateClass Update;
