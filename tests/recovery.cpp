#include "../src/Recovery.h"
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <limits>
namespace fs=std::filesystem;
struct Environment {
    std::wstring name,previous;bool existed{};
    Environment(const wchar_t* key,const wchar_t* value):name(key){
        wchar_t buffer[32768];const DWORD length=GetEnvironmentVariableW(key,buffer,_countof(buffer));
        existed=length!=0;if(existed&&length<_countof(buffer))previous.assign(buffer,length);
        SetEnvironmentVariableW(key,value);
    }
    ~Environment(){SetEnvironmentVariableW(name.c_str(),existed?previous.c_str():nullptr);}
};
struct NativeProcess {
    HANDLE job{};PROCESS_INFORMATION process{};
    ~NativeProcess(){
        if(process.hThread)CloseHandle(process.hThread);
        if(process.hProcess&&WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){
            TerminateProcess(process.hProcess,ERROR_CANCELLED);WaitForSingleObject(process.hProcess,1000);
        }
        if(job)CloseHandle(job);if(process.hProcess)CloseHandle(process.hProcess);
    }
};
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct MapFixture {
    HANDLE map{},canvas{};recovery::Shared* shared{};BYTE* pixels{};HANDLE fault{},ack{};wchar_t token[40]{};
    MapFixture(){try{
        GUID guid{};Require(SUCCEEDED(CoCreateGuid(&guid)),"GUID");wchar_t text[40];StringFromGUID2(guid,text,40);
        memcpy(token,text+1,36*sizeof(wchar_t));token[36]=0;wchar_t name[160];
        recovery::ObjectName(name,160,token,L"control");map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(recovery::Shared),name);
        shared=static_cast<recovery::Shared*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(recovery::Shared)));
        recovery::ObjectName(name,160,token,L"canvas");canvas=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE|SEC_RESERVE,0,recovery::CanvasCapacity*2,name);
        pixels=static_cast<BYTE*>(MapViewOfFile(canvas,FILE_MAP_ALL_ACCESS,0,0,recovery::CanvasCapacity*2));
        recovery::ObjectName(name,160,token,L"fault");fault=CreateEventW(nullptr,TRUE,FALSE,name);
        recovery::ObjectName(name,160,token,L"ack");ack=CreateEventW(nullptr,TRUE,FALSE,name);
        Require(shared&&pixels&&fault&&ack,"Shared mappings and events");
        *shared={};shared->magic=recovery::ProtocolMagic;shared->version=recovery::ProtocolVersion;shared->supervisor=GetCurrentProcessId();
    }
    catch(...){Cleanup();throw;}}
    void Cleanup(){if(shared)UnmapViewOfFile(shared);if(pixels)UnmapViewOfFile(pixels);for(HANDLE h:{map,canvas,fault,ack})if(h)CloseHandle(h);shared=nullptr;pixels=nullptr;map=canvas=fault=ack=nullptr;}
    ~MapFixture(){Cleanup();}
};
struct Bitmap {
    HDC dc{};HBITMAP image{};HGDIOBJ old{};
    Bitmap(int width,int height){try{
        HDC screen=GetDC(nullptr);dc=CreateCompatibleDC(screen);image=CreateCompatibleBitmap(screen,width,height);ReleaseDC(nullptr,screen);
        if(dc&&image)old=SelectObject(dc,image);Require(dc&&image&&old&&old!=HGDI_ERROR,"Test bitmap");
    }
    catch(...){Cleanup();throw;}}
    void Cleanup(){if(dc&&old&&old!=HGDI_ERROR)SelectObject(dc,old);if(dc)DeleteDC(dc);if(image)DeleteObject(image);dc=nullptr;image=nullptr;old=nullptr;}
    ~Bitmap(){Cleanup();}
};
std::string Read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
size_t Reports(const fs::path& folder,const wchar_t* extension){
    size_t count=0;for(auto& file:fs::directory_iterator(folder))
        if(file.path().filename().wstring().find(L"ZoomItCustom-")==0&&file.path().extension()==extension)++count;
    return count;
}
struct NativeResult{DWORD exit{};std::string output;double milliseconds{};size_t reports{},dumps{};};
NativeResult Run(const fs::path& binaries,const fs::path& root,const wchar_t* scenario,bool supervised=true){
    const fs::path folder=root/scenario;fs::create_directories(folder);
    const fs::path resultPath=folder/L"result.txt";
    Environment testScenario(L"ZOOMIT_TEST_SCENARIO",scenario);
    Environment testLogs(L"ZOOMIT_TEST_LOG_DIRECTORY",folder.c_str());
    Environment testResult(L"ZOOMIT_TEST_RESULT_PATH",resultPath.c_str());
    const wchar_t* reporterMode=wcscmp(scenario,L"reporter-hung")==0?L"hung":
        wcscmp(scenario,L"reporter-fail")==0?L"fail":
        wcscmp(scenario,L"reporter-launch-fail")==0?L"launch-fail":nullptr;
    Environment reporter(L"ZOOMIT_TEST_REPORTER_MODE",reporterMode);
    const auto worker=binaries/L"ZoomItCrashWorker.exe";
    const auto executable=supervised?binaries/L"ZoomItTestSupervisor.exe":worker;
    std::wstring command=L"\""+executable.wstring()+L"\"";
    if(supervised)command+=L" --test-worker \""+worker.wstring()+L"\"";
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;
    NativeProcess owned;auto& process=owned.process;
    owned.job=CreateJobObjectW(nullptr,nullptr);HANDLE job=owned.job;JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Test job");
    const auto before=std::chrono::steady_clock::now();
    Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&start,&process),"Launch native test");
    Require(AssignProcessToJobObject(job,process.hProcess)!=FALSE,"Isolate native test processes");
    Require(ResumeThread(process.hThread)!=DWORD(-1),"Resume native test");CloseHandle(process.hThread);process.hThread=nullptr;
    const DWORD wait=WaitForSingleObject(process.hProcess,20000);
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);
    Require(wait==WAIT_OBJECT_0,"Native test timeout");
    NativeResult result{code,Read(resultPath),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count(),
                        Reports(folder,L".txt"),Reports(folder,L".dmp")};
    std::cout<<"case=";for(const wchar_t* at=scenario;*at;++at)std::cout<<static_cast<char>(*at);
    std::cout<<" exit="<<result.exit<<" ms="<<result.milliseconds<<" reports="<<result.reports<<" dumps="<<result.dumps
             <<" outcome="<<result.output<<std::flush;
    return result;
}
int main(int argc,char** argv){
    const bool checkpointOnly=argc==2&&strcmp(argv[1],"--checkpoint-only")==0;
    try{
        wchar_t executable[MAX_PATH];GetModuleFileNameW(nullptr,executable,MAX_PATH);
        const fs::path binaries=fs::path(executable).parent_path();
        const fs::path root=binaries/(L"recovery-results-"+std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        Environment testLogs(L"ZOOMIT_TEST_LOG_DIRECTORY",root.c_str());
        const DWORD beforeGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        double fullHdMs{},ultraHdMs{},metadataMs{};uint64_t copiedBytes{};
        {
            MapFixture fixture;recovery::Client client;Require(client.Initialize(fixture.token),"Attach checkpoint client");
            Bitmap source(64,64),destination(64,64);
            recovery::State state{};state.mode=recovery::Mode::Draw;state.monitor={0,0,64,64};state.zoom=2;state.penWidth=5;state.rootPenWidth=5;
            SetPixel(source.dc,32,48,RGB(21,91,201));client.Commit(state,source.dc,nullptr,nullptr,true);
            recovery::State first{};Require(recovery::Latest(*fixture.shared,first),"First checkpoint");
            Require(client.RestoreCanvas(first,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),"Restore exact canvas");
            SetPixel(source.dc,32,48,RGB(201,91,21));client.Commit(state,source.dc,nullptr,nullptr,true);
            recovery::State second{};Require(recovery::Latest(*fixture.shared,second)&&second.sequence>first.sequence,"Second checkpoint");
            const auto oldFirst=fixture.shared->states[0],oldSecond=fixture.shared->states[1];
            fixture.shared->states[0].sequence=fixture.shared->states[1].sequence=0;
            client.PrepareRecoveryForTest();recovery::State completed{};
            Require(client.TakeRecovery(completed)&&completed.canvasSequence==second.canvasSequence&&
                client.RestoreCanvas(completed,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(201,91,21),
                "Completed image pair recovers even if both rolling metadata slots are torn");
            client.FinishRecovery();fixture.shared->states[0]=oldFirst;fixture.shared->states[1]=oldSecond;
            fixture.shared->states[second.sequence%2].checksum^=1;
            recovery::State fallback{};Require(recovery::Latest(*fixture.shared,fallback)&&fallback.sequence==first.sequence,"Corrupt state falls back");
            Require(client.RestoreCanvas(fallback,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),"Previous canvas survives interrupted publication");
            // Both rolling metadata slots now point at B; the retained pair for A must still work.
            for(unsigned i=0;i<100;++i){state.color=i;client.Commit(state);}
            Require(recovery::Latest(*fixture.shared,second)&&second.canvasSlot!=first.canvasSlot,"Latest metadata references B");
            fixture.pixels[static_cast<size_t>(second.canvasSlot)*recovery::CanvasCapacity]^=1;
            client.PrepareRecoveryForTest();
            Require(client.TakeRecovery(fallback)&&fallback.sequence==first.sequence&&fallback.color==first.color,
                "Corrupt image after metadata rollover restores the previous paired state");
            Require(client.RestoreCanvas(fallback,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),
                "Previous exact canvas survives metadata rollover");
            client.FinishRecovery();client.Commit(state);
            Require(recovery::Latest(*fixture.shared,second)&&second.canvasSlot==first.canvasSlot,
                "Metadata after fallback references the image actually restored");
            fixture.pixels[static_cast<size_t>(fallback.canvasSlot)*recovery::CanvasCapacity]^=1;
            Require(!client.RestoreCanvas(fallback,destination.dc),"Corrupt canvas rejected");
            for(unsigned i=0;i<100;++i)client.Commit(state,source.dc,nullptr,nullptr,true);
            Bitmap fullHd(1920,1080);state.monitor={0,0,1920,1080};
            const auto fullStart=std::chrono::steady_clock::now();
            for(unsigned i=0;i<10;++i)client.Commit(state,fullHd.dc,nullptr,nullptr,true);
            fullHdMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-fullStart).count()/10;
            Bitmap ultraHd(3840,2160);state.monitor={-3840,-2160,0,0};
            const auto ultraStart=std::chrono::steady_clock::now();
            for(unsigned i=0;i<6;++i)client.Commit(state,ultraHd.dc,nullptr,nullptr,true);
            ultraHdMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-ultraStart).count()/6;
            const auto beforeMetadata=client.Statistics();const auto metadataStart=std::chrono::steady_clock::now();
            for(unsigned i=0;i<10000;++i){state.color=i;client.Commit(state);}
            metadataMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-metadataStart).count()/10000;
            const auto afterMetadata=client.Statistics();copiedBytes=afterMetadata.hashedBytes;
            Require(afterMetadata.canvasCopies==beforeMetadata.canvasCopies&&
                afterMetadata.hashedBytes==beforeMetadata.hashedBytes&&
                afterMetadata.metadataCommits==beforeMetadata.metadataCommits+10000,
                "Metadata updates neither copy nor hash canvas pixels");
            Require(recovery::Latest(*fixture.shared,second)&&second.canvasWidth==3840&&second.canvasHeight==2160,
                "4K checkpoint with negative monitor coordinates remains valid");
            std::cout<<"checkpoint_1080p_average_ms="<<fullHdMs<<" checkpoint_4k_average_ms="<<ultraHdMs
                     <<" metadata_average_us="<<metadataMs*1000<<"\n";
        }
        Require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==beforeGdi,"No GDI handles leaked by checkpoint cycles");
        Require(Reports(root,L".txt")==0,"Healthy checkpoints must create no reports");
        unsigned failuresTested=0;
        for(auto failure:{recovery::TestFailure::Allocation,recovery::TestFailure::CreateDC,
                         recovery::TestFailure::CreateBitmap,recovery::TestFailure::SelectBitmap,
                         recovery::TestFailure::Capture,recovery::TestFailure::Patch,
                         recovery::TestFailure::Flush,recovery::TestFailure::Publication}){
            const auto folder=root/(L"partial-"+std::to_wstring(++failuresTested));fs::create_directories(folder);
            Environment failureLogs(L"ZOOMIT_TEST_LOG_DIRECTORY",folder.c_str());
            {
                MapFixture fixture;recovery::Client client;Require(client.Initialize(fixture.token),"Attach failure client");
                Bitmap source(64,64),destination(64,64),patch(8,8);RECT patchRect{20,20,28,28};
                recovery::State state{};state.mode=recovery::Mode::Draw;state.monitor={0,0,64,64};
                SetPixel(source.dc,32,48,RGB(21,91,201));client.Commit(state,source.dc,nullptr,nullptr,true);
                recovery::State before{};Require(recovery::Latest(*fixture.shared,before),"Complete checkpoint before failure");
                SetPixel(source.dc,32,48,RGB(201,91,21));
                for(unsigned i=0;i<20;++i){client.FailNext(failure);client.Commit(state,source.dc,patch.dc,&patchRect,true);}
                recovery::State after{};Require(recovery::Latest(*fixture.shared,after)&&after.sequence==before.sequence,
                    "Partial checkpoint never replaces the completed current checkpoint");
                Require(client.RestoreCanvas(after,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),
                    "Completed current pixels survive every failure stage");
                client.Commit(state,source.dc,nullptr,nullptr,true);
                Require(recovery::Latest(*fixture.shared,after)&&after.sequence>before.sequence&&
                    client.RestoreCanvas(after,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(201,91,21),
                    "Checkpoint capture succeeds immediately after a transient failure");
            }
            Require(Reports(folder,L".txt")==1,"Repeated checkpoint failure produces one report per session and condition");
            Require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==beforeGdi,"Failure paths release every GDI handle");
        }
        {
            const auto folder=root/L"unrecoverable-size";fs::create_directories(folder);
            Environment failureLogs(L"ZOOMIT_TEST_LOG_DIRECTORY",folder.c_str());MapFixture fixture;recovery::Client client;
            Require(client.Initialize(fixture.token),"Attach oversize checkpoint client");Bitmap smallImage(64,64);
            recovery::State state{};state.mode=recovery::Mode::Draw;state.monitor={0,0,8192,8192};
            for(unsigned i=0;i<100;++i)client.Commit(state,smallImage.dc,nullptr,nullptr,true);
            recovery::State safe{};Require(recovery::Latest(*fixture.shared,safe)&&safe.mode==recovery::Mode::Idle&&
                !safe.canvasSequence,"Absent recoverable canvas publishes safe desktop metadata");
            client.Commit(state);Require(recovery::Latest(*fixture.shared,safe)&&safe.mode==recovery::Mode::Idle,
                "Metadata-only update cannot invent an oversize recoverable drawing");
            Require(Reports(folder,L".txt")==1,"Oversize checkpoints do not create an error-log storm");
            client.SetSession(2);client.Commit(state,smallImage.dc,nullptr,nullptr,true);
            Require(Reports(folder,L".txt")==2,"A new session can report the condition again");
            state.mode=recovery::Mode::LiveZoom;state.monitor={0,0,1920,1080};state.liveZoom=2;
            state.source={-1,0,1919,1080};client.Commit(state);
            Require(recovery::Latest(*fixture.shared,safe)&&safe.mode==recovery::Mode::Idle,
                "Live source outside the checkpoint monitor is rejected before publication");
        }
        recovery::State invalid{};invalid.sequence=1;invalid.zoom=std::numeric_limits<float>::quiet_NaN();
        invalid.checksum=recovery::Hash(&invalid,sizeof(invalid));Require(!recovery::ValidState(invalid),"NaN state rejected");
        Require(!recovery::ValidToken(L"../other-process"),"Invalid session identifier rejected");
        const fs::path reportFolder=root/L"retention";fs::create_directories(reportFolder);
        recovery::Diagnostics diagnostic{};diagnostic.process=GetCurrentProcessId();diagnostic.error=ERROR_NOT_ENOUGH_MEMORY;
        wcscpy_s(diagnostic.operation,L"Prova di memoria insufficiente");
        for(int i=0;i<10;++i){Require(recovery::WriteReport(diagnostic,nullptr,nullptr,L"Prova",reportFolder.c_str()),"Write serious report");Sleep(1);}
        Require(Reports(reportFolder,L".txt")==8,"Bounded report retention");
        const auto blocker=root/L"blocked";{std::ofstream file(blocker);file<<"file";}
        Require(!recovery::WriteReport(diagnostic,nullptr,nullptr,L"Prova",(blocker/L"Logs").c_str()),"Unavailable log path must fail without crashing");
        if(checkpointOnly){
            std::cout<<"{\"checkpoint_passed\":true,\"checkpoint_cycles\":118,\"partial_failure_cases\":"<<failuresTested
                     <<",\"metadata_updates\":10101,\"checkpoint_1080p_ms\":"<<fullHdMs
                     <<",\"checkpoint_4k_ms\":"<<ultraHdMs<<",\"metadata_us\":"<<metadataMs*1000
                     <<",\"hashed_bytes\":"<<copiedBytes<<",\"gdi_leaks\":0,\"healthy_reports\":0}\n";
            return 0;
        }
        auto normal=Run(binaries,root,L"normal");
        Require(normal.exit==0&&normal.reports==0&&normal.dumps==0&&normal.output=="boot\nnormal\n","Voluntary exit has no restart and no log");
        auto serious=Run(binaries,root,L"serious");
        Require(serious.exit==0&&serious.reports==1&&serious.dumps==0,"Serious error only creates report");
        auto allocation=Run(binaries,root,L"allocation");
        Require(allocation.exit==0&&allocation.output.find("allocation-ok")!=std::string::npos&&allocation.reports==1,"Failed graphics allocation keeps app idle");
        auto exception=Run(binaries,root,L"exception");
        Require(exception.exit==0&&exception.reports==1&&exception.dumps==0&&exception.output=="boot\nexception-ok\n","Caught runtime exception returns to usable idle without restart");
        auto resume=Run(binaries,root,L"resume");
        Require(resume.exit==0&&resume.reports==0&&resume.dumps==0&&resume.output=="boot\nresume-ok\n","Resume safely closes obsolete surfaces without error log");
        double maximum=0;
        for(const wchar_t* scenario:{L"zoom",L"draw",L"live",L"break",L"layered",L"frozen",L"forced",L"corrupt-frame",L"corrupt-metadata",L"whiteboard",L"whiteboard-draw"}){
            const auto result=Run(binaries,root,scenario);
            Require(result.exit==0&&result.output.find("recovered-ok")!=std::string::npos&&
                result.output.find("recovered-failed")==std::string::npos&&result.output.find("corruption-failed")==std::string::npos,"Restore implemented modes after process crash");
            if(wcscmp(scenario,L"corrupt-metadata")==0)
                Require(result.output.find("metadata-no-copy-ok")!=std::string::npos&&
                    result.output.find("metadata-no-copy-failed")==std::string::npos,
                    "Thirty real drawing colour changes publish metadata without copying or hashing the canvas");
            Require(result.reports==1,"One crash report per crash");
            if(wcscmp(scenario,L"forced")!=0)Require(result.dumps==1,"Supervised crash produces minidump");
            maximum=(std::max)(maximum,result.milliseconds);
        }
        for(const wchar_t* scenario:{L"fault-exit",L"reporter-fail",L"reporter-launch-fail",L"reporter-hung"}){
            const auto result=Run(binaries,root,scenario);
            Require(result.exit==0&&result.reports==1&&result.output=="boot\ncrash\nboot\nrecovered-ok\n",
                "Reporter failure and simultaneous notifications recover with one crash report");
            Require(result.milliseconds<12000,"Failed or stuck reporter does not block recovery indefinitely");
            const auto folder=root/scenario;bool clearCrash=false;
            for(const auto& file:fs::directory_iterator(folder))if(file.path().extension()==L".txt"){
                const auto report=Read(file.path());if(report.find("Tipo: CRASH")!=std::string::npos)clearCrash=true;
            }
            Require(clearCrash,"Fallback crash report identifies the crash clearly");
            maximum=(std::max)(maximum,result.milliseconds);
        }
        auto twice=Run(binaries,root,L"twice");
        Require(twice.exit==0&&twice.output=="boot\ncrash\nboot\nrecovered-ok\ncrash\nboot\nrecovered-ok\n"&&twice.reports==2,"Repeated crash switches to safe startup");
        auto loop=Run(binaries,root,L"loop");
        Require(loop.exit==1&&loop.output=="boot\ncrash\nboot\ncrash\nboot\ncrash\n"&&loop.reports==3,"Crash loop stops after two retries");
        auto standalone=Run(binaries,root,L"standalone",false);
        Require(standalone.exit!=0&&standalone.reports==1&&standalone.output=="boot\ncrash\n","Standalone crash logs without restarting");
        std::cout<<"{\"recovery_passed\":true,\"native_cases\":23,\"maximum_restart_case_ms\":"<<maximum
                 <<",\"checkpoint_cycles\":118,\"partial_failure_cases\":"<<failuresTested
                 <<",\"metadata_updates\":10101,\"checkpoint_1080p_ms\":"<<fullHdMs
                 <<",\"checkpoint_4k_ms\":"<<ultraHdMs<<",\"metadata_us\":"<<metadataMs*1000
                 <<",\"hashed_bytes\":"<<copiedBytes<<",\"gdi_leaks\":0,\"healthy_reports\":0}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"RECOVERY TEST FAILURE: "<<error.what()<<"\n";return 1;}
}
