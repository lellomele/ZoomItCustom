#pragma once
#include <windows.h>
#include <atomic>
#include <new>

namespace zoomit {

// The hook never draws, calls a window procedure, or allocates. It only posts input.
// Its separate message thread is created lazily, sleeps when idle, and is reused.
class LiveZoomWheel {
    struct State {
        std::atomic<unsigned> owners{2};
        std::atomic<HWND> target{};
        std::atomic<ULONG_PTR> epoch{};
        std::atomic<ULONG_PTR> acknowledged{};
        std::atomic<bool> stopping{}, installed{};
        HANDLE changed{}, ready{}, thread{};
        UINT inputMessage{}, errorMessage{};
        ULONG_PTR observedEpoch{};
        bool controlledWheel{}; // Used only by the hook thread.
        ~State() { if(thread)CloseHandle(thread);if(ready)CloseHandle(ready);if(changed)CloseHandle(changed); }
        void Release() noexcept { if(owners.fetch_sub(1,std::memory_order_acq_rel)==1)delete this; }
    };
    State* state_{}; // Only the UI thread touches this owner.
    ULONG_PTR nextEpoch_{};
    HWND failedTarget_{};
    bool shuttingDown_{};
    inline static thread_local State* hookState_{};
#ifdef ZOOMIT_TESTING
public:
    inline static std::atomic<bool> denyInstallation{};
private:
#endif
    static LRESULT CALLBACK MouseHook(int code,WPARAM message,LPARAM param) noexcept {
        State* state=hookState_;
        if(code==HC_ACTION && message==WM_MOUSEWHEEL && state && !state->stopping.load()) {
            const ULONG_PTR epoch=state->epoch.load(std::memory_order_acquire);
            const HWND target=state->target.load(std::memory_order_acquire);
            if(state->observedEpoch!=epoch) {state->observedEpoch=epoch;state->controlledWheel=false;}
            if(target && (GetAsyncKeyState(VK_CONTROL)&0x8000) &&
               !(GetAsyncKeyState(VK_SHIFT)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000) &&
               !(GetAsyncKeyState(VK_LWIN)&0x8000) && !(GetAsyncKeyState(VK_RWIN)&0x8000)) {
                const auto input=reinterpret_cast<const MSLLHOOKSTRUCT*>(param);
                const int delta=static_cast<SHORT>(HIWORD(input->mouseData));
                if(delta && epoch==state->epoch.load(std::memory_order_acquire) &&
                   target==state->target.load(std::memory_order_acquire) &&
                   PostMessageW(target,state->inputMessage,epoch,delta)) {
                    state->controlledWheel=true;
                    return 1;
                } // Suppress the same Ctrl+wheel gesture in the underlying app.
            }
            if(target && state->controlledWheel) {
                // A normal or differently modified wheel gesture ends a partial Ctrl sequence.
                PostMessageW(target,state->inputMessage,epoch,0);
                state->controlledWheel=false;
            }
        }
        return CallNextHookEx(nullptr,code,message,param);
    }
    static DWORD WINAPI Run(void* argument) noexcept {
        State* state=static_cast<State*>(argument);
        hookState_=state;
        HHOOK hook{};
        ULONG_PTR attempted{};
        MSG message{};
        // Ensure a message queue exists before acknowledging the first activation.
        PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
        while(!state->stopping.load(std::memory_order_acquire)) {
            const ULONG_PTR epoch=state->epoch.load(std::memory_order_acquire);
            const HWND target=state->target.load(std::memory_order_acquire);
            if(!target && hook) { UnhookWindowsHookEx(hook);hook=nullptr; }
            if(target && !hook && attempted!=epoch) {
                attempted=epoch;
#ifdef ZOOMIT_TESTING
                if(denyInstallation.load())SetLastError(ERROR_ACCESS_DENIED);
                else
#endif
                    hook=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
                if(!hook) {
                    const DWORD error=GetLastError();
                    PostMessageW(target,state->errorMessage,epoch,error ? error : ERROR_GEN_FAILURE);
                }
            }
            state->installed.store(hook!=nullptr,std::memory_order_release);
            state->acknowledged.store(epoch,std::memory_order_release);
            SetEvent(state->ready);
            const DWORD waited=MsgWaitForMultipleObjectsEx(1,&state->changed,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(waited==WAIT_FAILED) {
                if(target)PostMessageW(target,state->errorMessage,epoch,GetLastError());
                break;
            }
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_QUIT) {state->stopping.store(true);break;}
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        }
        if(hook)UnhookWindowsHookEx(hook);
        state->installed.store(false,std::memory_order_release);
        hookState_=nullptr;state->Release();
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
    DWORD SetTarget(HWND target,UINT inputMessage,UINT errorMessage) noexcept {
        if(shuttingDown_)return ERROR_SUCCESS;
        if(!target)failedTarget_=nullptr;
        if(target && target==failedTarget_)return ERROR_SUCCESS;
        if(state_ && target==state_->target.load(std::memory_order_acquire))return ERROR_SUCCESS;
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
            next->inputMessage=inputMessage;next->errorMessage=errorMessage;
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
            const ULONGLONG deadline=GetTickCount64()+400;
            while(state_->acknowledged.load(std::memory_order_acquire)!=epoch) {
                if(state_->epoch.load(std::memory_order_acquire)!=epoch)return ERROR_SUCCESS;
                const auto now=GetTickCount64();
                if(now>=deadline || WaitWithSentMessages(state_->ready,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0) {
                    failedTarget_=target;state_->target.store(nullptr);state_->epoch.store(NewEpoch());SetEvent(state_->changed);
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
        return state_ && !shuttingDown_ && epoch && state_->epoch.load(std::memory_order_acquire)==epoch &&
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
    HANDLE Thread() const noexcept { return state_ ? state_->thread : nullptr; }
#endif
};
}
