#pragma once
#include <windows.h>
#include <atomic>
#include <new>

namespace zoomit {

// The hooks never draw, call a window procedure, or allocate. They only post input.
// Its separate message thread is created lazily, sleeps when idle, and is reused.
class LiveZoomWheel {
    struct State {
        std::atomic<unsigned> owners{2};
        std::atomic<HWND> target{};
        std::atomic<ULONG_PTR> epoch{};
        std::atomic<ULONG_PTR> acknowledged{};
        std::atomic<bool> stopping{}, installed{}, keyboardInstalled{}, ended{};
        std::atomic<DWORD> inputError{}, exitError{};
        std::atomic<ULONG_PTR> failedInputEpoch{}, failedInstallationEpoch{};
#ifdef ZOOMIT_TESTING
        ULONG_PTR serial{};
        std::atomic<bool> startupWaiting{};
#endif
        HANDLE changed{}, ready{}, thread{};
        UINT inputMessage{}, errorMessage{};
        std::atomic<UINT> escapeMessage{};
        ULONG_PTR observedEpoch{},observedEscapeEpoch{};
        bool controlledWheel{},escapeDown{},escapeOwned{}; // Used only by the hook thread.
#ifdef ZOOMIT_TESTING
        State() noexcept {liveStates.fetch_add(1);}
#endif
        ~State() {
            if(thread)CloseHandle(thread);if(ready)CloseHandle(ready);if(changed)CloseHandle(changed);
#ifdef ZOOMIT_TESTING
            liveStates.fetch_sub(1);
#endif
        }
        void Release() noexcept { if(owners.fetch_sub(1,std::memory_order_acq_rel)==1)delete this; }
    };
    struct RetainedState {
        State* value{};
        explicit RetainedState(State* state) noexcept : value(state) {value->owners.fetch_add(1);}
        ~RetainedState(){value->Release();}
        RetainedState(const RetainedState&)=delete;
    };
    State* state_{}; // Only the UI thread touches this owner.
    ULONG_PTR nextEpoch_{};
    HWND failedTarget_{};
    bool shuttingDown_{};
    inline static thread_local State* hookState_{};
#ifdef ZOOMIT_TESTING
public:
    inline static std::atomic<bool> denyInstallation{}, denyKeyboardInstallation{}, denyInputPost{};
    inline static std::atomic<unsigned> escapePosts{};
    inline static std::atomic<DWORD> startupDelayMilliseconds{};
    inline static std::atomic<ULONG_PTR> workerSerial{};
    inline static std::atomic<unsigned> liveStates{},liveWorkers{};
private:
#endif
    static bool PostInput(State* state,HWND target,ULONG_PTR epoch,int delta) noexcept {
#ifdef ZOOMIT_TESTING
        if(denyInputPost.load()) {SetLastError(ERROR_NOT_ENOUGH_QUOTA);return false;}
#endif
        return PostMessageW(target,state->inputMessage,epoch,delta)!=FALSE;
    }
    static bool PostEscape(HWND target,ULONG_PTR epoch,UINT message) noexcept {
#ifdef ZOOMIT_TESTING
        if(denyInputPost.load()) {SetLastError(ERROR_NOT_ENOUGH_QUOTA);return false;}
#endif
        const bool posted=PostMessageW(target,message,epoch,0)!=FALSE;
#ifdef ZOOMIT_TESTING
        if(posted)++escapePosts;
#endif
        return posted;
    }
    static void ObserveEscapeEpoch(State* state,ULONG_PTR epoch) noexcept {
        if(state->observedEscapeEpoch==epoch)return;
        state->observedEscapeEpoch=epoch;
        // Consumed DOWN input need not update Windows' asynchronous key state.
        // Keep an owned gesture until its actual UP, including across live sessions.
        if(state->escapeOwned)return;
        // Unowned input may have occurred while the idle hook was removed.
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)state->escapeDown=true;
        else state->escapeDown=false;
    }
    static bool PlainEscape(const KBDLLHOOKSTRUCT& key) noexcept {
        return !(key.flags&LLKHF_ALTDOWN) && !(GetAsyncKeyState(VK_CONTROL)&0x8000) &&
            !(GetAsyncKeyState(VK_SHIFT)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000) &&
            !(GetAsyncKeyState(VK_LWIN)&0x8000) && !(GetAsyncKeyState(VK_RWIN)&0x8000);
    }
    static void InputFailed(State* state,HWND target,ULONG_PTR epoch) noexcept {
        const DWORD error=GetLastError();
        if(epoch!=state->epoch.load(std::memory_order_acquire) || target!=state->target.load(std::memory_order_acquire))return;
        state->inputError.store(error ? error : ERROR_GEN_FAILURE,std::memory_order_relaxed);
        // Associate the error with the original generation, even if the UI changes target now.
        state->failedInputEpoch.store(epoch,std::memory_order_release);
        PostMessageW(target,state->errorMessage,epoch,error ? error : ERROR_GEN_FAILURE);
    }
    static LRESULT CALLBACK MouseHook(int code,WPARAM message,LPARAM param) noexcept {
        State* state=hookState_;
        if(code==HC_ACTION && message==WM_MOUSEWHEEL && state && !state->stopping.load()) {
            const ULONG_PTR epoch=state->epoch.load(std::memory_order_acquire);
            const HWND target=state->target.load(std::memory_order_acquire);
            if(state->observedEpoch!=epoch) {state->observedEpoch=epoch;state->controlledWheel=false;}
            if(target && state->failedInputEpoch.load(std::memory_order_acquire)!=epoch &&
               (GetAsyncKeyState(VK_CONTROL)&0x8000) &&
               !(GetAsyncKeyState(VK_SHIFT)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000) &&
               !(GetAsyncKeyState(VK_LWIN)&0x8000) && !(GetAsyncKeyState(VK_RWIN)&0x8000)) {
                const auto input=reinterpret_cast<const MSLLHOOKSTRUCT*>(param);
                const int delta=static_cast<SHORT>(HIWORD(input->mouseData));
                if(delta && epoch==state->epoch.load(std::memory_order_acquire) &&
                   target==state->target.load(std::memory_order_acquire) &&
                   PostInput(state,target,epoch,delta)) {
                    state->controlledWheel=true;
                    return 1;
                } // Suppress the same Ctrl+wheel gesture only after successful delivery.
                if(delta) {
                    if(epoch==state->epoch.load(std::memory_order_acquire) &&
                       target==state->target.load(std::memory_order_acquire))InputFailed(state,target,epoch);
                    state->controlledWheel=false;
                }
            }
            if(target && state->controlledWheel) {
                // A normal or differently modified wheel gesture ends a partial Ctrl sequence.
                if(!PostInput(state,target,epoch,0))InputFailed(state,target,epoch);
                state->controlledWheel=false;
            }
        }
        return CallNextHookEx(nullptr,code,message,param);
    }
    static LRESULT CALLBACK KeyboardHook(int code,WPARAM message,LPARAM param) noexcept {
        State* state=hookState_;
        if(code==HC_ACTION && state && !state->stopping.load()) {
            const auto key=reinterpret_cast<const KBDLLHOOKSTRUCT*>(param);
            if(key && key->vkCode==VK_ESCAPE &&
               (message==WM_KEYDOWN || message==WM_SYSKEYDOWN || message==WM_KEYUP || message==WM_SYSKEYUP)) {
                const ULONG_PTR epoch=state->epoch.load(std::memory_order_acquire);
                const HWND target=state->target.load(std::memory_order_acquire);
                const UINT exitMessage=state->escapeMessage.load(std::memory_order_acquire);
                ObserveEscapeEpoch(state,epoch);
                const bool eligible=target && exitMessage && epoch==state->epoch.load(std::memory_order_acquire) &&
                    target==state->target.load(std::memory_order_acquire) && exitMessage==state->escapeMessage.load(std::memory_order_acquire);
                const bool up=message==WM_KEYUP || message==WM_SYSKEYUP;
                if(up) {
                    const bool owned=state->escapeOwned;state->escapeDown=false;state->escapeOwned=false;
                    if(owned) {
                        // Retire the retained keyboard hook once the complete
                        // consumed gesture ends, even if LiveZoom has already exited.
                        SetEvent(state->changed);return 1;
                    }
                } else {
                    const bool repeated=state->escapeDown;state->escapeDown=true;
                    // Own the complete gesture, even during idle or an epoch change.
                    // Its repeats must neither leak to another app nor exit a new session.
                    if(state->escapeOwned)return 1;
                    const bool plain=PlainEscape(*key);
                    if(!repeated) {
                        if(eligible && plain && state->failedInputEpoch.load(std::memory_order_acquire)!=epoch &&
                           epoch==state->epoch.load(std::memory_order_acquire) && target==state->target.load(std::memory_order_acquire) &&
                           exitMessage==state->escapeMessage.load(std::memory_order_acquire)) {
                            if(PostEscape(target,epoch,exitMessage)){state->escapeOwned=true;return 1;}
                            InputFailed(state,target,epoch);
                        }
                    }
                }
            }
        }
        return CallNextHookEx(nullptr,code,message,param);
    }
    static DWORD WINAPI Run(void* argument) noexcept {
        State* state=static_cast<State*>(argument);
#ifdef ZOOMIT_TESTING
        liveWorkers.fetch_add(1);
#endif
        hookState_=state;
        HHOOK hook{},keyboard{};
        ULONG_PTR attempted{},keyboardAttempted{};
        MSG message{};
        // Ensure a message queue exists before acknowledging the first activation.
        PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
#ifdef ZOOMIT_TESTING
        const DWORD startupDelay=startupDelayMilliseconds.load();
        if(startupDelay) {
            state->startupWaiting.store(true,std::memory_order_release);
            Sleep(startupDelay);
            state->startupWaiting.store(false,std::memory_order_release);
        }
#endif
        while(!state->stopping.load(std::memory_order_acquire)) {
            const ULONG_PTR epoch=state->epoch.load(std::memory_order_acquire);
            const HWND target=state->target.load(std::memory_order_acquire);
            const UINT exitMessage=state->escapeMessage.load(std::memory_order_acquire);
            if(!target && hook) { UnhookWindowsHookEx(hook);hook=nullptr; }
            // A consumed Escape DOWN keeps this hook only until its real UP;
            // other idle input passes through and the mouse hook is already removed.
            if((!target || !exitMessage) && keyboard && !state->escapeOwned){UnhookWindowsHookEx(keyboard);keyboard=nullptr;}
            if(target && exitMessage)ObserveEscapeEpoch(state,epoch);
            if(target && !hook && attempted!=epoch) {
                attempted=epoch;
#ifdef ZOOMIT_TESTING
                if(denyInstallation.load())SetLastError(ERROR_ACCESS_DENIED);
                else
#endif
                    hook=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
                if(!hook) {
                    const DWORD error=GetLastError();
                    state->inputError.store(error ? error : ERROR_GEN_FAILURE,std::memory_order_relaxed);
                    state->failedInputEpoch.store(epoch,std::memory_order_release);
                    state->failedInstallationEpoch.store(epoch,std::memory_order_release);
                    PostMessageW(target,state->errorMessage,epoch,error ? error : ERROR_GEN_FAILURE);
                }
            }
            if(target && hook && exitMessage && !keyboard && keyboardAttempted!=epoch) {
                keyboardAttempted=epoch;
#ifdef ZOOMIT_TESTING
                if(denyInstallation.load() || denyKeyboardInstallation.load())SetLastError(ERROR_ACCESS_DENIED);
                else
#endif
                    keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,KeyboardHook,GetModuleHandleW(nullptr),0);
                if(!keyboard) {
                    const DWORD error=GetLastError();
                    state->inputError.store(error ? error : ERROR_GEN_FAILURE,std::memory_order_relaxed);
                    state->failedInputEpoch.store(epoch,std::memory_order_release);
                    state->failedInstallationEpoch.store(epoch,std::memory_order_release);
                    PostMessageW(target,state->errorMessage,epoch,error ? error : ERROR_GEN_FAILURE);
                }
            }
            state->keyboardInstalled.store(keyboard!=nullptr,std::memory_order_release);
            state->installed.store(hook && (!exitMessage || keyboard),std::memory_order_release);
            state->acknowledged.store(epoch,std::memory_order_release);
            SetEvent(state->ready);
            const DWORD waited=MsgWaitForMultipleObjectsEx(1,&state->changed,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(waited==WAIT_FAILED) {
                const DWORD error=GetLastError();
                state->exitError.store(error ? error : ERROR_GEN_FAILURE);
                if(target)PostMessageW(target,state->errorMessage,epoch,error ? error : ERROR_GEN_FAILURE);
                break;
            }
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT) {state->stopping.store(true);break;}
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        }
        if(hook)UnhookWindowsHookEx(hook);
        if(keyboard)UnhookWindowsHookEx(keyboard);
        state->keyboardInstalled.store(false,std::memory_order_release);
        state->installed.store(false,std::memory_order_release);
        hookState_=nullptr;
        state->ended.store(true,std::memory_order_release);
#ifdef ZOOMIT_TESTING
        liveWorkers.fetch_sub(1);
#endif
        state->Release();
        return 0;
    }
    static DWORD WaitWithSentMessages(HANDLE event,DWORD milliseconds) noexcept {
        const ULONGLONG deadline=GetTickCount64()+milliseconds;
        for(;;) {
            const ULONGLONG now=GetTickCount64();
            const DWORD remaining=now<deadline ? static_cast<DWORD>(deadline-now) : 0;
            const DWORD waited=MsgWaitForMultipleObjects(1,&event,FALSE,remaining,QS_SENDMESSAGE);
            if(waited!=WAIT_OBJECT_0+1)return waited;
            // Service sent hook callbacks without running queued hotkeys during a transition.
            MSG message{};PeekMessageW(&message,nullptr,WM_NULL,WM_NULL,PM_NOREMOVE);
        }
    }
    ULONG_PTR NewEpoch() noexcept { if(!++nextEpoch_)++nextEpoch_;return nextEpoch_; }
public:
    LiveZoomWheel() noexcept = default;
    LiveZoomWheel(const LiveZoomWheel&)=delete;
    LiveZoomWheel& operator=(const LiveZoomWheel&)=delete;
    ~LiveZoomWheel() { Shutdown(); }
    DWORD SetTarget(HWND target,UINT inputMessage,UINT errorMessage,UINT escapeMessage=0) noexcept {
        if(shuttingDown_)return ERROR_SUCCESS;
        // The worker owns its State until it has actually exited. A timeout alone
        // must never create a second hook or release that worker's resources.
        if(state_ && state_->ended.load(std::memory_order_acquire) &&
           WaitForSingleObject(state_->thread,0)==WAIT_OBJECT_0) {
            State* previous=state_;state_=nullptr;
            const HWND stoppedTarget=previous->target.exchange(nullptr);
            const DWORD error=previous->exitError.load();
            previous->epoch.store(NewEpoch(),std::memory_order_release);
            previous->Release();
            if(target && stoppedTarget==target) {
                failedTarget_=target;
                return error ? error : ERROR_OPERATION_ABORTED;
            }
        }
        if(!target)failedTarget_=nullptr;
        if(target && target==failedTarget_)return ERROR_SUCCESS;
        const bool keyboardChanged=state_ && state_->escapeMessage.load(std::memory_order_acquire)!=escapeMessage;
        if(state_)state_->escapeMessage.store(escapeMessage,std::memory_order_release);
        if(state_ && target && target==state_->target.load(std::memory_order_acquire) &&
           state_->failedInputEpoch.load(std::memory_order_acquire)==state_->epoch.load(std::memory_order_acquire)) {
            const DWORD error=state_->inputError.load();
            failedTarget_=target;state_->target.store(nullptr,std::memory_order_release);
            state_->epoch.store(NewEpoch(),std::memory_order_release);SetEvent(state_->changed);
            return error ? error : ERROR_GEN_FAILURE;
        }
        if(state_ && !keyboardChanged && target==state_->target.load(std::memory_order_acquire))return ERROR_SUCCESS;
        if(!state_ && !target)return ERROR_SUCCESS;
        const ULONG_PTR epoch=NewEpoch();
        if(!state_) {
            State* next=new(std::nothrow) State;
            if(!next) {failedTarget_=target;return ERROR_NOT_ENOUGH_MEMORY;}
            next->changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            next->ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!next->changed || !next->ready) {
                const DWORD error=GetLastError();next->owners.store(1);next->Release();failedTarget_=target;return error;
            }
            next->inputMessage=inputMessage;next->errorMessage=errorMessage;next->escapeMessage.store(escapeMessage);
#ifdef ZOOMIT_TESTING
            next->serial=workerSerial.fetch_add(1)+1;
#endif
            next->epoch.store(epoch);next->target.store(target);
            next->thread=CreateThread(nullptr,64*1024,Run,next,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
            if(!next->thread) {
                const DWORD error=GetLastError();next->owners.store(1);next->Release();failedTarget_=target;return error;
            }
            state_=next;
        } else {
            // Invalidate before publishing the new target, including handle reuse.
            state_->target.store(nullptr,std::memory_order_release);
            state_->epoch.store(epoch,std::memory_order_release);
            state_->target.store(target,std::memory_order_release);
            SetEvent(state_->changed);
        }
        if(target) {
            State* activation=state_;
            RetainedState retain(activation);
            const ULONGLONG deadline=GetTickCount64()+400;
            while(activation->acknowledged.load(std::memory_order_acquire)!=epoch ||
                  (!activation->installed.load(std::memory_order_acquire) &&
                   activation->failedInstallationEpoch.load(std::memory_order_acquire)!=epoch)) {
                if(state_!=activation || shuttingDown_ || activation->epoch.load(std::memory_order_acquire)!=epoch)return ERROR_SUCCESS;
                if(activation->ended.load(std::memory_order_acquire) && WaitForSingleObject(activation->thread,0)==WAIT_OBJECT_0)
                    return SetTarget(target,inputMessage,errorMessage,escapeMessage);
                const auto now=GetTickCount64();
                if(now>=deadline || WaitWithSentMessages(activation->ready,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0) {
                    if(state_!=activation || shuttingDown_ || activation->epoch.load(std::memory_order_acquire)!=epoch)return ERROR_SUCCESS;
                    failedTarget_=target;activation->target.store(nullptr);activation->epoch.store(NewEpoch());SetEvent(activation->changed);
                    return ERROR_TIMEOUT;
                }
            }
        }
        return ERROR_SUCCESS;
    }
    void InvalidatePending() noexcept {
        if(state_ && state_->target.load(std::memory_order_acquire)) {
            state_->epoch.store(NewEpoch(),std::memory_order_release);
            SetEvent(state_->changed);
        }
    }
    bool Accept(HWND target,ULONG_PTR epoch) const noexcept {
        return state_ && !shuttingDown_ && !state_->stopping.load(std::memory_order_acquire) &&
            !state_->ended.load(std::memory_order_acquire) && epoch && state_->epoch.load(std::memory_order_acquire)==epoch &&
            state_->target.load(std::memory_order_acquire)==target;
    }
    void MarkFailed(HWND target,ULONG_PTR epoch) noexcept {
        if(!Accept(target,epoch))return;
        SetTarget(nullptr,0,0);failedTarget_=target;
    }
    void Shutdown() noexcept {
        if(shuttingDown_)return;
        shuttingDown_=true;
        State* previous=state_;state_=nullptr;
        if(!previous)return;
        previous->target.store(nullptr);previous->epoch.store(NewEpoch());previous->stopping.store(true);
        SetEvent(previous->changed);
        // State is also owned by the worker, so a slow external hook cannot cause use-after-free.
        WaitWithSentMessages(previous->thread,400);
        previous->Release();
    }
#ifdef ZOOMIT_TESTING
    ULONG_PTR Epoch() const noexcept { return state_ ? state_->epoch.load() : 0; }
    bool Installed() const noexcept { return state_ && state_->installed.load(); }
    bool KeyboardInstalled() const noexcept { return state_ && state_->keyboardInstalled.load(); }
    HANDLE Thread() const noexcept { return state_ ? state_->thread : nullptr; }
    ULONG_PTR WorkerSerial() const noexcept { return state_ ? state_->serial : 0; }
    bool StartupWaiting() const noexcept { return state_ && state_->startupWaiting.load(std::memory_order_acquire); }
    void RequestStopForTesting() noexcept {
        if(!state_)return;
        state_->exitError.store(ERROR_OPERATION_ABORTED);
        state_->stopping.store(true,std::memory_order_release);SetEvent(state_->changed);
    }
#endif
};
}
