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
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct MapFixture {
    HANDLE map{},canvas{};recovery::Shared* shared{};BYTE* pixels{};HANDLE fault{},ack{};wchar_t token[40]{};
    MapFixture(){
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
    ~MapFixture(){if(shared)UnmapViewOfFile(shared);if(pixels)UnmapViewOfFile(pixels);for(HANDLE h:{map,canvas,fault,ack})if(h)CloseHandle(h);}
};
struct Bitmap {
    HDC dc{};HBITMAP image{};HGDIOBJ old{};
    Bitmap(int width,int height){
        HDC screen=GetDC(nullptr);dc=CreateCompatibleDC(screen);image=CreateCompatibleBitmap(screen,width,height);ReleaseDC(nullptr,screen);
        if(dc&&image)old=SelectObject(dc,image);Require(dc&&image&&old,"Test bitmap");
    }
    ~Bitmap(){if(dc&&old)SelectObject(dc,old);if(dc)DeleteDC(dc);if(image)DeleteObject(image);}
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
    SetEnvironmentVariableW(L"ZOOMIT_TEST_SCENARIO",scenario);
    SetEnvironmentVariableW(L"ZOOMIT_TEST_LOG_DIRECTORY",folder.c_str());
    SetEnvironmentVariableW(L"ZOOMIT_TEST_RESULT_PATH",resultPath.c_str());
    const auto worker=binaries/L"ZoomItCrashWorker.exe";
    const auto executable=supervised?binaries/L"ZoomItTestSupervisor.exe":worker;
    std::wstring command=L"\""+executable.wstring()+L"\"";
    if(supervised)command+=L" --test-worker \""+worker.wstring()+L"\"";
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Test job");
    const auto before=std::chrono::steady_clock::now();
    Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&start,&process),"Launch native test");
    Require(AssignProcessToJobObject(job,process.hProcess)!=FALSE,"Isolate native test processes");
    ResumeThread(process.hThread);CloseHandle(process.hThread);
    const DWORD wait=WaitForSingleObject(process.hProcess,20000);
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);CloseHandle(job);
    Require(wait==WAIT_OBJECT_0,"Native test timeout");
    NativeResult result{code,Read(resultPath),std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count(),
                        Reports(folder,L".txt"),Reports(folder,L".dmp")};
    std::cout<<"case=";for(const wchar_t* at=scenario;*at;++at)std::cout<<static_cast<char>(*at);
    std::cout<<" exit="<<result.exit<<" ms="<<result.milliseconds<<" reports="<<result.reports<<" dumps="<<result.dumps
             <<" outcome="<<result.output<<std::flush;
    return result;
}
int main(){
    try{
        wchar_t executable[MAX_PATH];GetModuleFileNameW(nullptr,executable,MAX_PATH);
        const fs::path binaries=fs::path(executable).parent_path();
        const fs::path root=binaries/(L"recovery-results-"+std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        SetEnvironmentVariableW(L"ZOOMIT_TEST_LOG_DIRECTORY",root.c_str());
        const DWORD beforeGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        {
            MapFixture fixture;recovery::Client client;Require(client.Initialize(fixture.token),"Attach checkpoint client");
            Bitmap source(64,64),destination(64,64);
            recovery::State state{};state.mode=recovery::Mode::Draw;state.monitor={0,0,64,64};state.zoom=2;state.penWidth=5;state.rootPenWidth=5;
            SetPixel(source.dc,32,48,RGB(21,91,201));client.Commit(state,source.dc,nullptr,nullptr,true);
            recovery::State first{};Require(recovery::Latest(*fixture.shared,first),"First checkpoint");
            Require(client.RestoreCanvas(first,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),"Restore exact canvas");
            SetPixel(source.dc,32,48,RGB(201,91,21));client.Commit(state,source.dc,nullptr,nullptr,true);
            recovery::State second{};Require(recovery::Latest(*fixture.shared,second)&&second.sequence>first.sequence,"Second checkpoint");
            fixture.shared->states[second.sequence%2].checksum^=1;
            recovery::State fallback{};Require(recovery::Latest(*fixture.shared,fallback)&&fallback.sequence==first.sequence,"Corrupt state falls back");
            Require(client.RestoreCanvas(fallback,destination.dc)&&GetPixel(destination.dc,32,48)==RGB(21,91,201),"Previous canvas survives interrupted publication");
            fixture.pixels[static_cast<size_t>(fallback.canvasSlot)*recovery::CanvasCapacity]^=1;
            Require(!client.RestoreCanvas(fallback,destination.dc),"Corrupt canvas rejected");
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<100;++i)client.Commit(state,source.dc,nullptr,nullptr,true);
            std::cout<<"checkpoint_64x64_100_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
            Bitmap fullHd(1920,1080);state.monitor={0,0,1920,1080};
            const auto fullStart=std::chrono::steady_clock::now();
            for(unsigned i=0;i<10;++i)client.Commit(state,fullHd.dc,nullptr,nullptr,true);
            std::cout<<"checkpoint_1080p_average_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-fullStart).count()/10<<"\n";
        }
        Require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==beforeGdi,"No GDI handles leaked by checkpoint cycles");
        Require(Reports(root,L".txt")==0,"Healthy checkpoints must create no reports");
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
        for(const wchar_t* scenario:{L"zoom",L"draw",L"live",L"break",L"layered",L"frozen",L"forced",L"corrupt-frame"}){
            const auto result=Run(binaries,root,scenario);
            Require(result.exit==0&&result.output.find("recovered-ok")!=std::string::npos&&
                result.output.find("recovered-failed")==std::string::npos&&result.output.find("corruption-failed")==std::string::npos,"Restore implemented modes after process crash");
            Require(result.reports==1,"One crash report per crash");
            if(wcscmp(scenario,L"forced")!=0)Require(result.dumps==1,"Supervised crash produces minidump");
            maximum=(std::max)(maximum,result.milliseconds);
        }
        auto twice=Run(binaries,root,L"twice");
        Require(twice.exit==0&&twice.output=="boot\ncrash\nboot\nrecovered-ok\ncrash\nboot\nrecovered-ok\n"&&twice.reports==2,"Repeated crash switches to safe startup");
        auto loop=Run(binaries,root,L"loop");
        Require(loop.exit==1&&loop.output=="boot\ncrash\nboot\ncrash\nboot\ncrash\n"&&loop.reports==3,"Crash loop stops after two retries");
        auto standalone=Run(binaries,root,L"standalone",false);
        Require(standalone.exit!=0&&standalone.reports==1&&standalone.output=="boot\ncrash\n","Standalone crash logs without restarting");
        std::cout<<"{\"recovery_passed\":true,\"native_cases\":16,\"maximum_restart_case_ms\":"<<maximum
                 <<",\"checkpoint_cycles\":112,\"gdi_leaks\":0,\"healthy_reports\":0}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"RECOVERY TEST FAILURE: "<<error.what()<<"\n";return 1;}
}
