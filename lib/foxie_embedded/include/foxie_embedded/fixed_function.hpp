#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace fxe {

// A fixed-capacity, heap-free, type-erased callable.
//
// Intended for embedded usage where `std::function` may allocate.
// - Stores the callable inline (up to STORAGE_BYTES).
// - Copyable and movable.
// - Empty state supported.
//
// Constraints:
// - Callable must be copy-constructible.
// - Callable size must fit within STORAGE_BYTES.
// - Callable alignment must be <= alignof(std::max_align_t).

template <typename Signature, size_t STORAGE_BYTES>
class FixedFunction;

template <typename R, typename... Args, size_t STORAGE_BYTES>
class FixedFunction<R(Args...), STORAGE_BYTES> final {
  public:
    FixedFunction() = default;

    FixedFunction(std::nullptr_t) {}

    FixedFunction(const FixedFunction& other) {
        CopyFrom(other);
    }

    FixedFunction(FixedFunction&& other) noexcept {
        MoveFrom(std::move(other));
    }

    template <typename T>
    FixedFunction(T&& callable) {
        Assign(std::forward<T>(callable));
    }

    ~FixedFunction() {
        Reset();
    }

    FixedFunction& operator=(const FixedFunction& other) {
        if (this == &other) {
            return *this;
        }
        Reset();
        CopyFrom(other);
        return *this;
    }

    FixedFunction& operator=(FixedFunction&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        Reset();
        MoveFrom(std::move(other));
        return *this;
    }

    FixedFunction& operator=(std::nullptr_t) {
        Reset();
        return *this;
    }

    template <typename T>
    FixedFunction& operator=(T&& callable) {
        Assign(std::forward<T>(callable));
        return *this;
    }

    explicit operator bool() const {
        return m_invoke != nullptr;
    }

    void Reset() {
        if (m_destroy) {
            m_destroy(StoragePtr());
        }
        m_invoke = nullptr;
        m_copy = nullptr;
        m_move = nullptr;
        m_destroy = nullptr;
    }

    R operator()(Args... args) const {
        return m_invoke(const_cast<void*>(static_cast<const void*>(StoragePtr())),
                        std::forward<Args>(args)...);
    }

  private:
    template <typename T>
    void Assign(T&& callable) {
        using U = std::decay_t<T>;

        static_assert(std::is_copy_constructible<U>::value,
                      "FixedFunction target must be copy-constructible");
        static_assert(sizeof(U) <= STORAGE_BYTES, "FixedFunction target too large");
        static_assert(alignof(U) <= alignof(std::max_align_t),
                      "FixedFunction target alignment too large");

        Reset();
        new (StoragePtr()) U(std::forward<T>(callable));

        m_invoke = [](void* storage, Args... a) -> R {
            auto* obj = static_cast<U*>(storage);
            if constexpr (std::is_void<R>::value) {
                (*obj)(std::forward<Args>(a)...);
            } else {
                return (*obj)(std::forward<Args>(a)...);
            }
        };

        m_copy = [](void* dst, const void* src) {
            new (dst) U(*static_cast<const U*>(src));
        };

        m_move = [](void* dst, void* src) {
            new (dst) U(std::move(*static_cast<U*>(src)));
            static_cast<U*>(src)->~U();
        };

        m_destroy = [](void* storage) {
            static_cast<U*>(storage)->~U();
        };
    }

    void* StoragePtr() {
        return static_cast<void*>(m_storage);
    }

    const void* StoragePtr() const {
        return static_cast<const void*>(m_storage);
    }

    void CopyFrom(const FixedFunction& other) {
        if (!other.m_invoke) {
            return;
        }

        m_invoke = other.m_invoke;
        m_copy = other.m_copy;
        m_move = other.m_move;
        m_destroy = other.m_destroy;

        if (m_copy) {
            m_copy(StoragePtr(), other.StoragePtr());
        }
    }

    void MoveFrom(FixedFunction&& other) {
        if (!other.m_invoke) {
            return;
        }

        m_invoke = other.m_invoke;
        m_copy = other.m_copy;
        m_move = other.m_move;
        m_destroy = other.m_destroy;

        if (m_move) {
            m_move(StoragePtr(), other.StoragePtr());
        } else if (m_copy) {
            m_copy(StoragePtr(), other.StoragePtr());
        }

        other.m_invoke = nullptr;
        other.m_copy = nullptr;
        other.m_move = nullptr;
        other.m_destroy = nullptr;
    }

    using InvokeFn = R (*)(void* storage, Args...);
    using CopyFn = void (*)(void* dst, const void* src);
    using MoveFn = void (*)(void* dst, void* src);
    using DestroyFn = void (*)(void* storage);

    InvokeFn m_invoke = nullptr;
    CopyFn m_copy = nullptr;
    MoveFn m_move = nullptr;
    DestroyFn m_destroy = nullptr;

    alignas(std::max_align_t) unsigned char m_storage[STORAGE_BYTES] = {};
};

} // namespace fxe
