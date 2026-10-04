#pragma once
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdint>

namespace zoomit {
struct DisplayTopology {
    struct Path {
        LUID sourceAdapter{}, targetAdapter{};
        UINT32 source{}, target{}, rotation{}, scaling{};
    };
    std::array<Path, 64> paths{};
    size_t count{};
    RECT desktop{};
    bool valid{};
    bool mirrored() const noexcept {
        for (size_t i=0;i<count;++i) for(size_t j=0;j<i;++j)
            if (paths[i].source==paths[j].source &&
                paths[i].sourceAdapter.HighPart==paths[j].sourceAdapter.HighPart &&
                paths[i].sourceAdapter.LowPart==paths[j].sourceAdapter.LowPart) return true;
        return false;
    }
    bool operator==(const DisplayTopology& other) const noexcept {
        if (!valid || !other.valid || count!=other.count || !EqualRect(&desktop,&other.desktop)) return false;
        for(size_t i=0;i<count;++i) {
            const auto& a=paths[i]; const auto& b=other.paths[i];
            if(a.sourceAdapter.HighPart!=b.sourceAdapter.HighPart || a.sourceAdapter.LowPart!=b.sourceAdapter.LowPart ||
               a.targetAdapter.HighPart!=b.targetAdapter.HighPart || a.targetAdapter.LowPart!=b.targetAdapter.LowPart ||
               a.source!=b.source || a.target!=b.target || a.rotation!=b.rotation || a.scaling!=b.scaling) return false;
        }
        return true;
    }
};
inline DisplayTopology ReadDisplayTopology() noexcept {
    DisplayTopology result{};
    result.desktop={GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
    result.desktop.right=result.desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);
    result.desktop.bottom=result.desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    UINT32 pathCount{}, modeCount{};
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount)!=ERROR_SUCCESS ||
        pathCount>result.paths.size() || modeCount>256) return result;
    std::array<DISPLAYCONFIG_PATH_INFO,64> paths{};
    std::array<DISPLAYCONFIG_MODE_INFO,256> modes{};
    pathCount=static_cast<UINT32>(paths.size()); modeCount=static_cast<UINT32>(modes.size());
    if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr)!=ERROR_SUCCESS)
        return result;
    result.count=pathCount;
    for(size_t i=0;i<result.count;++i) {
        const auto& p=paths[i];
        result.paths[i]={p.sourceInfo.adapterId,p.targetInfo.adapterId,p.sourceInfo.id,p.targetInfo.id,
                         static_cast<UINT32>(p.targetInfo.rotation),static_cast<UINT32>(p.targetInfo.scaling)};
    }
    std::sort(result.paths.begin(), result.paths.begin()+result.count,[](const auto& a,const auto& b) {
        if(a.sourceAdapter.HighPart!=b.sourceAdapter.HighPart) return a.sourceAdapter.HighPart<b.sourceAdapter.HighPart;
        if(a.sourceAdapter.LowPart!=b.sourceAdapter.LowPart) return a.sourceAdapter.LowPart<b.sourceAdapter.LowPart;
        if(a.source!=b.source) return a.source<b.source;
        if(a.targetAdapter.HighPart!=b.targetAdapter.HighPart) return a.targetAdapter.HighPart<b.targetAdapter.HighPart;
        if(a.targetAdapter.LowPart!=b.targetAdapter.LowPart) return a.targetAdapter.LowPart<b.targetAdapter.LowPart;
        return a.target<b.target;
    });
    result.valid=result.count>0 && result.desktop.right>result.desktop.left && result.desktop.bottom>result.desktop.top;
    return result;
}
inline bool FindSecondaryMonitor(RECT& rect) noexcept {
    struct Selection {RECT rect{}; bool found{};} selection;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM context)->BOOL {
        auto& found=*reinterpret_cast<Selection*>(context);
        MONITORINFO info{sizeof(info)};
        if(GetMonitorInfoW(monitor,&info) && !(info.dwFlags&MONITORINFOF_PRIMARY)) {
            found.rect=info.rcMonitor; found.found=true; return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&selection));
    if(selection.found) rect=selection.rect;
    return selection.found;
}
} // namespace zoomit
