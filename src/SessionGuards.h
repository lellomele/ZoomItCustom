#pragma once
#include <windows.h>

namespace zoomit {
class StickyKeysGuard {
    decltype(&SystemParametersInfoW) api_;
    STICKYKEYS saved_{sizeof(STICKYKEYS), 0};
    bool owned_{};
public:
    explicit StickyKeysGuard(decltype(api_) api = SystemParametersInfoW) noexcept : api_(api) {}
    ~StickyKeysGuard() { Restore(); }
    StickyKeysGuard(const StickyKeysGuard&) = delete;
    StickyKeysGuard& operator=(const StickyKeysGuard&) = delete;
    void SuppressShortcut() noexcept {
        if (owned_) return;
        STICKYKEYS current{sizeof(STICKYKEYS), 0};
        if (!api_(SPI_GETSTICKYKEYS, sizeof(current), &current, 0) || (current.dwFlags & SKF_STICKYKEYSON)) return;
        STICKYKEYS temporary = current;
        temporary.dwFlags &= ~(SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY);
        if (temporary.dwFlags == current.dwFlags) return;
        if (api_(SPI_SETSTICKYKEYS, sizeof(temporary), &temporary, 0)) {
            saved_ = current;
            owned_ = true;
        }
    }
    void Restore() noexcept {
        if (owned_ && api_(SPI_SETSTICKYKEYS, sizeof(saved_), &saved_, 0)) owned_ = false;
    }
};
class DisplayWakeGuard {
    decltype(&SetThreadExecutionState) api_;
    EXECUTION_STATE saved_{};
public:
    explicit DisplayWakeGuard(decltype(api_) api = SetThreadExecutionState) noexcept : api_(api) {}
    ~DisplayWakeGuard() { Restore(); }
    DisplayWakeGuard(const DisplayWakeGuard&) = delete;
    DisplayWakeGuard& operator=(const DisplayWakeGuard&) = delete;
    bool active() const noexcept { return saved_ != 0; }
    void SuppressSleep() noexcept {
        if (!saved_) saved_ = api_(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
    void Restore() noexcept {
        if (saved_ && api_(saved_ | ES_CONTINUOUS)) saved_ = 0;
    }
};
} // namespace zoomit
