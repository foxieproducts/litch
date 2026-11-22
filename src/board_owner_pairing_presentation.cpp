#include "modules/board_owner_pairing_runtime.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/animation_timing.hpp"
#include "modules/menu_level_overlay.hpp"

namespace modules {
namespace {
constexpr uint32_t CLEAR_CHASE_STEP_MS = 16U;
constexpr uint32_t CLEAR_BLINK_HALF_MS = 160U;
constexpr uint32_t CLEAR_PROMPT_PULSE_MS = 1200U;
constexpr uint32_t CUE_FADE_MS = BoardOwnerPairingRuntime::PRESENTATION_FADE_MS;
constexpr Rgb PAIRING_BLUE{0U, 96U, 255U};
constexpr Rgb PAIRING_ORANGE{255U, 96U, 0U};
constexpr Rgb PAIRING_PURPLE{128U, 0U, 255U};
constexpr animation::ValueToken SECONDARY_CROSSFADE{
    {640U, animation::PlaybackMode::Loop, 0U},
    {animation::Waveform::Breathe, 0U, 255U, 128U, 28U}};
constexpr animation::ValueToken SUCCESS_PULSE{
    {CLEAR_BLINK_HALF_MS * 2U, animation::PlaybackMode::Loop, 0U},
    {animation::Waveform::Breathe, 0U, 255U, 128U, 28U}};

bool IsPerimeterCue(const BoardOwnerPairingRuntime::PhysicalCue cue) {
    using Cue = BoardOwnerPairingRuntime::PhysicalCue;
    return cue == Cue::Clearing || cue == Cue::Cleared;
}
uint8_t FadeAlpha(const uint32_t elapsedMs) {
    if (elapsedMs >= CUE_FADE_MS) return 255U;
    const uint32_t phase = elapsedMs * 256U / CUE_FADE_MS;
    const uint32_t eased = phase * phase * (768U - 2U * phase) / 65536U;
    return static_cast<uint8_t>((eased * 255U + 128U) / 256U);
}
Rgb RingColor(const BoardOwnerPairingRuntime::PhysicalCue cue,
              const uint32_t elapsedMs, const uint8_t pixel) {
    using Cue = BoardOwnerPairingRuntime::PhysicalCue;
    const auto secondary = AlphaBlendRgb(PAIRING_ORANGE, PAIRING_PURPLE,
        animation::SampleValue({elapsedMs}, SECONDARY_CROSSFADE));
    if (cue == Cue::Cleared) {
        return ScaleRgbSaturatingBy256(
            pixel < EDGE_LED_COUNT / 2U ? PAIRING_BLUE : secondary,
            animation::SampleValue({elapsedMs}, SUCCESS_PULSE));
    }
    const uint32_t heldMs = elapsedMs < BoardOwnerPairingRuntime::CLEAR_HOLD_MS
        ? elapsedMs : BoardOwnerPairingRuntime::CLEAR_HOLD_MS;
    const uint32_t holdPosition256 = heldMs * EDGE_LED_COUNT * 256U /
        BoardOwnerPairingRuntime::CLEAR_HOLD_MS;
    const uint8_t head = static_cast<uint8_t>((cue == Cue::Clear
        ? holdPosition256 / 256U : elapsedMs / CLEAR_CHASE_STEP_MS) % EDGE_LED_COUNT);
    const auto tail = [&](const uint8_t at, const bool includeSecondary) {
        const uint8_t distance = static_cast<uint8_t>((at + EDGE_LED_COUNT - pixel) % EDGE_LED_COUNT);
        const uint8_t opposite = static_cast<uint8_t>((distance + EDGE_LED_COUNT / 2U) % EDGE_LED_COUNT);
        if (distance < 4U) return ScaleRgbSaturatingBy256(PAIRING_BLUE, 256U - distance * 48U);
        if (includeSecondary && opposite < 4U)
            return ScaleRgbSaturatingBy256(secondary, 256U - opposite * 48U);
        return RGB_BLACK;
    };
    const bool includeSecondary = cue != Cue::Clear;
    const auto color = AlphaBlendRgb(tail(head, includeSecondary),
        tail(static_cast<uint8_t>((head + 1U) % EDGE_LED_COUNT), includeSecondary),
        static_cast<uint8_t>(cue == Cue::Clear ? holdPosition256 % 256U :
            (elapsedMs % CLEAR_CHASE_STEP_MS) * 255U / CLEAR_CHASE_STEP_MS));
    if (cue == Cue::Clearing && elapsedMs < CUE_FADE_MS)
        return AlphaBlendRgb(tail(0U, false), color, FadeAlpha(elapsedMs));
    return color;
}
} // namespace

BoardOwnerPairingRuntime::WireStatus BoardOwnerPairingRuntime::Status(
    Connection connection) const {
    WireStatus status{};
    status.attempt = m_attempt;
    status.physicalEnd = PairPrompt() ? m_end : (m_end <= 1U ? m_end : 0U);
    status.reason = m_custody.GetReason();
    if (m_identityDurable) status.flags |= owner_pairing_session_wire::IdentityDurable;
    if (m_record == BoardOwnerRecord::Active || m_record == BoardOwnerRecord::Clearing ||
        m_record == BoardOwnerRecord::Adding)
        status.flags |= owner_pairing_session_wire::OwnerRetained;
    if (OwnerReady(connection)) status.flags |= owner_pairing_session_wire::ConnectionOwnerReady;
    const auto phase = m_custody.GetPhase();
    const auto operation = m_custody.GetOperation();
    if (!m_identityDurable || m_record == BoardOwnerRecord::Invalid) status.state = State::Unavailable;
    else if (m_settingsReset == SettingsReset::Failed) {
        status.state = State::Failed;
        status.reason = Reason::StorageFailed;
    } else if (m_settingsReset == SettingsReset::Pending) status.state = State::Clearing;
    else if (phase == Controller::Phase::Failed) status.state = State::Failed;
    else if (m_needsDisconnect) status.state = State::Clearing;
    else if (PairPrompt()) status.state = State::AwaitPhysical;
    else if (ClearPrompt() && (!m_localClearAlreadyAbsent || AnyButtonDown()))
        status.state = State::ClearPrompt;
    else if (operation == Controller::Operation::Pair &&
             (phase == Controller::Phase::AwaitSecurity ||
              (phase == Controller::Phase::SavingAddJournal && m_claimValid)))
        status.state = State::Securing;
    else if (operation == Controller::Operation::Pair && phase == Controller::Phase::SavingOwner)
        status.state = State::PersistingOwner;
    else if (operation == Controller::Operation::Unpair ||
             operation == Controller::Operation::OrphanCleanup ||
             (operation == Controller::Operation::Pair && phase != Controller::Phase::Idle))
        status.state = State::Clearing;
    else if (OwnerReady(connection)) status.state = State::OwnerReady;
    else if (m_custody.GetOwnership() == Controller::Ownership::Owned) status.state = State::OwnedOther;
    else if (m_custody.Pairable()) status.state = State::Unpaired;
    else status.state = State::Failed;
    if (m_settingsReset == SettingsReset::None && status.state == State::OwnedOther &&
        m_record == BoardOwnerRecord::Active &&
        m_identityDurable && m_custody.GetOperation() == Controller::Operation::None &&
        m_custody.SavedPhoneCount() >= 1U && m_custody.SavedPhoneCount() < MAX_SAVED_PHONES)
        status.flags |= owner_pairing_session_wire::CanAddPhone;
    return status;
}

BoardOwnerPairingRuntime::PhysicalPresentation BoardOwnerPairingRuntime::Presentation() const {
    PhysicalPresentation out{};
    out.state = Status().state;
    out.physicalEnd = m_end;
    // Optional pairing/storage failure is reported to the app. Local play
    // retains the board text and controls, with no substitute LED warning.
    if (m_settingsReset == SettingsReset::Failed ||
        m_custody.GetPhase() == Controller::Phase::Failed) return out;
    if (PairPrompt()) {
        out.active = true;
        out.cue = PhysicalCue::Claim;
        out.text = "X+O";
        out.nextAction = "press-both-blue-buttons";
    } else if (ClearPrompt()) {
        out.active = true;
        out.cue = PhysicalCue::Clear;
        out.text = "X+O";
        out.nextAction = m_holdingClear ? "keep-holding-both" : "hold-both-2-seconds";
        const uint32_t heldMs = m_holdingClear ? fxe::Millis() - m_holdStartedMs : 0U;
        out.progress256 = heldMs >= CLEAR_HOLD_MS ? 256U :
            static_cast<uint16_t>(heldMs * 256U / CLEAR_HOLD_MS);
    } else if (m_clearFeedbackActive) {
        out.active = true;
        out.cue = m_clearFeedbackSucceeded ? PhysicalCue::Cleared : PhysicalCue::Clearing;
        out.nextAction = m_clearFeedbackSucceeded ? "pair-from-app" : "wait";
    } else if (m_consentAcknowledgmentActive && m_custody.GetReason() == Reason::None &&
               fxe::Millis() - m_consentAcknowledgmentStartedMs < CUE_FADE_MS) {
        out.active = true;
        out.cue = PhysicalCue::Confirmed;
        out.nextAction = m_custody.GetOperation() != Controller::Operation::None
            ? "wait" : "ordinary-gameplay";
    } else if (m_settingsReset == SettingsReset::Pending ||
               m_custody.GetOperation() != Controller::Operation::None || m_needsDisconnect) {
        // Native/durable work remains pending, but asks for no more board input.
        out.cue = PhysicalCue::Waiting;
        out.nextAction = "wait";
    }
    return out;
}

void BoardOwnerPairingRuntime::UpdatePhysicalCue() {
    const auto next = Presentation().cue;
    if (next == m_physicalCue) return;
    m_previousPhysicalCue = m_physicalCue;
    m_previousPhysicalCueStartedMs = m_physicalCueStartedMs;
    m_physicalCue = next;
    m_physicalCueStartedMs = fxe::Millis();
}

BoardOwnerPairingRuntime::CueSnapshot BoardOwnerPairingRuntime::CaptureCue() const {
    const auto presentation = Presentation();
    return {presentation.cue, m_previousPhysicalCue,
        presentation.cue == m_physicalCue ? m_physicalCueStartedMs : fxe::Millis(),
        m_previousPhysicalCueStartedMs, m_clearFeedbackStartedMs, m_clearFeedbackSucceededMs,
        m_consentAcknowledgmentStartedMs, m_startedMs, m_holdStartedMs, m_end,
        presentation.active, m_holdingClear, PairPrompt()};
}

void BoardOwnerPairingRuntime::ComposePerimeterCue(
    const CueSnapshot& input,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay) {
    const auto cue = input.cue;
    const bool holding = cue == PhysicalCue::Clear && input.holdingClear;
    if (!holding && !IsPerimeterCue(cue)) return;
    const uint32_t now = fxe::Millis();
    const uint32_t started = input.startedMs;
    const uint32_t age = now - (holding ? input.holdStartedMs :
        cue == PhysicalCue::Cleared ? input.clearSucceededMs : started);
    const uint32_t clearAge = now - input.clearStartedMs;
    const bool followsHold = !holding && input.previous == PhysicalCue::Clear;
    uint8_t alpha = followsHold || cue == PhysicalCue::Clearing ? 255U : FadeAlpha(age);
    if (cue == PhysicalCue::Cleared && input.previous == PhysicalCue::Clearing) {
        alpha = 255U;
    }
    if (cue == PhysicalCue::Cleared && age >= CLEAR_FEEDBACK_MS - CUE_FADE_MS) {
        alpha = static_cast<uint8_t>(alpha * FadeAlpha(
            age < CLEAR_FEEDBACK_MS ? CLEAR_FEEDBACK_MS - age : 0U) / 255U);
    }
    overlay.perimeterAlpha255 = alpha;
    for (uint8_t pixel = 0U; pixel < EDGE_LED_COUNT; ++pixel) {
        auto color = RingColor(cue, cue == PhysicalCue::Clearing ? clearAge : age, pixel);
        if (followsHold && cue == PhysicalCue::Cleared && age < CUE_FADE_MS)
            color = AlphaBlendRgb(RingColor(PhysicalCue::Clear, CLEAR_HOLD_MS, pixel), color, FadeAlpha(age));
        if (cue == PhysicalCue::Cleared && input.previous == PhysicalCue::Clearing && age < CUE_FADE_MS)
            color = AlphaBlendRgb(RingColor(PhysicalCue::Clearing, clearAge, pixel), color, FadeAlpha(age));
        overlay.Set(pixel, color);
    }
}

void BoardOwnerPairingRuntime::ComposePhysicalCue(
    status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
    bool controlBanksSwapped) const {
    ComposePhysicalCue(CaptureCue(), overlay, controlBanksSwapped);
}

void BoardOwnerPairingRuntime::ComposePhysicalCue(const CueSnapshot& captured,
    status_led_frame_adapter::FixedPhysicalStatusOverlay& overlay,
    bool controlBanksSwapped) {
    auto input = captured;
    if (input.cue == PhysicalCue::Cleared &&
        fxe::Millis() - input.clearSucceededMs >= CLEAR_FEEDBACK_MS) return;
    if (input.cue == PhysicalCue::Confirmed &&
        fxe::Millis() - input.consentStartedMs >= CUE_FADE_MS) return;
    ComposePerimeterCue(input, overlay);
    if (!input.active) return;
    if (input.cue == PhysicalCue::Confirmed) {
        const uint32_t elapsed = fxe::Millis() - input.consentStartedMs;
        overlay.controlAccentAlpha255 = FadeAlpha(
            elapsed < CUE_FADE_MS ? CUE_FADE_MS - elapsed : 0U);
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            for (const uint8_t role : {uint8_t{0U}, uint8_t{5U}}) {
                overlay.controlAccentMask |= static_cast<uint16_t>(1U << (bank * 6U + role));
                overlay.controlAccentPixels[bank * 6U + role] = RGB_GREEN;
            }
        }
        return;
    }
    const bool clear = input.cue == PhysicalCue::Clear;
    const uint32_t pulse = (fxe::Millis() - input.promptStartedMs) % CLEAR_PROMPT_PULSE_MS;
    const uint32_t triangle = pulse < CLEAR_PROMPT_PULSE_MS / 2U
        ? pulse : CLEAR_PROMPT_PULSE_MS - pulse;
    const uint16_t scale = static_cast<uint16_t>(128U + (input.holdingClear
        ? (fxe::Millis() - input.holdStartedMs >= CLEAR_HOLD_MS ? 256U :
              (fxe::Millis() - input.holdStartedMs) * 256U / CLEAR_HOLD_MS) * 128U / 256U
        : 128U - triangle * 128U / (CLEAR_PROMPT_PULSE_MS / 2U)));
    const Rgb blue = ScaleRgbSaturatingBy256({0U, 96U, 255U}, clear ? scale : 256U);
    const uint8_t bank = static_cast<uint8_t>((input.end == 1U) != controlBanksSwapped);
    // Mask X/O at both ends so a wrong-end press never inherits a game cue.
    for (uint8_t logicalBank = 0U; logicalBank < 2U; ++logicalBank) {
        for (const uint8_t role : {uint8_t{0U}, uint8_t{5U}}) {
            const uint8_t index = logicalBank * 6U + role;
            overlay.controlMask |= static_cast<uint16_t>(1U << index);
            overlay.controlPixels[index] = RGB_BLACK;
        }
        for (const uint8_t role : {uint8_t{0U}, uint8_t{3U}}) {
            const uint8_t index = logicalBank * 4U + role;
            overlay.emulatedControlMask |= static_cast<uint8_t>(1U << index);
            overlay.emulatedControlPixels[index] = RGB_BLACK;
        }
    }
    Rgb o = blue;
    Rgb x = blue;
    const bool firstHalf = (fxe::Millis() / 400U) % 2U == 0U;
    if (input.cue == PhysicalCue::Restart) {
        o = RGB_BLACK;
        x = firstHalf ? RGB_RED : RGB_BLACK;
    } else if (input.cue == PhysicalCue::Clearing ||
               input.cue == PhysicalCue::Cleared) {
        o = x = RGB_BLACK;
        // V3 uses perimeter pixels for controls. This whole-ring episode owns
        // those pixels too; dedicated V3.3 controls remain masked below.
        overlay.emulatedControlMask = 0U;
    }
    for (uint8_t target = 0U; target < 2U; ++target) {
        if (target != bank && !(input.pairPrompt && input.end == owner_pairing_session_wire::EITHER_END)) continue;
        overlay.controlPixels[target * 6U] = o;
        overlay.controlPixels[target * 6U + 5U] = x;
        overlay.emulatedControlPixels[target * 4U] = o;
        overlay.emulatedControlPixels[target * 4U + 3U] = x;
    }
}

bool BoardOwnerPairingRuntime::SyncPhysicalMessage(
    MenuLevelOverlay& overlay, const std::array<Rgb, 64>& background,
    bool controlBanksSwapped, bool allowed) {
    return SyncPhysicalMessage(CaptureCue(), m_restartMessageVisible, overlay,
        background, controlBanksSwapped, allowed);
}

bool BoardOwnerPairingRuntime::SyncPhysicalMessage(const CueSnapshot& input,
    bool& wasVisible, MenuLevelOverlay& overlay, const std::array<Rgb, 64>& background,
    bool controlBanksSwapped, bool allowed) {
    if (allowed && input.cue == PhysicalCue::Restart) {
        const bool p2 = (input.end == 1U) != controlBanksSwapped;
        // The readable glyph is four columns wide. Overlapping two-letter
        // windows expose the whole restart abbreviation on the eight-column
        // board instead of silently clipping its third character.
        const char* text = (fxe::Millis() / 800U) % 2U == 0U ? "RS" : "ST";
        if (!wasVisible || !overlay.IsActive() || overlay.Text() != text) {
            overlay.SetOwner(p2 ? UiOwner::P2 : UiOwner::P1);
            overlay.ShowMessage(text, RGB_RED, background);
        }
        wasVisible = true;
        return true;
    }
    if (wasVisible && (overlay.Text() == "RS" || overlay.Text() == "ST")) overlay.Stop();
    wasVisible = false;
    return false;
}

} // namespace modules
