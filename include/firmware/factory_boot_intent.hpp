#pragma once

#include <cstdint>
#include <type_traits>

namespace firmware {

enum class FactoryBootIntent : uint8_t { None, Test, Verifier };
enum class FactoryBootProfile : uint8_t { Normal, Factory, Verifier };

inline constexpr FactoryBootProfile FactoryBootProfileFor(
    const bool testRequested, const bool validatedVerifierRequested) {
    return testRequested ? FactoryBootProfile::Factory
        : validatedVerifierRequested ? FactoryBootProfile::Verifier
        : FactoryBootProfile::Normal;
}

// One software-reset request, never a durable factory qualification record.
struct FactoryBootRequest {
    uint32_t tag;
    uint32_t complement;
};
static_assert(std::is_trivial_v<FactoryBootRequest> && sizeof(FactoryBootRequest) == 8U,
              "RTC request must survive reset without a C++ startup initializer");

inline void ArmFactoryBootRequest(FactoryBootRequest& request,
                                  const FactoryBootIntent intent) {
    request.tag = intent == FactoryBootIntent::None
        ? 0U : 0x46425400U | static_cast<uint8_t>(intent);
    request.complement = ~request.tag;
}

inline FactoryBootIntent ConsumeFactoryBootRequest(
    FactoryBootRequest& request, const bool softwareReset,
    const bool recovery, const bool supported) {
    const auto saved = request;
    request = {};
    if (!softwareReset || recovery || !supported ||
        saved.complement != ~saved.tag) return FactoryBootIntent::None;
    if (saved.tag == 0x46425401U) return FactoryBootIntent::Test;
    if (saved.tag == 0x46425402U) return FactoryBootIntent::Verifier;
    return FactoryBootIntent::None;
}

} // namespace firmware
