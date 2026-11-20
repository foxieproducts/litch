#pragma once

#include "foxie_embedded/millis.hpp"

namespace fxe::test_support {

class ManualMillisGuard final {
  public:
    ManualMillisGuard() : m_override(&fxe::ManualMillisProvider) {}
    ~ManualMillisGuard() = default;

    ManualMillisGuard(const ManualMillisGuard&) = delete;
    ManualMillisGuard& operator=(const ManualMillisGuard&) = delete;

  private:
    fxe::ScopedMillisProviderOverride m_override;
};

} // namespace fxe::test_support
