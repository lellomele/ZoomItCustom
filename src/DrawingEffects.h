#pragma once
#include <optional>
#include <cstdint>
#include <limits>

// GDI bitmap ownership is tied to its DC: delete the DC before the selected bitmap.
class DrawingDib {
    HDC dc_{};
    HBITMAP bitmap_{};
    BYTE* pixels_{};
public:
    static bool validBounds(const Gdiplus::Rect& bounds) {
        const int64_t maximum = (std::numeric_limits<INT>::max)();
        return bounds.Width > 0 && bounds.Height > 0 &&
            static_cast<uint64_t>(bounds.Width) * bounds.Height <= 64 * 1024 * 1024 &&
            static_cast<int64_t>(bounds.X) + bounds.Width <= maximum &&
            static_cast<int64_t>(bounds.Y) + bounds.Height <= maximum;
    }
    static bool boundsFromEdges(int64_t left, int64_t top, int64_t right, int64_t bottom,
                                Gdiplus::Rect& result) {
        const int64_t minimum = (std::numeric_limits<INT>::min)();
        const int64_t maximum = (std::numeric_limits<INT>::max)();
        if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
            left > maximum || top > maximum || right < minimum || bottom < minimum)
            return false;
        const int64_t width = right - left, height = bottom - top;
        if (width <= 0 || height <= 0 || width > maximum || height > maximum) return false;
        result = Gdiplus::Rect(static_cast<INT>(left), static_cast<INT>(top),
                               static_cast<INT>(width), static_cast<INT>(height));
        return validBounds(result);
    }
    static bool localPoint(POINT point, const Gdiplus::Rect& bounds, INT& x, INT& y) {
        const int64_t localX = static_cast<int64_t>(point.x) - bounds.X;
        const int64_t localY = static_cast<int64_t>(point.y) - bounds.Y;
        const int64_t minimum = (std::numeric_limits<INT>::min)();
        const int64_t maximum = (std::numeric_limits<INT>::max)();
        if (localX < minimum || localX > maximum || localY < minimum || localY > maximum)
            return false;
        x = static_cast<INT>(localX); y = static_cast<INT>(localY);
        return true;
    }
    DrawingDib(HDC source, const Gdiplus::Rect& bounds) {
        if (!source || !validBounds(bounds)) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = bounds.Width;
        info.bmiHeader.biHeight = -bounds.Height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(source);
        if (!dc_) return;
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap_ || !bits) return;
        const HGDIOBJ previous = SelectObject(dc_, bitmap_);
        if (!previous || previous == HGDI_ERROR) return;
        if (!BitBlt(dc_, 0, 0, bounds.Width, bounds.Height, source, bounds.X, bounds.Y, SRCCOPY))
            return;
        // Complete queued GDI writes before directly accessing DIB memory.
        GdiFlush();
        pixels_ = static_cast<BYTE*>(bits);
    }
    ~DrawingDib() { if (dc_) DeleteDC(dc_); if (bitmap_) DeleteObject(bitmap_); }
    DrawingDib(const DrawingDib&) = delete;
    DrawingDib& operator=(const DrawingDib&) = delete;
    BYTE* pixels() const { return pixels_; }
    HDC dc() const { return dc_; }
};

class DrawingBitmapLock {
    Gdiplus::Bitmap& bitmap_;
    Gdiplus::BitmapData data_{};
    bool locked_{};
public:
    explicit DrawingBitmapLock(Gdiplus::Bitmap& bitmap) : bitmap_(bitmap) {
        Gdiplus::Rect area(0, 0, bitmap.GetWidth(), bitmap.GetHeight());
        locked_ = bitmap.LockBits(&area, Gdiplus::ImageLockModeRead,
                                  PixelFormat32bppARGB, &data_) == Gdiplus::Ok;
    }
    ~DrawingBitmapLock() { if (locked_) bitmap_.UnlockBits(&data_); }
    DrawingBitmapLock(const DrawingBitmapLock&) = delete;
    DrawingBitmapLock& operator=(const DrawingBitmapLock&) = delete;
    bool valid() const { return locked_; }
    const BYTE* row(int y) const {
        return static_cast<const BYTE*>(data_.Scan0) + static_cast<ptrdiff_t>(y) * data_.Stride;
    }
};

bool PaintDrawingMask(HDC destination, HDC background, const Gdiplus::Rect& bounds,
                      Gdiplus::Bitmap& mask, bool blur)
{
    if (!DrawingDib::validBounds(bounds) || mask.GetLastStatus() != Gdiplus::Ok ||
        mask.GetWidth() != static_cast<UINT>(bounds.Width) ||
        mask.GetHeight() != static_cast<UINT>(bounds.Height))
        return false;
    DrawingBitmapLock maskLock(mask);
    if (!maskLock.valid()) return false;
    DrawingDib output(destination, bounds);
    if (!output.pixels()) return false;

    // Highlighting in place can read each source pixel before replacing it.
    // Blur always needs its own source because the effect also reads neighbours.
    std::optional<DrawingDib> original;
    if (blur || background != destination) {
        original.emplace(background, bounds);
        if (!original->pixels()) return false;
    }
    const BYTE* backgroundPixels = original ? original->pixels() : output.pixels();
    const size_t stride = static_cast<size_t>(bounds.Width) * 4;
    if (blur) {
        Gdiplus::Bitmap originalBitmap(bounds.Width, bounds.Height, bounds.Width * 4,
                                      PixelFormat32bppARGB, original->pixels());
        Gdiplus::Blur effect;
        Gdiplus::BlurParams parameters{g_BlurRadius, FALSE};
        RECT area{0, 0, bounds.Width, bounds.Height};
        if (originalBitmap.GetLastStatus() != Gdiplus::Ok ||
            effect.SetParameters(&parameters) != Gdiplus::Ok ||
            originalBitmap.ApplyEffect(&effect, &area) != Gdiplus::Ok)
            return false;
        DrawingBitmapLock backgroundLock(originalBitmap);
        if (!backgroundLock.valid()) return false;
        for (int y = 0; y < bounds.Height; ++y) {
            const BYTE* maskRow = maskLock.row(y);
            const BYTE* backgroundRow = backgroundLock.row(y);
            BYTE* out = output.pixels() + static_cast<size_t>(y) * stride;
            for (int x = 0; x < bounds.Width; ++x) {
                const size_t offset = static_cast<size_t>(x) * 4;
                if (maskRow[offset + 3]) memcpy(out + offset, backgroundRow + offset, 3);
            }
        }
    } else {
        // BlendColors uses a fixed channel mask for the current highlighter.
        // Compute it once, keeping the same integer colour values as before.
        const COLORREF channelMask = BlendColors(RGB(255, 255, 255), ColorFromColorRef(g_PenColor));
        const BYTE blue = GetBValue(channelMask), green = GetGValue(channelMask), red = GetRValue(channelMask);
        for (int y = 0; y < bounds.Height; ++y) {
            const BYTE* maskRow = maskLock.row(y);
            const BYTE* backgroundRow = backgroundPixels + static_cast<size_t>(y) * stride;
            BYTE* out = output.pixels() + static_cast<size_t>(y) * stride;
            for (int x = 0; x < bounds.Width; ++x) {
                const size_t offset = static_cast<size_t>(x) * 4;
                if (!maskRow[offset + 3]) continue;
                out[offset] = backgroundRow[offset] & blue;
                out[offset + 1] = backgroundRow[offset + 1] & green;
                out[offset + 2] = backgroundRow[offset + 2] & red;
            }
        }
    }
    return BitBlt(destination, bounds.X, bounds.Y, bounds.Width, bounds.Height, output.dc(), 0, 0, SRCCOPY) != FALSE;
}

void DrawMaskedLine(HDC destination, HDC background, const Gdiplus::Rect& bounds,
                    POINT from, POINT to, Gdiplus::Pen* pen, bool blur)
{
    INT fromX{}, fromY{}, toX{}, toY{};
    if (!pen || !DrawingDib::validBounds(bounds) ||
        !DrawingDib::localPoint(from, bounds, fromX, fromY) ||
        !DrawingDib::localPoint(to, bounds, toX, toY)) return;
    Gdiplus::Bitmap mask(bounds.Width, bounds.Height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&mask);
        graphics.DrawLine(pen, fromX, fromY, toX, toY);
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    PaintDrawingMask(destination, background, bounds, mask, blur);
}

void DrawEffectShape(DWORD shape, HDC destination, Gdiplus::Brush* brush,
                     Gdiplus::Pen* pen, int x1, int y1, int x2, int y2, bool blur)
{
    if ((shape == DRAW_LINE && !pen) ||
        ((shape == DRAW_RECTANGLE || shape == DRAW_ELLIPSE) && !blur && !brush)) return;
    const int64_t padding = shape == DRAW_LINE ? (static_cast<int64_t>(g_PenWidth) + 1) / 2 : 0;
    Gdiplus::Rect bounds;
    if (!DrawingDib::boundsFromEdges(static_cast<int64_t>((std::min)(x1, x2)) - padding,
                                    static_cast<int64_t>((std::min)(y1, y2)) - padding,
                                    static_cast<int64_t>((std::max)(x1, x2)) + padding,
                                    static_cast<int64_t>((std::max)(y1, y2)) + padding, bounds)) return;
    Gdiplus::Bitmap mask(bounds.Width, bounds.Height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&mask);
        Gdiplus::SolidBrush black(static_cast<Gdiplus::ARGB>(Gdiplus::Color::Black));
        Gdiplus::Brush* fill = blur ? &black : brush;
        switch (shape) {
        case DRAW_RECTANGLE: graphics.FillRectangle(fill, 0, 0, bounds.Width, bounds.Height); break;
        case DRAW_ELLIPSE: graphics.FillEllipse(fill, 0, 0, bounds.Width, bounds.Height); break;
        case DRAW_LINE: graphics.DrawLine(pen, static_cast<INT>(static_cast<int64_t>(x1) - bounds.X), static_cast<INT>(static_cast<int64_t>(y1) - bounds.Y), static_cast<INT>(static_cast<int64_t>(x2) - bounds.X), static_cast<INT>(static_cast<int64_t>(y2) - bounds.Y)); break;
        default: return;
        }
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    PaintDrawingMask(destination, destination, bounds, mask, blur);
}

void DrawHighlightedShape(DWORD shape, HDC dc, Gdiplus::Brush* brush, Gdiplus::Pen* pen,
                          int x1, int y1, int x2, int y2) {
    DrawEffectShape(shape, dc, brush, pen, x1, y1, x2, y2, false);
}
void DrawBlurredShape(DWORD shape, Gdiplus::Pen* pen, HDC dc, Gdiplus::Graphics*,
                      int x1, int y1, int x2, int y2) {
    DrawEffectShape(shape, dc, nullptr, pen, x1, y1, x2, y2, true);
}
