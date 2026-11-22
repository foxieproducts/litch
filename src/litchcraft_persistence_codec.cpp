#include "modules/litchcraft_persistence.hpp"

#include <algorithm>
#include <cstring>

#include "foxie_embedded/checksum.hpp"

namespace modules {
namespace {
constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::array<uint8_t, 4U> ADVANCED_MAGIC = {'L', 'C', 'A', '3'};
constexpr uint8_t ADVANCED_ENVELOPE_VERSION = 1U;
constexpr uint8_t ADVANCED_ACTIVE_FLAG = 1U;
using V1RecordBytes = std::array<uint8_t, 770U>;
using V2RecordBytes = std::array<uint8_t, 794U>;
V1RecordBytes V1Bytes(const LitchcraftRecord& record) {
    V1RecordBytes bytes{};
    bytes[0] = 1U;
    bytes[1] = record.active ? 1U : 0U;
    for (size_t i = 0U; i < record.canvas.size(); ++i) {
        bytes[2U + i * 3U] = record.canvas[i].r;
        bytes[3U + i * 3U] = record.canvas[i].g;
        bytes[4U + i * 3U] = record.canvas[i].b;
    }
    return bytes;
}
V2RecordBytes V2Bytes(const LitchcraftRecord& record) {
    V2RecordBytes bytes{};
    bytes[0] = 2U;
    bytes[1] = record.active ? 1U : 0U;
    for (size_t i = 0U; i < record.canvas.size(); ++i) {
        bytes[2U + i * 3U] = record.canvas[i].r;
        bytes[3U + i * 3U] = record.canvas[i].g;
        bytes[4U + i * 3U] = record.canvas[i].b;
    }
    constexpr size_t paletteOffset = 2U + 256U * 3U;
    for (size_t i = 0U; i < record.quickPalette.size(); ++i) {
        bytes[paletteOffset + i * 3U] = record.quickPalette[i].r;
        bytes[paletteOffset + i * 3U + 1U] = record.quickPalette[i].g;
        bytes[paletteOffset + i * 3U + 2U] = record.quickPalette[i].b;
    }
    return bytes;
}
int Decode(const char value) {
    if (value == '\0') return -1;
    const char* found = std::strchr(BASE64, value);
    return found ? static_cast<int>(found - BASE64) : -1;
}

void WriteLe16(uint8_t* const out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
}

void WriteLe32(uint8_t* const out, const uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
    out[2] = static_cast<uint8_t>(value >> 16U);
    out[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t ReadLe16(const uint8_t* const bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t ReadLe32(const uint8_t* const bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}
} // namespace

void LitchcraftPersistence::RefreshAdvancedEnvelopeChecksum(
    AdvancedEnvelope& envelope) {
    if (envelope.size < ADVANCED_BODY_OFFSET + ADVANCED_ENVELOPE_TRAILER_BYTES ||
        envelope.size > ADVANCED_SLOT_BYTES ||
        envelope.size > envelope.bytes.size()) {
        return;
    }
    WriteLe32(envelope.bytes.data() + envelope.size - ADVANCED_ENVELOPE_TRAILER_BYTES,
              fxe::Crc32(envelope.bytes.data(),
                         envelope.size - ADVANCED_ENVELOPE_TRAILER_BYTES));
}

LitchcraftPersistence::AdvancedRecordStatus
LitchcraftPersistence::ReadAdvancedSlot(const AdvancedSlot slot) {
    if (!IsAdvancedConfigured()) return AdvancedRecordStatus::BackendUnavailable;
    AdvancedEnvelope& envelope = m_advanced->envelope;
    envelope.size = 0U;
    size_t size = 0U;
    const auto read = m_ops.readAdvanced(m_ops.context, slot,
                                         envelope.bytes.data(),
                                         envelope.bytes.size(), size);
    if (read == AdvancedReadOutcome::Missing) return AdvancedRecordStatus::Missing;
    if (read == AdvancedReadOutcome::Failed) {
        return AdvancedRecordStatus::BackendUnavailable;
    }
    if (size < ADVANCED_BODY_OFFSET + ADVANCED_ENVELOPE_TRAILER_BYTES ||
        size > ADVANCED_SLOT_BYTES || size > envelope.bytes.size()) {
        return AdvancedRecordStatus::Invalid;
    }
    envelope.size = size;
    const uint8_t* const bytes = envelope.bytes.data();
    if (!std::equal(ADVANCED_MAGIC.begin(), ADVANCED_MAGIC.end(), bytes)) {
        return AdvancedRecordStatus::Invalid;
    }
    if (bytes[4] > ADVANCED_ENVELOPE_VERSION) {
        return AdvancedRecordStatus::FutureVersion;
    }
    if (bytes[4] != ADVANCED_ENVELOPE_VERSION ||
        (bytes[5] & ~ADVANCED_ACTIVE_FLAG) != 0U || bytes[6] != 0U ||
        bytes[7] != 0U || bytes[10] != 0U || bytes[11] != 0U) {
        return AdvancedRecordStatus::Invalid;
    }
    const size_t bodySize = ReadLe16(bytes + 8U);
    const size_t encodedSize = ADVANCED_BODY_OFFSET + bodySize +
                               ADVANCED_ENVELOPE_TRAILER_BYTES;
    if (bodySize < litchcraft_animation::ENCODED_HEADER_BYTES ||
        bodySize > litchcraft_animation::MAX_ENCODED_BYTES ||
        size != encodedSize ||
        fxe::Crc32(bytes, size - ADVANCED_ENVELOPE_TRAILER_BYTES) !=
            ReadLe32(bytes + size - ADVANCED_ENVELOPE_TRAILER_BYTES)) {
        return AdvancedRecordStatus::Invalid;
    }
    const uint8_t schema = bytes[ADVANCED_BODY_OFFSET];
    if (schema > litchcraft_animation::SCHEMA) {
        return fxe::Crc32(bytes + ADVANCED_BODY_OFFSET, bodySize) ==
                       ReadLe32(bytes + 12U)
            ? AdvancedRecordStatus::FutureVersion
            : AdvancedRecordStatus::Invalid;
    }
    if (schema != litchcraft_animation::SCHEMA) {
        return AdvancedRecordStatus::Invalid;
    }
    const bool valid = litchcraft_animation::DecodeDocument(
        bytes + ADVANCED_BODY_OFFSET, bodySize, ReadLe32(bytes + 12U),
        m_advanced->staging, m_advanced->document);
    if (!valid) return AdvancedRecordStatus::Invalid;
    m_advanced->active = (bytes[5] & ADVANCED_ACTIVE_FLAG) != 0U;
    // Retain the validated restore identity without keeping another envelope.
    // Boot revisions/receipts remain zero: they are runtime-only counters.
    m_savedEnvelopeCrc = ReadLe32(bytes + size - ADVANCED_ENVELOPE_TRAILER_BYTES);
    m_savedEnvelopeFrames = m_advanced->document.Value().frameCount;
    return AdvancedRecordStatus::Valid;
}

LitchcraftRecordStatus ReadLitchcraftRecord(const JsonDocument& document, LitchcraftRecord& out) {
    if (!document.containsKey(LITCHCRAFT_ART_KEY)) return LitchcraftRecordStatus::Missing;
    const auto record = document[LITCHCRAFT_ART_KEY].as<JsonArrayConst>();
    if (record.isNull() || !record[0].is<uint32_t>()) return LitchcraftRecordStatus::Invalid;
    const uint32_t version = record[0].as<uint32_t>();
    if (version > 2U) return LitchcraftRecordStatus::FutureVersion;
    const bool validShape =
        (version == 1U && record.size() == 4U) ||
        (version == 2U && record.size() == 5U);
    if (!validShape || !record[1].is<bool>() ||
        !record[2].is<const char*>() ||
        (version == 2U && !record[3].is<const char*>()) ||
        !record[version == 1U ? 3U : 4U].is<uint32_t>()) {
        return LitchcraftRecordStatus::Invalid;
    }
    const char* encoded = record[2].as<const char*>();
    if (std::strlen(encoded) != 1024U) return LitchcraftRecordStatus::Invalid;
    LitchcraftRecord candidate;
    candidate.active = record[1].as<bool>();
    for (size_t i = 0U; i < candidate.canvas.size(); ++i) {
        const int a = Decode(encoded[i * 4U]);
        const int b = Decode(encoded[i * 4U + 1U]);
        const int c = Decode(encoded[i * 4U + 2U]);
        const int d = Decode(encoded[i * 4U + 3U]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return LitchcraftRecordStatus::Invalid;
        candidate.canvas[i] = {uint8_t((a << 2U) | (b >> 4U)),
                               uint8_t((b << 4U) | (c >> 2U)), uint8_t((c << 6U) | d)};
    }
    uint32_t expectedCrc = 0U;
    if (version == 1U) {
        const auto bytes = V1Bytes(candidate);
        expectedCrc = fxe::Crc32(bytes.data(), bytes.size());
    } else {
        const char* encodedPalette = record[3].as<const char*>();
        if (std::strlen(encodedPalette) != 32U) return LitchcraftRecordStatus::Invalid;
        for (size_t i = 0U; i < candidate.quickPalette.size(); ++i) {
            const int a = Decode(encodedPalette[i * 4U]);
            const int b = Decode(encodedPalette[i * 4U + 1U]);
            const int c = Decode(encodedPalette[i * 4U + 2U]);
            const int d = Decode(encodedPalette[i * 4U + 3U]);
            if (a < 0 || b < 0 || c < 0 || d < 0) return LitchcraftRecordStatus::Invalid;
            candidate.quickPalette[i] = {
                uint8_t((a << 2U) | (b >> 4U)),
                uint8_t((b << 4U) | (c >> 2U)),
                uint8_t((c << 6U) | d)};
        }
        const auto bytes = V2Bytes(candidate);
        expectedCrc = fxe::Crc32(bytes.data(), bytes.size());
    }
    const uint32_t storedCrc = record[version == 1U ? 3U : 4U].as<uint32_t>();
    if (expectedCrc != storedCrc) {
        return LitchcraftRecordStatus::Invalid;
    }
    out = candidate;
    return LitchcraftRecordStatus::Valid;
}

bool WriteLitchcraftRecord(JsonDocument& document, const LitchcraftRecord& record,
                          LitchcraftEncodedCanvas& storage,
                          LitchcraftEncodedQuickPalette& paletteStorage) {
    const auto bytes = V2Bytes(record);
    for (size_t i = 0U; i < record.canvas.size(); ++i) {
        const auto pixel = record.canvas[i];
        storage[i * 4U] = BASE64[pixel.r >> 2U];
        storage[i * 4U + 1U] = BASE64[((pixel.r & 3U) << 4U) | (pixel.g >> 4U)];
        storage[i * 4U + 2U] = BASE64[((pixel.g & 15U) << 2U) | (pixel.b >> 6U)];
        storage[i * 4U + 3U] = BASE64[pixel.b & 63U];
    }
    storage.back() = '\0';
    for (size_t i = 0U; i < record.quickPalette.size(); ++i) {
        const auto color = record.quickPalette[i];
        paletteStorage[i * 4U] = BASE64[color.r >> 2U];
        paletteStorage[i * 4U + 1U] = BASE64[((color.r & 3U) << 4U) | (color.g >> 4U)];
        paletteStorage[i * 4U + 2U] = BASE64[((color.g & 15U) << 2U) | (color.b >> 6U)];
        paletteStorage[i * 4U + 3U] = BASE64[color.b & 63U];
    }
    paletteStorage.back() = '\0';
    auto array = document[LITCHCRAFT_ART_KEY].as<JsonArray>();
    if (array.isNull()) array = document[LITCHCRAFT_ART_KEY].to<JsonArray>();
    array[0] = 2U;
    array[1] = record.active;
    array[2] = static_cast<const char*>(storage.data());
    array[3] = static_cast<const char*>(paletteStorage.data());
    array[4] = fxe::Crc32(bytes.data(), bytes.size());
    return !document.overflowed() && array.size() == 5U;
}
bool ScrubLitchcraftRecord(JsonDocument& document, fxe::SettingsRemovedKeys* removed) {
    LitchcraftRecord record;
    const auto status = ReadLitchcraftRecord(document, record);
    if (status == LitchcraftRecordStatus::Missing || status == LitchcraftRecordStatus::Valid) return false;
    if (removed != nullptr && !removed->Record(LITCHCRAFT_ART_KEY)) return false;
    document.remove(LITCHCRAFT_ART_KEY);
    return true;
}
} // namespace modules
