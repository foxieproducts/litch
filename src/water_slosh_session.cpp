#include "modules/water_slosh_session.hpp"

#include <algorithm>
#include <cmath>

namespace modules {
namespace {

constexpr uint32_t SAMPLE_STALE_MS = 240U;
constexpr uint32_t ENTRY_CONFIRM_MS = 60U;
constexpr uint32_t ENTRY_FADE_MS = 450U;
constexpr uint32_t QUIET_HOLD_MS = 1800U;
constexpr uint32_t EXIT_FADE_MS = 250U;
constexpr uint32_t CONTROL_QUIET_MS = 1200U;

} // namespace

bool WaterSloshSession::ControlQuiet(const uint32_t nowMs) const {
    return m_heldControls == 0U &&
           (!m_hasControl || nowMs - m_controlMs >= CONTROL_QUIET_MS);
}

void WaterSloshSession::ObserveControl(const ButtonEvent& event,
                                      const uint32_t nowMs) {
    const uint8_t id = static_cast<uint8_t>(event.button) +
                      (event.owner == UiOwner::P2 ? 16U : 0U);
    if (id < 32U) {
        if (event.event == fxe::Button::Event::Release) {
            m_heldControls &= ~(1UL << id);
        } else {
            m_heldControls |= 1UL << id;
        }
    }
    NoteControl(nowMs);
    // Physical controls have an immediate, authoritative LED acknowledgement.
    // Keep NoteControl() independently smooth for mode-to-mode handoffs, but
    // never let that cosmetic envelope obscure a held-button response.
    SetAdmitted(false, nowMs);
}

void WaterSloshSession::ResetControlInput(const uint32_t nowMs) {
    m_heldControls = 0U;
    NoteControl(nowMs);
    SetAdmitted(false, nowMs);
}

void WaterSloshSession::NoteControl(const uint32_t nowMs) {
    m_controlMs = nowMs;
    m_hasControl = true;
    m_candidate = false;
    if (m_active) { m_fade.SetTarget(0U, EXIT_FADE_MS, nowMs); }
}

void WaterSloshSession::FadeOut(const uint32_t nowMs) {
    m_admitted = false;
    m_candidate = false;
    if (m_active) { m_fade.SetTarget(0U, EXIT_FADE_MS, nowMs); }
}

void WaterSloshSession::Cancel(const uint32_t nowMs) {
    m_active = false;
    m_hasSample = false;
    m_candidate = false;
    m_fade.Reset(0U, nowMs);
    m_updateMs = nowMs;
}

void WaterSloshSession::SetAdmitted(const bool admitted,
                                   const uint32_t nowMs) {
    const bool next = admitted && ControlQuiet(nowMs);
    if (next == m_admitted) { return; }
    m_admitted = next;
    if (!m_admitted) {
        // Admission loss means another production owner needs the LEDs now
        // (piece guidance, health, sleep, controls, or another mode). Preserve
        // gentle set-down/stale fades inside Update(), but never blend cosmetic
        // water over an authoritative frame.
        Cancel(nowMs);
    } else if (m_active) {
        m_fade.SetTarget(255U, ENTRY_FADE_MS, nowMs);
    }
}

void WaterSloshSession::Observe(const WaterSloshMotion& motion,
                               const uint32_t nowMs) {
    if (!m_admitted) {
        return;
    }
    const uint32_t sampleElapsed = m_hasSample ? nowMs - m_sampleMs : 0U;
    // An 8 ms one-pole force filter removes sensor grain without masking
    // impulse direction. The coefficient follows acquisition time, not FPS.
    const float alpha = !m_hasSample ? 1.0F :
        float(sampleElapsed) / (8.0F + float(sampleElapsed));
    const std::array<float, 3> values{motion.downXG, motion.downYG, motion.normalG};
    bool handling = false;
    bool awayFromRest = false;
    float changeG = 0.0F;
    for (unsigned axis = 0U; axis < 3U; ++axis) {
        const float value = values[axis];
        // Ordered comparison rejects NaN/infinity too; never use fast-math.
        if (!(std::fabs(value) <= 4.0F)) {
            Cancel(nowMs);
            return;
        }
        const bool inPlane = axis < 2U;
        const bool displaced = inPlane ? std::fabs(value) >= 0.14F
                                       : std::fabs(value - 1.0F) >= 0.10F;
        const float axisChangeG = m_hasSample ? std::fabs(value - m_previous[axis]) : 0.0F;
        awayFromRest |= displaced;
        handling |= (inPlane && displaced) || axisChangeG >= 0.10F;
        changeG += axisChangeG;
        m_force[axis] += alpha * (value - m_force[axis]);
        m_previous[axis] = value;
    }
    m_hasSample = true;
    m_sampleMs = nowMs;
    if (m_active) { m_surface.ExciteFoam(changeG); }
    if (handling) {
        m_handlingMs = nowMs;
    }
    if (m_active) { return; }
    if (!awayFromRest) {
        m_candidate = false;
    } else if (!m_candidate || sampleElapsed >= ENTRY_CONFIRM_MS) {
        // A bump can produce a large step both on impact and on return to
        // level. Confirm only while samples stay away from rest, and restart
        // if a gap spans the whole confirmation interval.
        m_candidate = true;
        m_candidateMs = nowMs;
    } else if (nowMs - m_candidateMs >= ENTRY_CONFIRM_MS) {
        m_surface.Reset();
        m_active = true;
        m_handlingMs = nowMs;
        m_updateMs = nowMs;
        m_fade.Reset(0U, nowMs);
        m_fade.SetTarget(255U, ENTRY_FADE_MS, nowMs);
    }
}

void WaterSloshSession::Update(const uint32_t nowMs) {
    if (!m_active) { return; }
    if (!m_admitted || !m_hasSample || nowMs - m_sampleMs >= SAMPLE_STALE_MS) {
        m_fade.SetTarget(0U, EXIT_FADE_MS, nowMs);
    }
    const uint32_t elapsed = nowMs - m_updateMs;
    m_surface.Advance({m_force[0], m_force[1], m_force[2]}, elapsed);
    m_updateMs = nowMs;

    if (!m_admitted || !m_hasSample || nowMs - m_sampleMs >= SAMPLE_STALE_MS) {
        m_fade.Update(nowMs);
        if (m_fade.IsSettled() && m_fade.Target() == 0U) { Cancel(nowMs); }
        return;
    }
    const uint32_t quietMs = nowMs - m_handlingMs;
    m_fade.SetTarget(quietMs < QUIET_HOLD_MS ? 255U : 0U,
                     quietMs < QUIET_HOLD_MS ? ENTRY_FADE_MS : EXIT_FADE_MS,
                     nowMs);
    m_fade.Update(nowMs);
    if (m_fade.IsSettled() && m_fade.Target() == 0U) { Cancel(nowMs); }
}

} // namespace modules
