//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Class to select a screenshot region
//
//==============================================================================
#pragma once

#include "pch.h"

class SelectRectangle
{
public:
    ~SelectRectangle() { Stop(); };

    void Alpha( BYTE alpha ) { m_alpha = alpha; }
    BYTE Alpha() const { return m_alpha; }
    void MinSize( int minSize ) { m_minSize = (std::clamp)(minSize,1,32768); }
    int MinSize() const { return m_minSize; }
    RECT SelectedRect() const { return m_selectedRect; }

    bool Start( HWND ownerWindow = nullptr, bool fullMonitor = false );
    void Stop();
    void UpdateOwner( HWND window );
#ifdef ZOOMIT_TESTING
    HWND TestWindow() const noexcept { return m_window.get(); }
    bool TestDragging() const noexcept { return m_dragging; }
    bool TestClipped() const noexcept { return m_setClip; }
    bool TestCancelled() const noexcept { return m_cancel; }
#endif

private:
    BYTE m_alpha = 176;
    int m_minSize = 34;
    RECT m_selectedRect{};

    bool m_cancel = false;
    bool m_dragging = false;
    const wchar_t* m_className = L"ZoomitSelectRectangle";
    UINT m_dpi{};
    RECT m_oldClipRect{};
    bool m_selected{ false };
    bool m_setClip{ false };
    POINT m_startPoint{};
    native::unique_hwnd m_window;

    bool ShowSelected();
    void RestoreClip() noexcept;
    LRESULT WindowProc( HWND window, UINT message, WPARAM wordParam, LPARAM longParam );
};
