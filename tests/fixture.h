#pragma once
#include <exception>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

// Assert only after leaving native callback boundaries. A test failure must not
// be mistaken for the application's recovery from a runtime exception.
inline std::exception_ptr g_TestCallbackFailure;
inline void RethrowTestCallbackFailure() {
    if (auto failure=std::exchange(g_TestCallbackFailure,{})) std::rethrow_exception(failure);
}
inline void RecordTestCallbackFailure() noexcept {
    if (!g_TestCallbackFailure) g_TestCallbackFailure=std::current_exception();
}
template<class F> INT_PTR TestDialogBoundary(HWND dialog,F&& run) noexcept {
    try { return run(); }
    catch (...) {
        RecordTestCallbackFailure();
        if (IsWindow(dialog)) PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
        return TRUE;
    }
}
template<class F> void TestCallbackBoundary(HWND cancelWindow,UINT cancelMessage,F&& run) noexcept {
    try { run(); }
    catch (...) {
        RecordTestCallbackFailure();
        if (IsWindow(cancelWindow) && cancelMessage) PostMessageW(cancelWindow,cancelMessage,VK_ESCAPE,0);
    }
}

// Keep a real HKCU handle for evidence and cleanup. Every production preference
// and autostart operation in this process uses the isolated root from startup
// through Options OK and window destruction, including failure unwinding.
class ProcessRegistryFixture {
    HKEY realRoot_{},fixture_{};
    std::wstring path_;
    bool redirected_{};
    std::vector<std::vector<BYTE>> before_;
    static std::vector<BYTE> ReadValue(HKEY root,const wchar_t* key,const wchar_t* name) {
        zoomit::startup::RegistryKey opened;
        DWORD type{},size{};
        const auto openedError=RegOpenKeyExW(root,key,0,KEY_QUERY_VALUE,&opened.handle);
        LSTATUS error=openedError;
        if (error==ERROR_SUCCESS) error=RegQueryValueExW(opened.handle,name,nullptr,&type,nullptr,&size);
        if (size>1024*1024) throw std::runtime_error("Original registry value exceeds safe test snapshot size");
        std::vector<BYTE> result(sizeof(error)+sizeof(type)+size);
        memcpy(result.data(),&error,sizeof(error));memcpy(result.data()+sizeof(error),&type,sizeof(type));
        if (error==ERROR_SUCCESS && size) {
            const auto read=RegQueryValueExW(opened.handle,name,nullptr,&type,result.data()+sizeof(error)+sizeof(type),&size);
            if (read!=ERROR_SUCCESS) throw std::runtime_error("Read original registry value for test isolation evidence");
        }
        return result;
    }
    std::vector<std::vector<BYTE>> Snapshot() const {
        std::vector<std::vector<BYTE>> values;
        for (const auto& entry:RegSettings) if (entry.ValueName)
            values.push_back(ReadValue(realRoot_,L"Software\\ZoomItCustom\\" APPNAME,entry.ValueName));
        values.push_back(ReadValue(realRoot_,zoomit::startup::RunKey,zoomit::startup::EntryName));
        return values;
    }
    void Cleanup() noexcept {
        if (redirected_) {RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);redirected_=false;}
        if (fixture_) {RegCloseKey(fixture_);fixture_=nullptr;}
        if (realRoot_) {if (!path_.empty())RegDeleteTreeW(realRoot_,path_.c_str());RegCloseKey(realRoot_);realRoot_=nullptr;}
    }
public:
    ProcessRegistryFixture() {
        try {
            if (RegOpenCurrentUser(KEY_READ|KEY_WRITE,&realRoot_)!=ERROR_SUCCESS)
                throw std::runtime_error("Open original HKCU for regression isolation");
            before_=Snapshot();
            path_=L"Software\\ZoomItCustom\\TestFixtures\\Regression_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
            if (RegCreateKeyExW(realRoot_,path_.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&fixture_,nullptr)!=ERROR_SUCCESS)
                throw std::runtime_error("Create process-wide isolated regression registry");
            if (RegOverridePredefKey(HKEY_CURRENT_USER,fixture_)!=ERROR_SUCCESS)
                throw std::runtime_error("Redirect HKCU before application initialization");
            redirected_=true;
        } catch (...) {Cleanup();throw;}
    }
    ~ProcessRegistryFixture(){Cleanup();}
    ProcessRegistryFixture(const ProcessRegistryFixture&)=delete;
    void VerifyUntouched() const {
        RethrowTestCallbackFailure();
        if (before_!=Snapshot()) throw std::runtime_error("Native regression changed real application preferences or autostart registration");
    }
};
