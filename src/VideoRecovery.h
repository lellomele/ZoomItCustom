#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "DriverResetEvents.h"
namespace zoomit::video {
constexpr UINT VideoRequest = WM_APP + 71, VideoStep = WM_APP + 72, VideoFinished = WM_APP + 73;
struct RepairWindow {
    bool pending{}, detached{};
    unsigned attempts{}, recent{};
    ULONGLONG began{}, lastSignal{}, retryAfter{}, completed{}, recentStart{};
    std::uint64_t generation{};
    bool Request(ULONGLONG now, std::uint64_t session, bool force = false) noexcept {
        if (pending && generation == session) {
            lastSignal = now;
            return false;
        }
        if (completed && now - completed < 1000 && !force)
            return false;
        if (!recentStart || now - recentStart >= 15000) {
            recentStart = now;
            recent = 0;
        }
        pending = true;
        detached = false;
        attempts = 0;
        began = lastSignal = now;
        retryAfter = 0;
        generation = session;
        ++recent;
        return true;
    }
    bool Exhausted(ULONGLONG now) const noexcept {
        return attempts >= 3 || now - began >= 8000 || recent > 3;
    }
    DWORD Delay(ULONGLONG now) const noexcept {
        const ULONGLONG quiet = lastSignal + 250, limit = began + 2000;
        const ULONGLONG due = (std::max)((std::min)(quiet, limit), retryAfter);
        return due > now ? static_cast<DWORD>((std::min)(due - now, ULONGLONG(2000))) : 0;
    }
    void Retry(ULONGLONG now) noexcept {
        retryAfter = now + 500;
    }
    void Success(ULONGLONG now) noexcept {
        pending = false;
        detached = false;
        completed = now;
    }
    void Cancel() noexcept {
        pending = false;
        detached = false;
        ++generation;
    }
};
inline bool FitViewport(RECT previous, RECT oldMonitor, RECT monitor, float zoom, RECT &output) noexcept {
    const int64_t width = int64_t(monitor.right) - monitor.left, height = int64_t(monitor.bottom) - monitor.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || !std::isfinite(zoom) || zoom < 1 ||
        zoom > 32 || previous.right <= previous.left || previous.bottom <= previous.top ||
        previous.left < oldMonitor.left || previous.top < oldMonitor.top || previous.right > oldMonitor.right ||
        previous.bottom > oldMonitor.bottom)
        return false;
    const LONG w = (std::max)(LONG(1), static_cast<LONG>(width / zoom));
    const LONG h = (std::max)(LONG(1), static_cast<LONG>(height / zoom));
    // Keep the same center relative to the monitor, even if Windows moves its origin.
    const int64_t centerX2 = int64_t(previous.left) - oldMonitor.left + int64_t(previous.right) - oldMonitor.left;
    const int64_t centerY2 = int64_t(previous.top) - oldMonitor.top + int64_t(previous.bottom) - oldMonitor.top;
    const int64_t left = int64_t(monitor.left) + (centerX2 - w) / 2;
    const int64_t top = int64_t(monitor.top) + (centerY2 - h) / 2;
    output.left = static_cast<LONG>((std::clamp)(left, int64_t(monitor.left), int64_t(monitor.right) - w));
    output.top = static_cast<LONG>((std::clamp)(top, int64_t(monitor.top), int64_t(monitor.bottom) - h));
    output.right = output.left + w;
    output.bottom = output.top + h;
    return true;
}
} // namespace zoomit::video
