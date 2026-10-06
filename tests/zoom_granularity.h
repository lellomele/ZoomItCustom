#pragma once
#include <limits>

struct ZoomGranularityResults {
    size_t migrationCases{}, registryCases{}, guiCases{}, staticCases{}, liveCases{}, reversalCases{};
    DWORD gdiBefore{}, gdiAfter{}, userBefore{}, userAfter{};
};
bool zoomOptionsSave=false;
DWORD zoomOptionsIndex=3;
size_t zoomOptionsCases=0;
const wchar_t* zoomOptionsCapture=nullptr;

INT_PTR CALLBACK ZoomGranularityOptionsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if(message==WM_TIMER && wParam==96) {
        KillTimer(dialog,96);
        HWND page=g_OptionsTabs[ZOOM_PAGE].hPage;
        HWND slider=GetDlgItem(page,IDC_ZOOM_SLIDER);
        require(SendMessage(slider,TBM_GETRANGEMIN,0,0)==0 &&
                SendMessage(slider,TBM_GETRANGEMAX,0,0)==11,"Options must expose all twelve initial zoom levels");
        require(IsWindowVisible(slider) && IsWindowEnabled(slider),"The zoom selector must be visible and usable");
        RECT thumb{};SendMessage(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        require(thumb.right>thumb.left && thumb.bottom>thumb.top,"The native slider must expose a visible thumb");
        const DWORD index=g_SliderZoomLevel, percent=g_InitialZoomPercent;
        for(DWORD i=0;i<12;++i) {
            SendMessage(slider,TBM_SETPOS,TRUE,i);
            SendMessage(page,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(slider));
            wchar_t actual[32]{},expected[32]{};
            GetDlgItemTextW(page,IDC_INITIAL_ZOOM_VALUE,actual,_countof(actual));
            const DWORD value=125+i*25;
            swprintf_s(expected,L"%u.%02ux",value/100,value%100);
            require(wcscmp(actual,expected)==0,"Slider movement must immediately show the selected numeric factor");
            require(g_SliderZoomLevel==index && g_InitialZoomPercent==percent,
                    "Previewing initial zoom must not apply it before OK");
            ++zoomOptionsCases;
        }
        SendMessage(slider,TBM_SETPOS,TRUE,zoomOptionsIndex);
        SendMessage(page,WM_HSCROLL,TB_ENDTRACK,reinterpret_cast<LPARAM>(slider));
        if(zoomOptionsCapture) {
            SetWindowPos(dialog,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
            SetForegroundWindow(dialog);pump(300);
            RedrawWindow(dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            RECT window{};GetWindowRect(dialog,&window);
            HDC screen=GetDC(nullptr), memory=CreateCompatibleDC(screen);
            HBITMAP image=CreateCompatibleBitmap(screen,window.right-window.left,window.bottom-window.top);
            const auto old=SelectObject(memory,image);
            require(BitBlt(memory,0,0,window.right-window.left,window.bottom-window.top,
                           screen,window.left,window.top,SRCCOPY),"Capture the rendered selector from the native screen");
            const DWORD result=SavePng(const_cast<wchar_t*>(zoomOptionsCapture),image);
            SelectObject(memory,old);DeleteDC(memory);DeleteObject(image);ReleaseDC(nullptr,screen);
            require(result==ERROR_SUCCESS,"Save new zoom selector verification image");
            zoomOptionsCapture=nullptr;
        }
        SendMessage(dialog,WM_COMMAND,zoomOptionsSave ? IDOK : IDCANCEL,0);
        return TRUE;
    }
    const auto result=OptionsProc(dialog,message,wParam,lParam);
    if(message==WM_INITDIALOG) {
        wchar_t actual[32]{},expected[32]{};
        GetDlgItemTextW(g_OptionsTabs[ZOOM_PAGE].hPage,IDC_INITIAL_ZOOM_VALUE,actual,_countof(actual));
        const DWORD percent=125+g_SliderZoomLevel*25;
        swprintf_s(expected,L"%u.%02ux",percent/100,percent%100);
        require(wcscmp(actual,expected)==0,"Options must initialize the factor label from the saved setting");
        SetTimer(dialog,96,40,nullptr);
    }
    return result;
}

ZoomGranularityResults RunZoomGranularityRegression(HWND host) {
    ZoomGranularityResults results{};
    const DWORD savedIndex=g_SliderZoomLevel,savedPercent=g_InitialZoomPercent,savedLegacy=g_LegacySliderZoomLevel;
    const BOOLEAN savedStaticAnimation=g_AnimateZoom,savedLiveAnimation=g_AnimateLiveZoom;
    const BOOL savedFullscreen=g_fullScreenWorkaround;
    const DWORD keys[]{g_ToggleKey,g_LiveZoomToggleKey,g_DrawToggleKey,g_BreakToggleKey,g_SnipToggleKey};
    auto restore=zoomit::OnExit([&] {
        g_SliderZoomLevel=savedIndex;g_InitialZoomPercent=savedPercent;g_LegacySliderZoomLevel=savedLegacy;
        g_AnimateZoom=savedStaticAnimation;g_AnimateLiveZoom=savedLiveAnimation;g_fullScreenWorkaround=savedFullscreen;
        g_ToggleKey=keys[0];g_LiveZoomToggleKey=keys[1];g_DrawToggleKey=keys[2];g_BreakToggleKey=keys[3];g_SnipToggleKey=keys[4];
        zoomOptionsSave=false;g_TestMode=true;
    });
    const std::wstring fixturePath=L"Software\\ZoomItCustom\\ZoomGranularity_"+std::to_wstring(GetCurrentProcessId());
    HKEY fixture{};
    require(RegCreateKeyExW(HKEY_CURRENT_USER,fixturePath.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&fixture,nullptr)==ERROR_SUCCESS,
            "Create isolated settings fixture");
    struct FixtureCleanup {
        std::wstring path;HKEY handle{};bool active{};
        ~FixtureCleanup(){if(active)RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);if(handle)RegCloseKey(handle);RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());}
    } cleanup{fixturePath,fixture};
    require(RegOverridePredefKey(HKEY_CURRENT_USER,fixture)==ERROR_SUCCESS,"Isolate all HKCU changes within the native test process");
    cleanup.active=true;
    ClassRegistry settings(L"ZoomLevels");
    REG_SETTING table[3]{};
    for(auto& setting:RegSettings) {
        if(setting.Setting==&g_LegacySliderZoomLevel)table[0]=setting;
        if(setting.Setting==&g_InitialZoomPercent)table[1]=setting;
    }
    require(table[0].ValueName && table[1].ValueName,"Test the actual old and new zoom persistence entries");
    auto writeRaw=[&](const wchar_t* name,DWORD value) {
        HKEY key{};require(RegCreateKeyExW(HKEY_CURRENT_USER,L"ZoomLevels",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)==ERROR_SUCCESS,"Open isolated zoom settings");
        const auto result=RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
        RegCloseKey(key);require(result==ERROR_SUCCESS,"Write raw persisted zoom preference");
    };
    auto removeNew=[&] {
        HKEY key{};if(RegOpenKeyExW(HKEY_CURRENT_USER,L"ZoomLevels",0,KEY_SET_VALUE,&key)==ERROR_SUCCESS){RegDeleteValueW(key,table[1].ValueName);RegCloseKey(key);}
    };
    // Every prior valid preference must retain its magnification on a first upgrade.
    const DWORD legacyValues[]{125,150,175,200,300,400};
    for(DWORD i=0;i<6;++i) {
        removeNew();writeRaw(table[0].ValueName,i);settings.ReadRegSettings(table);ValidateSettings();
        require(g_InitialZoomPercent==legacyValues[i] && g_ZoomLevels[g_SliderZoomLevel]==legacyValues[i]/100.0f,
                "Upgrade must preserve each legacy factor, including 3x and 4x");
        settings.WriteRegSettings(table);g_InitialZoomPercent=0;g_LegacySliderZoomLevel=0;
        settings.ReadRegSettings(table);ValidateSettings();
        require(g_InitialZoomPercent==legacyValues[i],"Migrated zoom setting must remain stable after the next read");
        ++results.migrationCases;
    }
    removeNew();writeRaw(table[0].ValueName,MAXDWORD);settings.ReadRegSettings(table);ValidateSettings();
    require(g_InitialZoomPercent==200 && g_SliderZoomLevel==3,"Invalid legacy index must safely default to 2x");
    ++results.migrationCases;
    for(DWORD i=0;i<12;++i) {
        SetInitialZoomIndex(i);settings.WriteRegSettings(table);
        g_SliderZoomLevel=MAXDWORD;g_InitialZoomPercent=0;settings.ReadRegSettings(table);ValidateSettings();
        require(g_SliderZoomLevel==i && g_InitialZoomPercent==125+i*25,"Every fine level must round-trip as a factor, without index drift");
        ++results.registryCases;
    }
    for(DWORD bad:{DWORD{1},DWORD{124},DWORD{126},DWORD{401},DWORD{MAXDWORD}}) {
        writeRaw(table[1].ValueName,bad);settings.ReadRegSettings(table);ValidateSettings();
        require(g_InitialZoomPercent==200 && g_SliderZoomLevel==3,"Invalid new factors must fall back safely without array access outside bounds");
        ++results.registryCases;
    }
    writeRaw(table[0].ValueName,5);writeRaw(table[1].ValueName,250);settings.ReadRegSettings(table);ValidateSettings();
    require(g_InitialZoomPercent==250 && g_SliderZoomLevel==5,"A valid new factor must take precedence over the legacy compatibility setting");
    ++results.registryCases;
    // Exercise native dialog Cancel and OK, using real persistence under the isolated HKCU root.
    SetInitialZoomIndex(3);g_ToggleKey=g_LiveZoomToggleKey=g_DrawToggleKey=g_BreakToggleKey=g_SnipToggleKey=0;
    wchar_t screenshot[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"ZOOMIT_TEST_ZOOM_OPTIONS_CAPTURE",screenshot,_countof(screenshot)))zoomOptionsCapture=screenshot;
    zoomOptionsIndex=9;zoomOptionsSave=false;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,ZoomGranularityOptionsProc);
    require(g_SliderZoomLevel==3 && g_InitialZoomPercent==200,"Cancel must preserve both the runtime factor and its persisted value");
    zoomOptionsSave=true;g_TestMode=false;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,ZoomGranularityOptionsProc);g_TestMode=true;
    require(g_SliderZoomLevel==9 && g_InitialZoomPercent==350,"Options OK must apply the selected 3.5x factor");
    g_InitialZoomPercent=0;g_SliderZoomLevel=MAXDWORD;
    reg.ReadRegSettings(RegSettings);ValidateSettings();
    require(g_SliderZoomLevel==9 && g_InitialZoomPercent==350,"Options OK must persist the selected initial factor through the real application table");
    zoomOptionsSave=false;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,ZoomGranularityOptionsProc);
    results.guiCases=zoomOptionsCases;
    auto waitFor=[&](auto done,const char* message) {
        const auto deadline=GetTickCount64()+2500;
        while(!done() && GetTickCount64()<deadline)pump(5);
        require(done(),message);
    };
    auto mainLevel=[] {return DecodeZoomLevel(static_cast<WPARAM>(SendMessage(g_hWndMain,WM_TEST_QUERY_VIEW,0,0)));};
    auto liveLevel=[] {
        require(IsWindow(g_hWndLiveZoom),"Read live factor only while its window exists");
        return *reinterpret_cast<const float*>(SendMessage(g_hWndLiveZoom,WM_USER_GET_ZOOM_LEVEL,0,0));
    };
    auto verifyNative=[&] {
        float native{};
        if(g_fullScreenWorkaround){int x{},y{};require(MagGetFullscreenTransform(&native,&x,&y),"Read native fullscreen zoom");}
        else {MAGTRANSFORM transform{};require(MagGetWindowTransform(g_hWndLiveZoomMag,&transform),"Read native window zoom");native=transform.v[0][0];}
        require(std::fabs(native-liveLevel())<0.0001f,"Real native magnification must match the selected fine factor");
    };
    auto clean=[&] {
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(30);
        require(!IsWindow(g_hWndLiveZoom) && !(SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0)&1),"Cleanup must leave the desktop");
        ActivateTestHost(host);ReadOwnedNormalPointer(host,"Fine zoom cleanup");
    };
    auto prepare=[&] {ActivateTestHost(host);SetCursorPos(125,125);pump(10);};
    for(bool animated:{false,true}) {
        g_AnimateZoom=animated;SetInitialZoomIndex(3);prepare();
        SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1'));
        waitFor([&]{return mainLevel()==2.0f;},"Static entry must reach 2x");
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);
        if(!animated)require(mainLevel()==2.25f && GetUpdateRect(g_hWndMain,nullptr,FALSE),"Immediate fine zoom must update both the factor and the displayed image");
        waitFor([&]{return mainLevel()==2.25f;},"Static Up must select 2.25x");++results.staticCases;
        for(int i=0;i<7;++i)SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);
        waitFor([&]{return mainLevel()==4.0f;},"Rapid static Up commands must accumulate at 4x, independently of frames");++results.reversalCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_UP,0);waitFor([&]{return mainLevel()==8.0f;},"Static zoom must retain the 4x to 8x boundary");
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_DOWN,0);waitFor([&]{return mainLevel()==4.0f;},"Static zoom must return from 8x to 4x");results.staticCases+=2;
        for(int i=0;i<12;++i)SendMessage(g_hWndMain,WM_KEYDOWN,VK_DOWN,0);
        waitFor([&]{return mainLevel()==1.0f;},"Static fine zoom must reach 1x without crossing its minimum");++results.staticCases;
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_DOWN,0);require(mainLevel()==1.0f,"Static zoom must clamp at 1x");clean();
        g_AnimateZoom=FALSE;SetInitialZoomIndex(0);prepare();SendMessage(g_hWndMain,WM_HOTKEY,ZOOM_HOTKEY,MAKELPARAM(MOD_CONTROL,'1'));
        SendMessage(g_hWndMain,WM_MOUSEWHEEL,MAKEWPARAM(0,3*WHEEL_DELTA),0);
        require(mainLevel()==2.0f,"A three-notch wheel message must advance exactly three fine levels");++results.staticCases;clean();
    }
    for(bool fullscreen:{false,true})for(bool animated:{false,true}) {
        g_fullScreenWorkaround=fullscreen;g_AnimateLiveZoom=animated;SetInitialZoomIndex(0);prepare();
        SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
        waitFor([&]{return liveLevel()==1.25f;},"Live entry must reach 1.25x");verifyNative();
        for(int i=1;i<12;++i) {
            const float wanted=1.25f+i*0.25f, previous=wanted-0.25f;
            SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
            if(animated)require(liveLevel()>previous && liveLevel()<wanted,"Checked animation must retain a visible intermediate frame even for small increases");
            else require(liveLevel()==wanted,"Unchecked animation must apply the fine level immediately");
            waitFor([&]{return liveLevel()==wanted;},"Live increase must settle at the next quarter step");verifyNative();++results.liveCases;
        }
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);waitFor([&]{return liveLevel()==8.0f;},"Live zoom must retain the 4x to 8x boundary");
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);waitFor([&]{return liveLevel()==4.0f;},"Live zoom must return from 8x to 4x");results.liveCases+=2;
        for(int i=10;i>=0;--i) {
            const float wanted=1.25f+i*0.25f, previous=wanted+0.25f;
            SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
            if(animated)require(liveLevel()<previous && liveLevel()>wanted,"Checked animation must retain intermediate frames for small reductions");
            waitFor([&]{return liveLevel()==wanted;},"Live decrease must retrace the same fine levels");verifyNative();++results.liveCases;
        }
        // Reverse requests before a frame finishes; the target must remain symmetrical.
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        waitFor([&]{return liveLevel()==1.25f;},"Rapid mixed LiveZoom requests must return to the original target");++results.reversalCases;
        SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);
        waitFor([]{return !IsWindow(g_hWndLiveZoom);},"Reducing below 1.25x must return to the desktop and complete LiveZoom exit");
        ReadOwnedNormalPointer(host,"Exit below fine zoom range");++results.liveCases;clean();
    }
    require(NextZoomLevel(32.0f,true)==32.0f && NextZoomLevel(1.0f,false)==1.0f,
            "Supported zoom boundaries must remain bounded");
    for(float odd:{1.1f,1.6f,3.99f,5.0f,std::numeric_limits<float>::quiet_NaN()}) {
        const float up=NextZoomLevel(odd,true),down=NextZoomLevel(odd,false);
        require(std::isfinite(up)&&std::isfinite(down)&&up>=1&&up<=32&&down>=1&&down<=32,
                "A restored intermediate or invalid factor must remain finite and bounded");
    }
    results.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    results.userBefore=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    g_AnimateLiveZoom=FALSE;g_fullScreenWorkaround=FALSE;SetInitialZoomIndex(3);
    prepare();SendMessage(g_hWndMain,WM_HOTKEY,LIVE_HOTKEY,0);
    const DWORD activeGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const DWORD activeUser=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    for(int i=0;i<200;++i) {SendMessage(g_hWndLiveZoom,WM_HOTKEY,0,0);SendMessage(g_hWndLiveZoom,WM_HOTKEY,1,0);}
    require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==activeGdi &&
            GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)==activeUser,"Repeated fine adjustments must not allocate additional graphics or window resources");
    clean();results.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);results.userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    require(results.gdiAfter<=results.gdiBefore+1 && results.userAfter<=results.userBefore+1,"Fine zoom transitions must release all transient resources");
    return results;
}
void PrintZoomGranularityResults(const ZoomGranularityResults& results) {
    std::cout<<"{\"passed\":true,\"zoom_granularity\":true,\"migration_cases\":"<<results.migrationCases
        <<",\"registry_cases\":"<<results.registryCases<<",\"gui_cases\":"<<results.guiCases
        <<",\"static_cases\":"<<results.staticCases<<",\"live_cases\":"<<results.liveCases
        <<",\"rapid_reversal_cases\":"<<results.reversalCases<<",\"resource_adjustments\":400"
        <<",\"gdi_before\":"<<results.gdiBefore<<",\"gdi_after\":"<<results.gdiAfter
        <<",\"user_before\":"<<results.userBefore<<",\"user_after\":"<<results.userAfter<<"}\n";
}


