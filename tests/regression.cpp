#include "../src/Zoomit.cpp"
#include <iostream>
#include <psapi.h>
#include <chrono>
#include <stdexcept>
#include <array>
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
#include "utility_geometry.h"

void pump(DWORD milliseconds) {
    const auto end=GetTickCount64()+milliseconds;
    MSG msg{};
    do {
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message!=WM_QUIT) { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
        Sleep(1);
    } while(GetTickCount64()<end);
}
size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb=sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
    return counters.PrivateUsage;
}
DWORD fakeStickyFlags=SKF_HOTKEYACTIVE|SKF_CONFIRMHOTKEY|SKF_AUDIBLEFEEDBACK;
unsigned fakeStickyWrites=0;
bool fakeStickyFailure=false;
BOOL WINAPI FakeSticky(UINT action,UINT,void* data,UINT) {
    auto* settings=static_cast<STICKYKEYS*>(data);
    if(action==SPI_GETSTICKYKEYS) {settings->dwFlags=fakeStickyFlags; return TRUE;}
    if(fakeStickyFailure) return FALSE;
    ++fakeStickyWrites; fakeStickyFlags=settings->dwFlags; return TRUE;
}
EXECUTION_STATE fakeExecution=ES_CONTINUOUS;
unsigned fakeExecutionWrites=0;
EXECUTION_STATE WINAPI FakeExecution(EXECUTION_STATE state) {
    ++fakeExecutionWrites; const auto previous=fakeExecution; fakeExecution=state; return previous;
}
BOOL WINAPI MissingMonitor(HMONITOR,LPMONITORINFO) {return FALSE;}
HMONITOR WINAPI StaleMonitor(POINT,DWORD) {return reinterpret_cast<HMONITOR>(1);}
bool nestedSnipHotkeys=false;
bool nestedDisplayChange=false;

bool cancelSnip = false;
void CALLBACK SelectTestRegion(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND selection = FindWindow(L"ZoomitSelectRectangle",nullptr);
    DWORD owner{};
    if (!selection || !GetWindowThreadProcessId(selection,&owner) || owner!=GetCurrentProcessId()) return;
    KillTimer(nullptr,timer);
    if(nestedSnipHotkeys) {
        const HWND original=g_hWndLiveZoom;
        SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
        require(g_SelectionActive && g_hWndLiveZoom==original,"Nested selection hotkeys must be ignored");
        nestedSnipHotkeys=false;
    }
    if(nestedDisplayChange) {SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080)); nestedDisplayChange=false;}
    if (cancelSnip) SendMessage(selection,WM_KEYDOWN,VK_ESCAPE,0);
    else {
        SendMessage(selection,WM_LBUTTONDOWN,0,MAKELPARAM(20,20));
        SendMessage(selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(100,100));
        SendMessage(selection,WM_LBUTTONUP,0,MAKELPARAM(100,100));
    }
}

bool optionsValid = true;
bool aboutCaptured=false;
bool optionsDisplayChange=false;
bool optionsSaveHotkey=false;
const wchar_t* aboutCapturePath=nullptr;
const wchar_t* liveCapturePath=nullptr;
INT_PTR CALLBACK TestOptionsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == 97) {
        KillTimer(dialog, 97);
        if(!aboutCaptured && (aboutCapturePath || liveCapturePath)) {
            HWND tabs=GetDlgItem(dialog,IDC_TAB);
            auto capturePage=[&](int page,const wchar_t* path) {
                TabCtrl_SetCurSel(tabs,page);
                NMHDR notification{tabs,IDC_TAB,TCN_SELCHANGE};
                SendMessage(dialog,WM_NOTIFY,IDC_TAB,reinterpret_cast<LPARAM>(&notification));
                RedrawWindow(dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
                optionsValid &= IsWindowVisible(g_OptionsTabs[page].hPage) != FALSE;
                RECT window{};GetWindowRect(dialog,&window);
                HDC screen=GetDC(nullptr), memory=CreateCompatibleDC(screen);
                HBITMAP image=CreateCompatibleBitmap(screen,window.right-window.left,window.bottom-window.top);
                SelectObject(memory,image);
                PrintWindow(dialog,memory,PW_RENDERFULLCONTENT);
                const auto result=SavePng(const_cast<wchar_t*>(path),image);
                DeleteDC(memory);DeleteObject(image);ReleaseDC(nullptr,screen);
                optionsValid &= result==ERROR_SUCCESS;
            };
            if(liveCapturePath) capturePage(LIVE_PAGE,liveCapturePath);
            if(aboutCapturePath) capturePage(ABOUT_PAGE,aboutCapturePath);
            aboutCaptured=true;
        }
        if (optionsDisplayChange) {
            SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080)); pump(25);
            optionsValid &= IsWindowVisible(g_hWndMain) && g_DisplayWake.active();
            optionsDisplayChange=false;
        }
        if (optionsSaveHotkey) {
            SendMessage(GetDlgItem(g_OptionsTabs[LIVE_PAGE].hPage,IDC_LIVE_HOTKEY),HKM_SETHOTKEY,
                        ((HOTKEYF_CONTROL|HOTKEYF_ALT)<<8)|VK_F24,0);
            SendMessage(GetDlgItem(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ZOOM_SLIDER),TBM_SETPOS,TRUE,4);
            SendMessage(dialog,WM_COMMAND,IDOK,0); optionsSaveHotkey=false;
        } else SendMessage(dialog, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    auto result = OptionsProc(dialog, message, wParam, lParam);
    if (message == WM_INITDIALOG) {
        optionsValid &= TabCtrl_GetItemCount(GetDlgItem(dialog, IDC_TAB)) == 5;
        for (auto& page : g_OptionsTabs) optionsValid &= IsWindow(page.hPage) != FALSE;
        HWND zoomSlider=GetDlgItem(g_OptionsTabs[LIVE_PAGE].hPage,IDC_ZOOM_SLIDER);
        optionsValid &= IsWindow(zoomSlider) && SendMessage(zoomSlider,TBM_GETRANGEMAX,0,0)==5 &&
                        SendMessage(zoomSlider,TBM_GETPOS,0,0)==g_SliderZoomLevel;
        wchar_t firstTab[32]{}, lastTab[32]{};
        TCITEM tabItem{}; tabItem.mask=TCIF_TEXT; tabItem.cchTextMax=_countof(firstTab);
        tabItem.pszText=firstTab; TabCtrl_GetItem(GetDlgItem(dialog,IDC_TAB),0,&tabItem);
        tabItem.pszText=lastTab; TabCtrl_GetItem(GetDlgItem(dialog,IDC_TAB),4,&tabItem);
        optionsValid &= wcscmp(firstTab,L"LiveZoom")==0 && wcscmp(lastTab,L"About")==0;
        wchar_t copyright[256]{}, version[64]{};
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_COPYRIGHT,copyright,_countof(copyright));
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_VERSION,version,_countof(version));
        const char* expectedAsciiVersion=FILE_VERSION_STRING;
        const std::wstring expectedVersion(expectedAsciiVersion,expectedAsciiVersion+strlen(expectedAsciiVersion));
        optionsValid &= wcsstr(copyright,L"Prof. ing. Raffaele Mele")!=nullptr && wcsstr(version,expectedVersion.c_str())!=nullptr;
        SetTimer(dialog, 97, 35, nullptr);
    }
    return result;
}

int main(int argc, char** argv) {
    const bool snipOnly = argc>1 && strcmp(argv[1],"--snip-only")==0;
    std::wstring capturePath, livePath;
    if(argc>2) {capturePath=std::filesystem::absolute(argv[2]).wstring();aboutCapturePath=capturePath.c_str();}
    if(argc>3) {livePath=std::filesystem::absolute(argv[3]).wstring();liveCapturePath=livePath.c_str();}
    POINT oldCursor{}; GetCursorPos(&oldCursor);
    try {
        RunUtilityGeometryTests();
        {
            const DWORD original=fakeStickyFlags;
            zoomit::StickyKeysGuard sticky(FakeSticky);
            sticky.SuppressShortcut(); sticky.SuppressShortcut();
            require(fakeStickyWrites==1 && fakeStickyFlags==SKF_AUDIBLEFEEDBACK,"StickyKeys suppression must preserve flags and snapshot once");
            fakeStickyFailure=true; sticky.Restore(); fakeStickyFailure=false; sticky.Restore();
            require(fakeStickyFlags==original && fakeStickyWrites==2,"Failed StickyKeys restore must be retried");
            fakeStickyFlags=SKF_STICKYKEYSON|original; sticky.SuppressShortcut();
            require(fakeStickyFlags==(SKF_STICKYKEYSON|original),"Active accessibility settings must be preserved");
            zoomit::DisplayWakeGuard wake(FakeExecution);
            wake.SuppressSleep(); wake.SuppressSleep();
            require(fakeExecutionWrites==1 && (fakeExecution&ES_DISPLAY_REQUIRED),"Sleep suppression must be per session");
            wake.Restore(); require(fakeExecution==ES_CONTINUOUS && fakeExecutionWrites==2,"Power state must be restored without changing preferences");
            wchar_t error[32]{};
            FormatErrorText(error,_countof(error),nullptr,0xEFFFFFFF);
            require(wcsstr(error,L"Windows error")!=nullptr && error[31]==0,"Unknown error must format safely");
            FormatErrorText(error,_countof(error),std::wstring(4096,L'x').c_str(),ERROR_FILE_NOT_FOUND);
            require(error[31]==0,"Long error descriptions must be truncated safely");
            zoomit::DisplayTopology mirror{}; mirror.valid=true; mirror.count=2; mirror.desktop={0,0,1920,1080};
            mirror.paths[0].source=0; mirror.paths[0].target=0; mirror.paths[1].source=0; mirror.paths[1].target=1;
            require(mirror.mirrored(),"Duplicate outputs must share one logical display");
            auto extended=mirror; extended.paths[1].source=1;
            require(!extended.mirrored() && !(mirror==extended),"Extended and duplicated desktops must be distinguished");
        }
        require(g_LiveZoomToggleKey==((HOTKEYF_CONTROL<<8)|'2') &&
                g_DrawToggleKey==((HOTKEYF_CONTROL<<8)|'3') &&
                g_BreakToggleKey==((HOTKEYF_CONTROL<<8)|'4') &&
                g_SnipToggleKey==((HOTKEYF_CONTROL<<8)|'5'),"Default shortcuts must follow requested order");
        g_LiveZoomToggleKey='L'; require(MigrateHotkeys(),"Old shortcut schema must migrate");
        require(g_LiveZoomToggleKey==((HOTKEYF_CONTROL<<8)|'2'),"Migrated LiveZoom shortcut");
        g_LiveZoomToggleKey='L'; require(!MigrateHotkeys() && g_LiveZoomToggleKey=='L',"Later customized shortcuts must be preserved");
        require(DecodeZoomLevel(EncodeZoomLevel(1.25f))==1.25f,"Fractional zoom must survive messages");
        g_SliderZoomLevel=0xffffffff; g_RootPenWidth=0xffffffff;
        g_BreakTimeout=0xffffffff; g_BreakTimerPosition=0xffffffff;
        ValidateSettings();
        require(g_SliderZoomLevel==3 && g_RootPenWidth==19 &&
                g_BreakTimeout==99 && g_BreakTimerPosition==8,"Corrupt settings must be clamped");
        g_SliderZoomLevel=0; g_RootPenWidth=5; g_BreakTimeout=10;
        {
            const std::wstring key = L"Software\\ZoomItCustom\\Regression_" + std::to_wstring(GetCurrentProcessId());
            struct RegistryCleanup {
                std::wstring path;
                ~RegistryCleanup() { RegDeleteTree(HKEY_CURRENT_USER,path.c_str()); }
            } cleanup{key};
            ClassRegistry settings(key.c_str());
            DWORD wordArray[2]{0x10002,0xffffffff}, number=42;
            BOOLEAN flag=TRUE;
            wchar_t text[16]=L"hello";
            BYTE binary[4]{1,2,3,4};
            REG_SETTING table[]{
                {L"Number",SETTING_TYPE_DWORD,0,&number,42},
                {L"Flag",SETTING_TYPE_BOOLEAN,0,&flag,1},
                {L"Text",SETTING_TYPE_STRING,sizeof(text),text,0},
                {L"Binary",SETTING_TYPE_BINARY,sizeof(binary),binary,0},
                {L"Array",SETTING_TYPE_DWORD_ARRAY,sizeof(wordArray),wordArray,0},
                {} };
            settings.WriteRegSettings(table);
            number=0; flag=FALSE; text[0]=0; memset(binary,0,sizeof(binary)); memset(wordArray,0,sizeof(wordArray));
            settings.ReadRegSettings(table);
            require(number==42 && flag && wcscmp(text,L"hello")==0 && binary[3]==4 &&
                    wordArray[0]==0x10002 && wordArray[1]==0xffffffff,"Registry values must round-trip without truncation");
            HKEY handle{}; require(RegOpenKeyEx(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE|KEY_QUERY_VALUE,&handle)==ERROR_SUCCESS,"Test registry open");
            DWORD bytes{};
            RegQueryValueEx(handle,L"Text",nullptr,nullptr,nullptr,&bytes);
            require(bytes==6*sizeof(wchar_t),"Registry strings must include their terminator");
            RegSetValueEx(handle,L"Number",0,REG_SZ,reinterpret_cast<const BYTE*>(L"bad"),8);
            BYTE shortBinary=9; RegSetValueEx(handle,L"Binary",0,REG_BINARY,&shortBinary,1);
            RegCloseKey(handle);
            settings.ReadRegSettings(table);
            require(number==42 && binary[0]==1 && binary[3]==4,"Malformed registry data must preserve valid defaults");
        }
        P_DRAW_UNDO history=nullptr;
        DeleteOldestUndo(&history);
        require(GetOldestUndo(history)==nullptr,"Empty undo history");
        HDC screen=CreateDC(L"DISPLAY",nullptr,nullptr,nullptr);
        HDC dc=CreateCompatibleDC(screen);
        HBITMAP bitmap=CreateCompatibleBitmap(screen,1920,1080);
        SelectObject(dc,bitmap);
        const DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        for(int i=0;i<40;++i) {
            SetPixel(dc,0,0,RGB(i,0,0));
            PushDrawUndo(dc,&history,1920,1080);
        }
        size_t count=0;
        for(auto item=history;item;item=item->Next) ++count;
        require(count<=8,"Undo history must stay within 64 MiB at 1080p");
        SetPixel(dc,0,0,RGB(255,0,0));
        require(PopDrawUndo(dc,&history,1920,1080),"Undo available");
        require(GetPixel(dc,0,0)==RGB(39,0,0),"Undo must restore image content");
        DeleteDrawUndoList(&history);
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==gdiBefore,"Undo must release every GDI object");
        DeleteDC(dc); DeleteObject(bitmap); DeleteDC(screen);
        Gdiplus::GdiplusStartupInput gdiplusInput;
        ULONG_PTR gdiplusToken{};
        require(Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok, "GDI+ startup");
        size_t effectsBefore{}, effectsAfter{}, effectMemoryBefore{}, effectMemoryAfter{};
        double effectsMilliseconds{};
        {
            DrawingDib canvas(screen = CreateDC(L"DISPLAY", nullptr, nullptr, nullptr),
                              Gdiplus::Rect(0, 0, 256, 256));
            require(canvas.pixels()!=nullptr, "Drawing canvas");
            Gdiplus::SolidBrush brush(Gdiplus::Color(128, 255, 255, 0));
            Gdiplus::Pen pen(Gdiplus::Color(255,255,255,0), 6);
            g_PenWidth=6; g_PenColor=0x8000ffff;
            RECT fill{0,0,256,256}; FillRect(canvas.dc(), &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,70,70);
            require(GetPixel(canvas.dc(),40,40) == BlendColors(RGB(255,255,255),ColorFromColorRef(g_PenColor)),
                    "Highlighter must preserve intended color");
            DrawBlurredShape(DRAW_ELLIPSE, &pen, canvas.dc(), nullptr, 20,20,70,70);
            effectsBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            effectMemoryBefore=privateBytes();
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<400;++i) {
                DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,70,70);
                DrawBlurredShape(DRAW_ELLIPSE, &pen, canvas.dc(), nullptr, 20,20,70,70);
                DrawMaskedLine(canvas.dc(),canvas.dc(),Gdiplus::Rect(10,10,80,40),{15,15},{75,35},&pen,false);
            }
            effectsMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            effectMemoryAfter=privateBytes();
            require(effectMemoryAfter <= effectMemoryBefore + 2*1024*1024,"Drawing effects must not accumulate heap allocations");
            effectsAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            require(effectsAfter==effectsBefore, "Highlight and blur must release GDI resources");
            DrawHighlightedShape(DRAW_RECTANGLE, canvas.dc(), &brush, &pen, 20,20,20,20);
        }
        DeleteDC(screen);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        g_PenColor=COLOR_RED | 0xff000000; g_PenWidth=5;
        g_TestMode=true; g_ShowTrayIcon=false; g_OptionsShown=true;
        g_DrawToggleKey=g_LiveZoomToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
        pMagInitialize=MagInitialize;
        pMagSetWindowSource=MagSetWindowSource;
        pMagSetWindowTransform=MagSetWindowTransform;
        pMagSetWindowFilterList=MagSetWindowFilterList;
        pMagSetFullscreenTransform=MagSetFullscreenTransform;
        pMagSetInputTransform=MagSetInputTransform;
        pMagShowSystemCursor=MagShowSystemCursor;
        pSetLayeredWindowAttributes=SetLayeredWindowAttributes;
        pGetMonitorInfo=GetMonitorInfoA;
        pMonitorFromPoint=MonitorFromPoint;
        pDwmIsCompositionEnabled=reinterpret_cast<type_pDwmIsCompositionEnabled>(GetProcAddress(LoadLibrary(L"dwmapi.dll"), "DwmIsCompositionEnabled"));
        g_OsVersion=WIN10_VERSION;
        require(MagInitialize()!=FALSE,"Magnification API initialization");
        g_hWndMain=InitInstance(GetModuleHandle(nullptr),SW_HIDE);
        require(g_hWndMain!=nullptr,"Main window creation");
        g_SliderZoomLevel=0;
        WNDCLASS wc{}; wc.lpfnWndProc=DefWindowProc; wc.hInstance=GetModuleHandle(nullptr); wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.lpszClassName=L"ZoomItTestHost"; RegisterClass(&wc);
        HWND host=CreateWindowEx(0,L"ZoomItTestHost",L"ZoomIt regression tests",WS_OVERLAPPEDWINDOW,50,50,450,250,
                                 nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
        ShowWindow(host,SW_SHOW);
        SetForegroundWindow(host);
        SetCursorPos(125,125);
        {
            const auto errors=g_TestErrorCount;
            g_TestFailCaptureAllocation=true; SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0); pump(5);
            require(!IsWindowVisible(g_hWndMain),"Drawing must abort when capture allocation fails");
            g_TestFailCaptureAllocation=true; SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0); pump(5);
            require(!IsWindowVisible(g_hWndMain) && !g_SelectionActive,"Snip must not start after allocation failure");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(100);
            g_TestFailCaptureAllocation=true; SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0); pump(5);
            require(IsWindowVisible(g_hWndLiveZoom) && !(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),"Failed LiveDraw must restore existing LiveZoom");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(5);
            require(g_TestErrorCount==errors+3,"Allocation failures must be reported exactly once");
            const auto initialize=pMagInitialize; pMagInitialize=nullptr;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);
            require(!IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),"Missing Magnification must fail safely");
            pMagInitialize=initialize;
            const auto monitorApi=pMonitorFromPoint; const auto infoApi=pGetMonitorInfo;
            pMonitorFromPoint=StaleMonitor; pGetMonitorInfo=MissingMonitor;
            MONITORINFO fallback{}; require(UpdateMonitorInfo({0,0},&fallback) && fallback.cbSize==sizeof(fallback),"Disconnected monitor handles need a usable fallback");
            pMonitorFromPoint=monitorApi; pGetMonitorInfo=infoApi;
        }
        {
            const auto errors=g_TestErrorCount;
            g_TestFailCapture=true; SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0); pump(5);
            require(!IsWindowVisible(g_hWndMain),"Failed screen copy must abort Draw without leaving an overlay");
            SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3')); pump(30);
            const auto objects=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            g_TestFailCapture=true; SendMessage(g_hWndMain,WM_COMMAND,IDC_COPY,CAPTURE_KEEP_POINTER);
            require(!g_TestSnipBitmap && IsWindowVisible(g_hWndMain),"Failed Snip copy must not publish an uncaptured image");
            g_TestFailCapture=true; SendMessage(g_hWndMain,WM_COMMAND,IDC_SAVE,CAPTURE_KEEP_POINTER);
            require(!g_bSaveInProgress && GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==objects,"Failed save capture must release graphics resources");
            require(g_TestErrorCount==errors+3,"Copy failures must be reported once per operation");
            SendMessage(g_hWndMain,WM_USER_CAPTURE_SESSION,0,CAPTURE_CLOSE_NOW); pump(5);
        }
        require(RegisterHotKey(nullptr,0x6ee,MOD_CONTROL,'1')!=FALSE,
                "Ctrl+1 must be available because standalone Zoom no longer registers it");
        UnregisterHotKey(nullptr,0x6ee);
        auto modeState=[&]{return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
        auto canvasState=[&] {
            TestCanvasState state{};
            require(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,reinterpret_cast<LPARAM>(&state))==1 && state.canvas,
                    "An active Draw session must expose its canvas to regression tests");
            return state;
        };
        struct CanvasSnapshot {
            TestCanvasState state{};
            LRESULT mode{};
            HWND live{}, magnifier{};
            bool canvasVisible{}, liveVisible{}, layered{};
            std::array<BYTE,80*60*3> rgb{};
        };
        auto snapshotCanvas=[&] {
            CanvasSnapshot snapshot{};
            snapshot.state=canvasState(); snapshot.mode=modeState();
            snapshot.live=g_hWndLiveZoom; snapshot.magnifier=g_hWndLiveZoomMag;
            snapshot.canvasVisible=IsWindowVisible(g_hWndMain)!=FALSE;
            snapshot.liveVisible=IsWindowVisible(g_hWndLiveZoom)!=FALSE;
            snapshot.layered=(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED)!=0;
            DrawingDib region(snapshot.state.canvas,Gdiplus::Rect(120,120,80,60));
            require(region.pixels()!=nullptr,"Draw snapshot must capture real canvas pixels");
            for(size_t pixel=0;pixel<80*60;++pixel)
                for(size_t channel=0;channel<3;++channel)
                    snapshot.rgb[pixel*3+channel]=region.pixels()[pixel*4+channel];
            return snapshot;
        };
        size_t protectedLiveHotkeys=0, protectedDrawCases=0, liveEraseCases=0;
        size_t rightClickPauseCases=0, rightClickCloseCases=0, leftClickResumeCases=0, degenerateShapeCases=0;
        auto requireArrow=[&](const char* message) {
            CURSORINFO cursor{sizeof(cursor)};
            require(GetCursorInfo(&cursor)!=FALSE,"Inspect system pointer during pen suspension");
            require((cursor.flags&CURSOR_SHOWING) && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),message);
        };
        auto annotationSample=[&] {
            DrawingDib region(canvasState().canvas,Gdiplus::Rect(148,148,12,5));
            require(region.pixels()!=nullptr,"Erasure test must inspect real drawing pixels");
            std::array<BYTE,12*5*3> rgb{};
            for(size_t pixel=0;pixel<12*5;++pixel)
                for(size_t channel=0;channel<3;++channel)
                    rgb[pixel*3+channel]=region.pixels()[pixel*4+channel];
            return rgb;
        };
        auto sampleIsBlack=[&] {
            const auto rgb=annotationSample();
            return std::all_of(rgb.begin(),rgb.end(),[](BYTE color){return color==0;});
        };
        auto eraseLiveDrawing=[&](bool penActive) {
            require(!sampleIsBlack(),"Erasure test must begin with visible annotations in the sampled region");
            SendMessage(g_hWndMain,WM_KEYDOWN,'E',0);
            const auto state=canvasState();
            require(sampleIsBlack() && state.undoCount==0 && !(modeState()&4),
                    "E must erase LiveDraw pixels to transparent black and release undo history");
            require(((modeState()&2)!=0)==penActive && IsWindowVisible(g_hWndLiveZoom) &&
                    (GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),
                    "Erasure must preserve LiveZoom and whether the pen is active or suspended");
            ++liveEraseCases;
        };
        auto protectDrawing=[&] {
            const auto before=snapshotCanvas();
            for(int repeat=0;repeat<3;++repeat) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
                ++protectedLiveHotkeys;
                const auto after=snapshotCanvas();
                require(after.state.canvas==before.state.canvas && after.state.undoCount==before.state.undoCount &&
                        after.state.haveDrawn==before.state.haveDrawn && after.state.zoomLevel==before.state.zoomLevel &&
                        EqualRect(&after.state.sourceView,&before.state.sourceView) && after.mode==before.mode &&
                        after.live==before.live && after.magnifier==before.magnifier &&
                        after.canvasVisible==before.canvasVisible && after.liveVisible==before.liveVisible &&
                        after.layered==before.layered && after.rgb==before.rgb,
                        "Ctrl+2 must preserve drawing pixels, undo history, mode, view and windows");
            }
            ++protectedDrawCases;
        };
        for(int scenario=0;scenario<3;++scenario) {
            const bool live=scenario!=0;
            g_fullScreenWorkaround=scenario==2;
            SetForegroundWindow(host); SetCursorPos(125,125);
            if(live) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2')); pump(100);
                require(IsWindowVisible(g_hWndLiveZoom),"LiveZoom must start before Ctrl+3");
            }
            SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3')); pump(25);
            require((modeState()&3)==3 && !(modeState()&4),"Ctrl+3 must activate the pen without starting a stroke");
            require(IsWindowVisible(g_hWndMain) && (IsWindowVisible(g_hWndLiveZoom)!=FALSE)==live,
                    "Ctrl+3 must keep LiveZoom running when drawing over a live image");
            require(((GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED)!=0)==live,
                    "Drawing over LiveZoom must use a transparent live overlay");
            CURSORINFO drawingCursor{sizeof(drawingCursor)};
            require(GetCursorInfo(&drawingCursor) && !(drawingCursor.flags&CURSOR_SHOWING),
                    "Draw must hide the arrow immediately so the pen remains the only pointer");
            protectDrawing();
            const auto initial=canvasState();
            SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
            SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
            const auto wheel=canvasState();
            require(initial.zoomLevel==1.0f && wheel.zoomLevel==1.0f && EqualRect(&initial.sourceView,&wheel.sourceView),
                    "Draw canvas must not inherit static Zoom magnification or panning");
            SetCursorPos(150,150); pump(5);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            require((modeState()&7)==7 && canvasState().haveDrawn,"Test stroke must be active and stored in the canvas");
            protectDrawing();
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            require((modeState()&7)==3 && canvasState().undoCount>initial.undoCount,
                    "Completed stroke must preserve the pen and an undo entry");
            protectDrawing();
            if(live) {
                eraseLiveDrawing(true);
                protectDrawing();
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
                SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
                SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            }
            SetCursorPos(175,150); pump(5);
            const auto completedAnnotations=annotationSample();
            const auto completedState=canvasState();
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(175,150));
            requireArrow("The first right-click must immediately restore a visible arrow while retaining Draw");
            require((modeState()&7)==1 && canvasState().haveDrawn,"Right-click must suspend the pen while keeping the drawing");
            require(annotationSample()==completedAnnotations && canvasState().undoCount==completedState.undoCount,
                    "Suspending a completed drawing must preserve its annotation pixels and undo history");
            const auto suspended=snapshotCanvas();
            SendMessage(g_hWndMain,WM_SETCURSOR,reinterpret_cast<WPARAM>(g_hWndMain),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,0,MAKELPARAM(180,155)); pump(15);
            requireArrow("The arrow must remain visible after cursor refresh and mouse movement with the pen suspended");
            const auto afterMovement=snapshotCanvas();
            require(afterMovement.state.canvas==suspended.state.canvas && afterMovement.state.undoCount==suspended.state.undoCount &&
                    afterMovement.state.haveDrawn==suspended.state.haveDrawn && afterMovement.mode==suspended.mode &&
                    afterMovement.rgb==suspended.rgb && afterMovement.live==suspended.live,
                    "Mouse movement with a suspended pen must not erase annotations, restart tracing or replace the canvas");
            ++rightClickPauseCases;
            protectDrawing();
            if(live) eraseLiveDrawing(false);
            SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3'));
            require((modeState()&7)==3 && canvasState().haveDrawn,
                    "Ctrl+3 must reactivate the suspended pen without starting a stroke or erasing the canvas");
            CURSORINFO reactivatedCursor{sizeof(reactivatedCursor)};
            require(GetCursorInfo(&reactivatedCursor) && !(reactivatedCursor.flags&CURSOR_SHOWING),
                    "Reactivating a suspended pen must hide the arrow immediately");
            require((IsWindowVisible(g_hWndLiveZoom)!=FALSE)==live &&
                    ((GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED)!=0)==live,
                    "Reactivating the pen must preserve the desktop or LiveDraw session");
            if(live) {
                require(sampleIsBlack(),"Reactivating an erased LiveDraw canvas must retain its transparent background");
                if(!g_fullScreenWorkaround)
                    require(!(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR),
                            "Reactivating LiveDraw must hide the magnified pointer while the pen is visible");
            }
            protectDrawing();
            SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0); pump(30);
            require(!IsWindowVisible(g_hWndMain),"Escape must remain available to close a protected drawing");
            if(live) {
                require(IsWindowVisible(g_hWndLiveZoom),"Leaving LiveDraw must preserve its running LiveZoom");
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2')); pump(20);
                require(!IsWindow(g_hWndLiveZoom),"Ctrl+2 must close LiveZoom after the drawing has explicitly ended");
            }
        }
        for(int scenario=0;scenario<3;++scenario) {
            const bool live=scenario!=0;
            g_fullScreenWorkaround=scenario==2;
            SetForegroundWindow(host); SetCursorPos(125,125);
            if(live) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2')); pump(100);
            }
            SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3')); pump(25);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            SetCursorPos(175,150); pump(5);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
            require((modeState()&7)==7,"Right-click regression must begin while a stroke is active");
            const auto activeAnnotations=annotationSample();
            const auto activeState=canvasState();
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(175,150));
            requireArrow("Right-click during a stroke must immediately restore a visible arrow");
            require((modeState()&7)==1 && canvasState().haveDrawn &&
                    canvasState().undoCount==activeState.undoCount && annotationSample()==activeAnnotations,
                    "Right-click during a stroke must suspend tracing while retaining annotations and undo history");
            SendMessage(g_hWndMain,WM_SETCURSOR,reinterpret_cast<WPARAM>(g_hWndMain),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,0,MAKELPARAM(180,155)); pump(15);
            requireArrow("The arrow must survive cursor refresh after interrupting an active stroke");
            require((modeState()&7)==1 && canvasState().undoCount==activeState.undoCount &&
                    annotationSample()==activeAnnotations,"Suspended strokes must remain unchanged after mouse movement");
            ++rightClickPauseCases;
            for(bool releaseBeforeRight : {false,true}) {
                const auto pausedAnnotations=annotationSample();
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
                require((modeState()&7)==3 && canvasState().haveDrawn && annotationSample()==pausedAnnotations,
                        "Left-click must reactivate a suspended pen without starting a stroke or erasing annotations");
                CURSORINFO resumedCursor{sizeof(resumedCursor)};
                require(GetCursorInfo(&resumedCursor) && !(resumedCursor.flags&CURSOR_SHOWING),
                        "Left-click must hide the arrow immediately when the pen is reactivated");
                if(live && !g_fullScreenWorkaround)
                    require(!(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR),
                            "Left-click pen reactivation must hide the magnified pointer");
                ++leftClickResumeCases;
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,MK_CONTROL,MAKELPARAM(160,150));
                require((modeState()&7)==7,"Degenerate rectangle test must begin tracing without mouse movement");
                if(releaseBeforeRight) {
                    SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(160,150));
                    require((modeState()&7)==3,"A rectangle with no movement must finish without retaining tracing state");
                }
                const auto beforeShapeRight=snapshotCanvas();
                SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(160,150));
                const auto afterShapeRight=snapshotCanvas();
                require((modeState()&7)==1 && afterShapeRight.state.haveDrawn &&
                        afterShapeRight.state.undoCount==beforeShapeRight.state.undoCount &&
                        afterShapeRight.state.canvas==beforeShapeRight.state.canvas &&
                        afterShapeRight.rgb==beforeShapeRight.rgb,
                        "Right-click after a shape with no movement must preserve every annotation pixel and undo entry");
                requireArrow("Suspending a shape with no movement must restore the visible arrow");
                ++rightClickPauseCases; ++degenerateShapeCases;
            }
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(175,150)); pump(30);
            require(modeState()==0 && !IsWindowVisible(g_hWndMain),"The second right-click must close the drawing canvas");
            require((IsWindowVisible(g_hWndLiveZoom)!=FALSE)==live,
                    "Closing Draw with the second right-click must preserve LiveZoom when it was running");
            requireArrow("The second right-click must leave a visible arrow on the desktop or LiveZoom");
            ++rightClickCloseCases;
            if(live) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2')); pump(20);
                require(!IsWindow(g_hWndLiveZoom),"LiveZoom must close normally after Draw has ended");
                requireArrow("Closing LiveZoom after two right-clicks must retain the system arrow");
            }
        }
        g_fullScreenWorkaround=false;
        SetForegroundWindow(host);
        size_t drawCycleCount=0;
        auto runCycle=[&](bool fullscreen, bool liveDraw=false, bool exitWithLiveHotkey=false) {
            ++drawCycleCount;
            g_fullScreenWorkaround=fullscreen;
            SetCursorPos(125,125);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(100);
            require(IsWindowVisible(g_hWndLiveZoom),"LiveZoom must start");
            SendMessage(g_hWndMain,WM_HOTKEY,liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0); pump(30);
            require((modeState()&3)==3 && IsWindowVisible(g_hWndLiveZoom) &&
                    (GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),
                    "Both Draw shortcuts must use LiveDraw while LiveZoom is active");
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            for(int x=151;x<175;++x) SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_KEYDOWN,'T',0);
            for(wchar_t letter : std::wstring(L"Test")) SendMessage(g_hWndMain,WM_CHAR,letter,0);
            SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
            if(exitWithLiveHotkey) {
                const auto before=modeState();
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2'));
                require(modeState()==before && IsWindowVisible(g_hWndLiveZoom) && IsWindowVisible(g_hWndMain),
                        "LiveZoom hotkey must preserve an active LiveDraw stroke");
                ++protectedLiveHotkeys;
            } else if(liveDraw) {
                SendMessage(g_hWndMain,WM_USER_EXIT_MODE,0,0); pump(15);
                if(!fullscreen) require(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR,
                                        "Suspending LiveDraw must restore the magnified pointer");
            }
            SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0); pump(100);
            require(!IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom),"Exiting LiveDraw must leave LiveZoom running");
            float level=*reinterpret_cast<float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
            require(level>1.0f,"LiveZoom must retain fractional magnification after LiveDraw");
            if(!fullscreen)
                require(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR,"Magnified cursor must be restored");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,MAKELPARAM(MOD_CONTROL,'2')); pump(100);
            require(!IsWindow(g_hWndLiveZoom),"LiveZoom must close after Draw has ended");
            CURSORINFO cursor{sizeof(cursor)};
            require(GetCursorInfo(&cursor)!=FALSE,"Inspect system pointer after LiveDraw");
            require((cursor.flags&CURSOR_SHOWING)!=0 && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),
                    "System arrow must be visible after exiting LiveZoom");
            SetForegroundWindow(host);
        };
        size_t liveToggleCount = 0;
        auto pointerVisible = [] {
            CURSORINFO cursor{sizeof(cursor)};
            require(GetCursorInfo(&cursor)!=FALSE,"Inspect system pointer");
            require((cursor.flags & CURSOR_SHOWING) && cursor.hCursor,
                    "Every LiveZoom exit must restore a real visible pointer");
            require(cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),"Exit must restore the system arrow");
        };
        for (int delay : {0,1,3,7,12,100,300}) {
            if(snipOnly) break;
            g_fullScreenWorkaround=false;
            for (int i=0;i<40;++i) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
                ++liveToggleCount;
                require(IsWindowVisible(g_hWndLiveZoom)==((i%2)==0),"Each activation must toggle LiveZoom exactly once");
                if(i%2) pointerVisible();
                if(delay) pump(delay);
                if(i%2) pointerVisible();
            }
            require(!IsWindow(g_hWndLiveZoom),"Every complete sequence must close without forced cleanup");
            pointerVisible();
            SetForegroundWindow(host);
        }
        {
            // Exercise the real Snip selection and capture without replacing the user's clipboard.
            ComputerGraphicsInit graphics;
            wchar_t missing[]=L"Z:\\ZoomIt_missing_file_12345678.png";
            require(!LoadImageFile(missing),"Missing background image must fail cleanly");
            require(SavePng(missing,nullptr)!=ERROR_SUCCESS,"PNG failure must be reported");
            SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3')); pump(100);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(175,150));
            const auto snipCanvas=snapshotCanvas();
            auto snipGdi = GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            for(int i=0;i<6;++i) {
                cancelSnip=false;
                nestedSnipHotkeys=true;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                BITMAP cropped{};
                require(g_TestSnipBitmap && GetObject(g_TestSnipBitmap,sizeof(cropped),&cropped),"Snip must capture inside active Draw");
                require(cropped.bmWidth==81 && cropped.bmHeight==81,"Snip crop dimensions");
                DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
                require(IsWindowVisible(g_hWndMain),"Snip must preserve the existing drawing canvas");
                cancelSnip=true;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                require(!g_TestSnipBitmap && IsWindowVisible(g_hWndMain),"Cancelled Snip must preserve the existing drawing canvas");
                cancelSnip=false;
                wchar_t temporary[MAX_PATH]; GetTempPath(MAX_PATH,temporary);
                const std::wstring imagePath=std::wstring(temporary)+L"ZoomItRegression_"+std::to_wstring(GetCurrentProcessId())+L".png";
                struct ImageCleanup {const wchar_t* path; ~ImageCleanup(){DeleteFile(path);}} imageCleanup{imagePath.c_str()};
                g_TestSavePath=imagePath.c_str(); g_TestSaveFilter= i%2 ? 2 : 1;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_SAVE_HOTKEY,0);
                {
                    Gdiplus::Bitmap image(imagePath.c_str());
                    require(image.GetLastStatus()==Gdiplus::Ok,"Snip PNG must be valid");
                    require(image.GetWidth()==81 && image.GetHeight()==81,"PNG must have requested scale");
                }
                DeleteFile(imagePath.c_str()); g_TestSavePath=nullptr;
                if(i==0) snipGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS); // Warm selection and image codecs once.
                require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==snipGdi,"Snip copy, save and cancellation must not leak GDI objects");
                const auto afterSnip=snapshotCanvas();
                require(afterSnip.state.canvas==snipCanvas.state.canvas && afterSnip.state.undoCount==snipCanvas.state.undoCount &&
                        afterSnip.state.haveDrawn && afterSnip.rgb==snipCanvas.rgb,
                        "Snip copy, save and cancellation must retain drawing pixels and undo history");
            }
            SendMessage(g_hWndMain,WM_USER_CAPTURE_SESSION,0,CAPTURE_CLOSE_NOW); pump(30);
            pointerVisible();
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(100);
            cancelSnip=false; SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0); pump(100);
            require(g_TestSnipBitmap && IsWindowVisible(g_hWndLiveZoom),"Snip from LiveZoom must resume LiveZoom");
            DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
            for(int wait=0;wait<50 && *reinterpret_cast<float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0))!=1.25f;++wait) pump(10);
            wchar_t temporary[MAX_PATH]; GetTempPath(MAX_PATH,temporary);
            const std::wstring imagePath=std::wstring(temporary)+L"ZoomItRegressionLive_"+std::to_wstring(GetCurrentProcessId())+L".png";
            struct LiveImageCleanup {const wchar_t* path; ~LiveImageCleanup(){DeleteFile(path);}} imageCleanup{imagePath.c_str()};
            g_TestSavePath=imagePath.c_str(); g_TestSaveFilter=2;
            SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_SAVE_HOTKEY,0); pump(100);
            {
                Gdiplus::Bitmap image(imagePath.c_str());
                require(image.GetLastStatus()==Gdiplus::Ok && image.GetWidth()==64 && image.GetHeight()==64,
                        "Snip from 1.25x LiveZoom must save Actual size at source resolution");
            }
            DeleteFile(imagePath.c_str()); g_TestSavePath=nullptr;
            require(IsWindowVisible(g_hWndLiveZoom),"Saving Snip from LiveZoom must resume its live view");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(30);
            pointerVisible();
        }
        size_t displayChangeCases=0;
        for(int mode=0;mode<4;++mode) {
            if(mode==0) SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3'));
            else if(mode==1) SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            else if(mode==2) {SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);}
            else SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0);
            pump(100);
            SendMessage(g_hWndMain,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080)); pump(120);
            require(IsWindow(g_hWndMain) && !IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom),"Display changes must stop capture while keeping the app alive");
            pointerVisible(); ++displayChangeCases;
            SendMessage(g_hWndMain,WM_USER_END_SESSION,0,0); pump(5);
            require(!IsWindowVisible(g_hWndMain),"Stale exit messages must not reactivate a drawing canvas");
            SetForegroundWindow(host);
        }
        SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,MAKELPARAM(MOD_CONTROL,'3')); pump(100);
        nestedDisplayChange=true; cancelSnip=false; SetTimer(nullptr,0,15,SelectTestRegion);
        SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0); pump(150);
        if(g_TestSnipBitmap){DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;}
        require(IsWindow(g_hWndMain) && !IsWindowVisible(g_hWndMain),"Display reset must defer safely until Snip finishes");
        pointerVisible(); ++displayChangeCases;
        g_BreakOnSecondary=false; g_BreakShowBackgroundFile=false;
        SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(30);
        require(g_DisplayWake.active() && SuppressIdleCommand(SC_SCREENSAVE) &&
            SuppressIdleCommand(SC_MONITORPOWER) && !SuppressIdleCommand(SC_CLOSE),"Timer must suppress idle display commands without changing preferences");
        optionsDisplayChange=true;
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc); pump(150);
        require(!IsWindowVisible(g_hWndMain) && !g_DisplayWake.active(),"Display changes during Options must release the timer and its wake lock after closing Options");
        pointerVisible(); ++displayChangeCases;
        optionsSaveHotkey=true;
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
        require(g_LiveZoomToggleMod==(MOD_CONTROL|MOD_ALT),"Saving LiveZoom options must update both key and modifiers");
        require(g_SliderZoomLevel==4,"Saving options must read initial magnification from the LiveZoom page");
        g_SliderZoomLevel=0;
        UnregisterAllHotkeys(g_hWndMain);g_LiveZoomToggleKey=0;
        const auto topology=zoomit::ReadDisplayTopology();
        if(!snipOnly) {runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);}
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
        g_BreakOnSecondary=false; g_BreakShowBackgroundFile=false;
        SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
        SendMessage(g_hWndMain,WM_USER_END_SESSION,0,0); pump(10);
        const auto gdiBaseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userBaseline=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryBefore=privateBytes();
        for(int i=0;i<(snipOnly ? 0 : 12);++i) {
            runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);
            DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
            SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
            SendMessage(g_hWndMain,WM_USER_END_SESSION,0,0); pump(10);
        }
        require(optionsValid,"Options must have five pages with About last and no standalone Zoom, recording or typing pages");
        const auto gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryAfter=privateBytes();
        require(gdiAfter<=gdiBaseline+1,"Repeated transitions must not leak GDI resources");
        require(userAfter<=userBaseline+1,"Repeated transitions must not leak USER resources");
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(70);
        DestroyWindow(g_hWndMain);
        require(!IsWindow(g_hWndLiveZoom),"Quitting must destroy the magnifier");
        CURSORINFO cursor{sizeof(cursor)};if (!GetCursorInfo(&cursor)) { std::cerr << "GetCursorInfo error=" << GetLastError() << "\n"; throw std::runtime_error("Cannot inspect system pointer"); }
        require((cursor.flags&CURSOR_SHOWING)!=0,"Quitting must restore the system pointer");
        DestroyWindow(host);
        MagUninitialize();
        SetCursorPos(oldCursor.x,oldCursor.y);
        std::cout<<"{\"passed\":true,\"live_draw_cycles\":"<<drawCycleCount<<",\"undo_1080p_entries\":"<<count
          <<",\"protected_draw_cases\":"<<protectedDrawCases<<",\"protected_live_hotkeys\":"<<protectedLiveHotkeys
          <<",\"right_click_pause_cases\":"<<rightClickPauseCases<<",\"right_click_close_cases\":"<<rightClickCloseCases
          <<",\"left_click_resume_cases\":"<<leftClickResumeCases<<",\"degenerate_shape_cases\":"<<degenerateShapeCases
          <<",\"live_erase_cases\":"<<liveEraseCases<<",\"live_toggle_events\":"<<liveToggleCount<<",\"effect_operations\":1200,\"effects_milliseconds\":"<<effectsMilliseconds
          <<",\"effect_gdi_before\":"<<effectsBefore<<",\"effect_gdi_after\":"<<effectsAfter
          <<",\"effect_private_before\":"<<effectMemoryBefore<<",\"effect_private_after\":"<<effectMemoryAfter
          <<",\"display_change_cases\":"<<displayChangeCases<<",\"active_display_paths\":"<<topology.count
          <<",\"mirrored_desktop\":"<<(topology.mirrored()?"true":"false")<<",\"allocation_failure_cases\":3,\"capture_failure_cases\":3,\"snip_cases\":21,\"options_open_close_cycles\":"<<(snipOnly?3:15)<<",\"timer_cycles\":"<<(snipOnly?2:14)
          <<",\"gdi_before\":"<<gdiBaseline<<",\"gdi_after\":"<<gdiAfter
          <<",\"user_before\":"<<userBaseline<<",\"user_after\":"<<userAfter
          <<",\"private_bytes_before\":"<<memoryBefore<<",\"private_bytes_after\":"<<memoryAfter<<"}\n";
        return 0;
    } catch(const std::exception& ex) {
        if(IsWindow(g_hWndMain)) DestroyWindow(g_hWndMain);
        if(IsWindow(g_hWndLiveZoom)) DestroyWindow(g_hWndLiveZoom);
        MagSetFullscreenTransform(1,0,0);
        MagShowSystemCursor(TRUE);
        MagUninitialize();
        ClipCursor(nullptr);
        SetCursorPos(oldCursor.x,oldCursor.y);
        std::cerr<<"FAILED: "<<ex.what()<<"\n";
        return 1;
    }
}





