#pragma once

struct LiveWheelResults {
    size_t nativeCases{},minimumCases{},partialCases{},passthroughCases{},blockedCases{},staleCases{},cycles{},failureCases{};
    DWORD handlesBefore{},handlesAfter{};
};
size_t downstreamWheelEvents{};
// Installed first, so this observer is downstream from the production hook.
// It consumes test gestures that the app passes through, keeping other apps untouched.
LRESULT CALLBACK WheelTestObserver(int code,WPARAM message,LPARAM param) {
    if(code==HC_ACTION && message==WM_MOUSEWHEEL) {++downstreamWheelEvents;return 1;}
    return CallNextHookEx(nullptr,code,message,param);
}
void InjectTestKey(WORD key,bool release) {
    INPUT input{};input.type=INPUT_KEYBOARD;input.ki.wVk=key;input.ki.dwFlags=release ? KEYEVENTF_KEYUP : 0;
    require(SendInput(1,&input,sizeof(input))==1,"Inject a native modifier into the process-owned wheel fixture");
    pump(5);
}
void InjectTestWheel(int delta) {
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_WHEEL;input.mi.mouseData=static_cast<DWORD>(delta);
    require(SendInput(1,&input,sizeof(input))==1,"Inject a real native wheel gesture");
    pump(15);
}
void CtrlTestWheel(int delta,WORD ctrl=VK_LCONTROL) {
    InjectTestKey(ctrl,false);InjectTestWheel(delta);InjectTestKey(ctrl,true);
}

LiveWheelResults RunLiveWheelRegression(HWND host) {
    LiveWheelResults results{};
    const BOOLEAN savedStaticAnimation=g_AnimateZoom,savedLiveAnimation=g_AnimateLiveZoom;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    const DWORD savedIndex=g_SliderZoomLevel,savedPercent=g_InitialZoomPercent,savedLegacy=g_LegacySliderZoomLevel;
    auto restore=zoomit::OnExit([&] {
        InjectTestKey(VK_LCONTROL,true);InjectTestKey(VK_RCONTROL,true);InjectTestKey(VK_MENU,true);InjectTestKey(VK_SHIFT,true);
        zoomit::LiveZoomWheel::denyInstallation.store(false);
        g_AnimateZoom=savedStaticAnimation;g_AnimateLiveZoom=savedLiveAnimation;g_fullScreenWorkaround=savedFullscreen;
        g_SliderZoomLevel=savedIndex;g_InitialZoomPercent=savedPercent;g_LegacySliderZoomLevel=savedLegacy;
        g_PolicyOptionsCheck={};g_PolicyModalCheck={};
    });
    auto waitFor=[&](auto done,const char* message) {
        const auto deadline=GetTickCount64()+2500;
        while(!done() && GetTickCount64()<deadline)pump(5);
        require(done(),message);
    };
    auto level=[] {
        require(IsWindow(g_hWndLiveZoom),"Read wheel factor while the LiveZoom window exists");
        return *reinterpret_cast<const float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
    };
    auto view=[] {return DecodeZoomLevel(static_cast<WPARAM>(SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,0)));};
    auto verifyNative=[&] {
        float actual{};
        if(g_fullScreenWorkaround) {int x{},y{};require(MagGetFullscreenTransform(&actual,&x,&y),"Read the real fullscreen wheel transform");}
        else {MAGTRANSFORM matrix{};require(MagGetWindowTransform(g_hWndLiveZoomMag,&matrix),"Read the real window wheel transform");actual=matrix.v[0][0];}
        require(std::fabs(actual-level())<0.0001f,"Native magnification must match the wheel-selected factor");
    };
    auto clean=[&] {
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(20);
        waitFor([]{return !g_LiveZoomWheel.Installed();},"Leaving LiveZoom must remove the wheel hook");
        require(!IsWindow(g_hWndLiveZoom),"Wheel cleanup must destroy the magnifier");
        ReadOwnedNormalPointer(host,"Ctrl+wheel cleanup");
    };
    clean();
    HHOOK observer=SetWindowsHookExW(WH_MOUSE_LL,WheelTestObserver,GetModuleHandleW(nullptr),0);
    require(observer!=nullptr,"Install downstream native observer before production activation");
    auto removeObserver=zoomit::OnExit([&]{UnhookWindowsHookEx(observer);});
    auto open=[&] {
        ActivateTestHost(host);SetCursorPos(125,125);pump(10);SetInitialZoomIndex(3);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
        waitFor([&]{return level()==2.0f;},"LiveZoom wheel fixture must begin at 2x");
        require(g_LiveZoomWheel.Installed() && g_LiveZoomWheel.Accept(g_hWndMain,g_LiveZoomWheel.Epoch()),
                "Entering pure LiveZoom must activate the production interception");
    };
    for(bool fullscreen:{false,true})for(bool animated:{false,true}) {
        g_fullScreenWorkaround=fullscreen;g_AnimateLiveZoom=animated;g_AnimateZoom=FALSE;open();
        const size_t downstream=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);waitFor([&]{return level()==2.25f;},"Ctrl+wheel up must increase by exactly one quarter step");
        verifyNative();require(downstreamWheelEvents==downstream,"Consumed Ctrl+wheel must not reach underlying applications or downstream hooks");++results.nativeCases;
        CtrlTestWheel(-WHEEL_DELTA,VK_RCONTROL);waitFor([&]{return level()==2.0f;},"Right Ctrl+wheel down must reverse the same step");verifyNative();++results.nativeCases;
        CtrlTestWheel(3*WHEEL_DELTA);waitFor([&]{return level()==2.75f;},"A multi-notch wheel event must use every requested step");++results.nativeCases;
        CtrlTestWheel(-3*WHEEL_DELTA);waitFor([&]{return level()==2.0f;},"Multi-notch reduction must retrace the same levels");++results.nativeCases;
        // Keep Ctrl held across the floor and subsequent native wheel events.
        const HWND floorWindow=g_hWndLiveZoom;
        const ULONG_PTR floorEpoch=g_LiveZoomWheel.Epoch();
        InjectTestKey(VK_LCONTROL,false);InjectTestWheel(-16*WHEEL_DELTA);
        waitFor([&]{return level()==1.0f;},"A multi-notch Ctrl+wheel reduction must remain in LiveZoom at 1x");verifyNative();
        for(int i=0;i<3;++i)InjectTestWheel(-WHEEL_DELTA);
        InjectTestWheel(-4*WHEEL_DELTA);pump(100);
        require(level()==1.0f && g_hWndLiveZoom==floorWindow && IsWindowVisible(floorWindow) &&
                g_LiveZoomWheel.Installed() && g_LiveZoomWheel.Epoch()==floorEpoch && downstreamWheelEvents==downstream,
                "At 1x, further Ctrl+wheel must keep the same live session and never reach the underlying app");++results.minimumCases;
        InjectTestWheel(WHEEL_DELTA);waitFor([&]{return level()==1.25f;},"Wheel up from the active 1x floor must select 1.25x");verifyNative();
        InjectTestWheel(3*WHEEL_DELTA);waitFor([&]{return level()==2.0f;},"Wheel up must resume the normal levels from 1x");++results.minimumCases;
        InjectTestWheel(-16*WHEEL_DELTA);waitFor([&]{return level()==1.0f;},"Return to the active 1x floor before explicit exit");
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
        waitFor([]{return !IsWindow(g_hWndLiveZoom);},"The explicit LiveZoom hotkey must still exit immediately at 1x");
        const size_t afterExplicitExit=downstreamWheelEvents;
        InjectTestWheel(WHEEL_DELTA);InjectTestKey(VK_LCONTROL,true);
        require(downstreamWheelEvents==afterExplicitExit+1,"Ctrl+wheel may resume its underlying action only after explicit LiveZoom exit");
        ReadOwnedNormalPointer(host,"Explicit LiveZoom exit at 1x");++results.minimumCases;open();
        InjectTestKey(VK_LCONTROL,false);
        InjectTestWheel(20);InjectTestWheel(20);require(level()==2.0f,"Partial wheel deltas must not each cause a full zoom step");
        InjectTestWheel(80);waitFor([&]{return level()==2.25f;},"High-resolution partial input must accumulate to one full notch");
        InjectTestWheel(-60);InjectTestWheel(60);require(level()==2.25f,"Opposing partial deltas must cancel without changing magnification");
        InjectTestKey(VK_LCONTROL,true);results.partialCases+=2;
        CtrlTestWheel(-WHEEL_DELTA);waitFor([&]{return level()==2.0f;},"Return to the partial-input baseline");
        const size_t beforePlain=downstreamWheelEvents;
        InjectTestWheel(WHEEL_DELTA);
        require(downstreamWheelEvents==beforePlain+1 && level()==2.0f,"Wheel without Ctrl must pass through and retain normal action");++results.passthroughCases;
        InjectTestKey(VK_LCONTROL,false);InjectTestKey(VK_MENU,false);InjectTestWheel(WHEEL_DELTA);
        InjectTestKey(VK_MENU,true);InjectTestKey(VK_LCONTROL,true);
        require(downstreamWheelEvents==beforePlain+2 && level()==2.0f,"Ctrl+Alt+wheel must retain its other shortcut rather than alter LiveZoom");++results.passthroughCases;
        // A normal wheel ends partial input; keyboard adjustment does as well.
        CtrlTestWheel(60);InjectTestWheel(WHEEL_DELTA);CtrlTestWheel(60);
        require(level()==2.0f,"Partial Ctrl input must not leak through an intervening normal wheel gesture");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);waitFor([&]{return level()==2.25f;},"The original keyboard controls must remain operational");
        CtrlTestWheel(60);require(level()==2.25f,"Changing with the keyboard must discard prior partial wheel input");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);waitFor([&]{return level()==2.0f;},"Keyboard controls must share the wheel target calculation");results.partialCases+=2;
        // Explicitly test a queued request crossing Draw, including paused frozen Draw.
        const ULONG_PTR stale=g_LiveZoomWheel.Epoch();
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,stale,WHEEL_DELTA);
        SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3'));
        SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(150,150));pump(40);
        require(view()==1.0f && !g_LiveZoomWheel.Accept(g_hWndMain,stale),"A queued wheel request must not alter a newly entered frozen drawing");++results.staleCases;
        const size_t beforeDraw=downstreamWheelEvents;CtrlTestWheel(WHEEL_DELTA);
        require(view()==1.0f && downstreamWheelEvents==beforeDraw+1,"Draw must disable the new wheel zoom interception");++results.blockedCases;
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
        require((SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0)&16)!=0,"First right click must pause the frozen LiveZoom drawing");
        SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
        CtrlTestWheel(WHEEL_DELTA);require(view()==1.0f,"Paused frozen drawing must never turn Ctrl+wheel into static zoom");++results.blockedCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(50);
        require(level()==2.0f,"Returning from frozen drawing must preserve the original live factor");
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,stale,WHEEL_DELTA);pump(20);
        require(level()==2.0f,"Obsolete wheel requests must stay ignored after drawing ends");++results.staleCases;
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL|MOD_SHIFT,'3'));
        SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(150,150));pump(35);
        const size_t beforeLiveDraw=downstreamWheelEvents;CtrlTestWheel(WHEEL_DELTA);
        require(level()==2.0f && downstreamWheelEvents==beforeLiveDraw+1,"LiveDraw over LiveZoom must preserve the live factor and pass through the wheel");++results.blockedCases;
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
        CtrlTestWheel(-WHEEL_DELTA);require(level()==2.0f,"Paused LiveDraw must keep wheel zoom disabled");++results.blockedCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(40);
        if(animated) {
            InjectTestKey(VK_LCONTROL,false);
            // Hold the timer frame while injecting native input so workstation scheduling
            // cannot finish the short exit before the test has delivered its wheel gesture.
            KillTimer(g_hWndLiveZoom,0);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
            if(!IsWindow(g_hWndLiveZoom) || level()>=2.0f)
                std::cerr<<"Wheel exit setup: fullscreen="<<fullscreen<<" animate="<<static_cast<int>(g_AnimateLiveZoom)
                    <<" exists="<<IsWindow(g_hWndLiveZoom)<<" main_mode="<<SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0)
                    <<" main_visible="<<IsWindowVisible(g_hWndMain)<<" live_visible="<<IsWindowVisible(g_hWndLiveZoom)
                    <<" level="<<(IsWindow(g_hWndLiveZoom)?level():0)<<"\n";
            require(IsWindow(g_hWndLiveZoom) && level()<2.0f,"Wheel reversal must begin during a real exit animation");
            InjectTestWheel(WHEEL_DELTA);InjectTestKey(VK_LCONTROL,true);
            SetTimer(g_hWndLiveZoom,0,ZOOM_LEVEL_STEP_TIME,nullptr);
            waitFor([&]{return level()==2.25f;},"Wheel during animated exit must use the prior logical target and reverse cleanly");++results.nativeCases;
        }
        clean();
    }
    g_fullScreenWorkaround=FALSE;g_AnimateLiveZoom=FALSE;open();
    const ULONG_PTR beforeOptions=g_LiveZoomWheel.Epoch();
    g_PolicyOptionsCheck=[&] {
        const float before=level();const size_t observed=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,beforeOptions,WHEEL_DELTA);pump(20);
        require(level()==before && downstreamWheelEvents==observed+1,"Options must block live wheel input and queued requests without changing its controls");
        ++results.blockedCases;++results.staleCases;
    };
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,PolicyOptionsProc);g_PolicyOptionsCheck={};
    SendMessage(g_hWndMain,WM_TIMER,3,0);require(g_LiveZoomWheel.Installed(),"Ending options must allow wheel zoom again");
    g_PolicyModalCheck=[&] {
        const size_t observed=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);require(downstreamWheelEvents==observed+1,"Snip selection must not intercept Ctrl+wheel");++results.blockedCases;
    };
    cancelSnip=true;SetTimer(nullptr,0,15,SelectPolicyRegion);
    SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
    g_PolicyModalCheck={};cancelSnip=false;pump(30);
    require(level()==2.0f && g_LiveZoomWheel.Installed(),"Cancelling Snip must preserve factor and reactivate wheel control");
    // End/re-enter between two half notches: no remainder from the previous session.
    CtrlTestWheel(60);const ULONG_PTR previousSession=g_LiveZoomWheel.Epoch();clean();open();
    CtrlTestWheel(60);require(level()==2.0f,"A new LiveZoom session must discard partial input from the previous session");
    PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,previousSession,WHEEL_DELTA);pump(15);
    require(level()==2.0f,"A wheel request must not survive complete exit and re-entry");++results.staleCases;
    CtrlTestWheel(60);require(level()==2.25f,"Only current-session partial input may form a notch");++results.partialCases;
    clean();
    const HANDLE worker=g_LiveZoomWheel.Thread();require(worker!=nullptr,"A sleeping worker must be reused instead of recreated each time");
    GetProcessHandleCount(GetCurrentProcess(),&results.handlesBefore);
    for(int i=0;i<25;++i) {
        open();CtrlTestWheel(WHEEL_DELTA);CtrlTestWheel(-WHEEL_DELTA);
        require(level()==2.0f && g_LiveZoomWheel.Thread()==worker,"Repeated entry/exit must retain exact targets and reuse one worker");
        clean();++results.cycles;
    }
    GetProcessHandleCount(GetCurrentProcess(),&results.handlesAfter);
    require(results.handlesAfter<=results.handlesBefore+1,"Repeated interception must not leak thread, event or hook handles");
    zoomit::LiveZoomWheel::denyInstallation.store(true);
    ActivateTestHost(host);SetCursorPos(125,125);SetInitialZoomIndex(3);
    SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));pump(40);
    require(IsWindowVisible(g_hWndLiveZoom) && !g_LiveZoomWheel.Installed(),"A denied optional hook must leave LiveZoom operational");
    SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(level()==2.25f,"Keyboard zoom must remain available after hook installation failure");++results.failureCases;
    clean();zoomit::LiveZoomWheel::denyInstallation.store(false);open();
    CtrlTestWheel(WHEEL_DELTA);require(level()==2.25f,"The next explicit session must retry a previously denied hook");++results.failureCases;clean();
    return results;
}
void PrintLiveWheelResults(const LiveWheelResults& result) {
    std::cout<<"{\"passed\":true,\"live_wheel\":true,\"native_cases\":"<<result.nativeCases
        <<",\"minimum_zoom_cases\":"<<result.minimumCases<<",\"partial_input_cases\":"<<result.partialCases<<",\"passthrough_cases\":"<<result.passthroughCases
        <<",\"blocked_cases\":"<<result.blockedCases<<",\"stale_request_cases\":"<<result.staleCases
        <<",\"entry_exit_cycles\":"<<result.cycles<<",\"installation_failure_cases\":"<<result.failureCases
        <<",\"handles_before\":"<<result.handlesBefore<<",\"handles_after\":"<<result.handlesAfter<<"}\n";
}
