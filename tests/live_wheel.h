#pragma once
#include <tlhelp32.h>

struct LiveWheelResults {
    size_t nativeCases{},minimumCases{},partialCases{},passthroughCases{},blockedCases{},staleCases{},cycles{},failureCases{};
    size_t workerRecoveryCases{},queueFailureCases{},timeoutCases{};
    size_t escapeCases{},escapeBlockedCases{},escapeModifierCases{},escapeFailureCases{};
    DWORD handlesBefore{},handlesAfter{};
};
struct WheelResources {
    const char* context{};DWORD handles{},threads{};unsigned states{},workers{};
};
WheelResources ReadWheelResources(const char* context) {
    WheelResources value{};value.context=context;
    {
        native::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));
        require(static_cast<bool>(snapshot),"Read native thread count for wheel ownership evidence");
        THREADENTRY32 entry{sizeof(entry)};
        require(Thread32First(snapshot.get(),&entry)!=FALSE,"Enumerate threads for wheel ownership evidence");
        do {if(entry.th32OwnerProcessID==GetCurrentProcessId())++value.threads;}while(Thread32Next(snapshot.get(),&entry));
    }
    require(GetProcessHandleCount(GetCurrentProcess(),&value.handles)!=FALSE,"Read native handle count for wheel ownership evidence");
    value.states=zoomit::LiveZoomWheel::liveStates.load();value.workers=zoomit::LiveZoomWheel::liveWorkers.load();
    return value;
}
void PrintWheelResources(const WheelResources& value) {
    std::cerr<<"Wheel resources: phase="<<value.context<<" handles="<<value.handles<<" threads="<<value.threads
             <<" states="<<value.states<<" workers="<<value.workers<<"\n";
}
size_t downstreamWheelEvents{},downstreamEscapeDown{},downstreamEscapeUp{};
bool deliverEscapeToOwnedWindows{},escapeOptionsInjected{},escapeOptionsWaitingReported{};
const char* liveWheelPhaseContext="legacy native wheel";
void TraceLiveWheelPhase(const char* step) {
    static const bool enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"ZOOMIT_TEST_TRACE_ESCAPE",value,2)>0 && value[0]==L'1';}();
    if(!enabled)return;
    std::cerr<<"Wheel phase: time="<<GetTickCount64()<<" context="<<liveWheelPhaseContext<<" step="<<step
        <<" epoch="<<g_LiveZoomWheel.Epoch()<<" mouse="<<g_LiveZoomWheel.Installed()
        <<" keyboard="<<g_LiveZoomWheel.KeyboardInstalled()<<" escape_posts="<<zoomit::LiveZoomWheel::escapePosts.load()
        <<" escape_down="<<downstreamEscapeDown<<" escape_up="<<downstreamEscapeUp
        <<" live="<<g_hWndLiveZoom<<" main="<<g_hWndMain<<" options="<<hWndOptions
        <<" foreground="<<GetForegroundWindow()<<"\n";
}
// A failed modal scenario must return to the fixture and report failure, rather
// than leave a test-owned dialog/selection blocking until the CTest process timeout.
class WheelModalWatchdog {
    UINT_PTR timer_{};
    inline static WheelModalWatchdog* active_{};
    static void CALLBACK Expire(HWND,UINT,UINT_PTR timer,DWORD) noexcept {
        auto active=active_;if(!active || active->timer_!=timer)return;
        KillTimer(nullptr,timer);active->timer_=0;
        TraceLiveWheelPhase("modal watchdog expired");
        try {throw std::runtime_error("Owned wheel modal scenario exceeded its 2500ms deadline");}
        catch (...) {RecordTestCallbackFailure();}
        struct Targets {HWND dialog{},selection{};} targets;
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM param)->BOOL {
            auto& found=*reinterpret_cast<Targets*>(param);
            wchar_t type[64]{};GetClassNameW(window,type,_countof(type));
            if(wcscmp(type,L"ZoomitSelectRectangle")==0)found.selection=window;
            if(wcscmp(type,L"#32770")==0 && IsWindowVisible(window))found.dialog=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(&targets));
        if(targets.selection)PostMessageW(targets.selection,WM_KEYDOWN,VK_ESCAPE,0);
        if(targets.dialog)PostMessageW(targets.dialog,WM_COMMAND,IDCANCEL,0);
    }
public:
    WheelModalWatchdog() {
        require(!active_,"Only one owned wheel modal watchdog may be active");
        timer_=SetTimer(nullptr,0,2500,Expire);
        require(timer_!=0,"Arm a bounded owned wheel modal watchdog");active_=this;
    }
    ~WheelModalWatchdog(){if(timer_)KillTimer(nullptr,timer_);if(active_==this)active_=nullptr;}
    WheelModalWatchdog(const WheelModalWatchdog&)=delete;
};
// Installed first, so this observer is downstream from the production hook.
// It consumes test gestures that the app passes through, keeping other apps untouched.
LRESULT CALLBACK WheelTestObserver(int code,WPARAM message,LPARAM param) {
    if(code==HC_ACTION && message==WM_MOUSEWHEEL) {++downstreamWheelEvents;return 1;}
    return CallNextHookEx(nullptr,code,message,param);
}
LRESULT CALLBACK EscapeTestObserver(int code,WPARAM message,LPARAM param) {
    if(code==HC_ACTION && reinterpret_cast<const KBDLLHOOKSTRUCT*>(param)->vkCode==VK_ESCAPE) {
        if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN)++downstreamEscapeDown;
        if(message==WM_KEYUP || message==WM_SYSKEYUP)++downstreamEscapeUp;
        TraceLiveWheelPhase(message==WM_KEYUP || message==WM_SYSKEYUP ? "downstream Escape UP" : "downstream Escape DOWN");
        const HWND foreground=GetForegroundWindow();DWORD process{};
        if(deliverEscapeToOwnedWindows && foreground && GetWindowThreadProcessId(foreground,&process) && process==GetCurrentProcessId()) {
            wchar_t type[64]{};GetClassNameW(foreground,type,_countof(type));
            if(foreground==g_hWndMain || foreground==hWndOptions || wcscmp(type,L"ZoomitSelectRectangle")==0)
                return CallNextHookEx(nullptr,code,message,param);
        }
        return 1; // Passed-through fixture Escape must never reach another application.
    }
    return CallNextHookEx(nullptr,code,message,param);
}
INT_PTR CALLBACK EscapeOptionsProbe(HWND dialog,UINT message,WPARAM word,LPARAM param) noexcept {
    return TestDialogBoundary(dialog,[&]() -> INT_PTR {
        if(message==WM_TIMER && word==76) {
            if(g_LiveZoomWheel.KeyboardInstalled()) {
                if(!escapeOptionsWaitingReported){escapeOptionsWaitingReported=true;TraceLiveWheelPhase("options timer waiting for keyboard retirement");}
                return TRUE;
            }
            TraceLiveWheelPhase("options timer injecting native Escape");
            KillTimer(dialog,76);INPUT keys[2]{};
            for(auto& key:keys){key.type=INPUT_KEYBOARD;key.ki.wVk=VK_ESCAPE;}
            keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
            require(SendInput(2,keys,sizeof(INPUT))==2,"Deliver native Escape to the owned modal options dialog");
            escapeOptionsInjected=true;TraceLiveWheelPhase("options native Escape queued");return TRUE;
        }
        if(message==WM_TIMER && word==77) {
            KillTimer(dialog,77);TraceLiveWheelPhase("options native Escape deadline expired");
            require(false,"Native Escape must cancel options through the Windows dialog loop");return TRUE;
        }
        const auto result=OptionsProc(dialog,message,word,param);
        if(message==WM_INITDIALOG) {
            escapeOptionsWaitingReported=false;TraceLiveWheelPhase("options initialized and timers arming");
            require(SetTimer(dialog,76,25,nullptr)!=0 && SetTimer(dialog,77,2000,nullptr)!=0,"Arm the owned options Escape fixture");
        }
        if(message==WM_DESTROY)TraceLiveWheelPhase("options destroyed");
        return result;
    });
}
void InjectTestKey(WORD key,bool release) {
    INPUT input{};input.type=INPUT_KEYBOARD;input.ki.wVk=key;input.ki.dwFlags=release ? KEYEVENTF_KEYUP : 0;
    if(key==VK_ESCAPE)TraceLiveWheelPhase(release ? "native Escape UP begin" : "native Escape DOWN begin");
    require(SendInput(1,&input,sizeof(input))==1,"Inject a native modifier into the process-owned wheel fixture");
    pump(5);
    if(key==VK_ESCAPE)TraceLiveWheelPhase(release ? "native Escape UP done" : "native Escape DOWN done");
}
void InjectTestWheel(int delta) {
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_WHEEL;input.mi.mouseData=static_cast<DWORD>(delta);
    require(SendInput(1,&input,sizeof(input))==1,"Inject a real native wheel gesture");
    pump(15);
}
void CtrlTestWheel(int delta,WORD ctrl=VK_LCONTROL) {
    InjectTestKey(ctrl,false);InjectTestWheel(delta);InjectTestKey(ctrl,true);
}

LRESULT CALLBACK WheelOwnedHostNoMenu(HWND window,UINT message,WPARAM word,LPARAM param,UINT_PTR,DWORD_PTR) {
    if(message==WM_SYSCOMMAND && (word&0xfff0)==SC_KEYMENU)return 0;
    return DefSubclassProc(window,message,word,param);
}
LiveWheelResults RunLiveWheelRegression(HWND host) {
    LiveWheelResults results{};
    const BOOLEAN savedStaticAnimation=g_AnimateZoom,savedLiveAnimation=g_AnimateLiveZoom;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    const DWORD savedIndex=g_SliderZoomLevel,savedPercent=g_InitialZoomPercent,savedLegacy=g_LegacySliderZoomLevel;
    auto restore=zoomit::OnExit([&] {
        deliverEscapeToOwnedWindows=false;InjectTestKey(VK_ESCAPE,true);
        InjectTestKey(VK_LCONTROL,true);InjectTestKey(VK_RCONTROL,true);InjectTestKey(VK_MENU,true);InjectTestKey(VK_SHIFT,true);
        zoomit::LiveZoomWheel::denyInstallation.store(false);
        zoomit::LiveZoomWheel::denyKeyboardInstallation.store(false);
        zoomit::LiveZoomWheel::denyInputPost.store(false);
        zoomit::LiveZoomWheel::startupDelayMilliseconds.store(0);
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
        TraceLiveWheelPhase("cleanup begin");
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(20);
        waitFor([]{return !g_LiveZoomWheel.Installed() && !g_LiveZoomWheel.KeyboardInstalled();},"Leaving LiveZoom must remove both input hooks");
        require(!IsWindow(g_hWndLiveZoom),"Wheel cleanup must destroy the magnifier");
        ResetThenReadOwnedNormalPointer(host,"Ctrl+wheel cleanup");TraceLiveWheelPhase("cleanup done");
    };
    clean();
    HHOOK observer=SetWindowsHookExW(WH_MOUSE_LL,WheelTestObserver,GetModuleHandleW(nullptr),0);
    require(observer!=nullptr,"Install downstream native observer before production activation");
    auto removeObserver=zoomit::OnExit([&]{UnhookWindowsHookEx(observer);});
    HHOOK keyboardObserver=SetWindowsHookExW(WH_KEYBOARD_LL,EscapeTestObserver,GetModuleHandleW(nullptr),0);
    require(keyboardObserver!=nullptr,"Install an owned downstream Escape observer before production activation");
    auto removeKeyboardObserver=zoomit::OnExit([&]{UnhookWindowsHookEx(keyboardObserver);});
    auto open=[&] {
        TraceLiveWheelPhase("LiveZoom activation begin");
        ActivateTestHost(host);SetCursorPos(125,125);pump(10);SetInitialZoomIndex(3);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
        waitFor([&]{return level()==2.0f;},"LiveZoom wheel fixture must begin at 2x");
        require(g_LiveZoomWheel.Installed() && g_LiveZoomWheel.KeyboardInstalled() && g_LiveZoomWheel.Accept(g_hWndMain,g_LiveZoomWheel.Epoch()),
                "Entering pure LiveZoom must activate the production interception");
        TraceLiveWheelPhase("LiveZoom activation done");
    };
    for(bool fullscreen:{false,true})for(bool animated:{false,true}) {
        g_fullScreenWorkaround=fullscreen;g_AnimateLiveZoom=animated;g_AnimateZoom=FALSE;open();
        const size_t downstream=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);waitFor([&]{return level()==2.25f;},"Ctrl+wheel up must increase by exactly one quarter step");
        verifyNative();require(downstreamWheelEvents==downstream,"Consumed Ctrl+wheel must not reach underlying applications or downstream hooks");++results.nativeCases;
        if(!fullscreen && g_ShowZoomIndicator)require(g_ZoomIndicator.Visible() && g_ZoomIndicator.Factor()==2.25f,
                "A real native Ctrl+wheel gesture must update the visible zoom indicator");
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
        ResetThenReadOwnedNormalPointer(host,"Explicit LiveZoom exit at 1x");++results.minimumCases;open();
        InjectTestKey(VK_LCONTROL,false);
        InjectTestWheel(20);InjectTestWheel(20);require(level()==2.0f,"Partial wheel deltas must not each cause a full zoom step");
        InjectTestWheel(80);waitFor([&]{return level()==2.25f;},"High-resolution partial input must accumulate to one full notch");
        InjectTestWheel(-60);InjectTestWheel(60);require(level()==2.25f,"Opposing partial deltas must cancel without changing magnification");
        InjectTestKey(VK_LCONTROL,true);results.partialCases+=2;
        CtrlTestWheel(-WHEEL_DELTA);waitFor([&]{return level()==2.0f;},"Return to the partial-input baseline");
        const size_t beforePlain=downstreamWheelEvents;
        InjectTestWheel(WHEEL_DELTA);
        waitFor([&]{return downstreamWheelEvents>=beforePlain+1;},"A normal wheel must arrive at the downstream native observer");
        if(downstreamWheelEvents!=beforePlain+1 || level()!=2.0f)
            std::cerr<<"Plain wheel: fullscreen="<<fullscreen<<" animated="<<animated<<" before="<<beforePlain
                <<" after="<<downstreamWheelEvents<<" factor="<<level()<<" ctrl="<<(GetAsyncKeyState(VK_CONTROL)&0x8000)<<"\n";
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
            const UINT_PTR heldTimer=static_cast<UINT_PTR>(SendMessage(g_hWndLiveZoom,WM_TEST_QUERY_TIMERS,0,0));
            require(heldTimer!=0,"LiveZoom animation must have a current timer identity");
            KillTimer(g_hWndLiveZoom,heldTimer);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
            if(!IsWindow(g_hWndLiveZoom) || level()>=2.0f)
                std::cerr<<"Wheel exit setup: fullscreen="<<fullscreen<<" animate="<<static_cast<int>(g_AnimateLiveZoom)
                    <<" exists="<<IsWindow(g_hWndLiveZoom)<<" main_mode="<<SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0)
                    <<" main_visible="<<IsWindowVisible(g_hWndMain)<<" live_visible="<<IsWindowVisible(g_hWndLiveZoom)
                    <<" level="<<(IsWindow(g_hWndLiveZoom)?level():0)<<"\n";
            require(IsWindow(g_hWndLiveZoom) && level()<2.0f,"Wheel reversal must begin during a real exit animation");
            InjectTestWheel(WHEEL_DELTA);InjectTestKey(VK_LCONTROL,true);
            require(SetTimer(g_hWndLiveZoom,heldTimer,ZOOM_LEVEL_STEP_TIME,nullptr)!=0,
                    "Resume the same controlled live animation timer");
            waitFor([&]{return level()==2.25f;},"Wheel during animated exit must use the prior logical target and reverse cleanly");++results.nativeCases;
        }
        clean();
    }
    // Establish the actual underlying-app focus condition after each live creation;
    // legacy fullscreen creation can briefly leave its overlay as the foreground window.
    const auto openEscape=[&](const char* context) {
        liveWheelPhaseContext=context;TraceLiveWheelPhase("Escape fixture begin");open();
        const bool pureLive=!IsWindowVisible(g_hWndMain);
        const ULONGLONG deadline=GetTickCount64()+2500;
        unsigned attempts{};BOOL requested{};
        if(pureLive)do {
            ActivateTestHost(host);requested=SetForegroundWindow(host);++attempts;
            pump(5); // Wait for outstanding owned activation/menu messages to settle.
        } while(GetForegroundWindow()!=host && GetTickCount64()<deadline);
        if(!pureLive || GetForegroundWindow()!=host) {
            const HWND foreground=GetForegroundWindow();DWORD process{};
            const DWORD thread=foreground ? GetWindowThreadProcessId(foreground,&process) : 0;
            wchar_t type[96]{};if(foreground)GetClassNameW(foreground,type,_countof(type));
            GUITHREADINFO gui{sizeof(gui)};const BOOL read=thread ? GetGUIThreadInfo(thread,&gui) : FALSE;
            std::cerr<<"Escape focus fixture: phase="<<context<<" fullscreen="<<g_fullScreenWorkaround
                <<" animated="<<static_cast<unsigned>(g_AnimateLiveZoom)<<" pure_live="<<pureLive
                <<" attempts="<<attempts<<" requested="<<requested<<" foreground="<<foreground
                <<" foreground_pid="<<process<<" foreground_thread="<<thread<<" own_pid="<<GetCurrentProcessId()
                <<" host="<<host<<" host_enabled="<<IsWindowEnabled(host)<<" live="<<g_hWndLiveZoom
                <<" main="<<g_hWndMain<<" gui_read="<<read<<" gui_flags="<<gui.flags
                <<" active="<<gui.hwndActive<<" focus="<<gui.hwndFocus<<" menu="<<gui.hwndMenuOwner
                <<" capture="<<gui.hwndCapture<<"\n";
            std::wcerr<<L"Escape foreground class: "<<type<<L"\n";
        }
        require(pureLive,"Native Escape fixture must start in pure LiveZoom without a drawing canvas");
        require(GetForegroundWindow()==host,"Native Escape fixture must explicitly focus its owned underlying application");
        TraceLiveWheelPhase("Escape fixture done");
    };
    // Real Escape reaches the production keyboard hook without the live overlay owning focus.
    for(bool fullscreen:{false,true})for(bool animated:{false,true}) {
        g_fullScreenWorkaround=fullscreen;g_AnimateLiveZoom=animated;openEscape("backend exit");
        require(GetForegroundWindow()==host,"Live Escape must work while its owned underlying application remains focused");
        const auto posts=zoomit::LiveZoomWheel::escapePosts.load();const size_t observed=downstreamEscapeDown;
        const HWND exiting=g_hWndLiveZoom;
        UINT_PTR heldTimer{};
        if(animated) {
            heldTimer=static_cast<UINT_PTR>(SendMessage(exiting,WM_TEST_QUERY_TIMERS,0,0));
            require(heldTimer && KillTimer(exiting,heldTimer),"Hold native animation frames while checking repeated Escape");
        }
        InjectTestKey(VK_ESCAPE,false);
        require(zoomit::LiveZoomWheel::escapePosts.load()==posts+1 && downstreamEscapeDown==observed,
                "The first native Escape must post one exit and never reach the underlying application");
        if(animated) {
            require(IsWindowVisible(exiting) && level()<2.0f,"Escape must begin a genuine animated live exit");
            const float leaving=level();
            for(int repeated=0;repeated<4;++repeated)InjectTestKey(VK_ESCAPE,false);
            require(zoomit::LiveZoomWheel::escapePosts.load()==posts+1 && level()==leaving && downstreamEscapeDown==observed,
                    "Held Escape repeats must not reverse or duplicate an animated exit");
        }
        InjectTestKey(VK_ESCAPE,true);
        if(animated)require(SetTimer(exiting,heldTimer,ZOOM_LEVEL_STEP_TIME,nullptr)!=0,"Resume the same owned exit animation timer");
        waitFor([]{return !IsWindow(g_hWndLiveZoom);},"Native Escape must complete LiveZoom exit without its overlay taking focus");
        ResetThenReadOwnedNormalPointer(host,"Native Escape completion");++results.escapeCases;
        openEscape("backend 1x exit");CtrlTestWheel(-16*WHEEL_DELTA);waitFor([&]{return level()==1.0f;},"Prepare active LiveZoom at 1x before native Escape");
        InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
        waitFor([]{return !IsWindow(g_hWndLiveZoom);},"Escape must explicitly exit LiveZoom even at its active 1x minimum");
        ++results.escapeCases;clean();
    }
    g_fullScreenWorkaround=FALSE;g_AnimateLiveZoom=FALSE;openEscape("held first session");
    const ULONG_PTR heldEpoch=g_LiveZoomWheel.Epoch();
    const size_t heldDownstreamDown=downstreamEscapeDown,heldDownstreamUp=downstreamEscapeUp;
    InjectTestKey(VK_ESCAPE,false);waitFor([]{return !IsWindow(g_hWndLiveZoom);},"Begin held Escape across a completed session");
    waitFor([]{return !g_LiveZoomWheel.Installed() && g_LiveZoomWheel.KeyboardInstalled();},
            "A consumed Escape must retain only its keyboard hook until the real release");
    openEscape("held replacement session");const HWND reopened=g_hWndLiveZoom;const auto heldPosts=zoomit::LiveZoomWheel::escapePosts.load();
    for(int repeated=0;repeated<3;++repeated)InjectTestKey(VK_ESCAPE,false);
    PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_ESCAPE,heldEpoch,0);pump(10);
    const bool heldSession=IsWindow(reopened) && g_hWndLiveZoom==reopened && level()==2.0f &&
        zoomit::LiveZoomWheel::escapePosts.load()==heldPosts && downstreamEscapeDown==heldDownstreamDown;
    if(!heldSession)std::cerr<<"Held Escape lifecycle: old_epoch="<<heldEpoch<<" new_epoch="<<g_LiveZoomWheel.Epoch()
        <<" old_window="<<reopened<<" current_window="<<g_hWndLiveZoom<<" exists="<<IsWindow(reopened)
        <<" factor="<<(IsWindow(g_hWndLiveZoom) ? level() : 0)<<" posts_before="<<heldPosts
        <<" posts_after="<<zoomit::LiveZoomWheel::escapePosts.load()<<" keyboard="<<g_LiveZoomWheel.KeyboardInstalled()
        <<" async_escape="<<(GetAsyncKeyState(VK_ESCAPE)&0x8000)<<" downstream_before="<<heldDownstreamDown
        <<" downstream_after="<<downstreamEscapeDown<<"\n";
    require(heldSession,"An initially held Escape and stale exit epoch must not close a newly activated session");
    InjectTestKey(VK_ESCAPE,true);
    require(downstreamEscapeUp==heldDownstreamUp,"The owned held Escape release must not leak to the underlying app");
    InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !IsWindow(g_hWndLiveZoom);},"A fresh Escape after release must exit the replacement session");++results.escapeCases;clean();
    // Release while completely idle, then verify a later session accepts a new press.
    // Sampling asynchronous key state cannot substitute for observing this owned UP.
    openEscape("idle release first session");const auto idlePosts=zoomit::LiveZoomWheel::escapePosts.load();
    const size_t idleDownstreamDown=downstreamEscapeDown,idleDownstreamUp=downstreamEscapeUp;
    InjectTestKey(VK_ESCAPE,false);waitFor([]{return !IsWindow(g_hWndLiveZoom);},"Exit before releasing the owned Escape during idle");
    waitFor([]{return !g_LiveZoomWheel.Installed() && g_LiveZoomWheel.KeyboardInstalled();},
            "Only the keyboard hook may remain while idle Escape is owned");
    for(int repeated=0;repeated<3;++repeated)InjectTestKey(VK_ESCAPE,false);
    require(zoomit::LiveZoomWheel::escapePosts.load()==idlePosts+1 && downstreamEscapeDown==idleDownstreamDown,
            "Owned Escape repeats during idle must not reach the underlying app or post another exit");
    InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !g_LiveZoomWheel.KeyboardInstalled();},"An owned Escape release during idle must immediately retire the keyboard hook");
    require(downstreamEscapeUp==idleDownstreamUp,"An idle owned Escape release must not leak to the underlying app");
    InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    require(downstreamEscapeDown==idleDownstreamDown+1 && downstreamEscapeUp==idleDownstreamUp+1,
            "A separate unowned Escape while idle must pass through normally");
    openEscape("fresh session after idle release");InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !IsWindow(g_hWndLiveZoom);},"A fresh Escape after its idle release must exit the next session");
    require(zoomit::LiveZoomWheel::escapePosts.load()==idlePosts+2,"A fresh gesture after idle release must produce exactly one new exit");
    ++results.escapeCases;clean();openEscape("modified Escape");
    {
    // Esc is observed downstream, but Alt release must not open a host menu
    // and enter a native modal loop unrelated to the shortcut being checked.
    constexpr UINT_PTR noMenuId=0x120e;
    require(SetWindowSubclass(host,WheelOwnedHostNoMenu,noMenuId,0)!=FALSE,"Keep modifier input inside the owned background fixture");
    const auto restoreHost=zoomit::OnExit([&]{RemoveWindowSubclass(host,WheelOwnedHostNoMenu,noMenuId);});
    for(WORD modifier:{WORD{VK_LCONTROL},WORD{VK_SHIFT},WORD{VK_MENU}}) {
        const auto posts=zoomit::LiveZoomWheel::escapePosts.load();const size_t observed=downstreamEscapeDown;
        InjectTestKey(modifier,false);InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);InjectTestKey(modifier,true);
        require(IsWindowVisible(g_hWndLiveZoom) && level()==2.0f && zoomit::LiveZoomWheel::escapePosts.load()==posts && downstreamEscapeDown==observed+1,
                "Modified Escape shortcuts must retain their ordinary action rather than exiting LiveZoom");++results.escapeModifierCases;
    }
    }
    clean();
    for(WPARAM drawing:{WPARAM{DRAW_HOTKEY},WPARAM{LIVE_DRAW_HOTKEY}})for(bool paused:{false,true}) {
        openEscape(drawing==DRAW_HOTKEY ? (paused ? "paused Draw" : "Draw") : (paused ? "paused LiveDraw" : "LiveDraw"));const ULONG_PTR obsolete=g_LiveZoomWheel.Epoch();
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_ESCAPE,obsolete,0);
        SendMessage(g_hWndMain,WM_HOTKEY,drawing,MAKELPARAM(MOD_CONTROL|(drawing==LIVE_DRAW_HOTKEY ? MOD_SHIFT : 0),'3'));
        SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(150,150));pump(20);
        if(paused)SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);
        waitFor([]{return !g_LiveZoomWheel.KeyboardInstalled();},"Draw and paused Draw must disable the live keyboard interception");
        const auto posts=zoomit::LiveZoomWheel::escapePosts.load();const size_t observed=downstreamEscapeDown;
        deliverEscapeToOwnedWindows=true;InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);deliverEscapeToOwnedWindows=false;
        waitFor([]{return !IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom);},"Native Escape must end only drawing and return to LiveZoom");
        require(level()==2.0f && zoomit::LiveZoomWheel::escapePosts.load()==posts && downstreamEscapeDown==observed+1,
                "Drawing Escape must retain its existing path while a queued old live exit stays obsolete");++results.escapeBlockedCases;clean();
    }
    openEscape("modal options");deliverEscapeToOwnedWindows=true;escapeOptionsInjected=false;
    const auto optionsPosts=zoomit::LiveZoomWheel::escapePosts.load();
    {
        WheelModalWatchdog watchdog;TraceLiveWheelPhase("native options dialog begin");
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,EscapeOptionsProbe);
        TraceLiveWheelPhase("native options dialog returned");
    }
    deliverEscapeToOwnedWindows=false;RethrowTestCallbackFailure();
    require(escapeOptionsInjected && !hWndOptions && IsWindowVisible(g_hWndLiveZoom) && level()==2.0f &&
            zoomit::LiveZoomWheel::escapePosts.load()==optionsPosts,"Native Escape must cancel modal options without exiting LiveZoom");
    ++results.escapeBlockedCases;clean();
    openEscape("modal Snip");const auto selectionPosts=zoomit::LiveZoomWheel::escapePosts.load();
    g_PolicyModalCheck=[&] {
        TraceLiveWheelPhase("native Snip selection callback begin");
        require(!g_LiveZoomWheel.KeyboardInstalled(),"Snip must disable live Escape interception before selection");
        deliverEscapeToOwnedWindows=true;InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);deliverEscapeToOwnedWindows=false;
        ++results.escapeBlockedCases;TraceLiveWheelPhase("native Snip selection callback done");
    };
    cancelSnip=true;
    {
        const UINT_PTR selectionTimer=SetTimer(nullptr,0,15,SelectPolicyRegion);
        require(selectionTimer!=0,"Arm the native Snip test selection callback");
        const auto stopSelectionTimer=zoomit::OnExit([&]{KillTimer(nullptr,selectionTimer);});
        WheelModalWatchdog watchdog;TraceLiveWheelPhase("native Snip selection begin");
        SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
        TraceLiveWheelPhase("native Snip selection returned");
    }
    g_PolicyModalCheck={};cancelSnip=false;RethrowTestCallbackFailure();pump(20);
    require(IsWindowVisible(g_hWndLiveZoom) && level()==2.0f && zoomit::LiveZoomWheel::escapePosts.load()==selectionPosts,
            "Native Escape during Snip must cancel selection and preserve the preceding live viewport");clean();
    liveWheelPhaseContext="denied keyboard hook";TraceLiveWheelPhase("fault injection begin");
    zoomit::LiveZoomWheel::denyKeyboardInstallation.store(true);
    ActivateTestHost(host);SetCursorPos(125,125);SetInitialZoomIndex(3);
    SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));pump(40);
    require(IsWindowVisible(g_hWndLiveZoom) && !g_LiveZoomWheel.Installed() && !g_LiveZoomWheel.KeyboardInstalled(),
            "A denied optional keyboard hook must keep LiveZoom operational and release interception");
    SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(level()==2.25f,"Keyboard factor adjustment must survive denied Escape hook installation");
    ++results.escapeFailureCases;TraceLiveWheelPhase("fault injection done");
    zoomit::LiveZoomWheel::denyKeyboardInstallation.store(false);clean();openEscape("keyboard hook retry");
    InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !IsWindow(g_hWndLiveZoom);},"A new explicit session must retry an initially denied keyboard hook");++results.escapeFailureCases;clean();
    openEscape("Escape post failure");const auto failedPosts=zoomit::LiveZoomWheel::escapePosts.load();const size_t failedObserved=downstreamEscapeDown;
    TraceLiveWheelPhase("Escape post fault injection begin");
    zoomit::LiveZoomWheel::denyInputPost.store(true);InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !g_LiveZoomWheel.Installed() && !g_LiveZoomWheel.KeyboardInstalled();},"Denied Escape posting must retire optional input interception");
    require(IsWindowVisible(g_hWndLiveZoom) && level()==2.0f && downstreamEscapeDown==failedObserved+1 &&
            zoomit::LiveZoomWheel::escapePosts.load()==failedPosts,"A failed exit post must pass Escape through and keep the app usable");
    zoomit::LiveZoomWheel::denyInputPost.store(false);++results.escapeFailureCases;
    TraceLiveWheelPhase("Escape post fault injection done");clean();openEscape("Escape post retry");
    InjectTestKey(VK_ESCAPE,false);InjectTestKey(VK_ESCAPE,true);
    waitFor([]{return !IsWindow(g_hWndLiveZoom);},"The next explicit session must retry Escape after a posting failure");++results.escapeFailureCases;clean();
    g_fullScreenWorkaround=FALSE;g_AnimateLiveZoom=FALSE;openEscape("legacy wheel modal tests");
    const ULONG_PTR beforeOptions=g_LiveZoomWheel.Epoch();
    g_PolicyOptionsCheck=[&] {
        const float before=level();const size_t observed=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);
        PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,beforeOptions,WHEEL_DELTA);pump(20);
        require(level()==before && downstreamWheelEvents==observed+1,"Options must block live wheel input and queued requests without changing its controls");
        ++results.blockedCases;++results.staleCases;
    };
    {
        WheelModalWatchdog watchdog;TraceLiveWheelPhase("legacy wheel options begin");
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,PolicyOptionsProc);
        TraceLiveWheelPhase("legacy wheel options returned");
    }
    g_PolicyOptionsCheck={};RethrowTestCallbackFailure();
    SendMessage(g_hWndMain,WM_USER_SESSION_TICK,3,0);require(g_LiveZoomWheel.Installed(),"Ending options must allow wheel zoom again");
    g_PolicyModalCheck=[&] {
        TraceLiveWheelPhase("legacy wheel Snip callback begin");
        const size_t observed=downstreamWheelEvents;
        CtrlTestWheel(WHEEL_DELTA);require(downstreamWheelEvents==observed+1,"Snip selection must not intercept Ctrl+wheel");++results.blockedCases;
        TraceLiveWheelPhase("legacy wheel Snip callback done");
    };
    cancelSnip=true;
    {
        const UINT_PTR selectionTimer=SetTimer(nullptr,0,15,SelectPolicyRegion);
        require(selectionTimer!=0,"Arm the legacy wheel Snip test selection callback");
        const auto stopSelectionTimer=zoomit::OnExit([&]{KillTimer(nullptr,selectionTimer);});
        WheelModalWatchdog watchdog;TraceLiveWheelPhase("legacy wheel Snip begin");
        SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,MAKELPARAM(MOD_CONTROL,'5'));
        TraceLiveWheelPhase("legacy wheel Snip returned");
    }
    g_PolicyModalCheck={};cancelSnip=false;RethrowTestCallbackFailure();pump(30);
    require(level()==2.0f && g_LiveZoomWheel.Installed(),"Cancelling Snip must preserve factor and reactivate wheel control");
    // End/re-enter between two half notches: no remainder from the previous session.
    CtrlTestWheel(60);const ULONG_PTR previousSession=g_LiveZoomWheel.Epoch();clean();open();
    CtrlTestWheel(60);require(level()==2.0f,"A new LiveZoom session must discard partial input from the previous session");
    PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,previousSession,WHEEL_DELTA);pump(15);
    require(level()==2.0f,"A wheel request must not survive complete exit and re-entry");++results.staleCases;
    CtrlTestWheel(60);require(level()==2.25f,"Only current-session partial input may form a notch");++results.partialCases;
    clean();
    const auto deniedInstallation=[&] {
        liveWheelPhaseContext="denied worker hooks";TraceLiveWheelPhase("fault injection begin");
        zoomit::LiveZoomWheel::denyInstallation.store(true);
        ActivateTestHost(host);SetCursorPos(125,125);SetInitialZoomIndex(3);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));pump(40);
        require(IsWindowVisible(g_hWndLiveZoom) && !g_LiveZoomWheel.Installed(),"A denied optional hook must leave LiveZoom operational");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(level()==2.25f,"Keyboard zoom must remain available after hook installation failure");++results.failureCases;
        clean();zoomit::LiveZoomWheel::denyInstallation.store(false);open();
        CtrlTestWheel(WHEEL_DELTA);require(level()==2.25f,"The next explicit session must retry a previously denied hook");++results.failureCases;clean();
        TraceLiveWheelPhase("fault injection done");
    };
    std::vector<WheelResources> resources;
    resources.push_back(ReadWheelResources("before controlled report warmup"));
    // The first localized system error description creates two cached OS handles.
    // Exercise the real failure/report path once before measuring repeatable ownership.
    deniedInstallation();
    resources.push_back(ReadWheelResources("after controlled report warmup"));
    const HANDLE worker=g_LiveZoomWheel.Thread();require(worker!=nullptr,"A sleeping worker must be reused instead of recreated each time");
    results.handlesBefore=resources.back().handles;
    const auto ownershipBaseline=resources.back();
    liveWheelPhaseContext="reusable worker cycles";TraceLiveWheelPhase("25 cycles begin");
    for(int i=0;i<25;++i) {
        open();CtrlTestWheel(WHEEL_DELTA);CtrlTestWheel(-WHEEL_DELTA);
        require(level()==2.0f && g_LiveZoomWheel.Thread()==worker,"Repeated entry/exit must retain exact targets and reuse one worker");
        clean();++results.cycles;
    }
    GetProcessHandleCount(GetCurrentProcess(),&results.handlesAfter);
    require(results.handlesAfter<=results.handlesBefore+1,"Repeated interception must not leak thread, event or hook handles");
    TraceLiveWheelPhase("25 cycles done");resources.push_back(ReadWheelResources("after reusable-worker cycles"));
    for(int cycle=0;cycle<3;++cycle) {
        deniedInstallation();
        resources.push_back(ReadWheelResources("after repeated denied installation and recovery"));
    }
    // A failed delivery must pass through rather than swallow the user's wheel.
    liveWheelPhaseContext="denied wheel post";TraceLiveWheelPhase("wheel fault begin");
    open();CtrlTestWheel(60);require(level()==2.0f,"Prepare a partial notch before denied delivery");
    const ULONG_PTR deniedEpoch=g_LiveZoomWheel.Epoch();
    const size_t beforeDenied=downstreamWheelEvents;
    zoomit::LiveZoomWheel::denyInputPost.store(true);CtrlTestWheel(60);
    waitFor([&]{return downstreamWheelEvents==beforeDenied+1 && !g_LiveZoomWheel.Installed();},
            "Failed wheel delivery must pass through and retire interception without ending LiveZoom");
    require(IsWindowVisible(g_hWndLiveZoom) && level()==2.0f && !g_LiveZoomWheel.Accept(g_hWndMain,deniedEpoch),
            "Denied posting must preserve the live viewport and invalidate the failed generation");
    zoomit::LiveZoomWheel::denyInputPost.store(false);
    PostMessage(g_hWndMain,WM_USER_LIVE_ZOOM_WHEEL,deniedEpoch,WHEEL_DELTA);pump(20);
    require(level()==2.0f,"A queued request from failed posting must remain obsolete");
    SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);require(level()==2.25f,"Keyboard must remain available after input posting failure");
    ++results.queueFailureCases;clean();open();
    CtrlTestWheel(60);require(level()==2.0f,"Failed-session partial input must not enter a replacement session");
    CtrlTestWheel(60);require(level()==2.25f,"A new session must retry posting and use only its own partial input");
    ++results.queueFailureCases;clean();

    TraceLiveWheelPhase("wheel fault done");resources.push_back(ReadWheelResources("after denied posting and recovery"));
    const auto duplicateThread=[](HANDLE thread) {
        HANDLE duplicate{};
        require(DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&duplicate,SYNCHRONIZE,FALSE,0)!=FALSE,
                "Retain a safe wait handle for an owned worker across shutdown");
        return native::unique_handle(duplicate);
    };
    // Local fixtures use an owned test host and do not enter a user application mode.
    constexpr UINT fixtureInput=WM_APP+61,fixtureError=WM_APP+62;
    liveWheelPhaseContext="worker termination/recreation";TraceLiveWheelPhase("worker fault begin");
    {
        zoomit::LiveZoomWheel probe;
        require(probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_SUCCESS && probe.Installed(),
                "Activate an isolated wheel worker before a controlled termination");
        const ULONG_PTR serial=probe.WorkerSerial(),epoch=probe.Epoch();
        auto stopped=duplicateThread(probe.Thread());probe.RequestStopForTesting();
        waitFor([&]{return WaitForSingleObject(stopped.get(),0)==WAIT_OBJECT_0;},"Controlled worker termination must finish without terminating another process");
        require(!probe.Accept(host,epoch),"An ended worker must reject pending input even before the UI reaps it");
        require(probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_OPERATION_ABORTED && !probe.Thread(),
                "Reap a confirmed ended worker and report its failure once");
        require(probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_SUCCESS && !probe.Thread(),
                "An ended worker must stay disabled for its failed session without repeated errors");
        require(probe.SetTarget(nullptr,0,0)==ERROR_SUCCESS && probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_SUCCESS &&
                probe.Installed() && probe.WorkerSerial()!=serial && !probe.Accept(host,epoch),
                "The next explicit session must create a fresh worker and generation");
        ++results.workerRecoveryCases;
    }
    TraceLiveWheelPhase("worker fault done");resources.push_back(ReadWheelResources("after terminated-worker recreation"));
    liveWheelPhaseContext="delayed worker reuse";TraceLiveWheelPhase("worker fault begin");
    {
        zoomit::LiveZoomWheel probe;
        zoomit::LiveZoomWheel::startupDelayMilliseconds.store(600);
        const ULONGLONG began=GetTickCount64();
        require(probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_TIMEOUT && GetTickCount64()-began<1500,
                "A delayed optional worker must return a bounded activation timeout");
        const ULONG_PTR serial=probe.WorkerSerial(),obsolete=probe.Epoch();
        zoomit::LiveZoomWheel::startupDelayMilliseconds.store(0);
        require(!probe.Accept(host,obsolete) && !probe.Installed(),"A timed out activation must never accept late input");
        require(probe.SetTarget(nullptr,0,0)==ERROR_SUCCESS && probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_SUCCESS &&
                probe.Installed() && probe.WorkerSerial()==serial,
                "A merely delayed worker must be reused after recovery rather than duplicated");
        require(!probe.Accept(host,obsolete),"Late input from timed out activation must remain invalid after recovery");
        ++results.timeoutCases;
    }
    TraceLiveWheelPhase("worker fault done");resources.push_back(ReadWheelResources("after delayed-worker reuse"));
    liveWheelPhaseContext="delayed worker shutdown";TraceLiveWheelPhase("worker fault begin");
    {
        zoomit::LiveZoomWheel probe;
        zoomit::LiveZoomWheel::startupDelayMilliseconds.store(1200);
        require(probe.SetTarget(host,fixtureInput,fixtureError)==ERROR_TIMEOUT,"Prepare a worker that outlives the bounded shutdown wait");
        auto stopped=duplicateThread(probe.Thread());
        const ULONGLONG began=GetTickCount64();probe.Shutdown();
        zoomit::LiveZoomWheel::startupDelayMilliseconds.store(0);
        require(GetTickCount64()-began<1000 && !probe.Thread(),"Shutdown must release its UI owner without waiting indefinitely for the worker");
        waitFor([&]{return WaitForSingleObject(stopped.get(),0)==WAIT_OBJECT_0;},
                "The delayed worker must finish safely after its UI owner has released it");
        ++results.timeoutCases;
    }
    TraceLiveWheelPhase("worker fault done");resources.push_back(ReadWheelResources("after bounded delayed shutdown"));
    results.handlesAfter=resources.back().handles;
    if(results.handlesAfter>results.handlesBefore+1 || resources.back().states!=ownershipBaseline.states ||
       resources.back().workers!=ownershipBaseline.workers)for(const auto& value:resources)PrintWheelResources(value);
    require(resources.back().states==ownershipBaseline.states && resources.back().workers==ownershipBaseline.workers,
            "Failure and delayed teardown must release every additional wheel state and worker");
    require(results.handlesAfter<=results.handlesBefore+1,
            "Worker failure, recreation and delayed shutdown must not leak native handles");
    return results;
}
void PrintLiveWheelResults(const LiveWheelResults& result) {
    std::cout<<"{\"passed\":true,\"cursor_check_scope\":\"logical-state-or-fixture-cleanup\",\"visual_cursor_verification\":false,\"live_wheel\":true,\"native_cases\":"<<result.nativeCases
        <<",\"minimum_zoom_cases\":"<<result.minimumCases<<",\"partial_input_cases\":"<<result.partialCases<<",\"passthrough_cases\":"<<result.passthroughCases
        <<",\"blocked_cases\":"<<result.blockedCases<<",\"stale_request_cases\":"<<result.staleCases
        <<",\"entry_exit_cycles\":"<<result.cycles<<",\"installation_failure_cases\":"<<result.failureCases
        <<",\"escape_exit_cases\":"<<result.escapeCases<<",\"escape_draw_modal_cases\":"<<result.escapeBlockedCases
        <<",\"escape_modifier_passthrough_cases\":"<<result.escapeModifierCases<<",\"escape_post_failure_cases\":"<<result.escapeFailureCases
        <<",\"worker_recovery_cases\":"<<result.workerRecoveryCases<<",\"input_post_failure_cases\":"<<result.queueFailureCases
        <<",\"bounded_worker_timeout_cases\":"<<result.timeoutCases
        <<",\"handles_before\":"<<result.handlesBefore<<",\"handles_after\":"<<result.handlesAfter<<"}\n";
}
