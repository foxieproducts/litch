#pragma once

#include <cstdint>

namespace fxe {

enum class ErrorCode : uint8_t {
    None = 0,
    Unknown = 1,
    Invalid = 2,
    Busy = 3,
    NotFound = 4,
    IoError = 5,
    Overflow = 6,
};

struct Status final {
    ErrorCode code = ErrorCode::None;

    static Status Ok() { return Status{}; }
    static Status Error(const ErrorCode error) { return Status{error}; }

    bool IsOk() const { return code == ErrorCode::None; }
};

template <typename T>
class Result final {
  public:
    static Result Ok(const T& value) {
        return Result(value, Status::Ok());
    }

    static Result Error(const ErrorCode error) {
        return Result(T{}, Status::Error(error));
    }

    bool IsOk() const { return m_status.IsOk(); }
    Status GetStatus() const { return m_status; }
    ErrorCode GetError() const { return m_status.code; }

    const T& Value() const { return m_value; }
    T& Value() { return m_value; }

  private:
    Result(const T& value, const Status status)
        : m_value(value), m_status(status) {}

    T m_value{};
    Status m_status{};
};

} // namespace fxe
