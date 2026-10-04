// Reference retained from ZoomItCustom 1.1 for pixel-compatibility and performance checks.
// Copyright (c) Microsoft Corporation. Licensed under the MIT license.
#pragma once

// GDI bitmap ownership is tied to its DC: delete the DC before the selected bitmap.
class DrawingDib {
    HDC dc_{};
    HBITMAP bitmap_{};
    BYTE* pixels_{};
public:
    DrawingDib(HDC source, const Gdiplus::Rect& bounds) {
        if (!source || bounds.Width <= 0 || bounds.Height <= 0 ||
            static_cast<size_t>(bounds.Width) * bounds.Height > 64 * 1024 * 1024)
            return;
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
        if (!bitmap_ || !SelectObject(dc_, bitmap_)) return;
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
    if (bounds.Width <= 0 || bounds.Height <= 0 || mask.GetLastStatus() != Gdiplus::Ok)
        return false;
    DrawingBitmapLock maskLock(mask);
    if (!maskLock.valid()) return false;
    DrawingDib output(destination, bounds);
    DrawingDib original(background, bounds);
    if (!output.pixels() || !original.pixels()) return false;
    Gdiplus::Bitmap originalBitmap(bounds.Width, bounds.Height, bounds.Width * 4,
                                  PixelFormat32bppARGB, original.pixels());
    if (blur) {
        Gdiplus::Blur effect;
        Gdiplus::BlurParams parameters{g_BlurRadius, FALSE};
        RECT area{0, 0, bounds.Width, bounds.Height};
        if (effect.SetParameters(&parameters) != Gdiplus::Ok ||
            originalBitmap.ApplyEffect(&effect, &area) != Gdiplus::Ok)
            return false;
    }
    DrawingBitmapLock backgroundLock(originalBitmap);
    if (!backgroundLock.valid()) return false;
    const Gdiplus::Color highlight = ColorFromColorRef(g_PenColor);
    for (int y = 0; y < bounds.Height; ++y) {
        const BYTE* maskRow = maskLock.row(y);
        const BYTE* backgroundRow = backgroundLock.row(y);
        BYTE* out = output.pixels() + static_cast<size_t>(y) * bounds.Width * 4;
        for (int x = 0; x < bounds.Width; ++x) {
            const size_t offset = static_cast<size_t>(x) * 4;
            if (!maskRow[offset + 3]) continue;
            if (blur) {
                memcpy(out + offset, backgroundRow + offset, 3);
            } else {
                const COLORREF blended = BlendColors(
                    RGB(backgroundRow[offset + 2], backgroundRow[offset + 1], backgroundRow[offset]), highlight);
                out[offset] = GetBValue(blended);
                out[offset + 1] = GetGValue(blended);
                out[offset + 2] = GetRValue(blended);
            }
        }
    }
    return BitBlt(destination, bounds.X, bounds.Y, bounds.Width, bounds.Height, output.dc(), 0, 0, SRCCOPY) != FALSE;
}

void DrawMaskedLine(HDC destination, HDC background, const Gdiplus::Rect& bounds,
                    POINT from, POINT to, Gdiplus::Pen* pen, bool blur)
{
    if (bounds.Width <= 0 || bounds.Height <= 0) return;
    Gdiplus::Bitmap mask(bounds.Width, bounds.Height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&mask);
        graphics.DrawLine(pen, static_cast<INT>(from.x - bounds.X), static_cast<INT>(from.y - bounds.Y),
                          static_cast<INT>(to.x - bounds.X), static_cast<INT>(to.y - bounds.Y));
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    reference::PaintDrawingMask(destination, background, bounds, mask, blur);
}

void DrawEffectShape(DWORD shape, HDC destination, Gdiplus::Brush* brush,
                     Gdiplus::Pen* pen, int x1, int y1, int x2, int y2, bool blur)
{
    Gdiplus::Rect bounds((std::min)(x1, x2), (std::min)(y1, y2), abs(x2 - x1), abs(y2 - y1));
    if (shape == DRAW_LINE)
        bounds.Inflate(static_cast<int>((g_PenWidth + 1) / 2), static_cast<int>((g_PenWidth + 1) / 2));
    if (bounds.Width <= 0 || bounds.Height <= 0) return;
    Gdiplus::Bitmap mask(bounds.Width, bounds.Height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&mask);
        Gdiplus::SolidBrush black(static_cast<Gdiplus::ARGB>(Gdiplus::Color::Black));
        Gdiplus::Brush* fill = blur ? &black : brush;
        switch (shape) {
        case DRAW_RECTANGLE: graphics.FillRectangle(fill, 0, 0, bounds.Width, bounds.Height); break;
        case DRAW_ELLIPSE: graphics.FillEllipse(fill, 0, 0, bounds.Width, bounds.Height); break;
        case DRAW_LINE: graphics.DrawLine(pen, x1 - bounds.X, y1 - bounds.Y, x2 - bounds.X, y2 - bounds.Y); break;
        default: return;
        }
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    reference::PaintDrawingMask(destination, destination, bounds, mask, blur);
}

void DrawHighlightedShape(DWORD shape, HDC dc, Gdiplus::Brush* brush, Gdiplus::Pen* pen,
                          int x1, int y1, int x2, int y2) {
    reference::DrawEffectShape(shape, dc, brush, pen, x1, y1, x2, y2, false);
}
void DrawBlurredShape(DWORD shape, Gdiplus::Pen* pen, HDC dc, Gdiplus::Graphics*,
                      int x1, int y1, int x2, int y2) {
    reference::DrawEffectShape(shape, dc, nullptr, pen, x1, y1, x2, y2, true);
}
