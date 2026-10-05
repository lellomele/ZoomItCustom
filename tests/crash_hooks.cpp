#include <windows.h>
#include <strsafe.h>
#include <bit>
#include <shellapi.h>
#include "../src/Recovery.h"
extern HWND g_hWndLiveZoom;
extern DWORD g_PenColor,g_PenWidth,g_RootPenWidth,g_BreakTimeout;
extern BOOLEAN g_AnimateZoom;
namespace {
constexpr UINT QueryMode=WM_APP+17,QueryCanvas=WM_APP+18;
void Record(const wchar_t* text){
    wchar_t path[MAX_PATH];if(!GetEnvironmentVariableW(L"ZOOMIT_TEST_RESULT_PATH",path,MAX_PATH))return;
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){
        char bytes[512];int count=WideCharToMultiByte(CP_UTF8,0,text,-1,bytes,512,nullptr,nullptr);DWORD written{};
        if(count>1)WriteFile(file,bytes,count-1,&written,nullptr);FlushFileBuffers(file);CloseHandle(file);
    }
}
bool CorruptLatestFrame(){
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);const wchar_t* token=nullptr;
    for(int i=1;args&&i+1<count;++i)if(wcscmp(args[i],L"--supervised")==0){token=args[i+1];break;}
    wchar_t name[160];HANDLE control{},image{};recovery::Shared* shared{};BYTE* pixels{};bool ok=false;
    if(recovery::ValidToken(token)){
        recovery::ObjectName(name,160,token,L"control");control=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
        if(control)shared=static_cast<recovery::Shared*>(MapViewOfFile(control,FILE_MAP_ALL_ACCESS,0,0,sizeof(recovery::Shared)));
        recovery::ObjectName(name,160,token,L"canvas");image=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
        if(image)pixels=static_cast<BYTE*>(MapViewOfFile(image,FILE_MAP_ALL_ACCESS,0,0,recovery::CanvasCapacity*2));
        recovery::State current{};
        if(shared&&pixels&&recovery::Latest(*shared,current)&&current.canvasSequence){
            pixels[static_cast<size_t>(current.canvasSlot)*recovery::CanvasCapacity]^=1;ok=true;
        }
    }
    if(shared)UnmapViewOfFile(shared);if(pixels)UnmapViewOfFile(pixels);
    if(control)CloseHandle(control);if(image)CloseHandle(image);if(args)LocalFree(args);return ok;
}
void CrashNow(){RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);}
}
void RunRecoveryScenario(HWND window){
    wchar_t scenario[64]{};GetEnvironmentVariableW(L"ZOOMIT_TEST_SCENARIO",scenario,64);
    const bool recovering=wcsstr(GetCommandLineW(),L"--recover")!=nullptr;
    const bool safe=wcsstr(GetCommandLineW(),L"--recover-safe")!=nullptr;
    Record(L"boot\n");
    if(wcscmp(scenario,L"loop")==0){Record(L"crash\n");RaiseFailFastException(nullptr,nullptr,0);return;}
    if(wcscmp(scenario,L"normal")==0){Record(L"normal\n");PostMessage(window,WM_COMMAND,IDCANCEL,0);return;}
    if(wcscmp(scenario,L"serious")==0){
        recovery::client.Serious(L"Errore grave provocato dalla prova",ERROR_NOT_ENOUGH_MEMORY);
        Record(L"serious-ok\n");PostMessage(window,WM_COMMAND,IDCANCEL,0);return;
    }
    if(wcscmp(scenario,L"allocation")==0){
        SetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",L"1");
        SendMessage(window,WM_HOTKEY,0,0);SetEnvironmentVariableW(L"ZOOMIT_TEST_FAIL_GDI",nullptr);
        Record(SendMessage(window,QueryMode,0,0)==0?L"allocation-ok\n":L"allocation-failed\n");
        PostMessage(window,WM_COMMAND,IDCANCEL,0);return;
    }
    if(recovering){
        const LRESULT mode=SendMessage(window,QueryMode,0,0);
        bool ok=true;
        if(safe)ok=mode==0&&!IsWindowVisible(g_hWndLiveZoom);
        else if(wcscmp(scenario,L"live")==0){
            auto level=reinterpret_cast<const float*>(SendMessage(g_hWndLiveZoom,WM_USER+102,0,0));
            ok=IsWindowVisible(g_hWndLiveZoom)&&level&&*level==2.5f;
        }else if(wcscmp(scenario,L"break")==0)ok=(mode&32)!=0;
        else{
            HDC canvas=reinterpret_cast<HDC>(SendMessage(window,QueryCanvas,0,0));
            ok=(wcscmp(scenario,L"zoom")==0?(mode&1)!=0&&(mode&16)==0:(mode&16)!=0) &&
                canvas&&GetPixel(canvas,32,48)==RGB(21,91,201);
            if(wcscmp(scenario,L"layered")==0)ok=ok&&IsWindowVisible(g_hWndLiveZoom);
            if(wcscmp(scenario,L"frozen")==0)ok=ok&&(mode&8);
        }
        Record(ok?L"recovered-ok\n":L"recovered-failed\n");
        if(wcscmp(scenario,L"twice")==0&&!safe){Record(L"crash\n");CrashNow();return;}
        PostMessage(window,WM_COMMAND,IDCANCEL,0);return;
    }
    g_AnimateZoom=FALSE;g_RootPenWidth=5;g_PenWidth=5;g_PenColor=0xFF0000FF;
    if(wcscmp(scenario,L"live")==0||wcscmp(scenario,L"layered")==0||wcscmp(scenario,L"frozen")==0){
        SendMessage(window,WM_HOTKEY,3,0);
        SendMessage(g_hWndLiveZoom,WM_USER+104,std::bit_cast<DWORD>(2.5f),0);
        SendMessage(window,WM_TIMER,3,0);
    }
    if(wcscmp(scenario,L"break")==0){
        g_BreakTimeout=2;SendMessage(window,WM_HOTKEY,2,0);
    }else if(wcscmp(scenario,L"live")!=0){
        SendMessage(window,WM_HOTKEY,wcscmp(scenario,L"zoom")==0?0:wcscmp(scenario,L"layered")==0?4:1,
            wcscmp(scenario,L"zoom")==0?MAKELPARAM(MOD_CONTROL,'1'):0);
        if(wcscmp(scenario,L"zoom")!=0){
            SendMessage(window,WM_LBUTTONDOWN,0,MAKELPARAM(120,130));
            SendMessage(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(160,180));
            SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(160,180));
        }
        HDC canvas=reinterpret_cast<HDC>(SendMessage(window,QueryCanvas,0,0));
        if(canvas)SetPixel(canvas,32,48,RGB(21,91,201));
        SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(160,180));
    }
    if(wcscmp(scenario,L"exception")==0){
        SendMessage(window,WM_APP+21,0,0);
        CURSORINFO cursor{sizeof(cursor)};GetCursorInfo(&cursor);
        Record(SendMessage(window,QueryMode,0,0)==0&&(cursor.flags&CURSOR_SHOWING)?L"exception-ok\n":L"exception-failed\n");
        PostMessage(window,WM_COMMAND,IDCANCEL,0);return;
    }
    if(wcscmp(scenario,L"resume")==0){
        SendMessage(window,WM_POWERBROADCAST,PBT_APMRESUMEAUTOMATIC,0);
        Record(SendMessage(window,QueryMode,0,0)==0?L"resume-ok\n":L"resume-failed\n");
        PostMessage(window,WM_COMMAND,IDCANCEL,0);return;
    }
    if(wcscmp(scenario,L"corrupt-frame")==0){
        HDC canvas=reinterpret_cast<HDC>(SendMessage(window,QueryCanvas,0,0));
        if(canvas)SetPixel(canvas,32,48,RGB(201,91,21));
        SendMessage(window,WM_LBUTTONUP,0,MAKELPARAM(160,180));
        if(!CorruptLatestFrame())Record(L"corruption-failed\n");
    }
    Record(L"crash\n");
    if(wcscmp(scenario,L"forced")==0)ExitProcess(0xC0000409);
    CrashNow();
}
