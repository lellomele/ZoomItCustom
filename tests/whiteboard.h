#pragma once

struct WhiteboardResults {unsigned nativeCases{},gridCases{},setupCases{},drawCycles{},policyCases{},hintCases{},resourceCycles{},optionsCases{},recoveryValidationCases{};DWORD gdiBefore{},gdiAfter{},userBefore{},userAfter{};};
inline unsigned g_BoardOptionStage{};
INT_PTR CALLBACK WhiteboardOptionsProbe(HWND dialog,UINT message,WPARAM word,LPARAM param) {
    return TestDialogBoundary(dialog,[&]() -> INT_PTR {
        const auto result=OptionsProc(dialog,message,word,param);
        if(message==WM_INITDIALOG)SetTimer(dialog,920,30,nullptr);
        if(message==WM_TIMER && word==920) {
            KillTimer(dialog,920);const HWND page=g_OptionsTabs[WHITEBOARD_PAGE].hPage;
            require(TabCtrl_GetItemCount(GetDlgItem(dialog,IDC_TAB))==7 && IsWindow(page),"Whiteboard must be the seventh-tab layout with About last");
            require(SendDlgItemMessage(page,IDC_WHITEBOARD_HOTKEY,HKM_GETHOTKEY,0,0)==g_WhiteboardToggleKey,
                "Whiteboard shortcut control must initialize from its saved preference");
            require(GetDlgItemInt(page,IDC_WHITEBOARD_SPACING,nullptr,FALSE)==g_WhiteboardSpacing,
                "Whiteboard square-size control must initialize from its saved preference");
            RECT client{};GetClientRect(page,&client);
            for(int control:{IDC_WHITEBOARD_HOTKEY,IDC_WHITEBOARD_BACKGROUND_KEY,IDC_WHITEBOARD_BLACK,
                IDC_WHITEBOARD_SPACING,IDC_WHITEBOARD_OPACITY,IDC_WHITEBOARD_OPACITY_SPIN}) {
                RECT rectangle{};GetWindowRect(GetDlgItem(page,control),&rectangle);
                MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&rectangle),2);
                if(rectangle.right>client.right || rectangle.bottom>client.bottom)
                    std::cerr<<"Board control clipped: id="<<control<<" bounds="<<rectangle.left<<","<<rectangle.top<<","<<rectangle.right<<","<<rectangle.bottom
                        <<" page="<<client.right<<","<<client.bottom<<"\n";
                require(rectangle.left>=0&&rectangle.top>=0&&rectangle.right<=client.right&&rectangle.bottom<=client.bottom,
                    "Every configurable whiteboard control must fit fully inside the tab page");
            }
            SetDlgItemInt(page,IDC_WHITEBOARD_SPACING,48,FALSE);SetDlgItemInt(page,IDC_WHITEBOARD_OPACITY,24,FALSE);
            CheckRadioButton(page,IDC_WHITEBOARD_WHITE,IDC_WHITEBOARD_BLACK,IDC_WHITEBOARD_BLACK);
            if(g_BoardOptionStage==1) {
                SendDlgItemMessage(page,IDC_WHITEBOARD_HOTKEY,HKM_SETHOTKEY,(HOTKEYF_CONTROL<<8)|'7',0);
                SendDlgItemMessage(page,IDC_WHITEBOARD_BACKGROUND_KEY,HKM_SETHOTKEY,'V',0);
                SendMessage(dialog,WM_COMMAND,IDOK,0);
            }else SendMessage(dialog,WM_COMMAND,IDCANCEL,0);
        }
        return result;
    });
}

WhiteboardResults RunWhiteboardRegression(HWND host) {
    WhiteboardResults r{};
    const auto old=WhiteboardOptions();const BOOLEAN oldAnimate=g_AnimateZoom,oldLiveAnimate=g_AnimateLiveZoom;
    auto restore=zoomit::OnExit([&] {
        SendMessage(g_hWndMain,recovery::ResetMessage,0,0);
        g_WhiteboardToggleKey=old.toggleKey;g_WhiteboardBackgroundKey=old.backgroundKey;g_WhiteboardBlack=old.black;
        g_WhiteboardSpacing=old.spacing;g_WhiteboardOpacity=old.opacity;g_AnimateZoom=oldAnimate;g_AnimateLiveZoom=oldLiveAnimate;
        UnregisterAllHotkeys(g_hWndMain);g_BoardOptionStage=0;
    });
    g_WhiteboardToggleKey=(HOTKEYF_CONTROL<<8)|'6';g_WhiteboardBackgroundKey='C';g_WhiteboardBlack=0;g_WhiteboardSpacing=32;g_WhiteboardOpacity=16;
    g_AnimateZoom=g_AnimateLiveZoom=FALSE;
    if(captureWhiteboardOptions)DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,TestOptionsProc);
    // Real offscreen pixels validate both colors, opacity and DPI-dependent spacing.
    HDC reference=GetDC(nullptr);HDC dc=CreateCompatibleDC(reference);HBITMAP image=CreateCompatibleBitmap(reference,300,300);
    ReleaseDC(nullptr,reference);require(dc&&image,"Prepare the grid pixel fixture");const auto previous=SelectObject(dc,image);
    auto pixels=zoomit::OnExit([&]{SelectObject(dc,previous);DeleteObject(image);DeleteDC(dc);});
    for(bool black:{false,true})for(UINT dpi:{96u,144u,192u}) {
        auto settings=WhiteboardOptions();settings.black=black;const int spacing=MulDiv(32,dpi,96);
        require(zoomit::whiteboard::PaintGrid(dc,RECT{0,0,300,300},settings,dpi),"Render procedural board grid");
        require(GetPixel(dc,1,1)==(black?RGB(0,0,0):RGB(255,255,255)),"Board background must be the selected solid color");
        require(GetPixel(dc,spacing,1)==zoomit::whiteboard::GridColor(black,16),"Grid line must use the selected semitransparent appearance");
        require(GetPixel(dc,spacing+1,1)==(black?RGB(0,0,0):RGB(255,255,255)),"Grid line must remain one physical pixel wide");++r.gridCases;
    }
    // Real registered Ctrl+6, local C and Ctrl+arrows exercise the Windows keyboard route.
    UnregisterHotKey(g_hWndMain,WHITEBOARD_HOTKEY);
    require(RegisterHotKey(g_hWndMain,WHITEBOARD_HOTKEY,MOD_CONTROL,'6')!=FALSE,"Register the actual Ctrl+6 whiteboard shortcut");
    auto nativeKey=[&](WORD key,bool control=false) {
        INPUT events[4]{};unsigned count{};
        const auto append=[&](WORD vk,bool up){events[count].type=INPUT_KEYBOARD;events[count].ki.wVk=vk;events[count++].ki.dwFlags=up?KEYEVENTF_KEYUP:0;};
        if(control)append(VK_CONTROL,false);append(key,false);append(key,true);if(control)append(VK_CONTROL,true);
        require(SendInput(count,events,sizeof(INPUT))==count,"Inject only the test-owned keyboard gesture");pump(40);
    };
    ActivateTestHost(host);SetCursorPos(125,125);nativeKey('6',true);
    const auto boardFocusDeadline=GetTickCount64()+2500;
    while((!g_Whiteboard.Active() || GetForegroundWindow()!=g_Whiteboard.Window()) && GetTickCount64()<boardFocusDeadline)pump(5);
    if(!g_Whiteboard.Active() || GetForegroundWindow()!=g_Whiteboard.Window()) {
        wchar_t foregroundClass[128]{};GetClassNameW(GetForegroundWindow(),foregroundClass,_countof(foregroundClass));
        std::cerr<<"Native board activation: active="<<g_Whiteboard.Active()<<" board="<<g_Whiteboard.Window()<<" foreground="<<GetForegroundWindow()
            <<" options="<<hWndOptions<<" mode="<<SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0)<<" key="<<g_WhiteboardToggleKey
            <<" ctrl="<<GetAsyncKeyState(VK_CONTROL)<<" shift="<<GetAsyncKeyState(VK_SHIFT)<<" alt="<<GetAsyncKeyState(VK_MENU)<<"\n";
        std::wcerr<<L"Foreground class: "<<foregroundClass<<L"\n";
    }
    require(g_Whiteboard.Active()&&GetForegroundWindow()==g_Whiteboard.Window(),"Native Ctrl+6 must activate the board and own keyboard focus");++r.nativeCases;
    nativeKey('C');require(g_Whiteboard.SessionOptions().black==1,"Native local C must change the background");++r.nativeCases;
    nativeKey(VK_UP,true);require(g_Whiteboard.SessionOptions().spacing==36,"Native Ctrl+Up must enlarge squares");++r.nativeCases;
    nativeKey(VK_DOWN,true);require(g_Whiteboard.SessionOptions().spacing==32,"Native Ctrl+Down must reduce squares");++r.nativeCases;
    nativeKey(VK_ESCAPE);require(g_Whiteboard.Active()&&g_Whiteboard.HintVisible(),"Native Esc must show the hint without exiting the board");++r.nativeCases;
    nativeKey('6',true);require(!g_Whiteboard.Active(),"Native repeated Ctrl+6 must explicitly close the board");++r.nativeCases;
    UnregisterHotKey(g_hWndMain,WHITEBOARD_HOTKEY);

    auto mode=[] {return SendMessage(g_hWndMain,WM_TEST_QUERY_MODE,0,0);};
    auto open=[&] {ActivateTestHost(host);SetCursorPos(125,125);SendMessage(g_hWndMain,WM_HOTKEY,WHITEBOARD_HOTKEY,0);pump(15);
        require(g_Whiteboard.Active() && IsWindowVisible(g_Whiteboard.Window()) && !IsWindowVisible(g_hWndMain),"Ctrl+6 must open only the board background");};
    auto close=[&] {SendMessage(g_hWndMain,WM_HOTKEY,WHITEBOARD_HOTKEY,0);pump(10);
        require(!g_Whiteboard.Active() && !IsWindowVisible(g_hWndMain) && mode()==0,"The board toggle must close both board and any drawing canvas");};
    open();HWND board=g_Whiteboard.Window();
    const auto initial=g_Whiteboard.SessionOptions();
    SendMessage(board,WM_KEYDOWN,'C',0);require(g_Whiteboard.SessionOptions().black==1,"Local C must switch only the board background");++r.setupCases;
    SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA/2),0);
    require(g_Whiteboard.SessionOptions().spacing==32,"Partial Ctrl+wheel input must not lose granularity");
    SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA/2),0);
    require(g_Whiteboard.SessionOptions().spacing==36,"Two partial wheel inputs must equal one grid-size step");++r.setupCases;
    for(int i=0;i<80;++i)SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
    require(g_Whiteboard.SessionOptions().spacing==256,"Grid size must clamp at its maximum");
    for(int i=0;i<80;++i)SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,-WHEEL_DELTA),0);
    require(g_Whiteboard.SessionOptions().spacing==8,"Grid size must clamp at its minimum");++r.setupCases;
    for(int i=0;i<6;++i)SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
    SendMessage(board,WM_KEYDOWN,VK_ESCAPE,0);pump(10);
    const UINT_PTR expired=g_Whiteboard.HintTimerId();
    require(g_Whiteboard.Active()&&g_Whiteboard.HintVisible()&&expired,"Esc outside Draw must display a temporary exit instruction without closing");++r.hintCases;
    close();open();board=g_Whiteboard.Window();SendMessage(board,WM_KEYDOWN,VK_ESCAPE,0);pump(10);
    const UINT_PTR fresh=g_Whiteboard.HintTimerId();require(fresh&&fresh!=expired,"Each hint timer must have a fresh session id");
    SendMessage(board,WM_TIMER,expired,0);require(g_Whiteboard.HintVisible(),"An old hint timer must not hide the new board's instruction");++r.hintCases;
    // Mode-conflicting native requests remain ignored while the board owns the session.
    for(WPARAM key:{WPARAM(ZOOM_HOTKEY),WPARAM(LIVE_HOTKEY),WPARAM(BREAK_HOTKEY),WPARAM(LIVE_DRAW_HOTKEY)}) {
        const HWND current=g_Whiteboard.Window();SendMessage(g_hWndMain,WM_HOTKEY,key,MAKELPARAM(MOD_CONTROL,'9'));pump(5);
        require(g_Whiteboard.Window()==current && !IsWindowVisible(g_hWndLiveZoom)&&!IsWindowVisible(g_hWndMain),"Other mode shortcuts must not replace an active board");++r.policyCases;
    }
    for(int cycle=0;cycle<5;++cycle) {
        const auto setup=g_Whiteboard.SessionOptions();
        SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(15);
        require((mode()&3)==3 && g_Whiteboard.Active()&&g_Whiteboard.InputBlocked(),"Draw shortcut must enter the ordinary Draw engine over the board");
        require(!g_Whiteboard.HintVisible(),"The board's exit instruction must be hidden before Draw captures its background");
        SendMessage(board,WM_KEYDOWN,'C',0);SendMessage(board,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),0);
        require(g_Whiteboard.SessionOptions().black==setup.black && g_Whiteboard.SessionOptions().spacing==setup.spacing,"Setup changes must be blocked for the duration of Draw");
        SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(160,150));
        for(int x=161;x<210;++x)SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,150));
        SendMessage(g_hWndMain,WM_LBUTTONUP,0,MAKELPARAM(210,150));
        SendMessage(g_hWndMain,WM_RBUTTONDOWN,0,0);pump(5);
        require((mode()&16)!=0,"Right click must retain the ordinary Draw suspension behavior");
        SendMessage(g_hWndMain,WM_KEYDOWN,VK_ESCAPE,0);pump(40);
        require(mode()==0 && g_Whiteboard.Active()&&!g_Whiteboard.InputBlocked() && !IsWindowVisible(g_hWndMain),"Esc must release Draw and its annotations while retaining an editable board");
        require(SendMessage(g_hWndMain,WM_TEST_QUERY_CANVAS,0,0)==0,"Esc must release the annotated Draw bitmap, not retain it behind an editable grid");
        require(GetForegroundWindow()==g_Whiteboard.Window(),"After Draw, keyboard focus must return to the board");
        SendMessage(board,WM_KEYDOWN,'C',0);
        require(g_Whiteboard.SessionOptions().black!=setup.black,"Background setup must become available again after Esc leaves Draw");++r.drawCycles;
    }
    SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(10);
    SendMessage(g_hWndMain,WM_LBUTTONDOWN,0,MAKELPARAM(160,150));SendMessage(g_hWndMain,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(170,150));
    require((mode()&4)!=0,"Prepare an active stroke before explicit whiteboard exit");
    close();require(GetCapture()!=g_hWndMain,"Explicit board exit must release an in-progress drawing gesture");++r.policyCases;
    // Configuration is initialized and cancel keeps all board preferences unchanged.
    const auto beforeCancel=WhiteboardOptions();g_BoardOptionStage=0;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,WhiteboardOptionsProbe);
    require(WhiteboardOptions().spacing==beforeCancel.spacing && WhiteboardOptions().black==beforeCancel.black,"Options Cancel must leave board preferences unchanged");++r.optionsCases;
    const bool savedTestMode=g_TestMode;g_TestMode=false;g_BoardOptionStage=1;
    DialogBox(g_hInstance,L"OPTIONS",g_hWndMain,WhiteboardOptionsProbe);g_TestMode=savedTestMode;
    require(g_WhiteboardToggleKey==((HOTKEYF_CONTROL<<8)|'7') && g_WhiteboardBackgroundKey=='V' &&
        g_WhiteboardSpacing==48&&g_WhiteboardOpacity==24&&g_WhiteboardBlack==1,"Options OK must apply configurable board shortcuts and appearance");++r.optionsCases;
    g_WhiteboardSpacing=32;reg.ReadRegSettings(RegSettings);require(g_WhiteboardSpacing==48,"Board options must persist through the isolated real registry path");++r.optionsCases;
    ActivateTestHost(host);SetCursorPos(125,125);nativeKey('7',true);
    require(g_Whiteboard.Active() && g_Whiteboard.SessionOptions().spacing==48 && g_Whiteboard.SessionOptions().black==1,
        "The user-configured native shortcut must open the configured background and grid");
    nativeKey('V');require(g_Whiteboard.SessionOptions().black==0,"The configured local background key must replace C");
    nativeKey('7',true);require(!g_Whiteboard.Active(),"The same configured shortcut must close the board");++r.nativeCases;
    // Validation makes malformed recovery state unusable without changing normal modes.
    for(recovery::Mode kind:{recovery::Mode::Whiteboard,recovery::Mode::WhiteboardDraw}) {
        recovery::State value{};value.mode=kind;value.monitor={0,0,300,300};value.boardSpacing=32;value.boardOpacity=16;value.sequence=1;
        value.checksum=recovery::Hash(&value,sizeof(value));require(recovery::ValidState(value),"Valid board recovery metadata must be accepted");
        value.boardSpacing=3;value.checksum=0;value.checksum=recovery::Hash(&value,sizeof(value));
        require(!recovery::ValidState(value),"Malformed grid recovery metadata must be rejected");++r.recoveryValidationCases;
    }
    // Repeated open/hint/Draw/close owns no persistent full-screen board image.
    open();close();pump(10);
    r.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);r.userBefore=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    for(int i=0;i<20;++i) {
        open();SendMessage(g_Whiteboard.Window(),WM_KEYDOWN,VK_ESCAPE,0);pump(5);
        if(i%2==0){SendMessage(g_hWndMain,WM_HOTKEY,DRAW_HOTKEY,0);pump(5);}
        close();++r.resourceCycles;
    }
    pump(30);r.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);r.userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    require(r.gdiAfter<=r.gdiBefore && r.userAfter<=r.userBefore+1,"Repeated board sessions must release fonts, windows and ordinary Draw resources");
    return r;
}
void PrintWhiteboardResults(const WhiteboardResults& r) {
    std::cout<<"{\"passed\":true,\"whiteboard\":true,\"grid_cases\":"<<r.gridCases<<",\"native_cases\":"<<r.nativeCases<<",\"setup_cases\":"<<r.setupCases
        <<",\"draw_cycles\":"<<r.drawCycles<<",\"policy_cases\":"<<r.policyCases<<",\"hint_cases\":"<<r.hintCases
        <<",\"options_cases\":"<<r.optionsCases<<",\"recovery_validation_cases\":"<<r.recoveryValidationCases
        <<",\"resource_cycles\":"<<r.resourceCycles<<",\"gdi_before\":"<<r.gdiBefore<<",\"gdi_after\":"<<r.gdiAfter
        <<",\"user_before\":"<<r.userBefore<<",\"user_after\":"<<r.userAfter<<"}\n";
}
