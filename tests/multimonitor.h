#pragma once
#include <map>
#include <tuple>

struct MultiMonitorResults {
    size_t monitors{}, activePaths{}, duplicatedSources{}, negativeOrigins{}, cases{};
    std::vector<RECT> bounds;
    std::vector<UINT> dpi;
};
MultiMonitorResults RunMultiMonitorRegression(HWND host) {
    MultiMonitorResults result;
    std::vector<MONITORINFOEXW> screens;
    require(EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        return [&]() noexcept -> BOOL {
            try {
                MONITORINFOEXW info{};info.cbSize=sizeof(info);
                require(GetMonitorInfoW(monitor,&info)!=FALSE,"Read the real test monitor");
                reinterpret_cast<std::vector<MONITORINFOEXW>*>(data)->push_back(info);return TRUE;
            } catch(...) {RecordTestCallbackFailure();return FALSE;}
        }();
    },reinterpret_cast<LPARAM>(&screens))!=FALSE,"Enumerate actual connected desktop monitors");
    result.monitors=screens.size();require(!screens.empty(),"At least one active desktop monitor is required");
    // Evidence only: this test never changes Windows display configuration.
    UINT32 pathCount{},modeCount{};
    for(unsigned attempt=0;attempt<3;++attempt) {
        const auto sizes=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount);
        if(sizes!=ERROR_SUCCESS)break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const auto read=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
        if(read==ERROR_INSUFFICIENT_BUFFER)continue;
        require(read==ERROR_SUCCESS,"Read the actual display topology for physical test evidence");
        result.activePaths=pathCount;
        std::map<std::tuple<LONG,DWORD,UINT32>,size_t> sources;
        for(UINT32 i=0;i<pathCount;++i) {
            const auto& source=paths[i].sourceInfo;
            ++sources[{source.adapterId.HighPart,source.adapterId.LowPart,source.id}];
        }
        for(const auto& entry:sources)if(entry.second>1)++result.duplicatedSources;
        break;
    }
    RECT oldHost{};require(GetWindowRect(host,&oldHost)!=FALSE,"Save the owned test host position");
    const auto oldAnimate=g_AnimateLiveZoom;
    const auto oldIndicator=g_ShowZoomIndicator;
    auto cleanup=zoomit::OnExit([&] {
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);
        MoveWindow(host,oldHost.left,oldHost.top,oldHost.right-oldHost.left,oldHost.bottom-oldHost.top,TRUE);
        g_AnimateLiveZoom=oldAnimate;g_ShowZoomIndicator=oldIndicator;
        if(g_TestSnipBitmap){DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;}
    });
    g_fullScreenWorkaround=false;g_AnimateZoom=FALSE;g_ShowZoomIndicator=FALSE;
    const auto mode=[] {return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
    for(const auto& screen:screens) {
        const RECT bounds=screen.rcMonitor;result.bounds.push_back(bounds);
        if(bounds.left<0||bounds.top<0)++result.negativeOrigins;
        require(zoomit::runtime::ValidRect(bounds),"Real monitor bounds must be valid");
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(40);
        require(MoveWindow(host,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,TRUE)!=FALSE,
                "Move only the process-owned test host to the next physical monitor");
        ActivateTestHost(host);RedrawWindow(host,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        result.dpi.push_back(GetDpiForWindow(host));
        for(bool animate:{false,true}) {
            g_AnimateLiveZoom=animate;
            auto position=[&] {
                ActivateTestHost(host);
                require(SetCursorPos(bounds.left+125,bounds.top+125)!=FALSE,"Place the owned test pointer on the current physical monitor");
                pump(30);
            };
            position();SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1'));pump(60);
            RECT canvasBounds{};require(GetWindowRect(g_hWndMain,&canvasBounds) && EqualRect(&canvasBounds,&bounds) && (mode()&3)==1,
                "Static Zoom must use the physical monitor beneath the pointer, including negative origins");
            cancelSnip=false;SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
            BITMAP snip{};require(g_TestSnipBitmap && GetObject(g_TestSnipBitmap,sizeof(snip),&snip) && snip.bmWidth==81 && snip.bmHeight==81,
                "Snip must use monitor-local coordinates on every connected monitor");
            DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,SHALLOW_DESTROY);pump(60);result.cases+=2;
            for(unsigned cycle=0;cycle<3;++cycle) {
                position();SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(180);
                LiveViewport view{};require(SendMessage(g_hWndLiveZoom,WM_USER_GET_LIVE_VIEWPORT,0,reinterpret_cast<LPARAM>(&view)) && EqualRect(&view.monitor,&bounds),
                    "LiveZoom must expose a coherent real monitor viewport");
                const float level=view.factor;
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(30);
                require((mode()&3)==3 && IsWindowVisible(g_hWndLiveZoom),"LiveDraw must remain live on every real monitor");
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
                SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(190,155));
                SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(190,155));
                const HDC canvas=reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));
                const COLORREF annotation=GetPixel(canvas,170,152);
                require(annotation!=CLR_INVALID,"Read the actual annotation on the selected monitor");
                SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
                require((mode()&23)==17 && GetCapture()==g_hWndMain,"Paused LiveDraw must retain its visible ring and capture on all monitors");
                for(WPARAM key:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(BREAK_HOTKEY)})SendMessage(g_hWndMain,WM_HOTKEY,key,MAKELPARAM(MOD_CONTROL,'1'));
                require((mode()&23)==17 && GetPixel(canvas,170,152)==annotation,"Ignored shortcuts must preserve paused annotations on real monitors");
                SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,0);pump(20);
                require((mode()&23)==17 && IsWindowVisible(g_hWndLiveZoom),"Unchanged physical topology notification must preserve paused drawing");
                SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(80);
                LiveViewport restored{};require(SendMessage(g_hWndLiveZoom,WM_USER_GET_LIVE_VIEWPORT,0,reinterpret_cast<LPARAM>(&restored)) && restored.factor==level,
                    "Leaving LiveDraw must retain its previous live factor on each monitor");
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(160);
                require(!IsWindow(g_hWndLiveZoom) && mode()==0,"Repeated LiveZoom exits must release the physical magnifier");
                const auto cursor=ResetThenReadOwnedNormalPointer(host,"Actual monitor LiveZoom exit");
                require((cursor.flags&CURSOR_SHOWING) && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),"Fixture-reset monitor exits must expose CURSOR_SHOWING with the logical arrow cursor");
                result.cases+=6;
            }
        }
    }
    return result;
}
void PrintMultiMonitorResults(const MultiMonitorResults& result) {
    std::cout<<"{\"passed\":true,\"cursor_check_scope\":\"logical-state-or-fixture-cleanup\",\"visual_cursor_verification\":false,\"physical_multimonitor\":true,\"logical_monitors\":"<<result.monitors
        <<",\"active_display_paths\":"<<result.activePaths<<",\"duplicated_sources\":"<<result.duplicatedSources
        <<",\"negative_origin_monitors\":"<<result.negativeOrigins<<",\"cases\":"<<result.cases<<",\"screens\":[";
    for(size_t i=0;i<result.bounds.size();++i) {
        if(i)std::cout<<",";const auto& rect=result.bounds[i];
        std::cout<<"{\"left\":"<<rect.left<<",\"top\":"<<rect.top<<",\"width\":"<<rect.right-rect.left
            <<",\"height\":"<<rect.bottom-rect.top<<",\"dpi\":"<<result.dpi[i]<<"}";
    }
    std::cout<<"]}\n";
}
