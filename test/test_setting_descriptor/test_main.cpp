#include <cstdio>

#include "foxie_embedded/setting_descriptor.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

constexpr const char* const kAnimationModes[] = {
    "off",
    "subtle",
    "full",
    nullptr,
};

constexpr fxe::SettingDescriptor kAnimationDescriptor(
    "anim.mode",
    fxe::SettingType::Enum,
    fxe::SettingCategory::Animation,
    1u,
    "subtle",
    0u,
    2u,
    kAnimationModes,
    3u,
    "Animation mode",
    fxe::VISIBLE_TO_APP | fxe::PERSISTED);

static_assert(kAnimationDescriptor.type == fxe::SettingType::Enum, "type must be constexpr");
static_assert(kAnimationDescriptor.category == fxe::SettingCategory::Animation,
              "category must be constexpr");
static_assert(kAnimationDescriptor.defaultUint == 1u, "default uint must be constexpr");
static_assert(kAnimationDescriptor.allowedCount == 3u, "allowed count must be constexpr");
static_assert(kAnimationDescriptor.HasFlag(fxe::VISIBLE_TO_APP), "flag helper must be constexpr");
static_assert(kAnimationDescriptor.IsPersisted(), "persisted helper must be constexpr");
static_assert(!kAnimationDescriptor.IsForceSave(), "force-save helper must be constexpr");

} // namespace

int main() {
    Check(kAnimationDescriptor.key != nullptr, "key exists");
    Check(kAnimationDescriptor.defaultStr != nullptr, "string default exists");
    Check(kAnimationDescriptor.allowedValues != nullptr, "allowed values exists");
    Check(kAnimationDescriptor.allowedValues[0] != nullptr, "first allowed value exists");
    Check(kAnimationDescriptor.allowedValues[2] != nullptr, "last allowed value exists");
    Check(kAnimationDescriptor.allowedValues[3] == nullptr, "allowed values sentinel exists");
    Check(kAnimationDescriptor.allowedValues[0][0] == 'o', "first allowed value content");
    Check(kAnimationDescriptor.allowedValues[2][0] == 'f', "last allowed value content");
    Check(kAnimationDescriptor.min == 0u, "min value set");
    Check(kAnimationDescriptor.max == 2u, "max value set");
    Check(kAnimationDescriptor.IsVisibleToApp(), "visible flag set");
    Check(kAnimationDescriptor.IsPersisted(), "persisted flag set");
    Check(!kAnimationDescriptor.IsInternal(), "internal flag not set");

    if (g_failures == 0) {
        std::puts("[TEST] setting_descriptor ok");
        return 0;
    }

    std::printf("[TEST] setting_descriptor failures=%d\n", g_failures);
    return 1;
}
