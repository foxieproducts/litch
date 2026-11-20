#pragma once

#include <cstdint>
#include <array>

#include "modules/water_slosh.hpp"
#include "modules/smooth_fade.hpp"
#include "modules/user_input.hpp"

namespace modules {

// Owns one handling episode; the firmware supplies admission and fresh samples.
// Sensor data/time are injected, so exactly this lifecycle is used in replay.
class WaterSloshSession final {
  public:
    void SetAdmitted(bool admitted, uint32_t nowMs);
    void FadeOut(uint32_t nowMs);
    void Observe(const WaterSloshMotion& motion, uint32_t nowMs);
    void SetAmbientLight(float lux, bool available) { m_surface.SetAmbientLight(lux, available); }
    void Update(uint32_t nowMs);
    void Cancel(uint32_t nowMs);
    void NoteControl(uint32_t nowMs);
    void ObserveControl(const ButtonEvent& event, uint32_t nowMs);
    void ResetControlInput(uint32_t nowMs);
    bool IsActive() const { return m_active; }
    bool IsAdmitted() const { return m_admitted; }
    bool ControlQuiet(uint32_t nowMs) const;
    uint8_t Opacity255() const { return m_fade.Value(); }
    const WaterSlosh& Surface() const { return m_surface; }

  private:
    WaterSlosh m_surface;
    SmoothFade255 m_fade;
    std::array<float, 3> m_force{0.0F, 0.0F, 1.0F};
    std::array<float, 3> m_previous{};
    uint32_t m_sampleMs = 0U;
    uint32_t m_updateMs = 0U;
    uint32_t m_handlingMs = 0U;
    uint32_t m_candidateMs = 0U;
    uint32_t m_controlMs = 0U;
    uint32_t m_heldControls = 0U;
    bool m_active = false;
    bool m_hasSample = false;
    bool m_candidate = false;
    bool m_admitted = false;
    bool m_hasControl = false;
};

static_assert(sizeof(WaterSloshSession) <= 512U,
              "water handling must fit the 512-byte persistent-state budget");

} // namespace modules
