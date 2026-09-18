#pragma once
#ifdef _WIN32
#include <utility>
#include <windows.h>
namespace axiom {
class UniqueWin32Handle final {
  public:
    UniqueWin32Handle() noexcept = default;
    explicit UniqueWin32Handle(HANDLE value) noexcept : value_{value} {}
    ~UniqueWin32Handle() {
        reset();
    }
    UniqueWin32Handle(const UniqueWin32Handle &) = delete;
    UniqueWin32Handle &operator=(const UniqueWin32Handle &) = delete;
    UniqueWin32Handle(UniqueWin32Handle &&other) noexcept
        : value_{std::exchange(other.value_, nullptr)} {}
    UniqueWin32Handle &operator=(UniqueWin32Handle &&other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return valid(value_);
    }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (valid(value_)) {
            CloseHandle(value_);
        }
        value_ = replacement;
    }

  private:
    HANDLE value_{nullptr};
    [[nodiscard]] static bool valid(HANDLE value) noexcept {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
};
} // namespace axiom
#endif
