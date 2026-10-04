#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace zoomit {

// A native cursor stays out of annotation bitmaps and does not repaint the desktop on movement.
class SuspendedDrawCursor {
    HCURSOR cursor_{};
    COLORREF color_{};
    int size_{};
public:
    SuspendedDrawCursor() = default;
    ~SuspendedDrawCursor() { Reset(); }
    SuspendedDrawCursor(const SuspendedDrawCursor&) = delete;
    SuspendedDrawCursor& operator=(const SuspendedDrawCursor&) = delete;

    HCURSOR Get() const noexcept { return cursor_ ? cursor_ : LoadCursor(nullptr, IDC_ARROW); }
    void Reset() noexcept {
        if (cursor_) {
            if (GetCursor() == cursor_) SetCursor(nullptr);
            DestroyCursor(cursor_);
            cursor_ = nullptr;
        }
    }
    bool Update(COLORREF color) noexcept {
        color &= 0xFFFFFF;
        const int size = (std::clamp)(GetSystemMetrics(SM_CXCURSOR), 32, 128);
        if (cursor_ && color == color_ && size == size_) return true;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
        void* bits{};
        HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        const std::array<BYTE, 128 * 128 / 8> emptyMask{};
        HBITMAP mask = CreateBitmap(size, size, 1, 1, emptyMask.data());
        if (!image || !mask || !bits) {
            if (image) DeleteObject(image);
            if (mask) DeleteObject(mask);
            return false;
        }
        // Premultiplied BGRA: opaque dark centre, active-colour rim, transparent corners.
        auto* pixels = static_cast<std::uint32_t*>(bits);
        const float radius = size * 0.25f;
        const float border = (std::max)(2.0f, size / 16.0f);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float dx = x - size / 2.0f, dy = y - size / 2.0f;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const auto alpha = static_cast<unsigned>((std::clamp)(radius + 0.5f - distance, 0.0f, 1.0f) * 255.0f);
                const COLORREF pixel = distance <= radius - border ? RGB(32, 32, 32) : color;
                pixels[y * size + x] = (alpha << 24) | ((GetRValue(pixel) * alpha / 255) << 16) |
                                       ((GetGValue(pixel) * alpha / 255) << 8) | (GetBValue(pixel) * alpha / 255);
            }
        }
        ICONINFO icon{};
        icon.xHotspot = icon.yHotspot = size / 2;
        icon.hbmColor = image;
        icon.hbmMask = mask;
        HCURSOR replacement = reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon));
        DeleteObject(image);
        DeleteObject(mask);
        if (!replacement) return false;
        if (cursor_ && GetCursor() == cursor_) SetCursor(replacement);
        if (cursor_) DestroyCursor(cursor_);
        cursor_ = replacement;
        color_ = color;
        size_ = size;
        return true;
    }
};

} // namespace zoomit
