#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include "resource.h"

namespace zoomit::startup {
enum class Mode : DWORD { Disabled, Normal, Supervised };
inline constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t EntryName[] = L"ZoomIt Custom";
inline constexpr wchar_t SupervisorName[] = L"ZoomItCustomSupervisor.exe";
inline constexpr wchar_t NormalName[] = L"ZoomItCustom.exe";

struct RegistryKey {
    HKEY handle{};
    ~RegistryKey() { if (handle) RegCloseKey(handle); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
};

inline DWORD ApplicationPath(wchar_t (&path)[MAX_PATH]) noexcept {
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length) return GetLastError();
    return length >= MAX_PATH ? ERROR_FILENAME_EXCED_RANGE : ERROR_SUCCESS;
}

inline DWORD SupervisorPath(const wchar_t* application, wchar_t (&path)[MAX_PATH]) noexcept {
    if (!application || !*application || PathIsRelativeW(application)) return ERROR_INVALID_PARAMETER;
    if (FAILED(StringCchCopyW(path, MAX_PATH, application))) return ERROR_FILENAME_EXCED_RANGE;
    wchar_t* name = PathFindFileNameW(path);
    if (name == path) return ERROR_INVALID_PARAMETER;
    return SUCCEEDED(StringCchCopyW(name, MAX_PATH - (name - path), SupervisorName))
        ? ERROR_SUCCESS : ERROR_FILENAME_EXCED_RANGE;
}

inline bool IsFile(const wchar_t* path) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool SupervisorAvailable(const wchar_t* application) noexcept {
    wchar_t path[MAX_PATH]{};
    return SupervisorPath(application, path) == ERROR_SUCCESS && IsFile(path);
}

inline Mode ReadMode(DWORD& error, const wchar_t* registryPath = RunKey) noexcept {
    wchar_t command[MAX_PATH]{};
    DWORD bytes = sizeof(command);
    error = RegGetValueW(HKEY_CURRENT_USER, registryPath, EntryName,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, command, &bytes);
    if (error == ERROR_FILE_NOT_FOUND) { error = ERROR_SUCCESS; return Mode::Disabled; }
    if (error != ERROR_SUCCESS) return Mode::Disabled;
    if (!command[0]) { error = ERROR_INVALID_DATA; return Mode::Disabled; }
    int count{};
    LPWSTR* arguments = CommandLineToArgvW(command, &count);
    if (!arguments) { error = ERROR_NOT_ENOUGH_MEMORY; return Mode::Disabled; }
    Mode mode = Mode::Disabled;
    if (count == 1 && !PathIsRelativeW(arguments[0])) {
        const auto name = PathFindFileNameW(arguments[0]);
        if (_wcsicmp(name, SupervisorName) == 0) mode = Mode::Supervised;
        else if (_wcsicmp(name, NormalName) == 0) mode = Mode::Normal;
    }
    LocalFree(arguments);
    if (mode == Mode::Disabled) error = ERROR_INVALID_DATA;
    return mode;
}

inline DWORD BuildCommand(Mode mode, const wchar_t* application,
                          wchar_t (&command)[MAX_PATH]) noexcept {
    command[0] = 0;
    if (mode == Mode::Disabled) return ERROR_SUCCESS;
    if (mode > Mode::Supervised || !application || !*application || PathIsRelativeW(application))
        return ERROR_INVALID_PARAMETER;
    wchar_t target[MAX_PATH]{};
    DWORD error = ERROR_SUCCESS;
    if (mode == Mode::Supervised) error = SupervisorPath(application, target);
    else if (FAILED(StringCchCopyW(target, MAX_PATH, application))) error = ERROR_FILENAME_EXCED_RANGE;
    if (error != ERROR_SUCCESS) return error;
    if (!IsFile(target)) return ERROR_FILE_NOT_FOUND;
    // Run values are a single quoted executable, without a second app launch.
    if (wcslen(target) > MAX_PATH - 3) return ERROR_FILENAME_EXCED_RANGE;
    return SUCCEEDED(StringCchPrintfW(command, MAX_PATH, L"\"%s\"", target))
        ? ERROR_SUCCESS : ERROR_FILENAME_EXCED_RANGE;
}

inline DWORD Configure(Mode mode, const wchar_t* application,
                       const wchar_t* registryPath = RunKey) noexcept {
    wchar_t command[MAX_PATH]{};
    const DWORD buildError = BuildCommand(mode, application, command);
    if (buildError != ERROR_SUCCESS) return buildError;
    RegistryKey key;
    DWORD error;
    if (mode == Mode::Disabled) {
        error = RegOpenKeyExW(HKEY_CURRENT_USER, registryPath, 0, KEY_SET_VALUE, &key.handle);
        if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (error != ERROR_SUCCESS) return error;
        error = RegDeleteValueW(key.handle, EntryName);
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    error = RegCreateKeyExW(HKEY_CURRENT_USER, registryPath, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key.handle, nullptr);
    if (error != ERROR_SUCCESS) return error;
    return RegSetValueExW(key.handle, EntryName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command), static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t)));
}

inline void InitializeControls(HWND dialog, bool supervised,
                               const wchar_t* application = nullptr,
                               const wchar_t* registryPath = RunKey) noexcept {
    wchar_t currentPath[MAX_PATH]{};
    const DWORD pathError = application ? ERROR_SUCCESS : ApplicationPath(currentPath);
    if (!application) application = currentPath;
    DWORD readError{};
    const Mode mode = ReadMode(readError, registryPath);
    CheckRadioButton(dialog, IDC_STARTUP_OFF, IDC_STARTUP_SUPERVISED,
                     IDC_STARTUP_OFF + static_cast<int>(mode));
    const bool available = pathError == ERROR_SUCCESS && SupervisorAvailable(application);
    EnableWindow(GetDlgItem(dialog, IDC_STARTUP_SUPERVISED), available);
    SetDlgItemTextW(dialog, IDC_SUPERVISION_STATUS,
        supervised ? L"Sessione: supervisione attiva" : L"Sessione: esecuzione autonoma");
    const wchar_t* hint = L"La scelta si applica al prossimo accesso a Windows.";
    if (!available) hint = L"Supervisore non disponibile: copia ZoomItCustomSupervisor.exe nella stessa cartella dell'app.";
    else if (readError != ERROR_SUCCESS) hint = L"Impossibile leggere la configurazione di avvio. Scegli un'opzione e premi OK per aggiornarla.";
    SetDlgItemTextW(dialog, IDC_STARTUP_HINT, hint);
}

inline DWORD SaveControls(HWND dialog, const wchar_t* application = nullptr,
                          const wchar_t* registryPath = RunKey) noexcept {
    Mode mode;
    if (IsDlgButtonChecked(dialog, IDC_STARTUP_OFF) == BST_CHECKED) mode = Mode::Disabled;
    else if (IsDlgButtonChecked(dialog, IDC_STARTUP_NORMAL) == BST_CHECKED) mode = Mode::Normal;
    else if (IsDlgButtonChecked(dialog, IDC_STARTUP_SUPERVISED) == BST_CHECKED) mode = Mode::Supervised;
    else return ERROR_INVALID_DATA;
    wchar_t currentPath[MAX_PATH]{};
    if (!application && mode != Mode::Disabled) {
        const DWORD error = ApplicationPath(currentPath);
        if (error != ERROR_SUCCESS) return error;
        application = currentPath;
    }
    return Configure(mode, application, registryPath);
}
} // namespace zoomit::startup
