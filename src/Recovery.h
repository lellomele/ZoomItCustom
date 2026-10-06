#pragma once
#include <windows.h>
#include <cstdint>
namespace recovery {
constexpr DWORD ProtocolVersion=2, ProtocolMagic=0x5a495452;
constexpr size_t CanvasCapacity=64ull*1024*1024;
constexpr UINT ResetMessage=WM_APP+42, RestoreMessage=WM_APP+43;
enum class Mode:DWORD { Idle,Zoom,LiveZoom,Draw,LiveDraw,FrozenLiveDraw,Break };
struct State {
    DWORD sequence{}; Mode mode{}; RECT monitor{},source{}; POINT view{},pointer{};
    float zoom{1},liveZoom{1};
    DWORD color{},penWidth{2},rootPenWidth{2},pointerArrow{},liveActive{};
    ULONGLONG breakDeadline{};
    LONG canvasSlot{-1}; DWORD canvasSequence{},canvasWidth{},canvasHeight{},checksum{};
};
struct CanvasInfo { DWORD sequence{},width{},height{},checksum{}; };
// Metadata-only changes retain the state belonging to each complete image.
struct CanvasCheckpoint { CanvasInfo image{}; State state{}; };
struct SystemState {
    volatile LONG stickyChanged{},saverChanged{},displayChanged{};
    STICKYKEYS sticky{sizeof(STICKYKEYS)}; BOOL saver{};
    wchar_t displayName[32]{}; DEVMODE display{};
};
struct Diagnostics {
    DWORD process{},thread{},message{},error{}; Mode mode{}; float zoom{1}; UINT_PTR wordParam{};
    wchar_t operation[128]{};
};
struct Crash {
    DWORD code{},thread{}; ULONG_PTR address{};
    EXCEPTION_RECORD record{}; CONTEXT context{};
};
struct Shared {
    DWORD magic{},version{},supervisor{};
    volatile LONG normalExit{},crashReported{},ready{};
    State states[2]{}; CanvasCheckpoint canvases[2]{};
    SystemState system{}; Diagnostics diagnostic{}; Crash crash{};
    wchar_t outcome[160]{};
};
DWORD Hash(const void*,size_t) noexcept;
bool ValidState(const State&) noexcept;
bool Latest(const Shared&,State&) noexcept;
bool ValidToken(const wchar_t*) noexcept;
void ObjectName(wchar_t*,size_t,const wchar_t*,const wchar_t*) noexcept;
bool WriteReport(const Diagnostics&,const Crash*,HANDLE,const wchar_t*,const wchar_t* directory=nullptr) noexcept;
void RestoreSystem(SystemState&) noexcept;
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
enum class TestFailure { None, Allocation, CreateDC, CreateBitmap, SelectBitmap, Capture, Patch, Flush, Publication };
struct TestStatistics { uint64_t canvasCopies{},hashedBytes{},metadataCommits{}; };
#endif
// Standalone operation creates no files, polling thread, or recovery canvas.
class Client {
    HANDLE map_{},canvasMap_{},faultEvent_{},faultAck_{};
    Shared* shared_{}; BYTE* pixels_{};
    Diagnostics diagnostic_{}; SystemState system_{}; State pending_{};
    bool CanvasValid(const State&)const noexcept;
    bool recovering_{},attached_{},safe_{},writing_{};
    DWORD stateSequence_{},canvasSequence_{},canvasSerial_{};
    LONG canvasSlot_{-1}; DWORD canvasWidth_{},canvasHeight_{};
    DWORD checkpointFailures_{}; Mode previousMode_{Mode::Idle}; uint64_t checkpointSession_{};
    void CheckpointFailure(DWORD,const wchar_t*,DWORD)noexcept;
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
    TestFailure testFailure_{}; TestStatistics statistics_{};
    bool Fail(TestFailure)noexcept;
#endif
public:
    ~Client();
    bool Initialize(const wchar_t* commandLine) noexcept;
    bool Supervised()const noexcept {return attached_;}
    bool Recovering()const noexcept {return recovering_;}
    bool TakeRecovery(State&)noexcept;
    void FinishRecovery()noexcept;
    void Note(UINT,WPARAM)noexcept;
    void Serious(const wchar_t*,DWORD error=0)noexcept;
    LONG Fault(EXCEPTION_POINTERS*)noexcept;
    void NormalExit()noexcept;
    SystemState& System()noexcept {return shared_?shared_->system:system_;}
    void SetSession(uint64_t session)noexcept {
        if(session!=checkpointSession_){checkpointSession_=session;checkpointFailures_=0;}
    }
    void Commit(State,HDC canvas=nullptr,HDC cursorPatch=nullptr,const RECT* cursorRect=nullptr,bool copyCanvas=false)noexcept;
    bool RestoreCanvas(const State&,HDC)noexcept;
#if defined(ZOOMIT_RECOVERY_TESTING) || defined(ZOOMIT_TESTING)
    void FailNext(TestFailure failure)noexcept {testFailure_=failure;}
    TestStatistics Statistics()const noexcept {return statistics_;}
    void PrepareRecoveryForTest()noexcept;
#endif
};
extern Client client;
}
