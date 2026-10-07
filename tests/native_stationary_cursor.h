#pragma once
#include <array>
#include <cstdint>

// Read-only Win32 evidence. This observes logical cursor state, not the pixels
// scanned out by the display. It must never activate, move or repair the cursor.
struct NativeCursorObservation {
    bool cursorRead{},positionRead{},foregroundRead{};
    DWORD cursorError{},positionError{},foregroundError{};
    CURSORINFO cursor{sizeof(CURSORINFO)};
    POINT position{};
    HWND foreground{};
    GUITHREADINFO foregroundInfo{sizeof(GUITHREADINFO)};
};
NativeCursorObservation ObserveNativeCursor() noexcept {
    NativeCursorObservation value;
    value.cursorRead=GetCursorInfo(&value.cursor)!=FALSE;
    if(!value.cursorRead)value.cursorError=GetLastError();
    value.positionRead=GetCursorPos(&value.position)!=FALSE;
    if(!value.positionRead)value.positionError=GetLastError();
    value.foreground=GetForegroundWindow();
    value.foregroundRead=GetGUIThreadInfo(0,&value.foregroundInfo)!=FALSE;
    if(!value.foregroundRead)value.foregroundError=GetLastError();
    return value;
}

// Only the test-owned child writes this shared block. Its normal window and
// actual Edit control choose their native cursors through Windows defaults.
struct alignas(8) NativeCursorHostState {
    DWORD parentId{};
    volatile LONG ready{},failed{},moves{},buttons{},wheels{},cursorEvents{},keyDowns{},keyUps{};
    volatile LONG64 window{},edit{};
};
struct NativeCursorChildContext {NativeCursorHostState* state{};HANDLE parent{};};
LONG ReadNativeCounter(volatile LONG& value) noexcept {return InterlockedCompareExchange(&value,0,0);}
HWND ReadNativeWindow(volatile LONG64& value) noexcept {
    return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(InterlockedCompareExchange64(&value,0,0)));
}
void RecordNativeHostInput(NativeCursorHostState* state,UINT message) noexcept {
    if(!state)return;
    if(message==WM_MOUSEMOVE)InterlockedIncrement(&state->moves);
    if(message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN)
        InterlockedIncrement(&state->buttons);
    if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)InterlockedIncrement(&state->wheels);
    if(message==WM_SETCURSOR)InterlockedIncrement(&state->cursorEvents);
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN)InterlockedIncrement(&state->keyDowns);
    if(message==WM_KEYUP || message==WM_SYSKEYUP)InterlockedIncrement(&state->keyUps);
}
LRESULT CALLBACK NativeCursorEditProc(HWND hwnd,UINT message,WPARAM word,LPARAM param,
                                     UINT_PTR id,DWORD_PTR context) noexcept {
    RecordNativeHostInput(reinterpret_cast<NativeCursorHostState*>(context),message);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,NativeCursorEditProc,id);
    return DefSubclassProc(hwnd,message,word,param);
}
LRESULT CALLBACK NativeCursorHostProc(HWND hwnd,UINT message,WPARAM word,LPARAM param) noexcept {
    auto* context=reinterpret_cast<NativeCursorChildContext*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        context=static_cast<NativeCursorChildContext*>(reinterpret_cast<CREATESTRUCTW*>(param)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));
    }
    if(context) {
        RecordNativeHostInput(context->state,message);
        // A CTest timeout or parent failure cannot leave this owned helper behind.
        if(message==WM_TIMER && word==1 && WaitForSingleObject(context->parent,0)==WAIT_OBJECT_0) {
            DestroyWindow(hwnd);return 0;
        }
        if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    }
    return DefWindowProcW(hwnd,message,word,param);
}
int RunNativeCursorHostChild(const char* token) noexcept {
    if(!token || !*token)return 2;
    for(const char* character=token;*character;++character)
        if((*character<'0' || *character>'9') && *character!='_')return 2;
    const std::wstring suffix(token,token+strlen(token));
    const std::wstring name=L"Local\\ZoomItNativeCursor_"+suffix;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L"_ready").c_str());
    auto* state=mapping ? static_cast<NativeCursorHostState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeCursorHostState))) : nullptr;
    auto close=zoomit::OnExit([&]{if(state)UnmapViewOfFile(state);if(ready)CloseHandle(ready);if(mapping)CloseHandle(mapping);});
    if(!state || !ready)return 2;
    NativeCursorChildContext context{state,OpenProcess(SYNCHRONIZE,FALSE,state->parentId)};
    auto closeParent=zoomit::OnExit([&]{if(context.parent)CloseHandle(context.parent);});
    if(!context.parent) {InterlockedExchange(&state->failed,1);SetEvent(ready);return 2;}
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    const HBRUSH background=CreateSolidBrush(RGB(42,70,94));
    auto closeBrush=zoomit::OnExit([&]{if(background)DeleteObject(background);});
    WNDCLASSW type{};type.lpfnWndProc=NativeCursorHostProc;type.hInstance=instance;
    type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hbrBackground=background;
    type.lpszClassName=L"ZoomItNativeStationaryCursorHost";
    if(!background || !RegisterClassW(&type)) {InterlockedExchange(&state->failed,1);SetEvent(ready);return 2;}
    auto unregister=zoomit::OnExit([&]{UnregisterClassW(type.lpszClassName,instance);});
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int width=(std::min)(700,(std::max)(400,static_cast<int>(work.right-work.left)-80));
    const int height=(std::min)(500,(std::max)(320,static_cast<int>(work.bottom-work.top)-80));
    HWND host=CreateWindowExW(0,type.lpszClassName,L"Native stationary cursor: arrow or Edit I-beam",
        WS_OVERLAPPEDWINDOW,work.left+40,work.top+40,width,height,nullptr,nullptr,instance,&context);
    HWND edit=host ? CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"A real native Edit control. Keep the mouse still.",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_READONLY,20,170,width-60,90,host,
        reinterpret_cast<HMENU>(101),instance,nullptr) : nullptr;
    if(!host || !edit || !SetWindowSubclass(edit,NativeCursorEditProc,1,reinterpret_cast<DWORD_PTR>(state)) ||
       !SetTimer(host,1,1000,nullptr)) {
        if(host)DestroyWindow(host);InterlockedExchange(&state->failed,1);SetEvent(ready);return 2;
    }
    InterlockedExchange64(&state->window,reinterpret_cast<LONG64>(host));
    InterlockedExchange64(&state->edit,reinterpret_cast<LONG64>(edit));
    ShowWindow(host,SW_SHOWNORMAL);UpdateWindow(host);
    InterlockedExchange(&state->ready,1);SetEvent(ready);
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
    return 0;
}
class NativeCursorHostProcess {
    HANDLE mapping_{},ready_{},process_{};
    NativeCursorHostState* state_{};
    void Cleanup() noexcept {
        if(state_) {
            if(HWND host=ReadNativeWindow(state_->window);host && IsWindow(host))PostMessageW(host,WM_CLOSE,0,0);
        }
        if(process_) {
            // This handle identifies only the helper this fixture created, never a user app.
            if(WaitForSingleObject(process_,3000)==WAIT_TIMEOUT) {
                TerminateProcess(process_,2);WaitForSingleObject(process_,1000);
            }
            CloseHandle(process_);process_=nullptr;
        }
        if(state_) {UnmapViewOfFile(state_);state_=nullptr;}
        if(ready_) {CloseHandle(ready_);ready_=nullptr;}
        if(mapping_) {CloseHandle(mapping_);mapping_=nullptr;}
    }
public:
    NativeCursorHostProcess() {
        try {
            const std::wstring token=std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
            const std::wstring name=L"Local\\ZoomItNativeCursor_"+token;
            mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(NativeCursorHostState),name.c_str());
            require(mapping_ && GetLastError()!=ERROR_ALREADY_EXISTS,"Create a new owned cursor-host mapping");
            state_=static_cast<NativeCursorHostState*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeCursorHostState)));
            require(state_!=nullptr,"Map the owned cursor-host state");
            *state_={};state_->parentId=GetCurrentProcessId();
            ready_=CreateEventW(nullptr,TRUE,FALSE,(name+L"_ready").c_str());
            require(ready_!=nullptr,"Create cursor-host readiness event");
            wchar_t executable[32768]{};
            const DWORD length=GetModuleFileNameW(nullptr,executable,_countof(executable));
            require(length && length<_countof(executable),"Read native regression executable path");
            std::wstring command=L"\""+std::wstring(executable)+L"\" --stationary-cursor-host "+token;
            STARTUPINFOW startup{sizeof(startup)}; // CREATE_NO_WINDOW hides only the console; the owned native host is shown normally.
            PROCESS_INFORMATION process{};
            require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,
                "Launch the test-owned native cursor host in a separate process");
            process_=process.hProcess;CloseHandle(process.hThread);
            const auto limit=GetTickCount64()+5000;
            while(WaitForSingleObject(ready_,0)!=WAIT_OBJECT_0 && GetTickCount64()<limit &&
                  WaitForSingleObject(process_,0)==WAIT_TIMEOUT)pump(5);
            require(ReadNativeCounter(state_->ready)!=0 && !ReadNativeCounter(state_->failed) &&
                    IsWindow(Window()) && IsWindow(Edit()),"Wait for the separate native cursor host");
        } catch (...) {Cleanup();throw;}
    }
    ~NativeCursorHostProcess(){Cleanup();}
    NativeCursorHostProcess(const NativeCursorHostProcess&)=delete;
    HWND Window() const noexcept {return ReadNativeWindow(state_->window);}
    HWND Edit() const noexcept {return ReadNativeWindow(state_->edit);}
    NativeCursorHostState& State() const noexcept {return *state_;}
};

// Observe our own live parent before its native removal dispatch reaches the
// production handler. This checks release ordering, not physical cursor pixels.
class NativeLiveCursorRemovalProbe {
    HWND parent_{},child_{};
    bool installed_{};
    static constexpr UINT_PTR SubclassId=0x4e435250;
    static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM word,LPARAM param,
                                 UINT_PTR id,DWORD_PTR context) noexcept {
        auto* self=reinterpret_cast<NativeLiveCursorRemovalProbe*>(context);
        if(self && !self->observed) {
            const bool positionedHide=message==WM_WINDOWPOSCHANGING && param &&
                (reinterpret_cast<const WINDOWPOS*>(param)->flags&SWP_HIDEWINDOW);
            if(positionedHide || (message==WM_SHOWWINDOW && word==FALSE) || message==WM_DESTROY) {
                self->observed=true;self->firstMessage=message;
                if(positionedHide)self->positionFlags=reinterpret_cast<const WINDOWPOS*>(param)->flags;
                self->childValid=IsWindow(self->child_) && GetParent(self->child_)==window;
                if(self->childValid) {
                    // Preserve the owner's last-error value across this passive probe.
                    const DWORD previous=GetLastError();SetLastError(ERROR_SUCCESS);
                    self->styleAtRemoval=GetWindowLongPtrW(self->child_,GWL_STYLE);
                    self->styleError=GetLastError();
                    self->styleRead=self->styleAtRemoval!=0 || self->styleError==ERROR_SUCCESS;
                    SetLastError(previous);
                }
            }
        }
        if(message==WM_NCDESTROY) {
            if(self)self->installed_=false;
            RemoveWindowSubclass(window,Proc,id);
        }
        return DefSubclassProc(window,message,word,param);
    }
public:
    bool observed{},childValid{},styleRead{};
    UINT firstMessage{},positionFlags{};
    DWORD styleError{};
    LONG_PTR initialStyle{},styleAtRemoval{};
    NativeLiveCursorRemovalProbe(HWND parent,HWND child):parent_(parent),child_(child) {
        require(IsWindow(parent_) && IsWindow(child_) && GetParent(child_)==parent_,
            "The teardown observer must attach to the existing live parent and its own magnifier");
        require(GetWindowThreadProcessId(parent_,nullptr)==GetCurrentThreadId(),
            "The passive live teardown subclass must run on its owning UI thread");
        initialStyle=GetWindowLongPtrW(child_,GWL_STYLE);
        require((initialStyle&MS_SHOWMAGNIFIEDCURSOR)!=0,
            "The native live trial must begin with the magnifier's cursor style enabled");
        installed_=SetWindowSubclass(parent_,Proc,SubclassId,reinterpret_cast<DWORD_PTR>(this))!=FALSE;
        require(installed_,"Attach the passive own-thread LiveZoom teardown observer");
    }
    ~NativeLiveCursorRemovalProbe() {
        if(installed_ && IsWindow(parent_))RemoveWindowSubclass(parent_,Proc,SubclassId);
    }
    NativeLiveCursorRemovalProbe(const NativeLiveCursorRemovalProbe&)=delete;
    bool ReleasedBeforeRemoval() const noexcept {
        return observed && childValid && styleRead && !(styleAtRemoval&MS_SHOWMAGNIFIEDCURSOR);
    }
    void Print(unsigned trial) const {
        std::cout<<"{\"stationary_cursor_handoff\":true,\"trial\":"<<trial
            <<",\"initial_child_style\":"<<initialStyle<<",\"first_removal_message\":"<<firstMessage
            <<",\"first_removal_position_flags\":"<<positionFlags<<",\"removal_observed\":"<<observed
            <<",\"child_valid_at_first_removal\":"<<childValid<<",\"child_style_read\":"<<styleRead
            <<",\"child_style_error\":"<<styleError<<",\"child_style_at_first_removal\":"<<styleAtRemoval
            <<",\"handoff_owner_disabled_before_removal\":"<<ReleasedBeforeRemoval()
            <<",\"scope\":\"own-control-removal-order-only\",\"visual_verified\":false}\n";
    }
};

// Passive low-level observer, with no return-path suppression or cursor changes.
// It catches movement away and back between samples, which final coordinates miss.
struct NativeStationaryMouseTrace {
    bool tracking{};
    POINT last{};
    unsigned moveEvents{},coordinateChanges{},buttons{},wheels{},injectedMoves{};
};
inline NativeStationaryMouseTrace* g_NativeStationaryMouseTrace{};
LRESULT CALLBACK NativeStationaryMouseObserver(int code,WPARAM message,LPARAM param) noexcept {
    auto* trace=g_NativeStationaryMouseTrace;
    if(code==HC_ACTION && trace && trace->tracking) {
        const auto* event=reinterpret_cast<const MSLLHOOKSTRUCT*>(param);
        if(message==WM_MOUSEMOVE) {
            ++trace->moveEvents;if(event->flags&LLMHF_INJECTED)++trace->injectedMoves;
            if(event->pt.x!=trace->last.x || event->pt.y!=trace->last.y)++trace->coordinateChanges;
            trace->last=event->pt;
        }
        if(message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN)++trace->buttons;
        if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)++trace->wheels;
    }
    return CallNextHookEx(nullptr,code,message,param);
}
unsigned InjectNativeStationaryKey(bool escape) {
    std::array<INPUT,4> input{};unsigned count{};
    auto key=[&](WORD value,bool up) {auto& item=input[count++];item.type=INPUT_KEYBOARD;item.ki.wVk=value;item.ki.dwFlags=up ? KEYEVENTF_KEYUP : 0;};
    if(!escape)key(VK_CONTROL,false);
    key(escape ? VK_ESCAPE : WORD('2'),false);key(escape ? VK_ESCAPE : WORD('2'),true);
    if(!escape)key(VK_CONTROL,true);
    const UINT sent=SendInput(count,input.data(),sizeof(INPUT));
    if(sent!=count) {
        // Only balance keys the fixture successfully pressed before the failure.
        INPUT releases[2]{};unsigned releaseCount{};
        for(UINT index=0;index<sent;++index)if(!(input[index].ki.dwFlags&KEYEVENTF_KEYUP)) {
            bool released=false;
            for(UINT next=index+1;next<sent;++next)if(input[next].ki.wVk==input[index].ki.wVk && (input[next].ki.dwFlags&KEYEVENTF_KEYUP))released=true;
            if(!released) {auto& item=releases[releaseCount++];item.type=INPUT_KEYBOARD;item.ki.wVk=input[index].ki.wVk;item.ki.dwFlags=KEYEVENTF_KEYUP;}
        }
        if(releaseCount)SendInput(releaseCount,releases,sizeof(INPUT));
        require(false,"Inject the complete native keyboard gesture without moving the mouse");
    }
    return count;
}
void PrintNativeCursorSample(unsigned trial,DWORD requestedDelay,ULONGLONG elapsed,
                             const NativeCursorObservation& value,const POINT& start,HCURSOR initial,
                             HWND expectedForeground,bool lastMagVisibilityRequest) {
    const bool positionSame=value.positionRead && start.x==value.position.x && start.y==value.position.y;
    std::cout<<"{\"stationary_cursor_sample\":true,\"trial\":"<<trial<<",\"requested_ms\":"<<requestedDelay
        <<",\"elapsed_ms\":"<<elapsed<<",\"cursor_read\":"<<value.cursorRead<<",\"cursor_error\":"<<value.cursorError
        <<",\"logical_flags\":"<<value.cursor.flags<<",\"logical_shape\":"<<reinterpret_cast<ULONG_PTR>(value.cursor.hCursor)
        <<",\"same_shape\":"<<(value.cursorRead && value.cursor.hCursor==initial)
        <<",\"position_read\":"<<value.positionRead<<",\"position_error\":"<<value.positionError
        <<",\"x\":"<<value.position.x<<",\"y\":"<<value.position.y<<",\"same_position\":"<<positionSame
        <<",\"foreground\":"<<reinterpret_cast<ULONG_PTR>(value.foreground)
        <<",\"original_foreground\":"<<(value.foreground==expectedForeground)
        <<",\"foreground_info_read\":"<<value.foregroundRead<<",\"foreground_info_error\":"<<value.foregroundError
        <<",\"capture_window\":"<<reinterpret_cast<ULONG_PTR>(value.foregroundInfo.hwndCapture)
        <<",\"last_mag_visibility_request\":"<<lastMagVisibilityRequest
        <<",\"visual_verified\":false}\n";
}

// Return 77 only for a precondition that makes the native trial inconclusive.
// Successful logical checks must never be described as proof of visible scanout.
int RunNativeStationaryCursorRegression() {
    std::cout<<std::boolalpha;
    const DWORD savedLiveKey=g_LiveZoomToggleKey,savedLiveMod=g_LiveZoomToggleMod;
    const DWORD savedIndex=g_SliderZoomLevel,savedPercent=g_InitialZoomPercent,savedLegacy=g_LegacySliderZoomLevel;
    const BOOLEAN savedAnimation=g_AnimateLiveZoom;
    const bool savedFullscreen=g_fullScreenWorkaround;
    const auto restore=zoomit::OnExit([&] {
        if(IsWindow(g_hWndLiveZoom))DestroyWindow(g_hWndLiveZoom);
        UnregisterHotKey(g_hWndMain,LIVE_HOTKEY);
        g_LiveZoomToggleKey=savedLiveKey;g_LiveZoomToggleMod=savedLiveMod;
        g_SliderZoomLevel=savedIndex;g_InitialZoomPercent=savedPercent;g_LegacySliderZoomLevel=savedLegacy;
        g_AnimateLiveZoom=savedAnimation;g_fullScreenWorkaround=savedFullscreen;
    });
    for(int key:{VK_CONTROL,VK_SHIFT,VK_MENU,VK_LWIN,VK_RWIN,VK_ESCAPE,int('2')})
        if(GetAsyncKeyState(key)&0x8000) {
            std::cout<<"{\"inconclusive\":true,\"reason\":\"required keyboard keys already held\",\"visual_verified\":false}\n";return 77;
        }
    g_LiveZoomToggleKey=(HOTKEYF_CONTROL<<8)|'2';g_LiveZoomToggleMod=MOD_CONTROL;
    if(!RegisterHotKey(g_hWndMain,LIVE_HOTKEY,MOD_CONTROL,'2')) {
        const DWORD error=GetLastError();
        std::cout<<"{\"inconclusive\":true,\"reason\":\"Ctrl+2 unavailable; no existing app was stopped\",\"error\":"<<error<<",\"visual_verified\":false}\n";return 77;
    }
    NativeCursorHostProcess host;
    require(!(GetWindowLongPtrW(host.Window(),GWL_EXSTYLE)&WS_EX_TOPMOST),"The separate cursor host must be a normal, non-topmost window");
    NativeStationaryMouseTrace trace;
    g_NativeStationaryMouseTrace=&trace;
    HHOOK observer=SetWindowsHookExW(WH_MOUSE_LL,NativeStationaryMouseObserver,GetModuleHandleW(nullptr),0);
    auto unhook=zoomit::OnExit([&] {trace.tracking=false;if(observer)UnhookWindowsHookEx(observer);g_NativeStationaryMouseTrace=nullptr;});
    require(observer!=nullptr,"Install a passive mouse-event observer for stationary evidence");
    g_fullScreenWorkaround=false;SetInitialZoomIndex(3);
    unsigned trials{},samples{},shapeChanges{},foregroundChanges{},hostMoves{},hostCursorEvents{},keyboardInputs{},handoffObservations{},handoffPasses{};
    bool logicalChecks=true,handoffChecks=true;
    for(bool animated:{false,true})for(bool edit:{false,true})for(bool escape:{false,true}) {
        require(!IsWindow(g_hWndLiveZoom),"Start a native stationary sequence from idle");
        trace.tracking=false;
        const auto foregroundDeadline=GetTickCount64()+2500;
        do {SetForegroundWindow(host.Window());pump(10);}while(GetForegroundWindow()!=host.Window() && GetTickCount64()<foregroundDeadline);
        if(GetForegroundWindow()!=host.Window()) {
            std::cout<<"{\"inconclusive\":true,\"reason\":\"normal child host could not obtain foreground before entry\",\"visual_verified\":false}\n";return 77;
        }
        const HWND surface=edit ? host.Edit() : host.Window();
        POINT point{edit ? 80 : 120,edit ? 35 : 90};
        require(ClientToScreen(surface,&point)!=FALSE && SetCursorPos(point.x,point.y)!=FALSE,
            "Prepare the cursor position once before the native sequence");
        pump(30);
        auto before=ObserveNativeCursor();
        const HCURSOR expected=LoadCursorW(nullptr,edit ? IDC_IBEAM : IDC_ARROW);
        const auto cursorDeadline=GetTickCount64()+2500;
        while((!before.cursorRead || !(before.cursor.flags&CURSOR_SHOWING) || before.cursor.hCursor!=expected) && GetTickCount64()<cursorDeadline) {
            pump(5);before=ObserveNativeCursor();
        }
        if(!before.cursorRead || !before.positionRead || !(before.cursor.flags&CURSOR_SHOWING) || before.cursor.hCursor!=expected) {
            std::cout<<"{\"inconclusive\":true,\"reason\":\"native starting cursor did not expose CURSOR_SHOWING and the expected logical shape\",\"flags\":"<<before.cursor.flags<<",\"visual_verified\":false}\n";return 77;
        }
        const POINT sequencePosition=before.position;
        g_AnimateLiveZoom=animated;
        // Two consecutive cycles share one fixed pointer position. No setup,
        // refocusing or repair is permitted between their exits and re-entries.
        for(unsigned repeat=0;repeat<2;++repeat) {
            trace={};trace.last=sequencePosition;trace.tracking=true;
            const LONG oldMoves=ReadNativeCounter(host.State().moves),oldCursorEvents=ReadNativeCounter(host.State().cursorEvents);
            ++trials;
            std::cout<<"{\"stationary_cursor_trial\":true,\"trial\":"<<trials<<",\"animated\":"<<animated
                <<",\"edit\":"<<edit<<",\"escape\":"<<escape<<",\"repeat\":"<<repeat
                <<",\"hold_ms\":1500,\"input\":\"registered Ctrl+2 and native keyboard only\",\"visual_verified\":false}\n";
            keyboardInputs+=InjectNativeStationaryKey(false);
            const auto entryDeadline=GetTickCount64()+2500;
            while(!IsWindowVisible(g_hWndLiveZoom) && GetTickCount64()<entryDeadline)pump(5);
            require(IsWindowVisible(g_hWndLiveZoom),"The actual registered Ctrl+2 must enter LiveZoom");
            pump(1500);
            NativeLiveCursorRemovalProbe removal(g_hWndLiveZoom,g_hWndLiveZoomMag);
            keyboardInputs+=InjectNativeStationaryKey(escape);
            const auto exitDeadline=GetTickCount64()+2500;
            while(IsWindow(g_hWndLiveZoom) && GetTickCount64()<exitDeadline)pump(1);
            require(!IsWindow(g_hWndLiveZoom),"Native hotkey or Esc must complete LiveZoom exit");
            const auto exitTime=GetTickCount64();
            for(DWORD delay:{0UL,17UL,33UL,100UL,250UL,1000UL}) {
                while(GetTickCount64()-exitTime<delay)pump(1);
                const auto value=ObserveNativeCursor();
                PrintNativeCursorSample(trials,delay,GetTickCount64()-exitTime,value,sequencePosition,before.cursor.hCursor,host.Window(),systemCursorShown);
                ++samples;
                logicalChecks&=value.cursorRead && value.positionRead && (value.cursor.flags&CURSOR_SHOWING) && value.cursor.hCursor &&
                    value.position.x==sequencePosition.x && value.position.y==sequencePosition.y;
                if(value.cursorRead && value.cursor.hCursor!=before.cursor.hCursor)++shapeChanges;
                if(value.foreground!=host.Window())++foregroundChanges;
            }
            // Record only after all fixed cursor samples; never repair a failure.
            removal.Print(trials);
            if(removal.observed)++handoffObservations;
            if(removal.ReleasedBeforeRemoval())++handoffPasses;
            handoffChecks&=removal.ReleasedBeforeRemoval();
            trace.tracking=false;
            const unsigned moves=static_cast<unsigned>(ReadNativeCounter(host.State().moves)-oldMoves);
            const unsigned cursorEvents=static_cast<unsigned>(ReadNativeCounter(host.State().cursorEvents)-oldCursorEvents);
            hostMoves+=moves;hostCursorEvents+=cursorEvents;
            std::cout<<"{\"stationary_cursor_input_trace\":true,\"trial\":"<<trials<<",\"mouse_move_events\":"<<trace.moveEvents
                <<",\"coordinate_change_events\":"<<trace.coordinateChanges<<",\"injected_move_events\":"<<trace.injectedMoves
                <<",\"mouse_buttons\":"<<trace.buttons<<",\"mouse_wheels\":"<<trace.wheels
                <<",\"host_mouse_messages\":"<<moves<<",\"host_setcursor_messages\":"<<cursorEvents<<"}\n";
            if(trace.coordinateChanges || trace.buttons || trace.wheels) {
                std::cout<<"{\"inconclusive\":true,\"reason\":\"mouse moved or received a button/wheel gesture during observation\",\"visual_verified\":false}\n";return 77;
            }
        }
    }
    std::cout<<"{\"logical_checks_passed\":"<<logicalChecks<<",\"stationary_native_input\":true,\"trials\":"<<trials
        <<",\"logical_samples\":"<<samples<<",\"shape_change_samples\":"<<shapeChanges<<",\"foreground_change_samples\":"<<foregroundChanges
        <<",\"native_keyboard_events_injected\":"<<keyboardInputs<<",\"host_mouse_messages\":"<<hostMoves
        <<",\"host_setcursor_messages\":"<<hostCursorEvents
        <<",\"test_cursor_repairs_during_observation\":0,\"test_focus_changes_during_observation\":0,\"test_mouse_injections_during_observation\":0"
        <<",\"handoff_checks_passed\":"<<handoffChecks<<",\"handoff_observations\":"<<handoffObservations
        <<",\"handoff_passes\":"<<handoffPasses
        <<",\"scope\":\"native-input-logical-state-and-own-control-removal-order\",\"visual_verified\":false,\"manual_visual_check_required\":true}\n";
    require(logicalChecks,"Stationary native exits must leave a logically showing cursor at unchanged coordinates; physical visibility requires separate verification");
    require(handoffChecks && handoffObservations==trials && handoffPasses==trials,
        "Every native live exit must disable its magnified-cursor style before its parent starts native removal; physical visibility still requires separate verification");
    return 0;
}
