//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Utility functions
//
//==============================================================================
#include "pch.h"
#include "Utility.h"
#include <cstdint>
#include <limits>
#include <cmath>

//----------------------------------------------------------------------------
//
// ForceRectInBounds
//
//----------------------------------------------------------------------------
namespace {
LONG ClampCoordinate(int64_t n) noexcept {
    return static_cast<LONG>((std::clamp)(n, static_cast<int64_t>((std::numeric_limits<LONG>::min)()),
                                             static_cast<int64_t>((std::numeric_limits<LONG>::max)())));
}
void BoundAxis(LONG& start, LONG& end, LONG lower, LONG upper) noexcept {
    const int64_t space = static_cast<int64_t>(upper) - lower;
    if (space <= 0) { start = end = lower; return; }
    const int64_t size = (std::clamp)(static_cast<int64_t>(end) - start, int64_t{0}, space);
    const int64_t first = (std::clamp)(static_cast<int64_t>(start), static_cast<int64_t>(lower),
                                      static_cast<int64_t>(upper) - size);
    start = ClampCoordinate(first); end = ClampCoordinate(first + size);
}
void PointsAxis(LONG a, LONG b, LONG minimum, LONG& start, LONG& end) noexcept {
    const int64_t low = (std::numeric_limits<LONG>::min)();
    const int64_t high = (std::numeric_limits<LONG>::max)();
    const int64_t size = (std::min)((std::max)((std::max)(int64_t{1}, static_cast<int64_t>(minimum)),
                        std::abs(static_cast<int64_t>(a)-b)+1), high-low);
    const int64_t first = (std::clamp)(a <= b ? static_cast<int64_t>(a) : static_cast<int64_t>(a)+1-size,
                                      low, high-size);
    start = ClampCoordinate(first); end = ClampCoordinate(first+size);
}
LONG ScaleAxis(LONG point, LONG a, LONG b, LONG c, LONG d) noexcept {
    const int64_t source = static_cast<int64_t>(b)-a, target = static_cast<int64_t>(d)-c;
    if (source <= 0 || target <= 0) return c;
    const int64_t center = static_cast<int64_t>(c)+target/2;
    const int64_t delta = static_cast<int64_t>(point)-(static_cast<int64_t>(a)+source/2);
    if (delta > (std::numeric_limits<int64_t>::max)()/target ||
        delta < (std::numeric_limits<int64_t>::min)()/target) {
        const long double mapped = center + static_cast<long double>(delta)*target/source;
        if (mapped <= (std::numeric_limits<LONG>::min)()) return (std::numeric_limits<LONG>::min)();
        if (mapped >= (std::numeric_limits<LONG>::max)()) return (std::numeric_limits<LONG>::max)();
        return static_cast<LONG>(std::llround(mapped));
    }
    const int64_t numerator = delta*target;
    int64_t offset = numerator/source;
    if (std::abs(numerator%source)*2 >= source) offset += numerator < 0 ? -1 : 1;
    if (offset > (std::numeric_limits<LONG>::max)()-center) return (std::numeric_limits<LONG>::max)();
    if (offset < (std::numeric_limits<LONG>::min)()-center) return (std::numeric_limits<LONG>::min)();
    return static_cast<LONG>(center+offset);
}
}
RECT ForceRectInBounds(RECT rect, const RECT& bounds) {
    BoundAxis(rect.left, rect.right, bounds.left, bounds.right);
    BoundAxis(rect.top, rect.bottom, bounds.top, bounds.bottom);
    return rect;
}

//----------------------------------------------------------------------------
//
// GetDpiForWindow
//
//----------------------------------------------------------------------------
UINT GetDpiForWindowHelper(HWND window) {
    static const auto function = reinterpret_cast<UINT (WINAPI *)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (function) { const UINT dpi = function(window); if (dpi) return dpi; }
    native::unique_screen_dc hdc{GetDC(nullptr)};
    const int dpi = hdc.get() ? GetDeviceCaps(hdc.get(), LOGPIXELSX) : 0;
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

//----------------------------------------------------------------------------
//
// GetMonitorRectFromCursor
//
//----------------------------------------------------------------------------
RECT GetMonitorRectFromCursor() {
    POINT point{}; GetCursorPos(&point);
    MONITORINFO info{}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info)) return info.rcMonitor;
    return RECT{0, 0, (std::max)(1,GetSystemMetrics(SM_CXSCREEN)), (std::max)(1,GetSystemMetrics(SM_CYSCREEN))};
}

//----------------------------------------------------------------------------
//
// RectFromPointsMinSize
//
//----------------------------------------------------------------------------
#ifdef _MSC_VER
    // avoid making RectFromPointsMinSize constexpr since that leads to link errors
    #pragma warning(push)
    #pragma warning(disable: 26497)
#endif

RECT RectFromPointsMinSize(POINT a, POINT b, LONG minSize) {
    RECT rect{};
    PointsAxis(a.x,b.x,minSize,rect.left,rect.right);
    PointsAxis(a.y,b.y,minSize,rect.top,rect.bottom);
    return rect;
}
#ifdef _MSC_VER
    #pragma warning(pop)
#endif
//----------------------------------------------------------------------------
//
// ScaleForDpi
//
//----------------------------------------------------------------------------
int ScaleForDpi( int value, UINT dpi )
{
    return MulDiv( value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI );
}

//----------------------------------------------------------------------------
//
// ScalePointInRects
//
//----------------------------------------------------------------------------
POINT ScalePointInRects(POINT point, const RECT& source, const RECT& target) {
    if (source.right <= source.left || source.bottom <= source.top ||
        target.right <= target.left || target.bottom <= target.top) return POINT{target.left,target.top};
    return POINT{ScaleAxis(point.x,source.left,source.right,target.left,target.right),
                 ScaleAxis(point.y,source.top,source.bottom,target.top,target.bottom)};
}
