//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Class to select a screenshot region
//
//==============================================================================
#include "pch.h"
#include "SelectRectangle.h"
#include "Utility.h"
#include "WindowsVersions.h"
#include "RuntimeSafety.h"

namespace {
bool ApplySelectionRegion(HWND window,RECT outer,RECT inside) noexcept {
    using zoomit::runtime::Api;
    if(!zoomit::runtime::ValidRect(outer))return false;
    if(inside.right<=inside.left || inside.bottom<=inside.top)inside={};
    if(!zoomit::runtime::Permit(Api::Graphics))return false;
    native::unique_hrgn region(CreateRectRgnIndirect(&outer));
    if(!region.get() || !zoomit::runtime::Permit(Api::Graphics))return false;
    native::unique_hrgn hole(CreateRectRgnIndirect(&inside));
    if(!hole.get() || !zoomit::runtime::Permit(Api::Graphics) ||
       CombineRgn(region.get(),region.get(),hole.get(),RGN_XOR)==ERROR ||
       !zoomit::runtime::Permit(Api::Graphics) || !SetWindowRgn(window,region.get(),TRUE))return false;
    region.release(); // Windows owns the region only after successful installation.
    return true;
}
}

//----------------------------------------------------------------------------
//
// SelectRectangle::Start
//
//----------------------------------------------------------------------------
bool SelectRectangle::Start( HWND ownerWindow, bool fullMonitor )
{
    Stop();
    RECT rect{};
    if(!GetMonitorRectFromCursor(rect) || !zoomit::runtime::ValidRect(rect))return false;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = []( HWND window, UINT message, WPARAM wordParam, LPARAM longParam ) -> LRESULT
    {
        if( message == WM_NCCREATE )
        {
            auto createStruct = reinterpret_cast<LPCREATESTRUCT>(longParam);
            SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams) );
            return TRUE;
        }

        auto self = reinterpret_cast<SelectRectangle*>(GetWindowLongPtrW( window, GWLP_USERDATA ));
        return self ? self->WindowProc(window, message, wordParam, longParam) : DefWindowProcW(window, message, wordParam, longParam);
    };
    windowClass.hInstance = GetModuleHandle( nullptr );
    windowClass.hCursor = LoadCursorW( nullptr, IDC_CROSS );
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject( BLACK_BRUSH ));
    windowClass.lpszClassName = m_className;
    if( RegisterClassW( &windowClass ) == 0 )
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        WNDCLASSW existingClass{};
        if (!GetClassInfoW(GetModuleHandle(nullptr), m_className, &existingClass)) return false;
        if (existingClass.lpfnWndProc != windowClass.lpfnWndProc) return false;
    }

    m_selected = false;
    m_cancel = false;
    m_dragging = false;
    m_window = native::unique_hwnd( CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, m_className, nullptr, WS_POPUP,
                                                  rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, ownerWindow,
                                                  nullptr, nullptr, this ) );
    if (!m_window.get()) return false;

    if( fullMonitor )
    {
        m_selectedRect = {0,0,rect.right-rect.left,rect.bottom-rect.top};
        if(!ShowSelected()){Stop();return false;}
    }
    else
    {
        if(!SetLayeredWindowAttributes(m_window.get(),0,Alpha(),LWA_ALPHA)){Stop();return false;}
    }

    ShowWindow( m_window.get(), SW_SHOW );
    SetForegroundWindow( m_window.get() );

    if( !fullMonitor )
    {
        m_setClip = zoomit::runtime::Permit(zoomit::runtime::Api::Cursor) && GetClipCursor(&m_oldClipRect) &&
            zoomit::runtime::Permit(zoomit::runtime::Api::Cursor) && ClipCursor(&rect);
    }

    if(fullMonitor)return m_selected && !m_cancel;

    MSG message{};
    int result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        TranslateMessage( &message );
        DispatchMessageW( &message );
        if( m_cancel )
        {
            return false;
        }
        if( m_selected )
        {
            break;
        }
    }
    if (result <= 0) {
        if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
        Stop();
        return false;
    }
    return m_selected && !m_cancel;
}

//----------------------------------------------------------------------------
//
// SelectRectangle::Stop
//
//----------------------------------------------------------------------------
void SelectRectangle::RestoreClip() noexcept {
    if(!m_setClip)return;
    m_setClip=false;
    if(!ClipCursor(&m_oldClipRect))ClipCursor(nullptr);
}
void SelectRectangle::Stop()
{
    // Set state first: releasing capture synchronously sends WM_CAPTURECHANGED.
    m_cancel=true;m_dragging=false;
    const HWND owned=m_window.get();
    if(owned && GetCapture()==owned)ReleaseCapture();
    RestoreClip();
    m_window.reset();
    m_selected=false;m_selectedRect={};
}

//----------------------------------------------------------------------------
//
// SelectRectangle::ShowSelected
//
//----------------------------------------------------------------------------
bool SelectRectangle::ShowSelected()
{
    const HWND window=m_window.get();
    if(!window || !zoomit::runtime::ValidRect(m_selectedRect))return false;
    POINT point{m_selectedRect.left,m_selectedRect.top};
    RECT outer{0,0,m_selectedRect.right-m_selectedRect.left,m_selectedRect.bottom-m_selectedRect.top};
    const int width=(std::max)(1,ScaleForDpi(2,m_dpi));
    // Older Windows captures layered windows; keep the border outside the snip.
    if(GetWindowsBuild(nullptr)<BUILD_WINDOWS_11_22H2) {
        InflateRect(&outer,width,width);OffsetRect(&outer,-outer.left,-outer.top);
        point.x-=width;point.y-=width;
    }
    RECT previous{};if(!GetWindowRect(window,&previous))return false;
    point.x+=previous.left;point.y+=previous.top;
    RECT inside=outer;InflateRect(&inside,-width,-width);
    if(!ApplySelectionRegion(window,outer,inside) || !SetLayeredWindowAttributes(window,0,191,LWA_ALPHA))return false;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style=GetWindowLongPtr(window,GWL_EXSTYLE);
    if(!SetWindowLongPtr(window,GWL_EXSTYLE,style|WS_EX_TRANSPARENT) && GetLastError()!=ERROR_SUCCESS)return false;
    // Set selected before disabling the window, which may synchronously lose focus.
    m_selected=true;
    EnableWindow(window,FALSE);
    if(!MoveWindow(window,point.x,point.y,outer.right,outer.bottom,TRUE)) {m_selected=false;return false;}
    return true;
}

//----------------------------------------------------------------------------
//
// SelectRectangle::UpdateOwner
//
//----------------------------------------------------------------------------
void SelectRectangle::UpdateOwner( HWND window )
{
    if( m_window != nullptr )
    {
        SetWindowLongPtr( m_window.get(), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(window) );
        SetWindowPos( m_window.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE );
    }
}

//----------------------------------------------------------------------------
//
// SelectRectangle::WindowProc
//
//----------------------------------------------------------------------------
LRESULT SelectRectangle::WindowProc( HWND window, UINT message, WPARAM wordParam, LPARAM longParam )
{
    switch( message )
    {
    case WM_CREATE:
        m_dpi = GetDpiForWindowHelper( window );
        SetWindowDisplayAffinity( window, WDA_EXCLUDEFROMCAPTURE );
        return 0;

    case WM_DESTROY:
        m_cancel=true;m_dragging=false;
        if(GetCapture()==window)ReleaseCapture();
        RestoreClip();m_window.release();
        return 0;

    case WM_LBUTTONDOWN:
    {
        if(m_cancel || m_selected)return 0;
        SetCapture(window);
        if(GetCapture()!=window){Stop();return 0;}
        m_dragging=true;
        RECT client{};if(!GetClientRect(window,&client) || !zoomit::runtime::ValidRect(client)){Stop();return 0;}
        m_startPoint={(std::clamp)(static_cast<LONG>(GET_X_LPARAM(longParam)),client.left,client.right-1),
                      (std::clamp)(static_cast<LONG>(GET_Y_LPARAM(longParam)),client.top,client.bottom-1)};
        [[fallthrough]];
    }
    case WM_MOUSEMOVE:
        if(!m_cancel && !m_selected && m_dragging && GetCapture()==window)
        {
            RECT rect{};if(!GetClientRect(window,&rect) || !zoomit::runtime::ValidRect(rect)){Stop();return 0;}
            POINT point{(std::clamp)(static_cast<LONG>(GET_X_LPARAM(longParam)),rect.left,rect.right-1),
                        (std::clamp)(static_cast<LONG>(GET_Y_LPARAM(longParam)),rect.top,rect.bottom-1)};
            const LONG minimum=(std::max)(1L,(std::min)(static_cast<LONG>(MinSize()),(std::min)(rect.right-rect.left,rect.bottom-rect.top)));
            m_selectedRect=ForceRectInBounds(RectFromPointsMinSize(m_startPoint,point,minimum),rect);
            if(!ApplySelectionRegion(window,rect,m_selectedRect)){Stop();return 0;}
        }
        return 0;

    case WM_CANCELMODE:
        if(!m_selected && !m_cancel)Stop();
        return 0;
    case WM_CAPTURECHANGED:
        if(m_dragging && !m_selected && reinterpret_cast<HWND>(longParam)!=window)Stop();
        return 0;

    case WM_KEYDOWN:
        if( wordParam == VK_ESCAPE )
        {
            Stop();
        }
        return 0;

    case WM_KILLFOCUS:
        if( !m_selected )
        {
            Stop();
        }
        return 0;

    case WM_LBUTTONUP:
    {
        // Ignore a button-up from an aborted drag or a previously completed selection.
        if(m_cancel || m_selected || !m_dragging)return 0;
        if(GetCapture()!=window){Stop();return 0;}
        m_dragging=false;RestoreClip();ReleaseCapture();
        if(!ShowSelected())Stop();
        return 0;
    }
    case WM_NCHITTEST:
        if( m_selected )
        {
            return HTTRANSPARENT;
        }
        break;

    case WM_PAINT:
        if( m_selected )
        {
            PAINTSTRUCT paint;
            auto deviceContext = BeginPaint( window, &paint );

            RECT rect;
            GetClientRect( window, &rect );

            // Draw a border matching the Windows graphics capture API border.
            // The outer frame is yellow and two logical pixels wide, while the
            // inner is black and 1 logical pixel wide.
            native::unique_hbrush brush{CreateSolidBrush( RGB( 255, 222, 0 ) )};
            FillRect( deviceContext, &rect, brush.get() );
            int width = ScaleForDpi( 1, m_dpi );
            InflateRect( &rect, -width, -width );
            FillRect( deviceContext, &rect, static_cast<HBRUSH>(GetStockObject( BLACK_BRUSH )) );

            EndPaint( window, &paint );
            return 0;
        }
        break;
    }

    return DefWindowProcW( window, message, wordParam, longParam );
}
