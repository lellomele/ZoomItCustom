#pragma once
#include "RuntimeSafety.h"
#include <cstdint>
#include <climits>
#include <limits>

inline bool ValidDrawingBounds(const Gdiplus::Rect& bounds) noexcept {
    const int64_t right=static_cast<int64_t>(bounds.X)+bounds.Width;
    const int64_t bottom=static_cast<int64_t>(bounds.Y)+bounds.Height;
    return bounds.Width>0 && bounds.Height>0 && bounds.Width<=32768 && bounds.Height<=32768 &&
        static_cast<uint64_t>(bounds.Width)*bounds.Height*4<=256ull*1024*1024 &&
        right<=INT_MAX && bottom<=INT_MAX;
}
inline bool DrawingBitmapExtent(HDC dc,SIZE& extent) noexcept {
    if(!dc || GetObjectType(dc)!=OBJ_MEMDC)return false;
    BITMAP bitmap{};const HGDIOBJ image=GetCurrentObject(dc,OBJ_BITMAP);
    if(!image || GetObjectW(image,static_cast<int>(sizeof(bitmap)),&bitmap)!=static_cast<int>(sizeof(bitmap)) || bitmap.bmWidth<=0 || bitmap.bmHeight<=0)return false;
    extent={bitmap.bmWidth,bitmap.bmHeight};return true;
}
// Work only on pixels present in the actual canvas. Clip before constructing
// the effect mask, so offscreen coordinates never cause oversized allocations.
inline bool ClipDrawingBounds(HDC destination,HDC background,int64_t left,int64_t top,
                              int64_t right,int64_t bottom,Gdiplus::Rect& clipped) noexcept {
    SIZE target{},source{};
    if(!DrawingBitmapExtent(destination,target) || !DrawingBitmapExtent(background,source))return false;
    left=(std::max)(int64_t{0},left);top=(std::max)(int64_t{0},top);
    right=(std::min)(right,static_cast<int64_t>((std::min)(target.cx,source.cx)));
    bottom=(std::min)(bottom,static_cast<int64_t>((std::min)(target.cy,source.cy)));
    if(right<=left || bottom<=top || right>INT_MAX || bottom>INT_MAX)return false;
    clipped={static_cast<INT>(left),static_cast<INT>(top),static_cast<INT>(right-left),static_cast<INT>(bottom-top)};
    return ValidDrawingBounds(clipped);
}

// GDI bitmap ownership is tied to its DC: delete the DC before the selected bitmap.
class DrawingDib {
    HDC dc_{};
    HBITMAP bitmap_{};
    BYTE* pixels_{};
public:
#if defined(ZOOMIT_TESTING) || defined(ZOOMIT_RECOVERY_TESTING)
    inline static std::atomic<unsigned> liveDcs{},liveBitmaps{},deleteFailures{};
    static std::array<unsigned,3> Ownership() noexcept {
        return {liveDcs.load(),liveBitmaps.load(),deleteFailures.load()};
    }
#endif
    DrawingDib(HDC source,const Gdiplus::Rect& bounds) {
        using zoomit::runtime::Api;
        if(!source || !ValidDrawingBounds(bounds))return;
        SIZE extent{};
        if(DrawingBitmapExtent(source,extent) && (bounds.X<0 || bounds.Y<0 ||
           static_cast<int64_t>(bounds.X)+bounds.Width>extent.cx || static_cast<int64_t>(bounds.Y)+bounds.Height>extent.cy))return;
        BITMAPINFO info{};
        info.bmiHeader={sizeof(BITMAPINFOHEADER),bounds.Width,-bounds.Height,1,32,BI_RGB};
        if(!zoomit::runtime::Permit(Api::Capture) || !(dc_=CreateCompatibleDC(source)))return;
#if defined(ZOOMIT_TESTING) || defined(ZOOMIT_RECOVERY_TESTING)
        ++liveDcs;
#endif
        void* bits{};
        if(!zoomit::runtime::Permit(Api::Capture) || !(bitmap_=CreateDIBSection(source,&info,DIB_RGB_COLORS,&bits,nullptr,0)))return;
#if defined(ZOOMIT_TESTING) || defined(ZOOMIT_RECOVERY_TESTING)
        ++liveBitmaps;
#endif
        if(!bits)return;
        if(!zoomit::runtime::Permit(Api::Capture))return;
        const auto old=SelectObject(dc_,bitmap_);if(!old || old==HGDI_ERROR)return;
        if(!zoomit::runtime::Permit(Api::Capture) ||
           !BitBlt(dc_,0,0,bounds.Width,bounds.Height,source,bounds.X,bounds.Y,SRCCOPY))return;
        // Complete queued GDI writes before exposing the pixels to CPU code.
        if(!zoomit::runtime::Permit(Api::Flush) || !GdiFlush())return;
        pixels_=static_cast<BYTE*>(bits);
    }
    ~DrawingDib(){
#if defined(ZOOMIT_TESTING) || defined(ZOOMIT_RECOVERY_TESTING)
        if(dc_){if(DeleteDC(dc_))--liveDcs;else ++deleteFailures;}
        if(bitmap_){if(DeleteObject(bitmap_))--liveBitmaps;else ++deleteFailures;}
#else
        if(dc_)DeleteDC(dc_);if(bitmap_)DeleteObject(bitmap_);
#endif
    }
    DrawingDib(const DrawingDib&)=delete;
    DrawingDib& operator=(const DrawingDib&)=delete;
    BYTE* pixels()const{return pixels_;}
    HDC dc()const{return dc_;}
};

class DrawingBitmapLock {
    Gdiplus::Bitmap& bitmap_;
    Gdiplus::BitmapData data_{};
    bool locked_{};
    UINT height_{};
public:
    explicit DrawingBitmapLock(Gdiplus::Bitmap& bitmap):bitmap_(bitmap) {
        const UINT width=bitmap.GetWidth();height_=bitmap.GetHeight();
        if(bitmap.GetLastStatus()!=Gdiplus::Ok || !width || !height_ || width>32768 || height_>32768)return;
        Gdiplus::Rect area(0,0,width,height_);
        locked_=bitmap.LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&data_)==Gdiplus::Ok;
        if(locked_ && (!data_.Scan0 || std::abs(static_cast<int64_t>(data_.Stride))<static_cast<int64_t>(width)*4)) {
            bitmap_.UnlockBits(&data_);locked_=false;
        }
    }
    ~DrawingBitmapLock(){if(locked_)bitmap_.UnlockBits(&data_);}
    DrawingBitmapLock(const DrawingBitmapLock&)=delete;
    DrawingBitmapLock& operator=(const DrawingBitmapLock&)=delete;
    bool valid()const{return locked_;}
    const BYTE* row(int y)const {
        if(!locked_ || y<0 || static_cast<UINT>(y)>=height_)return nullptr;
        return static_cast<const BYTE*>(data_.Scan0)+static_cast<ptrdiff_t>(y)*data_.Stride;
    }
};

bool PaintDrawingMask(HDC destination,HDC background,const Gdiplus::Rect& bounds,
                      Gdiplus::Bitmap& mask,bool blur) {
    if(!destination || !background || !ValidDrawingBounds(bounds) || mask.GetLastStatus()!=Gdiplus::Ok ||
       mask.GetWidth()<static_cast<UINT>(bounds.Width) || mask.GetHeight()<static_cast<UINT>(bounds.Height))return false;
    DrawingBitmapLock maskLock(mask);if(!maskLock.valid())return false;
    DrawingDib output(destination,bounds);if(!output.pixels())return false;
    DrawingDib original(background,bounds);if(!original.pixels())return false;
    Gdiplus::Bitmap originalBitmap(bounds.Width,bounds.Height,bounds.Width*4,PixelFormat32bppARGB,original.pixels());
    if(originalBitmap.GetLastStatus()!=Gdiplus::Ok)return false;
    if(blur) {
        Gdiplus::Blur effect;Gdiplus::BlurParams parameters{g_BlurRadius,FALSE};RECT area{0,0,bounds.Width,bounds.Height};
        if(effect.SetParameters(&parameters)!=Gdiplus::Ok || originalBitmap.ApplyEffect(&effect,&area)!=Gdiplus::Ok)return false;
    }
    DrawingBitmapLock backgroundLock(originalBitmap);if(!backgroundLock.valid())return false;
    const Gdiplus::Color highlight=ColorFromColorRef(g_PenColor);
    for(int y=0;y<bounds.Height;++y) {
        const BYTE* maskRow=maskLock.row(y);const BYTE* backgroundRow=backgroundLock.row(y);
        if(!maskRow || !backgroundRow)return false;
        BYTE* out=output.pixels()+static_cast<size_t>(y)*bounds.Width*4;
        for(int x=0;x<bounds.Width;++x) {
            const size_t offset=static_cast<size_t>(x)*4;
            if(!maskRow[offset+3])continue;
            if(blur)memcpy(out+offset,backgroundRow+offset,3);
            else {
                const COLORREF blended=BlendColors(RGB(backgroundRow[offset+2],backgroundRow[offset+1],backgroundRow[offset]),highlight);
                out[offset]=GetBValue(blended);out[offset+1]=GetGValue(blended);out[offset+2]=GetRValue(blended);
            }
        }
    }
    return zoomit::runtime::Permit(zoomit::runtime::Api::Capture) &&
        BitBlt(destination,bounds.X,bounds.Y,bounds.Width,bounds.Height,output.dc(),0,0,SRCCOPY)!=FALSE;
}

void DrawMaskedLine(HDC destination,HDC background,const Gdiplus::Rect& requested,
                    POINT from,POINT to,Gdiplus::Pen* pen,bool blur) {
    if(!pen || pen->GetLastStatus()!=Gdiplus::Ok || requested.Width<=0 || requested.Height<=0)return;
    Gdiplus::Rect bounds;
    if(!ClipDrawingBounds(destination,background,requested.X,requested.Y,
       static_cast<int64_t>(requested.X)+requested.Width,static_cast<int64_t>(requested.Y)+requested.Height,bounds))return;
    Gdiplus::Bitmap mask(bounds.Width,bounds.Height,PixelFormat32bppARGB);if(mask.GetLastStatus()!=Gdiplus::Ok)return;
    {
        Gdiplus::Graphics graphics(&mask);
        if(graphics.GetLastStatus()!=Gdiplus::Ok || graphics.Clear(Gdiplus::Color(0,0,0,0))!=Gdiplus::Ok ||
           graphics.DrawLine(pen,static_cast<Gdiplus::REAL>(static_cast<int64_t>(from.x)-bounds.X),
               static_cast<Gdiplus::REAL>(static_cast<int64_t>(from.y)-bounds.Y),
               static_cast<Gdiplus::REAL>(static_cast<int64_t>(to.x)-bounds.X),
               static_cast<Gdiplus::REAL>(static_cast<int64_t>(to.y)-bounds.Y))!=Gdiplus::Ok)return;
        graphics.Flush(Gdiplus::FlushIntentionSync);if(graphics.GetLastStatus()!=Gdiplus::Ok)return;
    }
    PaintDrawingMask(destination,background,bounds,mask,blur);
}

void DrawEffectShape(DWORD shape,HDC destination,Gdiplus::Brush* brush,
                     Gdiplus::Pen* pen,int x1,int y1,int x2,int y2,bool blur) {
    if(shape!=DRAW_RECTANGLE && shape!=DRAW_ELLIPSE && shape!=DRAW_LINE)return;
    if((shape==DRAW_LINE && (!pen || pen->GetLastStatus()!=Gdiplus::Ok)) || (shape!=DRAW_LINE && !blur && !brush))return;
    const int64_t left=(std::min)(x1,x2),top=(std::min)(y1,y2);
    const int64_t right=(std::max)(x1,x2),bottom=(std::max)(y1,y2);
    const int64_t padding=shape==DRAW_LINE ? ((std::min)(g_PenWidth,DWORD{600})+1)/2 : 0;
    Gdiplus::Rect bounds;
    if(!ClipDrawingBounds(destination,destination,left-padding,top-padding,right+padding,bottom+padding,bounds))return;
    Gdiplus::Bitmap mask(bounds.Width,bounds.Height,PixelFormat32bppARGB);if(mask.GetLastStatus()!=Gdiplus::Ok)return;
    {
        Gdiplus::Graphics graphics(&mask);Gdiplus::SolidBrush black(static_cast<Gdiplus::ARGB>(Gdiplus::Color::Black));
        Gdiplus::Brush* fill=blur ? &black : brush;
        if(graphics.GetLastStatus()!=Gdiplus::Ok || graphics.Clear(Gdiplus::Color(0,0,0,0))!=Gdiplus::Ok)return;
        Gdiplus::Status status=Gdiplus::InvalidParameter;
        switch(shape) {
        case DRAW_RECTANGLE:status=graphics.FillRectangle(fill,0,0,bounds.Width,bounds.Height);break;
        case DRAW_ELLIPSE:status=graphics.FillEllipse(fill,static_cast<Gdiplus::REAL>(left-bounds.X),static_cast<Gdiplus::REAL>(top-bounds.Y),
            static_cast<Gdiplus::REAL>(right-left),static_cast<Gdiplus::REAL>(bottom-top));break;
        case DRAW_LINE:status=graphics.DrawLine(pen,static_cast<Gdiplus::REAL>(static_cast<int64_t>(x1)-bounds.X),
            static_cast<Gdiplus::REAL>(static_cast<int64_t>(y1)-bounds.Y),static_cast<Gdiplus::REAL>(static_cast<int64_t>(x2)-bounds.X),
            static_cast<Gdiplus::REAL>(static_cast<int64_t>(y2)-bounds.Y));break;
        }
        if(status!=Gdiplus::Ok)return;
        graphics.Flush(Gdiplus::FlushIntentionSync);if(graphics.GetLastStatus()!=Gdiplus::Ok)return;
    }
    PaintDrawingMask(destination,destination,bounds,mask,blur);
}

void DrawHighlightedShape(DWORD shape,HDC dc,Gdiplus::Brush* brush,Gdiplus::Pen* pen,int x1,int y1,int x2,int y2) {
    DrawEffectShape(shape,dc,brush,pen,x1,y1,x2,y2,false);
}
void DrawBlurredShape(DWORD shape,Gdiplus::Pen* pen,HDC dc,Gdiplus::Graphics*,int x1,int y1,int x2,int y2) {
    DrawEffectShape(shape,dc,nullptr,pen,x1,y1,x2,y2,true);
}
