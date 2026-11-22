#pragma once
#include <cstdint>
struct esp_ota_select_entry_t {
    uint32_t ota_seq;
    uint8_t seq_label[20];
    uint32_t ota_state;
    uint32_t crc;
};
extern "C" {
uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t* entry);
bool bootloader_common_ota_select_valid(const esp_ota_select_entry_t* entry);
int bootloader_common_get_active_otadata(esp_ota_select_entry_t* entries);
}
