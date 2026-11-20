#include <cmath>
#include <cstdio>
#include <limits>

#include "hardware/board_led_output_mapping.hpp"
#include "hardware/motion_board_mapping.hpp"
#include "modules/water_slosh.hpp"
#include "modules/water_slosh_session.hpp"
#include "modules/water_slosh_presentation.hpp"

namespace {

int g_failures = 0;
void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void Run(modules::WaterSlosh& water,
         const modules::WaterSloshMotion motion,
         const uint32_t milliseconds) {
    for (uint32_t t = 0U; t < milliseconds; t += 8U) {
        water.Advance(motion, 8U);
    }
}

bool SameSurface(const modules::WaterSlosh& a, const modules::WaterSlosh& b) {
    const auto x = a.Measure();
    const auto y = b.Measure();
    return x.volume == y.volume && x.centerX == y.centerX &&
           x.centerY == y.centerY && x.surfaceVariance == y.surfaceVariance &&
           x.speedSquared == y.speedSquared;
}

void CheckPhysicalResponse() {
    modules::WaterSlosh water;
    const auto rest = water.Measure();
    Run(water, {0.45F, 0.0F, 0.89F}, 800U);
    const auto tilted = water.Measure();
    Check(tilted.centerX > rest.centerX + 0.35F,
          "lifting the left edge moves water toward the right/lower edge");
    Check(std::fabs(tilted.centerY - rest.centerY) < 0.001F,
          "one-axis tilt does not create sideways drift");
    Check(std::fabs(tilted.volume - rest.volume) < 0.01F,
          "tilt conserves the water volume");
    Run(water, {}, 160U);
    const auto released = water.Measure();
    Check(released.centerX > tilted.centerX,
          "water continues moving after the board levels: inertial slosh");
    Run(water, {}, 20000U);
    const auto settled = water.Measure();
    Check(std::fabs(settled.centerX - rest.centerX) < 0.25F &&
              settled.surfaceVariance < 0.2F,
          "reflections damp back toward a calm level surface");
}

void CheckBoundsAndDeterminism() {
    modules::WaterSlosh water;
    const auto volume = water.Measure().volume;
    for (uint32_t t = 0U; t < 120000U; t += 8U) {
        const float x = (t / 200U) % 2U == 0U ? 4.0F : -4.0F;
        const float y = (t / 304U) % 2U == 0U ? 4.0F : -4.0F;
        water.Advance({x, y, (t / 500U) % 2U == 0U ? 0.0F : 4.0F}, 8U);
    }
    const auto shaken = water.Measure();
    Check(std::isfinite(shaken.volume) && shaken.minimumDepth >= 0.0F &&
              shaken.maximumDepth <= volume,
          "two minutes of over-range shaking/freefall cannot create negative or unbounded water");
    Check(std::fabs(shaken.volume - volume) < 0.05F,
          "long diagonal shaking conserves volume within floating point roundoff");
    water.Reset();
    for (uint32_t i = 0U; i < 1000U; ++i) {
        water.Advance({i % 2U ? 0.012F : -0.012F, 0.015F, 1.008F}, 8U);
    }
    Check(water.Measure().surfaceVariance == 0.0F,
          "measured stationary sensor noise cannot animate the level surface");
    modules::WaterSlosh a;
    modules::WaterSlosh b;
    Run(a, {0.35F, -0.2F, 0.92F}, 960U);
    for (uint32_t t = 0U; t < 960U; t += 20U) {
        b.Advance({0.35F, -0.2F, 0.92F}, 20U);
    }
    Check(SameSurface(a, b),
          "50 Hz rendering and 125 Hz stepping yield identical fixed-step water");
    a.Reset();
    b.Reset();
    a.Advance({0.4F, 0.0F, 1.0F}, UINT32_MAX);
    b.Advance({0.4F, 0.0F, 1.0F}, modules::WaterSlosh::MAX_ADVANCE_MS);
    Check(SameSurface(a, b),
          "a paused render loop never replays an unbounded physics backlog");
    a.Advance({std::numeric_limits<float>::quiet_NaN(),
               std::numeric_limits<float>::infinity(), 0.0F}, 64U);
    Check(std::isfinite(a.Measure().volume),
          "invalid external floats cannot poison the bounded core");
}

void Feed(modules::WaterSloshSession& session, uint32_t start,
          uint32_t duration, modules::WaterSloshMotion motion) {
    for (uint32_t elapsed = 0U; elapsed <= duration; elapsed += 20U) {
        const uint32_t now = start + elapsed;
        session.Observe(motion, now);
        session.Update(now);
    }
}

void CheckLighterCoverage() {
    modules::WaterSlosh water;
    const auto volume = water.Measure().volume;
    Check(volume > 71.68F * 0.74F && volume < 71.68F * 0.76F,
          "second HIL tuning carries another twenty-five percent less optical water");
    uint32_t energy = 0U;
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        const auto color = water.ComposePixel(pixel);
        Check(color.b != 0U, "lighter water still evaluates every square and perimeter position");
        energy += color.r + color.g + color.b;
    }
    Check(energy >= 288U * 45U && energy <= 288U * 58U,
          "calm water no longer saturates full coverage across the entire surface");
}

void CheckSoftEntry() {
    modules::WaterSloshSession session;
    session.SetAdmitted(true, 0U);
    for (uint32_t now = 0U; now <= 80U; now += 40U) {
        session.Observe({0.45F, 0.0F, 0.9F}, now);
        session.Update(now);
    }
    Check(session.IsActive() && session.Opacity255() == 0U,
          "confirmed pickup starts transparent, not as a switched-on sheet");
    uint8_t previous = 0U;
    for (uint32_t elapsed = 40U; elapsed <= 480U; elapsed += 40U) {
        session.Observe({0.45F, 0.0F, 0.9F}, 80U + elapsed);
        session.Update(80U + elapsed);
        const uint8_t alpha = session.Opacity255();
        Check(alpha >= previous, "pickup fade stays monotonic at the real 25 Hz sample cadence");
        if (elapsed == 40U) { Check(alpha <= 10U, "first visible 40 ms step is at most four percent"); }
        if (elapsed == 120U) {
            auto interrupted = session;
            const uint8_t before = interrupted.Opacity255();
            interrupted.FadeOut(80U + elapsed);
            Check(interrupted.IsActive() && interrupted.Opacity255() == before,
                  "pond return retargets an unfinished fade without an opacity jump");
            interrupted.Update(80U + elapsed + 125U);
            Check(interrupted.IsActive() && interrupted.Opacity255() > 0U &&
                      interrupted.Opacity255() < before,
                  "pond-to-board return is partway through its 250 ms cross-fade at midpoint");
            interrupted.Update(80U + elapsed + 250U);
            Check(!interrupted.IsActive() && interrupted.Opacity255() == 0U,
                  "pond-to-board return completes at exactly 250 ms");
        }
        if (elapsed == 240U) { Check(alpha >= 110U && alpha <= 165U, "half-second eased fade is around half at its midpoint"); }
        if (elapsed == 400U) { Check(alpha < 255U, "entry remains a fade beyond the old 180 ms abrupt onset"); }
        previous = alpha;
    }
    Check(session.Opacity255() == 255U, "450 ms entry has reached full effect by the next 25 Hz sample");
    session.FadeOut(560U);
    Check(session.IsActive() && session.Opacity255() == 255U,
          "pond return starts from the currently displayed water frame");
    session.Update(685U);
    Check(session.IsActive() && session.Opacity255() >= 120U &&
              session.Opacity255() <= 135U,
          "normal-board pixels are about half exposed at the 125 ms midpoint");
    session.Update(810U);
    Check(!session.IsActive() && session.Opacity255() == 0U,
          "normal board fully replaces the pond at the 250 ms boundary");

    modules::WaterSloshSession levelPickup;
    levelPickup.SetAdmitted(true, 0U);
    levelPickup.Observe({}, 0U);
    levelPickup.Update(0U);
    levelPickup.Observe({0.0F, 0.0F, 1.18F}, 40U);
    levelPickup.Update(40U);
    levelPickup.Observe({}, 80U);
    levelPickup.Update(80U);
    levelPickup.Observe({}, 120U);
    levelPickup.Update(120U);
    Check(!levelPickup.IsActive() && levelPickup.Opacity255() == 0U,
          "an isolated vertical impulse clears when the board returns to rest");
    for (uint32_t now = 160U; now <= 240U; now += 40U) {
        levelPickup.Observe({0.0F, 0.0F, 1.18F}, now);
        levelPickup.Update(now);
    }
    Check(levelPickup.IsActive() && levelPickup.Opacity255() == 0U,
          "sustained level pickup starts the fade without needing tilt");
}

void CheckVerticalPressure() {
    modules::WaterSlosh up;
    modules::WaterSlosh down;
    Run(up, {0.0F, 0.0F, 1.5F}, 240U);
    Run(down, {0.0F, 0.0F, 0.4F}, 240U);
    const auto aqua = up.ComposePixel(130U);
    const auto indigo = down.ComposePixel(130U);
    Check(aqua.g > indigo.g + 10U && aqua.g * indigo.b > indigo.g * aqua.b,
          "upward specific force brightens aqua while low-g becomes deeper indigo");
    Check(aqua.r + aqua.g + aqua.b > indigo.r + indigo.g + indigo.b,
          "pressure tint is brighter under upward force without a free-running phase");
    Check(SameSurface(up, down), "vertical tint alone cannot invent lateral waves");
    Run(up, {}, 1000U);
    Run(down, {}, 1000U);
    const auto a = up.ComposePixel(130U);
    const auto b = down.ComposePixel(130U);
    Check(a.r == b.r && a.g == b.g && a.b == b.b,
          "equal final normal force returns exactly the same calm color");
}

void CheckShakeFoam() {
    modules::WaterSloshSession session;
    session.SetAdmitted(true, 0U);
    Feed(session, 0U, 600U, {0.3F, 0.0F, 1.0F});
    for (uint32_t now = 640U; now <= 1440U; now += 40U) {
        const float sign = (now / 40U) % 2U == 0U ? 1.0F : -1.0F;
        session.Observe({0.4F * sign, 0.3F * sign, 1.0F + 0.4F * sign}, now);
        session.Update(now);
    }
    unsigned flecks = 0U;
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        const auto color = session.Surface().ComposePixel(pixel);
        flecks += color.r > 28U && color.g > 30U && color.b > 35U;
    }
    Check(flecks > 8U && flecks < 180U,
          "three-axis shaking makes scattered visible pale foam, not a uniform flash");
    auto settled = session.Surface();
    Run(settled, {}, 12000U);
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        Check(settled.ComposePixel(pixel).r < 10U,
              "shake foam decays completely when the board stops");
    }
    session.NoteControl(1440U);
    Check(session.IsActive(), "foam cannot force a hard cut during control handoff");
    session.Update(2340U);
    Check(!session.IsActive(), "foam never delays completion of control handoff");
}

void CheckAmbientCaustic() {
    modules::WaterSloshSession dim;
    modules::WaterSloshSession bright;
    for (auto* session : {&dim, &bright}) {
        session->SetAdmitted(true, 0U);
        Feed(*session, 0U, 800U, {0.45F, 0.0F, 0.9F});
    }
    dim.SetAmbientLight(0.0F, true);
    bright.SetAmbientLight(1000.0F, true);
    unsigned changed = 0U;
    uint32_t dimEnergy = 0U;
    uint32_t brightEnergy = 0U;
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        const auto a = dim.Surface().ComposePixel(pixel);
        const auto b = bright.Surface().ComposePixel(pixel);
        changed += a.r != b.r || a.g != b.g || a.b != b.b;
        dimEnergy += a.r + a.g + a.b;
        brightEnergy += b.r + b.g + b.b;
    }
    Check(changed > 64U && brightEnergy > dimEnergy,
          "cached ambient mean brightens modal caustics across square and perimeter positions");
    dim.SetAmbientLight(0.0F, false);
    bright.SetAmbientLight(std::numeric_limits<float>::infinity(), true);
    for (uint16_t pixel = 0U; pixel < 288U; ++pixel) {
        const auto a = dim.Surface().ComposePixel(pixel);
        const auto b = bright.Surface().ComposePixel(pixel);
        Check(a.r == b.r && a.g == b.g && a.b == b.b,
              "unavailable and invalid light use the same deterministic neutral fallback");
    }
}

void CheckSessionAndMapping() {
    const auto mapping = hardware::MotionBoardMappingFor(hardware::BoardRevision::V3S3);
    modules::WaterSloshMotion motion;
    Check(mapping.Transform({0, 0, 0, 29, -646, 741}, motion) &&
              motion.downXG > 0.6F && std::fabs(motion.downYG) < 0.04F,
          "measured left-edge lift drives the physical right side");
    Check(mapping.Transform({0, 0, 0, -558, -2, 854}, motion) &&
              motion.downYG > 0.55F && std::fabs(motion.downXG) < 0.01F,
          "measured P2-edge lift drives the physical P1 side");
    Check(!hardware::MotionBoardMappingFor(hardware::BoardRevision::V2S3).IsValid() &&
              !hardware::MotionBoardMappingFor(hardware::BoardRevision::Unknown).IsValid() &&
              !hardware::MotionBoardMapping{1, 1, 3}.IsValid(),
          "missing, unsupported and duplicate-axis mappings fail closed");
    modules::WaterSloshSession session;
    session.SetAdmitted(true, 0U);
    Feed(session, 0U, 1000U, {-0.011F, -0.029F, 1.023F});
    Check(!session.IsActive(), "real measured resting board remains quiet");
    Feed(session, 1020U, 400U, {0.45F, 0.0F, 0.9F});
    Check(session.IsActive() && session.Opacity255() > 0U,
          "sustained handling starts the automatic eased fade");
    modules::BoardLedFrame emptyBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> emptyStatus{};
    auto a = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V3S3, false, false},
        emptyBoard, emptyStatus);
    auto b = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V3S3, true, true},
        emptyBoard, emptyStatus);
    modules::ApplyWaterSloshPhysicalPresentation(
        session, hardware::BoardRevision::V3S3, a);
    modules::ApplyWaterSloshPhysicalPresentation(
        session, hardware::BoardRevision::V3S3, b);
    bool same = true;
    for (size_t i = 0U; i < a.size(); ++i) {
        same &= a[i].r == b[i].r && a[i].g == b[i].g && a[i].b == b[i].b;
    }
    Check(same, "output orientation cannot rotate physically downhill liquid");
    Feed(session, 1440U, 2000U, {});
    Check(session.IsActive() && session.Opacity255() < 255U,
          "set-down leaves inertial settling then a gentle exit fade");
    Feed(session, 3460U, 1000U, {});
    Check(!session.IsActive(), "quiet set-down returns the normal board automatically");
    Feed(session, 4480U, 400U, {0.45F, 0.0F, 0.9F});
    session.Update(5120U);
    Check(session.IsActive() && session.Opacity255() > 0U,
          "lost sensor samples begin a continuous fade instead of cutting out");
    const uint8_t staleStartOpacity = session.Opacity255();
    session.Update(5245U);
    Check(session.IsActive() && session.Opacity255() > 0U &&
              session.Opacity255() < staleStartOpacity,
          "lost sensor return is partway through its 250 ms cross-fade at midpoint");
    session.Update(5370U);
    Check(!session.IsActive(), "lost sensor return completes at 250 ms without replaying stale tilt");
    session.SetAdmitted(true, UINT32_MAX - 200U);
    Feed(session, UINT32_MAX - 200U, 400U, {0.4F, 0.0F, 0.9F});
    Check(session.IsActive(), "handling lifecycle survives millisecond wrap");
    session.ObserveControl({modules::UserButton::P1X, fxe::Button::Event::Press,
                            modules::UiOwner::P1}, 220U);
    Check(!session.IsActive() && !session.ControlQuiet(2000U),
          "any held control immediately yields the LEDs to its acknowledgement");
    session.Update(1120U);
    Check(!session.IsActive(), "held control cannot revive cosmetic water");
    session.ObserveControl({modules::UserButton::P1X, fxe::Button::Event::Release,
                            modules::UiOwner::P1}, 2020U);
    Check(!session.ControlQuiet(2100U) && session.ControlQuiet(3220U),
          "control release needs a quiet interval before automatic reentry");
}

void CheckAdmission() {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::FreshSetup;
    snapshot.interaction.taggedState.tag = runtime::ChessInteractionPhase::Correction;
    modules::WaterSloshPresentationContext context;
    context.boardObserved = true;
    context.awake = true;
    Check(modules::CanPresentWaterSlosh(snapshot, context),
          "only empty fresh setup has the replaceable opening guide");
    snapshot.board.physicalOccupiedMask = 1ULL << 48U;
    Check(!modules::CanPresentWaterSlosh(snapshot, context),
          "the very first placed piece reclaims chess presentation");
    snapshot.board.physicalOccupiedMask = 0ULL;
    snapshot.game.variant = modules::GameVariant::Undecided;
    snapshot.game.initialDuckCandidateSquare = 18U;
    snapshot.board.physicalOccupiedMask = 1ULL << 18U;
    snapshot.interaction.humanIllegalMask = 1ULL << 18U;
    Check(modules::CanPresentWaterSlosh(snapshot, context),
          "authoritative provisional Vex alone can ride in fresh setup");
    snapshot.game.variant = modules::GameVariant::Duck;
    snapshot.game.duckSquare = 18U;
    Check(modules::CanPresentWaterSlosh(snapshot, context),
          "authoritative tracked Vex alone can ride in fresh setup");
    snapshot.interaction.humanIllegalMask |= 1ULL << 19U;
    Check(!modules::CanPresentWaterSlosh(snapshot, context),
          "any non-rider illegal bit still preempts singleton Vex water");
    snapshot.interaction.humanIllegalMask = 1ULL << 18U;
    snapshot.board.physicalOccupiedMask |= 1ULL << 48U;
    Check(!modules::CanPresentWaterSlosh(snapshot, context),
          "Vex plus any chess piece immediately preempts water");
    snapshot.board.physicalOccupiedMask = 1ULL << 19U;
    Check(!modules::CanPresentWaterSlosh(snapshot, context),
          "an untracked singleton cannot impersonate Vex");
    snapshot.board.physicalOccupiedMask = 0ULL;
    snapshot.interaction.humanIllegalMask = 0ULL;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::Gameplay;
    Check(modules::CanPresentWaterSlosh(snapshot, context),
          "an empty restored board can slosh after reboot despite stale gameplay identity");
    snapshot.game.hintThinking = true;
    Check(modules::CanPresentWaterSlosh(snapshot, context),
          "invisible background hint work cannot veto an empty-board pickup");
    snapshot.render.hintDisplayRequested = true;
    Check(!modules::CanPresentWaterSlosh(snapshot, context),
          "a hint actually presented to the player still preempts water");
    snapshot.game.hintThinking = false;
    snapshot.render.hintDisplayRequested = false;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::FreshSetup;
    context.healthState = runtime::RuntimeHealthState::DegradedUsable;
    Check(!modules::CanPresentWaterSlosh(snapshot, context), "health degradation preempts water");
    context.healthState = runtime::RuntimeHealthState::Healthy;
    context.minimalAnimations = true;
    Check(!modules::CanPresentWaterSlosh(snapshot, context), "Minimal Animations disables cosmetic water");
    context.minimalAnimations = false;
    snapshot.system.otaInProgress = true;
    Check(!modules::CanPresentWaterSlosh(snapshot, context), "OTA always owns its presentation");
    snapshot.system.otaInProgress = false;
    snapshot.system.hardwareTestActive = true;
    Check(!modules::CanPresentWaterSlosh(snapshot, context), "manufacturing tests always own their presentation");
    snapshot.system.hardwareTestActive = false;
    context.awake = false;
    Check(!modules::CanPresentWaterSlosh(snapshot, context), "sleep countdown and preparation preempt water");
}

} // namespace

int main() {
    CheckLighterCoverage();
    CheckSoftEntry();
    CheckVerticalPressure();
    CheckShakeFoam();
    CheckAmbientCaustic();
    CheckPhysicalResponse();
    CheckBoundsAndDeterminism();
    CheckSessionAndMapping();
    CheckAdmission();
    std::printf("[WATER_SLOSH] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
