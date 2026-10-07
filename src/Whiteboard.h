#pragma once
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include "Utility.h"
#include "RuntimeSafety.h"

namespace zoomit::whiteboard {
struct Options {
    DWORD toggleKey{(HOTKEYF_CONTROL<<8)|'6'};
    DWORD backgroundKey{'C'};
    DWORD black{},spacing{32},opacity{16};
};
inline bool ValidOptions(const Options& value) noexcept {
    const DWORD key=value.backgroundKey&0xff;
    return (value.toggleKey&0xff)!=0 && value.black<=1 && value.spacing>=8 && value.spacing<=256 &&
        value.spacing%4==0 && value.opacity>=1 && value.opacity<=60 && key &&
        key!=VK_ESCAPE && key!=VK_CONTROL && key!=VK_SHIFT && key!=VK_MENU &&
        !(key==VK_UP || key==VK_DOWN) && !(value.backgroundKey==value.toggleKey);
}
inline COLORREF GridColor(bool black,DWORD opacity) noexcept {
    const BYTE level=static_cast<BYTE>((255*(std::min)(opacity,DWORD{60})+50)/100);
    return black ? RGB(level,level,level) : RGB(255-level,255-level,255-level);
}
inline bool PaintGrid(HDC dc,RECT area,const Options& options,UINT dpi) noexcept {
    const int step=(std::max)(8,MulDiv(static_cast<int>(options.spacing),static_cast<int>(dpi ? dpi:96),96));
    HBRUSH background=static_cast<HBRUSH>(GetStockObject(options.black ? BLACK_BRUSH:WHITE_BRUSH));
    if(!FillRect(dc,&area,background))return false;
    HPEN pen=CreatePen(PS_SOLID,1,GridColor(options.black!=0,options.opacity));
    if(!pen)return false;
    const auto old=SelectObject(dc,pen);
    if(!old || old==HGDI_ERROR){DeleteObject(pen);return false;}
    bool ok=true;
    for(int x=area.left+step;x<area.right;x+=step)ok=(MoveToEx(dc,x,area.top,nullptr)&&LineTo(dc,x,area.bottom))&&ok;
    for(int y=area.top+step;y<area.bottom;y+=step)ok=(MoveToEx(dc,area.left,y,nullptr)&&LineTo(dc,area.right,y))&&ok;
    SelectObject(dc,old);DeleteObject(pen);return ok;
}
inline void ShortcutText(DWORD key,wchar_t* output,size_t count) noexcept {
    wchar_t name[64]{};
    const UINT scan=MapVirtualKeyW(key&0xff,MAPVK_VK_TO_VSC_EX);
    const LONG code=static_cast<LONG>((scan&0xff)<<16)|((scan&0xff00)?1L<<24:0);
    if(!GetKeyNameTextW(code,name,_countof(name)))swprintf_s(name,L"%c",static_cast<wchar_t>(key&0xff));
    const DWORD modifiers=(key>>8)&0xff;
    swprintf_s(output,count,L"%s%s%s%s",modifiers&HOTKEYF_CONTROL?L"Ctrl+":L"",
        modifiers&HOTKEYF_ALT?L"Alt+":L"",modifiers&HOTKEYF_SHIFT?L"Shift+":L"",name);
}
inline bool MatchesKey(DWORD binding,WPARAM key) noexcept {
    const DWORD modifiers=((GetKeyState(VK_CONTROL)&0x8000)?HOTKEYF_CONTROL:0)|
        ((GetKeyState(VK_MENU)&0x8000)?HOTKEYF_ALT:0)|((GetKeyState(VK_SHIFT)&0x8000)?HOTKEYF_SHIFT:0);
    return (binding&0xff)==key && (((binding>>8)&(HOTKEYF_CONTROL|HOTKEYF_ALT|HOTKEYF_SHIFT))==modifiers);
}

// A small opaque background window, not a screenshot or another drawing engine.
// The existing Draw canvas captures it and releases all annotations on normal exit.
class Board {
    HWND window_{},controller_{},previousForeground_{};
    RECT monitor_{};
    Options options_{};
    bool blocked_{},hint_{},paintFailed_{};
    int wheelRemainder_{};
    UINT changedMessage_{};
    HFONT hintFont_{};
    UINT fontDpi_{};
    zoomit::runtime::SessionTimers<1> hintTimers_;
    void Notify(bool defer=false) noexcept {
        if(!IsWindow(controller_))return;
        if(!defer && GetWindowThreadProcessId(controller_,nullptr)==GetCurrentThreadId())
            SendMessage(controller_,changedMessage_,0,0);
        else PostMessage(controller_,changedMessage_,0,0);
    }
    void ResizeGrid(int steps) noexcept {
        if(blocked_ || !steps)return;
        const int spacing=std::clamp(static_cast<int>(options_.spacing)+steps*4,8,256);
        if(spacing==static_cast<int>(options_.spacing))return;
        options_.spacing=static_cast<DWORD>(spacing);InvalidateRect(window_,nullptr,FALSE);Notify();
    }
    void PaintHint(HDC dc,RECT client) noexcept {
        const UINT dpi=GetDpiForWindowHelper(window_);
        if(!hintFont_ || fontDpi_!=dpi) {
            if(hintFont_)DeleteObject(hintFont_);
            hintFont_=CreateFontW(-MulDiv(22,static_cast<int>(dpi),96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
                DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            fontDpi_=dpi;
        }
        wchar_t shortcut[96]{},text[180]{};ShortcutText(options_.toggleKey,shortcut,_countof(shortcut));
        swprintf_s(text,L"Ripremi %s per uscire dalla lavagna.",shortcut);
        RECT label=client;label.left+=MulDiv(20,dpi,96);label.right-=MulDiv(20,dpi,96);
        label.top+=MulDiv(28,dpi,96);label.bottom=label.top+MulDiv(70,dpi,96);
        const auto font=hintFont_?SelectObject(dc,hintFont_):nullptr;
        const int mode=SetBkMode(dc,TRANSPARENT);
        const COLORREF color=SetTextColor(dc,options_.black?RGB(210,210,210):RGB(64,64,64));
        DrawTextW(dc,text,-1,&label,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
        SetTextColor(dc,color);SetBkMode(dc,mode);if(font)SelectObject(dc,font);
    }
    static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM word,LPARAM param) noexcept {
        auto* self=reinterpret_cast<Board*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Board*>(reinterpret_cast<CREATESTRUCTW*>(param)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));self->window_=window;
        }
        if(!self)return DefWindowProcW(window,message,word,param);
        switch(message) {
        case WM_ERASEBKGND:return TRUE;
        case WM_PAINT:{
            PAINTSTRUCT paint{};BeginPaint(window,&paint);RECT client{};GetClientRect(window,&client);
            if(!PaintGrid(paint.hdc,client,self->options_,GetDpiForWindowHelper(window))) {
                self->paintFailed_=true;self->Notify(true);
            }
            if(self->hint_ && !self->blocked_)self->PaintHint(paint.hdc,client);
            EndPaint(window,&paint);return 0;
        }
        case WM_DPICHANGED:
            InvalidateRect(window,nullptr,FALSE);
            if(IsWindow(self->controller_))PostMessage(self->controller_,WM_DISPLAYCHANGE,0,0);
            return 0;
        case WM_CLOSE:self->ShowExitHint();return 0;
        case WM_KEYDOWN:case WM_SYSKEYDOWN:
            if(self->blocked_)return 0;
            if(word==VK_ESCAPE){self->ShowExitHint();return 0;}
            if(MatchesKey(self->options_.backgroundKey,word)) {
                self->options_.black=!self->options_.black;InvalidateRect(window,nullptr,FALSE);self->Notify();return 0;
            }
            if((GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && (word==VK_UP || word==VK_DOWN)) {
                self->ResizeGrid(word==VK_UP?1:-1);return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if(!self->blocked_ && (LOWORD(word)&MK_CONTROL)) {
                self->wheelRemainder_+=GET_WHEEL_DELTA_WPARAM(word);
                const int steps=self->wheelRemainder_/WHEEL_DELTA;self->wheelRemainder_%=WHEEL_DELTA;
                self->ResizeGrid(steps);
            }
            return 0;
        case WM_TIMER:
            if(self->hintTimers_.Role(word)==0){self->hintTimers_.Stop(0);self->hint_=false;InvalidateRect(window,nullptr,FALSE);return 0;}
            break;
        case WM_SETCURSOR:
            if(LOWORD(param)==HTCLIENT){SetCursor(LoadCursor(nullptr,IDC_ARROW));return TRUE;}
            break;
        case WM_NCDESTROY:
            self->hintTimers_.StopAll();if(self->hintFont_)DeleteObject(self->hintFont_);
            self->hintFont_=nullptr;self->fontDpi_=0;self->window_=nullptr;self->hint_=false;self->blocked_=false;
            return DefWindowProcW(window,message,word,param);
        }
        return DefWindowProcW(window,message,word,param);
    }
public:
    bool Active()const noexcept{return IsWindow(window_)!=FALSE;}
    HWND Window()const noexcept{return window_;}
    RECT Monitor()const noexcept{return monitor_;}
    const Options& SessionOptions()const noexcept{return options_;}
    bool HintVisible()const noexcept{return hint_;}
    bool PaintFailed()const noexcept{return paintFailed_;}
    bool InputBlocked()const noexcept{return blocked_;}
#ifdef ZOOMIT_TESTING
    UINT_PTR HintTimerId()const noexcept{return hintTimers_.Id(0);}
#endif
    DWORD Open(HINSTANCE instance,HWND controller,UINT changedMessage,RECT monitor,const Options& options) noexcept {
        if(Active() || !ValidOptions(options))return ERROR_INVALID_PARAMETER;
        const int64_t width=static_cast<int64_t>(monitor.right)-monitor.left,height=static_cast<int64_t>(monitor.bottom)-monitor.top;
        if(width<=0 || height<=0 || width>32768 || height>32768)return ERROR_INVALID_PARAMETER;
        options_=options;monitor_=monitor;controller_=controller;changedMessage_=changedMessage;paintFailed_=false;
        blocked_=hint_=false;wheelRemainder_=0;previousForeground_=GetForegroundWindow();
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=Proc;cls.hInstance=instance;cls.hCursor=LoadCursor(nullptr,IDC_ARROW);
        cls.hIcon=LoadIcon(instance,L"APPICON");cls.hIconSm=cls.hIcon;cls.lpszClassName=L"ZoomItCustomWhiteboard";
        if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return GetLastError();
        HWND created=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls.lpszClassName,L"ZoomIt Custom Whiteboard",WS_POPUP,
            monitor.left,monitor.top,static_cast<int>(width),static_cast<int>(height),nullptr,nullptr,instance,this);
        if(!created)return GetLastError()?GetLastError():ERROR_NOT_ENOUGH_MEMORY;
        ShowWindow(created,SW_SHOW);UpdateWindow(created);SetForegroundWindow(created);SetActiveWindow(created);
        return paintFailed_?ERROR_GEN_FAILURE:ERROR_SUCCESS;
    }
    void Close(bool restoreFocus=true)noexcept {
        if(Active())DestroyWindow(window_);
        HWND previous=previousForeground_;previousForeground_=nullptr;
        if(restoreFocus && IsWindow(previous))SetForegroundWindow(previous);
    }
    void SetInputBlocked(bool value)noexcept {
        blocked_=value;
        if(value && hint_){hint_=false;hintTimers_.Stop(0);InvalidateRect(window_,nullptr,FALSE);}
        if(value)wheelRemainder_=0;
    }
    void UpdateHotkeys(DWORD toggle,DWORD background)noexcept {
        options_.toggleKey=toggle;options_.backgroundKey=background;
        if(hint_)InvalidateRect(window_,nullptr,FALSE);
    }
    void ShowExitHint()noexcept {
        if(!Active() || blocked_)return;
        hint_=true;InvalidateRect(window_,nullptr,FALSE);
        if(!hintTimers_.Start(window_,0,3000)){hint_=false;InvalidateRect(window_,nullptr,FALSE);}
    }
};
}
