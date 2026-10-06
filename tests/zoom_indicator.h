#pragma once
struct IndicatorResults {
    size_t zoomCases{},liveCases{},hiddenCases{},preferenceCases{},failureCases{},persistentCases{},cycles{};
    size_t maximumSurfaceBytes{};
    DWORD gdiBefore{},gdiAfter{},userBefore{},userAfter{};
};
bool indicatorSave{},indicatorChoice{};
size_t indicatorGuiCases{};
INT_PTR IndicatorOptionsProcImpl(HWND dialog,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_TIMER && wParam==96) {
        KillTimer(dialog,96);
        HWND page=g_OptionsTabs[ZOOM_PAGE].hPage,check=GetDlgItem(page,IDC_SHOW_ZOOM_INDICATOR);
        require(IsWindow(check),"Zoom options must expose the shared indicator checkbox");
        require((IsDlgButtonChecked(page,IDC_SHOW_ZOOM_INDICATOR)==BST_CHECKED)==(g_ShowZoomIndicator!=FALSE),
                "Checkbox must reflect the persisted preference");
        RECT control{},bounds{};GetWindowRect(check,&control);GetWindowRect(page,&bounds);
        require(control.left>=bounds.left && control.top>=bounds.top && control.right<=bounds.right && control.bottom<=bounds.bottom,
                "The indicator preference must fit inside the native options page");
        CheckDlgButton(page,IDC_SHOW_ZOOM_INDICATOR,indicatorChoice ? BST_CHECKED : BST_UNCHECKED);
        ++indicatorGuiCases;SendMessage(dialog,WM_COMMAND,indicatorSave ? IDOK : IDCANCEL,0);return TRUE;
    }
    const auto result=OptionsProc(dialog,message,wParam,lParam);
    if(message==WM_INITDIALOG)SetTimer(dialog,96,20,nullptr);
    return result;
}
INT_PTR CALLBACK IndicatorOptionsProc(HWND dialog,UINT message,WPARAM word,LPARAM param) noexcept {
    return TestDialogBoundary(dialog,[&]{return IndicatorOptionsProcImpl(dialog,message,word,param);});
}

std::function<void()> g_IndicatorOptionsModalCheck;
void CALLBACK CloseIndicatorFloorOptions(HWND,UINT,UINT_PTR timer,DWORD) noexcept {
    DWORD owner{};const HWND dialog=hWndOptions;
    if(!dialog || !GetWindowThreadProcessId(dialog,&owner) || owner!=GetCurrentProcessId())return;
    KillTimer(nullptr,timer);
    TestDialogBoundary(dialog,[&] {
        if(g_IndicatorOptionsModalCheck)g_IndicatorOptionsModalCheck();
        SendMessage(dialog,WM_COMMAND,IDCANCEL,0);return TRUE;
    });
}

IndicatorResults RunIndicatorRegression(HWND host) {
    IndicatorResults result{};
    const BOOLEAN savedEnabled=g_ShowZoomIndicator,savedZoom=g_AnimateZoom,savedLive=g_AnimateLiveZoom;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    const DWORD savedIndex=g_SliderZoomLevel,savedPercent=g_InitialZoomPercent,savedLegacy=g_LegacySliderZoomLevel;
    auto restore=zoomit::OnExit([&] {
        g_ShowZoomIndicator=savedEnabled;g_AnimateZoom=savedZoom;g_AnimateLiveZoom=savedLive;g_fullScreenWorkaround=savedFullscreen;
        g_SliderZoomLevel=savedIndex;g_InitialZoomPercent=savedPercent;g_LegacySliderZoomLevel=savedLegacy;
        zoomit::ZoomIndicator::denyPresentation=false;g_PolicyModalCheck={};g_IndicatorOptionsModalCheck={};
    });
    auto waitFor=[&](auto ready,const char* message) {
        const auto deadline=GetTickCount64()+3000;while(!ready() && GetTickCount64()<deadline)pump(5);require(ready(),message);
    };
    auto clean=[&] {SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(40);require(!g_ZoomIndicator.Visible() && !g_ZoomIndicator.SurfaceBytes(),"Idle must release indicator pixels and disable its display");};
    auto open=[&](bool live,bool animated) {
        clean();ActivateTestHost(host);SetCursorPos(125,125);pump(10);SetInitialZoomIndex(3);
        g_fullScreenWorkaround=FALSE;g_AnimateZoom=animated;g_AnimateLiveZoom=animated;
        SendMessage(g_hWndMain,WM_HOTKEY,live ? LIVE_HOTKEY : ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,live ? '2' : '1'));
    };
    auto liveLevel=[] {return *reinterpret_cast<const float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));};
    auto visible=[&](float value) {
        require(g_ZoomIndicator.Visible() && g_ZoomIndicator.Factor()==value,"The overlay must show the requested target, independently of intermediate animation frames");
        const bool persistent=value==1.0f && IsWindowVisible(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain);
        require(g_ZoomIndicator.Persistent()==persistent,"Only the pure LiveZoom floor must use a persistent indicator");
        require(g_ZoomIndicator.Opacity()==(persistent ? zoomit::ZoomIndicator::FloorOpacity : zoomit::ZoomIndicator::BriefOpacity),"The persistent LiveZoom floor must be more transparent than brief zoom changes");
        require(g_ZoomIndicator.ExpiryTimerActive()!=persistent && (persistent ? !g_ZoomIndicator.Expires() : g_ZoomIndicator.Expires()!=0),"Persistent floor status must have neither an expiry deadline nor an idle HUD timer");
        HWND window=g_ZoomIndicator.Window();const auto style=GetWindowLongPtr(window,GWL_EXSTYLE);
        require((style&(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW))==
                (WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW),"Indicator must remain translucent, non-activating and click-through");
        require(GetForegroundWindow()!=window && GetFocus()!=window && GetCapture()!=window,"Indicator must never take focus or mouse capture");
        require(SendMessage(window,WM_NCHITTEST,0,0)==HTTRANSPARENT && SendMessage(window,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE,
                "Native hit testing must reject clicks and activation");
        MONITORINFO info{};info.cbSize=sizeof(info);POINT at{0,0};GetMonitorInfoW(MonitorFromPoint(at,MONITOR_DEFAULTTOPRIMARY),&info);
        RECT position{};GetWindowRect(window,&position);
        require(position.left>=info.rcMonitor.left && position.top>=info.rcMonitor.top && position.right<info.rcMonitor.right && position.bottom<info.rcMonitor.bottom,
                "The lower-right overlay must remain inside the primary monitor");
        const size_t bytes=g_ZoomIndicator.SurfaceBytes();require(bytes && bytes<=2*1024*1024,"Indicator must use a small bitmap instead of another full-screen canvas");
        result.maximumSurfaceBytes=(std::max)(result.maximumSurfaceBytes,bytes);
        const SIZE size=g_ZoomIndicator.SurfaceSize();const auto pixels=g_ZoomIndicator.Pixels();
        size_t transparent{},painted{},edges{};
        for(size_t i=0;i<static_cast<size_t>(size.cx)*size.cy;++i) {
            const auto pixel=pixels[i];const unsigned alpha=pixel>>24;
            if(!alpha)++transparent;else {++painted;if(alpha<255)++edges;}
            require(((pixel>>16)&255)<=alpha && ((pixel>>8)&255)<=alpha && (pixel&255)<=alpha,
                    "Layered text must have valid premultiplied alpha, including anti-aliased edges");
        }
        require(transparent && painted && edges,"The surface must contain transparent background and smooth text edges");
    };
    // Preference defaults, malformed values and persistence use the fixture's isolated registry.
    REG_SETTING table[2]{};for(const auto& setting:RegSettings)if(setting.Setting==&g_ShowZoomIndicator)table[0]=setting;
    require(table[0].ValueName && table[0].DefaultSetting==1,"New installations must show the indicator by default");
    const std::wstring key=L"Software\\ZoomItCustom\\Indicator_"+std::to_wstring(GetCurrentProcessId());
    ClassRegistry settings(key.c_str());g_ShowZoomIndicator=FALSE;settings.ReadRegSettings(table);require(g_ShowZoomIndicator,"Missing preference must default to enabled");
    for(bool enabled:{false,true}) {g_ShowZoomIndicator=enabled;settings.WriteRegSettings(table);g_ShowZoomIndicator=!enabled;settings.ReadRegSettings(table);require((g_ShowZoomIndicator!=FALSE)==enabled,"Indicator preference must round-trip independently of animation");++result.preferenceCases;}
    HKEY malformed{};require(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE,&malformed)==ERROR_SUCCESS,"Open the isolated indicator preference");
    RegSetValueExW(malformed,table[0].ValueName,0,REG_SZ,reinterpret_cast<const BYTE*>(L"bad"),4*sizeof(wchar_t));RegCloseKey(malformed);
    g_ShowZoomIndicator=FALSE;settings.ReadRegSettings(table);require(g_ShowZoomIndicator,"Malformed preference must fall back safely to its enabled default");++result.preferenceCases;
    RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
    // Options persistence must not reserve the user's real application shortcuts.
    g_ToggleKey=g_LiveZoomToggleKey=g_DrawToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
    g_ShowZoomIndicator=TRUE;indicatorChoice=false;indicatorSave=false;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,IndicatorOptionsProc);require(g_ShowZoomIndicator,"Cancel must preserve the indicator preference");
    indicatorSave=true;g_TestMode=false;DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,IndicatorOptionsProc);g_TestMode=true;
    require(!g_ShowZoomIndicator,"Options OK must disable the indicator");g_ShowZoomIndicator=TRUE;reg.ReadRegSettings(RegSettings);require(!g_ShowZoomIndicator,"Options OK must persist the preference");
    indicatorChoice=true;g_TestMode=false;DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,IndicatorOptionsProc);g_TestMode=true;
    require(g_ShowZoomIndicator,"Options OK must enable the indicator again");result.preferenceCases+=indicatorGuiCases;
    for(bool animated:{false,true}) {
        open(false,animated);visible(2.0f);++result.zoomCases;
        pump(150);SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);visible(2.25f);
        const auto expires=g_ZoomIndicator.Expires();
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
        require(g_ZoomIndicator.Visible() && g_ZoomIndicator.Expires()==expires,"An ignored mode shortcut must not hide or extend the indicator");
        const HWND overlay=g_ZoomIndicator.Window();
        SendMessage(overlay,WM_TIMER,1,0);require(g_ZoomIndicator.Visible(),"An old queued timer must not hide a newly refreshed indicator");
        waitFor([&]{return !g_ZoomIndicator.Visible();},"Indicator must automatically disappear after its final adjustment");
        require(GetTickCount64()>=expires && !g_ZoomIndicator.SurfaceBytes(),"Expiry must release the bitmap and stop work while hidden");++result.zoomCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_DOWN,0);visible(2.0f);++result.zoomCases;
        for(int step=0;step<4;++step)SendMessage(g_hWndMain,WM_KEYDOWN,VK_DOWN,0);
        visible(1.0f);require(!g_ZoomIndicator.Persistent(),"Static Zoom at 1x must retain the existing brief indicator");
        waitFor([&]{return !g_ZoomIndicator.Visible();},"Static Zoom at 1x must still expire normally");++result.zoomCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);visible(1.25f);
        // Copy and save hide before screen capture; selection callback sees the hidden state.
        g_PolicyModalCheck=[&] {require(!g_ZoomIndicator.Visible() && !g_ZoomIndicator.SurfaceBytes(),"Snip must hide the indicator before selecting or capturing pixels");++result.hiddenCases;};
        cancelSnip=true;SetTimer(nullptr,0,15,SelectPolicyRegion);SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
        g_PolicyModalCheck={};cancelSnip=false;pump(40);require(!g_ZoomIndicator.Visible(),"Snip cancellation must not redisplay the indicator");++result.hiddenCases;clean();
        open(true,animated);visible(2.0f);++result.liveCases;
        const HWND live=g_hWndLiveZoom;
        SendMessage(live,WM_HOTKEY,0,0);visible(2.25f);
        SendMessage(live,WM_USER_SET_ZOOM,EncodeZoomLevel(1.75f),0);visible(1.75f);
        require(liveLevel()==1.75f,"An exact restored zoom must update both the magnifier and its visible label");
        SendMessage(live,WM_USER_SET_ZOOM,EncodeZoomLevel(2.25f),0);visible(2.25f);++result.liveCases;
        HWND exclusions[4]{};const int count=MagGetWindowFilterList(g_hWndLiveZoomMag,MW_FILTERMODE_EXCLUDE,4,exclusions);
        require(count==2 && std::find(exclusions,exclusions+count,g_ZoomIndicator.Window())!=exclusions+count,
                "The actual magnifier must exclude the indicator so it cannot magnify or duplicate it");
        RECT before{},after{};GetWindowRect(g_ZoomIndicator.Window(),&before);SetCursorPos(350,300);pump(80);GetWindowRect(g_ZoomIndicator.Window(),&after);
        require(EqualRect(&before,&after),"Live panning must not move or scale the overlay");++result.liveCases;
        SendMessage(live,WM_HOTKEY,1,0);visible(2.0f);
        for(int i=0;i<4;++i)SendMessage(live,WM_HOTKEY,1,0);visible(1.0f);
        waitFor([&]{return liveLevel()==1.0f;},"Indicator must coexist with active LiveZoom at its 1x minimum");
        const auto floorExpires=g_ZoomIndicator.Expires();SendMessage(live,WM_HOTKEY,1,0);
        require(!floorExpires && g_ZoomIndicator.Expires()==floorExpires && IsWindowVisible(live),"A reduction clamped at 1x must preserve persistent status without exiting LiveZoom");++result.liveCases;
        const HWND floorLabel=g_ZoomIndicator.Window();
        SendMessage(floorLabel,WM_TIMER,1,0);visible(1.0f);
        MSG oldTimer{};while(PeekMessage(&oldTimer,floorLabel,WM_TIMER,WM_TIMER,PM_REMOVE))DispatchMessage(&oldTimer);
        const auto timerNotifications=g_ZoomIndicator.TimerNotifications();const auto floorBytes=g_ZoomIndicator.SurfaceBytes();
        pump(zoomit::ZoomIndicator::Duration+150);visible(1.0f);
        require(g_ZoomIndicator.TimerNotifications()==timerNotifications && g_ZoomIndicator.SurfaceBytes()==floorBytes,
                "Persistent floor status must outlive the normal duration with no native HUD timer messages or extra surfaces");++result.persistentCases;
        HWND floorExclusions[4]{};const int floorCount=MagGetWindowFilterList(g_hWndLiveZoomMag,MW_FILTERMODE_EXCLUDE,4,floorExclusions);
        require(floorCount==2 && std::find(floorExclusions,floorExclusions+floorCount,floorLabel)!=floorExclusions+floorCount,
                "The persistent floor status must remain excluded from the actual magnifier capture");++result.persistentCases;
        g_IndicatorOptionsModalCheck=[&] {
            require(!g_ZoomIndicator.Visible() && !g_ZoomIndicator.SurfaceBytes() && !g_ZoomIndicator.ExpiryTimerActive(),
                    "Options must remove persistent status while the modal panel is open");++result.hiddenCases;
        };
        const UINT_PTR optionsTimer=SetTimer(nullptr,0,15,CloseIndicatorFloorOptions);
        require(optionsTimer!=0,"Schedule the process-owned floor Options closure");
        SendMessage(g_hWndMain,WM_COMMAND,IDC_OPTIONS,0);KillTimer(nullptr,optionsTimer);g_IndicatorOptionsModalCheck={};
        RethrowTestCallbackFailure();visible(1.0f);++result.persistentCases;
        for(bool liveDraw:{false,true}) {
            SendMessage(g_hWndMain,WM_HOTKEY,liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL | (liveDraw ? MOD_SHIFT : 0),'3'));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(150,150));pump(40);
            require(!g_ZoomIndicator.Visible() && !g_ZoomIndicator.SurfaceBytes(),"Draw and LiveDraw must remove indicator pixels before using their canvas");
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
            require(!g_ZoomIndicator.Visible(),"Paused drawing must not show the zoom factor");
            SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(80);
            require(IsWindowVisible(g_hWndLiveZoom),"Returning from drawing must retain the active LiveZoom floor");
            visible(1.0f);++result.hiddenCases;++result.persistentCases;
        }
        g_PolicyModalCheck=[&] {require(!g_ZoomIndicator.Visible() && !g_ZoomIndicator.SurfaceBytes(),"Floor Snip must hide persistent status before selection and capture");++result.hiddenCases;};
        cancelSnip=true;SetTimer(nullptr,0,15,SelectPolicyRegion);SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
        g_PolicyModalCheck={};cancelSnip=false;pump(50);visible(1.0f);++result.persistentCases;
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);visible(1.25f);
        const auto higherExpires=g_ZoomIndicator.Expires();
        waitFor([&]{return !g_ZoomIndicator.Visible();},"Returning above 1x must restore the normal brief duration");
        require(GetTickCount64()>=higherExpires && !g_ZoomIndicator.Persistent() && !g_ZoomIndicator.ExpiryTimerActive(),"A higher zoom factor must release the persistent floor surface when its brief label expires");++result.persistentCases;
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);visible(1.5f);
        g_PolicyModalCheck=[&] {require(!g_ZoomIndicator.Visible(),"LiveZoom Snip must hide the indicator before freezing the screen");++result.hiddenCases;};
        cancelSnip=true;SetTimer(nullptr,0,15,SelectPolicyRegion);SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
        g_PolicyModalCheck={};cancelSnip=false;pump(50);require(!g_ZoomIndicator.Visible() && IsWindowVisible(g_hWndLiveZoom),"LiveZoom must resume after Snip without reintroducing the indicator");++result.hiddenCases;clean();
    }
    g_ShowZoomIndicator=FALSE;open(false,false);require(!g_ZoomIndicator.Visible(),"Disabled preference must suppress static zoom entry");
    SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);require(!g_ZoomIndicator.Visible(),"Disabled preference must suppress static adjustments");
    open(true,false);SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(!g_ZoomIndicator.Visible(),"Disabled preference must suppress live entry and adjustments");
    for(int step=0;step<5;++step)SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
    SendMessage(g_hWndMain,WM_USER_SESSION_TICK,3,0);
    require(liveLevel()==1.0f && !g_ZoomIndicator.Visible() && !g_ZoomIndicator.Persistent(),"Disabled preference must also suppress persistent status at the active 1x floor");clean();result.preferenceCases+=4;
    g_ShowZoomIndicator=TRUE;zoomit::ZoomIndicator::denyPresentation=true;open(true,false);
    require(g_ZoomIndicator.Failed() && IsWindowVisible(g_hWndLiveZoom) && liveLevel()==2,"An indicator allocation failure must leave LiveZoom operational");
    SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(liveLevel()==2.25f && !g_ZoomIndicator.Visible(),"Zoom controls must continue after indicator failure");++result.failureCases;
    zoomit::ZoomIndicator::denyPresentation=false;open(true,false);visible(2.0f);++result.failureCases;clean();
    open(true,false);visible(2.0f);
    wchar_t capture[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"ZOOMIT_TEST_INDICATOR_CAPTURE",capture,_countof(capture))) {
        const SIZE size=g_ZoomIndicator.SurfaceSize();HDC dc=CreateCompatibleDC(nullptr),screen=GetDC(host);HBITMAP bitmap=CreateCompatibleBitmap(screen,size.cx,size.cy);ReleaseDC(host,screen);SelectObject(dc,bitmap);
        RECT canvas{0,0,size.cx,size.cy};HBRUSH background=CreateSolidBrush(RGB(60,95,150));FillRect(dc,&canvas,background);DeleteObject(background);
        BLENDFUNCTION blend{AC_SRC_OVER,0,g_ZoomIndicator.Opacity(),AC_SRC_ALPHA};AlphaBlend(dc,0,0,size.cx,size.cy,g_ZoomIndicator.SurfaceDC(),0,0,size.cx,size.cy,blend);
        require(SavePng(capture,bitmap)==ERROR_SUCCESS,"Save the actual overlay surface for visual verification");DeleteDC(dc);DeleteObject(bitmap);
    }
    // Negative monitor origins and larger DPI values must preserve corner placement.
    for(const RECT monitor : {RECT{-1280,0,0,1024},RECT{0,-1080,1920,0}})for(UINT dpi : {96u,192u}) {
        require(g_ZoomIndicator.Show(3.75f,monitor,dpi)==ERROR_SUCCESS,"Render at a non-primary monitor origin");
        RECT actual{};GetWindowRect(g_ZoomIndicator.Window(),&actual);
        require(actual.left>=monitor.left && actual.top>=monitor.top && actual.right<monitor.right && actual.bottom<monitor.bottom,
                "Indicator geometry must remain in bounds with negative monitor coordinates and different DPI");++result.liveCases;
    }
    clean();result.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);result.userBefore=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    for(int i=0;i<20;++i) {open(i%2,false);if(i%2){for(int step=0;step<4;++step)SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);visible(1.0f);}else SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);clean();++result.cycles;}
    result.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);result.userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    require(result.gdiAfter<=result.gdiBefore && result.userAfter<=result.userBefore,"Repeated indicators must not leak bitmaps, DCs, windows or timers");
    return result;
}
void PrintIndicatorResults(const IndicatorResults& r) {
    std::cout<<"{\"passed\":true,\"zoom_indicator\":true,\"static_cases\":"<<r.zoomCases<<",\"live_cases\":"<<r.liveCases
        <<",\"hidden_during_capture_or_drawing\":"<<r.hiddenCases<<",\"preference_cases\":"<<r.preferenceCases<<",\"failure_cases\":"<<r.failureCases
        <<",\"persistent_floor_cases\":"<<r.persistentCases<<",\"show_hide_cycles\":"<<r.cycles<<",\"maximum_surface_bytes\":"<<r.maximumSurfaceBytes<<",\"gdi_before\":"<<r.gdiBefore<<",\"gdi_after\":"<<r.gdiAfter
        <<",\"user_before\":"<<r.userBefore<<",\"user_after\":"<<r.userAfter<<"}\n";
}
