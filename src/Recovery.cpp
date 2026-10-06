#include "Recovery.h"
#include "version.h"
#include <strsafe.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <magnification.h>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace recovery {
Client client;
DWORD Hash(const void* input,size_t size) noexcept {
    const BYTE* data=static_cast<const BYTE*>(input); DWORD hash=2166136261u;
    while(size>=4){DWORD word;memcpy(&word,data,4);hash=(hash^word)*16777619u;data+=4;size-=4;}
    while(size--)hash=(hash^*data++)*16777619u;
    return hash;
}
static bool ValidRect(RECT r) noexcept {
    const int64_t w=static_cast<int64_t>(r.right)-r.left,h=static_cast<int64_t>(r.bottom)-r.top;
    return w>0 && h>0 && w<=32768 && h<=32768;
}
static bool ValidFields(const State& state) noexcept {
    if(state.mode>Mode::Break ||
       !std::isfinite(state.zoom)||!std::isfinite(state.liveZoom)||state.zoom<1||state.zoom>32||
       state.liveZoom<1||state.liveZoom>32||state.penWidth<1||state.penWidth>600||
       state.rootPenWidth<1||state.rootPenWidth>40)return false;
    if(state.mode!=Mode::Idle && !ValidRect(state.monitor))return false;
    if(state.liveActive>1||state.pointerArrow>1)return false;
    if(state.liveActive||state.mode==Mode::LiveZoom||state.mode==Mode::FrozenLiveDraw){
        if(!ValidRect(state.source)||state.source.left<state.monitor.left||state.source.top<state.monitor.top||
           state.source.right>state.monitor.right||state.source.bottom>state.monitor.bottom)return false;
    }
    if(!state.canvasSequence&&(state.canvasSlot!=-1||state.canvasWidth||state.canvasHeight))return false;
    if(state.canvasSequence && (state.canvasSlot<0||state.canvasSlot>1||!state.canvasWidth||
       !state.canvasHeight||state.canvasWidth>32768||state.canvasHeight>32768||
       static_cast<size_t>(state.canvasWidth)*state.canvasHeight*4>CanvasCapacity))return false;
    return true;
}
bool ValidState(const State& state) noexcept {
    if(!state.sequence)return false;State copy=state;copy.checksum=0;
    return Hash(&copy,sizeof(copy))==state.checksum&&ValidFields(state);
}
bool Latest(const Shared& shared,State& state) noexcept {
    const State a=shared.states[0],b=shared.states[1];const bool va=ValidState(a),vb=ValidState(b);
    if(!va&&!vb)return false;state=!vb||(va&&a.sequence>b.sequence)?a:b;return true;
}
static bool RecoveryLatest(const Shared& shared,State& state)noexcept{
    State best{};Latest(shared,best);
    // A complete image pair can outlive a crash in the final rolling-metadata publication.
    for(const CanvasCheckpoint& checkpoint:shared.canvases)
        if(ValidState(checkpoint.state)&&checkpoint.state.sequence>best.sequence)best=checkpoint.state;
    if(!best.sequence)return false;state=best;return true;
}
bool ValidToken(const wchar_t* token) noexcept {
    if(!token||wcslen(token)!=36)return false;
    for(size_t i=0;i<36;++i){
        if(i==8||i==13||i==18||i==23){if(token[i]!=L'-')return false;}
        else if(!((token[i]>=L'0'&&token[i]<=L'9')||(token[i]>=L'a'&&token[i]<=L'f')||
                  (token[i]>=L'A'&&token[i]<=L'F')))return false;
    }return true;
}
void ObjectName(wchar_t* out,size_t count,const wchar_t* token,const wchar_t* suffix) noexcept {
    StringCchPrintfW(out,count,L"Local\\ZoomItCustomRecovery-%s-%s",token,suffix);
}
static bool WriteText(HANDLE file,const wchar_t* text) noexcept {
    char bytes[16384];DWORD written{};
    const int length=WideCharToMultiByte(CP_UTF8,0,text,-1,bytes,sizeof(bytes),nullptr,nullptr);
    return length>1 && WriteFile(file,bytes,static_cast<DWORD>(length-1),&written,nullptr) &&
        written==static_cast<DWORD>(length-1);
}
static void PruneReports(const wchar_t* directory) noexcept {
    wchar_t pattern[MAX_PATH],oldest[MAX_PATH];WIN32_FIND_DATAW data{};
    StringCchPrintfW(pattern,MAX_PATH,L"%s\\ZoomItCustom-*.txt",directory);
    // Run only after a serious error. No directory scans during normal operation.
    for(;;){
        HANDLE find=FindFirstFileW(pattern,&data);if(find==INVALID_HANDLE_VALUE)return;
        size_t count=0;FILETIME oldestTime{MAXDWORD,MAXDWORD};oldest[0]=0;
        do{
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            ++count;
            if(CompareFileTime(&data.ftCreationTime,&oldestTime)<0){
                oldestTime=data.ftCreationTime;StringCchPrintfW(oldest,MAX_PATH,L"%s\\%s",directory,data.cFileName);
            }
        }while(FindNextFileW(find,&data));FindClose(find);
        if(count<=8||!oldest[0]||!DeleteFileW(oldest))return;
        if(auto ext=wcsrchr(oldest,L'.')){wcscpy_s(ext,5,L".dmp");DeleteFileW(oldest);}
    }
}
bool WriteReport(const Diagnostics& diagnostic,const Crash* crash,HANDLE process,
                 const wchar_t* outcome,const wchar_t* directory) noexcept {
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
    wchar_t testDirectory[MAX_PATH]{};
    if (!directory && GetEnvironmentVariableW(L"ZOOMIT_TEST_LOG_DIRECTORY", testDirectory, MAX_PATH))
        directory = testDirectory;
#endif
    wchar_t folder[MAX_PATH]{},path[MAX_PATH]{},root[MAX_PATH]{};
    if(directory)StringCchCopyW(folder,MAX_PATH,directory);
    else{
        DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",root,MAX_PATH);
        if(!count||count>=MAX_PATH)GetTempPathW(MAX_PATH,root);
        StringCchPrintfW(folder,MAX_PATH,L"%s\\ZoomItCustom",root);CreateDirectoryW(folder,nullptr);
        StringCchPrintfW(folder,MAX_PATH,L"%s\\ZoomItCustom\\Logs",root);
    }CreateDirectoryW(folder,nullptr);
    static volatile LONG reportSequence=0;
    const LONG serial=InterlockedIncrement(&reportSequence);
    SYSTEMTIME time{};GetLocalTime(&time);
    StringCchPrintfW(path,MAX_PATH,L"%s\\ZoomItCustom-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%llu-%ld.txt",
        folder,time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
        time.wMilliseconds,diagnostic.process,GetTickCount64(),serial);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE&&!directory&&GetTempPathW(MAX_PATH,root)){
        StringCchPrintfW(folder,MAX_PATH,L"%sZoomItCustom-Logs",root);CreateDirectoryW(folder,nullptr);
        StringCchPrintfW(path,MAX_PATH,L"%s\\ZoomItCustom-%lu-%llu-%ld.txt",folder,diagnostic.process,GetTickCount64(),serial);
        file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    if(file==INVALID_HANDLE_VALUE)return false;
    wchar_t text[4096]{},errorText[512]{};
    if(!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,diagnostic.error,0,errorText,512,nullptr))
        StringCchCopyW(errorText,512,L"Descrizione non disponibile");
    constexpr const wchar_t* modes[]{L"Desktop",L"Zoom",L"LiveZoom",L"Draw",L"LiveDraw",L"Draw su LiveZoom congelato",L"Break"};
    const DWORD mode=static_cast<DWORD>(diagnostic.mode);
    const wchar_t* modeName=mode<_countof(modes)?modes[mode]:L"Stato sconosciuto";
    StringCchPrintfW(text,4096,L"ZoomIt Custom " TEXT(FILE_VERSION_STRING) L"\r\n"
        L"Data locale: %04u-%02u-%02u %02u:%02u:%02u\r\nTipo: %s\r\n"
        L"Processo: %lu  Thread: %lu\r\nOperazione: %s\r\n"
        L"Modalita: %s; zoom: %.3g\r\nErrore: 0x%08lX (%lu) %s\r\nUltimo messaggio: 0x%04lX; parametro: %llu\r\n"
        L"Monitor logici: %d; desktop virtuale: %dx%d, origine (%d,%d)\r\nEsito: %s\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,
        crash?L"CRASH":L"ERRORE GRAVE",diagnostic.process,diagnostic.thread,diagnostic.operation,
        modeName,diagnostic.zoom,diagnostic.error,diagnostic.error,errorText,diagnostic.message,
        static_cast<unsigned long long>(diagnostic.wordParam),GetSystemMetrics(SM_CMONITORS),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),outcome);
    if(!WriteText(file,text)){CloseHandle(file);DeleteFileW(path);return false;}
    bool complete=true;
    if(crash){
        StringCchPrintfW(text,4096,L"Eccezione: 0x%08lX; indirizzo: 0x%llX; thread: %lu\r\n",
            crash->code,static_cast<unsigned long long>(crash->address),crash->thread);complete=WriteText(file,text)&&complete;
        if(process){
            wchar_t dumpPath[MAX_PATH];StringCchCopyW(dumpPath,MAX_PATH,path);
            if(auto ext=wcsrchr(dumpPath,L'.'))wcscpy_s(ext,5,L".dmp");
            HANDLE dump=CreateFileW(dumpPath,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            BOOL dumped=FALSE;DWORD dumpError=0;
            if(dump!=INVALID_HANDLE_VALUE){
                Crash snapshot=*crash;EXCEPTION_POINTERS pointers{&snapshot.record,&snapshot.context};
                MINIDUMP_EXCEPTION_INFORMATION info{snapshot.thread,&pointers,FALSE};
                dumped=MiniDumpWriteDump(process,diagnostic.process,dump,
                    static_cast<MINIDUMP_TYPE>(MiniDumpNormal|MiniDumpWithThreadInfo),
                    snapshot.record.ExceptionCode?&info:nullptr,nullptr,nullptr);
                if(!dumped)dumpError=GetLastError();CloseHandle(dump);if(!dumped)DeleteFileW(dumpPath);
            }else dumpError=GetLastError();
            StringCchPrintfW(text,4096,L"Minidump: %s; errore: 0x%08lX\r\n",dumped?dumpPath:L"non disponibile",dumpError);complete=WriteText(file,text)&&complete;
        }else complete=WriteText(file,L"Minidump: processo gia terminato; rapporto basato sul codice di uscita.\r\n")&&complete;
    }
    complete=FlushFileBuffers(file)!=FALSE&&complete;CloseHandle(file);
    if(!complete){DeleteFileW(path);if(auto ext=wcsrchr(path,L'.')){wcscpy_s(ext,5,L".dmp");DeleteFileW(path);}return false;}
    PruneReports(folder);return true;
}
void RestoreSystem(SystemState& state) noexcept {
    if(state.stickyChanged&&SystemParametersInfoW(SPI_SETSTICKYKEYS,sizeof(STICKYKEYS),&state.sticky,SPIF_SENDCHANGE))
        InterlockedExchange(&state.stickyChanged,0);
    if(state.saverChanged&&SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE,state.saver,nullptr,0))
        InterlockedExchange(&state.saverChanged,0);
    if(state.displayChanged&&state.displayName[0]&&
        ChangeDisplaySettingsExW(state.displayName,&state.display,nullptr,0,nullptr)==DISP_CHANGE_SUCCESSFUL)
        InterlockedExchange(&state.displayChanged,0);
    ClipCursor(nullptr);
    if(MagInitialize()){
        MagSetFullscreenTransform(1,0,0);MagSetInputTransform(FALSE,nullptr,nullptr);MagShowSystemCursor(TRUE);MagUninitialize();
    }SetCursor(LoadCursorW(nullptr,IDC_ARROW));
}
static LONG WINAPI OnFault(EXCEPTION_POINTERS* pointers){return client.Fault(pointers);}
Client::~Client(){
    if(pixels_)UnmapViewOfFile(pixels_);if(shared_)UnmapViewOfFile(shared_);
    for(HANDLE h:{map_,canvasMap_,faultEvent_,faultAck_})if(h)CloseHandle(h);
}
bool Client::Initialize(const wchar_t* commandLine) noexcept {
    diagnostic_.process=GetCurrentProcessId();diagnostic_.thread=GetCurrentThreadId();
    DWORD stack=64*1024;SetThreadStackGuarantee(&stack);SetUnhandledExceptionFilter(OnFault);
    SetErrorMode(GetErrorMode()|SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    int count{};LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&count);const wchar_t* token=nullptr;
    for(int i=1;args&&i<count;++i){
        if(wcscmp(args[i],L"--supervised")==0&&i+1<count)token=args[++i];
        else if(wcscmp(args[i],L"--recover-safe")==0)safe_=true;
        else if(wcscmp(args[i],L"--recover")==0)recovering_=true;
    }
    if(commandLine&&ValidToken(commandLine))token=commandLine;
    bool valid=true;
    if(token){
        wchar_t name[160];if(!ValidToken(token))valid=false;
        else{
            ObjectName(name,160,token,L"control");map_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
            if(map_)shared_=static_cast<Shared*>(MapViewOfFile(map_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
            if(!shared_||shared_->magic!=ProtocolMagic||shared_->version!=ProtocolVersion)valid=false;
            if(valid){
                HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,shared_->supervisor);
                valid=parent&&WaitForSingleObject(parent,0)==WAIT_TIMEOUT;if(parent)CloseHandle(parent);
            }
            if(valid){
                ObjectName(name,160,token,L"canvas");canvasMap_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
                if(canvasMap_)pixels_=static_cast<BYTE*>(MapViewOfFile(canvasMap_,FILE_MAP_ALL_ACCESS,0,0,CanvasCapacity*2));
                ObjectName(name,160,token,L"fault");faultEvent_=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
                ObjectName(name,160,token,L"ack");faultAck_=OpenEventW(SYNCHRONIZE,FALSE,name);
                valid=pixels_&&faultEvent_&&faultAck_;
            }
            if(valid){
                attached_=true;shared_->diagnostic=diagnostic_;State previous{};
                if(RecoveryLatest(*shared_,previous)){
                    stateSequence_=previous.sequence;canvasSequence_=previous.canvasSequence;canvasSlot_=previous.canvasSlot;
                    canvasSerial_=previous.canvasSequence;
                    for(const CanvasCheckpoint& checkpoint:shared_->canvases)
                        canvasSerial_=(std::max)(canvasSerial_,checkpoint.image.sequence);
                    for(const State& rolling:shared_->states)
                        if(ValidState(rolling))canvasSerial_=(std::max)(canvasSerial_,rolling.canvasSequence);
                    canvasWidth_=previous.canvasWidth;canvasHeight_=previous.canvasHeight;
                    if(recovering_&&!safe_)pending_=previous;
                }else recovering_=false;
            }
        }
    }
    if(args)LocalFree(args);
    if(!valid){Serious(L"Connessione al supervisore",ERROR_INVALID_DATA);return false;}return true;
}
bool Client::TakeRecovery(State& state)noexcept{
    if(!recovering_||safe_||!ValidState(pending_))return false;
    const auto accept=[&](const State& valid) noexcept {
        state=valid;
        // Future metadata must reference the image actually restored, including a fallback.
        canvasSlot_=valid.canvasSlot;canvasSequence_=valid.canvasSequence;
        canvasWidth_=valid.canvasWidth;canvasHeight_=valid.canvasHeight;
        return true;
    };
    if(!pending_.canvasSequence||CanvasValid(pending_))return accept(pending_);
    // Metadata can roll over both current-state slots; each complete image retains its own state.
    State best{};
    const auto candidate=[&](const State& previous) noexcept {
        if(previous.sequence<pending_.sequence&&ValidState(previous)&&
           (!previous.canvasSequence||CanvasValid(previous))&&previous.sequence>best.sequence)best=previous;
    };
    for(const State& previous:shared_->states)candidate(previous);
    for(const CanvasCheckpoint& previous:shared_->canvases)candidate(previous.state);
    if(best.sequence)return accept(best);
    Serious(L"Checkpoint di recupero non valido; avvio sul desktop",ERROR_INVALID_DATA);
    return false;
}
void Client::FinishRecovery()noexcept{recovering_=false;pending_={};if(shared_)InterlockedExchange(&shared_->ready,1);}
void Client::Note(UINT message,WPARAM wordParam)noexcept{
    diagnostic_.message=message;diagnostic_.wordParam=wordParam;diagnostic_.thread=GetCurrentThreadId();
    if(shared_)shared_->diagnostic=diagnostic_;
}
void Client::Serious(const wchar_t* operation,DWORD error)noexcept{
    if(writing_)return;writing_=true;diagnostic_.error=error;diagnostic_.process=GetCurrentProcessId();StringCchCopyW(diagnostic_.operation,128,operation);
    if(shared_)shared_->diagnostic=diagnostic_;
    WriteReport(diagnostic_,nullptr,nullptr,L"Operazione interrotta; tentativo di mantenere o ripristinare uno stato utilizzabile.");
    writing_=false;
}
LONG Client::Fault(EXCEPTION_POINTERS* pointers)noexcept{
    Crash crash{};crash.code=pointers->ExceptionRecord->ExceptionCode;crash.thread=GetCurrentThreadId();
    crash.address=reinterpret_cast<ULONG_PTR>(pointers->ExceptionRecord->ExceptionAddress);
    crash.record=*pointers->ExceptionRecord;crash.record.ExceptionRecord=nullptr;crash.context=*pointers->ContextRecord;
    diagnostic_.thread=crash.thread;diagnostic_.error=crash.code;
    StringCchCopyW(diagnostic_.operation,128,L"Eccezione runtime non gestita");
    if(attached_){
        shared_->diagnostic=diagnostic_;shared_->crash=crash;MemoryBarrier();
        InterlockedExchange(&shared_->crashReported,1);SetEvent(faultEvent_);WaitForSingleObject(faultAck_,5000);
    }else WriteReport(diagnostic_,&crash,GetCurrentProcess(),L"Esecuzione autonoma: riapertura manuale necessaria.");
    return EXCEPTION_EXECUTE_HANDLER;
}
void Client::NormalExit()noexcept{if(shared_&&attached_)InterlockedExchange(&shared_->normalExit,1);}
void Client::CheckpointFailure(DWORD condition,const wchar_t* operation,DWORD error)noexcept{
    if(checkpointFailures_&condition)return;
    checkpointFailures_|=condition;Serious(operation,error);
}
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
bool Client::Fail(TestFailure failure)noexcept{
    if(testFailure_!=failure)return false;
    testFailure_=TestFailure::None;SetLastError(ERROR_GEN_FAILURE);return true;
}
void Client::PrepareRecoveryForTest()noexcept{
    recovering_=shared_&&RecoveryLatest(*shared_,pending_);safe_=false;
}
#define RECOVERY_FAIL(name) Fail(TestFailure::name)
#else
#define RECOVERY_FAIL(name) false
#endif
void Client::Commit(State state,HDC canvas,HDC cursorPatch,const RECT* cursorRect,bool copyCanvas)noexcept{
    diagnostic_.mode=state.mode;diagnostic_.zoom=state.zoom;
    if(shared_)shared_->diagnostic=diagnostic_;
    if(!attached_||recovering_)return;
    if(previousMode_==Mode::Idle&&state.mode!=Mode::Idle)checkpointFailures_=0;
    previousMode_=state.mode;
    const bool imageMode=state.mode==Mode::Zoom||state.mode==Mode::Draw||
        state.mode==Mode::LiveDraw||state.mode==Mode::FrozenLiveDraw;
    // Validate geometry and scalars before overwriting the inactive image slot.
    state.canvasSlot=-1;state.canvasSequence=state.canvasWidth=state.canvasHeight=0;
    state.sequence=stateSequence_+1;state.checksum=0;
    if(!ValidFields(state)){CheckpointFailure(1,L"Validazione stato di recupero",ERROR_INVALID_DATA);return;}
    const auto publish=[&](State value) noexcept {
        value.sequence=++stateSequence_;value.checksum=0;value.checksum=Hash(&value,sizeof(value));
        const DWORD index=value.sequence%2;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->states[index].sequence),0);
        State unpublished=value;unpublished.sequence=0;shared_->states[index]=unpublished;MemoryBarrier();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->states[index].sequence),value.sequence);
        return value;
    };
    const auto safeWithoutCanvas=[&]() noexcept {
        // An oversize/new surface can keep running normally, but cannot claim a restorable image.
        if(!canvasSequence_){State idle{};idle.color=state.color;idle.penWidth=state.penWidth;
            idle.rootPenWidth=state.rootPenWidth;publish(idle);}
    };
    bool captured=false;LONG newSlot=canvasSlot_;DWORD newSequence=canvasSequence_;
    DWORD newWidth=canvasWidth_,newHeight=canvasHeight_,newHash=0;
    if(copyCanvas&&imageMode){
        const int64_t width=static_cast<int64_t>(state.monitor.right)-state.monitor.left;
        const int64_t height=static_cast<int64_t>(state.monitor.bottom)-state.monitor.top;
        if(!canvas||width<=0||height<=0||static_cast<uint64_t>(width)*height*4>CanvasCapacity){
            CheckpointFailure(2,L"Dimensione checkpoint grafico",canvas?ERROR_NOT_ENOUGH_MEMORY:ERROR_INVALID_HANDLE);
            safeWithoutCanvas();return;
        }
        const int w=static_cast<int>(width),h=static_cast<int>(height);
        newSlot=canvasSlot_==0?1:0;BYTE* data=pixels_+static_cast<size_t>(newSlot)*CanvasCapacity;
        const size_t bytes=static_cast<size_t>(w)*h*4;
        if(RECOVERY_FAIL(Allocation)||!VirtualAlloc(data,bytes,MEM_COMMIT,PAGE_READWRITE)){
            CheckpointFailure(4,L"Memoria checkpoint",GetLastError());safeWithoutCanvas();return;
        }
        // From this point the inactive image might be partial; never expose its old associated state.
        shared_->canvases[newSlot].state.sequence=0;shared_->canvases[newSlot].image.sequence=0;MemoryBarrier();
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};void* dibPixels{};
        HDC dc=RECOVERY_FAIL(CreateDC)?nullptr:CreateCompatibleDC(canvas);
        HBITMAP bitmap=!dc||RECOVERY_FAIL(CreateBitmap)?nullptr:CreateDIBSection(canvas,&info,DIB_RGB_COLORS,
            &dibPixels,canvasMap_,static_cast<DWORD>(newSlot*CanvasCapacity));
        HGDIOBJ old=dc&&bitmap&&!RECOVERY_FAIL(SelectBitmap)?SelectObject(dc,bitmap):nullptr;
        bool ok=dc&&bitmap&&old&&old!=HGDI_ERROR&&!RECOVERY_FAIL(Capture)&&
            BitBlt(dc,0,0,w,h,canvas,0,0,SRCCOPY);
        if(ok&&cursorPatch&&cursorRect){
            const int64_t patchWidth=static_cast<int64_t>(cursorRect->right)-cursorRect->left;
            const int64_t patchHeight=static_cast<int64_t>(cursorRect->bottom)-cursorRect->top;
            ok=patchWidth>0&&patchHeight>0&&patchWidth<=32768&&patchHeight<=32768&&
                !RECOVERY_FAIL(Patch)&&BitBlt(dc,cursorRect->left,cursorRect->top,
                    static_cast<int>(patchWidth),static_cast<int>(patchHeight),cursorPatch,0,0,SRCCOPY)!=FALSE;
        }
        const bool flushed=GdiFlush()!=FALSE&&!RECOVERY_FAIL(Flush);
        if(old&&old!=HGDI_ERROR)SelectObject(dc,old);if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);
        if(!ok||!flushed){CheckpointFailure(!flushed?16:8,!flushed?L"Sincronizzazione checkpoint grafico":
            L"Copia checkpoint grafico",ERROR_GEN_FAILURE);safeWithoutCanvas();return;}
        newSequence=canvasSerial_+1;newWidth=w;newHeight=h;newHash=Hash(data,bytes);
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
        ++statistics_.canvasCopies;statistics_.hashedBytes+=bytes;
#endif
        if(RECOVERY_FAIL(Publication)){
            CheckpointFailure(32,L"Pubblicazione checkpoint grafico",ERROR_GEN_FAILURE);safeWithoutCanvas();return;
        }
        captured=true;
    }
    if(imageMode&&newSequence&&newWidth==static_cast<DWORD>(state.monitor.right-state.monitor.left)&&
       newHeight==static_cast<DWORD>(state.monitor.bottom-state.monitor.top)){
        state.canvasSlot=newSlot;state.canvasSequence=newSequence;state.canvasWidth=newWidth;state.canvasHeight=newHeight;
    }else if(imageMode){
        // Never publish an active image mode with absent or incompatible image data.
        state.mode=Mode::Idle;state.liveActive=0;state.zoom=state.liveZoom=1;state.source={};
    }
    if(!ValidFields(state)){CheckpointFailure(1,L"Validazione stato di recupero",ERROR_INVALID_DATA);return;}
    if(captured){
        // The complete image and its state are independently usable if a crash interrupts metadata publication.
        CanvasCheckpoint checkpoint{{newSequence,newWidth,newHeight,newHash},state};
        checkpoint.state.sequence=stateSequence_+1;checkpoint.state.checksum=0;
        checkpoint.state.checksum=Hash(&checkpoint.state,sizeof(checkpoint.state));
        const DWORD completed=checkpoint.state.sequence;checkpoint.state.sequence=0;
        shared_->canvases[newSlot]=checkpoint;MemoryBarrier();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->canvases[newSlot].state.sequence),completed);
        canvasSlot_=newSlot;canvasSequence_=canvasSerial_=newSequence;canvasWidth_=newWidth;canvasHeight_=newHeight;
    }
    publish(state);
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
    if(!captured)++statistics_.metadataCommits;
#endif
}
#undef RECOVERY_FAIL
bool Client::CanvasValid(const State& state)const noexcept{
    if(!attached_||!state.canvasSequence||!ValidState(state))return false;
    const auto info=shared_->canvases[state.canvasSlot].image;
    if(info.sequence!=state.canvasSequence||info.width!=state.canvasWidth||info.height!=state.canvasHeight)return false;
    const BYTE* data=pixels_+static_cast<size_t>(state.canvasSlot)*CanvasCapacity;
    const size_t bytes=static_cast<size_t>(state.canvasWidth)*state.canvasHeight*4;
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(data,&memory,sizeof(memory))&&memory.State==MEM_COMMIT&&memory.RegionSize>=bytes&&Hash(data,bytes)==info.checksum;
}
bool Client::RestoreCanvas(const State& state,HDC destination)noexcept{
    if(!destination||!CanvasValid(state))return false;
    const BYTE* data=pixels_+static_cast<size_t>(state.canvasSlot)*CanvasCapacity;
    BITMAPINFO bitmap{};bitmap.bmiHeader={sizeof(BITMAPINFOHEADER),static_cast<LONG>(state.canvasWidth),-static_cast<LONG>(state.canvasHeight),1,32,BI_RGB};
    const bool copied=SetDIBitsToDevice(destination,0,0,state.canvasWidth,state.canvasHeight,0,0,0,
        state.canvasHeight,data,&bitmap,DIB_RGB_COLORS)==static_cast<int>(state.canvasHeight);
    return GdiFlush()!=FALSE&&copied;
}
}
