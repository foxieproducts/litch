#include "modules/boot_display.hpp"

#include <algorithm>
#include <string_view>
#include "foxie_embedded/millis.hpp"
#include "modules/boot_fox_asset.hpp"
#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/litchcraft_session.hpp"

namespace modules {
bool BootArtworkUnavailableAfterRecovery(const litchcraft_frame_store::Status status) {
    // Recovery Ok precedes worker-owned stream publication. Only an actual
    // absent/failed recovery proves that the cosmetic episode must be skipped.
    return status != litchcraft_frame_store::Status::NotConfigured &&
           status != litchcraft_frame_store::Status::Ok;
}
BootDisplayChoice ReadBootDisplayChoice(const SettingsStore& settings) {
    const char* raw = settings.ReadValue("BOOT_DISPLAY", "fox");
    const std::string_view value(raw != nullptr ? raw : "fox");
    if (value == "off")
        return BootDisplayChoice::Off;
    if (value == "artwork")
        return BootDisplayChoice::StoredArtwork;
    return BootDisplayChoice::Fox;
}

void BootDisplay::Begin(const BootDisplayChoice choice, const bool eligible, const Context context) {
    m_context = context;
    m_consumedXMask = 0U;
    m_hasPresented = false;
    m_initialOccupancy = 0ULL;
    m_choice = choice;
    m_state = eligible && choice != BootDisplayChoice::Off ? State::Waiting : State::Retired;
}
void BootDisplay::Cancel() {
    if (IsActive() || IsPending())
        m_state = State::Cancelled;
}
void BootDisplay::Presented() {
    if (WasCancelled())
        m_state = State::Retired;
    if (IsActive() && !m_hasPresented) {
        m_startedMs = fxe::Millis();
        m_hasPresented = true;
    }
}
void BootDisplay::ObserveBoard(const bool valid, const uint64_t occupied, const bool playMode) {
    if (!valid)
        return;
    if (m_state == State::Waiting) {
        if (playMode && m_context.resumableGame && occupied == m_context.resumeOccupancy) {
            m_state = State::Retired;
            return;
        }
        m_initialOccupancy = occupied;
        m_startedMs = fxe::Millis();
        m_state = m_choice == BootDisplayChoice::StoredArtwork ? State::WaitingArtwork : State::Fox;
    } else if ((IsActive() || IsPending()) && occupied != m_initialOccupancy) {
        Cancel();
    }
}
void BootDisplay::UpdateArtwork(LitchcraftSession& artwork, const bool artworkUnavailable) {
    const bool started = m_state == State::WaitingArtwork && artwork.BeginBootArtworkOnce();
    const bool wasArtwork = UsesArtwork() || started;
    if (wasArtwork) artwork.UpdateBootArtwork();
    else artwork.CancelBootArtwork();
    ObserveArtwork(started, artwork.BootArtworkFinished(),
        artwork.IsBootArtworkPlaying(), artworkUnavailable);
}
void BootDisplay::ObserveArtwork(const bool started, const bool finished,
                                  const bool playing, const bool unavailable) {
    if (m_state == State::WaitingArtwork) {
        if (started) m_state = State::Artwork;
        else if (unavailable) Cancel();
    }
    if (UsesArtwork()) {
        if (finished) {
            m_state = State::ArtworkCrossfade;
            m_hasPresented = false;
        } else if (!playing) Cancel();
    } else if (IsArtworkCrossfading() && m_hasPresented &&
               fxe::Millis() - m_startedMs >= CROSSFADE_MS) {
        m_state = State::Retired;
    }
}
void BootDisplay::UpdatePriority(const bool urgent,
                                 const Context context) {
    if (!IsActive() && !IsPending())
        return;
    // Explicit user mutation/controls cancel at ingress. Derived game saves
    // share SettingsStore's revision and must not retire an unchanged boot.
    if (urgent || context.modeGeneration != m_context.modeGeneration ||
        context.artworkGeneration != m_context.artworkGeneration)
        Cancel();
}
bool BootDisplay::ConsumeButton(const ButtonEvent& event) {
    const bool x = IsXButton(event.button);
    const uint8_t bit = event.button == UserButton::P2X ? 2U : 1U;
    if (x && (m_consumedXMask & bit) != 0U) {
        if (event.event == fxe::Button::Event::Release)
            m_consumedXMask &= ~bit;
        return true;
    }
    if (event.event != fxe::Button::Event::Press)
        return false;
    const bool skip = (IsActive() || m_state == State::WaitingArtwork) && x;
    if (skip)
        m_consumedXMask |= bit;
    Cancel();
    return skip;
}
void BootDisplay::SuppressOrdinaryStatus(hardware::PhysicalLedFrame& frame) const {
    // Only V3.3 admits this episode. Its twelve dedicated control emitters
    // are separate from the thirty-two ordinary semantic perimeter targets.
    for (uint8_t edge = 0U; edge < EDGE_LED_COUNT; ++edge)
        frame[256U + hardware::V3_3PhysicalIndexForSemanticStatus(edge)] = {};
}
uint16_t BootDisplay::Apply(hardware::PhysicalLedFrame& liveFrame,
                        const bool boardRotate,
                        const bool minimalAnimations,
                        const BoardLedFrame* artwork,
                        const bool ordinaryStatus) {
    if (ordinaryStatus && (IsPending() || UsesArtwork() || IsArtworkCrossfading()))
        SuppressOrdinaryStatus(liveFrame);
    if (m_choice == BootDisplayChoice::StoredArtwork && IsPending()) {
        // Admission and artwork startup run on separate owner captures. Keep
        // the live board hidden until the selected artwork can own its frame.
        std::fill_n(liveFrame.begin(), 256U, Rgb{});
        return 256U;
    }
    if (UsesArtwork()) {
        if (artwork == nullptr)
            return 256U;
        for (uint8_t square = 0U; square < 64U; ++square) {
            const auto& source = (*artwork)[square];
            const Rgb colors[4] = {source.left, source.top, source.right, source.bottom};
            const uint8_t mapped = runtime::MapSquareIndex(square, boardRotate);
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                liveFrame[mapped * 4U + (boardRotate ? (slot + 2U) % 4U : slot)] =
                    ScaleRgbSaturatingBy256(colors[slot], litchcraft_physical_presentation::CANVAS_SCALE256);
            }
        }
        return 256U;
    }
    if (!IsActive() || IsArtworkCrossfading())
        return 256U;
    if (!m_hasPresented)
        m_startedMs = fxe::Millis();
    const uint32_t sinceBlack = fxe::Millis() - m_startedMs;
    const uint32_t elapsed = sinceBlack > BLACK_LEAD_IN_MS
        ? sinceBlack - BLACK_LEAD_IN_MS : 0U;
    if (elapsed >= FOX_MS + FOX_CROSSFADE_MS) {
        Cancel();
        return 256U;
    }
    if (ordinaryStatus) SuppressOrdinaryStatus(liveFrame);
    const uint16_t fade = static_cast<uint16_t>(
        std::min<uint32_t>(256U, elapsed * 256U / FADE_IN_MS));
    const uint16_t liveMix = elapsed <= FOX_MS ? 0U : static_cast<uint16_t>((elapsed - FOX_MS) * 256U / FOX_CROSSFADE_MS);
    for (uint8_t square = 0U; square < 64U; ++square) {
        litchcraft_animation::Square source{};
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const uint16_t index = square * 4U + slot;
            const bool sparkle = !minimalAnimations && boot_fox_asset::IsYellow(index);
            source[slot] = {boot_fox_asset::PIXELS[index],
                            sparkle ? litchcraft_animation::Effect::Sparkle
                                    : litchcraft_animation::Effect::Off,
                            sparkle ? uint16_t{500U} : uint16_t{0U}};
        }
        const auto colors = litchcraft_animation::ComposeSquare(
            source, std::min<uint32_t>(elapsed, FOX_MS - 1U), 1U, 0U, square * 4U);
        const uint8_t mappedSquare = runtime::MapSquareIndex(square, boardRotate);
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            const uint16_t index = mappedSquare * 4U +
                                   (boardRotate ? (slot + 2U) % 4U : slot);
            const Rgb logo = colors[slot];
            const auto blend = [liveMix](uint8_t from, uint8_t to) {
                return static_cast<uint8_t>((from * (256U - liveMix) + to * liveMix + 128U) >> 8U);
            };
            const Rgb live = liveFrame[index];
            liveFrame[index] = {blend(logo.r, live.r), blend(logo.g, live.g), blend(logo.b, live.b)};
        }
    }
    return fade;
}
} // namespace modules
