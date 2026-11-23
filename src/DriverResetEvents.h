#pragma once
#include <windows.h>
#include <winevt.h>
#include <atomic>

namespace zoomit::video {
inline constexpr UINT DriverReset = WM_APP + 70;

// Windows delivers future TDR notifications while this LiveZoom session is
// visible. The callback neither reads event data nor touches magnifier objects.
class DriverResetEvents {
    EVT_HANDLE subscription_{};
    ULONG_PTR session_{};
    inline static std::atomic<HWND> target_{};

    static DWORD WINAPI OnEvent(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID context, EVT_HANDLE) noexcept {
        if (action == EvtSubscribeActionDeliver) {
            if (HWND window = target_.load(std::memory_order_acquire))
                PostMessageW(window, DriverReset, reinterpret_cast<ULONG_PTR>(context), 0);
        }
        return ERROR_SUCCESS;
    }

  public:
    void Start(HWND window, ULONG_PTR session) noexcept {
        Stop();
        session_ = session;
        target_.store(window, std::memory_order_release);
        subscription_ =
            EvtSubscribe(nullptr, nullptr, L"System", L"*[System[Provider[@Name='Display'] and EventID=4101]]",
                         nullptr, reinterpret_cast<void *>(session), OnEvent, EvtSubscribeToFutureEvents);
        if (!subscription_)
            Stop(); // Display messages and graphics failures still trigger recovery.
    }
    bool Accept(ULONG_PTR session) const noexcept {
        return subscription_ && session == session_;
    }
    void Stop() noexcept {
        target_.store(nullptr, std::memory_order_release);
        if (subscription_)
            EvtClose(subscription_);
        subscription_ = nullptr;
        session_ = 0;
    }
};
} // namespace zoomit::video
