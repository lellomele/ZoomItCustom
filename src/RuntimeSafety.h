#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace zoomit::runtime {
enum class Api : unsigned { Cursor, Monitor, Graphics, Timer, Filter, Capture, Flush, Encoder, Post, Count };
#if defined(ZOOMIT_TESTING) || defined(ZOOMIT_RECOVERY_TESTING)
inline std::array<std::atomic<unsigned>, static_cast<unsigned>(Api::Count)> calls{}, failAt{};
inline std::array<std::atomic<bool>, static_cast<unsigned>(Api::Count)> failAlways{};
inline void Clear() noexcept {
    for (unsigned i=0;i<static_cast<unsigned>(Api::Count);++i) {
        calls[i].store(0); failAt[i].store(0); failAlways[i].store(false);
    }
}
inline void FailAt(Api api,unsigned occurrence) noexcept {
    const auto index=static_cast<unsigned>(api);
    calls[index].store(0);failAlways[index].store(false);failAt[index].store(occurrence);
}
inline void FailAlways(Api api,bool enabled=true) noexcept {
    const auto index=static_cast<unsigned>(api);calls[index].store(0);failAt[index].store(0);failAlways[index].store(enabled);
}
inline unsigned Calls(Api api) noexcept {return calls[static_cast<unsigned>(api)].load();}
inline bool Permit(Api api) noexcept {
    const auto index=static_cast<unsigned>(api);const unsigned count=++calls[index];
    if(failAlways[index].load() || (failAt[index].load() && failAt[index].load()==count)) {
        SetLastError(api==Api::Graphics ? ERROR_NOT_ENOUGH_MEMORY : ERROR_GEN_FAILURE);return false;
    }
    return true;
}
#else
inline constexpr bool Permit(Api) noexcept {return true;}
#endif
inline BOOL ReadCursor(POINT* point) noexcept {return Permit(Api::Cursor) && GetCursorPos(point);}
inline bool ValidRect(RECT rect) noexcept {
    const int64_t width=static_cast<int64_t>(rect.right)-rect.left;
    const int64_t height=static_cast<int64_t>(rect.bottom)-rect.top;
    return width>0 && height>0 && width<=32768 && height<=32768;
}
inline bool ValidViewport(RECT source,RECT monitor,float factor) noexcept {
    return ValidRect(source) && ValidRect(monitor) && factor>=1.0f && factor<=32.0f &&
        source.left>=monitor.left && source.top>=monitor.top && source.right<=monitor.right && source.bottom<=monitor.bottom;
}

// Fresh IDs prevent a queued WM_TIMER from targeting a later session or a rearmed role.
// Access is confined to the owning UI thread; no extra thread or polling is required.
inline UINT_PTR nextTimerId=0x4000;
template<size_t Count> class SessionTimers {
    struct Entry {HWND window{};UINT_PTR id{};};
    std::array<Entry,Count> entries_{};
public:
    SessionTimers()=default;
    SessionTimers(const SessionTimers&)=delete;
    SessionTimers& operator=(const SessionTimers&)=delete;
    ~SessionTimers(){StopAll();}
    UINT_PTR Id(unsigned role) const noexcept {return role<Count ? entries_[role].id : 0;}
    int Role(UINT_PTR id) const noexcept {
        if(!id)return -1;
        for(unsigned role=0;role<Count;++role)if(entries_[role].id==id)return static_cast<int>(role);
        return -1;
    }
    UINT_PTR Start(HWND window,unsigned role,UINT interval) noexcept {
        if(role>=Count || !window)return 0;
        if(++nextTimerId<0x4000)nextTimerId=0x4000;
        const UINT_PTR id=Permit(Api::Timer) ? SetTimer(window,nextTimerId,interval,nullptr) : 0;
        if(!id)return 0;
        Stop(role);entries_[role]={window,id};return id;
    }
    void Stop(unsigned role) noexcept {
        if(role>=Count)return;
        const auto old=entries_[role];entries_[role]={};
        if(old.id)KillTimer(old.window,old.id);
    }
    void StopAll() noexcept {for(unsigned role=0;role<Count;++role)Stop(role);}
};
}
