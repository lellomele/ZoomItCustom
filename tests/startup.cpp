#include "../src/Startup.h"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace fs = std::filesystem;
using zoomit::startup::Mode;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Fixture {
    fs::path base, directory, application, supervisor;
    std::wstring registry;
    Fixture() {
        wchar_t module[MAX_PATH]{};
        Require(zoomit::startup::ApplicationPath(module) == ERROR_SUCCESS, "Test module path");
        base = fs::canonical(fs::path(module).parent_path());
        directory = base / (L"startup-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory / L"cartella con spazi");
        application = directory / L"cartella con spazi" / zoomit::startup::NormalName;
        supervisor = application.parent_path() / zoomit::startup::SupervisorName;
        { std::ofstream file(application); file << "fixture"; }
        { std::ofstream file(supervisor); file << "fixture"; }
        registry = L"Software\\ZoomItCustom\\StartupTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    }
    ~Fixture() {
        RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
        std::error_code error;
        // The only recursive cleanup target is our own fixture directory in the build folder.
        if (fs::weakly_canonical(directory, error).parent_path() == base)
            fs::remove_all(directory, error);
    }
};
std::wstring Command(const Fixture& fixture) {
    wchar_t value[MAX_PATH]{};
    DWORD size = sizeof(value);
    Require(RegGetValueW(HKEY_CURRENT_USER, fixture.registry.c_str(), zoomit::startup::EntryName,
        RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS, "Read test startup command");
    return value;
}
DWORD Values(const Fixture& fixture) {
    zoomit::startup::RegistryKey key;
    Require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.registry.c_str(), 0, KEY_QUERY_VALUE, &key.handle) == ERROR_SUCCESS,
        "Open isolated startup key");
    DWORD count{};
    Require(RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        &count, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS, "Count startup values");
    return count;
}
INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM word, LPARAM) {
    if (message == WM_COMMAND && LOWORD(word) == IDCANCEL) { DestroyWindow(dialog); return TRUE; }
    return FALSE;
}
struct Dialog {
    HWND handle{};
    Dialog() {
        handle = CreateDialogW(GetModuleHandleW(nullptr), L"OPTIONS", nullptr, DialogProc);
        Require(handle != nullptr, "Create native options controls");
    }
    ~Dialog() { if (IsWindow(handle)) DestroyWindow(handle); }
};
int main() {
    try {
        Fixture fixture;
        DWORD error{};
        Require(zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Disabled && error == ERROR_SUCCESS,
            "No startup registration is disabled");
        Require(zoomit::startup::Configure(Mode::Disabled, nullptr, fixture.registry.c_str()) == ERROR_SUCCESS,
            "Disabling an absent startup registration succeeds");
        zoomit::startup::RegistryKey absent;
        Require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.registry.c_str(), 0, KEY_QUERY_VALUE, &absent.handle) == ERROR_FILE_NOT_FOUND,
            "Disabling must not create a registry key");
        wchar_t command[MAX_PATH]{};
        Require(zoomit::startup::BuildCommand(Mode::Normal, fixture.application.c_str(), command) == ERROR_SUCCESS &&
            std::wstring(command) == L"\"" + fixture.application.wstring() + L"\"", "Quote complete path with spaces");
        Require(zoomit::startup::Configure(Mode::Normal, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_SUCCESS &&
            zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Normal && error == ERROR_SUCCESS, "Normal startup round trip");
        Require(Command(fixture) == command && Values(fixture) == 1, "Exactly one normal app entry");
        Require(zoomit::startup::Configure(Mode::Supervised, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_SUCCESS &&
            zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Supervised &&
            Command(fixture) == L"\"" + fixture.supervisor.wstring() + L"\"" && Values(fixture) == 1, "Supervisor replaces the same single entry");

        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "Initialize dialog controls");
        {
            Dialog dialog;
            zoomit::startup::InitializeControls(dialog.handle, false, fixture.application.c_str(), fixture.registry.c_str());
            wchar_t text[160]{};
            GetDlgItemTextW(dialog.handle, IDC_SUPERVISION_STATUS, text, 160);
            Require(wcscmp(text, L"Sessione: esecuzione autonoma") == 0 &&
                IsDlgButtonChecked(dialog.handle, IDC_STARTUP_SUPERVISED) == BST_CHECKED &&
                IsWindowEnabled(GetDlgItem(dialog.handle, IDC_STARTUP_SUPERVISED)), "Startup choice differs from current standalone session");
            const auto original = Command(fixture);
            SendMessageW(GetDlgItem(dialog.handle, IDC_STARTUP_NORMAL), BM_CLICK, 0, 0);
            SendMessageW(dialog.handle, WM_COMMAND, IDCANCEL, 0);
            Require(Command(fixture) == original, "Selecting then cancelling preserves startup command");
        }
        {
            Dialog dialog;
            zoomit::startup::InitializeControls(dialog.handle, true, fixture.application.c_str(), fixture.registry.c_str());
            wchar_t text[160]{};
            GetDlgItemTextW(dialog.handle, IDC_SUPERVISION_STATUS, text, 160);
            Require(wcscmp(text, L"Sessione: supervisione attiva") == 0, "Supervised session label");
            SendMessageW(GetDlgItem(dialog.handle, IDC_STARTUP_NORMAL), BM_CLICK, 0, 0);
            Require(IsDlgButtonChecked(dialog.handle, IDC_STARTUP_NORMAL) == BST_CHECKED &&
                zoomit::startup::SaveControls(dialog.handle, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_SUCCESS &&
                zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Normal, "Native radio selection saves normal startup");
        }
        const auto normalCommand = Command(fixture);
        fs::remove(fixture.supervisor);
        Require(zoomit::startup::Configure(Mode::Supervised, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_FILE_NOT_FOUND &&
            Command(fixture) == normalCommand, "Missing supervisor preserves previous registration");
        {
            Dialog dialog;
            zoomit::startup::InitializeControls(dialog.handle, false, fixture.application.c_str(), fixture.registry.c_str());
            wchar_t text[200]{};
            GetDlgItemTextW(dialog.handle, IDC_STARTUP_HINT, text, 200);
            Require(!IsWindowEnabled(GetDlgItem(dialog.handle, IDC_STARTUP_SUPERVISED)) &&
                wcsstr(text, zoomit::startup::SupervisorName), "Missing supervisor disables only that choice and explains why");
            CheckRadioButton(dialog.handle, IDC_STARTUP_OFF, IDC_STARTUP_SUPERVISED, IDC_STARTUP_SUPERVISED);
            Require(zoomit::startup::SaveControls(dialog.handle, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_FILE_NOT_FOUND &&
                Command(fixture) == normalCommand, "Unavailable selection cannot overwrite valid startup");
            SendMessageW(GetDlgItem(dialog.handle, IDC_STARTUP_OFF), BM_CLICK, 0, 0);
            Require(zoomit::startup::SaveControls(dialog.handle, nullptr, fixture.registry.c_str()) == ERROR_SUCCESS &&
                zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Disabled, "Disable works with missing supervisor");
        }
        fs::create_directory(fixture.supervisor);
        Require(!zoomit::startup::SupervisorAvailable(fixture.application.c_str()), "Directory cannot act as supervisor executable");
        fs::remove(fixture.supervisor);
        { std::ofstream file(fixture.supervisor); file << "fixture"; }

        Require(zoomit::startup::Configure(Mode::Normal, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_SUCCESS, "Reset normal fixture");
        Require(zoomit::startup::Configure(static_cast<Mode>(99), fixture.application.c_str(), fixture.registry.c_str()) == ERROR_INVALID_PARAMETER &&
            Command(fixture) == normalCommand, "Invalid mode cannot damage registration");
        Require(zoomit::startup::BuildCommand(Mode::Normal, L"ZoomItCustom.exe", command) == ERROR_INVALID_PARAMETER, "Relative startup path rejected");
        const std::wstring longPath = L"C:\\" + std::wstring(300, L'x') + L"\\ZoomItCustom.exe";
        Require(zoomit::startup::Configure(Mode::Normal, longPath.c_str(), fixture.registry.c_str()) == ERROR_FILENAME_EXCED_RANGE &&
            Command(fixture) == normalCommand, "Excessive path rejected before registry write");

        {
            zoomit::startup::RegistryKey key;
            Require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.registry.c_str(), 0, KEY_SET_VALUE, &key.handle) == ERROR_SUCCESS, "Open fixture for corruption tests");
            DWORD number = 42;
            Require(RegSetValueExW(key.handle, zoomit::startup::EntryName, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&number), sizeof(number)) == ERROR_SUCCESS, "Inject malformed value");
            Require(RegSetValueExW(key.handle, L"Unrelated", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&number), sizeof(number)) == ERROR_SUCCESS, "Unrelated entry fixture");
        }
        Require(zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Disabled && error != ERROR_SUCCESS,
            "Malformed registration cannot crash the dialog");
        Require(zoomit::startup::Configure(Mode::Disabled, nullptr, fixture.registry.c_str()) == ERROR_SUCCESS && Values(fixture) == 1,
            "Disable removes only the owned startup entry");

        // Legacy quoted normal entries remain recognized even when their old folder no longer exists.
        {
            zoomit::startup::RegistryKey key;
            Require(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.registry.c_str(), 0, KEY_SET_VALUE, &key.handle) == ERROR_SUCCESS, "Legacy registration fixture");
            const wchar_t legacy[] = L"\"C:\\old folder\\ZoomItCustom.exe\"";
            Require(RegSetValueExW(key.handle, zoomit::startup::EntryName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(legacy), sizeof(legacy)) == ERROR_SUCCESS, "Write legacy normal registration");
        }
        Require(zoomit::startup::ReadMode(error, fixture.registry.c_str()) == Mode::Normal && error == ERROR_SUCCESS, "Legacy startup mode recognized");
        Require(zoomit::startup::Configure(Mode::Normal, fixture.application.c_str(), fixture.registry.c_str()) == ERROR_SUCCESS &&
            Command(fixture) == normalCommand, "Confirm updates old installation path");
        std::cout << "{\"startup_passed\":true,\"registry_cases\":14,\"native_ui_cases\":6,\"real_startup_modified\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "STARTUP TEST FAILURE: " << error.what() << "\n";
        return 1;
    }
}
