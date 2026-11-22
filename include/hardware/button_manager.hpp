#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/button.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/user_input.hpp"

namespace hardware {

template <size_t BUTTON_COUNT, size_t QUEUE_CAPACITY = 16>
class ButtonManager final : public modules::IUserInput {
    static_assert(QUEUE_CAPACITY > 0U, "Button event capacity must be positive");
  public:
    void Begin() {
        m_count = 0U;
        m_overflow = false;
        m_initialized = true;
        m_applyingTimer = false;
        m_rawOrder = 0U;
        m_lastPollMs = m_lastAdvanceMs = fxe::Millis();
    }

    void ConfigureButton(const size_t index, const modules::UserButton button,
                         const modules::UiOwner owner, const fxe::Button::Config& cfg,
                         const fxe::Button::DetectionFn& detectionFn, const bool seedRawDown) {
        if (index >= BUTTON_COUNT) return;
        auto& state = m_buttons[index];
        state.button = button;
        state.owner = owner;
        state.pressOrder = state.releaseOrder = 0U;
        auto config = cfg;
        config.onEvent = {};
        config.onTimedEvent = [this, index](const fxe::Button::Event event, const uint32_t atMs) {
            Enqueue(m_buttons[index], event, atMs);
        };
        state.logic.SetConfig(config);
        state.logic.SetDetectionFn(detectionFn);
        state.logic.Reset();
        if (seedRawDown) state.logic.SeedRawDown(detectionFn ? detectionFn() : false);
    }

    void Update() {
        for (auto& button : m_buttons) button.logic.Update();
    }

    // Input and timer deadlines share one timeline, including boundary ties.
    // Overload invalidates the whole stream: the owner reseeds held controls
    // and resets gesture consumers rather than losing a semantic Release.
    void FeedEdge(const size_t index, const bool rawDown, const uint32_t atMs) {
        if (!m_initialized || m_overflow || index >= BUTTON_COUNT) return;
        AdvanceTo(atMs);
        if (m_overflow) return;
        auto& button = m_buttons[index];
        if (++m_rawOrder == 0U) ++m_rawOrder;
        uint32_t pendingAt = 0U;
        if (rawDown && !button.logic.IsPressed()) button.pressOrder = m_rawOrder;
        if (!rawDown && button.logic.IsPressed() && !button.logic.PendingReleaseAt(pendingAt))
            button.releaseOrder = m_rawOrder;
        button.logic.FeedEdge(rawDown, atMs);
        AdvanceTo(atMs);
    }

    void AdvanceTo(const uint32_t atMs) {
        if (!m_initialized || m_overflow) return;
        if (Before(atMs, m_lastAdvanceMs)) { Overflow(); return; }
        constexpr size_t MAX_CATCHUP_STEPS = 256U;
        for (size_t step = 0U;; ++step) {
            size_t selected = BUTTON_COUNT;
            uint32_t earliest = 0U;
            for (size_t index = 0U; index < BUTTON_COUNT; ++index) {
                uint32_t deadline = 0U;
                if (!m_buttons[index].logic.NextDeadline(deadline) || Before(atMs, deadline)) continue;
                if (selected == BUTTON_COUNT || Before(deadline, earliest)) {
                    selected = index;
                    earliest = deadline;
                }
            }
            if (selected == BUTTON_COUNT) { m_lastAdvanceMs = atMs; return; }
            if (step == MAX_CATCHUP_STEPS) { Overflow(); return; }
            m_applyingTimer = true;
            m_buttons[selected].logic.AdvanceOneAt(earliest);
            m_applyingTimer = false;
            if (m_overflow) return;
        }
    }

    bool HasPendingRelease() const {
        uint32_t ignored = 0U;
        for (const auto& button : m_buttons)
            if (button.logic.PendingReleaseAt(ignored)) return true;
        return false;
    }

    uint32_t ButtonTimelineThroughMs() const {
        uint32_t through = m_lastAdvanceMs;
        for (const auto& button : m_buttons) {
            uint32_t releaseAt = 0U;
            if (button.logic.PendingReleaseAt(releaseAt) && Before(releaseAt, through)) through = releaseAt;
        }
        return through;
    }

    bool ConsumeOverflow() {
        const bool overflow = m_overflow;
        m_overflow = false;
        return overflow;
    }

    bool TryReadQueuedEvent(modules::ButtonEvent& outEvent) {
        if (!m_initialized || m_overflow || m_count == 0U) return false;
        // An unresolved UP may still produce an earlier Release. Keep every
        // button's newer effects behind that global chronology barrier.
        for (const auto& button : m_buttons) {
            uint32_t pendingAt = 0U;
            if (button.logic.PendingReleaseAt(pendingAt) &&
                !Earlier(m_queue[0U].tMs, m_queue[0U].order, pendingAt, button.releaseOrder)) return false;
        }
        const QueuedEvent& first = m_queue[0U];
        outEvent = {first.button, first.event, first.owner, first.tMs, true};
        for (size_t index = 1U; index < m_count; ++index) m_queue[index - 1U] = m_queue[index];
        --m_count;
        return true;
    }

    bool TryReadButtonEvent(modules::ButtonEvent& outEvent) override {
        if (!m_initialized) return false;
        const uint32_t nowMs = fxe::Millis();
        if (nowMs != m_lastPollMs && m_count == 0U) {
            Update();
            m_lastPollMs = m_lastAdvanceMs = nowMs;
        }
        return TryReadQueuedEvent(outEvent);
    }

  private:
    struct ButtonState {
        modules::UserButton button = modules::UserButton::None;
        modules::UiOwner owner = modules::UiOwner::None;
        fxe::Button logic;
        uint32_t pressOrder = 0U;
        uint32_t releaseOrder = 0U;
    };

    struct QueuedEvent {
        // Every queued event has a timestamp. Set timestampValid when delivering it.
        uint32_t tMs;
        uint32_t order;
        modules::UserButton button;
        fxe::Button::Event event;
        modules::UiOwner owner;
    };

    static bool Before(const uint32_t lhs, const uint32_t rhs) {
        return static_cast<int32_t>(lhs - rhs) < 0;
    }

    static bool Earlier(const uint32_t lhsMs, const uint32_t lhsOrder,
                        const uint32_t rhsMs, const uint32_t rhsOrder) {
        if (lhsMs != rhsMs) return Before(lhsMs, rhsMs);
        if (lhsOrder == rhsOrder) return false;
        // Deadlines are processed before the next captured edge at that time.
        return lhsOrder == 0U || (rhsOrder != 0U && Before(lhsOrder, rhsOrder));
    }

    void Overflow() { m_count = 0U; m_overflow = true; m_initialized = false; }

    void Enqueue(const ButtonState& button, const fxe::Button::Event event, const uint32_t atMs) {
        if (m_overflow) return;
        if (m_count == QUEUE_CAPACITY) { Overflow(); return; }
        const uint32_t order = !button.logic.GetConfig().immediatePress ||
            (m_applyingTimer && event != fxe::Button::Event::Release) ? 0U :
            event == fxe::Button::Event::Press ? button.pressOrder :
            event == fxe::Button::Event::Release ? button.releaseOrder : 0U;
        size_t slot = m_count;
        while (slot > 0U && Earlier(atMs, order, m_queue[slot - 1U].tMs, m_queue[slot - 1U].order)) {
            m_queue[slot] = m_queue[slot - 1U];
            --slot;
        }
        m_queue[slot] = QueuedEvent{atMs, order, button.button, event, button.owner};
        ++m_count;
    }

    bool m_initialized = false;
    bool m_overflow = false;
    bool m_applyingTimer = false;
    std::array<ButtonState, BUTTON_COUNT> m_buttons{};
    std::array<QueuedEvent, QUEUE_CAPACITY> m_queue{};
    size_t m_count = 0U;
    uint32_t m_lastPollMs = 0U;
    uint32_t m_lastAdvanceMs = 0U;
    uint32_t m_rawOrder = 0U;
};

} // namespace hardware
