#pragma once
#include <windows.h>
#include <utility>
#include <cstdint>
#include <initializer_list>
#include "RuntimeSafety.h"
namespace zoomit {
template<class F> class ScopeExit {
    F function_;
public:
    explicit ScopeExit(F function):function_(std::move(function)){}
    ~ScopeExit() noexcept { function_(); }
    ScopeExit(const ScopeExit&)=delete;
};
template<class F> ScopeExit<F> OnExit(F function){return ScopeExit<F>(std::move(function));}

// A new session is published only after all allocations, selections and captures succeed.
struct GraphicsSession {
private:
    RECT captureArea_{};
public:
    HDC screen{},canvas{},saved{},cursor{};
    HBITMAP image{},background{},pointer{};
    HPEN pen{};
    static constexpr unsigned PreparationChecks=13;
    GraphicsSession() noexcept=default;
    GraphicsSession(const GraphicsSession&)=delete;
    GraphicsSession& operator=(const GraphicsSession&)=delete;
    ~GraphicsSession(){Reset();}
    void Reset() noexcept {
        for(HDC dc:{canvas,saved,cursor,screen})if(dc)DeleteDC(dc);
        canvas=saved=cursor=screen=nullptr;
        for(HGDIOBJ object:{static_cast<HGDIOBJ>(image),static_cast<HGDIOBJ>(background),
                          static_cast<HGDIOBJ>(pointer),static_cast<HGDIOBJ>(pen)})if(object)DeleteObject(object);
        image=background=pointer=nullptr;pen=nullptr;
        captureArea_={};
    }
    bool Allocate(RECT monitor,DWORD penWidth,COLORREF color)noexcept{
        Reset();
        const int64_t w=static_cast<int64_t>(monitor.right)-monitor.left,h=static_cast<int64_t>(monitor.bottom)-monitor.top;
        if(w<=0||h<=0||w>32768||h>32768||static_cast<uint64_t>(w)*h*4>512ull*1024*1024)return false;
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
        wchar_t failure[8];
        if(GetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",failure,8))return false;
#endif
        const auto permit=[] {return runtime::Permit(runtime::Api::Graphics);};
        if(!permit() || !(screen=CreateDCW(L"DISPLAY",nullptr,nullptr,nullptr)))return false;
        if(!permit() || !(canvas=CreateCompatibleDC(screen)))return false;
        if(!permit() || !(saved=CreateCompatibleDC(screen)))return false;
        if(!permit() || !(cursor=CreateCompatibleDC(screen)))return false;
        if(!permit() || !(image=CreateCompatibleBitmap(screen,static_cast<int>(w),static_cast<int>(h))))return false;
        if(!permit() || !(background=CreateCompatibleBitmap(screen,static_cast<int>(w),static_cast<int>(h))))return false;
        if(!permit() || !(pointer=CreateCompatibleBitmap(screen,610,610)))return false;
        if(!permit() || !(pen=CreatePen(PS_SOLID,penWidth,color&0xFFFFFF)))return false;
        const auto selected=[](HDC dc,HGDIOBJ object){
            if(!runtime::Permit(runtime::Api::Graphics))return false;
            const auto old=SelectObject(dc,object);return old && old!=HGDI_ERROR;
        };
        if (!selected(canvas,image) || !selected(saved,background) || !selected(cursor,pointer)) return false;
        captureArea_=monitor;
        return true;
    }
    // Freeze the desktop once. Erase must recover exactly the pixels the user saw.
    template<class Blit = decltype(&BitBlt)>
    bool Capture(Blit blit = &BitBlt) const noexcept {
        const int width=captureArea_.right-captureArea_.left;
        const int height=captureArea_.bottom-captureArea_.top;
        return width>0 && height>0 && runtime::Permit(runtime::Api::Graphics) &&
            blit(canvas,0,0,width,height,screen,captureArea_.left,captureArea_.top,SRCCOPY|CAPTUREBLT) &&
            runtime::Permit(runtime::Api::Graphics) && blit(saved,0,0,width,height,canvas,0,0,SRCCOPY);
    }
};
}
