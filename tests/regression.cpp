#include "../src/Zoomit.cpp"
#include <iostream>
#include <psapi.h>
#include <chrono>
#include <stdexcept>
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
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
bool systemCursorShown = true;
bool denyInputTransform = false;
// The privileged fullscreen input transform is simulated for state-machine tests.
// Native window magnification uses the real APIs; a separate case exercises access denial.
BOOL WINAPI TestInputTransform(BOOL enabled, RECT*, RECT*) {
    if (enabled && denyInputTransform) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
BOOL WINAPI TestShowSystemCursor(BOOL show) {
    const BOOL result=MagShowSystemCursor(show);
    if(result) systemCursorShown=show!=FALSE;
    return result;
}
bool cancelSnip = false;
void CALLBACK SelectTestRegion(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND selection = FindWindow(L"ZoomitSelectRectangle",nullptr);
    DWORD owner{};
    if (!selection || !GetWindowThreadProcessId(selection,&owner) || owner!=GetCurrentProcessId()) return;
    KillTimer(nullptr,timer);
    if (cancelSnip) SendMessage(selection,WM_KEYDOWN,VK_ESCAPE,0);
    else {
        SendMessage(selection,WM_LBUTTONDOWN,0,MAKELPARAM(20,20));
        SendMessage(selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(100,100));
        SendMessage(selection,WM_LBUTTONUP,0,MAKELPARAM(100,100));
    }
}

bool optionsValid = true;
bool aboutCaptured=false;
const wchar_t* aboutCapturePath=nullptr;
INT_PTR CALLBACK TestOptionsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == 97) {
        KillTimer(dialog, 97);
        HWND tabs=GetDlgItem(dialog,IDC_TAB);
        TabCtrl_SetCurSel(tabs,ABOUT_PAGE);
        NMHDR notification{tabs,IDC_TAB,TCN_SELCHANGE};
        SendMessage(dialog,WM_NOTIFY,IDC_TAB,reinterpret_cast<LPARAM>(&notification));
        optionsValid &= IsWindowVisible(g_OptionsTabs[ABOUT_PAGE].hPage)!=FALSE;
        if (!aboutCaptured && aboutCapturePath) {
            RedrawWindow(dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            RECT window{};GetWindowRect(dialog,&window);
            HDC screen=GetDC(nullptr), memory=CreateCompatibleDC(screen);
            HBITMAP image=CreateCompatibleBitmap(screen,window.right-window.left,window.bottom-window.top);
            SelectObject(memory,image);
            PrintWindow(dialog,memory,PW_RENDERFULLCONTENT);
            require(SavePng(const_cast<wchar_t*>(aboutCapturePath),image)==ERROR_SUCCESS,"Save About verification image");
            DeleteDC(memory);DeleteObject(image);ReleaseDC(nullptr,screen);
            aboutCaptured=true;
        }
        SendMessage(dialog, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    auto result = OptionsProc(dialog, message, wParam, lParam);
    if (message == WM_INITDIALOG) {
        optionsValid &= TabCtrl_GetItemCount(GetDlgItem(dialog, IDC_TAB)) == 6;
        for (auto& page : g_OptionsTabs) optionsValid &= IsWindow(page.hPage) != FALSE;
        wchar_t title[128]{}, version[64]{}, copyright[256]{}, lastTab[32]{};
        GetWindowText(dialog,title,_countof(title));
        optionsValid &= wcscmp(title,L"ZoomIt Custom 1.1.4")==0;
        TCITEM item{};item.mask=TCIF_TEXT;item.pszText=lastTab;item.cchTextMax=_countof(lastTab);
        TabCtrl_GetItem(GetDlgItem(dialog,IDC_TAB),ABOUT_PAGE,&item);
        optionsValid &= wcscmp(lastTab,L"About")==0;
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_VERSION,version,_countof(version));
        GetDlgItemText(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_COPYRIGHT,copyright,_countof(copyright));
        optionsValid &= wcscmp(version,L"Version 1.1.4")==0 &&
                        wcsstr(copyright,L"Prof. ing. Raffaele Mele")!=nullptr;
        optionsValid &= IsWindow(GetDlgItem(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_REPOSITORY)) &&
                        IsWindow(GetDlgItem(g_OptionsTabs[ABOUT_PAGE].hPage,IDC_ABOUT_LICENSE));
        wchar_t supervision[96]{};
        GetDlgItemTextW(dialog,IDC_SUPERVISION_STATUS,supervision,_countof(supervision));
        optionsValid &= wcscmp(supervision,L"Sessione: esecuzione autonoma")==0 &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_OFF)) &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_NORMAL)) &&
            IsWindow(GetDlgItem(dialog,IDC_STARTUP_SUPERVISED));
        SetTimer(dialog, 97, 35, nullptr);
    }
    return result;
}

int main(int argc, char** argv) {
    const bool snipOnly = argc>1 && strcmp(argv[1],"--snip-only")==0;
    std::wstring capturePath;
    if(argc>2) {capturePath=std::filesystem::absolute(argv[2]).wstring();aboutCapturePath=capturePath.c_str();}
    else {
        wchar_t image[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"ZOOMIT_TEST_OPTIONS_CAPTURE",image,MAX_PATH)) {
            capturePath=std::filesystem::absolute(image).wstring();aboutCapturePath=capturePath.c_str();
        }
    }
    POINT oldCursor{}; GetCursorPos(&oldCursor);
    try {
        const auto logFolder = std::filesystem::current_path() / ("regression-errors-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(logFolder);
        SetEnvironmentVariableW(L"ZOOMIT_TEST_LOG_DIRECTORY", logFolder.c_str());
        require(g_ToggleKey==((HOTKEYF_CONTROL<<8)|'1') &&
                g_LiveZoomToggleKey==((HOTKEYF_CONTROL<<8)|'2') &&
                g_DrawToggleKey==((HOTKEYF_CONTROL<<8)|'3') &&
                g_BreakToggleKey==((HOTKEYF_CONTROL<<8)|'4') &&
                g_SnipToggleKey==((HOTKEYF_CONTROL<<8)|'5'),"Default shortcuts must follow requested order");
        g_ToggleKey='Z'; require(MigrateHotkeys(),"Old shortcut schema must migrate");
        require(g_ToggleKey==((HOTKEYF_CONTROL<<8)|'1'),"Migrated zoom shortcut");
        g_ToggleKey='Z'; require(!MigrateHotkeys() && g_ToggleKey=='Z',"Later customized shortcuts must be preserved");
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
        g_ToggleKey=g_DrawToggleKey=g_LiveZoomToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
        g_AnimateZoom=FALSE;
        pMagInitialize=MagInitialize;
        pMagSetWindowSource=MagSetWindowSource;
        pMagSetWindowTransform=MagSetWindowTransform;
        pMagSetWindowFilterList=MagSetWindowFilterList;
        pMagSetFullscreenTransform=MagSetFullscreenTransform;
        pMagSetInputTransform=TestInputTransform;
        pMagShowSystemCursor=TestShowSystemCursor;
        pSetLayeredWindowAttributes=SetLayeredWindowAttributes;
        pGetMonitorInfo=GetMonitorInfoA;
        pMonitorFromPoint=MonitorFromPoint;
        pDwmIsCompositionEnabled=reinterpret_cast<type_pDwmIsCompositionEnabled>(GetProcAddress(LoadLibrary(L"dwmapi.dll"), "DwmIsCompositionEnabled"));
        g_OsVersion=WIN10_VERSION;
        require(MagInitialize()!=FALSE,"Magnification API initialization");
        g_hWndMain=InitInstance(GetModuleHandle(nullptr),SW_HIDE);
        require(g_hWndMain!=nullptr,"Main window creation");
        g_SliderZoomLevel=0; g_AnimateZoom=FALSE;
        WNDCLASS wc{}; wc.lpfnWndProc=DefWindowProc; wc.hInstance=GetModuleHandle(nullptr); wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.lpszClassName=L"ZoomItTestHost"; RegisterClass(&wc);
        // Keep the test pointer over a window owned by this process; browser hover cursors are legitimate after exit.
        HWND host=CreateWindowEx(0,L"ZoomItTestHost",L"ZoomIt regression tests",WS_OVERLAPPEDWINDOW,0,0,
                                 GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),
                                 nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
        ShowWindow(host,SW_SHOW);
        SetForegroundWindow(host);
        SetCursorPos(125,125);
        size_t ignoredZoomToggles=0, ignoredZoomMenuCommands=0, liveZoomIgnoreCases=0;
        auto modeState=[&]{return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
        auto checkIgnoredZoom=[&] {
            const auto mode=modeState();
            const HWND live=g_hWndLiveZoom, magnifier=g_hWndLiveZoomMag;
            const auto style=GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE);
            const bool canvasVisible=IsWindowVisible(g_hWndMain)!=FALSE;
            const float level=*reinterpret_cast<float*>(SendMessage(live,WM_USER_GET_ZOOM_LEVEL,0,0));
            const RECT view=*reinterpret_cast<RECT*>(SendMessage(live,WM_USER_GET_SOURCE_RECT,0,0));
            const LONG magnifierStyle=IsWindow(magnifier)?GetWindowLong(magnifier,GWL_STYLE):0;
            const HDC canvas=reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));
            std::vector<BYTE> before;
            if(canvasVisible) {
                require(canvas!=nullptr,"Drawing canvas must be present before testing ignored Zoom");
                DrawingDib region(canvas,Gdiplus::Rect(120,120,80,60));
                require(region.pixels()!=nullptr,"Capture drawing pixels before ignored Zoom");
                before.assign(region.pixels(),region.pixels()+80*60*4);
            }
            auto unchanged=[&] {
                require(g_hWndLiveZoom==live && IsWindowVisible(live) && g_hWndLiveZoomMag==magnifier &&
                        modeState()==mode && (IsWindowVisible(g_hWndMain)!=FALSE)==canvasVisible &&
                        GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)==style,
                        "Zoom toggle in LiveZoom must preserve the current windows and drawing mode");
                const RECT after=*reinterpret_cast<RECT*>(SendMessage(live,WM_USER_GET_SOURCE_RECT,0,0));
                require(*reinterpret_cast<float*>(SendMessage(live,WM_USER_GET_ZOOM_LEVEL,0,0))==level &&
                        EqualRect(&after,&view) && (!IsWindow(magnifier)||GetWindowLong(magnifier,GWL_STYLE)==magnifierStyle),
                        "Ignored Zoom toggle must preserve live magnification, view and pointer style");
                if(canvasVisible) {
                    require(reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0))==canvas,
                            "Ignored Zoom must retain the drawing canvas");
                    DrawingDib region(canvas,Gdiplus::Rect(120,120,80,60));
                    require(region.pixels()!=nullptr,"Capture drawing pixels after ignored Zoom");
                    for(size_t pixel=0;pixel<80*60;++pixel)
                        for(size_t channel=0;channel<3;++channel)
                            require(region.pixels()[pixel*4+channel]==before[pixel*4+channel],
                                    "Ignored Zoom must preserve annotation pixels");
                }
            };
            for(LPARAM shortcut : {MAKELPARAM(MOD_CONTROL,'1'),MAKELPARAM(MOD_CONTROL|MOD_ALT,VK_F24)}) {
                SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,shortcut);
                unchanged();++ignoredZoomToggles;
            }
            SendMessage(g_hWndMain,WM_COMMAND,IDC_ZOOM,0);
            unchanged();++ignoredZoomMenuCommands;++liveZoomIgnoreCases;
        };
        for(bool fullscreen : {false,true}) {
            g_fullScreenWorkaround=fullscreen;
            SetCursorPos(125,125);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
            require(IsWindowVisible(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain) && modeState()==0,
                    "LiveZoom must start without a frozen canvas or implicit Draw");
            checkIgnoredZoom();pump(20);
            require(modeState()==0 && !IsWindowVisible(g_hWndMain),"Ignored Zoom must not queue a delayed transition");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(30);
            require(IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom) && (modeState()&3)==3,
                    "The original LiveDraw command must still work through its internal capture request");
            checkIgnoredZoom();
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            require((modeState()&7)==7,"Drawing stroke must be active before ignored Zoom");
            checkIgnoredZoom();
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            require((modeState()&7)==3,"Drawing must remain active after completing a stroke");
            checkIgnoredZoom();pump(10);
            require(IsWindowVisible(g_hWndMain) && IsWindowVisible(g_hWndLiveZoom),
                    "Ignored menu Zoom must not queue destruction of LiveDraw");
            SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(100);
            if(IsWindowVisible(g_hWndLiveZoom)) SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(30);require(!IsWindow(g_hWndLiveZoom),"LiveZoom must still close normally");
            SetForegroundWindow(host);
        }
        g_fullScreenWorkaround=true;
        denyInputTransform=true;
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
        require(!IsWindow(g_hWndLiveZoom) && modeState()==0 && systemCursorShown,
                "Denied fullscreen input transform must return safely to desktop with visible cursor");
        denyInputTransform=false;
        g_fullScreenWorkaround=false;
        size_t suspensionCases=0, suspensionCycles=0, suspendedSnipCases=0, nativeResumeCases=0;
        auto currentCanvas=[&]{return reinterpret_cast<HDC>(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0));};
        auto canvasPixels=[&] {
            DrawingDib image(currentCanvas(),Gdiplus::Rect(100,100,300,200));
            require(image.pixels()!=nullptr,"Read annotation pixels for suspension tests");
            return std::vector<BYTE>(image.pixels(),image.pixels()+300*200*4);
        };
        auto samePixels=[&](const std::vector<BYTE>& before) {
            const auto after=canvasPixels();
            for(size_t pixel=0;pixel<300*200;++pixel)
                for(size_t channel=0;channel<3;++channel)
                    require(before[pixel*4+channel]==after[pixel*4+channel],
                            "Suspension, mouse movement and colour changes must not alter annotations");
        };
        auto verifyRing=[&](bool savePreview=false) {
            require((modeState()&23)==17,"First right click must suspend Draw without leaving zoom or tracing");
            const HCURSOR ring=reinterpret_cast<HCURSOR>(SendMessage(g_hWndMain,WM_TEST_QUERY_SUSPENDED_CURSOR,0,0));
            require(ring && ring!=LoadCursor(nullptr,IDC_ARROW),"Suspension must use the coloured circle");
            CURSORINFO pointer{sizeof(pointer)};
            require(GetCursorInfo(&pointer) && (pointer.flags&CURSOR_SHOWING) && pointer.hCursor==ring,
                    "The suspension circle must remain visible as the real mouse cursor");
            require(GetCapture()==g_hWndMain,"Suspension must receive both buttons even through transparent LiveDraw");
            require(systemCursorShown,"Suspension ring must enable the native cursor surface");
            ICONINFO icon{};require(GetIconInfo(ring,&icon)!=FALSE,"Inspect suspension cursor");
            BITMAP bitmap{};GetObject(icon.hbmColor,sizeof(bitmap),&bitmap);
            HDC screen=GetDC(nullptr);
            {
                DrawingDib image(screen,Gdiplus::Rect(0,0,bitmap.bmWidth+16,bitmap.bmHeight+16));
                HBRUSH background=CreateSolidBrush(RGB(240,240,240));
                RECT area{0,0,bitmap.bmWidth+16,bitmap.bmHeight+16};FillRect(image.dc(),&area,background);DeleteObject(background);
                require(DrawIconEx(image.dc(),8,8,ring,bitmap.bmWidth,bitmap.bmHeight,0,nullptr,DI_NORMAL)!=FALSE,
                        "Render suspension cursor");
                const int centre=bitmap.bmWidth/2, radius=bitmap.bmWidth/4;
                require(GetPixel(image.dc(),8+centre,8+centre)==RGB(32,32,32),"Suspension circle must have a dark interior");
                require(GetPixel(image.dc(),8+centre+radius-1,8+centre)==(g_PenColor&0xFFFFFF),
                        "Suspension circle border must match the active drawing colour");
                require(GetPixel(image.dc(),8,8)==RGB(240,240,240),"Cursor corners must be transparent");
                if(savePreview && aboutCapturePath) {
                    auto path=std::filesystem::path(aboutCapturePath).parent_path()/L"suspension-pointer-1.1.3.png";
                    HBITMAP imageBitmap=static_cast<HBITMAP>(GetCurrentObject(image.dc(),OBJ_BITMAP));
                    require(SavePng(const_cast<wchar_t*>(path.c_str()),imageBitmap)==ERROR_SUCCESS,"Save suspension pointer preview");
                }
            }
            ReleaseDC(nullptr,screen);DeleteObject(icon.hbmColor);DeleteObject(icon.hbmMask);
        };
        auto rawMouse=[&] {POINT point{};GetCursorPos(&point);ScreenToClient(g_hWndMain,&point);return point;};
        auto resume=[&](POINT client) {
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(client.x,client.y));
            require((modeState()&23)==7,"One left click must restore Draw and start a new stroke immediately");
            require(GetCapture()!=g_hWndMain && !SendMessage(g_hWndMain,WM_TEST_QUERY_SUSPENDED_CURSOR,0,0),
                    "Resuming Draw must release suspension capture and ring");
            if(IsWindowVisible(g_hWndLiveZoom))
                require(!systemCursorShown,"Resumed LiveDraw must hide the hardware cursor to avoid duplicate pointers, including fullscreen magnification");
            const POINT point=rawMouse();
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x+8,point.y));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(point.x+8,point.y));
            require((modeState()&23)==3,"Resumed stroke must finish in the original Draw mode");
        };
        for(int variant=0;variant<7;++variant) {
            ++suspensionCases;
            g_AnimateZoom=FALSE;g_fullScreenWorkaround=variant==3 || variant==5;
            g_DrawPointer=variant==6;
            g_PenColor=COLOR_RED|0xff000000;g_RootPenWidth=5;g_PenWidth=5;
            SetCursorPos(125,125);
            if(variant>=2 && variant<=5) {
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(100);
                SendMessage(g_hWndMain,WM_HOTKEY,variant>=4 ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0);pump(30);
            } else if(variant==1) {
                SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1'));pump(60);
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            } else {
                SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(30);
            }
            SendMessage(g_hWndMain,WM_MOUSEMOVE,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(160,150));
            // Also exercise suspension during a stroke, before its mouse-up arrives.
            if(variant%2==0) SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(160,150));
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,MAKELPARAM(160,150));SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
            verifyRing(variant==0);
            const auto before=canvasPixels();
            RECT viewBefore{},viewAfter{};
            const auto level=SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,reinterpret_cast<LPARAM>(&viewBefore));
            for(int i=0;i<8;++i) {
                SendMessage(g_hWndMain,WM_SETCURSOR,reinterpret_cast<WPARAM>(g_hWndMain),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
                SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(210+i,210));
            }
            POINT moved{270,230};ClientToScreen(g_hWndMain,&moved);SetCursorPos(moved.x,moved.y);pump(20);
            verifyRing();samePixels(before);
            require(SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,reinterpret_cast<LPARAM>(&viewAfter))==level &&
                    EqualRect(&viewBefore,&viewAfter),"Suspended static view must not pan when moving its cursor");
            SendMessage(g_hWndMain,WM_KEYDOWN,'G',0);
            verifyRing();samePixels(before);
            if(variant==4 || variant==5) checkIgnoredZoom();
            if(variant==1) {
                cancelSnip=false;SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                require(g_TestSnipBitmap!=nullptr,"Snip must work while static Draw is suspended");
                DeleteObject(g_TestSnipBitmap);g_TestSnipBitmap=nullptr;++suspendedSnipCases;
                verifyRing();samePixels(before);
            }
            // Resuming must not connect the old stroke to the new mouse position.
            const POINT click=rawMouse();
            const float factor=DecodeZoomLevel(level);
            const POINT next{viewBefore.left+static_cast<LONG>(click.x/factor),
                             viewBefore.top+static_cast<LONG>(click.y/factor)};
            const POINT between{(160+next.x)/2,(150+next.y)/2};
            const COLORREF beforeBetween=GetPixel(currentCanvas(),between.x,between.y);
            if(variant==4) {
                SetForegroundWindow(g_hWndMain);
                require(GetForegroundWindow()==g_hWndMain,"LiveDraw must own the foreground before native click tests");
                INPUT clickInput{};clickInput.type=INPUT_MOUSE;clickInput.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
                require(SendInput(1,&clickInput,sizeof(clickInput))==1,"Send real left button down");pump(20);
                require((modeState()&23)==7 && GetCapture()!=g_hWndMain,"The real left click must reach paused transparent LiveDraw and resume its stroke");
                clickInput.mi.dwFlags=MOUSEEVENTF_LEFTUP;
                require(SendInput(1,&clickInput,sizeof(clickInput))==1,"Send real left button up");pump(20);
                require((modeState()&23)==3,"The real resumed stroke must finish normally");++nativeResumeCases;
            } else {
                resume(click);
            }
            require(GetPixel(currentCanvas(),between.x,between.y)==beforeBetween,
                    "Resuming must not draw a line connecting the previous stroke to the new position");
            require((g_DrawPointer!=FALSE)==(variant==6),"Resume must preserve the previous coloured pointer shape");
            require((g_PenColor&0xFFFFFF)==COLOR_GREEN,"Resume must preserve the selected active colour");
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);pump(100);
            require(!IsWindowVisible(g_hWndMain) && !(modeState()&16) && GetCapture()!=g_hWndMain,
                    "Second right click must completely leave Draw and release mouse capture");
            require(!(GetWindowLongPtr(g_hWndMain,GWL_EXSTYLE)&WS_EX_LAYERED),"Second right click must clear LiveDraw's transparent window mode");
            if(IsWindowVisible(g_hWndLiveZoom)) SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(30);
            CURSORINFO pointer{sizeof(pointer)};
            require(GetCursorInfo(&pointer) && (pointer.flags&CURSOR_SHOWING) && pointer.hCursor==LoadCursor(nullptr,IDC_ARROW),
                    "Every completed suspension exit must restore the normal mouse pointer");
            SetForegroundWindow(host);
        }
        g_DrawPointer=FALSE;g_fullScreenWorkaround=FALSE;
        SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(30);
        auto pauseResume=[&] {
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);
            SendMessage(g_hWndMain,WM_KEYDOWN,suspensionCycles%2 ? 'B' : 'R',0);
            verifyRing();resume(rawMouse());++suspensionCycles;
        };
        for(int i=0;i<12;++i) pauseResume();
        const DWORD suspendedGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const DWORD suspendedUser=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        for(int i=0;i<64;++i) pauseResume();
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==suspendedGdi &&
                GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)==suspendedUser,
                "Repeated suspend/resume and colour changes must not leak cursors, bitmaps or capture");
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(70);
        require(!IsWindowVisible(g_hWndMain) && !(modeState()&16) && GetCapture()!=g_hWndMain,
                "Escape must clear suspension and mouse capture");
        for(bool fullscreen : {false,true}) {
            g_fullScreenWorkaround=fullscreen;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(70);
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(30);
            SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(30);
            require(!IsWindowVisible(g_hWndMain) && !IsWindow(g_hWndLiveZoom) && GetCapture()!=g_hWndMain && !(modeState()&16),
                    "Ctrl+2 must close suspended LiveDraw and release capture");
        }
        g_fullScreenWorkaround=FALSE;

        SetCursorPos(125,125);SetForegroundWindow(host);pump(10);
        size_t drawCycleCount=0;
        auto runCycle=[&](bool fullscreen, bool liveDraw=false, bool exitWithLiveHotkey=false) {
            ++drawCycleCount;
            g_fullScreenWorkaround=fullscreen;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(100);
            require(IsWindowVisible(g_hWndLiveZoom),"LiveZoom must start");
            SendMessage(g_hWndMain,WM_HOTKEY,liveDraw ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,0);
            pump(30);
            SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(150,150));
            for(int x=151;x<175;++x) SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,150));
            SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(175,150));
            SendMessage(g_hWndMain,WM_KEYDOWN,'T',0);
            for(wchar_t letter : std::wstring(L"Test")) SendMessage(g_hWndMain,WM_CHAR,letter,0);
            SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
            if(liveDraw && exitWithLiveHotkey) {
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
                SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(30);
                require(!IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),"LiveZoom shortcut must close active LiveDraw synchronously");
                CURSORINFO cursor{sizeof(cursor)};
                require(GetCursorInfo(&cursor) && (cursor.flags&CURSOR_SHOWING) && cursor.hCursor==LoadCursor(nullptr,IDC_ARROW),"Closing active LiveDraw must restore the system pointer");
                SetForegroundWindow(host);
                return;
            }
            if(liveDraw) {
                SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(175,150));
                SendMessage(g_hWndMain,WM_USER_EXIT_MODE,0,0);
                pump(15);
                verifyRing();
                if(!fullscreen) require(!(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR),
                                        "Paused LiveDraw must show one suspension ring without a duplicate magnified cursor");
                SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);
            } else SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,SHALLOW_DESTROY);
            pump(100);
            require(IsWindowVisible(g_hWndLiveZoom),"Exiting static drawing must restore LiveZoom");
            float level=*reinterpret_cast<float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
            require(level>1.0f,"Restored LiveZoom must retain fractional magnification");
            if(!fullscreen)
                require(GetWindowLong(g_hWndLiveZoomMag,GWL_STYLE)&MS_SHOWMAGNIFIEDCURSOR,"Magnified cursor must be restored");
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
            pump(100);
            require(!IsWindow(g_hWndLiveZoom),"LiveZoom must close");
            CURSORINFO cursor{sizeof(cursor)};
            if (!GetCursorInfo(&cursor)) { std::cerr << "GetCursorInfo error=" << GetLastError() << "\n"; throw std::runtime_error("Cannot inspect system pointer"); }
            if (!(cursor.flags & CURSOR_SHOWING)) std::cerr << "pointer flags=" << cursor.flags << " cursor=" << cursor.hCursor << " fullscreen=" << fullscreen << "\n";
            require((cursor.flags&CURSOR_SHOWING)!=0,"System pointer must be visible after exiting LiveZoom");
            SetForegroundWindow(host);
        };
        size_t liveToggleCount = 0;
        auto pointerVisible = [] {
            CURSORINFO cursor{sizeof(cursor)};
            require(GetCursorInfo(&cursor)!=FALSE,"Inspect system pointer");
            require((cursor.flags & CURSOR_SHOWING) && cursor.hCursor,
                    "Every LiveZoom exit must restore a real visible pointer");
            if(cursor.hCursor!=LoadCursor(nullptr,IDC_ARROW)) {
                POINT point{};GetCursorPos(&point);wchar_t name[128]{};GetClassName(WindowFromPoint(point),name,_countof(name));
                std::wcerr<<L"Unexpected exit cursor "<<cursor.hCursor<<L" at "<<point.x<<L","<<point.y<<L" over "<<name<<L"\n";
            }
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
            g_AnimateZoom=FALSE;
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1')); pump(100);
            auto snipGdi = GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            for(int i=0;i<6;++i) {
                cancelSnip=false;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                BITMAP cropped{};
                require(g_TestSnipBitmap && GetObject(g_TestSnipBitmap,sizeof(cropped),&cropped),"Snip must capture inside active zoom");
                require(cropped.bmWidth==81 && cropped.bmHeight==81,"Snip crop dimensions");
                DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
                require(IsWindowVisible(g_hWndMain),"Snip must preserve pre-existing static zoom");
                cancelSnip=true;
                SetTimer(nullptr,0,15,SelectTestRegion);
                SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0);
                require(!g_TestSnipBitmap && IsWindowVisible(g_hWndMain),"Cancelled Snip must preserve static zoom");
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
                    require(image.GetWidth()==(i%2 ? 64 : 81) && image.GetHeight()==(i%2 ? 64 : 81),"PNG must have requested scale");
                }
                DeleteFile(imagePath.c_str()); g_TestSavePath=nullptr;
                if(i==0) snipGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS); // Warm selection and image codecs once.
                require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==snipGdi,"Snip copy, save and cancellation must not leak GDI objects");
            }
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,SHALLOW_DESTROY); pump(30);
            pointerVisible();
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(100);
            cancelSnip=false; SetTimer(nullptr,0,15,SelectTestRegion);
            SendMessage(g_hWndMain,WM_HOTKEY,SNIP_HOTKEY,0); pump(100);
            require(g_TestSnipBitmap && IsWindowVisible(g_hWndLiveZoom),"Snip from LiveZoom must resume LiveZoom");
            DeleteObject(g_TestSnipBitmap); g_TestSnipBitmap=nullptr;
            SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0); pump(30);
            pointerVisible();
        }
        if(!snipOnly) {runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);}
        DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
        g_BreakOnSecondary=false; g_BreakShowBackgroundFile=false;
        SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
        SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,0); pump(10);
        const auto gdiBaseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userBaseline=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryBefore=privateBytes();
        for(int i=0;i<(snipOnly ? 0 : 12);++i) {
            g_AnimateZoom=(i%2)!=0;
            runCycle(false); runCycle(true); runCycle(false,true); runCycle(true,true);
            runCycle(false,true,true); runCycle(true,true,true);
            DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
            SendMessage(g_hWndMain,WM_COMMAND,IDC_BREAK,0); pump(10);
            SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,0); pump(10);
        }
        require(optionsValid,"Options must have six pages with About last, correct title/version and no recording or typing pages");
        const auto gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        const auto userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        const auto memoryAfter=privateBytes();
        require(gdiAfter<=gdiBaseline+1,"Repeated transitions must not leak GDI resources");
        require(userAfter<=userBaseline+1,"Repeated transitions must not leak USER resources");
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);pump(70);
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_DRAW_HOTKEY,0);pump(20);
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);SendMessage(g_hWndMain,WM_RBUTTONUP,0,0);verifyRing();
        DestroyWindow(g_hWndMain);
        require(GetCapture()!=g_hWndMain,"Quitting while suspended must release mouse capture");
        require(!IsWindow(g_hWndLiveZoom),"Quitting must destroy the magnifier");
        CURSORINFO cursor{sizeof(cursor)};if (!GetCursorInfo(&cursor)) { std::cerr << "GetCursorInfo error=" << GetLastError() << "\n"; throw std::runtime_error("Cannot inspect system pointer"); }
        require((cursor.flags&CURSOR_SHOWING)!=0,"Quitting must restore the system pointer");
        DestroyWindow(host);
        MagUninitialize();
        SetCursorPos(oldCursor.x,oldCursor.y);
        std::cout<<"{\"passed\":true,\"live_draw_cycles\":"<<drawCycleCount<<",\"undo_1080p_entries\":"<<count
          <<",\"suspension_cases\":"<<suspensionCases<<",\"suspend_resume_cycles\":"<<suspensionCycles
          <<",\"native_resume_cases\":"<<nativeResumeCases<<",\"suspended_snip_cases\":"<<suspendedSnipCases<<",\"live_zoom_ignore_cases\":"<<liveZoomIgnoreCases<<",\"ignored_zoom_toggles\":"<<ignoredZoomToggles
          <<",\"ignored_zoom_menu_commands\":"<<ignoredZoomMenuCommands<<",\"live_toggle_events\":"<<liveToggleCount<<",\"effect_operations\":1200,\"effects_milliseconds\":"<<effectsMilliseconds
          <<",\"effect_gdi_before\":"<<effectsBefore<<",\"effect_gdi_after\":"<<effectsAfter
          <<",\"effect_private_before\":"<<effectMemoryBefore<<",\"effect_private_after\":"<<effectMemoryAfter
          <<",\"snip_cases\":19,\"options_open_close_cycles\":"<<(snipOnly?1:13)<<",\"timer_cycles\":"<<(snipOnly?1:13)
          <<",\"gdi_before\":"<<gdiBaseline<<",\"gdi_after\":"<<gdiAfter
          <<",\"user_before\":"<<userBaseline<<",\"user_after\":"<<userAfter
          <<",\"private_bytes_before\":"<<memoryBefore<<",\"private_bytes_after\":"<<memoryAfter<<"}\n";
        return 0;
    } catch(const std::exception& ex) {
        std::cerr<<"FAILED: "<<ex.what()<<"\n";
        if(IsWindow(g_hWndMain)) DestroyWindow(g_hWndMain);
        if(IsWindow(g_hWndLiveZoom)) DestroyWindow(g_hWndLiveZoom);
        MagSetFullscreenTransform(1,0,0);
        MagShowSystemCursor(TRUE);
        MagUninitialize();
        ClipCursor(nullptr);
        SetCursorPos(oldCursor.x,oldCursor.y);
        return 1;
    }
}





