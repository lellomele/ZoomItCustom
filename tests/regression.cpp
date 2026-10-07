#include "../src/Zoomit.cpp"
#include <iostream>
#include <psapi.h>
#include <chrono>
#include <stdexcept>
#include <functional>
#include "fixture.h"
void require(bool condition, const char* message) {
    RethrowTestCallbackFailure();
    if(!condition) throw std::runtime_error(message);
}
void pump(DWORD milliseconds) {
    const auto end=GetTickCount64()+milliseconds;
    MSG msg{};
    do {
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message!=WM_QUIT) { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
        Sleep(1);
    } while(GetTickCount64()<end);
    RethrowTestCallbackFailure();
}
void ActivateTestHost(HWND host) {
    // Maintain an owned background under active app windows. Raising the host must
    // never cover or steal focus from an active canvas and trigger its normal focus-loss exit.
    constexpr UINT positionFlags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE;
    SetWindowPos(host,HWND_TOPMOST,0,0,0,0,positionFlags);
    const bool canvasVisible=IsWindowVisible(g_hWndMain)!=FALSE;
    if(canvasVisible) SetWindowPos(g_hWndMain,HWND_TOPMOST,0,0,0,0,positionFlags);
    if(IsWindowVisible(g_hWndLiveZoom)) SetWindowPos(g_hWndLiveZoom,HWND_TOPMOST,0,0,0,0,positionFlags);
    SetForegroundWindow(canvasVisible ? g_hWndMain : host);
}
size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb=sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
    return counters.PrivateUsage;
}
// Last successful MagShowSystemCursor request, not a compositor/scanout measurement.
bool systemCursorShown = true;
bool denyInputTransform = false;
// The privileged fullscreen input transform is simulated for state-machine tests.
// Native window magnification uses the real APIs; a separate case exercises access denial.
BOOL WINAPI TestInputTransform(BOOL enabled, RECT*, RECT*) {
    if (enabled && denyInputTransform) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
BOOL WINAPI TestShowSystemCursor(BOOL show) {
    const BOOL result=MagShowSystemCursor(show);
    if(result) systemCursorShown=show!=FALSE;
    return result;
}
// Fixture reset followed by a read. This repairs cursor state and must never be
// used as evidence for stationary exit visibility. See ObserveNativeCursor.
CURSORINFO ResetThenReadOwnedNormalPointer(HWND host,const char* context) {
    ActivateTestHost(host);
    POINT position{120,120};ClientToScreen(host,&position);SetCursorPos(position.x,position.y);
    // Process the owned window's normal cursor event synchronously, preserving the
    // immediate toggle tests without depending on another application's hover cursor.
    SendMessage(host,WM_SETCURSOR,reinterpret_cast<WPARAM>(host),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
    CURSORINFO cursor{};
    const auto probe=[&] {cursor={sizeof(CURSORINFO)};return GetCursorInfo(&cursor);};
    BOOL read=probe();DWORD error=read ? ERROR_SUCCESS : GetLastError();
    // Windows may temporarily replace a visible arrow with its process-startup feedback.
    // A native query can also fail transiently while that known feedback ends.
    // Never retry a successfully read hidden or drawing cursor.
    const HCURSOR startup=LoadCursor(nullptr,IDC_APPSTARTING);
    bool startupObserved=read && (cursor.flags&CURSOR_SHOWING) && cursor.hCursor==startup;
    const ULONGLONG feedbackDeadline=GetTickCount64()+3500;
    while(GetTickCount64()<feedbackDeadline &&
          ((read && (cursor.flags&CURSOR_SHOWING) && cursor.hCursor==startup) ||
           (!read && startupObserved && error==ERROR_INVALID_PARAMETER))) {
        pump(10);
        SendMessage(host,WM_SETCURSOR,reinterpret_cast<WPARAM>(host),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
        read=probe();error=read ? ERROR_SUCCESS : GetLastError();
        startupObserved|=read && (cursor.flags&CURSOR_SHOWING) && cursor.hCursor==startup;
    }
    if(!read || !(cursor.flags&CURSOR_SHOWING) || cursor.hCursor!=LoadCursor(nullptr,IDC_ARROW)) {
        POINT actual{};GetCursorPos(&actual);const HWND surface=WindowFromPoint(actual);
        wchar_t surfaceClass[96]{};GetClassNameW(surface,surfaceClass,_countof(surfaceClass));
        std::cerr<<"Owned normal pointer mismatch: context="<<context<<" read="<<read<<" error="<<error
                 <<" flags="<<cursor.flags<<" cursor="<<cursor.hCursor<<" expected="<<LoadCursor(nullptr,IDC_ARROW)
                 <<" last_mag_visibility_request="<<systemCursorShown<<" foreground="<<GetForegroundWindow()<<" capture="<<GetCapture()
                 <<" mode="<<(IsWindow(g_hWndMain) ? SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0) : 0)
                 <<" pointer="<<actual.x<<","<<actual.y<<" surface="<<surface<<" host="<<host<<"\n";
        std::wcerr<<L"Owned normal pointer surface: "<<surfaceClass<<L"\n";
    }
    require(read!=FALSE,context);
    return cursor;
}
// Only alter queried geometry; no test changes the physical display topology.
bool g_TestChangedMonitorGeometry{};
BOOL WINAPI ScenarioMonitorInfo(HMONITOR monitor,LPMONITORINFO information) {
    if (!GetMonitorInfoA(monitor,information)) return FALSE;
    if (g_TestChangedMonitorGeometry) {
        information->rcMonitor.right-=64;
        information->rcWork.right=(std::min)(information->rcWork.right,information->rcMonitor.right);
    }
    return TRUE;
}

bool cancelSnip = false;
void CALLBACK SelectTestRegion(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND selection = FindWindow(L"ZoomitSelectRectangle",nullptr);
    DWORD owner{};
    if (!selection || !GetWindowThreadProcessId(selection,&owner) || owner!=GetCurrentProcessId()) return;
    KillTimer(nullptr,timer);
    if (cancelSnip) SendMessage(selection,WM_KEYDOWN,VK_ESCAPE,0);
    else {
        SendMessage(selection,WM_LBUTTONDOWN,0,MAKELPARAM(20,20));
        SendMessage(selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(100,100));
        SendMessage(selection,WM_LBUTTONUP,0,MAKELPARAM(100,100));
    }
}

bool optionsValid = true;
bool aboutCaptured=false;
const wchar_t* aboutCapturePath=nullptr;
bool captureLiveOptions=false;
bool captureDrawOptions=false;
bool verifyAnimationCancel=false;
bool saveAnimationOptions=false;
bool saveLiveAnimation=false;
bool saveZoomAnimation=false;
INT_PTR TestOptionsProcImpl(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == 97) {
        KillTimer(dialog, 97);
        HWND tabs=GetDlgItem(dialog,IDC_TAB);
        const int capturedPage=captureDrawOptions ? DRAW_PAGE :
            (captureLiveOptions || verifyAnimationCancel || saveAnimationOptions) ? LIVE_PAGE : ABOUT_PAGE;
        TabCtrl_SetCurSel(tabs,capturedPage);
        NMHDR notification{tabs,IDC_TAB,TCN_SELCHANGE};
        SendMessage(dialog,WM_NOTIFY,IDC_TAB,reinterpret_cast<LPARAM>(&notification));
        optionsValid &= IsWindowVisible(g_OptionsTabs[capturedPage].hPage)!=FALSE;
        if(saveAnimationOptions) {
            CheckDlgButton(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ANIMATE_LIVE_ZOOM,
                           saveLiveAnimation ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(g_OptionsTabs[ZOOM_PAGE].hPage,IDC_ANIMATE_ZOOM,
                           saveZoomAnimation ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog,IDC_SHOW_TRAY_ICON,BST_UNCHECKED);
        }
        if(verifyAnimationCancel) {
            const BOOLEAN live=g_AnimateLiveZoom, zoom=g_AnimateZoom;
            HWND livePage=g_OptionsTabs[LIVE_PAGE].hPage;
            const UINT zoomCheck=IsDlgButtonChecked(g_OptionsTabs[ZOOM_PAGE].hPage,IDC_ANIMATE_ZOOM);
            SendMessage(GetDlgItem(livePage,IDC_ANIMATE_LIVE_ZOOM),BM_CLICK,0,0);
            optionsValid &= IsDlgButtonChecked(livePage,IDC_ANIMATE_LIVE_ZOOM)==
                (live ? BST_UNCHECKED : BST_CHECKED);
            optionsValid &= IsDlgButtonChecked(g_OptionsTabs[ZOOM_PAGE].hPage,IDC_ANIMATE_ZOOM)==zoomCheck &&
                g_AnimateLiveZoom==live && g_AnimateZoom==zoom;
        }
        if (!aboutCaptured && aboutCapturePath && (!verifyAnimationCancel || captureLiveOptions)) {
            RedrawWindow(dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            RECT window{};GetWindowRect(dialog,&window);
            HDC screen=GetDC(nullptr), memory=CreateCompatibleDC(screen);
            HBITMAP image=CreateCompatibleBitmap(screen,window.right-window.left,window.bottom-window.top);
            SelectObject(memory,image);
            PrintWindow(dialog,memory,PW_RENDERFULLCONTENT);
            require(SavePng(const_cast<wchar_t*>(aboutCapturePath),image)==ERROR_SUCCESS,"Save About verification image");
            DeleteDC(memory);DeleteObject(image);ReleaseDC(nullptr,screen);
            aboutCaptured=true;
        }
        SendMessage(dialog, WM_COMMAND, saveAnimationOptions ? IDOK : IDCANCEL, 0);
        return TRUE;
    }
    auto result = OptionsProc(dialog, message, wParam, lParam);
    if (message == WM_INITDIALOG) {
        optionsValid &= TabCtrl_GetItemCount(GetDlgItem(dialog, IDC_TAB)) == 6;
        for (auto& page : g_OptionsTabs) optionsValid &= IsWindow(page.hPage) != FALSE;
        wchar_t animationLabel[96]{};
        GetDlgItemTextW(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ANIMATE_LIVE_ZOOM,
                       animationLabel,_countof(animationLabel));
        optionsValid &= IsWindow(GetDlgItem(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ANIMATE_LIVE_ZOOM)) &&
            wcscmp(animationLabel,L"Animate zoom in and zoom out:")==0 &&
            IsDlgButtonChecked(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ANIMATE_LIVE_ZOOM)==
                (g_AnimateLiveZoom ? BST_CHECKED : BST_UNCHECKED) &&
            IsDlgButtonChecked(g_OptionsTabs[ZOOM_PAGE].hPage,IDC_ANIMATE_ZOOM)==
                (g_AnimateZoom ? BST_CHECKED : BST_UNCHECKED);
        wchar_t title[128]{}, version[64]{}, copyright[256]{}, lastTab[32]{};
        GetWindowText(dialog,title,_countof(title));
        optionsValid &= wcscmp(title,L"ZoomIt Custom " _T(FILE_VERSION_STRING))==0;
        TCITEM item{};item.mask=TCIF_TEXT;item.pszText=lastTab;item.cchTextMax=_countof(lastTab);
        TabCtrl_GetItem(GetDlgItem(dialog,IDC_TAB),ABOUT_PAGE,&item);
        optionsValid &= wcscmp(lastTab,L"About")==0;
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_VERSION,version,_countof(version));
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_COPYRIGHT,copyright,_countof(copyright));
        optionsValid &= wcscmp(version,L"Version " _T(FILE_VERSION_STRING))==0 &&
                        wcsstr(copyright,L"Prof. ing. Raffaele Mele")!=nullptr;
        optionsValid &= IsWindow(GetDlgItem(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_REPOSITORY)) &&
                        IsWindow(GetDlgItem(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_LICENSE));
        wchar_t supervision[96]{};
        GetDlgItemTextW(dialog,IDC_SUPERVISION_STATUS,supervision,_countof(supervision));
        optionsValid &= wcscmp(supervision,L"Sessione: esecuzione autonoma")==0 &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_OFF)) &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_NORMAL)) &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_SUPERVISED));
        SetTimer(dialog, 97, 35, nullptr);
    }
    return result;
}

INT_PTR CALLBACK TestOptionsProc(HWND dialog,UINT message,WPARAM word,LPARAM param) noexcept {
    return TestDialogBoundary(dialog,[&]{return TestOptionsProcImpl(dialog,message,word,param);});
}

struct LiveAnimationResults {
    size_t immediateCases{}, animatedCases{}, reverseEvents{}, setZoomCases{}, observedFrames{};
    size_t drawCases{}, snipCases{}, resetCases{}, optionsCases{}, registryCases{}, zoomKeyCases{}, leakCycles{};
    DWORD gdiBefore{}, gdiAfter{}, userBefore{}, userAfter{};
};

LiveAnimationResults RunLiveAnimationRegression(HWND host) {
    LiveAnimationResults results{};
    struct PreferenceSnapshot {PVOID value;std::vector<BYTE> bytes;};
    std::vector<PreferenceSnapshot> preferences;
    for(const auto& setting:RegSettings) {
        if(!setting.ValueName || !setting.Setting) continue;
        size_t size=setting.Size;
        switch(setting.Type) {
        case SETTING_TYPE_DWORD:size=sizeof(DWORD);break;
        case SETTING_TYPE_BOOLEAN:size=sizeof(BOOLEAN);break;
        case SETTING_TYPE_DOUBLE:size=sizeof(double);break;
        case SETTING_TYPE_WORD:size=sizeof(WORD);break;
        default:break;
        }
        const auto bytes=static_cast<const BYTE*>(setting.Setting);
        preferences.push_back({setting.Setting,std::vector<BYTE>(bytes,bytes+size)});
    }
    const DWORD modifiers[]{g_ToggleMod,g_LiveZoomToggleMod,g_DrawToggleMod,g_BreakToggleMod,g_SnipToggleMod};
    const DWORD savedPenWidth=g_PenWidth;
    const BOOLEAN savedPointer=g_DrawPointer,savedPenDown=g_PenDown,savedPenInverted=g_PenInverted;
    auto restorePreferences=zoomit::OnExit([&] {
        for(const auto& setting:preferences)
            memcpy(setting.value,setting.bytes.data(),setting.bytes.size());
        g_ToggleMod=modifiers[0];g_LiveZoomToggleMod=modifiers[1];g_DrawToggleMod=modifiers[2];
        g_BreakToggleMod=modifiers[3];g_SnipToggleMod=modifiers[4];
        g_PenWidth=savedPenWidth;g_DrawPointer=savedPointer;g_PenDown=savedPenDown;g_PenInverted=savedPenInverted;
    });
    const BOOLEAN savedZoomAnimation=g_AnimateZoom, savedLiveAnimation=g_AnimateLiveZoom;
    const DWORD savedSlider=g_SliderZoomLevel;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    g_SliderZoomLevel=11;
    const float target=g_ZoomLevels[g_SliderZoomLevel];
    auto mode=[] {return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
    auto level=[] {
        require(IsWindow(g_hWndLiveZoom),"LiveZoom window must exist before reading its magnification");
        const auto value=reinterpret_cast<const float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
        require(value!=nullptr && std::isfinite(*value),"LiveZoom must expose a finite zoom factor");
        return *value;
    };
    auto nativeLevel=[&] {
        float actual{};
        if(g_fullScreenWorkaround) {
            int x{},y{};
            require(MagGetFullscreenTransform(&actual,&x,&y)!=FALSE,"Read real fullscreen magnification");
        } else {
            MAGTRANSFORM transform{};
            require(MagGetWindowTransform(g_hWndLiveZoomMag,&transform)!=FALSE,"Read real native magnifier transform");
            actual=transform.v[0][0];
        }
        const float logical=level();
        if(std::fabs(actual-logical)>=0.0001f)
            std::cerr<<"Native zoom mismatch: actual="<<actual<<" state="<<logical
                     <<" fullscreen="<<g_fullScreenWorkaround<<" window="<<g_hWndLiveZoom
                     <<" visible="<<IsWindowVisible(g_hWndLiveZoom)<<"\n";
        require(std::fabs(actual-logical)<0.0001f,"Visible native magnification must match the LiveZoom state");
    };
    auto waitUntil=[&](auto complete,const char* message) {
        const ULONGLONG deadline=GetTickCount64()+2500;
        while(!complete() && GetTickCount64()<deadline) pump(5);
        require(complete(),message);
    };
    auto resetAndCheckLogicalCursor=[&] {
        ActivateTestHost(host);SetCursorPos(125,125);pump(10);
        CURSORINFO cursor{sizeof(cursor)};
        require(GetCursorInfo(&cursor) && (cursor.flags&CURSOR_SHOWING) &&
                cursor.hCursor==LoadCursor(nullptr,IDC_ARROW) && systemCursorShown,
                "Fixture-reset animated and immediate exits must expose the logical arrow and a successful system-cursor visibility request");
    };
    auto toggle=[&] {SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);};
    auto settle=[&] {
        waitUntil([&] {return IsWindow(g_hWndLiveZoom) && level()==target;},
                  "Animated LiveZoom entry must settle at the selected zoom factor");
        nativeLevel();
    };
    auto observeProgress=[&](float previous,bool increasing,float lower,float upper,const char* context) {
        struct Sample {UINT message;ULONGLONG elapsed;float factor;bool alive,visible;};
        std::array<Sample,16> samples{};size_t count{};
        const HWND expected=g_hWndLiveZoom;const ULONGLONG started=GetTickCount64(),deadline=started+2500;
        bool progressed=false;
        // Observe after each dispatched event. A slow native paint cannot cause
        // this fixture to exhaust an entire animation before sampling its frame.
        while(GetTickCount64()<deadline && IsWindow(expected) && g_hWndLiveZoom==expected) {
            MSG message{};
            if(PeekMessage(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessage(&message);}
                RethrowTestCallbackFailure();
                const bool alive=IsWindow(expected) && g_hWndLiveZoom==expected;
                const float factor=alive ? level() : 0;
                const bool visible=alive && IsWindowVisible(expected);
                samples[count++%samples.size()]={message.message,GetTickCount64()-started,factor,alive,visible};
                if(visible && factor>lower && factor<upper && (increasing ? factor>previous : factor<previous)) {
                    nativeLevel();progressed=true;++results.observedFrames;break;
                }
            } else Sleep(1);
        }
        if(!progressed) {
            std::cerr<<"Animation observation failed: "<<context<<" fullscreen="<<g_fullScreenWorkaround
                     <<" previous="<<previous<<" bounds="<<lower<<","<<upper<<" elapsed_ms="<<GetTickCount64()-started<<"\n";
            const size_t available=(std::min)(count,samples.size());
            for(size_t i=count-available;i<count;++i) {
                const auto& sample=samples[i%samples.size()];
                std::cerr<<"  message="<<sample.message<<" elapsed_ms="<<sample.elapsed<<" factor="<<sample.factor
                         <<" alive="<<sample.alive<<" visible="<<sample.visible<<"\n";
            }
        }
        require(progressed,context);
    };
    auto beginExit=[&] {
        const float before=level();toggle();
        require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<before,
                "Checked LiveZoom exit must synchronously publish an intermediate factor without instantly closing");
        nativeLevel();const float firstExitFrame=level();
        observeProgress(firstExitFrame,false,1,before,"LiveZoom exit must progress through another real intermediate native frame");
    };
    auto close=[&] {
        toggle();
        waitUntil([] {return !IsWindow(g_hWndLiveZoom);},"Animated LiveZoom exit must finish and destroy the magnifier");
        resetAndCheckLogicalCursor();
    };
    auto prepare=[&](bool fullscreen) {
        require(!IsWindow(g_hWndLiveZoom) && !(mode()&1),"Animation cases must start from the desktop");
        g_fullScreenWorkaround=fullscreen;
        ActivateTestHost(host);SetCursorPos(125,125);pump(10);
    };

    // Copy the real production entries into an isolated key; never write user preferences.
    {
        const std::wstring key=L"Software\\ZoomItCustom\\AnimationRegression_"+std::to_wstring(GetCurrentProcessId());
        struct Cleanup {std::wstring path;~Cleanup(){RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());}} cleanup{key};
        REG_SETTING table[3]{};
        for(auto& setting:RegSettings) {
            if(setting.ValueName && setting.Setting==&g_AnimateZoom) table[0]=setting;
            if(setting.ValueName && setting.Setting==&g_AnimateLiveZoom) table[1]=setting;
        }
        require(table[0].ValueName && table[1].ValueName &&
                table[1].Type==SETTING_TYPE_BOOLEAN && table[1].DefaultSetting==0,
                "LiveZoom animation must have its own persisted setting, disabled by default");
        ClassRegistry settings(key.c_str());
        g_AnimateZoom=FALSE;g_AnimateLiveZoom=TRUE;settings.ReadRegSettings(table);
        require(g_AnimateZoom && !g_AnimateLiveZoom,"Missing LiveZoom setting must preserve the independent legacy defaults");
        ++results.registryCases;
        for(bool live:{false,true}) {
            g_AnimateZoom=!live;g_AnimateLiveZoom=live;settings.WriteRegSettings(table);
            g_AnimateZoom=live;g_AnimateLiveZoom=!live;settings.ReadRegSettings(table);
            require((g_AnimateZoom!=FALSE)==!live && (g_AnimateLiveZoom!=FALSE)==live,
                    "Zoom and LiveZoom animation preferences must persist independently");
            ++results.registryCases;
        }
        HKEY handle{};
        require(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE,&handle)==ERROR_SUCCESS,
                "Open isolated animation registry fixture");
        const wchar_t bad[]=L"invalid";
        const LSTATUS malformed=RegSetValueExW(handle,table[1].ValueName,0,REG_SZ,
            reinterpret_cast<const BYTE*>(bad),sizeof(bad));
        RegCloseKey(handle);
        require(malformed==ERROR_SUCCESS,"Write malformed isolated animation fixture");
        g_AnimateLiveZoom=TRUE;settings.ReadRegSettings(table);
        require(!g_AnimateLiveZoom,"Invalid LiveZoom animation preference must fall back to its safe default");
        ++results.registryCases;
    }
    verifyAnimationCancel=true;
    for(bool live:{false,true}) {
        g_AnimateLiveZoom=live;g_AnimateZoom=!live;
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
        require((g_AnimateLiveZoom!=FALSE)==live && (g_AnimateZoom!=FALSE)==!live,
                "Cancel must leave both animation preferences unchanged");
        ++results.optionsCases;
    }
    verifyAnimationCancel=false;
    require(optionsValid,"LiveZoom animation checkbox must initialize, toggle and cancel independently of Zoom");

    // Exercise the actual OK/persistence path with all HKCU operations redirected
    // inside this process. Startup and application preferences stay in the fixture.
    {
        // Options persistence stays inside the process-wide HKCU fixture.
        const DWORD keys[]{g_ToggleKey,g_LiveZoomToggleKey,g_DrawToggleKey,g_BreakToggleKey,g_SnipToggleKey};
        const BOOLEAN tray=g_ShowTrayIcon;
        auto restore=zoomit::OnExit([&] {
            g_TestMode=true;saveAnimationOptions=false;
            g_ToggleKey=keys[0];g_LiveZoomToggleKey=keys[1];g_DrawToggleKey=keys[2];
            g_BreakToggleKey=keys[3];g_SnipToggleKey=keys[4];g_ShowTrayIcon=tray;
        });
        g_ToggleKey=g_LiveZoomToggleKey=g_DrawToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
        for(bool enabled:{false,true}) {
            g_AnimateLiveZoom=!enabled;g_AnimateZoom=enabled;
            saveAnimationOptions=true;saveLiveAnimation=enabled;saveZoomAnimation=!enabled;
            // Enable real persistence only while HKCU is process-locally isolated.
            g_TestMode=false;
            DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
            g_TestMode=true;saveAnimationOptions=false;
            require((g_AnimateLiveZoom!=FALSE)==enabled && (g_AnimateZoom!=FALSE)==!enabled,
                    "Options OK must save the two independent animation checkboxes");
            ++results.optionsCases;
            g_AnimateLiveZoom=!enabled;g_AnimateZoom=enabled;reg.ReadRegSettings(RegSettings);
            require((g_AnimateLiveZoom!=FALSE)==enabled && (g_AnimateZoom!=FALSE)==!enabled,
                    "Options OK must persist both preferences in the actual application setting table");
            ++results.registryCases;
            DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
            require((g_AnimateLiveZoom!=FALSE)==enabled && (g_AnimateZoom!=FALSE)==!enabled,
                    "Reopened Options must retain the saved LiveZoom animation preference");
            ++results.optionsCases;
        }
    }
    require(optionsValid,"Saving and reopening LiveZoom animation options must preserve independent checkbox states");

    for(bool fullscreen:{false,true}) {
        for(bool zoomAnimation:{false,true}) {
            prepare(fullscreen);g_AnimateZoom=zoomAnimation;g_AnimateLiveZoom=FALSE;
            toggle();
            require(IsWindowVisible(g_hWndLiveZoom) && level()==target,
                    "Unchecked LiveZoom animation must apply selected zoom immediately, independently of Zoom");
            nativeLevel();
            SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
            require(level()==8,"Unchecked LiveZoom animation must also change the factor immediately");
            nativeLevel();
            for(int press=0;press<3;++press) SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
            require(level()==ZOOM_LEVEL_MAX,"Repeated immediate zoom-in must clamp to the supported maximum");
            for(int press=0;press<3;++press) SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
            require(level()==target,"Immediate zoom-out must follow the selected factor without changing Zoom's preference");
            ++results.zoomKeyCases;
            toggle();
            require(!IsWindow(g_hWndLiveZoom),"Unchecked LiveZoom animation must close synchronously");
            pump(35);require(!IsWindow(g_hWndLiveZoom),"Immediate exit must not leave a delayed animation callback");
            resetAndCheckLogicalCursor();++results.immediateCases;
        }
        prepare(fullscreen);g_AnimateZoom=FALSE;g_AnimateLiveZoom=TRUE;
        toggle();
        require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<target,
                "Checked LiveZoom animation must visibly enter through an intermediate zoom factor");
        nativeLevel();
        const float first=level();
        observeProgress(first,true,1,target,"LiveZoom entry must progress through another real intermediate native frame");
        settle();beginExit();
        waitUntil([] {return !IsWindow(g_hWndLiveZoom);},"Animated exit must reach the desktop");
        resetAndCheckLogicalCursor();++results.animatedCases;

        prepare(fullscreen);toggle();settle();
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
        require(level()>target && level()<8,"Checked zoom-in commands must advance through a real intermediate factor");
        nativeLevel();
        waitUntil([&] {return IsWindow(g_hWndLiveZoom) && level()==8;},
                  "Animated zoom-in command must settle at twice its logical factor");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        require(level()>target && level()<8,"Checked zoom-out commands must animate toward the lower factor");
        nativeLevel();settle();++results.zoomKeyCases;
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
        waitUntil([&] {return IsWindow(g_hWndLiveZoom) && level()==16;},
                  "Rapid animated zoom commands must accumulate the requested target instead of intermediate frames");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        settle();++results.zoomKeyCases;
        beginExit();
        require(IsWindow(g_hWndLiveZoom),"Zoom command reversal must begin during animated exit");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
        waitUntil([&] {return IsWindow(g_hWndLiveZoom) && level()==8;},
                  "Zoom-in during animated exit must resume from the saved logical factor");
        nativeLevel();close();++results.zoomKeyCases;

        // Reverse an exit without replacing its window or retaining a stale destroy request.
        prepare(fullscreen);toggle();settle();
        const HWND exiting=g_hWndLiveZoom;beginExit();
        require(IsWindow(exiting) && level()<target,"Reversal case must start during a real exit animation");
        toggle();++results.reverseEvents;
        require(g_hWndLiveZoom==exiting && IsWindowVisible(exiting),
                "A repeated LiveZoom toggle must reverse the current exit in the same window");
        settle();pump(120);
        require(g_hWndLiveZoom==exiting && level()==target,"A reversed exit must not destroy the reactivated magnifier");
        close();
        prepare(fullscreen);toggle();settle();
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        require(level()>3.75f && level()<target,"Different-target reversal must start during animated factor reduction");
        beginExit();toggle();++results.reverseEvents;
        waitUntil([&] {return IsWindow(g_hWndLiveZoom) && level()==3.75f;},
                  "Exit reversal must restore the requested lower factor instead of the intermediate frame");
        nativeLevel();close();
        for(int delay:{0,7,30}) {
            prepare(fullscreen);
            for(int press=0;press<9;++press) {toggle();++results.reverseEvents;if(delay) pump(delay);}
            settle();
            require(IsWindowVisible(g_hWndLiveZoom),"An odd rapid toggle sequence must finish in LiveZoom");
            close();
            require(!IsWindow(g_hWndLiveZoom),"The final opposite toggle must finish on the desktop");
        }

        // Recovery and Draw use this production message to install an exact, stable factor.
        for(bool exitingPhase:{false,true}) {
            prepare(fullscreen);toggle();
            if(exitingPhase) {settle();beginExit();}
            else {require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<target,"Transition operation must begin during a real entry frame");nativeLevel();}
            const HWND live=g_hWndLiveZoom;
            const float requested=exitingPhase ? 2.5f : 2.25f;
            SendMessage(live,WM_USER_SET_ZOOM,EncodeZoomLevel(requested),0);pump(450);
            require(g_hWndLiveZoom==live && IsWindowVisible(live) && level()==requested,
                    "Installing an exact zoom factor must cancel pending entry or exit animation");
            nativeLevel();close();++results.setZoomCases;
        }
        for(bool liveDraw:{false,true}) {
            for(bool exitingPhase:{false,true}) {
                prepare(fullscreen);toggle();
                if(exitingPhase) {settle();beginExit();}
            else {require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<target,"Transition operation must begin during a real entry frame");nativeLevel();}
                SendMessage(g_hWndMain,WM_HOTKEY,liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0);pump(60);
                require(IsWindowVisible(g_hWndMain) && (mode()&3)==3,
                        "Draw entered during a transition must activate its drawing canvas");
                SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(150,150));pump(150);
                require(IsWindowVisible(g_hWndMain) && (mode()&1),
                        "A pending animated exit must not tear down active Draw");
                if(liveDraw) require(IsWindowVisible(g_hWndLiveZoom) && level()==target,
                                     "Layered LiveDraw must retain a stable native LiveZoom factor");
                SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(100);
                require(IsWindowVisible(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),
                        "Exiting Draw entered during a transition must resume LiveZoom");
                require(level()==target,"Restored LiveZoom must keep its intended pre-Draw factor");
                close();++results.drawCases;
            }
        }
        for(bool exitingPhase:{false,true}) {
            prepare(fullscreen);toggle();
            if(exitingPhase) {settle();beginExit();}
            else {require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<target,"Transition operation must begin during a real entry frame");nativeLevel();}
            cancelSnip=false;SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);pump(60);
            require(g_TestSnipBitmap && IsWindowVisible(g_hWndLiveZoom) && !(mode()&1) && level()==target,
                    "Snip during entry or exit must capture and resume the intended LiveZoom factor");
            DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;
            close();++results.snipCases;
        }
        for(bool exitingPhase:{false,true}) {
            prepare(fullscreen);toggle();
            if(exitingPhase) {settle();beginExit();}
            else {require(IsWindowVisible(g_hWndLiveZoom) && level()>1 && level()<target,"Transition operation must begin during a real entry frame");nativeLevel();}
            SendMessage(g_hWndMain,recovery::ResetMessage,0,0);
            require(!IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain) && !(mode()&1),
                    "Forced runtime recovery must cancel animations and return to the desktop synchronously");
            pump(150);
            require(!IsWindow(g_hWndLiveZoom),"Recovery must not leave a timer that recreates LiveZoom");
            resetAndCheckLogicalCursor();++results.resetCases;
        }
    }
    prepare(false);toggle();settle();close();pump(75);
    results.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    results.userBefore=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    const size_t privateBefore=privateBytes();
    struct ResourceReading {DWORD gdi,user;};
    std::vector<ResourceReading> cycleResources;
    wchar_t diagnosticFlag[2]{};
    const bool resourceDiagnostics=GetEnvironmentVariableW(L"ZOOMIT_TEST_ANIMATION_RESOURCE_DIAGNOSTICS",
                                                            diagnosticFlag,_countof(diagnosticFlag))!=0;
    for(int cycle=0;cycle<8;++cycle) {
        prepare((cycle%2)!=0);toggle();settle();beginExit();toggle();settle();close();
        ++results.leakCycles;
        const ResourceReading reading{GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),
                                      GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)};
        cycleResources.push_back(reading);
        if(resourceDiagnostics) std::cerr<<"Animation resources cycle="<<cycle<<" gdi="<<reading.gdi
                                        <<" user="<<reading.user<<"\n";
    }
    const auto readResources=[&] {
        results.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        results.userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    };
    const auto withinBudget=[&] {
        return results.gdiAfter<=results.gdiBefore+1 && results.userAfter<=results.userBefore+1;
    };
    readResources();
    const DWORD immediateGdi=results.gdiAfter,immediateUser=results.userAfter;
    const ULONGLONG drainStarted=GetTickCount64(),drainDeadline=drainStarted+500;
    while(!withinBudget() && GetTickCount64()<drainDeadline) {pump(10);readResources();}
    if(!withinBudget() || resourceDiagnostics) {
        std::cerr<<"Animation resource summary: baseline_gdi="<<results.gdiBefore
                 <<" baseline_user="<<results.userBefore<<" immediate_gdi="<<immediateGdi
                 <<" immediate_user="<<immediateUser<<" final_gdi="<<results.gdiAfter
                 <<" final_user="<<results.userAfter<<" drain_ms="<<(GetTickCount64()-drainStarted)
                 <<" private_before="<<privateBefore<<" private_after="<<privateBytes()
                 <<" live_window="<<g_hWndLiveZoom<<" live_visible="<<IsWindowVisible(g_hWndLiveZoom)
                 <<" main_mode="<<mode()<<" capture="<<GetCapture()<<"\n";
        for(size_t cycle=0;cycle<cycleResources.size();++cycle)
            std::cerr<<"Animation resource history: cycle="<<cycle<<" gdi="<<cycleResources[cycle].gdi
                     <<" user="<<cycleResources[cycle].user<<"\n";
    }
    require(withinBudget(),"Animated entry, reversed exit and teardown must not accumulate GDI or USER resources");
    g_AnimateZoom=savedZoomAnimation;g_AnimateLiveZoom=savedLiveAnimation;
    g_SliderZoomLevel=savedSlider;g_fullScreenWorkaround=savedFullscreen;
    return results;
}

void PrintLiveAnimationResults(const LiveAnimationResults& result) {
    std::cout<<"{\"passed\":true,\"cursor_check_scope\":\"logical-state-or-fixture-cleanup\",\"visual_cursor_verification\":false,\"immediate_cases\":"<<result.immediateCases
        <<",\"animated_cases\":"<<result.animatedCases<<",\"reverse_events\":"<<result.reverseEvents
        <<",\"set_zoom_cases\":"<<result.setZoomCases<<",\"observed_intermediate_native_frames\":"<<result.observedFrames<<",\"draw_transition_cases\":"<<result.drawCases
        <<",\"snip_transition_cases\":"<<result.snipCases<<",\"reset_transition_cases\":"<<result.resetCases
        <<",\"options_cases\":"<<result.optionsCases<<",\"registry_cases\":"<<result.registryCases
        <<",\"zoom_key_cases\":"<<result.zoomKeyCases<<",\"leak_cycles\":"<<result.leakCycles<<",\"gdi_before\":"<<result.gdiBefore
        <<",\"gdi_after\":"<<result.gdiAfter<<",\"user_before\":"<<result.userBefore
        <<",\"user_after\":"<<result.userAfter<<"}\n";
}


struct ModePolicyResults {
    size_t states{}, ignoredKeys{}, snipCases{}, modalKeys{}, registrationCases{}, animationCases{}, guiCases{}, resetCases{}, allocationCases{}, unavailableApiCases{}, duplicateExitCases{}, lateExitCases{}, penContactCases{}, sequenceCases{}, topologyCases{}, timerIdentityCases{}, stagedGraphicsCases{}, runtimeFailureCases{};
};
std::function<void()> g_PolicyModalCheck;
std::function<void()> g_PolicyOptionsCheck;
RECT g_PolicySelectionRect{20,20,100,100};
void CheckPolicySaveDialog() {
    TestCallbackBoundary(nullptr,0,[&]{if(g_PolicyModalCheck) g_PolicyModalCheck();});
}
void SelectPolicyRegionImpl(HWND window, UINT message, UINT_PTR timer, DWORD time) {
    HWND selection=FindWindow(L"ZoomitSelectRectangle",nullptr);
    DWORD owner{};
    if(!selection || !GetWindowThreadProcessId(selection,&owner) || owner!=GetCurrentProcessId()) return;
    KillTimer(nullptr,timer);
    if(g_PolicyModalCheck) g_PolicyModalCheck();
    if(cancelSnip) SendMessage(selection,WM_KEYDOWN,VK_ESCAPE,0);
    else {
        SendMessage(selection,WM_LBUTTONDOWN,0,MAKELPARAM(g_PolicySelectionRect.left,g_PolicySelectionRect.top));
        SendMessage(selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(g_PolicySelectionRect.right,g_PolicySelectionRect.bottom));
        SendMessage(selection,WM_LBUTTONUP,0,MAKELPARAM(g_PolicySelectionRect.right,g_PolicySelectionRect.bottom));
    }
}
INT_PTR PolicyOptionsProcImpl(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if(message==WM_TIMER && wParam==98) {
        KillTimer(dialog,98);
        if(g_PolicyOptionsCheck) g_PolicyOptionsCheck();
        if(g_PolicyModalCheck) g_PolicyModalCheck();
        SendMessage(dialog,WM_COMMAND,IDCANCEL,0);
        return TRUE;
    }
    const auto result=OptionsProc(dialog,message,wParam,lParam);
    if(message==WM_INITDIALOG) SetTimer(dialog,98,25,nullptr);
    return result;
}

void CALLBACK SelectPolicyRegion(HWND window,UINT message,UINT_PTR timer,DWORD time) noexcept {
    HWND selection=FindWindowW(L"ZoomitSelectRectangle",nullptr);
    DWORD owner{};if (!selection || !GetWindowThreadProcessId(selection,&owner) || owner!=GetCurrentProcessId()) return;
    TestCallbackBoundary(selection,WM_KEYDOWN,[&]{SelectPolicyRegionImpl(window,message,timer,time);});
}
INT_PTR CALLBACK PolicyOptionsProc(HWND dialog,UINT message,WPARAM word,LPARAM param) noexcept {
    return TestDialogBoundary(dialog,[&]{return PolicyOptionsProcImpl(dialog,message,word,param);});
}

ModePolicyResults RunModePolicyRegression(HWND host) {
    ModePolicyResults results{};
    const DWORD keys[]{g_ToggleKey,g_LiveZoomToggleKey,g_DrawToggleKey,g_BreakToggleKey,g_SnipToggleKey};
    const DWORD mods[]{g_ToggleMod,g_LiveZoomToggleMod,g_DrawToggleMod,g_BreakToggleMod,g_SnipToggleMod};
    const BOOLEAN savedZoomAnimation=g_AnimateZoom,savedLiveAnimation=g_AnimateLiveZoom;
    const DWORD savedSlider=g_SliderZoomLevel;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    auto restore=zoomit::OnExit([&] {
        UnregisterAllHotkeys(g_hWndMain);
        g_ToggleKey=keys[0];g_LiveZoomToggleKey=keys[1];g_DrawToggleKey=keys[2];
        g_BreakToggleKey=keys[3];g_SnipToggleKey=keys[4];
        g_ToggleMod=mods[0];g_LiveZoomToggleMod=mods[1];g_DrawToggleMod=mods[2];
        g_BreakToggleMod=mods[3];g_SnipToggleMod=mods[4];
        g_AnimateZoom=savedZoomAnimation;g_AnimateLiveZoom=savedLiveAnimation;
        g_SliderZoomLevel=savedSlider;g_fullScreenWorkaround=savedFullscreen;
        g_PolicyModalCheck={};g_PolicyOptionsCheck={};g_TestBeforeSaveDialog=nullptr;g_TestSavePath=nullptr;
        cancelSnip=false;
    });
    g_ToggleKey=(HOTKEYF_CONTROL<<8)|'1';g_LiveZoomToggleKey=(HOTKEYF_CONTROL<<8)|'2';
    g_DrawToggleKey=(HOTKEYF_CONTROL<<8)|'3';g_BreakToggleKey=(HOTKEYF_CONTROL<<8)|'4';
    g_SnipToggleKey=(HOTKEYF_CONTROL<<8)|'5';
    g_ToggleMod=g_LiveZoomToggleMod=g_DrawToggleMod=g_BreakToggleMod=g_SnipToggleMod=MOD_CONTROL;
    g_AnimateZoom=FALSE;g_AnimateLiveZoom=FALSE;g_SliderZoomLevel=11;
    g_BreakOnSecondary=FALSE;g_BreakShowBackgroundFile=FALSE;g_DrawPointer=FALSE;
    g_PenColor=COLOR_RED|0xff000000;g_RootPenWidth=5;g_PenWidth=5;
    const LONG_PTR hostBackground=GetClassLongPtrW(host,GCLP_HBRBACKGROUND);
    const LONG_PTR whiteBackground=reinterpret_cast<LONG_PTR>(GetStockObject(WHITE_BRUSH));
    SetClassLongPtrW(host,GCLP_HBRBACKGROUND,whiteBackground);
    const bool hostWasTopmost=(GetWindowLongPtrW(host,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
    auto restoreHostBackground=zoomit::OnExit([&] {
        SetClassLongPtrW(host,GCLP_HBRBACKGROUND,hostBackground);
        SetWindowPos(host,hostWasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    });
    require(GetClassLongPtrW(host,GCLP_HBRBACKGROUND)==whiteBackground,
            "Native test host must retain a deterministic white background through modal repaints");
    RedrawWindow(host,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
    require(g_GraphicsInit.Ready(),"Mode-policy graphics must use the application initializer");
    auto flushNativeFrames=[] {
        const auto flush=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"dwmapi.dll"),"DwmFlush"));
        const HRESULT first=flush ? flush() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        pump(50);
        const HRESULT second=flush ? flush() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        return std::pair<HRESULT,HRESULT>{first,second};
    };
    auto mode=[] {return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
    auto shortcut=[](WPARAM action) {
        switch(action) {
        case ZOOM_HOTKEY:return MAKELPARAM(MOD_CONTROL,'1');
        case LIVE_HOTKEY:return MAKELPARAM(MOD_CONTROL,'2');
        case DRAW_HOTKEY:return MAKELPARAM(MOD_CONTROL,'3');
        case LIVE_DRAW_HOTKEY:return MAKELPARAM(MOD_CONTROL|MOD_SHIFT,'3');
        case BREAK_HOTKEY:return MAKELPARAM(MOD_CONTROL,'4');
        case SNIP_HOTKEY:return MAKELPARAM(MOD_CONTROL,'5');
        default:return MAKELPARAM(MOD_CONTROL|MOD_SHIFT,'5');
        }
    };
    auto press=[&](WPARAM action) {SendMessage(g_hWndMain,WM_HOTKEY,action,shortcut(action));};
    struct Snapshot {
        LRESULT mode{},level{};
        RECT view{},liveView{};
        HWND live{},magnifier{},capture{};
        LONG_PTR style{};
        BOOL visible{},liveVisible{};
        HDC canvas{};
        HCURSOR ring{};
        float liveLevel{};
        std::vector<BYTE> pixels;
    };
    auto snapshot=[&] {
        Snapshot state{};
        state.mode=mode();state.level=SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,reinterpret_cast<LPARAM>(&state.view));
        state.live=g_hWndLiveZoom;state.magnifier=g_hWndLiveZoomMag;
        state.visible=IsWindowVisible(g_hWndMain);state.liveVisible=IsWindowVisible(g_hWndLiveZoom);
        state.style=GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE);state.capture=GetCapture();
        state.ring=reinterpret_cast<HCURSOR>(SendMessage(g_hWndMain,WM_TEST_QUERY_SUSPENDED_CURSOR,0,0));
        state.canvas=reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));
        if(IsWindow(state.live)) {
            state.liveLevel=*reinterpret_cast<float*>(SendMessage(state.live,WM_USER_GET_ZOOM_LEVEL,0,0));
            state.liveView=*reinterpret_cast<RECT*>(SendMessage(state.live,WM_USER_GET_SOURCE_RECT,0,0));
        }
        if(state.canvas && (state.mode&1)) {
            DrawingDib image(state.canvas,Gdiplus::Rect(300,300,180,100));
            require(image.pixels()!=nullptr,"Read preserved drawing pixels for mode policy");
            state.pixels.assign(image.pixels(),image.pixels()+180*100*4);
        }
        return state;
    };
    auto same=[&](const Snapshot& before,const char* context,bool permitLiveCaptureRecreation=false) {
        const auto after=snapshot();
        // Plain LiveZoom Snip recreates its magnifier. Its hidden temporary static canvas
        // is an implementation detail; the visible live factor and source view must survive.
        const bool plainLiveCapture=permitLiveCaptureRecreation && before.mode==0 && before.liveVisible && !before.visible;
        const bool stable=before.mode==after.mode && before.visible==after.visible &&
            before.liveVisible==after.liveVisible && before.capture==after.capture && before.ring==after.ring &&
            before.liveLevel==after.liveLevel && EqualRect(&before.liveView,&after.liveView) &&
            (plainLiveCapture ? IsWindow(after.live) &&
                (g_fullScreenWorkaround ? !IsWindow(after.magnifier) : IsWindow(after.magnifier)) :
                before.level==after.level && EqualRect(&before.view,&after.view) && before.live==after.live &&
                before.magnifier==after.magnifier && before.style==after.style && before.canvas==after.canvas &&
                before.pixels.size()==after.pixels.size());
        if(!stable) std::cerr<<"Mode policy state mismatch: "<<context<<" before="<<before.mode<<" after="<<after.mode
                            <<" live_before="<<before.live<<" live_after="<<after.live
                            <<" level_before="<<before.level<<" level_after="<<after.level
                            <<" live_factor_before="<<before.liveLevel<<" live_factor_after="<<after.liveLevel
                            <<" live_view_before="<<before.liveView.left<<","<<before.liveView.top<<","<<before.liveView.right<<","<<before.liveView.bottom
                            <<" live_view_after="<<after.liveView.left<<","<<after.liveView.top<<","<<after.liveView.right<<","<<after.liveView.bottom
                            <<" fullscreen="<<g_fullScreenWorkaround<<" main_visible="<<before.visible<<"/"<<after.visible
                            <<" live_visible="<<before.liveVisible<<"/"<<after.liveVisible
                            <<" magnifier="<<before.magnifier<<"/"<<after.magnifier
                            <<" capture="<<before.capture<<"/"<<after.capture<<" ring="<<before.ring<<"/"<<after.ring
                            <<" canvas="<<before.canvas<<"/"<<after.canvas<<" style="<<before.style<<"/"<<after.style<<"\n";
        require(stable,context);
        for(size_t pixel=0;pixel<before.pixels.size()/4;++pixel)
            for(size_t channel=0;channel<3;++channel)
                require(before.pixels[pixel*4+channel]==after.pixels[pixel*4+channel],
                        "Ignored shortcut and completed Snip must preserve every sampled annotation pixel");
    };
    auto ignore=[&](WPARAM action) {
        const auto before=snapshot();
        press(action);
        same(before,"A conflicting external mode shortcut must leave the complete current state unchanged");
        ++results.ignoredKeys;
    };
    auto clean=[&] {
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(50);
        require(mode()==0 && !IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),
                "Mode policy fixtures must reset completely to the desktop");
        // Keep this process-owned host directly under the subsequently shown topmost canvas.
        ActivateTestHost(host);
        RedrawWindow(host,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        SetCursorPos(125,125);pump(25);
    };
    std::string phase="mode setup";
    auto stroke=[&](bool finish) {
        const auto before=snapshot();
        SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(340,340));
        const auto afterDown=mode();
        // Keep an unfinished synthetic stroke before its first move. LiveDraw correctly
        // repairs a missed physical mouse-up when a move arrives with no button held.
        if(finish) {
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(360,340));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(360,340));
        }
        if(!finish && (mode()&7)!=7) {
            const auto after=snapshot();POINT pointer{};GetCursorPos(&pointer);
            std::cerr<<"Tracing fixture mismatch: phase="<<phase<<" before="<<before.mode
                     <<" after_down="<<afterDown<<" after_event="<<after.mode
                     <<" fullscreen="<<g_fullScreenWorkaround<<" view_factor="<<DecodeZoomLevel(after.level)
                     <<" view="<<after.view.left<<","<<after.view.top<<","<<after.view.right<<","<<after.view.bottom
                     <<" live_visible="<<after.liveVisible<<" live_factor="<<after.liveLevel
                     <<" cursor="<<pointer.x<<","<<pointer.y<<" capture="<<after.capture
                     <<" selection="<<g_SelectionActive<<" save="<<g_bSaveInProgress<<" pen_down="<<unsigned(g_PenDown)<<"\n";
        }
    };
    auto paused=[&] {
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(360,340));
        SendMessage(g_hWndMain,WM_RBUTTONUP,0,MAKELPARAM(360,340));
        require((mode()&23)==17 && GetCapture()==g_hWndMain,
                "Paused policy fixture must retain Draw with its suspension cursor and capture");
    };
    auto modalCheck=[&] {
        const auto before=snapshot();
        for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(DRAW_HOTKEY),
                           WPARAM(LIVE_DRAW_HOTKEY),WPARAM(BREAK_HOTKEY),WPARAM(SNIP_HOTKEY),WPARAM(SNIP_SAVE_HOTKEY)}) {
            press(action);same(before,"All external mode shortcuts must be ignored inside a modal operation");
            ++results.modalKeys;
        }
    };
    auto snip=[&](int outcome) {
        std::pair<HRESULT,HRESULT> flushed{};
        if(g_PolicySelectionRect.left==320) flushed=flushNativeFrames();
        const auto before=snapshot();
        COLORREF nativeBefore=CLR_INVALID,hostBefore=CLR_INVALID,layeredKeyBefore{};
        BYTE layeredAlphaBefore{};DWORD layeredFlagsBefore{};
        BOOL layeredReadBefore{};float fullscreenBefore{};int fullscreenXBefore{},fullscreenYBefore{};
        if(g_PolicySelectionRect.left==320) {
            HDC screen=GetDC(nullptr),hostDc=GetDC(host);
            POINT sample{g_PolicySelectionRect.left+70,g_PolicySelectionRect.top+70};
            nativeBefore=GetPixel(screen,sample.x,sample.y);ScreenToClient(host,&sample);
            hostBefore=GetPixel(hostDc,sample.x,sample.y);ReleaseDC(host,hostDc);ReleaseDC(nullptr,screen);
            layeredReadBefore=GetLayeredWindowAttributes(g_hWndMain,&layeredKeyBefore,&layeredAlphaBefore,&layeredFlagsBefore);
            MagGetFullscreenTransform(&fullscreenBefore,&fullscreenXBefore,&fullscreenYBefore);
        }
        cancelSnip=outcome==1;
        const std::wstring path=(std::filesystem::temp_directory_path()/
            (L"ZoomItPolicy_"+std::to_wstring(GetCurrentProcessId())+L".png")).wstring();
        auto deleteImage=zoomit::OnExit([&] {DeleteFileW(path.c_str());g_TestSavePath=nullptr;g_PolicyModalCheck={};});
        if(outcome>=2) {g_TestSavePath=outcome==3 ? nullptr : path.c_str();g_TestSaveFilter=1;}
        g_PolicyModalCheck=modalCheck;
        SetTimer(nullptr,0,15,SelectPolicyRegion);
        press(outcome>=2 ? SNIP_SAVE_HOTKEY : SNIP_HOTKEY);
        g_PolicyModalCheck={};
        if(outcome==0) {
            BITMAP bitmap{};
            require(g_TestSnipBitmap && GetObject(g_TestSnipBitmap,sizeof(bitmap),&bitmap) &&
                    bitmap.bmWidth==81 && bitmap.bmHeight==81,"Eligible mode must capture the requested Snip rectangle");
            if(g_PolicySelectionRect.left==320) {
                HDC screen=GetDC(nullptr),captured=CreateCompatibleDC(screen);
                const HGDIOBJ previous=SelectObject(captured,g_TestSnipBitmap);
                require(GetPixel(captured,25,20)==COLOR_RED,
                        "Desktop LiveDraw Snip must include the visible red annotation");
                const COLORREF background=GetPixel(captured,70,70);
                if(background!=RGB(255,255,255)) {
                    RECT hostWindow{},mainWindow{};GetWindowRect(host,&hostWindow);GetWindowRect(g_hWndMain,&mainWindow);
                    COLORREF layeredKey{};BYTE layeredAlpha{};DWORD layeredFlags{};
                    const BOOL layeredRead=GetLayeredWindowAttributes(g_hWndMain,&layeredKey,&layeredAlpha,&layeredFlags);
                    float fullscreen{};int fullscreenX{},fullscreenY{};MagGetFullscreenTransform(&fullscreen,&fullscreenX,&fullscreenY);
                    HDC hostDc=GetDC(host);POINT sample{g_PolicySelectionRect.left+70,g_PolicySelectionRect.top+70};ScreenToClient(host,&sample);
                    const COLORREF nativeHost=GetPixel(hostDc,sample.x,sample.y);ReleaseDC(host,hostDc);
                    POINT surfacePoint{g_PolicySelectionRect.left+70,g_PolicySelectionRect.top+70};
                    const HWND surface=WindowFromPoint(surfacePoint);wchar_t surfaceClass[128]{},surfaceTitle[256]{};
                    GetClassNameW(surface,surfaceClass,_countof(surfaceClass));GetWindowTextW(surface,surfaceTitle,_countof(surfaceTitle));
                    std::cerr<<"LiveDraw Snip background mismatch: captured_rgb="<<std::hex<<background
                             <<" native_rgb="<<GetPixel(screen,g_PolicySelectionRect.left+70,g_PolicySelectionRect.top+70)
                             <<" canvas_rgb="<<GetPixel(before.canvas,g_PolicySelectionRect.left+70,g_PolicySelectionRect.top+70)
                             <<" host_rgb="<<nativeHost<<" native_before="<<nativeBefore<<" host_before="<<hostBefore
                             <<" color_key_before="<<layeredKeyBefore<<" color_key_after="<<layeredKey
                             <<std::dec<<" alpha_before="<<unsigned(layeredAlphaBefore)<<" alpha_after="<<unsigned(layeredAlpha)
                             <<" layered_flags_before="<<layeredFlagsBefore<<" layered_flags_after="<<layeredFlags
                             <<" layered_read_before="<<layeredReadBefore<<" layered_read_after="<<layeredRead
                             <<" fullscreen_before="<<fullscreenBefore<<","<<fullscreenXBefore<<","<<fullscreenYBefore
                             <<" fullscreen_after="<<fullscreen<<","<<fullscreenX<<","<<fullscreenY
                             <<" host_brush="<<GetClassLongPtrW(host,GCLP_HBRBACKGROUND)
                             <<" host_visible="<<IsWindowVisible(host)
                             <<" host_rect="<<hostWindow.left<<","<<hostWindow.top<<","<<hostWindow.right<<","<<hostWindow.bottom
                             <<" main_rect="<<mainWindow.left<<","<<mainWindow.top<<","<<mainWindow.right<<","<<mainWindow.bottom
                             <<" dwm_flush="<<flushed.first<<","<<flushed.second<<" surface="<<surface<<" main="<<g_hWndMain<<"\n";
                    std::wcerr<<L"LiveDraw Snip surface: class="<<surfaceClass<<L" title="<<surfaceTitle<<L"\n";
                }
                require(background==RGB(255,255,255),
                        "Desktop LiveDraw Snip must include the real desktop behind its transparent canvas");
                SelectObject(captured,previous);DeleteDC(captured);ReleaseDC(nullptr,screen);
            }
            DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;
        } else if(outcome==2) {
            Gdiplus::Bitmap image(path.c_str());
            require(image.GetLastStatus()==Gdiplus::Ok && image.GetWidth()==81 && image.GetHeight()==81,
                    "Eligible mode must save a valid PNG Snip without changing drawing state");
        } else {
            require(!g_TestSnipBitmap && GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES,
                    "Cancelled Snip or save must produce no output and preserve the old drawing state");
        }
        same(before,"Snip copy, cancellation and save must restore the preceding logical live mode or exact drawing state",true);
        if(before.mode==0 && before.liveVisible && !before.visible) {
            pump(50);
            same(before,"Plain LiveZoom Snip must retain its restored factor and source view through subsequent timer frames",true);
        }
        ++results.snipCases;
    };
    g_TestBeforeSaveDialog=CheckPolicySaveDialog;

    // Plain Zoom deliberately has no Draw/LiveDraw hotkey transition.
    clean();press(ZOOM_HOTKEY);pump(60);
    require((mode()&3)==1 && IsWindowVisible(g_hWndMain),"External Zoom shortcut must open plain Zoom");
    for(WPARAM action:{WPARAM(LIVE_HOTKEY),WPARAM(DRAW_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY),WPARAM(BREAK_HOTKEY)}) ignore(action);
    snip(0);snip(1);snip(2);snip(3);++results.states;
    press(ZOOM_HOTKEY);pump(40);require(mode()==0 && !IsWindowVisible(g_hWndMain),"Own Zoom shortcut must still close plain Zoom");

    // Each rendering path must support both frozen Draw and transparent LiveDraw.
    for(bool fullscreen:{false,true}) {
        g_fullScreenWorkaround=fullscreen;
        clean();press(LIVE_HOTKEY);pump(70);
        require(IsWindowVisible(g_hWndLiveZoom) && mode()==0,"External LiveZoom shortcut must open only LiveZoom");
        ignore(ZOOM_HOTKEY);ignore(BREAK_HOTKEY);snip(0);snip(1);++results.states;
        press(LIVE_HOTKEY);pump(40);require(!IsWindow(g_hWndLiveZoom),"Own LiveZoom shortcut must still close plain LiveZoom");
        for(bool liveDraw:{false,true}) {
            clean();press(LIVE_HOTKEY);pump(70);
            press(liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY);pump(30);
            require((mode()&3)==3 && IsWindowVisible(g_hWndMain) &&
                    (IsWindowVisible(g_hWndLiveZoom)!=FALSE)==liveDraw,
                    "LiveZoom must allow frozen Draw and live overlay through their external shortcuts");
            require(((GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED)!=0)==liveDraw,
                    "Draw must freeze the view and LiveDraw must keep a transparent overlay");
            stroke(true);
            for(int state=0;state<3;++state) {
                phase=std::string(fullscreen ? "fullscreen " : "window ")+(liveDraw ? "LiveDraw state " : "frozen Draw state ")+std::to_string(state);
                if(state==1) paused();
                if(state==2) {stroke(false);require((mode()&7)==7,"Tracing fixture must retain its active stroke");}
                for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(DRAW_HOTKEY),
                                   WPARAM(LIVE_DRAW_HOTKEY),WPARAM(BREAK_HOTKEY)}) ignore(action);
                if(liveDraw || state==2) {ignore(SNIP_HOTKEY);ignore(SNIP_SAVE_HOTKEY);}
                else for(int outcome=0;outcome<4;++outcome) snip(outcome);
                ++results.states;
                if(state==2) SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(360,340));
            }
            clean();
        }
    }
    // The desktop permits both Draw forms, with Snip safe before and after suspension.
    g_fullScreenWorkaround=FALSE;
    for(bool liveDraw:{false,true}) {
        clean();press(liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY);pump(30);
        require((mode()&3)==3 && IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom),
                "Desktop Draw and LiveDraw must start through their own external shortcuts");
        if(liveDraw) flushNativeFrames();
        stroke(true);
        if(liveDraw) {
            g_PolicySelectionRect={320,320,400,400};snip(0);g_PolicySelectionRect={20,20,100,100};
        }
        for(int state=0;state<3;++state) {
            phase=std::string(liveDraw ? "desktop LiveDraw state " : "desktop frozen Draw state ")+std::to_string(state);
            if(state==1) paused();
            if(state==2) {stroke(false);require((mode()&7)==7,"Desktop tracing fixture must retain its active stroke");}
            for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(DRAW_HOTKEY),
                               WPARAM(LIVE_DRAW_HOTKEY),WPARAM(BREAK_HOTKEY)}) ignore(action);
            if(state==2) {ignore(SNIP_HOTKEY);ignore(SNIP_SAVE_HOTKEY);}
            else for(int outcome=0;outcome<4;++outcome) snip(outcome);
            ++results.states;
            if(state==2) SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(360,340));
        }
        clean();
    }
    // Restoring Draw after Snip must not insert a synthetic undo entry.
    clean();press(DRAW_HOTKEY);pump(20);
    const HDC undoCanvas=reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));
    const COLORREF beforeStroke=GetPixel(undoCanvas,345,340);
    stroke(true);require(GetPixel(undoCanvas,345,340)==COLOR_RED,"Undo fixture must contain a real annotation stroke");
    snip(0);
    BYTE keyboard[256]{},pressedKeyboard[256]{};
    require(GetKeyboardState(keyboard)!=FALSE,"Snapshot keyboard state for native Undo test");
    memcpy(pressedKeyboard,keyboard,sizeof(keyboard));pressedKeyboard[VK_CONTROL]|=0x80;
    auto restoreKeyboard=zoomit::OnExit([&] {SetKeyboardState(keyboard);});
    require(SetKeyboardState(pressedKeyboard)!=FALSE,"Set Ctrl for the existing native Undo handler");
    SendMessage(g_hWndMain,WM_KEYDOWN,'Z',0);SetKeyboardState(keyboard);
    require(GetPixel(undoCanvas,345,340)==beforeStroke,"First Undo after Snip must remove the real preceding stroke");
    ++results.snipCases;clean();

    clean();press(BREAK_HOTKEY);pump(30);require((mode()&32)!=0,"External Pause shortcut must start the timer");
    for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(DRAW_HOTKEY),
                       WPARAM(LIVE_DRAW_HOTKEY),WPARAM(SNIP_HOTKEY),WPARAM(SNIP_SAVE_HOTKEY)}) ignore(action);
    press(BREAK_HOTKEY);require((mode()&32)!=0,"Own Pause shortcut must restart and retain the active timer");++results.states;
    clean();g_PolicyModalCheck=modalCheck;
    const DWORD optionsDraw=g_DrawToggleKey,optionsLive=g_LiveZoomToggleKey;
    g_PolicyOptionsCheck=[&] {
        HWND page=g_OptionsTabs[DRAW_PAGE].hPage;
        auto label=[&] {wchar_t text[96]{};GetDlgItemTextW(page,IDC_LIVE_DRAW_HOTKEY,text,_countof(text));return std::wstring(text);};
        require(label()==L"Ctrl+Shift+3","Draw page must explicitly display the default derived LiveDraw hotkey");++results.guiCases;
        auto edit=[&](DWORD key) {
            HWND control=GetDlgItem(page,IDC_DRAW_HOTKEY);SendMessage(control,HKM_SETHOTKEY,key,0);
            SendMessage(page,WM_COMMAND,MAKEWPARAM(IDC_DRAW_HOTKEY,EN_CHANGE),reinterpret_cast<LPARAM>(control));
        };
        edit(((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8)|VK_F23);
        if(label()!=L"Ctrl+Alt+Shift+F23")
            std::wcerr<<L"Customized LiveDraw label mismatch: actual="<<label()
                      <<L" control_key="<<SendDlgItemMessage(page,IDC_DRAW_HOTKEY,HKM_GETHOTKEY,0,0)<<L"\n";
        require(label()==L"Ctrl+Alt+Shift+F23","LiveDraw label must follow an unsaved customized Draw hotkey");++results.guiCases;
        edit(0);require(label()==L"Disabled with Draw","Disabling Draw must immediately indicate LiveDraw is disabled");++results.guiCases;
        edit(((HOTKEYF_CONTROL|HOTKEYF_SHIFT)<<8)|'3');
        require(label()==L"Remove Shift from Draw shortcut","GUI must explain the collision when Draw already includes Shift");++results.guiCases;
        require(g_DrawToggleKey==optionsDraw && g_LiveZoomToggleKey==optionsLive,
                "Unsaved GUI hotkey edits must not mutate the active registration preferences");
    };
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,PolicyOptionsProc);
    g_PolicyOptionsCheck={};
    require(g_DrawToggleKey==optionsDraw && g_LiveZoomToggleKey==optionsLive,"Cancel must preserve the configured Draw and LiveZoom shortcuts");++results.guiCases;
    g_PolicyModalCheck={};require(!hWndOptions && mode()==0 && !IsWindow(g_hWndLiveZoom),"Closing Options must not run any ignored shortcut later");

    // Runtime reset while a nested selection is open must invalidate the old capture safely.
    for(bool save:{false,true}) {
        clean();press(DRAW_HOTKEY);pump(20);stroke(true);
        g_PolicyModalCheck=[&] {
            SendMessage(g_hWndMain,recovery::ResetMessage,0,0);
            require(mode()==0 && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom),
                    "Runtime reset during Snip selection must safely invalidate the original drawing session");
        };
        cancelSnip=true;SetTimer(nullptr,0,15,SelectPolicyRegion);
        press(save ? SNIP_SAVE_HOTKEY : SNIP_HOTKEY);
        g_PolicyModalCheck={};cancelSnip=false;
        require(mode()==0 && !g_TestSnipBitmap,"Reset selection must not use the freed drawing bitmap or revive a mode");
        press(DRAW_HOTKEY);pump(20);
        require((mode()&3)==3,"Capture modal guard must unwind correctly after runtime reset");
        ++results.resetCases;clean();
    }

    // Pending frames do not make another mode eligible, including the hidden frozen-LiveZoom state.
    for(bool fullscreen:{false,true}) {
        g_fullScreenWorkaround=fullscreen;g_AnimateLiveZoom=TRUE;
        clean();press(LIVE_HOTKEY);
        ignore(ZOOM_HOTKEY);ignore(BREAK_HOTKEY);
        press(DRAW_HOTKEY);pump(15);
        require((mode()&3)==3 && !IsWindowVisible(g_hWndLiveZoom),"Draw must safely finish pending LiveZoom animation and freeze the same view");
        ignore(ZOOM_HOTKEY);ignore(LIVE_HOTKEY);ignore(LIVE_DRAW_HOTKEY);++results.animationCases;
        clean();press(LIVE_HOTKEY);pump(400);press(LIVE_HOTKEY);
        ignore(ZOOM_HOTKEY);ignore(BREAK_HOTKEY);pump(550);
        require(!IsWindow(g_hWndLiveZoom) && mode()==0,"Ignored animation-time shortcuts must never be replayed after exit");
        ++results.animationCases;clean();
    }
    g_AnimateLiveZoom=FALSE;g_fullScreenWorkaround=FALSE;

    // Exit requests are idempotent and belong to the session that originally queued them.
    for(int exitVariant=0;exitVariant<6;++exitVariant) {
        const bool live=exitVariant>=2;
        const bool overlay=exitVariant==1 || exitVariant==3 || exitVariant==5;
        g_fullScreenWorkaround=exitVariant>=4;
        clean();if(live) {press(LIVE_HOTKEY);pump(60);}
        const WPARAM drawingAction=overlay ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY;
        press(drawingAction);pump(20);stroke(true);paused();
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);
        MSG exit{};
        require(PeekMessage(&exit,g_hWndMain,WM_USER_EXIT_ZOOM,WM_USER_EXIT_ZOOM,PM_REMOVE)!=FALSE,
                "Esc must queue a real exit request carrying the original drawing session identity");
        DispatchMessage(&exit);DispatchMessage(&exit);pump(40);
        require(mode()==0 && !IsWindowVisible(g_hWndMain) && GetCapture()!=g_hWndMain &&
                !(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED) &&
                (IsWindowVisible(g_hWndLiveZoom)!=FALSE)==live,
                "Dispatching a queued drawing exit twice must close only once without recreating Zoom or Draw");
        ++results.duplicateExitCases;
        press(drawingAction);pump(20);stroke(true);
        require((mode()&3)==3,"A new drawing session must remain available after the prior exit");
        const auto newSession=snapshot();
        DispatchMessage(&exit);DispatchMessage(&exit);
        same(newSession,"A delayed exit from an older session must leave the new drawing session entirely unchanged");
        pump(30);
        require((mode()&3)==3 && IsWindowVisible(g_hWndMain) &&
                reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0))==newSession.canvas,
                "Ignored stale exits must not queue a delayed close of the new drawing session");
        ++results.lateExitCases;clean();
    }
    g_fullScreenWorkaround=FALSE;

    // Model pen contact independently of the physical mouse; no tablet hardware is simulated.
    {
        clean();press(LIVE_DRAW_HOTKEY);pump(20);
        require((GetAsyncKeyState(VK_LBUTTON)&0x8000)==0,"Pen-contact model requires the physical mouse button to be released");
        const BOOLEAN priorPenDown=g_PenDown;
        auto restorePenContact=zoomit::OnExit([&] {g_PenDown=priorPenDown;});
        const LPARAM priorExtraInfo=GetMessageExtraInfo();
        auto restorePenExtraInfo=zoomit::OnExit([&] {SetMessageExtraInfo(priorExtraInfo);});
        stroke(false);g_PenDown=TRUE;
        require((mode()&23)==7,"Pen-contact model must start with a live drawing stroke");
        SendMessage(g_hWndMain,WM_USER_SESSION_TICK,3,0);
        SendPenMessage(g_hWndMain,WM_MOUSEMOVE,MAKELPARAM(360,340));
        SetMessageExtraInfo(priorExtraInfo);
        require((mode()&23)==7,"Timer and movement mouse-release repair must preserve a pen contact that is still down");
        g_PenDown=FALSE;SendMessage(g_hWndMain,WM_USER_SESSION_TICK,3,0);
        require((mode()&23)==3,"Idle timer must complete the missed stationary release after pen contact ends");
        ++results.penContactCases;clean();
    }

    // Simulated pen contact resumes a paused LiveDraw ring through the intentional
    // ReleaseCapture callback without losing contact or the active pointer identity.
    {
        clean();press(LIVE_DRAW_HOTKEY);pump(20);stroke(true);paused();
        const LPARAM previousExtra=GetMessageExtraInfo();const BOOLEAN previousDown=g_PenDown;
        const auto pointerType=pGetPointerType;const auto penInfo=pGetPointerPenInfo;
        auto restorePen=zoomit::OnExit([&]{SetMessageExtraInfo(previousExtra);g_PenDown=previousDown;pGetPointerType=pointerType;pGetPointerPenInfo=penInfo;});
        pGetPointerType=nullptr;pGetPointerPenInfo=nullptr;
        SendMessage(g_hWndMain,WM_POINTERDOWN,7,MAKELPARAM(340,340));
        require(g_PenDown && (mode()&23)==7,"Pen down on the paused ring must resume tracing without capture-release callbacks clearing the contact");++results.penContactCases;
        const auto firstContact=snapshot();
        SendMessage(g_hWndMain,WM_POINTERDOWN,8,MAKELPARAM(330,330));
        SendMessage(g_hWndMain,WM_POINTERDOWN,7,MAKELPARAM(330,330));
        SendMessage(g_hWndMain,WM_POINTERUP,8,MAKELPARAM(330,330));
        same(firstContact,"A second contact, duplicate down and foreign pointer-up must not replace the active pen stroke");
        require(g_PenDown && (mode()&23)==7,"Foreign pen events must preserve the original active pointer contact");++results.penContactCases;
        SendMessage(g_hWndMain,WM_USER_SESSION_TICK,3,0);
        SendMessage(g_hWndMain,WM_POINTERUPDATE,7,MAKELPARAM(360,340));
        require(g_PenDown && (mode()&23)==7,"The same simulated pen contact must survive a tracking tick and pointer update");++results.penContactCases;
        SendMessage(g_hWndMain,WM_POINTERUP,7,MAKELPARAM(360,340));
        require(!g_PenDown && (mode()&23)==17,"A matching pen up must finish the stroke and return to the paused ring");
        SendMessage(g_hWndMain,WM_POINTERDOWN,7,MAKELPARAM(340,345));
        require(g_PenDown && (mode()&23)==7,"A second pen contact must resume drawing from the paused ring again");
        SendMessage(g_hWndMain,WM_POINTERUP,7,MAKELPARAM(360,345));
        require(!g_PenDown && (mode()&23)==17,"Completing the second pen contact must preserve the reusable paused drawing state");++results.penContactCases;clean();
        SendMessage(g_hWndMain,WM_POINTERDOWN,7,MAKELPARAM(340,340));
        require(!g_PenDown && mode()==0 && !IsWindowVisible(g_hWndMain),"A delayed pointer-down on the idle desktop must not resurrect contact or drawing state");++results.penContactCases;
    }

    // An unavailable optional magnifier filter must not prevent desktop LiveDraw or its cleanup.
    {
        clean();const auto filter=pMagSetWindowFilterList;
        auto restoreFilter=zoomit::OnExit([&] {pMagSetWindowFilterList=filter;});
        pMagSetWindowFilterList=nullptr;
        press(LIVE_DRAW_HOTKEY);pump(25);
        require((mode()&3)==3 && (GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),
                "Desktop LiveDraw must work when the optional magnifier filter API is unavailable");
        stroke(true);paused();SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(25);
        require(mode()==0 && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) &&
                !(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED) && GetCapture()!=g_hWndMain,
                "Esc must completely clean up suspended desktop LiveDraw without calling an unavailable magnifier API");
        CURSORINFO cursor{sizeof(cursor)};
        require(GetCursorInfo(&cursor) && (cursor.flags&CURSOR_SHOWING) && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW) && systemCursorShown,
                "Optional API absence must leave CURSOR_SHOWING, the logical arrow and a successful visibility request after LiveDraw");
        ++results.unavailableApiCases;clean();
    }
    // Denied graphics allocation must keep the previous session intact and leave no half-created overlay.
    auto denyDrawing=[&](WPARAM action,bool live) {
        const auto before=snapshot();
        const bool nativeCursorShown=systemCursorShown;
        const LONG magnifierStyle=IsWindow(g_hWndLiveZoomMag) ? GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE) : 0;
        wchar_t oldValue[64]{};
        const DWORD oldLength=GetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",oldValue,_countof(oldValue));
        require(oldLength<_countof(oldValue),"Graphics failure fixture must be able to restore its original environment");
        {
            auto restoreFailure=zoomit::OnExit([&] {SetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",oldLength ? oldValue : nullptr);});
            SetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",L"1");
            press(action);
        }
        require(mode()==0 && !IsWindowVisible(g_hWndMain) &&
                !(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED) &&
                !SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0) && GetCapture()!=g_hWndMain,
                "Failed Draw preparation must not publish an empty canvas, drawing mode, timer or layered overlay");
        if(live) {
            require(g_hWndLiveZoom==before.live && g_hWndLiveZoomMag==before.magnifier &&
                    GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)==before.style &&
                    (!IsWindow(g_hWndLiveZoomMag) || GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)==magnifierStyle),
                    "Denied Draw allocation must retain the existing LiveZoom windows and magnified cursor style");
            require(systemCursorShown==nativeCursorShown,
                    "Denied drawing allocation must retain the previous hardware cursor visibility and avoid duplicate pointers");
            same(before,"Denied graphics preparation must preserve the previous logical LiveZoom factor and source view",true);
        } else {
            require(!IsWindow(g_hWndLiveZoom),"Denied desktop Draw allocation must remain idle");
        }
        pump(15);
        require(mode()==0 && !IsWindowVisible(g_hWndMain) && (IsWindowVisible(g_hWndLiveZoom)!=FALSE)==live,
                "Failed drawing entry must not queue a later activation or discard the previous LiveZoom session");
        press(action);pump(25);
        require((mode()&3)==3 && IsWindowVisible(g_hWndMain),
                "Drawing must become available immediately after the allocation failure is removed");
        ++results.allocationCases;clean();
    };
    for(WPARAM action:{WPARAM(DRAW_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {clean();denyDrawing(action,false);}
    for(bool fullscreen:{false,true}) {
        g_fullScreenWorkaround=fullscreen;
        for(WPARAM action:{WPARAM(DRAW_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
            clean();press(LIVE_HOTKEY);pump(60);denyDrawing(action,true);
        }
    }
    g_fullScreenWorkaround=FALSE;

    // Real queued timer identities must not operate a later canvas or magnifier.
    {
        clean();press(LIVE_DRAW_HOTKEY);pump(20);
        const UINT_PTR oldMain=static_cast<UINT_PTR>(SendMessage(g_hWndMain,WM_TEST_QUERY_TIMERS,0,3));
        require(oldMain>=0x4000,"LiveDraw must expose the current native canvas timer identity");
        clean();press(LIVE_DRAW_HOTKEY);pump(20);
        const UINT_PTR current=static_cast<UINT_PTR>(SendMessage(g_hWndMain,WM_TEST_QUERY_TIMERS,0,3));
        require(current && current!=oldMain,"A new drawing session must use a fresh timer identity");
        const auto before=snapshot();SendMessage(g_hWndMain,WM_TIMER,oldMain,0);
        same(before,"A late native canvas timer must leave the next drawing session unchanged");++results.timerIdentityCases;clean();
        press(LIVE_HOTKEY);pump(40);
        const UINT_PTR oldLive=static_cast<UINT_PTR>(SendMessage(g_hWndLiveZoom,WM_TEST_QUERY_TIMERS,0,0));
        require(oldLive>=0x4000,"LiveZoom must expose its current magnifier timer identity");
        clean();press(LIVE_HOTKEY);pump(40);
        const UINT_PTR newLive=static_cast<UINT_PTR>(SendMessage(g_hWndLiveZoom,WM_TEST_QUERY_TIMERS,0,0));
        require(newLive && newLive!=oldLive,"A new magnifier must use a fresh timer identity even if Windows reuses its handle");
        const auto liveBefore=snapshot();SendMessage(g_hWndLiveZoom,WM_TIMER,oldLive,0);
        same(liveBefore,"A timer left by an old magnifier must never pan or close a new magnifier");++results.timerIdentityCases;clean();
    }

    // Deterministic cross-feature sequences retain annotations through ignored keys,
    // partial wheel input, pauses, capture cancellation and explicit mode exits.
    for(bool layered:{false,true}) {
        clean();g_AnimateLiveZoom=FALSE;SetInitialZoomIndex(3);press(LIVE_HOTKEY);pump(30);
        const float original=snapshot().liveLevel;
        const ULONG_PTR obsolete=g_LiveZoomWheel.Epoch();
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,obsolete,60);
        const WPARAM drawingAction=layered ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY;
        press(drawingAction);pump(30);stroke(true);paused();
        const auto pausedBefore=snapshot();
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,obsolete,60);pump(15);
        same(pausedBefore,"Partial wheel requests from LiveZoom must not survive transition into suspended drawing");
        for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(BREAK_HOTKEY)})ignore(action);
        if(layered)ignore(SNIP_HOTKEY);else snip(1);
        SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(340,340));
        SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(340,340));
        require((mode()&23)==3,"Left click must resume the same annotation canvas after the complex pause sequence");
        stroke(true);ignore(ZOOM_HOTKEY);paused();
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);pump(40);
        require(mode()==0 && IsWindowVisible(g_hWndLiveZoom) && snapshot().liveLevel==original,
                "Explicit second right click must finish drawing and retain the original LiveZoom factor");
        snip(1);
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,obsolete,WHEEL_DELTA);pump(15);
        require(snapshot().liveLevel==original,"Old wheel requests must remain rejected after drawing and Snip finish");
        ++results.sequenceCases;clean();
    }

    // Simulated display messages exercise safety paths; physical clone displays and
    // mixed-DPI hardware still require a separate Windows integration environment.
    {
        const auto monitorApi=pGetMonitorInfo;
        auto restoreMonitor=zoomit::OnExit([&]{pGetMonitorInfo=monitorApi;g_TestChangedMonitorGeometry=false;});
        pGetMonitorInfo=ScenarioMonitorInfo;
        for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(DRAW_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
            clean();press(action);pump(30);
            const auto sameLayout=snapshot();
            SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,MAKELPARAM(GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)));
            same(sameLayout,"A display notification with unchanged queried geometry must preserve the current mode");++results.topologyCases;
            SendMessage(g_hWndMain,WM_DPICHANGED,MAKEWPARAM(96,96),0);
            same(sameLayout,"A DPI notification with unchanged logical geometry must not discard annotations");++results.topologyCases;
            g_TestChangedMonitorGeometry=true;
            SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,0);
            g_TestChangedMonitorGeometry=false;
            require(mode()==0 && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) && GetCapture()!=g_hWndMain,
                    "Obsolete display geometry must synchronously release drawing, magnification and capture");
            ResetThenReadOwnedNormalPointer(host,"Changed simulated monitor geometry");++results.topologyCases;
        }
        for(UINT cancellation:{WM_CANCELMODE,WM_CAPTURECHANGED}) {
            clean();press(LIVE_DRAW_HOTKEY);pump(20);stroke(false);
            require((mode()&7)==7,"Capture cancellation fixture must have an unfinished stroke");
            SendMessage(g_hWndMain,cancellation,0,reinterpret_cast<LPARAM>(host));
            require((mode()&7)==3,"Losing capture or cancelling a mode must terminate the unfinished stroke safely");
            ignore(ZOOM_HOTKEY);stroke(true);paused();
            require((mode()&23)==17,"Drawing must remain usable after a cancelled stroke");++results.topologyCases;clean();
        }
    }

    // Fail each graphics preparation stage after earlier allocations have succeeded.
    // This tests rollback and ownership, not only an early refusal before allocation.
    {
        using zoomit::runtime::Api;
        auto clearFailures=zoomit::OnExit([]{zoomit::runtime::Clear();});
        clean();const DWORD directGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        for(unsigned stage=1;stage<=zoomit::GraphicsSession::PreparationChecks;++stage) {
            zoomit::runtime::FailAt(Api::Graphics,stage);
            {
                zoomit::GraphicsSession candidate;
                require(!candidate.Prepare({0,0,192,128},5,COLOR_RED),"Every injected graphics preparation stage must fail deterministically");
                require(zoomit::runtime::Calls(Api::Graphics)==stage,"The requested partial preparation failure must actually be reached");
            }
            zoomit::runtime::Clear();
            require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==directGdi,"Every partially prepared graphics session must release all owned GDI resources");++results.stagedGraphicsCases;
        }
        for(bool live:{false,true})for(WPARAM action:{WPARAM(DRAW_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
            clean();if(live){press(LIVE_HOTKEY);pump(30);}
            for(unsigned stage=1;stage<=zoomit::GraphicsSession::PreparationChecks;++stage) {
                const auto before=snapshot();const DWORD resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
                zoomit::runtime::FailAt(Api::Graphics,stage);press(action);
                const unsigned reached=zoomit::runtime::Calls(Api::Graphics);zoomit::runtime::Clear();
                require(reached>=stage,"Drawing entry must exercise the injected graphics preparation stage");
                require(mode()==0 && !IsWindowVisible(g_hWndMain) && !SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0) && GetCapture()!=g_hWndMain,
                        "Partial drawing preparation failure must never publish a canvas or take mouse capture");
                if(live)same(before,"Every failed partial drawing entry must preserve the preceding LiveZoom session",true);
                else require(!IsWindow(g_hWndLiveZoom),"Failed desktop drawing preparation must remain idle");
                require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=resources,"Partial drawing entry failures must not accumulate GDI resources");++results.stagedGraphicsCases;
            }
            press(action);pump(20);stroke(true);
            require((mode()&3)==3,"Drawing must work again immediately after the last staged failure is removed");clean();
        }
        for(Api api:{Api::Cursor,Api::Monitor})for(WPARAM action:{WPARAM(ZOOM_HOTKEY),WPARAM(DRAW_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
            clean();zoomit::runtime::FailAlways(api);press(action);zoomit::runtime::Clear();
            require(mode()==0 && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) && GetCapture()!=g_hWndMain,
                    "Unavailable cursor or monitor information must reject new modes without a half-created overlay");++results.runtimeFailureCases;
        }
        for(Api api:{Api::Cursor,Api::Monitor}) {
            clean();press(LIVE_HOTKEY);pump(30);const auto before=snapshot();
            zoomit::runtime::FailAlways(api);SendMessage(g_hWndLiveZoom,WM_USER_SESSION_TICK,0,0);zoomit::runtime::Clear();
            same(before,"A transient cursor or monitor query failure must skip an active LiveZoom frame without corrupting its view");++results.runtimeFailureCases;clean();
        }
        g_AnimateZoom=TRUE;SetInitialZoomIndex(3);zoomit::runtime::FailAt(Api::Timer,1);press(ZOOM_HOTKEY);zoomit::runtime::Clear();
        require((mode()&3)==1 && DecodeZoomLevel(snapshot().level)==2.0f,"A denied static animation timer must degrade to the complete requested zoom");++results.runtimeFailureCases;clean();g_AnimateZoom=FALSE;
        for(WPARAM action:{WPARAM(LIVE_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
            zoomit::runtime::FailAt(Api::Timer,1);press(action);zoomit::runtime::Clear();
            require(mode()==0 && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) && GetCapture()!=g_hWndMain,
                    "A denied essential live timer must leave a usable idle state");++results.runtimeFailureCases;clean();
        }
        // A persistent unavailable input desktop must recover even if its queued
        // reset notification cannot be posted. Hold the UI pump until the deadline
        // so no native timer can consume the recovery before the injected failure.
        clean();press(LIVE_HOTKEY);pump(30);const auto healthy=snapshot();
        zoomit::runtime::FailAlways(Api::Cursor);SendMessage(g_hWndLiveZoom,WM_USER_SESSION_TICK,0,0);
        same(healthy,"The initial unavailable-input frame must preserve its last valid LiveZoom view");++results.runtimeFailureCases;
        Sleep(1550);zoomit::runtime::FailAlways(Api::Post);
        SendMessage(g_hWndLiveZoom,WM_USER_SESSION_TICK,0,0);
        const unsigned postAttempts=zoomit::runtime::Calls(Api::Post);zoomit::runtime::Clear();
        require(postAttempts && mode()==0 && !IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain) && GetCapture()!=g_hWndMain,
                "A failed queued reset must fall back to synchronous safe cleanup after persistent input loss");
        ResetThenReadOwnedNormalPointer(host,"Persistent input loss with failed reset posting");++results.runtimeFailureCases;clean();
    }

    // Real registrations must derive LiveDraw from Draw, independent of LiveZoom.
    require(GetLiveDrawHotkey((HOTKEYF_CONTROL<<8)|'3')==((HOTKEYF_CONTROL|HOTKEYF_SHIFT)<<8|'3') &&
            GetLiveDrawHotkey(0)==0 && GetLiveDrawHotkey((HOTKEYF_CONTROL|HOTKEYF_SHIFT)<<8|'3')==0,
            "LiveDraw must add Shift to Draw and disable ambiguous or missing Draw shortcuts");
    ++results.registrationCases;
    UnregisterAllHotkeys(g_hWndMain);
    g_ToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
    g_DrawToggleKey=((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8)|VK_F23;g_DrawToggleMod=MOD_CONTROL|MOD_ALT;
    g_LiveZoomToggleKey=((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8)|VK_F22;g_LiveZoomToggleMod=MOD_CONTROL|MOD_ALT;
    RegisterAllHotkeys(g_hWndMain);
    require(!RegisterHotKey(host,71,MOD_CONTROL|MOD_ALT,VK_F23),"Configured Draw key must be registered by production code");
    require(!RegisterHotKey(host,72,MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F23),"Draw plus Shift must be registered for LiveDraw");
    require(RegisterHotKey(host,73,MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F22)!=FALSE,
            "Shift variant of LiveZoom must be free after LiveDraw is moved to Draw plus Shift");
    UnregisterHotKey(host,73);results.registrationCases+=3;
    auto nativeKey=[&](bool shift) {
        std::vector<INPUT> input;
        auto key=[&](WORD vk,bool release) {INPUT item{};item.type=INPUT_KEYBOARD;item.ki.wVk=vk;item.ki.dwFlags=release ? KEYEVENTF_KEYUP : 0;input.push_back(item);};
        key(VK_CONTROL,false);key(VK_MENU,false);if(shift) key(VK_SHIFT,false);
        key(VK_F23,false);key(VK_F23,true);
        if(shift) key(VK_SHIFT,true);key(VK_MENU,true);key(VK_CONTROL,true);
        require(SendInput(static_cast<UINT>(input.size()),input.data(),sizeof(INPUT))==input.size(),
                "Inject real configured Draw shortcut into native registered hotkeys");
        pump(80);
        require((mode()&3)==3 && ((GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED)!=0)==shift,
                "Real configured Draw and Draw plus Shift must activate exactly their intended modes");
        ++results.registrationCases;clean();
    };
    nativeKey(false);nativeKey(true);
    UnregisterAllHotkeys(g_hWndMain);
    return results;
}
void PrintModePolicyResults(const ModePolicyResults& result) {
    std::cout<<"{\"passed\":true,\"cursor_check_scope\":\"logical-state-or-fixture-cleanup\",\"visual_cursor_verification\":false,\"mode_policy_states\":"<<result.states
        <<",\"ignored_external_keys\":"<<result.ignoredKeys<<",\"snip_restore_cases\":"<<result.snipCases
        <<",\"modal_ignored_keys\":"<<result.modalKeys<<",\"native_registration_cases\":"<<result.registrationCases
        <<",\"animation_policy_cases\":"<<result.animationCases<<",\"gui_hotkey_cases\":"<<result.guiCases
        <<",\"selection_reset_cases\":"<<result.resetCases
        <<",\"allocation_failure_cases\":"<<result.allocationCases<<",\"unavailable_api_cases\":"<<result.unavailableApiCases
        <<",\"duplicate_exit_cases\":"<<result.duplicateExitCases<<",\"late_exit_cases\":"<<result.lateExitCases
        <<",\"pen_contact_cases\":"<<result.penContactCases
        <<",\"complex_sequences\":"<<result.sequenceCases<<",\"simulated_topology_cases\":"<<result.topologyCases
        <<",\"stale_native_timer_cases\":"<<result.timerIdentityCases<<",\"staged_graphics_failure_cases\":"<<result.stagedGraphicsCases
        <<",\"runtime_query_and_timer_failure_cases\":"<<result.runtimeFailureCases<<"}\n";
}

#include "zoom_granularity.h"
#include "live_wheel.h"
#include "zoom_indicator.h"
#include "capture.h"
#include "drawing_effects.h"
#include "selection.h"
#include "multimonitor.h"
#include "native_stationary_cursor.h"

int main(int argc, char** argv) {
    // The child serves only a normal native host, before app/registry initialization.
    if(argc>2 && strcmp(argv[1],"--stationary-cursor-host")==0)return RunNativeCursorHostChild(argv[2]);
    const bool stationaryNativeOnly=argc>1 && strcmp(argv[1],"--stationary-native-cursor-only")==0;
    const bool captureOnly = argc>1 && strcmp(argv[1],"--capture-only")==0;
    const bool effectsOnly = argc>1 && strcmp(argv[1],"--drawing-effects-only")==0;
    const bool selectionOnly = argc>1 && strcmp(argv[1],"--selection-only")==0;
    const bool multiMonitorOnly = argc>1 && strcmp(argv[1],"--multimonitor-only")==0;
    const bool snipOnly = argc>1 && strcmp(argv[1],"--snip-only")==0;
    const bool animationOnly = argc>1 && strcmp(argv[1],"--live-animation-only")==0;
    const bool policyOnly = argc>1 && strcmp(argv[1],"--mode-policy-only")==0;
    const bool zoomOnly = argc>1 && strcmp(argv[1],"--zoom-granularity-only")==0;
    const bool wheelOnly = argc>1 && strcmp(argv[1],"--live-wheel-only")==0;
    const bool indicatorOnly = argc>1 && strcmp(argv[1],"--indicator-only")==0;
    std::wstring capturePath;
    if(argc>2) {capturePath=std::filesystem::absolute(argv[2]).wstring();aboutCapturePath=capturePath.c_str();}
    else {
        wchar_t image[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"ZOOMIT_TEST_LIVE_OPTIONS_CAPTURE",image,MAX_PATH)) {
            captureLiveOptions=true;
            capturePath=std::filesystem::absolute(image).wstring();aboutCapturePath=capturePath.c_str();
        } else if(GetEnvironmentVariableW(L"ZOOMIT_TEST_DRAW_OPTIONS_CAPTURE",image,MAX_PATH)) {
            captureDrawOptions=true;
            capturePath=std::filesystem::absolute(image).wstring();aboutCapturePath=capturePath.c_str();
        } else if(GetEnvironmentVariableW(L"ZOOMIT_TEST_OPTIONS_CAPTURE",image,MAX_PATH)) {
            capturePath=std::filesystem::absolute(image).wstring();aboutCapturePath=capturePath.c_str();
        }
    }
    POINT oldCursor{}; GetCursorPos(&oldCursor);
    // Keep isolation alive while failure cleanup destroys application windows.
    std::unique_ptr<ProcessRegistryFixture> isolatedRegistry;
    bool nativeUiInitialized=false;
    try {
        isolatedRegistry=std::make_unique<ProcessRegistryFixture>();
        // Prove callback failures survive native boundaries without becoming app errors.
        TestCallbackBoundary(nullptr,0,[]{throw std::runtime_error("callback-probe");});
        bool callbackProbe=false;
        try {RethrowTestCallbackFailure();} catch(const std::runtime_error& error) {callbackProbe=strcmp(error.what(),"callback-probe")==0;}
        require(callbackProbe,"Callback assertion evidence must reach the test driver");
        const auto logFolder = std::filesystem::current_path() / ("regression-errors-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(logFolder);
        SetEnvironmentVariableW(L"ZOOMIT_TEST_LOG_DIRECTORY", logFolder.c_str());
        if(captureOnly) {
            const auto result=RunCaptureRegression();isolatedRegistry->VerifyUntouched();PrintCaptureResults(result);return 0;
        }
        if(effectsOnly) {
            const auto result=RunDrawingEffectsRegression();isolatedRegistry->VerifyUntouched();PrintDrawingEffectsResults(result);return 0;
        }
        require(g_ToggleKey==((HOTKEYF_CONTROL<<8)|'1') &&
                g_LiveZoomToggleKey==((HOTKEYF_CONTROL<<8)|'2') &&
                g_DrawToggleKey==((HOTKEYF_CONTROL<<8)|'3') &&
                g_BreakToggleKey==((HOTKEYF_CONTROL<<8)|'4') &&
                g_SnipToggleKey==((HOTKEYF_CONTROL<<8)|'5'),"Default shortcuts must follow requested order");
        g_ToggleKey='Z'; require(MigrateHotkeys(),"Old shortcut schema must migrate");
        require(g_ToggleKey==((HOTKEYF_CONTROL<<8)|'1'),"Migrated zoom shortcut");
        g_ToggleKey='Z'; require(!MigrateHotkeys() && g_ToggleKey=='Z',"Later customized shortcuts must be preserved");
        require(DecodeZoomLevel(EncodeZoomLevel(1.25f))==1.25f,"Fractional zoom must survive messages");
        g_SliderZoomLevel=0xffffffff; g_InitialZoomPercent=0xffffffff; g_RootPenWidth=0xffffffff;
        g_BreakTimeout=0xffffffff; g_BreakTimerPosition=0xffffffff;
        ValidateSettings();
        require(g_SliderZoomLevel==3 && g_RootPenWidth==19 &&
                g_BreakTimeout==99 && g_BreakTimerPosition==8,"Corrupt settings must be clamped");
        g_SliderZoomLevel=0; g_RootPenWidth=5; g_BreakTimeout=10;
        {
            const std::wstring key = L"Software\\ZoomItCustom\\Regression_" + std::to_wstring(GetCurrentProcessId());
            struct RegistryCleanup {
                std::wstring path;
                ~RegistryCleanup() { RegDeleteTree(HKEY_CURRENT_USER,path.c_str()); }
            } cleanup{key};
            ClassRegistry settings(key.c_str());
            DWORD wordArray[2]{0x10002,0xffffffff}, number=42;
            BOOLEAN flag=TRUE;
            wchar_t text[16]=L"hello";
            BYTE binary[4]{1,2,3,4};
            REG_SETTING table[]{
                {L"Number",SETTING_TYPE_DWORD,0,&number,42},
                {L"Flag",SETTING_TYPE_BOOLEAN,0,&flag,1},
                {L"Text",SETTING_TYPE_STRING,sizeof(text),text,0},
                {L"Binary",SETTING_TYPE_BINARY,sizeof(binary),binary,0},
                {L"Array",SETTING_TYPE_DWORD_ARRAY,sizeof(wordArray),wordArray,0},
                {} };
            settings.WriteRegSettings(table);
            number=0; flag=FALSE; text[0]=0; memset(binary,0,sizeof(binary)); memset(wordArray,0,sizeof(wordArray));
            settings.ReadRegSettings(table);
            require(number==42 && flag && wcscmp(text,L"hello")==0 && binary[3]==4 &&
                    wordArray[0]==0x10002 && wordArray[1]==0xffffffff,"Registry values must round-trip without truncation");
            HKEY handle{}; require(RegOpenKeyEx(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE|KEY_QUERY_VALUE,&handle)==ERROR_SUCCESS,"Test registry open");
            DWORD bytes{};
            RegQueryValueEx(handle,L"Text",nullptr,nullptr,nullptr,&bytes);
            require(bytes==6*sizeof(wchar_t),"Registry strings must include their terminator");
            RegSetValueEx(handle,L"Number",0,REG_SZ,reinterpret_cast<const BYTE*>(L"bad"),8);
            BYTE shortBinary=9; RegSetValueEx(handle,L"Binary",0,REG_BINARY,&shortBinary,1);
            RegCloseKey(handle);
            settings.ReadRegSettings(table);
            require(number==42 && binary[0]==1 && binary[3]==4,"Malformed registry data must preserve valid defaults");
        }
        P_DRAW_UNDO history=nullptr;
        DeleteOldestUndo(&history);
        require(GetOldestUndo(history)==nullptr,"Empty undo history");
        HDC screen=CreateDC(L"DISPLAY",nullptr,nullptr,nullptr);
        HDC dc=CreateCompatibleDC(screen);
        HBITMAP bitmap=CreateCompatibleBitmap(screen,1920,1080);
        SelectObject(dc,bitmap);
        const DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        for(int i=0;i<40;++i) {
            SetPixel(dc,0,0,RGB(i,0,0));
            PushDrawUndo(dc,&history,1920,1080);
        }
        size_t count=0;
        for(auto item=history;item;item=item->Next) ++count;
        require(count<=8,"Undo history must stay within 64 MiB at 1080p");
        SetPixel(dc,0,0,RGB(255,0,0));
        require(PopDrawUndo(dc,&history,1920,1080),"Undo available");
        require(GetPixel(dc,0,0)==RGB(39,0,0),"Undo must restore image content");
        DeleteDrawUndoList(&history);
        const DWORD undoGdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        if(undoGdiAfter>gdiBefore)
            std::cerr<<"Undo resource mismatch: before="<<gdiBefore<<" after="<<undoGdiAfter<<"\n";
        require(undoGdiAfter<=gdiBefore,"Undo must release every GDI object");
        DeleteDC(dc); DeleteObject(bitmap); DeleteDC(screen);
        require(g_GraphicsInit.Ready(),"Regression effects must use the application's single GDI+ initializer");
        size_t effectsBefore{}, effectsAfter{}, effectMemoryBefore{}, effectMemoryAfter{};
        double effectsMilliseconds{};
        {
            DrawingDib canvas(screen = CreateDC(L"DISPLAY", nullptr, nullptr, nullptr),
                              Gdiplus::Rect(0, 0, 256, 256));
            require(canvas.pixels()!=nullptr, "Drawing canvas");
            Gdiplus::SolidBrush brush(Gdiplus::Color(128, 255, 255, 0));
            Gdiplus::Pen pen(Gdiplus::Color(255,255,255,0), 6);
            g_PenWidth=6; g_PenColor=0x8000ffff;
            RECT fill{0,0,256,256}; FillRect(canvas.dc(), &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,70,70);
            require(GetPixel(canvas.dc(),40,40) == BlendColors(RGB(255,255,255),ColorFromColorRef(g_PenColor)),
                    "Highlighter must preserve intended color");
            DrawBlurredShape(DRAW_ELLIPSE, &pen, canvas.dc(), nullptr, 20,20,70,70);
            effectsBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            effectMemoryBefore=privateBytes();
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<400;++i) {
                DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,70,70);
                DrawBlurredShape(DRAW_ELLIPSE, &pen, canvas.dc(), nullptr, 20,20,70,70);
                DrawMaskedLine(canvas.dc(),canvas.dc(),Gdiplus::Rect(10,10,80,40),{15,15},{75,35},&pen,false);
            }
            effectsMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            effectMemoryAfter=privateBytes();
            require(effectMemoryAfter <= effectMemoryBefore + 2*1024*1024,"Drawing effects must not accumulate heap allocations");
            effectsAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            require(effectsAfter==effectsBefore, "Highlight and blur must release GDI resources");
            DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,20,20);
        }
        DeleteDC(screen);
        g_PenColor=COLOR_RED | 0xff000000; g_PenWidth=5;
        g_TestMode=true; g_ShowTrayIcon=false; g_OptionsShown=true;
        g_ToggleKey=g_DrawToggleKey=g_LiveZoomToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
        g_AnimateZoom=FALSE;
        pMagInitialize=MagInitialize;
        pMagSetWindowSource=MagSetWindowSource;
        pMagSetWindowTransform=MagSetWindowTransform;
        pMagSetWindowFilterList=MagSetWindowFilterList;
        pMagSetFullscreenTransform=MagSetFullscreenTransform;
        pMagSetInputTransform=TestInputTransform;
        pMagShowSystemCursor=TestShowSystemCursor;
        pSetLayeredWindowAttributes=SetLayeredWindowAttributes;
        pGetMonitorInfo=GetMonitorInfoA;
        pMonitorFromPoint=MonitorFromPoint;
        pDwmIsCompositionEnabled=reinterpret_cast<type_pDwmIsCompositionEnabled>(GetProcAddress(LoadLibrary(L"dwmapi.dll"), "DwmIsCompositionEnabled"));
        g_OsVersion=WIN10_VERSION;
        require(MagInitialize()!=FALSE,"Magnification API initialization");
        nativeUiInitialized=true;
        g_hWndMain=InitInstance(GetModuleHandle(nullptr),SW_HIDE);
        require(g_hWndMain!=nullptr,"Main window creation");
        g_SliderZoomLevel=0; g_AnimateZoom=FALSE;g_AnimateLiveZoom=FALSE;
        if(stationaryNativeOnly) {
            const int result=RunNativeStationaryCursorRegression();
            DestroyWindow(g_hWndMain);MagUninitialize();
            // Restore the original position only after every observation has ended.
            if(result!=77)SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();return result;
        }
        WNDCLASS wc{}; wc.lpfnWndProc=DefWindowProc; wc.hInstance=GetModuleHandle(nullptr); wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.lpszClassName=L"ZoomItTestHost"; RegisterClass(&wc);
        // Keep native cursor and capture probes over this process-owned temporary host.
        // Ambient browser panels may otherwise stay above a normal host and supply their
        // own hover cursor even when the test thread has correctly restored its cursor.
        // The host is destroyed at fixture exit; production magnifiers/canvases are shown above it.
        HWND host=CreateWindowEx(WS_EX_TOPMOST,L"ZoomItTestHost",L"ZoomIt regression tests",WS_OVERLAPPEDWINDOW,0,0,
                                 GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),
                                 nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
        ShowWindow(host,SW_SHOW);
        ActivateTestHost(host);
        SetCursorPos(125,125);
        if(selectionOnly) {
            const auto result=RunSelectionRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintSelectionResults(result);return 0;
        }
        if(multiMonitorOnly) {
            const auto result=RunMultiMonitorRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintMultiMonitorResults(result);return 0;
        }
        if(indicatorOnly) {
            const auto indicator=RunIndicatorRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();
            SetCursorPos(oldCursor.x,oldCursor.y);isolatedRegistry->VerifyUntouched();PrintIndicatorResults(indicator);return 0;
        }
        if(wheelOnly) {
            const auto wheel=RunLiveWheelRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();
            SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintLiveWheelResults(wheel);
            return 0;
        }
        if(zoomOnly) {
            const auto zoom=RunZoomGranularityRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();
            SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintZoomGranularityResults(zoom);
            return 0;
        }
        if(policyOnly) {
            const auto policy=RunModePolicyRegression(host);
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();
            SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintModePolicyResults(policy);
            return 0;
        }
        const LiveAnimationResults liveAnimation=animationOnly ? RunLiveAnimationRegression(host) : LiveAnimationResults{};
        if(animationOnly) {
            DestroyWindow(g_hWndMain);DestroyWindow(host);MagUninitialize();
            SetCursorPos(oldCursor.x,oldCursor.y);
            isolatedRegistry->VerifyUntouched();PrintLiveAnimationResults(liveAnimation);
            return 0;
        }
        size_t ignoredZoomToggles=0, ignoredZoomMenuCommands=0, liveZoomIgnoreCases=0;
        auto modeState=[&]{return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
        auto checkIgnoredZoom=[&] {
            const auto mode=modeState();
            const HWND live=g_hWndLiveZoom, magnifier=g_hWndLiveZoomMag;
            const auto style=GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE);
            const bool canvasVisible=IsWindowVisible(g_hWndMain)!=FALSE;
            const float level=*reinterpret_cast<float*>(SendMessage(live,WM_USER_GET_ZOOM_LEVEL,0,0));
            const RECT view=*reinterpret_cast<RECT*>(SendMessage(live,WM_USER_GET_SOURCE_RECT,0,0));
            const LONG magnifierStyle=IsWindow(magnifier)?GetWindowLong(magnifier,GWL_STYLE):0;
            const HDC canvas=reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));
            std::vector<BYTE> before;
            if(canvasVisible) {
                require(canvas!=nullptr,"Drawing canvas must be present before testing ignored Zoom");
                DrawingDib region(canvas,Gdiplus::Rect(120,120,80,60));
                require(region.pixels()!=nullptr,"Capture drawing pixels before ignored Zoom");
                before.assign(region.pixels(),region.pixels()+80*60*4);
            }
            auto unchanged=[&] {
                require(g_hWndLiveZoom==live && IsWindowVisible(live) && g_hWndLiveZoomMag==magnifier &&
                        modeState()==mode && (IsWindowVisible(g_hWndMain)!=FALSE)==canvasVisible &&
                        GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)==style,
                        "Zoom toggle in LiveZoom must preserve the current windows and drawing mode");
                const RECT after=*reinterpret_cast<RECT*>(SendMessage(live,WM_USER_GET_SOURCE_RECT,0,0));
                require(*reinterpret_cast<float*>(SendMessage(live,WM_USER_GET_ZOOM_LEVEL,0,0))==level &&
                        EqualRect(&after,&view) && (!IsWindow(magnifier)||GetWindowLong(magnifier,GWL_STYLE)==magnifierStyle),
                        "Ignored Zoom toggle must preserve live magnification, view and pointer style");
                if(canvasVisible) {
                    require(reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0))==canvas,
                            "Ignored Zoom must retain the drawing canvas");
                    DrawingDib region(canvas,Gdiplus::Rect(120,120,80,60));
                    require(region.pixels()!=nullptr,"Capture drawing pixels after ignored Zoom");
                    for(size_t pixel=0;pixel<80*60;++pixel)
                        for(size_t channel=0;channel<3;++channel)
                            require(region.pixels()[pixel*4+channel]==before[pixel*4+channel],
                                    "Ignored Zoom must preserve annotation pixels");
                }
            };
            for(LPARAM shortcut : {MAKELPARAM(MOD_CONTROL,'1'),MAKELPARAM(MOD_CONTROL|MOD_ALT,VK_F24)}) {
                SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,shortcut);
                unchanged();++ignoredZoomToggles;
            }
            SendMessage(g_hWndMain,WM_COMMAND,IDC_ZOOM,0);
            unchanged();++ignoredZoomMenuCommands;++liveZoomIgnoreCases;
        };
        for(bool fullscreen : {false,true}) {
            g_fullScreenWorkaround=fullscreen;
            SetCursorPos(125,125);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
            require(IsWindowVisible(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain) && modeState()==0,
                    "LiveZoom must start without a frozen canvas or implicit Draw");
            checkIgnoredZoom();pump(20);
            require(modeState()==0 && !IsWindowVisible(g_hWndMain),"Ignored Zoom must not queue a delayed transition");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(30);
            require(IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom) && (modeState()&3)==3,
                    "The original LiveDraw command must still work through its internal capture request");
            checkIgnoredZoom();
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            require((modeState()&7)==7,"Drawing stroke must be active before ignored Zoom");
            checkIgnoredZoom();
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            require((modeState()&7)==3,"Drawing must remain active after completing a stroke");
            checkIgnoredZoom();pump(10);
            require(IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom),
                    "Ignored menu Zoom must not queue destruction of LiveDraw");
            SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(100);
            if(IsWindowVisible(g_hWndLiveZoom)) SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(30);require(!IsWindow(g_hWndLiveZoom),"LiveZoom must still close normally");
            ActivateTestHost(host);
        }
        g_fullScreenWorkaround=true;
        denyInputTransform=true;
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
        require(!IsWindow(g_hWndLiveZoom) && modeState()==0 && systemCursorShown,
                "Denied fullscreen input transform must return to idle and successfully request system-cursor visibility");
        denyInputTransform=false;
        g_fullScreenWorkaround=false;
        size_t suspensionCases=0, suspensionCycles=0, suspendedSnipCases=0, nativeResumeCases=0;
        size_t ringChecks=0;
        int ringVariant=-1;
        const char* ringContext="suspension variants";
        auto currentCanvas=[&]{return reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));};
        auto canvasPixels=[&] {
            DrawingDib image(currentCanvas(),Gdiplus::Rect(100,100,300,200));
            require(image.pixels()!=nullptr,"Read annotation pixels for suspension tests");
            return std::vector<BYTE>(image.pixels(),image.pixels()+300*200*4);
        };
        auto samePixels=[&](const std::vector<BYTE>& before) {
            const auto after=canvasPixels();
            for(size_t pixel=0;pixel<300*200;++pixel)
                for(size_t channel=0;channel<3;++channel)
                    require(before[pixel*4+channel]==after[pixel*4+channel],
                            "Suspension, mouse movement and colour changes must not alter annotations");
        };
        auto verifyRing=[&](bool savePreview=false) {
            require((modeState()&23)==17,"First right click must suspend Draw without leaving zoom or tracing");
            const HCURSOR ring=reinterpret_cast<HCURSOR>(SendMessage(g_hWndMain,WM_TEST_QUERY_SUSPENDED_CURSOR,0,0));
            require(ring && ring!=LoadCursor(nullptr,IDC_ARROW),"Suspension must use the coloured circle");
            CURSORINFO pointer{sizeof(pointer)};
            ++ringChecks;
            const BOOL inspected=GetCursorInfo(&pointer);
            const DWORD inspectError=inspected ? ERROR_SUCCESS : GetLastError();
            const bool logicalRingPresent=inspected && (pointer.flags&CURSOR_SHOWING) && pointer.hCursor==ring;
            if(!logicalRingPresent) {
                POINT position{};GetCursorPos(&position);
                HWND surface=WindowFromPoint(position);
                wchar_t surfaceClass[96]{};GetClassNameW(surface,surfaceClass,_countof(surfaceClass));
                std::cerr<<"Ring mismatch: context="<<ringContext<<" variant="<<ringVariant<<" check="<<ringChecks
                         <<" suspension_cases="<<suspensionCases<<" cycles="<<suspensionCycles
                         <<" mode="<<modeState()<<" fullscreen="<<g_fullScreenWorkaround
                         <<" live_visible="<<IsWindowVisible(g_hWndLiveZoom)
                         <<" animate_zoom="<<unsigned(g_AnimateZoom)<<" animate_live="<<unsigned(g_AnimateLiveZoom)
                         <<" cursor_read="<<inspected<<" cursor_error="<<inspectError<<" cursor_flags="<<pointer.flags
                         <<" cursor="<<pointer.hCursor<<" expected="<<ring
                         <<" last_mag_visibility_request="<<systemCursorShown<<" thread_cursor="<<GetCursor()
                         <<" foreground="<<GetForegroundWindow()<<" capture="<<GetCapture()
                         <<" main="<<g_hWndMain<<" mouse="<<position.x<<","<<position.y<<" surface="<<surface<<"\n";
                std::wcerr<<L"Ring mismatch surface class: "<<surfaceClass<<L"\n";
            }
            require(logicalRingPresent,"Suspension must expose CURSOR_SHOWING with the logical circle-cursor handle");
            require(GetCapture()==g_hWndMain,"Suspension must receive both buttons even through transparent LiveDraw");
            require(systemCursorShown,"Suspension ring must successfully request system-cursor visibility");
            ICONINFO icon{};require(GetIconInfo(ring,&icon)!=FALSE,"Inspect suspension cursor");
            BITMAP bitmap{};GetObject(icon.hbmColor,sizeof(bitmap),&bitmap);
            HDC screen=GetDC(nullptr);
            {
                DrawingDib image(screen,Gdiplus::Rect(0,0,bitmap.bmWidth+16,bitmap.bmHeight+16));
                HBRUSH background=CreateSolidBrush(RGB(240,240,240));
                RECT area{0,0,bitmap.bmWidth+16,bitmap.bmHeight+16};FillRect(image.dc(),&area,background);DeleteObject(background);
                require(DrawIconEx(image.dc(),8,8,ring,bitmap.bmWidth,bitmap.bmHeight,0,nullptr,DI_NORMAL)!=FALSE,
                        "Render suspension cursor");
                const int centre=bitmap.bmWidth/2, radius=bitmap.bmWidth/4;
                require(GetPixel(image.dc(),8+centre,8+centre)==RGB(32,32,32),"Suspension circle must have a dark interior");
                require(GetPixel(image.dc(),8+centre+radius-1,8+centre)==(g_PenColor&0xFFFFFF),
                        "Suspension circle border must match the active drawing colour");
                require(GetPixel(image.dc(),8,8)==RGB(240,240,240),"Cursor corners must be transparent");
                if(savePreview && aboutCapturePath) {
                    auto path=std::filesystem::path(aboutCapturePath).parent_path()/L"suspension-pointer-1.1.3.png";
                    HBITMAP imageBitmap=static_cast<HBITMAP>(GetCurrentObject(image.dc(),OBJ_BITMAP));
                    require(SavePng(const_cast<wchar_t*>(path.c_str()),imageBitmap)==ERROR_SUCCESS,"Save suspension pointer preview");
                }
            }
            ReleaseDC(nullptr,screen);DeleteObject(icon.hbmColor);DeleteObject(icon.hbmMask);
        };
        auto rawMouse=[&] {POINT point{};GetCursorPos(&point);ScreenToClient(g_hWndMain,&point);return point;};
        auto resume=[&](POINT client) {
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(client.x,client.y));
            require((modeState()&23)==7,"One left click must restore Draw and start a new stroke immediately");
            require(GetCapture()!=g_hWndMain && !SendMessage(g_hWndMain,WM_TEST_QUERY_SUSPENDED_CURSOR,0,0),
                    "Resuming Draw must release suspension capture and ring");
            if(IsWindowVisible(g_hWndLiveZoom))
                require(!systemCursorShown,"Resumed LiveDraw must successfully request system-cursor hiding, including fullscreen magnification");
            const POINT point=rawMouse();
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x+8,point.y));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(point.x+8,point.y));
            require((modeState()&23)==3,"Resumed stroke must finish in the original Draw mode");
        };
        for(int variant=0;variant<7;++variant) {
            ringVariant=variant;ringContext="suspension variants";
            ++suspensionCases;
            g_AnimateZoom=FALSE;g_fullScreenWorkaround=variant==3 || variant==5;
            g_DrawPointer=variant==6;
            g_PenColor=COLOR_RED|0xff000000;g_RootPenWidth=5;g_PenWidth=5;
            SetCursorPos(125,125);
            if(variant>=2 && variant<=5) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
                SendMessage(g_hWndMain,WM_HOTKEY,variant>=4 ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0);pump(30);
            } else if(variant==1) {
                SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1'));pump(60);
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            } else {
                SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(30);
            }
            SendMessage(g_hWndMain,WM_MOUSEMOVE,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(160,150));
            // Also exercise suspension during a stroke, before its mouse-up arrives.
            if(variant%2==0) SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(160,150));
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(160,150));SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
            verifyRing(variant==0);
            const auto before=canvasPixels();
            RECT viewBefore{},viewAfter{};
            const auto level=SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,reinterpret_cast<LPARAM>(&viewBefore));
            for(int i=0;i<8;++i) {
                SendMessage(g_hWndMain,WM_SETCURSOR,reinterpret_cast<WPARAM>(g_hWndMain),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
                SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(210+i,210));
            }
            POINT moved{270,230};ClientToScreen(g_hWndMain,&moved);SetCursorPos(moved.x,moved.y);pump(20);
            verifyRing();samePixels(before);
            require(SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,reinterpret_cast<LPARAM>(&viewAfter))==level &&
                    EqualRect(&viewBefore,&viewAfter),"Suspended static view must not pan when moving its cursor");
            SendMessage(g_hWndMain,WM_KEYDOWN,'G',0);
            verifyRing();samePixels(before);
            if(variant==4 || variant==5) checkIgnoredZoom();
            if(variant==1) {
                cancelSnip=false;SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                require(g_TestSnipBitmap!=nullptr,"Snip must work while static Draw is suspended");
                DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;++suspendedSnipCases;
                verifyRing();samePixels(before);
            }
            // Resuming must not connect the old stroke to the new mouse position.
            const POINT click=rawMouse();
            const float factor=DecodeZoomLevel(level);
            const POINT next{viewBefore.left+static_cast<LONG>(click.x/factor),
                             viewBefore.top+static_cast<LONG>(click.y/factor)};
            const POINT between{(160+next.x)/2,(150+next.y)/2};
            const COLORREF beforeBetween=GetPixel(currentCanvas(),between.x,between.y);
            if(variant==4) {
                SetForegroundWindow(g_hWndMain);
                require(GetForegroundWindow()==g_hWndMain,"LiveDraw must own the foreground before native click tests");
                INPUT clickInput{};clickInput.type=INPUT_MOUSE;clickInput.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
                bool nativeButtonHeld=false;
                auto releaseNativeButton=zoomit::OnExit([&] {
                    if(nativeButtonHeld) {INPUT release{};release.type=INPUT_MOUSE;release.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&release,sizeof(release));}
                });
                const auto modeBeforeDown=modeState();const SHORT asyncBeforeDown=GetAsyncKeyState(VK_LBUTTON);
                require(SendInput(1,&clickInput,sizeof(clickInput))==1,"Send real left button down");nativeButtonHeld=true;pump(20);
                require((modeState()&23)==7 && GetCapture()!=g_hWndMain,"The real left click must reach paused transparent LiveDraw and resume its stroke");
                const auto modeBeforeUp=modeState();const SHORT asyncBeforeUp=GetAsyncKeyState(VK_LBUTTON);
                const HWND foregroundBeforeUp=GetForegroundWindow(),captureBeforeUp=GetCapture();
                clickInput.mi.dwFlags=MOUSEEVENTF_LEFTUP;
                require(SendInput(1,&clickInput,sizeof(clickInput))==1,"Send real left button up");nativeButtonHeld=false;
                // Native release delivery and idle missed-release repair have a bounded deadline.
                const ULONGLONG releaseDeadline=GetTickCount64()+50;
                do {pump(2);} while((modeState()&23)!=3 && GetTickCount64()<releaseDeadline);
                if((modeState()&23)!=3) {
                    POINT pointer{};GetCursorPos(&pointer);const HWND surface=WindowFromPoint(pointer);
                    wchar_t surfaceClass[96]{};GetClassNameW(surface,surfaceClass,_countof(surfaceClass));
                    std::cerr<<"Native resumed release mismatch: variant="<<ringVariant<<" mode_before_down="<<modeBeforeDown
                             <<" mode_before_up="<<modeBeforeUp<<" mode_after_up="<<modeState()
                             <<" async_before_down="<<asyncBeforeDown<<" async_before_up="<<asyncBeforeUp
                             <<" async_after_up="<<GetAsyncKeyState(VK_LBUTTON)
                             <<" foreground_before="<<foregroundBeforeUp<<" foreground_after="<<GetForegroundWindow()
                             <<" capture_before="<<captureBeforeUp<<" capture_after="<<GetCapture()
                             <<" pointer="<<pointer.x<<","<<pointer.y<<" surface="<<surface<<" main="<<g_hWndMain<<"\n";
                    std::wcerr<<L"Native resumed release surface: "<<surfaceClass<<L"\n";
                }
                require((modeState()&23)==3,"The real resumed stroke must finish normally");++nativeResumeCases;
            } else {
                resume(click);
            }
            require(GetPixel(currentCanvas(),between.x,between.y)==beforeBetween,
                    "Resuming must not draw a line connecting the previous stroke to the new position");
            require((g_DrawPointer!=FALSE)==(variant==6),"Resume must preserve the previous coloured pointer shape");
            require((g_PenColor&0xFFFFFF)==COLOR_GREEN,"Resume must preserve the selected active colour");
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
            const auto modeBeforeExit=modeState();const HWND focusBeforeExit=GetFocus();
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);pump(100);
            if(IsWindowVisible(g_hWndMain) || (modeState()&16) || GetCapture()==g_hWndMain) {
                POINT pointer{};GetCursorPos(&pointer);const HWND surface=WindowFromPoint(pointer);
                wchar_t surfaceClass[96]{};GetClassNameW(surface,surfaceClass,_countof(surfaceClass));
                std::cerr<<"Second right-click exit mismatch: variant="<<ringVariant<<" context="<<ringContext
                         <<" before="<<modeBeforeExit<<" after="<<modeState()<<" fullscreen="<<g_fullScreenWorkaround
                         <<" main_visible="<<IsWindowVisible(g_hWndMain)<<" main_style="<<GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)
                         <<" live="<<g_hWndLiveZoom<<" live_visible="<<IsWindowVisible(g_hWndLiveZoom)
                         <<" capture="<<GetCapture()<<" main="<<g_hWndMain<<" foreground="<<GetForegroundWindow()
                         <<" focus_before="<<focusBeforeExit<<" focus_after="<<GetFocus()<<" pointer="<<pointer.x<<","<<pointer.y
                         <<" surface="<<surface<<"\n";
                std::wcerr<<L"Second right-click exit surface: "<<surfaceClass<<L"\n";
            }
            require(!IsWindowVisible(g_hWndMain) && !(modeState()&16) && GetCapture()!=g_hWndMain,
                    "Second right click must completely leave Draw and release mouse capture");
            require(!(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),"Second right click must clear LiveDraw's transparent window mode");
            if(IsWindowVisible(g_hWndLiveZoom)) SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(30);
            const CURSORINFO pointer=ResetThenReadOwnedNormalPointer(host,"Completed suspension exit");
            require((pointer.flags&CURSOR_SHOWING) && pointer.hCursor==LoadCursor(nullptr,IDC_ARROW),
                    "Fixture-reset suspension exits must expose CURSOR_SHOWING with the logical arrow cursor");
        }
        g_DrawPointer=FALSE;g_fullScreenWorkaround=FALSE;
        SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(30);
        auto pauseResume=[&] {
            ringVariant=8;ringContext="repeated pause/resume";
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
            SendMessage(g_hWndMain,WM_KEYDOWN,suspensionCycles%2 ? 'B' : 'R',0);
            verifyRing();resume(rawMouse());++suspensionCycles;
        };
        for(int i=0;i<12;++i) pauseResume();
        const DWORD suspendedGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const DWORD suspendedUser=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        for(int i=0;i<64;++i) pauseResume();
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==suspendedGdi &&
                GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)==suspendedUser,
                "Repeated suspend/resume and colour changes must not leak cursors, bitmaps or capture");
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(70);
        require(!IsWindowVisible(g_hWndMain) && !(modeState()&16) && GetCapture()!=g_hWndMain,
                "Escape must clear suspension and mouse capture");
        for(bool fullscreen : {false,true}) {
            ringVariant=fullscreen ? 10 : 9;ringContext="suspended LiveDraw hotkey exit";
            g_fullScreenWorkaround=fullscreen;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(70);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(30);
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(30);
            require(!IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) && GetCapture()!=g_hWndMain && !(modeState()&16),
                    "Ctrl+2 must close suspended LiveDraw and release capture");
        }
        g_fullScreenWorkaround=FALSE;

        SetCursorPos(125,125);ActivateTestHost(host);pump(10);
        size_t drawCycleCount=0;
        auto runCycle=[&](bool fullscreen, bool liveDraw=false, bool exitWithLiveHotkey=false) {
            ringVariant=fullscreen ? 12 : 11;ringContext="LiveDraw regression cycle";
            ++drawCycleCount;
            g_fullScreenWorkaround=fullscreen;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(100);
            require(IsWindowVisible(g_hWndLiveZoom),"LiveZoom must start");
            SendMessage(g_hWndMain,WM_HOTKEY,liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0);
            pump(30);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            for(int x=151;x<175;++x) SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_KEYDOWN,'T',0);
            for(wchar_t letter : std::wstring(L"Test")) SendMessage(g_hWndMain,WM_CHAR,letter,0);
            SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
            if(liveDraw && exitWithLiveHotkey) {
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(30);
                require(!IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),"LiveZoom shortcut must close active LiveDraw synchronously");
                const CURSORINFO cursor=ResetThenReadOwnedNormalPointer(host,"Closing active LiveDraw");
                require((cursor.flags&CURSOR_SHOWING) && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),"Fixture-reset LiveDraw exit must expose CURSOR_SHOWING with the logical arrow cursor");
                return;
            }
            if(liveDraw) {
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
                SendMessage(g_hWndMain,WM_USER_EXIT_MODE,0,0);
                pump(15);
                verifyRing();
                if(!fullscreen) require(!(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR),
                                        "Paused LiveDraw must show one suspension ring without a duplicate magnified cursor");
                SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);
            } else SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,SHALLOW_DESTROY);
            pump(100);
            require(IsWindowVisible(g_hWndLiveZoom),"Exiting static drawing must restore LiveZoom");
            float level=*reinterpret_cast<float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
            require(level>1.0f,"Restored LiveZoom must retain fractional magnification");
            if(!fullscreen)
                require(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR,"Magnified cursor must be restored");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(100);
            require(!IsWindow(g_hWndLiveZoom),"LiveZoom must close");
            const CURSORINFO cursor=ResetThenReadOwnedNormalPointer(host,"Exiting LiveZoom after drawing");
            require((cursor.flags&CURSOR_SHOWING)!=0,"Fixture-reset LiveZoom exit must expose the CURSOR_SHOWING flag");
        };
        size_t liveToggleCount = 0;
        auto resetAndCheckLogicalPointer = [&] {
            const CURSORINFO cursor=ResetThenReadOwnedNormalPointer(host,"LiveZoom toggle exit");
            require((cursor.flags & CURSOR_SHOWING) && cursor.hCursor,
                    "Fixture-reset LiveZoom exit must expose CURSOR_SHOWING and a non-null logical cursor handle");
            if(cursor.hCursor!=LoadCursor(nullptr,IDC_ARROW)) {
                POINT point{};GetCursorPos(&point);wchar_t name[128]{};GetClassName(WindowFromPoint(point),name,_countof(name));
                std::wcerr<<L"Unexpected exit cursor "<<cursor.hCursor<<L" at "<<point.x<<L","<<point.y<<L" over "<<name<<L"\n";
            }
            require(cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),"Exit must restore the system arrow");
        };
        for (int delay : {0,1,3,7,12,100,300}) {
            if(snipOnly) break;
            g_fullScreenWorkaround=false;
            for (int i=0;i<40;++i) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
                ++liveToggleCount;
                require(IsWindowVisible(g_hWndLiveZoom)==((i%2)==0),"Each activation must toggle LiveZoom exactly once");
                if(i%2) resetAndCheckLogicalPointer();
                if(delay) pump(delay);
                if(i%2) resetAndCheckLogicalPointer();
            }
            require(!IsWindow(g_hWndLiveZoom),"Every complete sequence must close without forced cleanup");
            resetAndCheckLogicalPointer();
            ActivateTestHost(host);
        }
        {
            // Exercise the real Snip selection and capture without replacing the user's clipboard.
            require(g_GraphicsInit.Ready(),"Snip graphics must use the application initializer");
            wchar_t missing[]=L"Z:\\ZoomIt_missing_file_12345678.png";
            require(!LoadImageFile(missing),"Missing background image must fail cleanly");
            require(SavePng(missing,nullptr)!=ERROR_SUCCESS,"PNG failure must be reported");
            g_AnimateZoom=FALSE;
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1')); pump(100);
            auto snipGdi = GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            for(int i=0;i<6;++i) {
                cancelSnip=false;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                BITMAP cropped{};
                require(g_TestSnipBitmap && GetObject(g_TestSnipBitmap,sizeof(cropped),&cropped),"Snip must capture inside active zoom");
                require(cropped.bmWidth==81 && cropped.bmHeight==81,"Snip crop dimensions");
                DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
                require(IsWindowVisible(g_hWndMain),"Snip must preserve pre-existing static zoom");
                cancelSnip=true;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                require(!g_TestSnipBitmap && IsWindowVisible(g_hWndMain),"Cancelled Snip must preserve static zoom");
                cancelSnip=false;
                wchar_t temporary[MAX_PATH]; GetTempPath(MAX_PATH,temporary);
                const std::wstring imagePath=std::wstring(temporary)+L"ZoomItRegression_"+std::to_wstring(GetCurrentProcessId())+L".png";
                struct ImageCleanup {const wchar_t* path; ~ImageCleanup(){DeleteFile(path);}} imageCleanup{imagePath.c_str()};
                g_TestSavePath=imagePath.c_str(); g_TestSaveFilter= i%2 ? 2 : 1;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_SAVE_HOTKEY,0);
                {
                    Gdiplus::Bitmap image(imagePath.c_str());
                    require(image.GetLastStatus()==Gdiplus::Ok,"Snip PNG must be valid");
                    require(image.GetWidth()==(i%2 ? 64 : 81) && image.GetHeight()==(i%2 ? 64 : 81),"PNG must have requested scale");
                }
                DeleteFile(imagePath.c_str()); g_TestSavePath=nullptr;
                if(i==0) snipGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS); // Warm selection and image codecs once.
                require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==snipGdi,"Snip copy, save and cancellation must not leak GDI objects");
            }
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,SHALLOW_DESTROY); pump(30);
            resetAndCheckLogicalPointer();
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(100);
            cancelSnip=false; SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0); pump(100);
            require(g_TestSnipBitmap && IsWindowVisible(g_hWndLiveZoom),"Snip from LiveZoom must resume LiveZoom");
            DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(30);
            resetAndCheckLogicalPointer();
        }
        if(!snipOnly) {runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);}
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
        g_BreakOnSecondary=false; g_BreakShowBackgroundFile=false;
        SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
        SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,0); pump(10);
        const auto gdiBaseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userBaseline=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryBefore=privateBytes();
        for(int i=0;i<(snipOnly ? 0 : 12);++i) {
            g_AnimateZoom=(i%2)!=0;
            runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);
            DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
            SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,0); pump(10);
        }
        require(optionsValid,"Options must have six pages with About last, correct title/version and no recording or typing pages");
        const auto gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryAfter=privateBytes();
        require(gdiAfter<=gdiBaseline+1,"Repeated transitions must not leak GDI resources");
        require(userAfter<=userBaseline+1,"Repeated transitions must not leak USER resources");
        ringVariant=13;ringContext="quit while suspended";
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(70);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(20);
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
        DestroyWindow(g_hWndMain);
        require(GetCapture()!=g_hWndMain,"Quitting while suspended must release mouse capture");
        require(!IsWindow(g_hWndLiveZoom),"Quitting must destroy the magnifier");
        const CURSORINFO cursor=ResetThenReadOwnedNormalPointer(host,"Quitting while suspended");
        require((cursor.flags&CURSOR_SHOWING)!=0,"Fixture-reset quit cleanup must expose the CURSOR_SHOWING flag");
        DestroyWindow(host);
        MagUninitialize();
        SetCursorPos(oldCursor.x,oldCursor.y);
        isolatedRegistry->VerifyUntouched();
        std::cout<<"{\"passed\":true,\"cursor_check_scope\":\"logical-state-or-fixture-cleanup\",\"visual_cursor_verification\":false,\"live_draw_cycles\":"<<drawCycleCount<<",\"undo_1080p_entries\":"<<count
          <<",\"suspension_cases\":"<<suspensionCases<<",\"suspend_resume_cycles\":"<<suspensionCycles
          <<",\"native_resume_cases\":"<<nativeResumeCases<<",\"suspended_snip_cases\":"<<suspendedSnipCases<<",\"live_zoom_ignore_cases\":"<<liveZoomIgnoreCases<<",\"ignored_zoom_toggles\":"<<ignoredZoomToggles
          <<",\"ignored_zoom_menu_commands\":"<<ignoredZoomMenuCommands<<",\"live_toggle_events\":"<<liveToggleCount<<",\"effect_operations\":1200,\"effects_milliseconds\":"<<effectsMilliseconds
          <<",\"effect_gdi_before\":"<<effectsBefore<<",\"effect_gdi_after\":"<<effectsAfter
          <<",\"effect_private_before\":"<<effectMemoryBefore<<",\"effect_private_after\":"<<effectMemoryAfter
          <<",\"snip_cases\":19,\"options_open_close_cycles\":"<<(snipOnly?1:13)<<",\"timer_cycles\":"<<(snipOnly?1:13)
          <<",\"live_animation_immediate_cases\":"<<liveAnimation.immediateCases
          <<",\"live_animation_animated_cases\":"<<liveAnimation.animatedCases
          <<",\"live_animation_reverse_events\":"<<liveAnimation.reverseEvents
          <<",\"live_animation_draw_cases\":"<<liveAnimation.drawCases
          <<",\"live_animation_snip_cases\":"<<liveAnimation.snipCases
          <<",\"live_animation_reset_cases\":"<<liveAnimation.resetCases
          <<",\"live_animation_registry_cases\":"<<liveAnimation.registryCases
          <<",\"live_animation_options_cases\":"<<liveAnimation.optionsCases
          <<",\"live_animation_zoom_key_cases\":"<<liveAnimation.zoomKeyCases
          <<",\"live_animation_leak_cycles\":"<<liveAnimation.leakCycles
          <<",\"gdi_before\":"<<gdiBaseline<<",\"gdi_after\":"<<gdiAfter
          <<",\"user_before\":"<<userBaseline<<",\"user_after\":"<<userAfter
          <<",\"private_bytes_before\":"<<memoryBefore<<",\"private_bytes_after\":"<<memoryAfter<<"}\n";
        return 0;
    } catch(const std::exception& ex) {
        std::cerr<<"FAILED: "<<ex.what()<<"\n";
        if(nativeUiInitialized) {
            if(IsWindow(g_hWndMain))DestroyWindow(g_hWndMain);
            if(IsWindow(g_hWndLiveZoom))DestroyWindow(g_hWndLiveZoom);
            MagSetFullscreenTransform(1,0,0);MagShowSystemCursor(TRUE);MagUninitialize();
            ClipCursor(nullptr);SetCursorPos(oldCursor.x,oldCursor.y);
        }
        return 1;
    }
}





