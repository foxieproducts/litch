#pragma once
/*
 * Copyright 2020-2025 Ryan Powell <ryan@nable-embedded.io> and
 * esp-nimble-cpp, NimBLE-Arduino contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Native SDK boundary: exact NimBLE-Arduino 2.3.7 UUID/payload method
// bodies, made inline. RF submission is controlled by NimBLEDevice.h; packet
// capacity, field types, UUID packing and name admission remain the SDK code.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define NIMBLE_LOGE(...)
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128
#define BLE_HS_ADV_MAX_SZ 31
#define BLE_HS_ADV_MAX_FIELD_SZ 29
#define BLE_HS_ADV_TYPE_FLAGS 1
#define BLE_HS_ADV_TYPE_COMP_UUIDS16 3
#define BLE_HS_ADV_TYPE_COMP_UUIDS32 5
#define BLE_HS_ADV_TYPE_COMP_UUIDS128 7
#define BLE_HS_ADV_TYPE_INCOMP_NAME 8
#define BLE_HS_ADV_TYPE_COMP_NAME 9
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
struct ble_uuid_t {uint8_t type;};
struct ble_uuid16_t {ble_uuid_t u; uint16_t value;};
struct ble_uuid32_t {ble_uuid_t u; uint32_t value;};
struct ble_uuid128_t {ble_uuid_t u; uint8_t value[16];};
union ble_uuid_any_t {ble_uuid_t u;ble_uuid16_t u16;ble_uuid32_t u32;ble_uuid128_t u128;};
class NimBLEUUID {public: NimBLEUUID(const std::string&);uint8_t bitSize()const;const uint8_t* getValue()const;private:ble_uuid_any_t m_uuid{};};
class NimBLEAdvertisementData {public:bool addData(const uint8_t*,size_t);bool setFlags(uint8_t);bool addServiceUUID(const NimBLEUUID&);bool setName(const std::string&,bool=true);int getDataLocation(uint8_t)const;bool removeData(uint8_t);std::vector<uint8_t>getPayload()const;void clearData(){m_payload.clear();}private:std::vector<uint8_t>m_payload;};
inline NimBLEUUID::NimBLEUUID(const std::string& value) {
    if (value.length() == 4) {
        m_uuid.u.type    = BLE_UUID_TYPE_16;
        m_uuid.u16.value = strtoul(value.c_str(), nullptr, 16);
    } else if (value.length() == 8) {
        m_uuid.u.type    = BLE_UUID_TYPE_32;
        m_uuid.u32.value = strtoul(value.c_str(), nullptr, 16);
    } else if (value.length() == 16) {
        memcpy(m_uuid.u128.value, &value[0], 16);
        m_uuid.u.type = BLE_UUID_TYPE_128;
    } else if (value.length() == 36) {
        char*    position;
        uint64_t first_half = (strtoull(&value[0], &position, 16) << 32) +
                              (strtoull(position + 1, &position, 16) << 16) + strtoull(position + 1, &position, 16);
        uint64_t second_half = (strtoull(position + 1, &position, 16) << 48) + strtoull(position + 1, nullptr, 16);
        memcpy(m_uuid.u128.value + 8, &first_half, 8);
        memcpy(m_uuid.u128.value, &second_half, 8);
        m_uuid.u.type = BLE_UUID_TYPE_128;
    } else {
        NIMBLE_LOGE(LOG_TAG, "Invalid UUID length");
        m_uuid.u.type = 0;
    }
}
inline uint8_t NimBLEUUID::bitSize() const {
    return this->m_uuid.u.type;
}
inline const uint8_t* NimBLEUUID::getValue() const {
    switch (bitSize()) {
        case BLE_UUID_TYPE_16:
            return reinterpret_cast<const uint8_t*>(&m_uuid.u16.value);
        case BLE_UUID_TYPE_32:
            return reinterpret_cast<const uint8_t*>(&m_uuid.u32.value);
        case BLE_UUID_TYPE_128:
            return m_uuid.u128.value;
        default:
            return nullptr;
    }
}
inline bool NimBLEAdvertisementData::addData(const uint8_t* data, size_t length) {
    if (m_payload.size() + length > BLE_HS_ADV_MAX_SZ) {
        NIMBLE_LOGE(LOG_TAG, "Data length exceeded");
        return false;
    }

    m_payload.insert(m_payload.end(), data, data + length);
    return true;
}
inline bool NimBLEAdvertisementData::setFlags(uint8_t flag) {
    int dataLoc = getDataLocation(BLE_HS_ADV_TYPE_FLAGS);
    if (dataLoc != -1) {
        if (flag) {
            m_payload[dataLoc + 2] = flag | BLE_HS_ADV_F_BREDR_UNSUP;
            return true;
        } else {
            return removeData(BLE_HS_ADV_TYPE_FLAGS);
        }
    }

    uint8_t data[3];
    data[0] = 2;
    data[1] = BLE_HS_ADV_TYPE_FLAGS;
    data[2] = flag | BLE_HS_ADV_F_BREDR_UNSUP;
    return addData(data, 3);
}
inline bool NimBLEAdvertisementData::addServiceUUID(const NimBLEUUID& serviceUUID) {
    uint8_t bytes = serviceUUID.bitSize() / 8;
    int     type;
    switch (bytes) {
        case 2:
            type = BLE_HS_ADV_TYPE_COMP_UUIDS16;
            break;
        case 4:
            type = BLE_HS_ADV_TYPE_COMP_UUIDS32;
            break;
        case 16:
            type = BLE_HS_ADV_TYPE_COMP_UUIDS128;
            break;
        default:
            NIMBLE_LOGE(LOG_TAG, "Cannot add UUID, invalid size!");
            return false;
    }

    int     dataLoc = getDataLocation(type);
    uint8_t length  = bytes;
    if (dataLoc == -1) {
        length += 2;
    }

    if (length + getPayload().size() > BLE_HS_ADV_MAX_SZ) {
        NIMBLE_LOGE(LOG_TAG, "Cannot add UUID, data length exceeded!");
        return false;
    }

    uint8_t        data[BLE_HS_ADV_MAX_SZ];
    const uint8_t* uuid = serviceUUID.getValue();
    if (dataLoc == -1) {
        data[0] = 1 + bytes;
        data[1] = type;
        memcpy(&data[2], uuid, bytes);
        return addData(data, length);
    }

    m_payload.insert(m_payload.begin() + dataLoc + m_payload[dataLoc] + 1, uuid, uuid + bytes);
    m_payload[dataLoc] += bytes;
    return true;
}
inline bool NimBLEAdvertisementData::setName(const std::string& name, bool isComplete) {
    if (name.length() > BLE_HS_ADV_MAX_FIELD_SZ) {
        NIMBLE_LOGE(LOG_TAG, "Name too long - truncating");
        isComplete = false;
    }

    uint8_t data[BLE_HS_ADV_MAX_SZ];
    uint8_t length = 2 + std::min<uint8_t>(name.length(), BLE_HS_ADV_MAX_FIELD_SZ);
    data[0]        = length - 1;
    data[1]        = isComplete ? BLE_HS_ADV_TYPE_COMP_NAME : BLE_HS_ADV_TYPE_INCOMP_NAME;
    memcpy(&data[2], name.c_str(), std::min<uint8_t>(name.length(), BLE_HS_ADV_MAX_FIELD_SZ));
    return addData(data, length);
}
inline int NimBLEAdvertisementData::getDataLocation(uint8_t type) const {
    size_t index = 0;
    while (index < m_payload.size()) {
        if (m_payload[index + 1] == type) {
            return index;
        }
        index += m_payload[index] + 1;
    }
    return -1;
}
inline bool NimBLEAdvertisementData::removeData(uint8_t type) {
    int dataLoc = getDataLocation(type);
    if (dataLoc != -1) {
        std::vector<uint8_t> swap(m_payload.begin(), m_payload.begin() + dataLoc);
        int                  nextData = dataLoc + m_payload[dataLoc] + 1;
        swap.insert(swap.end(), m_payload.begin() + nextData, m_payload.end());
        swap.swap(m_payload);
        return true;
    }

    return false;
}
inline std::vector<uint8_t> NimBLEAdvertisementData::getPayload() const {
    return m_payload;
}

#undef NIMBLE_LOGE
