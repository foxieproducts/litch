#pragma once

#include <cstdint>
#include "hardware/board_led_output_mapping.hpp"
#include "modules/settings_store.hpp"
#include "modules/user_input.hpp"

namespace modules {
namespace litchcraft_frame_store { enum class Status : uint8_t; }
bool BootArtworkUnavailableAfterRecovery(litchcraft_frame_store::Status status);
class LitchcraftSession;
enum class BootDisplayChoice : uint8_t { Fox,
                                         StoredArtwork,
                                         Off };
BootDisplayChoice ReadBootDisplayChoice(const SettingsStore& settings);

// One normal-boot presentation, admitted only by a genuine Hall observation.
// Owns no gameplay, artwork, framebuffer, storage or transport state.
class BootDisplay final {
  public:
    static constexpr uint32_t BLACK_LEAD_IN_MS = 100U;
    static constexpr uint32_t FADE_IN_MS = 250U;
    static constexpr uint32_t FOX_MS = 1000U;
    static constexpr uint32_t FOX_CROSSFADE_MS = 500U;
    static constexpr uint32_t CROSSFADE_MS = 250U;
    struct Context {
        uint32_t modeGeneration = 0U;
        uint32_t artworkGeneration = 0U;
        uint64_t resumeOccupancy = 0ULL;
        bool resumableGame = false;
    };
    void Begin(BootDisplayChoice choice, bool eligible, Context context);
    // Compare a fresh logical observation with the immutable restored game.
    void ObserveBoard(bool valid, uint64_t occupied, bool playMode = true);
    void UpdatePriority(bool urgent, Context context);
    void Cancel();
    // Anchor the visible episode only after a completed black transfer.
    void Presented();
    // Unavailable means terminal absent/failed recovery; Ok may still publish.
    void UpdateArtwork(LitchcraftSession& artwork, bool artworkUnavailable);
    // Same state owner for immutable artwork feedback from the animation task.
    void ObserveArtwork(bool started, bool finished, bool playing, bool unavailable);
    void SynchronizePresentation(const BootDisplay& shown) {
        const uint8_t consumed = m_consumedXMask;
        *this = shown;
        m_consumedXMask = consumed;
    }
    bool IsPending() const { return m_state == State::Waiting || m_state == State::WaitingArtwork; }
    bool NeedsArtwork() const { return m_state == State::WaitingArtwork; }
    bool IsArtworkCrossfading() const { return m_state == State::ArtworkCrossfade; }
    bool NeedsCrossfadeStart() const { return IsArtworkCrossfading() && !m_hasPresented; }
    bool WasCancelled() const { return m_state == State::Cancelled; }
    bool UsesArtwork() const { return m_state == State::Artwork; }
    bool IsActive() const { return m_state == State::Fox || UsesArtwork() || IsArtworkCrossfading(); }
    // Unknown Hall/artwork authority is still a pending startup decision.
    bool IsComplete() const { return m_state != State::Uninitialized && !IsPending() && !IsActive(); }
    // Consume a cancel X gesture through its release so it cannot also Undo.
    bool ConsumeButton(const ButtonEvent& event);
    // Compose the full-demand image; return board-only opacity to apply with
    // the existing LED accent after budgeting, so the limiter cannot undo fades.
    uint16_t Apply(hardware::PhysicalLedFrame& liveFrame, bool boardRotate, bool minimalAnimations,
        const BoardLedFrame* artwork = nullptr, bool ordinaryStatus = false);

  private:
    void SuppressOrdinaryStatus(hardware::PhysicalLedFrame& frame) const;
    enum class State : uint8_t { Uninitialized,
                                 Retired,
                                 Waiting,
                                 Fox,
                                 WaitingArtwork,
                                 Artwork,
                                 ArtworkCrossfade,
                                 Cancelled };
    Context m_context{};
    uint64_t m_initialOccupancy = 0ULL;
    uint32_t m_startedMs = 0U;
    State m_state = State::Uninitialized;
    uint8_t m_consumedXMask = 0U;
    bool m_hasPresented = false;
    BootDisplayChoice m_choice = BootDisplayChoice::Fox;
};
static_assert(sizeof(BootDisplay) <= 40U, "boot presentation stays bounded");
} // namespace modules
