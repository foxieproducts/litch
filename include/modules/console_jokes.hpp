#pragma once

#include <cstdint>

#include "foxie_embedded/console.hpp"

namespace modules {

class ConsoleJokes final {
  public:
    bool Handle(fxe::Console& console, const fxe::Console::Args& args);

  private:
    uint8_t m_listReply = 0U;
    uint8_t m_cdReply = 0U;
    uint8_t m_lightsReply = 0U;
};

} // namespace modules
