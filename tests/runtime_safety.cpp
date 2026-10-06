#include "../src/RuntimeSafety.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <climits>
#include <limits>

namespace runtime=zoomit::runtime;
void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
struct HiddenFixture {
    HWND window{CreateWindowExW(0,L"STATIC",L"ZoomIt safety tests",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr)};
    HiddenFixture(){Require(window!=nullptr,"Create message-only timer fixture");}
    ~HiddenFixture(){if(window)DestroyWindow(window);}
};
int main() {
    try {
        size_t geometryCases{},timerCases{},faultCases{};
        Require(!runtime::ValidRect({0,0,0,10}) && !runtime::ValidRect({0,0,10,0}),"Reject empty display rectangles");
        Require(!runtime::ValidRect({10,10,0,0}),"Reject inverted display rectangles");
        Require(!runtime::ValidRect({LONG_MIN,0,LONG_MAX,100}),"Coordinate subtraction must not overflow");
        Require(runtime::ValidRect({-32768,-1000,0,31768}),"Accept negative origins and maximum supported dimensions");geometryCases+=4;
        for(LONG left:{-1920L,0L,2560L})for(LONG top:{-1080L,0L,1440L})for(LONG width:{1L,1280L,3840L,32768L}) {
            RECT monitor{left,top,left+width,top+2160};
            for(float factor:{1.0f,1.25f,3.75f,4.0f,32.0f}) {
                const LONG sourceWidth=(std::max)(1L,static_cast<LONG>(width/factor));
                const LONG sourceHeight=(std::max)(1L,static_cast<LONG>(2160/factor));
                const RECT source{left,top,left+sourceWidth,top+sourceHeight};
                Require(runtime::ValidViewport(source,monitor,factor),"Accept finite in-bounds views across monitor origins");
                RECT outside=source;outside.left=left-1;Require(!runtime::ValidViewport(outside,monitor,factor),"Reject left edge outside current monitor");
                outside=source;outside.bottom=monitor.bottom+1;Require(!runtime::ValidViewport(outside,monitor,factor),"Reject bottom edge outside current monitor");geometryCases+=3;
            }
            for(float invalid:{0.0f,0.99f,32.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
                Require(!runtime::ValidViewport(monitor,monitor,invalid),"Reject invalid zoom factors independently of valid geometry");++geometryCases;
            }
        }
        runtime::Clear();runtime::FailAt(runtime::Api::Graphics,2);
        Require(runtime::Permit(runtime::Api::Graphics) && !runtime::Permit(runtime::Api::Graphics) && runtime::Permit(runtime::Api::Graphics),"A numbered failure must affect exactly the requested call");
        Require(runtime::Calls(runtime::Api::Graphics)==3,"Numbered failure evidence counts real attempts");faultCases+=2;
        runtime::FailAlways(runtime::Api::Cursor);
        Require(!runtime::Permit(runtime::Api::Cursor) && !runtime::Permit(runtime::Api::Cursor),"Permanent fault injection must remain deterministic");++faultCases;
        runtime::Clear();Require(runtime::Calls(runtime::Api::Cursor)==0 && runtime::Permit(runtime::Api::Cursor),"Reset must remove all injected failures");++faultCases;
        HiddenFixture fixture;
        runtime::SessionTimers<4> timers;
        Require(timers.Role(0)==-1 && !timers.Id(0) && !timers.Start(nullptr,0,10),"Inactive and invalid timer identities cannot dispatch a role");++timerCases;
        for(unsigned role=0;role<4;++role) {
            const UINT_PTR first=timers.Start(fixture.window,role,60000);Require(first>=0x4000 && timers.Role(first)==static_cast<int>(role),"Each native timer must map to its own logical role");
            const UINT_PTR second=timers.Start(fixture.window,role,60000);Require(second!=first && timers.Role(first)==-1 && timers.Role(second)==static_cast<int>(role),"Rearming must invalidate queued messages carrying the prior identity");
            runtime::FailAt(runtime::Api::Timer,1);
            Require(!timers.Start(fixture.window,role,60000) && timers.Id(role)==second && timers.Role(second)==static_cast<int>(role),"Failure to replace a timer must preserve the existing active role");
            runtime::Clear();timers.Stop(role);Require(!timers.Id(role) && timers.Role(second)==-1,"Stopping must invalidate queued messages before later sessions");timerCases+=4;
        }
        Require(!timers.Start(fixture.window,4,10) && timers.Role(0x4000)==-1,"Reject out-of-range timer roles");++timerCases;
        for(unsigned cycle=0;cycle<100;++cycle) {
            const auto old=timers.Start(fixture.window,0,60000);timers.StopAll();const auto current=timers.Start(fixture.window,0,60000);
            Require(old && current && current!=old && timers.Role(old)==-1 && timers.Role(current)==0,"Session restart cannot accept a timer from an older session");timers.StopAll();++timerCases;
        }
        runtime::Clear();
        std::cout<<"{\"passed\":true,\"runtime_safety\":true,\"geometry_cases\":"<<geometryCases
            <<",\"timer_identity_cases\":"<<timerCases<<",\"fault_control_cases\":"<<faultCases<<",\"visible_windows\":0}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"SAFETY TEST FAILURE: "<<error.what()<<"\n";return 1;}
}
