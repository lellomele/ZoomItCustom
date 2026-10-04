#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include "resource.h"
#include "version.h"

#define ZOOMIT_ABOUT_WIDE_IMPL(value) L##value
#define ZOOMIT_ABOUT_WIDE(value) ZOOMIT_ABOUT_WIDE_IMPL(value)

namespace zoomit::about {

inline void Initialize(HWND dialog) {
    if (!GetDlgItem(dialog, IDC_ABOUT_VERSION)) return;
    SetDlgItemTextW(dialog, IDC_ABOUT_VERSION,
                    L"Version " ZOOMIT_ABOUT_WIDE(FILE_VERSION_STRING));
}

// The destination comes from the control ID, never notification text.
inline bool HandleLink(HWND dialog, LPARAM notification) {
    if (!notification) return false;
    const auto* link = reinterpret_cast<const NMLINK*>(notification);
    if (link->hdr.code != NM_CLICK && link->hdr.code != NM_RETURN) return false;
    const wchar_t* destination = nullptr;
    if (link->hdr.idFrom == IDC_ABOUT_REPOSITORY) {
        destination = L"https://github.com/lellomele/ZoomItCustom";
    } else if (link->hdr.idFrom == IDC_ABOUT_LICENSE) {
        if (link->item.iLink == 0)
            destination = L"https://github.com/lellomele/ZoomItCustom/blob/main/LICENSE";
        else if (link->item.iLink == 1)
            destination = L"https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt";
    }
    if (!destination) return false;
    ShellExecuteW(dialog, L"open", destination, nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

} // namespace zoomit::about

#undef ZOOMIT_ABOUT_WIDE
#undef ZOOMIT_ABOUT_WIDE_IMPL
