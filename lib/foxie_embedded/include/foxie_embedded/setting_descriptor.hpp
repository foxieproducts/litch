#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

enum class SettingType : uint8_t {
    UInt32 = 0,
    Bool,
    String,
    Enum,
};

enum class SettingCategory : uint8_t {
    Display = 0,
    Animation,
    Behavior,
    Clock,
    Hardware,
    Internal,
};

enum SettingDescriptorFlags : uint32_t {
    VISIBLE_TO_APP = 1u << 0,
    PERSISTED = 1u << 1,
    FORCE_SAVE = 1u << 2,
    INTERNAL = 1u << 3,
    RETIRED = 1u << 4,
};

struct SettingDescriptor {
    constexpr SettingDescriptor() = default;

    constexpr SettingDescriptor(const char* descriptorKey,
                                const SettingType descriptorType,
                                const SettingCategory descriptorCategory,
                                const uint32_t descriptorDefaultUint,
                                const char* descriptorDefaultStr,
                                const uint32_t descriptorMin,
                                const uint32_t descriptorMax,
                                const char* const* descriptorAllowedValues,
                                const uint8_t descriptorAllowedCount,
                                const char* descriptorDescription,
                                const uint32_t descriptorFlags)
        : key(descriptorKey),
          type(descriptorType),
          category(descriptorCategory),
          defaultUint(descriptorDefaultUint),
          defaultStr(descriptorDefaultStr),
          min(descriptorMin),
          max(descriptorMax),
          allowedValues(descriptorAllowedValues),
          allowedCount(descriptorAllowedCount),
          description(descriptorDescription),
          flags(descriptorFlags) {}

    const char* key = nullptr;
    SettingType type = SettingType::UInt32;
    SettingCategory category = SettingCategory::Internal;

    uint32_t defaultUint = 0;
    const char* defaultStr = nullptr;

    uint32_t min = 0;
    uint32_t max = 0;

    const char* const* allowedValues = nullptr;
    uint8_t allowedCount = 0;

    const char* description = nullptr;
    uint32_t flags = 0;

    [[nodiscard]] constexpr bool HasFlag(const uint32_t flag) const {
        return (flags & flag) != 0u;
    }

    [[nodiscard]] constexpr bool IsRetired() const {
        return HasFlag(RETIRED);
    }

    [[nodiscard]] constexpr bool IsPersisted() const {
        return HasFlag(PERSISTED);
    }

    [[nodiscard]] constexpr bool IsVisibleToApp() const {
        return HasFlag(VISIBLE_TO_APP);
    }

    [[nodiscard]] constexpr bool IsInternal() const {
        return HasFlag(INTERNAL);
    }

    [[nodiscard]] constexpr bool IsForceSave() const {
        return HasFlag(FORCE_SAVE);
    }
};

} // namespace fxe
