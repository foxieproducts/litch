#pragma once

#include "modules/ambient_lux_averager.hpp"
#include "modules/upward_output_slew.hpp"

namespace firmware {

// Makes presentation-only brightness progress atomic with a completed LED
// transfer. Sensor samples and electrical authorization remain authoritative.
class LedBrightnessAttempt final {
  public:
    LedBrightnessAttempt(modules::AmbientLuxAverager::SmoothingCheckpoint& ambient,
                         modules::UpwardOutputSlew& source)
        : m_ambient(ambient), m_source(source), m_ambientBefore(ambient),
          m_sourceBefore(source) {}

    bool Finish(const bool completed) {
        if (completed) return true;
        m_ambient = m_ambientBefore;
        m_source = m_sourceBefore;
        return false;
    }

  private:
    modules::AmbientLuxAverager::SmoothingCheckpoint& m_ambient;
    modules::UpwardOutputSlew& m_source;
    modules::AmbientLuxAverager::SmoothingCheckpoint m_ambientBefore;
    modules::UpwardOutputSlew m_sourceBefore;
};

} // namespace firmware
