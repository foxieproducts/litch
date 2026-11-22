#pragma once
namespace logger {
template <typename... Args> void Trace(const char*, Args...) {}
template <typename... Args> void Info(const char*, Args...) {}
template <typename... Args> void Error(const char*, Args...) {}
template <typename... Args> void Warn(const char*, Args...) {}
}
