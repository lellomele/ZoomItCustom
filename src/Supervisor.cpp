#include "Recovery.h"
#include "version.h"
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <objbase.h>
#include <string>
#include <new>
namespace {
struct Handles {
    HANDLE control{},canvas{},fault{},ack{},instance{};
    recovery::Shared* shared{};
    ~Handles(){if(shared)UnmapViewOfFile(shared);for(HANDLE h:{control,canvas,fault,ack,instance})if(h)CloseHandle(h);}
};
bool Launch(const wchar_t* executable,wchar_t* command,PROCESS_INFORMATION& process) {
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    return CreateProcessW(executable,command,nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=FALSE;
}
int ReportProcess(const wchar_t* token,DWORD pid) {
    if(!recovery::ValidToken(token))return 1;
    wchar_t name[160];recovery::ObjectName(name,160,token,L"control");
    HANDLE map=OpenFileMappingW(FILE_MAP_READ,FALSE,name);if(!map)return 1;
    auto shared=static_cast<const recovery::Shared*>(MapViewOfFile(map,FILE_MAP_READ,0,0,sizeof(recovery::Shared)));
    bool ok=false;
#ifdef ZOOMIT_RECOVERY_TESTING
    wchar_t reporterMode[32]{};GetEnvironmentVariableW(L"ZOOMIT_TEST_REPORTER_MODE",reporterMode,_countof(reporterMode));
    if(wcscmp(reporterMode,L"hung")==0)Sleep(INFINITE);
    if(wcscmp(reporterMode,L"fail")==0){if(shared)UnmapViewOfFile(shared);CloseHandle(map);return 1;}
#endif
    if(shared&&shared->magic==recovery::ProtocolMagic&&shared->version==recovery::ProtocolVersion&&shared->diagnostic.process==pid){
        HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        ok=recovery::WriteReport(shared->diagnostic,&shared->crash,process,shared->outcome);
        if(process)CloseHandle(process);
    }
    if(shared)UnmapViewOfFile(shared);CloseHandle(map);return ok?0:1;
}
int RunSupervisor(const wchar_t* executableOverride=nullptr) {
    Handles objects;
    #ifdef ZOOMIT_RECOVERY_TESTING
    objects.instance=CreateMutexW(nullptr,FALSE,L"Local\\ZoomItCustomSupervisorTest");
#else
    objects.instance=CreateMutexW(nullptr,FALSE,L"Local\\ZoomItCustomSupervisor");
#endif
    const DWORD mutexError=GetLastError();
    if(!objects.instance)return 1;
    if(mutexError==ERROR_ALREADY_EXISTS){MessageBoxW(nullptr,L"Il supervisore e gia in esecuzione.",L"ZoomIt Custom",MB_OK|MB_ICONINFORMATION);return 0;}
#ifndef ZOOMIT_RECOVERY_TESTING
    if(FindWindowW(L"ZoomItCustomClass",nullptr)){
        MessageBoxW(nullptr,L"ZoomIt Custom e gia aperto. Chiudilo e avvia il supervisore per usare il recupero automatico.",L"ZoomIt Custom",MB_OK|MB_ICONINFORMATION);return 0;
    }
    #endif
    wchar_t supervisor[MAX_PATH],application[MAX_PATH],token[40],name[160],command[2*MAX_PATH+160];
    const DWORD pathLength=GetModuleFileNameW(nullptr,supervisor,MAX_PATH);
    if(!pathLength||pathLength>=MAX_PATH)return 1;
    if(executableOverride)StringCchCopyW(application,MAX_PATH,executableOverride);
    else{
        StringCchCopyW(application,MAX_PATH,supervisor);wchar_t* slash=wcsrchr(application,L'\\');if(!slash)return 1;
        StringCchCopyW(slash+1,MAX_PATH-(slash+1-application),L"ZoomItCustom.exe");
    }
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))return 1;
    wchar_t braces[40];StringFromGUID2(guid,braces,40);
    memcpy(token,braces+1,36*sizeof(wchar_t));token[36]=0;
    recovery::ObjectName(name,160,token,L"control");
    objects.control=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(recovery::Shared),name);
    if(objects.control)objects.shared=static_cast<recovery::Shared*>(MapViewOfFile(objects.control,FILE_MAP_ALL_ACCESS,0,0,sizeof(recovery::Shared)));
    recovery::ObjectName(name,160,token,L"canvas");
    objects.canvas=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE|SEC_RESERVE,0,
        static_cast<DWORD>(recovery::CanvasCapacity*2),name);
    recovery::ObjectName(name,160,token,L"fault");objects.fault=CreateEventW(nullptr,TRUE,FALSE,name);
    recovery::ObjectName(name,160,token,L"ack");objects.ack=CreateEventW(nullptr,TRUE,FALSE,name);
    if(!objects.shared||!objects.canvas||!objects.fault||!objects.ack){
        MessageBoxW(nullptr,L"Impossibile preparare il recupero automatico. Puoi avviare direttamente ZoomItCustom.exe.",L"ZoomIt Custom",MB_OK|MB_ICONERROR);return 1;
    }
    *objects.shared={};objects.shared->magic=recovery::ProtocolMagic;objects.shared->version=recovery::ProtocolVersion;
    objects.shared->supervisor=GetCurrentProcessId();
    unsigned retries=0;ULONGLONG lastCrash=0;
    for(;;){
        ResetEvent(objects.fault);ResetEvent(objects.ack);
        InterlockedExchange(&objects.shared->normalExit,0);InterlockedExchange(&objects.shared->crashReported,0);
        InterlockedExchange(&objects.shared->ready,0);objects.shared->crash={};
        StringCchPrintfW(command,_countof(command),L"\"%s\" --supervised %s %s",application,token,
            retries==0?L"":retries==1?L"--recover":L"--recover-safe");
        PROCESS_INFORMATION child{};
        if(!Launch(application,command,child)){
            recovery::Diagnostics error{};error.process=GetCurrentProcessId();error.error=GetLastError();
            StringCchCopyW(error.operation,128,L"Avvio ZoomIt Custom");
            recovery::WriteReport(error,nullptr,nullptr,L"Avvio supervisionato non riuscito.");
            MessageBoxW(nullptr,L"Impossibile avviare ZoomItCustom.exe. Il rapporto e nella cartella dei log.",L"ZoomIt Custom",MB_OK|MB_ICONERROR);return 1;
        }
        CloseHandle(child.hThread);
#ifdef ZOOMIT_RECOVERY_TESTING
        wchar_t testScenario[64]{};GetEnvironmentVariableW(L"ZOOMIT_TEST_SCENARIO",testScenario,_countof(testScenario));
        // Force both objects signalled before the wait, exposing the lowest-index selection race.
        if(wcscmp(testScenario,L"fault-exit")==0)WaitForSingleObject(child.hProcess,5000);
#endif
        HANDLE waitHandles[]{child.hProcess,objects.fault};
        DWORD event=WaitForMultipleObjects(2,waitHandles,FALSE,INFINITE);
        bool reportSaved=false;
        const ULONGLONG detected=GetTickCount64();
        if(lastCrash&&detected-lastCrash>=60000)retries=0;
        StringCchCopyW(objects.shared->outcome,160,retries==0?
            L"Supervisione: riavvio e ripristino dell'ultimo stato valido.":
            retries==1?L"Supervisione: secondo crash ravvicinato, riavvio sul desktop.":
            L"Supervisione: tre crash ravvicinati, riavvii automatici fermati.");
        if(event==WAIT_OBJECT_0+1||
           (event==WAIT_OBJECT_0&&objects.shared->crashReported)){
            // A crash notification and process termination can become signalled together.
            // Process termination wins the wait by index; it does not prove a report was saved.
            // Dump in a short-lived helper: a stuck dump must never block the supervisor.
            wchar_t dumpCommand[2*MAX_PATH+160];
            StringCchPrintfW(dumpCommand,_countof(dumpCommand),L"\"%s\" --report %s %lu",supervisor,token,child.dwProcessId);
            PROCESS_INFORMATION reporter{};
            bool allowReporter=true;
#ifdef ZOOMIT_RECOVERY_TESTING
            wchar_t reporterMode[32]{};GetEnvironmentVariableW(L"ZOOMIT_TEST_REPORTER_MODE",reporterMode,_countof(reporterMode));
            allowReporter=wcscmp(reporterMode,L"launch-fail")!=0;
#endif
            if(allowReporter&&Launch(supervisor,dumpCommand,reporter)){
                CloseHandle(reporter.hThread);
                if(WaitForSingleObject(reporter.hProcess,3000)==WAIT_TIMEOUT){
                    TerminateProcess(reporter.hProcess,ERROR_TIMEOUT);WaitForSingleObject(reporter.hProcess,1000);
                }
                DWORD result=1;GetExitCodeProcess(reporter.hProcess,&result);reportSaved=result==0;CloseHandle(reporter.hProcess);
            }
            if(!reportSaved)reportSaved=recovery::WriteReport(objects.shared->diagnostic,&objects.shared->crash,nullptr,
                L"Raccolta minidump non completata nei tempi previsti; recupero automatico prosegue.");
            SetEvent(objects.ack);
            if(WaitForSingleObject(child.hProcess,3000)==WAIT_TIMEOUT){
                TerminateProcess(child.hProcess,objects.shared->crash.code);WaitForSingleObject(child.hProcess,1000);
            }
        }else if(event!=WAIT_OBJECT_0){
            SetEvent(objects.ack);CloseHandle(child.hProcess);return 1;
        }
        DWORD exitCode=1;GetExitCodeProcess(child.hProcess,&exitCode);CloseHandle(child.hProcess);
        if(objects.shared->normalExit)return 0;
        if(!reportSaved){
            auto diagnostic=objects.shared->diagnostic;diagnostic.process=child.dwProcessId;diagnostic.error=exitCode;
            StringCchCopyW(diagnostic.operation,128,L"Terminazione anomala del processo");
            recovery::Crash crash=objects.shared->crash;
            if(!objects.shared->crashReported)crash.code=exitCode;
            recovery::WriteReport(diagnostic,&crash,nullptr,objects.shared->outcome);
        }
        recovery::RestoreSystem(objects.shared->system);
        const ULONGLONG now=GetTickCount64();
        if(lastCrash&&now-lastCrash>=60000)retries=0;
        lastCrash=now;
        if(++retries>2){
#ifndef ZOOMIT_RECOVERY_TESTING
            MessageBoxW(nullptr,L"ZoomIt Custom ha avuto tre arresti ravvicinati. Il recupero automatico e stato fermato per evitare un ciclo di riavvii. Consulta i rapporti in %LOCALAPPDATA%\\ZoomItCustom\\Logs.",
                L"ZoomIt Custom",MB_OK|MB_ICONERROR);
#endif
            return 1;
        }
    }
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int count{};LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&count);
    int result;
    if(args&&count==4&&wcscmp(args[1],L"--report")==0){
        wchar_t* end{};const unsigned long pid=wcstoul(args[3],&end,10);
        result=pid&&end&&!*end?ReportProcess(args[2],static_cast<DWORD>(pid)):1;
    }
#ifdef ZOOMIT_RECOVERY_TESTING
    else if(args&&count==3&&wcscmp(args[1],L"--test-worker")==0)result=RunSupervisor(args[2]);
#endif
    else result=RunSupervisor();
    if(args)LocalFree(args);return result;
}
