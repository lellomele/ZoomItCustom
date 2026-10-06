#pragma once
struct SelectionResults {size_t completed{},cancelled{},regionFailures{},clipFailures{},queryFailures{},cycles{};DWORD gdiBefore{},gdiAfter{},userBefore{},userAfter{};};
namespace selection_test {
enum class Action { Complete, Cancel, CaptureLoss, Stop, PreviewFailure, BorderFailure, ClipFailure };
inline SelectRectangle* current{};inline Action action{};inline unsigned failStage{};inline HWND host{};inline RECT previousClip{};
void CALLBACK Drive(HWND,UINT,UINT_PTR timer,DWORD) noexcept {
    if(!current || !current->TestWindow())return;
    KillTimer(nullptr,timer);const HWND window=current->TestWindow();
    TestCallbackBoundary(window,WM_KEYDOWN,[&] {
        auto releaseFixtureCapture=zoomit::OnExit([]{if(GetCapture()==host)ReleaseCapture();});
        if(action==Action::Cancel){current->Stop();return;}
        if(action==Action::ClipFailure) {
            RECT actual{};require(!current->TestClipped() && GetClipCursor(&actual) && EqualRect(&actual,&previousClip),"Failed clip read/set must not mark or alter the user's previous clipping rectangle");
        }
        SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(100,100));
        require(!current->TestDragging() && !current->TestCancelled(),"A mouse-up without an owned drag must not complete or cancel selection");
        if(action==Action::PreviewFailure)zoomit::runtime::FailAt(zoomit::runtime::Api::Graphics,failStage);
        SendMessage(window,WM_LBUTTONDOWN,0,MAKELPARAM(20,20));
        if(action==Action::PreviewFailure) {
            require(zoomit::runtime::Calls(zoomit::runtime::Api::Graphics)>=failStage,"Region preview failure must reach its injected operation");zoomit::runtime::Clear();
            require(current->TestCancelled() && !current->TestWindow() && GetCapture()!=window,"Region preview failure must cancel without leaking capture or an opaque overlay");return;
        }
        require(current->TestDragging() && GetCapture()==window,"A real selection drag must own capture");
        if(action==Action::CaptureLoss){SetCapture(host);require(GetCapture()==host,"The fixture must take capture away from selection");}
        else if(action==Action::Stop)current->Stop();
        else {
            SendMessage(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(100,100));
            if(action==Action::BorderFailure)zoomit::runtime::FailAt(zoomit::runtime::Api::Graphics,failStage);
            SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(100,100));
        }
        if(action==Action::CaptureLoss || action==Action::Stop || action==Action::BorderFailure) {
            zoomit::runtime::Clear();SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(100,100));
            require(current->TestCancelled() && !current->TestWindow() && !current->TestDragging(),"Capture loss, stop and region failure must reject late mouse-up events");
        } else {
            const RECT selected=current->SelectedRect();
            require(selected.left==20 && selected.top==20 && selected.right==101 && selected.bottom==101,"A normal drag must preserve its inclusive selected pixel rectangle");
            require(!current->TestDragging() && GetCapture()!=window,"Completing selection must release only its owned capture");
        }
    });
}
inline bool Run(HWND owner,Action operation,unsigned stage=0) {
    SelectRectangle selection;current=&selection;action=operation;failStage=stage;host=owner;
    require(GetClipCursor(&previousClip)!=FALSE,"Read original clipping rectangle for isolated selection case");
    auto cleanup=zoomit::OnExit([&]{selection.Stop();current=nullptr;zoomit::runtime::Clear();if(GetCapture()==owner)ReleaseCapture();});
    if(operation==Action::ClipFailure)zoomit::runtime::FailAt(zoomit::runtime::Api::Cursor,stage);
    const UINT_PTR timer=SetTimer(nullptr,0,15,Drive);require(timer!=0,"Schedule owned selection driver");
    auto cancelTimer=zoomit::OnExit([&]{KillTimer(nullptr,timer);});
    const bool selected=selection.Start(owner);RethrowTestCallbackFailure();selection.Stop();zoomit::runtime::Clear();
    RECT restored{};require(GetClipCursor(&restored) && EqualRect(&restored,&previousClip),"Every selection outcome must restore the original clipping rectangle");
    return selected;
}
}
SelectionResults RunSelectionRegression(HWND host) {
    SelectionResults result{};
    SendMessage(g_hWndMain,recovery::ResetMessage,0,0);pump(20);ActivateTestHost(host);SetCursorPos(125,125);
    struct ResourceStep {const char* phase;unsigned stage;DWORD gdi,user;};std::vector<ResourceStep> resourceSteps;
    auto record=[&](const char* phase,unsigned stage=0){resourceSteps.push_back({phase,stage,GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)});};
    record("initial");
    require(!selection_test::Run(host,selection_test::Action::Cancel),"Warm-up selection must cancel cleanly");record("warm-cancel");
    require(selection_test::Run(host,selection_test::Action::Complete),"Warm-up must exercise completed border rendering and region transfer");record("warm-complete-first");
    require(selection_test::Run(host,selection_test::Action::Complete),"Repeated completed warm-up must exercise the same resource path");record("warm-complete-second");
    result.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);result.userBefore=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    require(selection_test::Run(host,selection_test::Action::Complete),"Normal selection must still complete");++result.completed;record("completed");
    for(auto action:{selection_test::Action::CaptureLoss,selection_test::Action::Stop,selection_test::Action::Cancel}) {
        require(!selection_test::Run(host,action),"Cancelled selection must never return stale pixels");++result.cancelled;record("cancelled",static_cast<unsigned>(action));
    }
    for(unsigned stage=1;stage<=4;++stage)for(auto action:{selection_test::Action::PreviewFailure,selection_test::Action::BorderFailure}) {
        require(!selection_test::Run(host,action,stage),"Failure to allocate, combine or install a selection region must cancel cleanly");++result.regionFailures;record(action==selection_test::Action::PreviewFailure ? "preview-failure" : "border-failure",stage);
    }
    for(unsigned stage:{2u,3u}) {require(selection_test::Run(host,selection_test::Action::ClipFailure,stage),"Selection must remain usable when optional cursor clipping fails");++result.clipFailures;record("clip-failure",stage);}
    for(auto api:{zoomit::runtime::Api::Cursor,zoomit::runtime::Api::Monitor}) {
        SelectRectangle selection;zoomit::runtime::FailAlways(api);
        require(!selection.Start(host) && !selection.TestWindow() && selection.TestCancelled(),"Failed cursor or monitor query must reject selection before creating a window");zoomit::runtime::Clear();++result.queryFailures;record("query-failure",static_cast<unsigned>(api));
    }
    {SelectRectangle selection;selection.MinSize(INT_MAX);require(selection.MinSize()==32768,"Selection minimum must reject overflow-sized values");selection.MinSize(-1);require(selection.MinSize()==1,"Selection minimum must reject negative values");++result.queryFailures;}
    for(unsigned stage=1;stage<=4;++stage) {
        SelectRectangle selection;zoomit::runtime::FailAt(zoomit::runtime::Api::Graphics,stage);
        require(!selection.Start(host,true) && !selection.TestWindow(),"Whole-monitor border failure must not leave a full-screen overlay");zoomit::runtime::Clear();++result.regionFailures;record("whole-monitor-failure",stage);
    }
    for(unsigned i=0;i<10;++i){require(!selection_test::Run(host,selection_test::Action::CaptureLoss),"Repeated selection capture loss must remain recoverable");++result.cycles;record("capture-loss-cycle",i);}
    result.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);result.userAfter=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    wchar_t diagnostic[2]{};const bool verbose=GetEnvironmentVariableW(L"ZOOMIT_TEST_SELECTION_RESOURCE_DIAGNOSTICS",diagnostic,2)!=0;
    const bool budget=result.gdiAfter<=result.gdiBefore && result.userAfter<=result.userBefore;
    if(!budget || verbose) {
        for(const auto& step:resourceSteps)std::cerr<<"Selection resource phase="<<step.phase<<" stage="<<step.stage<<" gdi="<<step.gdi<<" user="<<step.user<<"\n";
        std::cerr<<"Selection resource summary: baseline="<<result.gdiBefore<<","<<result.userBefore<<" final="<<result.gdiAfter<<","<<result.userAfter<<" capture="<<GetCapture()<<"\n";
    }
    require(budget,"Selections and failed region ownership transfers must not leak GDI or USER handles");return result;
}
void PrintSelectionResults(const SelectionResults& r) {
    std::cout<<"{\"passed\":true,\"selection_safety\":true,\"completed\":"<<r.completed<<",\"cancelled\":"<<r.cancelled
        <<",\"region_failure_cases\":"<<r.regionFailures<<",\"clip_failure_cases\":"<<r.clipFailures<<",\"query_validation_cases\":"<<r.queryFailures
        <<",\"cycles\":"<<r.cycles<<",\"gdi_before\":"<<r.gdiBefore<<",\"gdi_after\":"<<r.gdiAfter
        <<",\"user_before\":"<<r.userBefore<<",\"user_after\":"<<r.userAfter<<"}\n";
}
