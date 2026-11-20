#pragma once

#include <cstdint>
#include <string_view>

#include "modules/user_input.hpp"

namespace trace {

enum class FrameHashKindToken : uint8_t {
    Any = 0,
    Board = 1,
    Edge = 2,
};

inline FrameHashKindToken ParseFrameHashKindToken(const std::string_view kind) {
    if (kind == "board") {
        return FrameHashKindToken::Board;
    }
    if (kind == "edge") {
        return FrameHashKindToken::Edge;
    }
    return FrameHashKindToken::Any;
}

inline bool TryParseTraceButtonId(const std::string_view id, modules::UserButton& outButton) {
    if (id == "Up") {
        outButton = modules::UserButton::Up;
        return true;
    }
    if (id == "Down") {
        outButton = modules::UserButton::Down;
        return true;
    }
    if (id == "Left") {
        outButton = modules::UserButton::Left;
        return true;
    }
    if (id == "Right") {
        outButton = modules::UserButton::Right;
        return true;
    }
    if (id == "P1X" || id == "P1") {
        outButton = modules::UserButton::P1X;
        return true;
    }
    if (id == "P2X" || id == "P2") {
        outButton = modules::UserButton::P2X;
        return true;
    }
    if (id == "P1O") {
        outButton = modules::UserButton::P1O;
        return true;
    }
    if (id == "P2O") {
        outButton = modules::UserButton::P2O;
        return true;
    }
    return false;
}

inline bool TryParseTraceButtonEvent(const std::string_view evt, fxe::Button::Event& outEvent) {
    if (evt == "press" || evt == "pressed" || evt == "Press" || evt == "Pressed") {
        outEvent = fxe::Button::Event::Press;
        return true;
    }
    if (evt == "release" || evt == "released" || evt == "Release" || evt == "Released") {
        outEvent = fxe::Button::Event::Release;
        return true;
    }
    if (evt == "repeat" || evt == "Repeat") {
        outEvent = fxe::Button::Event::Repeat;
        return true;
    }
    if (evt == "long_hold" || evt == "LongHold" || evt == "longhold" || evt == "Long_Hold") {
        outEvent = fxe::Button::Event::LongHold;
        return true;
    }
    return false;
}

} // namespace trace
