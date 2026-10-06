#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>

namespace zoomit {
// A small, non-activating overlay. No full-screen bitmap, worker or idle timer.
class ZoomIndicator {
    HWND window_{};
    HDC dc_{};
    HBITMAP bitmap_{};
    void* pixels_{};
    SIZE size_{};
    ULONGLONG expires_{};
    float factor_{};
    UINT dpi_{};
    RECT monitor_{};
    wchar_t text_[24]{};
    bool failed_{};
    void ReleaseSurface() noexcept {
        // Delete the DC first, so its selected bitmap is no longer in use.
        if(dc_)DeleteDC(dc_);dc_=nullptr;
        if(bitmap_)DeleteObject(bitmap_);bitmap_=nullptr;pixels_=nullptr;size_={};
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam) noexcept {
        auto self=reinterpret_cast<ZoomIndicator*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<ZoomIndicator*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(self) {
            switch(message) {
            case WM_NCHITTEST:return HTTRANSPARENT;
            case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
            case WM_TIMER:
                if(wParam==1 && GetTickCount64()>=self->expires_)self->Hide();
                return 0;
            case WM_DISPLAYCHANGE:case WM_DPICHANGED:self->Hide();return 0;
            case WM_CLOSE:return 0;
            case WM_NCDESTROY:self->ReleaseSurface();self->window_=nullptr;self->expires_=0;break;
            }
        }
        return DefWindowProcW(window,message,wParam,lParam);
    }
public:
    static constexpr UINT Duration=1200;
    ZoomIndicator() noexcept=default;
    ZoomIndicator(const ZoomIndicator&)=delete;
    ZoomIndicator& operator=(const ZoomIndicator&)=delete;
    ~ZoomIndicator(){Destroy();}
    static void Format(float factor,wchar_t (&text)[24]) noexcept {
        const int value=static_cast<int>(std::lround(factor*100.0f));
        wchar_t decimal[8]{};
        if(!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_SDECIMAL,decimal,_countof(decimal)))decimal[0]=L'.';
        if(value%100==0)swprintf_s(text,L"%d\u00d7",value/100);
        else if(value%10==0)swprintf_s(text,L"%d%c%d\u00d7",value/100,decimal[0],value%100/10);
        else swprintf_s(text,L"%d%c%02d\u00d7",value/100,decimal[0],value%100);
    }
    DWORD Ensure() noexcept {
        if(window_ || failed_)return ERROR_SUCCESS;
        WNDCLASSW type{};type.lpfnWndProc=Procedure;type.hInstance=GetModuleHandleW(nullptr);
        type.lpszClassName=L"ZoomItCustomZoomIndicator";
        if(!RegisterClassW(&type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return GetLastError();
        window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
            type.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,type.hInstance,this);
        if(!window_)return GetLastError();
        // Best effort on current Windows; capture paths also explicitly hide and flush.
        SetWindowDisplayAffinity(window_,0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
        return ERROR_SUCCESS;
    }
    DWORD Show(float factor,const RECT& monitor,UINT dpi) noexcept {
        if(failed_)return ERROR_SUCCESS;
        if(!window_ || !std::isfinite(factor) || factor<1 || factor>32 ||
           monitor.right<=monitor.left || monitor.bottom<=monitor.top)return ERROR_INVALID_PARAMETER;
        dpi=std::clamp(dpi,96u,384u);
        if(Visible() && factor_==factor && dpi_==dpi && EqualRect(&monitor_,&monitor))return ERROR_SUCCESS;
        const int width=MulDiv(240,dpi,96),height=MulDiv(86,dpi,96);
        if(!dc_ || size_.cx!=width || size_.cy!=height) {
            ReleaseSurface();
            dc_=CreateCompatibleDC(nullptr);
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,&pixels_,nullptr,0);
            if(!dc_ || !bitmap_ || !pixels_){ReleaseSurface();return ERROR_NOT_ENOUGH_MEMORY;}
            if(!SelectObject(dc_,bitmap_)){ReleaseSurface();return ERROR_NOT_ENOUGH_MEMORY;}
            size_={width,height};
        }
        Format(factor,text_);
        {
            using namespace Gdiplus;
            Bitmap image(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(pixels_));
            Graphics graphics(&image);
            FontFamily preferred(L"Segoe UI");
            const FontFamily* family=preferred.GetLastStatus()==Ok ? &preferred : FontFamily::GenericSansSerif();
            GraphicsPath glyphs;
            const REAL scale=static_cast<REAL>(dpi)/96.0f;
            if(image.GetLastStatus()!=Ok || graphics.GetLastStatus()!=Ok ||
               graphics.Clear(Color(0,0,0,0))!=Ok ||
               glyphs.AddString(text_,-1,family,FontStyleBold,54.0f*scale,PointF(0,0),nullptr)!=Ok)
                return ERROR_NOT_ENOUGH_MEMORY;
            RectF bounds{};if(glyphs.GetBounds(&bounds)!=Ok)return ERROR_GEN_FAILURE;
            Matrix position;position.Translate(width-8*scale-bounds.GetRight(),height-8*scale-bounds.GetBottom());
            glyphs.Transform(&position);graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen outline(Color(245,15,15,15),4.0f*scale);outline.SetLineJoin(LineJoinRound);
            SolidBrush letters(Color(255,255,255,255));
            if(graphics.DrawPath(&outline,&glyphs)!=Ok || graphics.FillPath(&letters,&glyphs)!=Ok)return ERROR_GEN_FAILURE;
        }
        const int margin=MulDiv(24,dpi,96);
        POINT location{(std::max)(monitor.left,monitor.right-width-margin),
                       (std::max)(monitor.top,monitor.bottom-height-margin)};
        POINT origin{};BLENDFUNCTION blend{AC_SRC_OVER,0,205,AC_SRC_ALPHA};
#ifdef ZOOMIT_TESTING
        if(denyPresentation)return ERROR_NOT_ENOUGH_MEMORY;
#endif
        if(!UpdateLayeredWindow(window_,nullptr,&location,&size_,dc_,&origin,0,&blend,ULW_ALPHA))return GetLastError();
        factor_=factor;dpi_=dpi;monitor_=monitor;expires_=GetTickCount64()+Duration;
        ShowWindow(window_,SW_SHOWNOACTIVATE);KeepOnTop();
        if(!SetTimer(window_,1,Duration,nullptr)){const DWORD error=GetLastError();Hide();return error ? error : ERROR_GEN_FAILURE;}
        return ERROR_SUCCESS;
    }
    void KeepOnTop() noexcept {
        if(Visible())SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE);
    }
    void Hide(bool beforeCapture=false) noexcept {
        if(window_){KillTimer(window_,1);ShowWindow(window_,SW_HIDE);}
        expires_=0;ReleaseSurface();
        if(beforeCapture && window_)DwmFlush();
    }
    void Fail() noexcept {Hide();failed_=true;}
    void BeginSession() noexcept {Hide();failed_=false;}
    void Destroy() noexcept {Hide();if(window_)DestroyWindow(window_);window_=nullptr;failed_=false;}
    HWND Window() const noexcept{return window_;}
    bool Visible() const noexcept{return window_ && IsWindowVisible(window_);}
    bool Failed() const noexcept{return failed_;}
#ifdef ZOOMIT_TESTING
    inline static bool denyPresentation{};
    float Factor() const noexcept{return factor_;}
    const wchar_t* Text() const noexcept{return text_;}
    ULONGLONG Expires() const noexcept{return expires_;}
    size_t SurfaceBytes() const noexcept{return static_cast<size_t>(size_.cx)*size_.cy*4;}
    HDC SurfaceDC() const noexcept{return dc_;}
    const std::uint32_t* Pixels() const noexcept{return static_cast<const std::uint32_t*>(pixels_);}
    SIZE SurfaceSize() const noexcept{return size_;}
    HBITMAP Surface() const noexcept{return bitmap_;}
#endif
};
}
