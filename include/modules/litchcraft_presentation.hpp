#pragma once

#include <array>
#include <cstdint>

#include "modules/board_led_frame.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_presentation {

enum class Source : uint8_t { Static, Legacy, Stream, WaitingForStore };
enum class BootTransition : uint8_t { None, Cancelled, Finished };

struct EntryHold final {
    uint32_t startedAtMs = 0U;
    bool active = false;
};
void ComposeEntryStatus(uint8_t hardware, uint32_t elapsedMs,
    std::array<Rgb, EDGE_LED_COUNT>& status);

struct Context final {
    uint32_t session = 0U;
    uint32_t modeGeneration = 0U;
    uint32_t documentEpoch = 0U;
    uint32_t contentRevision = 0U;
    uint32_t intentEpoch = 0U;
    bool active = false;
    bool transferSuspended = false;
    uint32_t transferEpoch = 0U;
};

struct Playback final {
    uint64_t elapsedMs = 0U;
    uint64_t effectElapsedMs = 0U;
    uint32_t clockMs = 0U;
    uint32_t frameStartedAtMs = 0U;
    uint32_t pausedAtMs = 0U;
    uint32_t corruptStartedAtMs = 0U;
    uint16_t selectedFrame = 0U;
    uint8_t bootFlags = 0U;
    uint8_t bootResumeFrame = 0U;
    bool playing = false;
    bool frameStarted = false;
    bool corruptDwell = false;
    bool awaitingInitialFrame = false;
    bool awaitingPlaybackFrame = false;
    bool awaitingRecoveryFrame = false;
    bool suspended = false;
};

void AdvanceClock(Playback& playback, uint32_t nowMs, bool suspended);

bool SamePlayback(const Playback& first, const Playback& second);
bool SameContext(const Context& first, const Context& second);
uint8_t ColorPickBlendAlpha(uint32_t startedAtMs, uint32_t nowMs);
uint16_t PlaybackStatusScale256(bool playing, uint16_t from256, uint32_t elapsedMs);

// Main-owned conversion resolves legacy/static data and every overlay value.
// A task's private copy reuses frame as Store-copy workspace; the mailbox is
// immutable and the renderer retains no additional raw frame/document.
struct Visuals {
    uint64_t blockedOccupancy = 0U;
    std::array<Rgb, 8U> quickPalette{};
    std::array<Rgb, 4U> pickBefore{};
    std::array<uint8_t, 8U> quickPaletteSquares{};
    uint32_t sparkleSeed = 1U;
    uint32_t playbackStatusChangedAtMs = 0U;
    uint32_t quickPaletteTouchedMs = 0U;
    uint32_t quickPaletteFadeAtMs = 0U;
    uint32_t pickStartedMs = 0U;
    uint32_t eraseSelectedMs = 0U;
    uint32_t shakeEraseAtMs = 0U;
    uint32_t entryHoldStartedMs = 0U;
    uint16_t playbackStatusFrom256 = 256U;
    Rgb color{};
    Source source = Source::Static;
    uint8_t hardware = 3U;
    uint8_t pickSquare = 0U;
    uint8_t quickPaletteIndex = 0U;
    bool active = false;
    bool erasing = false;
    bool eraseCueStarted = false;
    bool quickPaletteVisible = false;
    bool quickPaletteFading = false;
    bool pickPending = false;
    bool shakeEraseActive = false;
    bool exitHoldActive = false;
};
bool SameVisuals(const Visuals& first, const Visuals& second);

struct Inputs final : Visuals {
    litchcraft_frame_store::Frame frame{};
    Playback playback{};
    Context context{};
    uint32_t capturedAtMs = 0U;
    // Semantic visuals/content freshness; cursor reseeding uses context.intentEpoch.
    uint32_t presentationEpoch = 0U;
};
static_assert(sizeof(Inputs) <= 1536U, "one bounded Litchcraft scene payload");

// Stable platform adapter, never included in a published Inputs value. The
// wake callback must use these values rather than re-read a live session.
struct StreamPort final {
    litchcraft_frame_stream::FrameStream* stream = nullptr;
    void (*wake)(void*, bool, uint32_t, bool, bool) = nullptr;
    void* context = nullptr;
    bool confirmedOutput = false;
};

struct Feedback final {
    Playback playback{};
    Context context{};
    litchcraft_frame_stream::FrameStream::PresentationToken token{};
    uint32_t sequence = 0U;
    BootTransition bootTransition = BootTransition::None;
    bool ready = false;
    bool intentApplied = true;
};

// This is the existing bounded Store playback algorithm, shared by the native
// session wrapper and independent renderer. It never reads filesystem bytes.
BootTransition AdvanceStreamed(Playback& playback, const Context& context,
    const StreamPort& port, uint32_t nowMs,
    litchcraft_frame_store::Frame* workspace = nullptr,
    litchcraft_frame_stream::FrameStream::PresentationToken* token = nullptr);

class Renderer final {
  public:
    Feedback Compose(Inputs& working, const StreamPort& port,
        BoardLedFrame& board, std::array<Rgb, EDGE_LED_COUNT>& status);
    // Call only after both actual output chains confirm this candidate.
    bool NotePresented(Feedback& feedback, const StreamPort& port);
    void Cancel(const Feedback& feedback, const StreamPort& port);
    void Reset(const StreamPort& port);

  private:
    Playback m_playback{};
    Context m_context{};
    uint32_t m_sequence = 0U;
    bool m_initialized = false;
    Source m_source = Source::Static;
    BootTransition m_bootTransition = BootTransition::None;
    bool m_adoptInput = true;
};

} // namespace modules::litchcraft_presentation
