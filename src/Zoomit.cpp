//============================================================================
//
// Zoomit
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
// Screen zoom and annotation tool.
//
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//============================================================================
#include "pch.h"

#include "zoomit.h"
#include "Utility.h"
#include "WindowsVersions.h"
#include "ZoomItSettings.h"


HINSTANCE		g_hInstance;

COLORREF	g_CustomColors[16];

#define ZOOM_HOTKEY				0
#define DRAW_HOTKEY				1
#define BREAK_HOTKEY			2
#define LIVE_HOTKEY				3
#define LIVE_DRAW_HOTKEY		    4
#define SNIP_HOTKEY			    8
#define SNIP_SAVE_HOTKEY		    9

#define ZOOM_PAGE	  0
#define LIVE_PAGE	  1
#define DRAW_PAGE	  2


#define BREAK_PAGE	  3
#define SNIP_PAGE	  4

OPTION_TABS g_OptionsTabs[] = {
    { _T("Zoom"), NULL },
    { _T("LiveZoom"), NULL },
    { _T("Draw"), NULL },


    { _T("Break"), NULL },
    { _T("Snip"), NULL }
};

float g_ZoomLevels[] = {
    1.25,
    1.50,
    1.75,
    2.00,
    3.00,
    4.00
};

//

//


const DWORD CURSOR_ARM_LENGTH = 4;

const float NORMAL_BLUR_RADIUS = 20;
const float STRONG_BLUR_RADIUS = 40;

DWORD	g_ToggleMod;
DWORD	g_LiveZoomToggleMod;
DWORD	g_DrawToggleMod;
DWORD	g_BreakToggleMod; 

DWORD   g_SnipToggleMod;

BOOLEAN	g_ZoomOnLiveZoom = FALSE;
DWORD	g_PenWidth = PEN_WIDTH;
float   g_BlurRadius = NORMAL_BLUR_RADIUS;
HWND	hWndOptions = NULL;
BOOLEAN	g_DrawPointer = FALSE;
BOOLEAN g_PenDown = FALSE;
BOOLEAN g_PenInverted = FALSE;
DWORD	g_OsVersion;
HWND	g_hWndLiveZoom = NULL;
HWND    g_hWndLiveZoomMag = NULL;
HWND	g_hWndMain;
int		g_AlphaBlend = 0x80;
BOOL	g_fullScreenWorkaround = FALSE;
bool	g_bSaveInProgress = false;

// True if ZoomIt was started by PowerToys instead of standalone.
BOOLEAN g_StartedByPowerToys = FALSE;
BOOLEAN g_running = TRUE;
bool g_LiveZoomNeedsWindowsUpdate = false;
#ifdef ZOOMIT_TESTING
bool g_TestMode = false;
HBITMAP g_TestSnipBitmap = nullptr;
const wchar_t* g_TestSavePath = nullptr;
DWORD g_TestSaveFilter = 1;
#endif

void PersistSettings();

bool MigrateHotkeys() noexcept {
    if (g_SettingsSchema >= 2) return false;
    g_ToggleKey = (HOTKEYF_CONTROL << 8) | '1';
    g_LiveZoomToggleKey = (HOTKEYF_CONTROL << 8) | '2';
    g_DrawToggleKey = (HOTKEYF_CONTROL << 8) | '3';
    g_BreakToggleKey = (HOTKEYF_CONTROL << 8) | '4';
    g_SnipToggleKey = (HOTKEYF_CONTROL << 8) | '5';
    g_SettingsSchema = 2;
    return true;
}

bool g_SelectionActive = false;

type_pGetMonitorInfo		pGetMonitorInfo;
type_MonitorFromPoint		pMonitorFromPoint;
type_pSHAutoComplete		pSHAutoComplete;
type_pSetLayeredWindowAttributes	pSetLayeredWindowAttributes;
type_pSetProcessDPIAware	pSetProcessDPIAware;
type_pMagSetWindowSource	pMagSetWindowSource;
type_pMagSetWindowTransform pMagSetWindowTransform;
type_pMagSetFullscreenTransform pMagSetFullscreenTransform;
type_pMagSetInputTransform	pMagSetInputTransform;
type_pMagShowSystemCursor	pMagShowSystemCursor;
type_pMagSetWindowFilterList pMagSetWindowFilterList;
type_pMagInitialize			pMagInitialize;
type_pDwmIsCompositionEnabled	pDwmIsCompositionEnabled;
type_pGetPointerType		pGetPointerType;
type_pGetPointerPenInfo pGetPointerPenInfo;
type_pSystemParametersInfoForDpi pSystemParametersInfoForDpi;
type_pGetDpiForWindow		pGetDpiForWindow;

type_pSHQueryUserNotificationState	pSHQueryUserNotificationState;


void RestoreSystemPointer() noexcept {
    if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void ValidateSettings() noexcept {
    if (g_SliderZoomLevel >= _countof(g_ZoomLevels)) g_SliderZoomLevel = 3;
    g_RootPenWidth = (std::clamp)(g_RootPenWidth, DWORD{1}, DWORD{19});

    g_BreakTimeout = (std::clamp)(g_BreakTimeout, DWORD{1}, DWORD{99});
    g_BreakOpacity = (std::clamp)(g_BreakOpacity, DWORD{0}, DWORD{100});
    g_BreakTimerPosition = (std::min)(g_BreakTimerPosition, DWORD{8});

    g_LogFont.lfFaceName[LF_FACESIZE - 1] = 0;
}

ClassRegistry	reg( _T("Software\\ZoomItCustom\\") APPNAME );

void PersistSettings() {
#ifdef ZOOMIT_TESTING
    if (g_TestMode) return;
#endif
    reg.WriteRegSettings(RegSettings);
}

ComputerGraphicsInit	g_GraphicsInit;


//----------------------------------------------------------------------------
//
// Saves specified filePath to clipboard. 
//
//----------------------------------------------------------------------------
//----------------------------------------------------------------------
//
// OutputDebug
//
//----------------------------------------------------------------------
void OutputDebug(const TCHAR* format, ...)
{
#if _DEBUG
    TCHAR	msg[1024];
    va_list	va;

#ifdef _MSC_VER
// For some reason, ARM64 Debug builds causes an analyzer error on va_start: "error C26492: Don't use const_cast to cast away const or volatile (type.3)."
#pragma warning(push)
#pragma warning(disable : 26492)
#endif
    va_start(va, format);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    _vstprintf_s(msg, format, va);
    va_end(va);

    OutputDebugString(msg);
#endif
}

//----------------------------------------------------------------------------
//
// InitializeFonts
//
// Return a bold equivalent of either a DPI aware font face for GUI text or
// just the stock object for DEFAULT_GUI_FONT.
//
//----------------------------------------------------------------------------
void InitializeFonts( HWND hwnd, HFONT *bold )
{
    LOGFONT logFont;
    bool haveLogFont = false;

    if( *bold )
    {
        DeleteObject( *bold );
        *bold = nullptr;
    }

    if( pSystemParametersInfoForDpi && pGetDpiForWindow )
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof( metrics );

        if( pSystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof( metrics ), &metrics, 0, pGetDpiForWindow( hwnd ) ) )
        {
            CopyMemory( &logFont, &metrics.lfMessageFont, sizeof( logFont ) );
            haveLogFont = true;
        }
    }

    if( !haveLogFont )
    {
        auto normal = static_cast<HFONT>(GetStockObject( DEFAULT_GUI_FONT ));
        GetObject( normal, sizeof( logFont ), &logFont );
        haveLogFont = true; // for correctness
    }

    logFont.lfWeight = FW_BOLD;
    *bold = CreateFontIndirect( &logFont );
}

//----------------------------------------------------------------------------
//
// EnsureForeground
//
//----------------------------------------------------------------------------
void EnsureForeground()
{
    if( !IsWindowVisible( g_hWndMain ) )
        SetForegroundWindow( g_hWndMain );
}

//----------------------------------------------------------------------------
//
// RestoreForeground
//
//----------------------------------------------------------------------------
void RestoreForeground()
{
    // If the main window is not visible, move foreground to the next window
    if( !IsWindowVisible( g_hWndMain ) ) {

        // Activate the next window by unhiding and hiding the main window
        MoveWindow( g_hWndMain, 0, 0, 0, 0, FALSE );
        ShowWindow( g_hWndMain, SW_SHOWNA );
        ShowWindow( g_hWndMain, SW_HIDE );

		OutputDebug(L"RESTORE FOREGROUND\n");
    }
}

//----------------------------------------------------------------------------
//
// ErrorDialog
//
//----------------------------------------------------------------------------
VOID ErrorDialog( HWND hParent, PCTSTR message, DWORD _Error )
{
    LPTSTR	lpMsgBuf;
    TCHAR	errmsg[1024];

    FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                    NULL, _Error, 
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, NULL );
    _stprintf( errmsg, L"%s: %s", message, lpMsgBuf );
    MessageBox( hParent, errmsg, APPNAME, MB_OK|MB_ICONERROR);
}

//----------------------------------------------------------------------------
//
// ErrorDialogString
//
//----------------------------------------------------------------------------
VOID ErrorDialogString( HWND hParent, PCTSTR Message, const wchar_t *_Error )
{
    TCHAR	errmsg[1024];

    _stprintf_s( errmsg, _countof( errmsg ), L"%s: %s", Message, _Error );
    if( hParent == g_hWndMain )
    {
        EnsureForeground();
    }
    MessageBox(hParent, errmsg, APPNAME, MB_OK | MB_ICONERROR);
    if( hParent == g_hWndMain )
    {
        RestoreForeground();
    }
}


//--------------------------------------------------------------------
//
// SetAutostartFilePath
//
// Sets the file path for later autostart config.
// 
//--------------------------------------------------------------------
void SetAutostartFilePath()
{
    HKEY hZoomit;
    DWORD error;
    TCHAR imageFile[MAX_PATH] = { 0 };

    error = RegCreateKeyEx( HKEY_CURRENT_USER, _T( "Software\\ZoomItCustom\\Zoomit" ), 0,
        0, 0, KEY_SET_VALUE, NULL, &hZoomit, NULL );
    if( error == ERROR_SUCCESS ) {

        GetModuleFileName( NULL, imageFile + 1, _countof( imageFile ) - 2 );
        imageFile[0] = '"';
        *(_tcschr( imageFile, 0 )) = '"';
        error = RegSetValueEx( hZoomit, L"FilePath", 0, REG_SZ, (BYTE *) imageFile,
            static_cast<DWORD>(_tcslen( imageFile ) + 1)* sizeof( TCHAR ));
        RegCloseKey( hZoomit );
    }
}

//--------------------------------------------------------------------
//
// ConfigureAutostart
//
// Enables or disables Zoomit autostart for the current image file.
// 
//--------------------------------------------------------------------
bool ConfigureAutostart( HWND hParent, bool Enable ) 
{
    HKEY hRunKey, hZoomit;
    DWORD error, length, type;
    TCHAR imageFile[MAX_PATH];

    error = RegOpenKeyEx( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 
        0, KEY_SET_VALUE, &hRunKey );
    if( error == ERROR_SUCCESS ) {

        if( Enable ) {
            
            error = RegOpenKeyEx( HKEY_CURRENT_USER, _T("Software\\ZoomItCustom\\Zoomit"), 0, 
                        KEY_QUERY_VALUE, &hZoomit );
            if( error == ERROR_SUCCESS ) {

                length = sizeof(imageFile);
#ifdef _WIN64
                // Unconditionally reset filepath in case this was already set by 32 bit version
                SetAutostartFilePath();		
#endif
                error = RegQueryValueEx( hZoomit, _T( "Filepath" ), 0, &type, (BYTE *) imageFile, &length );
                RegCloseKey( hZoomit );
                if( error == ERROR_SUCCESS ) {		

                    error = RegSetValueEx( hRunKey, APPNAME, 0, REG_SZ, (BYTE *) imageFile,
                        static_cast<DWORD>(_tcslen(imageFile)+1) * sizeof(TCHAR));
                }
            }
        } else {

            error = RegDeleteValue( hRunKey, APPNAME );
            if( error == ERROR_FILE_NOT_FOUND ) error = ERROR_SUCCESS;
        }
        RegCloseKey( hRunKey );
    } 
    if( error != ERROR_SUCCESS ) {

        ErrorDialog( hParent, L"Error configuring auto start", error );
    }
    return error == ERROR_SUCCESS;
}


//--------------------------------------------------------------------
//
// IsAutostartConfigured
//
// Is this version of zoomit configured to autostart.
// 
//--------------------------------------------------------------------
bool IsAutostartConfigured()
{
    HKEY	hRunKey;
    TCHAR	imageFile[MAX_PATH]; 
    DWORD	error, imageFileLength, type;

    error = RegOpenKeyEx( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 
        0, KEY_QUERY_VALUE, &hRunKey );
    if( error == ERROR_SUCCESS ) {

        imageFileLength = sizeof(imageFile);
        error = RegQueryValueEx( hRunKey, APPNAME, 0, &type, (BYTE *) imageFile, &imageFileLength );
        RegCloseKey( hRunKey );
    }
    return error == ERROR_SUCCESS;
}


#ifndef _WIN64

//--------------------------------------------------------------------
//
// RunningOnWin64
//
// Returns true if this is the 32-bit version of the executable
// and we're on 64-bit Windows
// 
//--------------------------------------------------------------------
typedef BOOL (__stdcall *P_IS_WOW64PROCESS)(
            HANDLE hProcess,
            PBOOL Wow64Process
            );
BOOL 
RunningOnWin64(
    VOID 
    )
{
    P_IS_WOW64PROCESS		pIsWow64Process;
    BOOL				isWow64 = FALSE;

    pIsWow64Process = (P_IS_WOW64PROCESS) GetProcAddress(GetModuleHandle(_T("kernel32.dll")),
                            "IsWow64Process");
    if( pIsWow64Process ) {
        
        pIsWow64Process( GetCurrentProcess(), &isWow64 );
    }	 
    return isWow64;
}


//--------------------------------------------------------------------
//
// ExtractImageResource
//
// Extracts the specified file that is located in a resource for 
// this executable.
//
//--------------------------------------------------------------------
BOOLEAN ExtractImageResource( PTCHAR ResourceName, PTCHAR TargetFile )
{
    HRSRC		hResource;
    HGLOBAL		hImageResource;
    DWORD		dwImageSize;    
    LPVOID		lpvImage;
    FILE		*hFile;

    // Locate the resource
    hResource = FindResource( NULL, ResourceName, _T("BINRES") ); 
    if( !hResource ) 
        return FALSE;
    
    hImageResource	= LoadResource( NULL, hResource );
    dwImageSize		= SizeofResource( NULL, hResource );
    lpvImage		= LockResource( hImageResource );

    // Now copy it out
    _tfopen_s( &hFile, TargetFile, _T("wb") );
    if( hFile == NULL ) return FALSE;

    fwrite( lpvImage, 1, dwImageSize, hFile );
    fclose( hFile );
    return TRUE;
}

//--------------------------------------------------------------------
//
// Run64bitVersion
//
// Returns true if this is the 32-bit version of the executable
// and we're on 64-bit Windows
// 
//--------------------------------------------------------------------
DWORD 
Run64bitVersion( 
    void
    )
{
    TCHAR		szPath[MAX_PATH];
    TCHAR		originalPath[MAX_PATH];
    TCHAR		tmpPath[MAX_PATH];
    SHELLEXECUTEINFO	info = { 0 };

    if ( GetModuleFileName( NULL, szPath, sizeof(szPath)/sizeof(TCHAR)) == 0 ) {

        return -1;
    }
    _tcscpy_s( originalPath, _countof(originalPath), szPath );

    *_tcsrchr( originalPath, '.') = 0;
    _tcscat_s( originalPath, _countof(szPath), _T("64.exe"));

    //
    // Extract the 64-bit version
    //
    ExpandEnvironmentStrings( L"%TEMP%", tmpPath, sizeof tmpPath / sizeof ( TCHAR));
    _tcscat_s( tmpPath, _countof(tmpPath), _tcsrchr( originalPath, '\\'));
    _tcscpy_s( szPath, _countof(szPath), tmpPath );
    if( !ExtractImageResource( _T("RCZOOMIT64"), szPath )) {

        if( GetFileAttributes( szPath ) == INVALID_FILE_ATTRIBUTES ) {

            ErrorDialog( NULL,_T("Error launching 64-bit version"), GetLastError());
            return -1;
        }
    }

    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    info.lpFile = szPath;
    info.lpParameters = GetCommandLine();
    info.nShow = SW_SHOWNORMAL;
    if( !ShellExecuteEx( &info ) ) {

        ErrorDialog( NULL,_T("Error launching 64-bit version"), GetLastError());
        DeleteFile( szPath );
        return -1;
    }
    WaitForSingleObject( info.hProcess, INFINITE );

    DWORD result;
    GetExitCodeProcess( info.hProcess, &result );
    CloseHandle( info.hProcess );
    DeleteFile( szPath );
    return result;
}
#endif


//----------------------------------------------------------------------------
//
// IsPresentationMode
//
//----------------------------------------------------------------------------
BOOLEAN IsPresentationMode()
{
    QUERY_USER_NOTIFICATION_STATE pUserState;

    pSHQueryUserNotificationState( &pUserState );
    return pUserState == QUNS_PRESENTATION_MODE;
}

//----------------------------------------------------------------------------
//
// EnableDisableSecondaryDisplay
// 
// Creates a second display on the secondary monitor for displaying the
// break timer. 
//
//----------------------------------------------------------------------------
LONG EnableDisableSecondaryDisplay( HWND hWnd, BOOLEAN Enable, 
                                    PDEVMODE OriginalDevMode ) 
{
    LONG		result;
    DEVMODE		devMode{};

    if( Enable ) {

        //
        // Prepare the position of Display 2 to be right to the right of Display 1
        //
        devMode.dmSize = sizeof(devMode);
        devMode.dmDriverExtra = 0;
        EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &devMode); 
        *OriginalDevMode = devMode;

        //
        // Enable display 2 in the registry
        //
        devMode.dmPosition.x = devMode.dmPelsWidth;
        devMode.dmFields = DM_POSITION |
                            DM_DISPLAYORIENTATION |
                            DM_BITSPERPEL |
                            DM_PELSWIDTH |
                            DM_PELSHEIGHT |
                            DM_DISPLAYFLAGS |
                            DM_DISPLAYFREQUENCY; 
        result = ChangeDisplaySettingsEx( L"\\\\.\\DISPLAY2",
                                          &devMode,
                                          NULL,
                                          CDS_NORESET | CDS_UPDATEREGISTRY,
                                          NULL);

    } else {

        OriginalDevMode->dmFields = DM_POSITION |
                            DM_DISPLAYORIENTATION |
                            DM_BITSPERPEL |
                            DM_PELSWIDTH |
                            DM_PELSHEIGHT |
                            DM_DISPLAYFLAGS |
                            DM_DISPLAYFREQUENCY;
        result = ChangeDisplaySettingsEx( L"\\\\.\\DISPLAY2",
                                          OriginalDevMode,
                                          NULL,
                                          CDS_NORESET | CDS_UPDATEREGISTRY,
                                          NULL);
    }

    //
    // Update the hardware
    //
    if( result == DISP_CHANGE_SUCCESSFUL ) {

        if( !ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL)) {

            result = GetLastError();
        }

        //
        // If enabling, move zoomit to the second monitor
        //
        if( Enable && result == DISP_CHANGE_SUCCESSFUL ) {

            SetWindowPos(FindWindowW(L"ZoomItCustomClass", NULL),
                     NULL,
                     devMode.dmPosition.x,
                     0,
                     0,
                     0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetCursorPos( devMode.dmPosition.x+1, devMode.dmPosition.y+1 );
        }
    }
    return result;
}

//----------------------------------------------------------------------------
//
// GetLineBounds
//
// Gets the rectangle bounding a line, taking into account pen width
// 
//----------------------------------------------------------------------------
Gdiplus::Rect GetLineBounds( POINT p1, POINT p2, int penWidth )
{
    Gdiplus::Rect rect( min(p1.x, p2.x), min(p1.y, p2.y), 
                        abs(p1.x - p2.x), abs( p1.y - p2.y));
    rect.Inflate( penWidth, penWidth );
    return rect;
}

//----------------------------------------------------------------------------
//
// InvalidateGdiplusRect
//
// Invalidate portion of window specified by Gdiplus::Rect
// 
//----------------------------------------------------------------------------
void InvalidateGdiplusRect(HWND hWnd, Gdiplus::Rect BoundsRect)
{
    RECT lineBoundsGdi;
    lineBoundsGdi.left = BoundsRect.X;
    lineBoundsGdi.top = BoundsRect.Y;
    lineBoundsGdi.right = BoundsRect.X + BoundsRect.Width;
    lineBoundsGdi.bottom = BoundsRect.Y + BoundsRect.Height;
    InvalidateRect(hWnd, &lineBoundsGdi, FALSE);
}

//----------------------------------------------------------------------------
//
// ColorFromColorRef
//
// Returns a color object from the colorRef that includes the alpha channel
// 
//----------------------------------------------------------------------------
Gdiplus::Color ColorFromColorRef(DWORD colorRef) {
    BYTE a = (colorRef >> 24) & 0xFF;  // Extract the alpha channel value
    BYTE b = (colorRef >> 16) & 0xFF;  // Extract the red channel value
    BYTE g = (colorRef >> 8) & 0xFF;   // Extract the green channel value
    BYTE r = colorRef & 0xFF;          // Extract the blue channel value
    OutputDebug( L"ColorFromColorRef: %d %d %d %d\n", a, r, g, b );
    return Gdiplus::Color(a, r, g, b);
}

//----------------------------------------------------------------------------
//
// AdjustHighlighterColor
//
// Lighten the color. 
// 
//----------------------------------------------------------------------------
void AdjustHighlighterColor(BYTE* red, BYTE* green, BYTE* blue) {

    // Adjust the color to be more visible
    *red = min( 0xFF, *red ? *red + 0x40 : *red + 0x80 );
    *green = min( 0xFF, *green ? *green + 0x40 : *green + 0x80);
    *blue = min( 0xFF, *blue ? *blue + 0x40 : *blue + 0x80);
}

//----------------------------------------------------------------------------
//
// BlendColors
//
// Blends two colors together using the alpha channel of the second color.
// The highlighter is the second color. 
// 
//----------------------------------------------------------------------------
COLORREF BlendColors(COLORREF color1, const Gdiplus::Color& color2) {

    BYTE redResult, greenResult, blueResult;

    // Extract the channels from the COLORREF
    BYTE red1 = GetRValue(color1);
    BYTE green1 = GetGValue(color1);
    BYTE blue1 = GetBValue(color1);

    // Get the channels and alpha from the Gdiplus::Color
    BYTE blue2 = color2.GetBlue();
    BYTE green2 = color2.GetGreen();
    BYTE red2 = color2.GetRed();
    float alpha2 = color2.GetAlpha() / 255.0f;  // Normalize to [0, 1]
    //alpha2 /= 2; // Use half the alpha for higher contrast

    // Don't blend grey's as much
    // int minValue = min(red1, min(green1, blue1));
    // int maxValue = max(red1, max(green1, blue1));
    if(TRUE) { // red1 > 0x10 && red1 < 0xC0 && (maxValue - minValue < 0x40)) {

        // This does a standard bright highlight	
        alpha2 = 0;
        AdjustHighlighterColor( &red2, &green2, &blue2 );
        redResult	= red2 & red1;
        greenResult = green2 & green1;
        blueResult	= blue2 & blue1;
    }
    else {

        // Blend each channel
        redResult = static_cast<BYTE>(red2 * alpha2 + red1 * (1 - alpha2));
        greenResult = static_cast<BYTE>(green2 * alpha2 + green1 * (1 - alpha2));
        blueResult = static_cast<BYTE>(blue2 * alpha2 + blue1 * (1 - alpha2));
    }
    // Combine the result channels back into a COLORREF
    return RGB(redResult, greenResult, blueResult);
}

//----------------------------------------------------------------------------
//
// DrawHighlightedShape
//
// Draws the shape with the highlighter color.
//
//----------------------------------------------------------------------------
#include "DrawingEffects.h"

//----------------------------------------------------------------------------
//
// CreateFadedDesktopBackground
//
// Creates a snapshot of the desktop that's faded and alpha blended with 
// black.
//
//----------------------------------------------------------------------------
HBITMAP CreateFadedDesktopBackground( HDC hdc, LPRECT rcScreen, LPRECT rcCrop )
{
    // create bitmap
    int		width		= rcScreen->right - rcScreen->left;
    int		height		= rcScreen->bottom - rcScreen->top;
    HDC		hdcScreen	= hdc;
    HDC		hdcMem		= CreateCompatibleDC( hdcScreen );
    HBITMAP	hBitmap		= CreateCompatibleBitmap( hdcScreen, width, height );
    HBITMAP	hOld		= static_cast<HBITMAP>(SelectObject( hdcMem, hBitmap ));
    HBRUSH	hBrush		= CreateSolidBrush(RGB(0, 0, 0));
    
    // start with black background
    FillRect( hdcMem, rcScreen, hBrush );
    if(rcCrop != NULL && rcCrop->left != -1 ) {

        // copy screen contents that are not cropped
        BitBlt(hdcMem, rcCrop->left, rcCrop->top, rcCrop->right - rcCrop->left,
            rcCrop->bottom - rcCrop->top, hdcScreen, rcCrop->left, rcCrop->top, SRCCOPY);
    }

    // blend screen contents into it
    BLENDFUNCTION	blend = { 0 };
    blend.BlendOp				= AC_SRC_OVER;
    blend.BlendFlags			= 0;
    blend.SourceConstantAlpha   = 0x4F;
    blend.AlphaFormat			= 0;
    AlphaBlend( hdcMem,0, 0, width, height, 
                hdcScreen, rcScreen->left, rcScreen->top, 
                width, height, blend );

    SelectObject( hdcMem, hOld );
    DeleteDC( hdcMem );
    DeleteObject(hBrush);
    ReleaseDC( NULL, hdcScreen );

    return hBitmap;
}

//----------------------------------------------------------------------------
//
// AdjustToMoveBoundary
//
// Shifts to accomodate move boundary.
//
//----------------------------------------------------------------------------
void AdjustToMoveBoundary( float zoomLevel, int *coordinate, int cursor, int size, int max )
{
    int diff = static_cast<int> (static_cast<float>(size)/ static_cast<float>(LIVEZOOM_MOVE_REGIONS));
    if( cursor - *coordinate < diff ) 
        *coordinate = max( 0, cursor - diff ); 
    else if( (*coordinate + size) - cursor < diff ) 
        *coordinate = min( cursor + diff - size, max - size );
}

//----------------------------------------------------------------------------
//
// GetZoomedTopLeftCoordinates
//
// Gets the left top coordinate of the zoomed area of the screen
//
//----------------------------------------------------------------------------
void GetZoomedTopLeftCoordinates( float zoomLevel, POINT *cursorPos, int *x, int width, int *y, int height )
{
    // smoother and more natural zoom in
    float scaledWidth = width/zoomLevel;
    float scaledHeight = height/zoomLevel;
    *x = max( 0, min( (int) (width - scaledWidth), (int) (cursorPos->x - (int) (((float) cursorPos->x/ (float) width)*scaledWidth))));
    AdjustToMoveBoundary( zoomLevel, x, cursorPos->x, static_cast<int>(scaledWidth), width );
    *y = max( 0, min( (int) (height - scaledHeight), (int) (cursorPos->y - (int) (((float) cursorPos->y/ (float) height)*scaledHeight))));
    AdjustToMoveBoundary( zoomLevel, y, cursorPos->y, static_cast<int>(scaledHeight), height );
}


//----------------------------------------------------------------------------
//
// ScaleImage
//
// Use gdi+ for anti-aliased bitmap stretching. 
//
//----------------------------------------------------------------------------
void ScaleImage( HDC hdcDst, float xDst, float yDst, float wDst, float hDst, 
                 HBITMAP bmSrc, float xSrc, float ySrc, float wSrc, float hSrc )
{
    Gdiplus::Graphics	dstGraphics( hdcDst );
    {
        Gdiplus::Bitmap		srcBitmap( bmSrc, NULL );

        dstGraphics.SetInterpolationMode( Gdiplus::InterpolationModeLowQuality );
        dstGraphics.SetPixelOffsetMode( Gdiplus::PixelOffsetModeHalf );

        dstGraphics.DrawImage( &srcBitmap, Gdiplus::RectF(xDst,yDst,wDst,hDst), xSrc, ySrc, wSrc, hSrc, Gdiplus::UnitPixel );
    }
}


//----------------------------------------------------------------------------
//
// GetEncoderClsid
//
//----------------------------------------------------------------------------
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
   UINT  num = 0;          // number of image encoders
   UINT  size = 0;         // size of the image encoder array in bytes
using namespace Gdiplus;

   ImageCodecInfo* pImageCodecInfo = NULL;

   GetImageEncodersSize(&num, &size);
   if(size == 0)
      return -1;  // Failure

   pImageCodecInfo = static_cast<ImageCodecInfo*>(malloc(size));
   if(pImageCodecInfo == NULL)
      return -1;  // Failure

   GetImageEncoders(num, size, pImageCodecInfo);

   for(UINT j = 0; j < num; ++j)
   {
      if( wcscmp(pImageCodecInfo[j].MimeType, format) == 0 )
      {
         *pClsid = pImageCodecInfo[j].Clsid;
         free(pImageCodecInfo);
         return j;  // Success
      }    
   }

   free(pImageCodecInfo);
   return -1;  // Failure
}

//----------------------------------------------------------------------
//  
// ConvertToUnicode
//
//----------------------------------------------------------------------
void 
ConvertToUnicode( 
    PCHAR aString, 
    PWCHAR  wString, 
    DWORD wStringLength 
    )
{
    size_t	len;

    len = MultiByteToWideChar( CP_ACP, 0, aString, static_cast<int>(strlen(aString)), 
                wString, wStringLength );
    wString[len] = 0;
}


//----------------------------------------------------------------------------
//
// LoadImageFile
//
// Use gdi+ to load an image.
//
//----------------------------------------------------------------------------
HBITMAP LoadImageFile(PTCHAR filename)
{
    HBITMAP bitmapHandle{};
    Gdiplus::Bitmap bitmap(filename);
    if (bitmap.GetLastStatus() != Gdiplus::Ok ||
        bitmap.GetHBITMAP(Gdiplus::Color(0), &bitmapHandle) != Gdiplus::Ok) return nullptr;
    return bitmapHandle;
}

//----------------------------------------------------------------------------
//
// SavePng
//
// Use gdi+ to save a PNG.
//
//----------------------------------------------------------------------------
DWORD SavePng(PTCHAR filename, HBITMAP bitmapHandle)
{
    if (!bitmapHandle) return ERROR_NOT_ENOUGH_MEMORY;
    Gdiplus::Bitmap bitmap(bitmapHandle, nullptr);
    CLSID pngClsid{};
    if (bitmap.GetLastStatus() != Gdiplus::Ok || GetEncoderClsid(L"image/png", &pngClsid) < 0)
        return ERROR_INVALID_DATA;
    return bitmap.Save(filename, &pngClsid, nullptr) == Gdiplus::Ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

//----------------------------------------------------------------------------
//
// EnableDisableTrayIcon
//
//----------------------------------------------------------------------------
void EnableDisableTrayIcon( HWND hWnd, BOOLEAN Enable )
{
    NOTIFYICONDATA tNotifyIconData;

    memset( &tNotifyIconData, 0, sizeof(tNotifyIconData));
    tNotifyIconData.cbSize = sizeof(NOTIFYICONDATA); 
    tNotifyIconData.hWnd = hWnd; 
    tNotifyIconData.uID = 1; 
    tNotifyIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; 
    tNotifyIconData.uCallbackMessage = WM_USER_TRAY_ACTIVATE; 
    tNotifyIconData.hIcon = LoadIcon( g_hInstance, L"APPICON" ); 
    lstrcpyn(tNotifyIconData.szTip, APPNAME, _countof(tNotifyIconData.szTip));
    Shell_NotifyIcon(Enable ? NIM_ADD : NIM_DELETE, &tNotifyIconData); 
}

//----------------------------------------------------------------------------
//
// EnableDisableOpacity
//
//----------------------------------------------------------------------------
void EnableDisableOpacity( HWND hWnd, BOOLEAN Enable ) 
{
    DWORD	exStyle;

    if( pSetLayeredWindowAttributes && g_BreakOpacity != 100 ) {

        if( Enable ) {

            exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
            SetWindowLong(hWnd, GWL_EXSTYLE, (exStyle | WS_EX_LAYERED));

            pSetLayeredWindowAttributes(hWnd, 0, static_cast<BYTE> ((255 * g_BreakOpacity) / 100), LWA_ALPHA);
            RedrawWindow(hWnd, 0, 0, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);

        } else {

            exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
            SetWindowLong(hWnd, GWL_EXSTYLE, (exStyle & ~WS_EX_LAYERED));
        }
    }
}

//----------------------------------------------------------------------------
//
// EnableDisableScreenSaver
//
//----------------------------------------------------------------------------
void EnableDisableScreenSaver( BOOLEAN Enable ) 
{
    SystemParametersInfo(SPI_SETSCREENSAVEACTIVE,Enable,0,0); 
    SystemParametersInfo(SPI_SETPOWEROFFACTIVE,Enable,0,0); 
    SystemParametersInfo(SPI_SETLOWPOWERACTIVE,Enable,0,0); 
}

//----------------------------------------------------------------------------
//
// EnableDisableStickyKeys
//
//----------------------------------------------------------------------------
void EnableDisableStickyKeys( BOOLEAN Enable )
{
    static STICKYKEYS	prevStickyKeyValue = {0};
    STICKYKEYS			newStickyKeyValue = {0};

    // Need to do this on Vista tablet to stop sticky key popup when you 
    // hold down the shift key and draw with the pen.
    if( Enable ) {

        if( prevStickyKeyValue.cbSize == sizeof(STICKYKEYS)) {

            SystemParametersInfo(SPI_SETSTICKYKEYS, 
                    sizeof(STICKYKEYS), &prevStickyKeyValue, SPIF_SENDCHANGE);
        }

    } else {

        prevStickyKeyValue.cbSize = sizeof(STICKYKEYS);
        if (SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), 
                &prevStickyKeyValue, 0)) {

            newStickyKeyValue.cbSize = sizeof(STICKYKEYS);
            newStickyKeyValue.dwFlags = 0;
            if( !SystemParametersInfo(SPI_SETSTICKYKEYS, 
                sizeof(STICKYKEYS), &newStickyKeyValue, SPIF_SENDCHANGE)) {

                // DWORD error = GetLastError();

            }
        }
    }
}


//----------------------------------------------------------------------------
//
// GetKeyMod
//
//----------------------------------------------------------------------------
constexpr DWORD GetKeyMod( DWORD Key )
{
    DWORD	 keyMod = 0;
    if( (Key >> 8) & HOTKEYF_ALT ) keyMod |= MOD_ALT;
    if( (Key >> 8) & HOTKEYF_CONTROL) keyMod |= MOD_CONTROL;
    if( (Key >> 8) & HOTKEYF_SHIFT) keyMod |= MOD_SHIFT;
    if( (Key >> 8) & HOTKEYF_EXT) keyMod |= MOD_WIN;
    return keyMod;
}


//----------------------------------------------------------------------------
//
// AdvancedBreakProc
//
//----------------------------------------------------------------------------
INT_PTR CALLBACK AdvancedBreakProc( HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam ) 
{
    TCHAR	opacity[10];
    static	TCHAR newSoundFile[MAX_PATH];
    static	TCHAR newBackgroundFile[MAX_PATH];
    TCHAR	filePath[MAX_PATH], initDir[MAX_PATH];
    DWORD	i;
    OPENFILENAME	openFileName;

    switch ( message )  {
    case WM_INITDIALOG:
        if( pSHAutoComplete ) {
            pSHAutoComplete( GetDlgItem( hDlg, IDC_SOUND_FILE), SHACF_FILESYSTEM );
            pSHAutoComplete( GetDlgItem( hDlg, IDC_BACKGROUND_FILE), SHACF_FILESYSTEM );
        }
        CheckDlgButton( hDlg, IDC_CHECK_BACKGROUND_FILE, 
            g_BreakShowBackgroundFile ? BST_CHECKED: BST_UNCHECKED );
        CheckDlgButton( hDlg, IDC_CHECK_SOUND_FILE, 
            g_BreakPlaySoundFile ? BST_CHECKED: BST_UNCHECKED );
        CheckDlgButton( hDlg, IDC_CHECK_SHOW_EXPIRED,
            g_ShowExpiredTime ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hDlg, IDC_CHECK_BACKGROUND_STRETCH,
            g_BreakBackgroundStretch ? BST_CHECKED : BST_UNCHECKED );
#if 0
        CheckDlgButton( hDlg, IDC_CHECK_SECONDARYDISPLAY,
            g_BreakOnSecondary ? BST_CHECKED : BST_UNCHECKED );
#endif
        if( pSetLayeredWindowAttributes == NULL ) {

            EnableWindow( GetDlgItem( hDlg, IDC_OPACITY ), FALSE );
        }

        // sound file
        if( !g_BreakPlaySoundFile ) {

            EnableWindow( GetDlgItem( hDlg, IDC_STATIC_SOUND_FILE ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_SOUND_FILE ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_SOUND_BROWSE ), FALSE );
        }
        _tcscpy( newSoundFile, g_BreakSoundFile );
        _tcscpy( filePath, g_BreakSoundFile );
        if( _tcsrchr( filePath, '\\' )) _tcscpy( filePath, _tcsrchr( g_BreakSoundFile, '\\' )+1);
        if( _tcsrchr( filePath, '.' )) *_tcsrchr( filePath, '.' ) = 0;
        SetDlgItemText( hDlg, IDC_SOUND_FILE, filePath );

        // background file
        if( !g_BreakShowBackgroundFile ) {

            EnableWindow( GetDlgItem( hDlg, IDC_STATIC_DESKTOP_BACKGROUND ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_STATIC_DESKTOP_BACKGROUND ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_STATIC_BACKGROUND_FILE ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_BACKGROUND_FILE ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_BACKGROUND_BROWSE ), FALSE );
            EnableWindow( GetDlgItem( hDlg, IDC_CHECK_BACKGROUND_STRETCH ), FALSE );
        }
        CheckDlgButton( hDlg, 
            g_BreakShowDesktop ? IDC_STATIC_DESKTOP_BACKGROUND : IDC_STATIC_BACKGROUND_FILE, BST_CHECKED );
        _tcscpy( newBackgroundFile, g_BreakBackgroundFile );
        SetDlgItemText( hDlg, IDC_BACKGROUND_FILE, g_BreakBackgroundFile );

        CheckDlgButton( hDlg, IDC_TIMER_POS1 + g_BreakTimerPosition, BST_CHECKED );

        for( i = 10; i <= 100; i += 10) {

            _stprintf( opacity, L"%d%%", i );
            SendMessage( GetDlgItem( hDlg, IDC_OPACITY ), CB_ADDSTRING, 0, 
                    reinterpret_cast<LPARAM>(opacity));
        }
        SendMessage( GetDlgItem( hDlg, IDC_OPACITY ), CB_SETCURSEL, 
                g_BreakOpacity / 10 - 1, 0 );
        return TRUE;

    case WM_COMMAND:
        switch ( HIWORD( wParam )) {
        case BN_CLICKED:
            if( LOWORD( wParam ) == IDC_CHECK_SOUND_FILE ) {

                EnableWindow( GetDlgItem( hDlg, IDC_STATIC_SOUND_FILE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_SOUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_SOUND_FILE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_SOUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_SOUND_BROWSE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_SOUND_FILE) == BST_CHECKED );				
            }
            if( LOWORD( wParam ) == IDC_CHECK_BACKGROUND_FILE ) {

                EnableWindow( GetDlgItem( hDlg, IDC_CHECK_BACKGROUND_STRETCH ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_STATIC_DESKTOP_BACKGROUND ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_STATIC_BACKGROUND_FILE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_BACKGROUND_FILE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE) == BST_CHECKED );
                EnableWindow( GetDlgItem( hDlg, IDC_BACKGROUND_BROWSE ), 
                        IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE) == BST_CHECKED );				
            }
            break;
        }
        switch ( LOWORD( wParam )) {
        case IDC_SOUND_BROWSE:
            memset( &openFileName, 0, sizeof(openFileName ));
            openFileName.lStructSize       = OPENFILENAME_SIZE_VERSION_400;
            openFileName.hwndOwner         = hDlg;
            openFileName.hInstance         = static_cast<HINSTANCE>(g_hInstance);
            openFileName.nMaxFile          = sizeof(filePath)/sizeof(filePath[0]);
            openFileName.Flags				= OFN_LONGNAMES;
            openFileName.lpstrTitle        = L"Specify sound file...";
            openFileName.lpstrDefExt       = L"*.wav";
            openFileName.nFilterIndex      = 1;
            openFileName.lpstrFilter       = L"Sounds\0*.wav\0All Files\0*.*\0";

            GetDlgItemText( hDlg, IDC_SOUND_FILE, filePath, sizeof(filePath ));
            if( _tcsrchr( filePath, '\\' )) {

                _tcscpy( initDir, filePath );
                _tcscpy( filePath, _tcsrchr( initDir, '\\' )+1);
                *(_tcsrchr( initDir, '\\' )+1) = 0;
            } else {

                _tcscpy( filePath, L"%WINDIR%\\Media" );
                ExpandEnvironmentStrings( filePath, initDir, sizeof(initDir)/sizeof(initDir[0]));
                GetDlgItemText( hDlg, IDC_SOUND_FILE, filePath, sizeof(filePath ));
            }
            openFileName.lpstrInitialDir = initDir;
            openFileName.lpstrFile = filePath;
            if( GetOpenFileName( &openFileName )) {

                _tcscpy( newSoundFile, filePath );
                if(_tcsrchr( filePath, '\\' )) _tcscpy( filePath, _tcsrchr( newSoundFile, '\\' )+1);
                if(_tcsrchr( filePath, '.' )) *_tcsrchr( filePath, '.' ) = 0;
                SetDlgItemText( hDlg, IDC_SOUND_FILE, filePath );
            }
            break;

        case IDC_BACKGROUND_BROWSE:
            memset( &openFileName, 0, sizeof(openFileName ));
            openFileName.lStructSize       = OPENFILENAME_SIZE_VERSION_400;
            openFileName.hwndOwner         = hDlg;
            openFileName.hInstance         = static_cast<HINSTANCE>(g_hInstance);
            openFileName.nMaxFile          = sizeof(filePath)/sizeof(filePath[0]);
            openFileName.Flags				= OFN_LONGNAMES;
            openFileName.lpstrTitle        = L"Specify background file...";
            openFileName.lpstrDefExt       = L"*.bmp";
            openFileName.nFilterIndex      = 5;
            openFileName.lpstrFilter       = L"Bitmap Files (*.bmp;*.dib)\0*.bmp;*.dib\0"
                                             "PNG (*.png)\0*.png\0"
                                             "JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)\0*.jpg;*.jpeg;*.jpe;*.jfif\0"
                                             "GIF (*.gif)\0*.gif\0"
                                             "All Picture Files\0.bmp;*.dib;*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif)\0"
                                             "All Files\0*.*\0\0";

            GetDlgItemText( hDlg, IDC_BACKGROUND_FILE, filePath, sizeof(filePath ));
            if(_tcsrchr( filePath, '\\' )) {

                _tcscpy( initDir, filePath );
                _tcscpy( filePath, _tcsrchr( initDir, '\\' )+1);
                *(_tcsrchr( initDir, '\\' )+1) = 0;
            } else {

                _tcscpy( filePath, L"%USERPROFILE%\\Pictures" );
                ExpandEnvironmentStrings( filePath, initDir, sizeof(initDir)/sizeof(initDir[0]));
                GetDlgItemText( hDlg, IDC_BACKGROUND_FILE, filePath, sizeof(filePath ));
            }
            openFileName.lpstrInitialDir = initDir;
            openFileName.lpstrFile = filePath;
            if( GetOpenFileName( &openFileName )) {

                _tcscpy( newBackgroundFile, filePath );
                SetDlgItemText( hDlg, IDC_BACKGROUND_FILE, filePath );
            }
            break;

        case IDOK:

            // sound file has to be valid
            g_BreakPlaySoundFile = IsDlgButtonChecked( hDlg, IDC_CHECK_SOUND_FILE ) == BST_CHECKED;
            g_BreakShowBackgroundFile = IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_FILE ) == BST_CHECKED;
            g_BreakBackgroundStretch = IsDlgButtonChecked( hDlg, IDC_CHECK_BACKGROUND_STRETCH ) == BST_CHECKED;
#if 0
            g_BreakOnSecondary = IsDlgButtonChecked( hDlg, IDC_CHECK_SECONDARYDISPLAY ) == BST_CHECKED;
#endif
            if( g_BreakPlaySoundFile && GetFileAttributes( newSoundFile ) == -1 ) {

                MessageBox( hDlg, L"The specified sound file is inaccessible", 
                        L"Advanced Break Options Error", MB_ICONERROR );
                break;
            }
            _tcscpy( g_BreakSoundFile, newSoundFile );

            // Background file
            g_BreakShowDesktop = IsDlgButtonChecked( hDlg, IDC_STATIC_DESKTOP_BACKGROUND ) == BST_CHECKED;

            if( !g_BreakShowDesktop && g_BreakShowBackgroundFile && GetFileAttributes( newBackgroundFile ) == -1 ) {

                MessageBox( hDlg, L"The specified background file is inaccessible", 
                        L"Advanced Break Options Error", MB_ICONERROR );
                break;
            }
            _tcscpy( g_BreakBackgroundFile, newBackgroundFile );

            for( i = 0; i < 10; i++ ) {

                if( IsDlgButtonChecked( hDlg, IDC_TIMER_POS1+i) == BST_CHECKED ) {

                    g_BreakTimerPosition = i;
                    break;
                }
            }
            GetDlgItemText( hDlg, IDC_OPACITY, opacity, sizeof(opacity)/sizeof(opacity[0])); 
            _stscanf( opacity, L"%d%%", &g_BreakOpacity );
            PersistSettings();
            EndDialog(hDlg, 0);
            break;

        case IDCANCEL:
            EndDialog( hDlg, 0 );
            return TRUE;
        }
        break;

    default:
        break;
    }
    return FALSE;
}


//----------------------------------------------------------------------------
//
// OptionsTabProc
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//
INT_PTR CALLBACK OptionsTabProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    if (message == WM_INITDIALOG) return TRUE;
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_ADVANCED_BREAK) {
        DialogBox(g_hInstance, L"ADVANCED_BREAK", dialog, AdvancedBreakProc);
        return TRUE;
    }
    return FALSE;
}

// OptionsAddTabs
//
//----------------------------------------------------------------------------
VOID OptionsAddTabs( HWND hOptionsDlg, HWND hTabCtrl ) 
{
    int		i;
    TCITEM	tcItem;
    RECT	rc, pageRc;

    GetWindowRect( hTabCtrl, &rc );
    for( i = 0; i < sizeof( g_OptionsTabs )/sizeof(g_OptionsTabs[0]); i++ ) {

        tcItem.mask = TCIF_TEXT;
        tcItem.pszText = g_OptionsTabs[i].TabTitle;
        TabCtrl_InsertItem( hTabCtrl, i, &tcItem );
        g_OptionsTabs[i].hPage = CreateDialog( g_hInstance, g_OptionsTabs[i].TabTitle, 
                    hOptionsDlg, OptionsTabProc );
    }
    TabCtrl_AdjustRect( hTabCtrl, FALSE, &rc );
    for( i = 0; i < sizeof( g_OptionsTabs )/sizeof(g_OptionsTabs[0]); i++ ) {

        pageRc = rc;
        MapWindowPoints( NULL, g_OptionsTabs[i].hPage, reinterpret_cast<LPPOINT>(&pageRc), 2); 

        SetWindowPos( g_OptionsTabs[i].hPage,
             HWND_TOP,
             pageRc.left, pageRc.top,
             pageRc.right - pageRc.left, pageRc.bottom - pageRc.top,
             SWP_NOACTIVATE|(i == 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

        if( pEnableThemeDialogTexture ) {

            pEnableThemeDialogTexture( g_OptionsTabs[i].hPage, ETDT_ENABLETAB );
        }
    }
}

//----------------------------------------------------------------------------
//
// UnregisterAllHotkeys
//
//----------------------------------------------------------------------------
void UnregisterAllHotkeys( HWND hWnd )
{
    UnregisterHotKey( hWnd, ZOOM_HOTKEY);
    UnregisterHotKey( hWnd, LIVE_HOTKEY);
    UnregisterHotKey( hWnd, LIVE_DRAW_HOTKEY);
    UnregisterHotKey( hWnd, DRAW_HOTKEY);
    UnregisterHotKey( hWnd, BREAK_HOTKEY);
    UnregisterHotKey( hWnd, SNIP_HOTKEY );
    UnregisterHotKey( hWnd, SNIP_SAVE_HOTKEY);


}

//----------------------------------------------------------------------------
//
// RegisterAllHotkeys
//
//----------------------------------------------------------------------------
void RegisterAllHotkeys(HWND hWnd)
{
    if (g_ToggleKey) 			RegisterHotKey(hWnd, ZOOM_HOTKEY, g_ToggleMod, g_ToggleKey & 0xFF);
    if (g_LiveZoomToggleKey) {
        RegisterHotKey(hWnd, LIVE_HOTKEY, g_LiveZoomToggleMod, g_LiveZoomToggleKey & 0xFF);
        RegisterHotKey(hWnd, LIVE_DRAW_HOTKEY, (g_LiveZoomToggleMod ^ MOD_SHIFT), g_LiveZoomToggleKey & 0xFF);
    }
    if (g_DrawToggleKey) 		RegisterHotKey(hWnd, DRAW_HOTKEY, g_DrawToggleMod, g_DrawToggleKey & 0xFF);
    if (g_BreakToggleKey) 		RegisterHotKey(hWnd, BREAK_HOTKEY, g_BreakToggleMod, g_BreakToggleKey & 0xFF);
    
    if (g_SnipToggleKey) {
        RegisterHotKey(hWnd, SNIP_HOTKEY, g_SnipToggleMod, g_SnipToggleKey & 0xFF);
        RegisterHotKey(hWnd, SNIP_SAVE_HOTKEY, (g_SnipToggleMod ^ MOD_SHIFT), g_SnipToggleKey & 0xFF);
    }
    
}

//----------------------------------------------------------------------------
//
// UpdateDrawTabHeaderFont
//
//----------------------------------------------------------------------------
HFONT g_DrawHeaderFont = nullptr;
void UpdateDrawTabHeaderFont()
{
    HFONT& headerFont = g_DrawHeaderFont;
    TCHAR 			text[64];

    if( headerFont != nullptr )
    {
        DeleteObject( headerFont );
        headerFont = nullptr;
    }

    constexpr int headers[] = { IDC_PEN_CONTROL, IDC_COLORS, IDC_HIGHLIGHT_AND_BLUR, IDC_SHAPES, IDC_SCREEN };
    for( int i = 0; i < _countof( headers ); i++ )
    {
        // Change the header font to bold
        HWND hHeader = GetDlgItem( g_OptionsTabs[DRAW_PAGE].hPage, headers[i] );
        if( headerFont == nullptr )
        {
            HFONT hFont = reinterpret_cast<HFONT>(SendMessage( hHeader, WM_GETFONT, 0, 0 ));
            LOGFONT lf = {};
            GetObject( hFont, sizeof( LOGFONT ), &lf );
            lf.lfWeight = FW_BOLD;
            headerFont = CreateFontIndirect( &lf );
        }
        SendMessage( hHeader, WM_SETFONT, reinterpret_cast<WPARAM>(headerFont), 0 );

        // Resize the control to fit the text
        GetWindowText( hHeader, text, sizeof( text ) / sizeof( text[0] ) );
        RECT rc;
        GetWindowRect( hHeader, &rc );
        MapWindowPoints( NULL, g_OptionsTabs[DRAW_PAGE].hPage, reinterpret_cast<LPPOINT>(&rc), 2 );
        HDC hDC = GetDC( hHeader );
        auto previousFont = SelectFont(hDC, headerFont);
        DrawText( hDC, text, static_cast<int>(_tcslen( text )), &rc, DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_VCENTER );
        SelectFont(hDC, previousFont);
        ReleaseDC( hHeader, hDC );
        SetWindowPos( hHeader, nullptr, 0, 0, rc.right - rc.left + ScaleForDpi( 4, GetDpiForWindowHelper( hHeader ) ), rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER );
    }
}

//----------------------------------------------------------------------------
//
// OptionsProc
//
//----------------------------------------------------------------------------
INT_PTR CALLBACK OptionsProc( HWND hDlg, UINT message, 
                             WPARAM wParam, LPARAM lParam ) 
{
    static HFONT	hFontBold = nullptr;
    PNMLINK			notify = nullptr;
    static int		curTabSel = 0;
    static HWND fontOwner = nullptr;
    static HWND		hTabCtrl;
    static HWND		hOpacity;
    static HWND		hToggleKey;
    TCHAR			text[32];


    DWORD			newLiveZoomToggleKey, newLiveZoomToggleMod;

    DWORD newToggleKey{}, newTimeout{}, newToggleMod{}, newBreakToggleKey{}, newSnipToggleKey{};
    DWORD newDrawToggleKey{}, newDrawToggleMod{}, newBreakToggleMod{}, newSnipToggleMod{};
    switch ( message )  {
    case WM_INITDIALOG:
    {
        if( hWndOptions ) {

            BringWindowToTop( hWndOptions );
            SetFocus( hWndOptions );
            SetForegroundWindow( hWndOptions );
            EndDialog( hDlg, 0 );
            return FALSE;
        }
        hWndOptions = hDlg;
        fontOwner = hDlg;
        curTabSel = 0;
        SendMessage(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIcon(g_hInstance, L"APPICON")));
        SendMessage(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(
            LoadImage(g_hInstance, L"APPICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                      GetSystemMetrics(SM_CYSMICON), LR_SHARED)));

        SetForegroundWindow( hDlg );
        SetActiveWindow( hDlg );
        SetWindowPos( hDlg, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW ); 
#if 1
        // set version info
        TCHAR               filePath[MAX_PATH];
        const TCHAR* verString;

        GetModuleFileName(NULL, filePath, _countof(filePath));
        DWORD               zero = 0;
        DWORD               infoSize = GetFileVersionInfoSize(filePath, &zero);
        std::vector<BYTE> versionInfo(infoSize);
        if (infoSize && GetFileVersionInfo(filePath, 0, infoSize, versionInfo.data())) {
            verString = GetVersionString(reinterpret_cast<VERSION_INFO*>(versionInfo.data()), _T("FileVersion"));
            if (verString) SetDlgItemText(hDlg, IDC_VERSION, (std::wstring(L"ZoomIt Custom ") + verString).c_str());
            verString = GetVersionString(reinterpret_cast<VERSION_INFO*>(versionInfo.data()), _T("LegalCopyright"));
            if (verString) SetDlgItemText(hDlg, IDC_COPYRIGHT, verString);
        }
#endif
        // Add tabs
        hTabCtrl = GetDlgItem( hDlg, IDC_TAB );
        OptionsAddTabs( hDlg, hTabCtrl );

        InitializeFonts( hDlg, &hFontBold );
        UpdateDrawTabHeaderFont();

        if (g_LiveZoomNeedsWindowsUpdate)
            SetDlgItemText(g_OptionsTabs[LIVE_PAGE].hPage, IDC_LIVE_STATUS,
                           L"LiveZoom unavailable on this Windows build. Please update Windows.");

        // Configure options
        SendMessage( GetDlgItem( g_OptionsTabs[ZOOM_PAGE].hPage, IDC_HOTKEY), HKM_SETRULES, 
            static_cast<WPARAM>(HKCOMB_NONE), // invalid key combinations 
            MAKELPARAM(HOTKEYF_ALT, 0));     // add ALT to invalid entries 

        if( g_ToggleKey )		SendMessage( GetDlgItem( g_OptionsTabs[ZOOM_PAGE].hPage, IDC_HOTKEY), HKM_SETHOTKEY, g_ToggleKey, 0 );
        if( pMagInitialize ) {

            if( g_LiveZoomToggleKey )	SendMessage( GetDlgItem( g_OptionsTabs[LIVE_PAGE].hPage, IDC_LIVE_HOTKEY), HKM_SETHOTKEY, g_LiveZoomToggleKey, 0 );

        } else {

            EnableWindow( GetDlgItem( g_OptionsTabs[LIVE_PAGE].hPage, IDC_LIVE_HOTKEY), FALSE );
            EnableWindow( GetDlgItem( g_OptionsTabs[LIVE_PAGE].hPage, IDC_ZOOM_LEVEL), FALSE );
            EnableWindow( GetDlgItem( g_OptionsTabs[LIVE_PAGE].hPage, IDC_ZOOM_SPIN), FALSE );
        }
        if( g_DrawToggleKey )	SendMessage( GetDlgItem( g_OptionsTabs[DRAW_PAGE].hPage, IDC_DRAW_HOTKEY), HKM_SETHOTKEY, g_DrawToggleKey, 0 );
        if( g_BreakToggleKey )	SendMessage( GetDlgItem( g_OptionsTabs[BREAK_PAGE].hPage, IDC_BREAK_HOTKEY), HKM_SETHOTKEY, g_BreakToggleKey, 0 );
        
        if( g_SnipToggleKey) 	SendMessage( GetDlgItem( g_OptionsTabs[SNIP_PAGE].hPage, IDC_SNIP_HOTKEY), HKM_SETHOTKEY, g_SnipToggleKey, 0 );
        CheckDlgButton( hDlg, IDC_SHOW_TRAY_ICON, 
            g_ShowTrayIcon ? BST_CHECKED: BST_UNCHECKED );
        CheckDlgButton( hDlg, IDC_AUTOSTART, 
            IsAutostartConfigured() ? BST_CHECKED: BST_UNCHECKED );
        CheckDlgButton( g_OptionsTabs[ZOOM_PAGE].hPage, IDC_ANIMATE_ZOOM, 
            g_AnimateZoom ? BST_CHECKED: BST_UNCHECKED );

        SendMessage( GetDlgItem(g_OptionsTabs[ZOOM_PAGE].hPage, IDC_ZOOM_SLIDER), TBM_SETRANGE, false, MAKELONG(0,_countof(g_ZoomLevels)-1) );
        SendMessage( GetDlgItem(g_OptionsTabs[ZOOM_PAGE].hPage, IDC_ZOOM_SLIDER), TBM_SETPOS, true, g_SliderZoomLevel );

        _stprintf( text, L"%d", g_PenWidth );
        SetDlgItemText( g_OptionsTabs[DRAW_PAGE].hPage, IDC_PEN_WIDTH, text );
        SendMessage( GetDlgItem( g_OptionsTabs[DRAW_PAGE].hPage, IDC_PEN_WIDTH ), EM_LIMITTEXT, 1, 0 );
        SendMessage (GetDlgItem( g_OptionsTabs[DRAW_PAGE].hPage, IDC_SPIN), UDM_SETRANGE, 0L, 
                            MAKELPARAM (19, 1));

        _stprintf( text, L"%d", g_BreakTimeout );
        SetDlgItemText( g_OptionsTabs[BREAK_PAGE].hPage, IDC_TIMER, text );
        SendMessage( GetDlgItem( g_OptionsTabs[BREAK_PAGE].hPage, IDC_TIMER ), EM_LIMITTEXT, 2, 0 );
        SendMessage (GetDlgItem( g_OptionsTabs[BREAK_PAGE].hPage, IDC_SPIN_TIMER), UDM_SETRANGE, 0L, 
                            MAKELPARAM (99, 1));
        CheckDlgButton( g_OptionsTabs[BREAK_PAGE].hPage, IDC_CHECK_SHOW_EXPIRED,
            g_ShowExpiredTime ? BST_CHECKED : BST_UNCHECKED );


        UnregisterAllHotkeys(GetParent( hDlg ));
        PostMessage( hDlg, WM_USER, 0, 0 );
        return TRUE;
    }

    case WM_USER+100:
        BringWindowToTop( hDlg );
        SetFocus( hDlg );
        SetForegroundWindow( hDlg );
        return TRUE;

    case WM_DPICHANGED:
        InitializeFonts( hDlg, &hFontBold );
        UpdateDrawTabHeaderFont();
        break;

    case WM_CTLCOLORSTATIC:
        if( reinterpret_cast<HWND>(lParam) == GetDlgItem( hDlg, IDC_TITLE ) || 
            reinterpret_cast<HWND>(lParam) == GetDlgItem(hDlg, IDC_DRAWING) ||
            reinterpret_cast<HWND>(lParam) == GetDlgItem(hDlg, IDC_ZOOM) ||
            reinterpret_cast<HWND>(lParam) == GetDlgItem(hDlg, IDC_BREAK)) {

            HDC	hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode( hdc, TRANSPARENT );
            SelectObject( hdc, hFontBold );
            return PtrToLong(GetSysColorBrush( COLOR_BTNFACE ));
        }
        break;

    case WM_NOTIFY:
        notify = reinterpret_cast<PNMLINK>(lParam);
        if( notify->hdr.idFrom == IDC_LINK )
        {
            switch( notify->hdr.code )
            {
            case NM_CLICK:
            case NM_RETURN:
                ShellExecute( hDlg, _T("open"), notify->item.szUrl, NULL, NULL, SW_SHOWNORMAL );
                break;
            }
        }
        else switch( notify->hdr.code )
        {
        case TCN_SELCHANGE:
            ShowWindow( g_OptionsTabs[curTabSel].hPage, SW_HIDE );
            curTabSel = TabCtrl_GetCurSel(hTabCtrl);
            ShowWindow( g_OptionsTabs[curTabSel].hPage, SW_SHOW );
            break;
        }
        break;

    case WM_COMMAND:
        switch ( LOWORD( wParam )) {
        case IDOK:
        {
            if( !ConfigureAutostart( hDlg, IsDlgButtonChecked( hDlg, IDC_AUTOSTART) == BST_CHECKED )) {

                break;
            }
            g_ShowTrayIcon = IsDlgButtonChecked( hDlg, IDC_SHOW_TRAY_ICON ) == BST_CHECKED;
            g_AnimateZoom = IsDlgButtonChecked( g_OptionsTabs[ZOOM_PAGE].hPage, IDC_ANIMATE_ZOOM ) == BST_CHECKED;


            newToggleKey = static_cast<DWORD>(SendMessage( GetDlgItem( g_OptionsTabs[ZOOM_PAGE].hPage, IDC_HOTKEY), HKM_GETHOTKEY, 0, 0 ));
            newLiveZoomToggleKey = static_cast<DWORD>(SendMessage( GetDlgItem( g_OptionsTabs[LIVE_PAGE].hPage, IDC_LIVE_HOTKEY), HKM_GETHOTKEY, 0, 0 ));
            newDrawToggleKey = static_cast<DWORD>(SendMessage( GetDlgItem( g_OptionsTabs[DRAW_PAGE].hPage, IDC_DRAW_HOTKEY), HKM_GETHOTKEY, 0, 0 ));
            newBreakToggleKey = static_cast<DWORD>(SendMessage( GetDlgItem( g_OptionsTabs[BREAK_PAGE].hPage, IDC_BREAK_HOTKEY), HKM_GETHOTKEY, 0, 0 ));

            newSnipToggleKey = static_cast<DWORD>(SendMessage( GetDlgItem( g_OptionsTabs[SNIP_PAGE].hPage, IDC_SNIP_HOTKEY), HKM_GETHOTKEY, 0, 0 ));

            newToggleMod = GetKeyMod( newToggleKey );
            newLiveZoomToggleMod = GetKeyMod( newLiveZoomToggleKey );
            newDrawToggleMod = GetKeyMod( newDrawToggleKey );
            newBreakToggleMod = GetKeyMod( newBreakToggleKey );

            newSnipToggleMod = GetKeyMod( newSnipToggleKey );

            g_SliderZoomLevel = static_cast<int>(SendMessage( GetDlgItem(g_OptionsTabs[ZOOM_PAGE].hPage, IDC_ZOOM_SLIDER), TBM_GETPOS, 0, 0 ));


            g_ShowExpiredTime = IsDlgButtonChecked(  g_OptionsTabs[BREAK_PAGE].hPage, IDC_CHECK_SHOW_EXPIRED ) == BST_CHECKED;
            GetDlgItemText( g_OptionsTabs[BREAK_PAGE].hPage, IDC_TIMER, text, 3 );
            text[2] = 0;
            newTimeout = _tstoi( text );

            if( newToggleKey && !RegisterHotKey( GetParent( hDlg ), ZOOM_HOTKEY, newToggleMod, newToggleKey & 0xFF )) {

                MessageBox( hDlg, L"The specified zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.",
                    APPNAME, MB_ICONERROR );
                UnregisterAllHotkeys(GetParent( hDlg ));
                break;

            } else if(newLiveZoomToggleKey && 
                (!RegisterHotKey( GetParent( hDlg ), LIVE_HOTKEY, newLiveZoomToggleMod, newLiveZoomToggleKey & 0xFF ) ||
                !RegisterHotKey(GetParent(hDlg), LIVE_DRAW_HOTKEY, (newLiveZoomToggleMod ^ MOD_SHIFT), newLiveZoomToggleKey & 0xFF))) {

                MessageBox( hDlg, L"The specified live-zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.",
                    APPNAME, MB_ICONERROR );
                UnregisterAllHotkeys(GetParent( hDlg ));
                break;

            } else if( newDrawToggleKey && !RegisterHotKey( GetParent( hDlg ), DRAW_HOTKEY, newDrawToggleMod, newDrawToggleKey & 0xFF )) {

                MessageBox( hDlg, L"The specified draw w/out zoom hotkey is already in use.\nSelect a different draw w/out zoom hotkey.",
                    APPNAME, MB_ICONERROR );
                UnregisterAllHotkeys(GetParent( hDlg ));
                break;

            } else if( newBreakToggleKey && !RegisterHotKey( GetParent( hDlg ), BREAK_HOTKEY, newBreakToggleMod, newBreakToggleKey & 0xFF )) {

                MessageBox( hDlg, L"The specified break timer hotkey is already in use.\nSelect a different break timer hotkey.",
                    APPNAME, MB_ICONERROR );
                UnregisterAllHotkeys(GetParent( hDlg ));
                break;

            } 
            else if (newSnipToggleKey && 
                (!RegisterHotKey(GetParent(hDlg), SNIP_HOTKEY, newSnipToggleMod, newSnipToggleKey & 0xFF) ||
                 !RegisterHotKey(GetParent(hDlg), SNIP_SAVE_HOTKEY, (newSnipToggleMod ^ MOD_SHIFT), newSnipToggleKey & 0xFF))) {

                MessageBox(hDlg, L"The specified snip hotkey is already in use.\nSelect a different snip hotkey.",
                    APPNAME, MB_ICONERROR);
                UnregisterAllHotkeys(GetParent(hDlg));
                break;

            }			
             else {
        
                g_BreakTimeout = newTimeout;
                g_ToggleKey = newToggleKey;
                g_LiveZoomToggleKey = newLiveZoomToggleKey;
                g_ToggleMod = newToggleMod;
                g_DrawToggleKey = newDrawToggleKey;
                g_DrawToggleMod = newDrawToggleMod;
                g_BreakToggleKey = newBreakToggleKey;
                g_BreakToggleMod = newBreakToggleMod;


                g_SnipToggleKey = newSnipToggleKey;
                g_SnipToggleMod = newSnipToggleMod;
                PersistSettings();
                EnableDisableTrayIcon( GetParent( hDlg ), g_ShowTrayIcon );

                hWndOptions = NULL;
                EndDialog( hDlg, 0 );
                return TRUE;				
            }
            break;
        }

        case IDCANCEL:
            RegisterAllHotkeys(GetParent(hDlg));
            hWndOptions = NULL;
            EndDialog( hDlg, 0 );
            return TRUE;
        }
        break; 
        
    case WM_DESTROY:
        if (fontOwner == hDlg) {
            DeleteObject(hFontBold); hFontBold = nullptr;
            DeleteObject(g_DrawHeaderFont); g_DrawHeaderFont = nullptr;
            fontOwner = nullptr;
        }
        return FALSE;
    case WM_CLOSE:
        hWndOptions = NULL;
        RegisterAllHotkeys(GetParent(hDlg));
        EndDialog( hDlg, 0 );
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

//----------------------------------------------------------------------------
//
// DeleteDrawUndoList
//
//----------------------------------------------------------------------------
void DeleteDrawUndoList( P_DRAW_UNDO *DrawUndoList )
{
    P_DRAW_UNDO	nextUndo;

    nextUndo = *DrawUndoList;
    while( nextUndo ) {

        *DrawUndoList = nextUndo->Next;
        DeleteDC( nextUndo->hDc );
        DeleteObject( nextUndo->hBitmap );
        free( nextUndo );
        nextUndo = *DrawUndoList;
    }
    *DrawUndoList = NULL;
}

//----------------------------------------------------------------------------
//
// PopDrawUndo
//
//----------------------------------------------------------------------------
BOOLEAN PopDrawUndo( HDC hDc, P_DRAW_UNDO *DrawUndoList, 
                  int width, int height )
{
    P_DRAW_UNDO	nextUndo;

    nextUndo = *DrawUndoList;
    if( nextUndo ) {

        BitBlt( hDc, 0, 0, width, height, 
            nextUndo->hDc, 0, 0, SRCCOPY|CAPTUREBLT );
        *DrawUndoList = nextUndo->Next;
        DeleteDC( nextUndo->hDc );
        DeleteObject( nextUndo->hBitmap );
        free( nextUndo );
        return TRUE;

    } else {

        Beep( 700, 200 );
        return FALSE;
    }
}


//----------------------------------------------------------------------------
//
// DeleteOldestUndo
//
//----------------------------------------------------------------------------
void DeleteOldestUndo(P_DRAW_UNDO* list)
{
    if (!list || !*list) return;
    P_DRAW_UNDO* tail = list;
    while ((*tail)->Next) tail = &(*tail)->Next;
    auto undo = *tail;
    *tail = nullptr;
    DeleteDC(undo->hDc);
    DeleteObject(undo->hBitmap);
    free(undo);
}

//----------------------------------------------------------------------------
//
// GetOldestUndo
// 
//----------------------------------------------------------------------------
P_DRAW_UNDO GetOldestUndo(P_DRAW_UNDO list)
{
    if (!list) return nullptr;
    while (list->Next) list = list->Next;
    return list;
}


//----------------------------------------------------------------------------
//
// PushDrawUndo
//
//----------------------------------------------------------------------------
void PushDrawUndo(HDC dc, P_DRAW_UNDO* list, int width, int height)
{
    if (!dc || !list || width <= 0 || height <= 0) return;
    // Bound history by bytes as well as entries: a 4K snapshot is about 32 MiB.
    const size_t snapshotBytes = static_cast<size_t>(width) * height * 4;
    const size_t capacity = (std::max)(size_t{1},
        (std::min)(size_t{MAX_UNDO_HISTORY}, size_t{64 * 1024 * 1024} / snapshotBytes));
    size_t count = 0;
    for (auto undo = *list; undo; undo = undo->Next) ++count;
    while (count >= capacity) { DeleteOldestUndo(list); --count; }
    auto bitmap = CreateCompatibleBitmap(dc, width, height);
    if (!bitmap && *list) {
        DeleteOldestUndo(list);
        bitmap = CreateCompatibleBitmap(dc, width, height);
    }
    if (!bitmap) return;
    auto undo = static_cast<P_DRAW_UNDO>(calloc(1, sizeof(DRAW_UNDO)));
    if (!undo) { DeleteObject(bitmap); return; }
    undo->hDc = CreateCompatibleDC(dc);
    undo->hBitmap = bitmap;
    if (!undo->hDc || !SelectObject(undo->hDc, bitmap) ||
        !BitBlt(undo->hDc, 0, 0, width, height, dc, 0, 0, SRCCOPY)) {
        DeleteDC(undo->hDc);
        DeleteObject(bitmap);
        free(undo);
        return;
    }
    undo->Next = *list;
    *list = undo;
}

//----------------------------------------------------------------------------
//

//
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//
// BlankScreenArea
//
//----------------------------------------------------------------------------
void BlankScreenArea( HDC hDc, PRECT Rc, int BlankMode )
{
    if( BlankMode == 'K' ) {

        HBRUSH hBrush = CreateSolidBrush( RGB( 0, 0, 0 ));
        FillRect( hDc, Rc, hBrush );
        DeleteObject( static_cast<HGDIOBJ>(hBrush) );

    } else {

        FillRect( hDc, Rc, GetSysColorBrush( COLOR_WINDOW ));
    }
}

//----------------------------------------------------------------------------
//

//
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//

//
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//
// BoundMouse
//
//----------------------------------------------------------------------------
RECT BoundMouse( float zoomLevel, MONITORINFO *monInfo, int width, int height,
                    POINT *cursorPos )
{
    RECT		rc;
    int			x, y;

    GetZoomedTopLeftCoordinates( zoomLevel, cursorPos, &x, width, &y, height );
    rc.left = monInfo->rcMonitor.left + x; 
    rc.right = rc.left + static_cast<int>(width/zoomLevel);
    rc.top = monInfo->rcMonitor.top + y;
    rc.bottom = rc.top + static_cast<int>(height/zoomLevel);

    OutputDebug( L"x: %d y: %d width: %d height: %d zoomLevel: %g\n",
        cursorPos->x, cursorPos->y, width, height, zoomLevel);
    OutputDebug( L"left: %d top: %d right: %d bottom: %d\n",
            rc.left, rc.top, rc.right, rc.bottom);
    OutputDebug( L"mon.left: %d mon.top: %d mon.right: %d mon.bottom: %d\n",
        monInfo->rcMonitor.left, monInfo->rcMonitor.top, monInfo->rcMonitor.right, monInfo->rcMonitor.bottom);
    
    ClipCursor( &rc );
    return rc;
}

//----------------------------------------------------------------------------
//
// DrawArrow
//
//----------------------------------------------------------------------------
void DrawArrow( HDC hdc, int x1, int y1, int x2, int y2, double length, double width,
        bool UseGdiplus )
{
    // get normalized dx/dy
    double dx = static_cast<double>(x2) - x1;
    double dy = static_cast<double>(y2) - y1;
    double bodyLen = sqrt( dx*dx + dy*dy );
    if ( bodyLen )  {
        dx /= bodyLen;
        dy /= bodyLen;
    } else {
        dx = 1;
        dy = 0;
    }

    // get midpoint of base
    int xMid = x2 - static_cast<int>(length*dx+0.5);
    int yMid = y2 - static_cast<int>(length*dy+0.5);
    
    // get left wing
    int xLeft = xMid - static_cast<int>(dy*width+0.5);
    int yLeft = yMid + static_cast<int>(dx*width+0.5);

    // get right wing
    int xRight = xMid + static_cast<int>(dy*width+0.5);
    int yRight = yMid - static_cast<int>(dx*width+0.5);

    // Bring in midpoint to make a nicer arrow
    xMid = x2 - static_cast<int>(length/2*dx+0.5);
    yMid = y2 - static_cast<int>(length/2*dy+0.5);
    if (UseGdiplus) {

        Gdiplus::Graphics	dstGraphics(hdc);

        if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
        {
            dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        }
        Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
        Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
#if 0
        Gdiplus::PointF	pts[] = {
            {(Gdiplus::REAL)x1, (Gdiplus::REAL)y1},
            {(Gdiplus::REAL)xMid, (Gdiplus::REAL)yMid},
            {(Gdiplus::REAL)xLeft, (Gdiplus::REAL)yLeft},
            {(Gdiplus::REAL)x2, (Gdiplus::REAL)y2},
            {(Gdiplus::REAL)xRight, (Gdiplus::REAL)yRight},
            {(Gdiplus::REAL)xMid, (Gdiplus::REAL)yMid}
        };
        dstGraphics.DrawPolygon(&pen, pts, _countof(pts));
#else
        Gdiplus::GraphicsPath path;
        path.StartFigure();
        path.AddLine(static_cast<INT>(x1), static_cast<INT>(y1), static_cast<INT>(x2), static_cast<INT>(y2));
        path.AddLine(static_cast<INT>(x2), static_cast<INT>(y2), static_cast<INT>(xMid), static_cast<INT>(yMid));
        path.AddLine(static_cast<INT>(xMid), static_cast<INT>(yMid), static_cast<INT>(xLeft), static_cast<INT>(yLeft));
        path.AddLine(static_cast<INT>(xLeft), static_cast<INT>(yLeft), static_cast<INT>(x2), static_cast<INT>(y2));
        path.AddLine(static_cast<INT>(x2), static_cast<INT>(y2), static_cast<INT>(xRight), static_cast<INT>(yRight));
        path.AddLine(static_cast<INT>(xRight), static_cast<INT>(yRight), static_cast<INT>(xMid), static_cast<INT>(yMid));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        dstGraphics.DrawPath(&pen, &path);
#endif
    }
    else {
        POINT	pts[] = {
            x1, y1,
            xMid, yMid,
            xLeft, yLeft,
            x2, y2,
            xRight, yRight,
            xMid, yMid
        };

        // draw arrow head filled with current color
        HBRUSH hBrush = CreateSolidBrush(g_PenColor);
        HBRUSH hOldBrush = SelectBrush(hdc, hBrush);
        Polygon(hdc, pts, sizeof(pts) / sizeof(pts[0]));

        DeleteObject(hBrush);
        SelectObject(hdc, hOldBrush);
    }
}

//----------------------------------------------------------------------------
//
// DrawShape
//
//----------------------------------------------------------------------------
VOID DrawShape( DWORD Shape, HDC hDc, RECT *Rect, bool UseGdiPlus = false )
{
    bool	isBlur = false;

    Gdiplus::Graphics	dstGraphics(hDc);

	if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
	{
		dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	}
    Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
    Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

    // Check for highlighting or blur
    Gdiplus::Brush *pBrush = NULL;
    if (PEN_COLOR_HIGHLIGHT(g_PenColor)) {
        // Use half the alpha for higher contrast
        DWORD newColor = g_PenColor & 0xFFFFFF | ((g_AlphaBlend / 2) << 24);
        pBrush = new Gdiplus::SolidBrush(ColorFromColorRef(newColor));
        if(UseGdiPlus && Shape != DRAW_LINE && Shape != DRAW_ARROW)
            InflateRect(Rect, g_PenWidth/2, g_PenWidth/2);
    }
    else if ((g_PenColor & 0xFFFFFF) == COLOR_BLUR) {
        if (UseGdiPlus && Shape != DRAW_LINE && Shape != DRAW_ARROW)
            InflateRect(Rect, g_PenWidth / 2, g_PenWidth / 2);
        isBlur = true;
    }

    switch (Shape) {
    case DRAW_RECTANGLE:
        if (UseGdiPlus)
            if(pBrush)
                DrawHighlightedShape(DRAW_RECTANGLE, hDc, pBrush, NULL, 
                    static_cast<int>(Rect->left - 1), static_cast<int>(Rect->top - 1),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom));
            else if (isBlur)
                DrawBlurredShape( DRAW_RECTANGLE, &pen, hDc, &dstGraphics,
                    static_cast<int>(Rect->left - 1), static_cast<int>(Rect->top - 1),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom) );
            else
                dstGraphics.DrawRectangle(&pen,
                    Gdiplus::Rect::Rect(Rect->left - 1, Rect->top - 1,
                        Rect->right - Rect->left, Rect->bottom - Rect->top));
        else
            Rectangle(hDc, Rect->left, Rect->top,
                Rect->right, Rect->bottom);
        break;
    case DRAW_ELLIPSE:
        if (UseGdiPlus)
            if (pBrush)
                DrawHighlightedShape(DRAW_ELLIPSE, hDc, pBrush, NULL,
                    static_cast<int>(Rect->left - 1), static_cast<int>(Rect->top - 1),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom));
            else if (isBlur)
                DrawBlurredShape( DRAW_ELLIPSE, &pen, hDc, &dstGraphics,
                    static_cast<int>(Rect->left - 1), static_cast<int>(Rect->top - 1),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom));
            else
                dstGraphics.DrawEllipse(&pen,
                    Gdiplus::Rect::Rect(Rect->left - 1, Rect->top - 1,
                        Rect->right - Rect->left, Rect->bottom - Rect->top));
        else
            Ellipse(hDc, Rect->left, Rect->top,
                Rect->right, Rect->bottom);
        break;
    case DRAW_LINE:
        if (UseGdiPlus)
            if (pBrush)
                DrawHighlightedShape(DRAW_LINE, hDc, NULL, &pen,
                    static_cast<int>(Rect->left), static_cast<int>(Rect->top),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom));
            else if (isBlur)
                DrawBlurredShape(DRAW_LINE, &pen, hDc, &dstGraphics,
                    static_cast<int>(Rect->left), static_cast<int>(Rect->top),
                    static_cast<int>(Rect->right), static_cast<int>(Rect->bottom));
            else
                dstGraphics.DrawLine(&pen,
                    static_cast<INT>(Rect->left - 1), static_cast<INT>(Rect->top - 1),
                    static_cast<INT>(Rect->right), static_cast<INT>(Rect->bottom));
        else {
            MoveToEx(hDc, Rect->left, Rect->top, NULL);
            LineTo(hDc, Rect->right + 1, Rect->bottom + 1);
        }
        break;
    case DRAW_ARROW:
        DrawArrow(hDc, Rect->right + 1, Rect->bottom + 1,
            Rect->left, Rect->top,
            static_cast<double>(g_PenWidth) * 2.5, static_cast<double>(g_PenWidth) * 1.5, UseGdiPlus);
        break;
    }
    if( pBrush ) delete pBrush;
}

//----------------------------------------------------------------------------
//
// SendPenMessage
//
// Inserts the pen message marker.
//
//----------------------------------------------------------------------------
VOID SendPenMessage(HWND hWnd, UINT Message, LPARAM lParam)
{
    WPARAM		wParam = 0;
    //
    // Get key states
    //
    if(GetKeyState(VK_LCONTROL) < 0 ) {

        wParam |= MK_CONTROL;
    } 
    if( GetKeyState( VK_LSHIFT) < 0 || GetKeyState( VK_RSHIFT) < 0 ) {

        wParam |= MK_SHIFT;
    }
    SetMessageExtraInfo(static_cast<LPARAM>(MI_WP_SIGNATURE));
    SendMessage(hWnd, Message, wParam, lParam);
}


//----------------------------------------------------------------------------
//
// ScalePenPosition
// 
// Maps pen input to mouse input coordinates based on zoom level. Returns
// 0 if pen is active but we didn't send this message to ourselves (pen
// signature will be missing). 
//
//----------------------------------------------------------------------------
LPARAM ScalePenPosition( float zoomLevel, MONITORINFO *monInfo, RECT boundRc,
                    UINT message, LPARAM lParam )
{
    RECT	rc;
    WORD	x, y;
    LPARAM	extraInfo;

    extraInfo = GetMessageExtraInfo();
    if( g_PenDown ) { 

        // ignore messages we didn't tag as pen
        if (extraInfo == MI_WP_SIGNATURE) {

            OutputDebug( L"Tablet Pen message\n");

            // tablet input: don't bound the cursor
            ClipCursor(NULL);

            x = LOWORD(lParam);
            y = HIWORD(lParam);

            x = static_cast<WORD>((x - static_cast<WORD>(monInfo->rcMonitor.left))/ zoomLevel) + static_cast<WORD>(boundRc.left - monInfo->rcMonitor.left);
            y = static_cast<WORD>((y - static_cast<WORD>(monInfo->rcMonitor.top)) / zoomLevel) + static_cast<WORD>(boundRc.top - monInfo->rcMonitor.top);

            lParam = MAKELPARAM(x, y);
        }
        else {

            OutputDebug(L"Ignore pen message we didn't send\n");
            lParam = 0;
        }
    
    } else {

        if( !GetClipCursor( &rc )) {

            ClipCursor( &boundRc );
        }
        OutputDebug( L"Mouse message\n");
    }
    return lParam;
} 


//----------------------------------------------------------------------------
//
// DrawHighlightedCursor
//
//----------------------------------------------------------------------------
BOOLEAN DrawHighlightedCursor( float ZoomLevel, int Width, int Height )
{
    DWORD zoomWidth = static_cast<DWORD> (static_cast<float>(Width)/ZoomLevel);
    DWORD zoomHeight = static_cast<DWORD> (static_cast<float>(Height)/ZoomLevel);
    if( g_PenWidth < 5 && zoomWidth > g_PenWidth * 100 && zoomHeight > g_PenWidth * 100 ) {

        return TRUE;

    } else {

        return FALSE;
    }
}

//----------------------------------------------------------------------------
//
// InvalidateCursorMoveArea
//
//----------------------------------------------------------------------------
void InvalidateCursorMoveArea( HWND hWnd, float zoomLevel, int width, int height, 
                              POINT currentPt, POINT prevPt, POINT cursorPos )
{
    int		x, y;
    RECT	rc;
    int		invWidth = g_PenWidth;

    if( DrawHighlightedCursor( zoomLevel, width, height ) ) {
        
        invWidth = g_PenWidth * 3 + 1;
    }
    GetZoomedTopLeftCoordinates( zoomLevel, &cursorPos, &x, width, &y, height );
    rc.left = static_cast<int>(max( 0, (int) ((min( prevPt.x, currentPt.x)-invWidth - x) * zoomLevel)));
    rc.right = static_cast<int>((max( prevPt.x, currentPt.x)+invWidth - x) * zoomLevel);
    rc.top = static_cast<int>(max( 0, (int) ((min( prevPt.y, currentPt.y)-invWidth - y) * zoomLevel)));
    rc.bottom = static_cast<int>((max( prevPt.y, currentPt.y)+invWidth -y) * zoomLevel);
    InvalidateRect( hWnd, &rc, FALSE );

    OutputDebug( L"INVALIDATE: (%d, %d) - (%d, %d)\n", rc.left, rc.top, rc.right, rc.bottom);
}


//----------------------------------------------------------------------------
//
// SavCursorArea
//
//----------------------------------------------------------------------------
void SaveCursorArea( HDC hDcTarget, HDC hDcSource, POINT pt )
{
    OutputDebug( L"SaveCursorArea\n");
    int penWidth = g_PenWidth + 2;
    BitBlt( hDcTarget, 0, 0, penWidth +CURSOR_ARM_LENGTH*2, penWidth +CURSOR_ARM_LENGTH*2,
        hDcSource, static_cast<INT> (pt.x- penWidth /2)-CURSOR_ARM_LENGTH,
        static_cast<INT>(pt.y- penWidth /2)-CURSOR_ARM_LENGTH, SRCCOPY|CAPTUREBLT );
}

//----------------------------------------------------------------------------
//
// RestoreCursorArea
//
//----------------------------------------------------------------------------
void RestoreCursorArea( HDC hDcTarget, HDC hDcSource, POINT pt )
{
    OutputDebug( L"RestoreCursorArea\n");
    int penWidth = g_PenWidth + 2;
    BitBlt( hDcTarget, static_cast<INT>(pt.x- penWidth /2)-CURSOR_ARM_LENGTH,
        static_cast<INT>(pt.y- penWidth /2)-CURSOR_ARM_LENGTH, penWidth +CURSOR_ARM_LENGTH*2,
        penWidth + CURSOR_ARM_LENGTH*2, hDcSource, 0, 0, SRCCOPY|CAPTUREBLT );
}


//----------------------------------------------------------------------------
//
// DrawCursor
//
//----------------------------------------------------------------------------
void DrawCursor( HDC hDcTarget, POINT pt, float ZoomLevel, int Width, int Height )
{
    RECT	rc;

    if( g_DrawPointer ) {

        Gdiplus::Graphics	dstGraphics(hDcTarget);
        if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
        {
            dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        }
        Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
        Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));

        rc.left = pt.x - CURSOR_ARM_LENGTH;
        rc.right = pt.x + CURSOR_ARM_LENGTH;
        rc.top = pt.y - CURSOR_ARM_LENGTH;
        rc.bottom = pt.y + CURSOR_ARM_LENGTH;

        Gdiplus::GraphicsPath path;
        path.StartFigure();
        path.AddLine(static_cast<INT>(rc.left) - 1, static_cast<INT>(rc.top) - 1, static_cast<INT>(rc.right), static_cast<INT>(rc.bottom));
        path.AddLine(static_cast<INT>(rc.left) - 2, static_cast<INT>(rc.top) - 1, rc.left + (rc.right - rc.left) / 2, rc.top - 1);
        path.AddLine(static_cast<INT>(rc.left) - 1, static_cast<INT>(rc.top) - 2, rc.left - 1, rc.top + (rc.bottom - rc.top) / 2);
        path.AddLine(static_cast<INT>(rc.left) - 1, static_cast<INT>(rc.top) - 2, rc.left - 1, rc.top + (rc.bottom - rc.top) / 2);
        path.AddLine(static_cast<INT>(rc.left + (rc.right - rc.left) / 2), rc.top - 1, rc.left - 1, rc.top + (rc.bottom - rc.top) / 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        dstGraphics.DrawPath(&pen, &path);
        OutputDebug(L"DrawPointer: %d %d %d %d\n", rc.left, rc.top, rc.right, rc.bottom);

    } else if( DrawHighlightedCursor( ZoomLevel, Width, Height )) {

        OutputDebug(L"DrawHighlightedCursor: %d %d %d %d\n", pt.x, pt.y, g_PenWidth, g_PenWidth);
        Gdiplus::Graphics	dstGraphics(hDcTarget);
        if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
        {
            dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        }
        Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
        Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
        Gdiplus::GraphicsPath path;
        path.StartFigure();
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        path.AddLine(static_cast<INT>(pt.x - CURSOR_ARM_LENGTH), pt.y, pt.x + CURSOR_ARM_LENGTH, pt.y);
        path.CloseFigure();
        path.StartFigure();
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        path.AddLine(static_cast<INT>(pt.x), pt.y - CURSOR_ARM_LENGTH, pt.x, pt.y + CURSOR_ARM_LENGTH);
        path.CloseFigure();
        dstGraphics.DrawPath(&pen, &path);

    } else {

        Gdiplus::Graphics	dstGraphics(hDcTarget);
        {
            if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
            {
                dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            }
            Gdiplus::Color	color = ColorFromColorRef(g_PenColor);

            Gdiplus::SolidBrush solidBrush(color);

            dstGraphics.FillEllipse(&solidBrush, static_cast<INT>(pt.x-g_PenWidth/2), static_cast<INT>(pt.y-g_PenWidth/2),
                        static_cast<INT>(g_PenWidth), static_cast<INT>(g_PenWidth));
        }
    }
}

//----------------------------------------------------------------------------
//
// ResizePen
//
//----------------------------------------------------------------------------
void ResizePen( HWND hWnd, HDC hdcScreenCompat, HDC hdcScreenCursorCompat, POINT prevPt,
                BOOLEAN g_Tracing, BOOLEAN *g_Drawing, float g_LiveZoomLevel, 
                BOOLEAN isUser, int newWidth )
{
    if( !g_Tracing ) {

        RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );
    }

    OutputDebug( L"RESIZE_PEN-PRE: penWidth: %d ", g_PenWidth );
    int prevWidth = g_PenWidth;
    if( g_ZoomOnLiveZoom )
    {
        if( isUser )
        {
            // Amplify user delta proportional to LiveZoomLevel
            newWidth = g_PenWidth + static_cast<int> ((newWidth - static_cast<int>(g_PenWidth))*g_LiveZoomLevel);
        }

        g_PenWidth = min( max( newWidth, MIN_PEN_WIDTH ),
            min( static_cast<int>(MAX_PEN_WIDTH * g_LiveZoomLevel), MAX_LIVE_PEN_WIDTH ) );
        g_RootPenWidth = static_cast<int>(g_PenWidth / g_LiveZoomLevel);
    }
    else
    {
        g_PenWidth = min( max( newWidth, MIN_PEN_WIDTH ), MAX_PEN_WIDTH );
        g_RootPenWidth = g_PenWidth;
    }

    if(prevWidth == static_cast<int>(g_PenWidth) ) {
        // No change
        return;
    }

    OutputDebug( L"newWidth: %d\nRESIZE_PEN-POST: penWidth: %d\n", newWidth, g_PenWidth );
    PersistSettings();
    SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );
    *g_Drawing = FALSE;
    EnableDisableStickyKeys( TRUE );
    SendMessage( hWnd, WM_LBUTTONDOWN, static_cast<WPARAM>(-1), MAKELPARAM(prevPt.x, prevPt.y) );
}

//----------------------------------------------------------------------------
//
// IsPenInverted
//
//----------------------------------------------------------------------------
bool IsPenInverted( WPARAM wParam )
{
    POINTER_INPUT_TYPE pointerType;
    POINTER_PEN_INFO penInfo;
    return
        pGetPointerType( GET_POINTERID_WPARAM( wParam ), &pointerType ) && ( pointerType == PT_PEN ) &&
        pGetPointerPenInfo( GET_POINTERID_WPARAM( wParam ), &penInfo ) && ( penInfo.penFlags & PEN_FLAG_INVERTED );
}


//----------------------------------------------------------------------------
//
// UpdateMonitorInfo
//
//----------------------------------------------------------------------------
void UpdateMonitorInfo( POINT point, MONITORINFO* monInfo )
{
    HMONITOR hMon{};
    if( pMonitorFromPoint != nullptr )
    {
        hMon = pMonitorFromPoint( point, MONITOR_DEFAULTTONEAREST );
    }
    if( hMon != nullptr )
    {
        monInfo->cbSize = sizeof *monInfo;
        pGetMonitorInfo( hMon, monInfo );
    }
    else
    {
        *monInfo = {};
        HDC hdcScreen = CreateDC( L"DISPLAY", nullptr, nullptr, nullptr );
        if( hdcScreen != nullptr )
        {
            monInfo->rcMonitor.right = GetDeviceCaps( hdcScreen, HORZRES );
            monInfo->rcMonitor.bottom = GetDeviceCaps( hdcScreen, VERTRES );
            DeleteDC( hdcScreen );
        }
    }
}


//----------------------------------------------------------------------------
//
// ShowMainWindow
//
//----------------------------------------------------------------------------
void ShowMainWindow(HWND hWnd, const MONITORINFO& monInfo, int width, int height)
{
    // Show the window first
    SetWindowPos(hWnd, HWND_TOPMOST, monInfo.rcMonitor.left, monInfo.rcMonitor.top,
        width, height, SWP_SHOWWINDOW | SWP_NOCOPYBITS);

    // Now invalidate and update the window
    InvalidateRect(hWnd, NULL, TRUE);
    UpdateWindow(hWnd);

    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
}

//----------------------------------------------------------------------------
//
// MainWndProc
//
//----------------------------------------------------------------------------
LRESULT APIENTRY MainWndProc(
    HWND hWnd,     
    UINT message,
    WPARAM wParam, 
    LPARAM lParam)
{
    static int		width, height;
    static HDC		hdcScreen, hdcScreenCompat, hdcScreenCursorCompat, hdcScreenSaveCompat;
    static HBITMAP	hbmpCompat, hbmpDrawingCompat, hbmpCursorCompat;
    static BITMAP	bmp;
    static BOOLEAN	g_TimerActive = FALSE;
    static BOOLEAN	g_Zoomed = FALSE;


    static DEVMODE	secondaryDevMode;
    static RECT		g_LiveZoomSourceRect;
    static float	g_LiveZoomLevel;
    static float	zoomLevel;
    static float	zoomTelescopeStep;
    static float	zoomTelescopeTarget;
    static POINT	cursorPos;
    static POINT	savedCursorPos;

    static RECT		boundRc;
    static POINT	prevPt;


    static P_DRAW_UNDO drawUndoList = NULL;

    static BOOLEAN	g_HaveDrawn = FALSE;
    static DWORD	g_DrawingShape = 0;
    static DWORD    prevPenWidth = g_PenWidth;
    static POINT	g_RectangleAnchor;
    static RECT		g_rcRectangle;
    static BOOLEAN	g_Tracing = FALSE;
    static int		g_BlankedScreen = 0;
    static int		g_StraightDirection = 0;
    static BOOLEAN	g_Drawing = FALSE;
    static HWND		g_ActiveWindow = NULL;
    static int		breakTimeout;
    static HBITMAP	g_hBackgroundBmp = NULL;
    static HDC		g_hDcBackgroundFile;
    static HPEN		hDrawingPen;
    static HFONT	hTimerFont;
    static HFONT	hNegativeTimerFont;

    static MONITORINFO	monInfo;
    static MONITORINFO  lastMonInfo;
    static HWND		hTargetWindow = NULL;
    static RECT		rcTargetWindow;
    static BOOLEAN  forcePenResize = TRUE;
    static BOOLEAN  activeBreakShowDesktop = g_BreakShowDesktop;
    static BOOLEAN	activeBreakShowBackgroundFile = g_BreakShowBackgroundFile;
    static TCHAR    activeBreakBackgroundFile[MAX_PATH] = {0};
    static UINT     wmTaskbarCreated;
#if 0
    TITLEBARINFO	titleBarInfo;
    WINDOWINFO		targetWindowInfo;
#endif
    RECT			rc, rc1;
    PAINTSTRUCT		ps; 
    TCHAR			timerText[16];
    TCHAR			negativeTimerText[16];
    BOOLEAN			penInverted;
    BOOLEAN			zoomIn;
    HDC				hDc;
    int				x, y, delta;
    HMENU			hPopupMenu;
    OPENFILENAME	openFileName;
    static TCHAR	filePath[MAX_PATH] = {L"zoomit"};
    NOTIFYICONDATA	tNotifyIconData;

    const auto releaseSession = [&]() {
        DeleteDrawUndoList(&drawUndoList);

        DeleteDC(hdcScreenCompat); hdcScreenCompat = nullptr;
        DeleteDC(hdcScreenCursorCompat); hdcScreenCursorCompat = nullptr;
        DeleteDC(hdcScreenSaveCompat); hdcScreenSaveCompat = nullptr;
        DeleteDC(hdcScreen); hdcScreen = nullptr;
        DeleteObject(hbmpCompat); hbmpCompat = nullptr;
        DeleteObject(hbmpCursorCompat); hbmpCursorCompat = nullptr;
        DeleteObject(hbmpDrawingCompat); hbmpDrawingCompat = nullptr;
        DeleteObject(hDrawingPen); hDrawingPen = nullptr;

        DeleteObject(hTimerFont); hTimerFont = nullptr;
        DeleteObject(hNegativeTimerFont); hNegativeTimerFont = nullptr;
    };

    switch (message) {
    case WM_CREATE:

        // get default font
        GetObject( GetStockObject(DEFAULT_GUI_FONT), sizeof g_LogFont, &g_LogFont ); 
        g_LogFont.lfWeight = FW_NORMAL;
        hDc = CreateCompatibleDC( NULL );
        g_LogFont.lfHeight = -MulDiv(8, GetDeviceCaps(hDc, LOGPIXELSY), 72);
        DeleteDC( hDc );

        reg.ReadRegSettings( RegSettings );
        if (MigrateHotkeys()) PersistSettings();
        ValidateSettings();
        
        // to support migrating from 
        if ((g_PenColor >> 24) == 0) {
            g_PenColor |= 0xFF << 24;
        }

        g_PenWidth = g_RootPenWidth;

        g_ToggleMod = GetKeyMod( g_ToggleKey );
        g_LiveZoomToggleMod = GetKeyMod( g_LiveZoomToggleKey );
        g_DrawToggleMod = GetKeyMod( g_DrawToggleKey );
        g_BreakToggleMod = GetKeyMod( g_BreakToggleKey );

        g_SnipToggleMod = GetKeyMod( g_SnipToggleKey );

#ifdef ZOOMIT_TESTING
        if (g_TestMode) return TRUE;
#endif
        if( !g_OptionsShown && !g_StartedByPowerToys ) {
            // First run should show options when running as standalone. If not running as standalone,
            // options screen won't show and we should register keys instead.
            SendMessage( hWnd, WM_COMMAND, IDC_OPTIONS, 0 );
            g_OptionsShown = TRUE;
            PersistSettings();
        } else {
            BOOL	showOptions = FALSE;

            if( g_ToggleKey && !RegisterHotKey( hWnd, ZOOM_HOTKEY, g_ToggleMod, g_ToggleKey & 0xFF)) {

                MessageBox( hWnd, L"The specified zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.",
                    APPNAME, MB_ICONERROR );
                showOptions = TRUE;

            } else if( g_LiveZoomToggleKey && 
                (!RegisterHotKey( hWnd, LIVE_HOTKEY, g_LiveZoomToggleMod, g_LiveZoomToggleKey & 0xFF) ||
                    !RegisterHotKey(hWnd, LIVE_DRAW_HOTKEY, (g_LiveZoomToggleMod ^ MOD_SHIFT), g_LiveZoomToggleKey & 0xFF))) {

                MessageBox( hWnd, L"The specified live-zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.",
                    APPNAME, MB_ICONERROR );
                showOptions = TRUE;

            } else if( g_DrawToggleKey && !RegisterHotKey( hWnd, DRAW_HOTKEY, g_DrawToggleMod, g_DrawToggleKey & 0xFF )) {

                MessageBox( hWnd, L"The specified draw w/out zoom hotkey is already in use.\nSelect a different draw w/out zoom hotkey.",
                    APPNAME, MB_ICONERROR );
                showOptions = TRUE;

            }
            else if (g_BreakToggleKey && !RegisterHotKey(hWnd, BREAK_HOTKEY, g_BreakToggleMod, g_BreakToggleKey & 0xFF)) {

                MessageBox(hWnd, L"The specified break timer hotkey is already in use.\nSelect a different break timer hotkey.",
                    APPNAME, MB_ICONERROR);
                showOptions = TRUE;

            }
            
            else if (g_SnipToggleKey && 
                (!RegisterHotKey(hWnd, SNIP_HOTKEY, g_SnipToggleMod, g_SnipToggleKey & 0xFF) ||
                 !RegisterHotKey(hWnd, SNIP_SAVE_HOTKEY, (g_SnipToggleMod ^ MOD_SHIFT), g_SnipToggleKey & 0xFF))) {

                MessageBox(hWnd, L"The specified snip hotkey is already in use.\nSelect a different snip hotkey.",
                    APPNAME, MB_ICONERROR);
                showOptions = TRUE;

            }
            
            if( showOptions ) {

                SendMessage( hWnd, WM_COMMAND, IDC_OPTIONS, 0 );
            }
        }
        SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
        wmTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
        return TRUE;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_Zoomed &&
            (!(GetWindowLongPtr(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) || g_Drawing)) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_USER_RESTORE_SYSTEM_POINTER:
        // A repair queued by an old magnifier must never alter a new session.
        if (!g_Zoomed && !IsWindowVisible(g_hWndLiveZoom)) RestoreSystemPointer();
        return 0;

    case WM_CLOSE:
        // Do not allow users to close the main window, for example with Alt-F4.
        return 0;

    case WM_HOTKEY:


        //
        // Magic value that comes from tray context menu
        //
        if (lParam == 1) {

            //
            // Sleep to let context menu dismiss
            //
            Sleep(250);
        }
        switch( wParam ) {
        case LIVE_DRAW_HOTKEY:
        {
            OutputDebug(L"LIVE_DRAW_HOTKEY\n");
            LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);

            if ((exStyle & WS_EX_LAYERED)) {
                OutputDebug(L"LiveDraw reactivate\n");

                // Just focus on the window and re-enter drawing mode
                SetFocus(hWnd);
                SetForegroundWindow(hWnd);
                SendMessage(hWnd, WM_LBUTTONDOWN, 0, MAKELPARAM(cursorPos.x, cursorPos.y));
                SendMessage(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(cursorPos.x, cursorPos.y));
                if( IsWindowVisible( g_hWndLiveZoom ) )
                {
                    SendMessage( g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, FALSE, 0 );
                }
                break;
            }
            else {
                OutputDebug(L"LiveDraw create\n");

                exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
                SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hWnd, COLORREF(RGB(0, 0, 0)), 0, LWA_COLORKEY);
                pMagSetWindowFilterList( g_hWndLiveZoomMag, MW_FILTERMODE_EXCLUDE, 0, nullptr );
            }
            [[fallthrough]];
        }
        case DRAW_HOTKEY:
            //
            // Enter drawing mode without zoom
            //

            if( !g_Zoomed ) {
                OutputDebug(L"LiveDraw: %d (%d)\n", wParam, (wParam == LIVE_DRAW_HOTKEY));

                if( IsWindowVisible( g_hWndLiveZoom )) {

                    OutputDebug(L"   In Live zoom\n");
                    SendMessage(hWnd, WM_HOTKEY, ZOOM_HOTKEY, wParam == LIVE_DRAW_HOTKEY ? LIVE_DRAW_ZOOM : 0);

                } else {
                    OutputDebug(L"   Not in Live zoom\n");
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, wParam == LIVE_DRAW_HOTKEY ? LIVE_DRAW_ZOOM : 0 );
                    zoomLevel = zoomTelescopeTarget = 1;
                    SendMessage( hWnd, WM_LBUTTONDOWN, 0, MAKELPARAM( cursorPos.x, cursorPos.y ));
                }
                if(wParam == LIVE_DRAW_HOTKEY) {

                    SetLayeredWindowAttributes(hWnd, COLORREF(RGB(0, 0, 0)), 0, LWA_COLORKEY);
                    SendMessage(hWnd, WM_KEYDOWN, 'K', LIVE_DRAW_ZOOM);
                    SetTimer(hWnd, 3, 10, NULL);
                    SendMessage(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(cursorPos.x, cursorPos.y));
                    ShowMainWindow(hWnd, monInfo, width, height);
                    if( ( g_PenColor & 0xFFFFFF ) == COLOR_BLUR )
                    {
                        // Blur is not supported in LiveDraw
                        g_PenColor = COLOR_RED;
                    }
                    // Highlight is not supported in LiveDraw
                    g_PenColor |= 0xFF << 24;
				}
            } 
            break;

        case SNIP_SAVE_HOTKEY:
        case SNIP_HOTKEY:
        {
            // Block liveZoom liveDraw snip due to mirroring bug
            if( IsWindowVisible( g_hWndLiveZoom )
                && ( GetWindowLongPtr( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED ) )
            {
                break;
            }

            bool zoomed = true;

            // First, static zoom
            if( !g_Zoomed )
            {
                zoomed = false;
                if( IsWindowVisible( g_hWndLiveZoom ) )
                {
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_ZOOM );
                }
                else
                {
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, LIVE_DRAW_ZOOM);
                }
                zoomLevel = zoomTelescopeTarget = 1;
            }
            else if( g_Drawing )
            {
                // Exit drawing mode to hide the drawing cursor
                SendMessage( hWnd, WM_USER_EXIT_MODE, 0, 0 );


            }
            ShowMainWindow(hWnd, monInfo, width, height);

            // Now copy crop or copy+save
            if( LOWORD( wParam ) == SNIP_SAVE_HOTKEY )
            {
                SendMessage( hWnd, WM_COMMAND, IDC_SAVE_CROP, ( zoomed ? 0 : SHALLOW_ZOOM ) );
            }
            else
            {
                SendMessage( hWnd, WM_COMMAND, IDC_COPY_CROP, ( zoomed ? 0 : SHALLOW_ZOOM ) );
            }

            // Now if we weren't zoomed, unzoom
            if( !zoomed )
            {
                if( g_ZoomOnLiveZoom )
                {
                    // hiding the cursor allows for a cleaner transition back to the magnified cursor
                    ShowCursor( false );
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
                    ShowCursor( true );
                }
                else
                {
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_ZOOM );
                }
            }

            // Preserve zoom that was already active before Snip.
            if( g_Zoomed && !zoomed )
            {
                // If from liveDraw, extra care is needed to destruct
                if( GetWindowLong( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED )
                {
                    OutputDebug( L"Exiting liveDraw after snip\n" );
                    SendMessage( hWnd, WM_KEYDOWN, VK_ESCAPE, 0 );
                }
                else
                {
                    // Set wparam to 1 to exit without animation
                    OutputDebug(L"Exiting zoom after snip\n" );
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_DESTROY );
                }
            }
            break;
        }

        case BREAK_HOTKEY:
            //
            // Go to break timer
            //
            if( !g_Zoomed && !IsWindowVisible( g_hWndLiveZoom )) {

                SendMessage( hWnd, WM_COMMAND, IDC_BREAK, 0 );
            }
            break;

        case LIVE_HOTKEY:
            //
            // Live zoom
            //
            OutputDebug(L"*** LIVE_HOTKEY\n");

            // If LiveZoom and LiveDraw are active then exit both
            if( g_Zoomed && IsWindowVisible( g_hWndLiveZoom ) && ( GetWindowLongPtr( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED ) )
            {
                KillTimer(hWnd, 3);
                SetWindowLongPtr(hWnd, GWL_EXSTYLE, GetWindowLongPtr(hWnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
                SendMessage(hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_DESTROY);
                if (IsWindow(g_hWndLiveZoom)) DestroyWindow(g_hWndLiveZoom);
                RestoreSystemPointer();
                break;
            }

            if( !g_Zoomed && !g_TimerActive ) {

                if( g_hWndLiveZoom == NULL ) {
                    OutputDebug(L"Create LIVEZOOM\n");
                    g_hWndLiveZoom = CreateWindowEx( WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                        L"ZoomItCustomMagnifier", L"ZoomIt Live Zoom", 
                        WS_POPUP | WS_CLIPSIBLINGS,
                        0, 0, 0, 0, NULL, NULL, g_hInstance, static_cast<PVOID>(GetForegroundWindow()) );
                    if (!g_hWndLiveZoom) {
                        if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
                        MessageBox(hWnd, L"Unable to initialize LiveZoom.", APPNAME, MB_ICONERROR);
                        break;
                    }
                    pSetLayeredWindowAttributes( hWnd, 0, 0, LWA_ALPHA );
                    EnableWindow( g_hWndLiveZoom, FALSE );
                    pMagSetWindowFilterList( g_hWndLiveZoomMag, MW_FILTERMODE_EXCLUDE, 1, &hWnd );

                } else {

                    if( IsWindowVisible( g_hWndLiveZoom )) {
                        // Complete teardown before a subsequent activation can start.
                        DestroyWindow(g_hWndLiveZoom);
                        RestoreSystemPointer();

                    } else {
                    
                        OutputDebug(L"Show liveZoom\n");
                        ShowWindow( g_hWndLiveZoom, SW_SHOW );
                    }
                }

                
            }
            break;


        case ZOOM_HOTKEY:
            //
            // Zoom
            //
            // Don't react to hotkey while options are open or we're
            // saving the screen or live zoom is active
            //
            if( hWndOptions ) {

                break;
            }
            
            OutputDebug( L"ZOOM HOTKEY: %d\n", lParam);
            if( g_TimerActive ) {

                //
                // Finished with break timer
                //
                if( g_BreakOnSecondary )
                {
                    EnableDisableSecondaryDisplay( hWnd, FALSE, &secondaryDevMode );
                }

                if( lParam != SHALLOW_DESTROY )
                {
                    ShowWindow( hWnd, SW_HIDE );
                    if( g_hBackgroundBmp )
                    {
                        DeleteDC( g_hDcBackgroundFile ); g_hDcBackgroundFile = nullptr;
                        DeleteObject( g_hBackgroundBmp );
                        g_hBackgroundBmp = NULL;
                    }
                }

                SetFocus( GetDesktopWindow() );
                KillTimer( hWnd, 0 );
                g_TimerActive = FALSE;
                releaseSession();
                EnableDisableScreenSaver( TRUE );
                EnableDisableOpacity( hWnd, FALSE );

            } else {

                if( !g_Zoomed ) {

                    g_Zoomed = TRUE;
                    g_DrawingShape = FALSE;
                    OutputDebug( L"Zoom on\n");


                    // Hide the cursor before capturing if in live zoom
                    if( g_hWndLiveZoom != nullptr )
                    {
                        OutputDebug(L"Hide cursor\n");
                        SendMessage( g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, FALSE, 0 );
                        SendMessage( g_hWndLiveZoom, WM_TIMER, 0, 0 );
                        SendMessage( g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, FALSE, 0 );
                    }

                    // Get screen DCs
                    hdcScreen = CreateDC(L"DISPLAY", static_cast<PTCHAR>(NULL),
                            static_cast<PTCHAR>(NULL), static_cast<CONST DEVMODE *>(NULL));
                    hdcScreenCompat = CreateCompatibleDC(hdcScreen); 
                    hdcScreenSaveCompat = CreateCompatibleDC(hdcScreen); 
                    hdcScreenCursorCompat = CreateCompatibleDC(hdcScreen); 

                    // Determine what monitor we're on
                    GetCursorPos(&cursorPos);
                    UpdateMonitorInfo( cursorPos, &monInfo );
                    width = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
                    height = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
                    OutputDebug( L"ZOOM x: %d y: %d width: %d height: %d zoomLevel: %g\n",
                            cursorPos.x, cursorPos.y, width, height, zoomLevel );

                    // Create display bitmap
                    bmp.bmBitsPixel = static_cast<BYTE>(GetDeviceCaps(hdcScreen, BITSPIXEL));
                    bmp.bmPlanes = static_cast<BYTE>(GetDeviceCaps(hdcScreen, PLANES));
                    bmp.bmWidth = width;
                    bmp.bmHeight = height;
                    bmp.bmWidthBytes = ((bmp.bmWidth + 15) &~15)/8; 
                    hbmpCompat = CreateBitmap(bmp.bmWidth, bmp.bmHeight, 
                        bmp.bmPlanes, bmp.bmBitsPixel, static_cast<CONST VOID *>(NULL));
                     SelectObject(hdcScreenCompat, hbmpCompat); 

                    // Create saved bitmap
                    hbmpDrawingCompat = CreateBitmap(bmp.bmWidth, bmp.bmHeight, 
                        bmp.bmPlanes, bmp.bmBitsPixel, static_cast<CONST VOID *>(NULL));
                    SelectObject(hdcScreenSaveCompat, hbmpDrawingCompat);

                    // Create cursor save bitmap

                    hbmpCursorCompat = CreateBitmap( MAX_LIVE_PEN_WIDTH+CURSOR_ARM_LENGTH*2,
                        MAX_LIVE_PEN_WIDTH+CURSOR_ARM_LENGTH*2, bmp.bmPlanes,
                        bmp.bmBitsPixel, static_cast<CONST VOID *>(NULL));
                    SelectObject(hdcScreenCursorCompat, hbmpCursorCompat);

                    BitBlt(hdcScreenCompat, 0, 0, width, height, hdcScreen,
                           monInfo.rcMonitor.left, monInfo.rcMonitor.top, SRCCOPY | CAPTUREBLT);
                    BitBlt(hdcScreenSaveCompat, 0, 0, width, height, hdcScreen,
                           monInfo.rcMonitor.left, monInfo.rcMonitor.top, SRCCOPY | CAPTUREBLT);

                    // Abort cleanly if a display or font allocation fails.
                    if (!hdcScreen || !hdcScreenCompat || !hdcScreenSaveCompat ||
                        !hdcScreenCursorCompat || !hbmpCompat || !hbmpDrawingCompat || !hbmpCursorCompat) {

                        releaseSession();
                        g_Zoomed = FALSE;
                        g_ZoomOnLiveZoom = FALSE;
                        if (IsWindowVisible(g_hWndLiveZoom))
                            SendMessage(g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, TRUE, 0);
                        else if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
                        ShowWindow(hWnd, SW_HIDE);
                        MessageBox(hWnd, L"Not enough graphics resources to enter zoom mode.", APPNAME, MB_ICONERROR);
                        break;
                    }
                    // Create drawing pen
                    hDrawingPen = CreatePen(PS_SOLID, g_PenWidth, g_PenColor & 0xFFFFFF);

                    g_BlankedScreen = FALSE;

                    g_Drawing = FALSE;

                    g_HaveDrawn = FALSE;
                    EnableDisableStickyKeys( TRUE );
    
                    // Go full screen
                    g_ActiveWindow = GetForegroundWindow();
                    OutputDebug( L"active window: %x\n", PtrToLong(g_ActiveWindow) );

                    if( lParam != LIVE_DRAW_ZOOM) {

                        OutputDebug(L"Calling ShowMainWindow\n");
                        ShowMainWindow(hWnd, monInfo, width, height);
                    }
                
                    // Start telescoping zoom. Lparam is non-zero if this
                    // was a real hotkey and not the message we send ourself to enter
                    // unzoomed drawing mode.

                    //
                    // Are we switching from live zoom to standard zoom?
                    //
                    if( IsWindowVisible( g_hWndLiveZoom )) {

                        // Enter drawing mode
                        OutputDebug(L"Enter liveZoom draw\n");
                        g_LiveZoomSourceRect = *reinterpret_cast<RECT *>(SendMessage( g_hWndLiveZoom, WM_USER_GET_SOURCE_RECT, 0, 0 ));
                        g_LiveZoomLevel = *reinterpret_cast<float*>(SendMessage(g_hWndLiveZoom, WM_USER_GET_ZOOM_LEVEL, 0, 0));
                        
                        // Set live zoom level to 1 in preparation of us being full screen static
                        zoomLevel = 1.0;
                        zoomTelescopeTarget = 1.0;
                        if (lParam != LIVE_DRAW_ZOOM) {

                            g_ZoomOnLiveZoom = TRUE;
                        }

                        UpdateWindow( hWnd ); // overwrites where cursor erased
                        if( lParam != SHALLOW_ZOOM )
                        {
                            // Put the drawing cursor where the magnified cursor was
                            OutputDebug(L"Setting cursor\n");

                            if (lParam != LIVE_DRAW_ZOOM)
                            {
                                cursorPos = ScalePointInRects( cursorPos, g_LiveZoomSourceRect, monInfo.rcMonitor );
                                SetCursorPos( cursorPos.x, cursorPos.y );
                                UpdateWindow( hWnd ); // overwrites where cursor erased
                                SendMessage( hWnd, WM_LBUTTONDOWN, 0, MAKELPARAM( cursorPos.x, cursorPos.y ));
                            }
                        }
                        else
                        {
                            InvalidateRect( hWnd, NULL, FALSE );
                        }
                        UpdateWindow( hWnd );
                        
                        if( lParam != LIVE_DRAW_ZOOM ) {

                            OutputDebug(L"Calling ShowMainWindow 2\n");

                            ShowWindow( g_hWndLiveZoom, SW_HIDE );
                        }

                    } else if( lParam != 0 && lParam != LIVE_DRAW_ZOOM ) {

                        zoomTelescopeStep = ZOOM_LEVEL_STEP_IN;
                        zoomTelescopeTarget = g_ZoomLevels[g_SliderZoomLevel];
                        if( g_AnimateZoom ) 
                            zoomLevel = static_cast<float>(1.0) * zoomTelescopeStep; 
                        else
                            zoomLevel = zoomTelescopeTarget;
                        SetTimer( hWnd, 1, ZOOM_LEVEL_STEP_TIME, NULL );
                    }

                } else {

                    OutputDebug( L"Zoom off: don't animate=%d\n", lParam );
                    // turn off liveDraw
                    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);

                    if( lParam != SHALLOW_DESTROY && !g_ZoomOnLiveZoom && g_AnimateZoom &&
                        g_TelescopeZoomOut && zoomTelescopeTarget != 1 ) {

                        // Start telescoping zoom. 
                        zoomTelescopeStep = ZOOM_LEVEL_STEP_OUT;
                        zoomTelescopeTarget = 1.0;
                        SetTimer( hWnd, 2, ZOOM_LEVEL_STEP_TIME, NULL );

                    } else {

                        // Simulate timer expiration
                        zoomTelescopeStep = 0;
                        zoomTelescopeTarget = zoomLevel = 1.0;
                        SendMessage( hWnd, WM_TIMER, 2, lParam );
                    }
                }
            }
            break;
        }
        return TRUE;

    case WM_POINTERUPDATE: {
        penInverted = IsPenInverted(wParam);
        OutputDebug( L"WM_POINTERUPDATE: contact: %d button down: %d X: %d Y: %d\n",
            IS_POINTER_INCONTACT_WPARAM(wParam),
            penInverted,
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam));
        if( penInverted != g_PenInverted) {

            g_PenInverted = penInverted;
            if (g_PenInverted) {
                if (PopDrawUndo(hdcScreenCompat, &drawUndoList, width, height)) {

                    SaveCursorArea(hdcScreenCursorCompat, hdcScreenCompat, prevPt);
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        } else if( g_PenDown && !penInverted) {

            SendPenMessage(hWnd, WM_MOUSEMOVE, lParam);
        }
        }
        return TRUE;

    case WM_POINTERUP:
        OutputDebug(L"WM_POINTERUP\n");
        penInverted = IsPenInverted(wParam);
        if (!penInverted) {

            SendPenMessage(hWnd, WM_LBUTTONUP, lParam);
            SendPenMessage(hWnd, WM_RBUTTONDOWN, lParam);
            g_PenDown = FALSE;
        }
        break;

    case WM_POINTERDOWN: 
        OutputDebug(L"WM_POINTERDOWN\n");
        penInverted = IsPenInverted(wParam);
        if (!penInverted) {

            g_PenDown = TRUE;

            // Enter drawing mode
            SendPenMessage(hWnd, WM_LBUTTONDOWN, lParam);
            SendPenMessage(hWnd, WM_MOUSEMOVE, lParam);
            SendPenMessage(hWnd, WM_LBUTTONUP, lParam);
            SendPenMessage(hWnd, WM_MOUSEMOVE, lParam);
            PopDrawUndo(hdcScreenCompat, &drawUndoList, width, height);

            // Enter tracing mode
            SendPenMessage(hWnd, WM_LBUTTONDOWN, lParam);
        }
        break;

    case WM_KILLFOCUS:
        if( ( g_SelectionActive == FALSE ) && g_Zoomed && !g_bSaveInProgress ) {

            // Turn off zoom if not in liveDraw
            DWORD layeringFlag = 0;
            GetLayeredWindowAttributes(hWnd, NULL, NULL, &layeringFlag);
            if( !(layeringFlag & LWA_COLORKEY)) {

                PostMessage(hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0);
            }
        }
        break;

    case WM_MOUSEWHEEL:

        //
        // Zoom or modify break timer
        //
        if( GET_WHEEL_DELTA_WPARAM(wParam) < 0 ) 
            wParam -= (WHEEL_DELTA-1) << 16;
        else 
            wParam += (WHEEL_DELTA-1) << 16;
        delta = GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA;
        OutputDebug( L"mousewheel: wParam: %d delta: %d\n", 
                GET_WHEEL_DELTA_WPARAM(wParam), delta );
        if( g_Zoomed ) {
            
            {

                if( g_Drawing && (LOWORD( wParam ) & MK_CONTROL) ) {

                    ResizePen( hWnd, hdcScreenCompat, hdcScreenCursorCompat, prevPt,
                        g_Tracing, &g_Drawing, g_LiveZoomLevel, TRUE, g_PenWidth + delta );

                // Perform static zoom unless in liveDraw
                } else if( !( GetWindowLongPtr( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED ) ) {

                    if( delta > 0 ) zoomIn = TRUE;
                    else {
                        zoomIn = FALSE;
                        delta = -delta;
                    }
                    while( delta-- ) {

                        if( zoomIn ) {
                            
                            if( zoomTelescopeTarget < ZOOM_LEVEL_MAX ) {

                                if( zoomTelescopeTarget < 2 ) {

                                    zoomTelescopeTarget = 2;

                                } else {
                            
                                    // Start telescoping zoom
                                    zoomTelescopeTarget = zoomTelescopeTarget * 2; 
                                }
                                zoomTelescopeStep = ZOOM_LEVEL_STEP_IN; 
                                if( g_AnimateZoom ) 
                                    zoomLevel *= zoomTelescopeStep; 
                                else
                                    zoomLevel = zoomTelescopeTarget;

                                if( zoomLevel > zoomTelescopeTarget ) 
                                    zoomLevel = zoomTelescopeTarget;
                                else
                                    SetTimer( hWnd, 1, ZOOM_LEVEL_STEP_TIME, NULL );
                            }

                        } else if( zoomTelescopeTarget > ZOOM_LEVEL_MIN ) {

                            // Let them more gradually zoom out from 2x to 1x
                            if( zoomTelescopeTarget <= 2 ) {

                                zoomTelescopeTarget *= .75; 
                                if( zoomTelescopeTarget < ZOOM_LEVEL_MIN ) 
                                    zoomTelescopeTarget = ZOOM_LEVEL_MIN;

                            } else {

                                zoomTelescopeTarget = zoomTelescopeTarget/2; 
                            }
                            zoomTelescopeStep = ZOOM_LEVEL_STEP_OUT; 
                            if( g_AnimateZoom ) 
                                zoomLevel *= zoomTelescopeStep; 
                            else
                                zoomLevel = zoomTelescopeTarget;

                            if( zoomLevel < zoomTelescopeTarget )
                            {
                                zoomLevel = zoomTelescopeTarget;
                                // Force update on final step out
                                InvalidateRect( hWnd, NULL, FALSE );
                            }
                            else
                            {
                                SetTimer( hWnd, 1, ZOOM_LEVEL_STEP_TIME, NULL );
                            }
                        }
                    }
                    if( zoomLevel != zoomTelescopeTarget ) {

                        if( g_Drawing ) {

                            if( !g_Tracing ) {

                                RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );
                            }
                            //SetCursorPos( monInfo.rcMonitor.left + cursorPos.x, 
                            //		monInfo.rcMonitor.top + cursorPos.y );
                        }
                        InvalidateRect( hWnd, NULL, FALSE );
                    }				
                }
            }
        } else if( g_TimerActive && (breakTimeout > 0 || delta )) {

            if( delta ) {

                if( breakTimeout < 0 ) breakTimeout = 0;
                if( breakTimeout % 60 ) {
                    breakTimeout += (60 - breakTimeout % 60);
                    delta--;
                }
                breakTimeout += delta * 60;

            } else {

                if( breakTimeout % 60 ) {
                    breakTimeout -= breakTimeout % 60;
                    delta--;
                }
                breakTimeout -= delta * 60;
            }
            if( breakTimeout < 0 ) breakTimeout = 0;
            KillTimer( hWnd, 0 );
            SetTimer( hWnd, 0, 1000, NULL );
            InvalidateRect( hWnd, NULL, TRUE );
        }

        if( zoomLevel != 1 && g_Drawing ) {

            // Constrain the mouse to the visible region
            boundRc = BoundMouse( zoomTelescopeTarget, &monInfo, width, height, &cursorPos );

        } else {

            ClipCursor( NULL );
        }
        return TRUE;

    case WM_KEYDOWN:

        
        switch (wParam) { 
        case 'R':
        case 'B':
        case 'Y':
        case 'O':
        case 'G':
        case 'X':
        case 'P':
            if( (g_Zoomed || g_TimerActive)) {
            
                PDWORD	penColor;
                if( g_TimerActive )
                    penColor = &g_BreakPenColor;
                else
                    penColor = &g_PenColor;

                if( wParam == 'R' )		 *penColor = COLOR_RED;
                else if( wParam == 'G' ) *penColor = COLOR_GREEN;
                else if( wParam == 'B' ) *penColor = COLOR_BLUE;
                else if( wParam == 'Y' ) *penColor = COLOR_YELLOW;
                else if( wParam == 'O' ) *penColor = COLOR_ORANGE;
                else if( wParam == 'P' ) *penColor = COLOR_PINK;
                else if( wParam == 'X' )
                {
                    if( GetWindowLong( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED )
                    {
                        // Blur is not supported in LiveDraw
                        break;
                    }
                    *penColor = COLOR_BLUR;
                }

                bool shift = GetKeyState( VK_SHIFT ) & 0x8000;
                if( shift && ( GetWindowLong( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED ) )
                {
                    // Highlight is not supported in LiveDraw
                    break;
                }

                PersistSettings();
                DeleteSelectedPen(hdcScreenCompat, hDrawingPen);
                SetTextColor( hdcScreenCompat, *penColor );

                // Highlight and blur level
                if( shift && *penColor != COLOR_BLUR )
                {
                    *penColor |= (g_AlphaBlend << 24);
                }
                else
                {
                    if( *penColor == COLOR_BLUR )
                    {
                        g_BlurRadius = shift ? STRONG_BLUR_RADIUS : NORMAL_BLUR_RADIUS;
                    }
                    *penColor |= (0xFF << 24);
                }
                hDrawingPen = CreatePen(PS_SOLID, g_PenWidth, *penColor & 0xFFFFFF);

                SelectObject( hdcScreenCompat, hDrawingPen );
                if( g_Drawing ) {

                    SendMessage( hWnd, WM_MOUSEMOVE, 0, MAKELPARAM( prevPt.x, prevPt.y ));				
                
                } else if( g_TimerActive ) {
    
                    InvalidateRect( hWnd, NULL, FALSE );				
                
                } 
            }
            break;

        case 'Z':
            if( (GetKeyState( VK_CONTROL ) & 0x8000 ) && g_HaveDrawn && !g_Tracing ) {

                if( PopDrawUndo( hdcScreenCompat, &drawUndoList, width, height )) {
                
                    if( g_Drawing ) {

                        SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );
                        SendMessage( hWnd, WM_MOUSEMOVE, 0, MAKELPARAM( prevPt.x, prevPt.y ));			
                    }
                    else {

                        SaveCursorArea(hdcScreenCursorCompat, hdcScreenCompat, prevPt);
                    }
                    InvalidateRect( hWnd, NULL, FALSE );
                }
            }
            break;

        case VK_SPACE:
            if( g_Drawing && !g_Tracing ) {

                SetCursorPos(  boundRc.left + (boundRc.right - boundRc.left)/2,
                         boundRc.top + (boundRc.bottom - boundRc.top)/2 );
                SendMessage( hWnd, WM_MOUSEMOVE, 0, 
                    MAKELPARAM( (boundRc.right - boundRc.left)/2,
                                (boundRc.bottom - boundRc.top)/2 ));
            }
            break;

        case 'W':
        case 'K':
            // Block user-driven sketch pad in liveDraw
            if( lParam != LIVE_DRAW_ZOOM
                && ( GetWindowLongPtr( hWnd, GWL_EXSTYLE ) & WS_EX_LAYERED ) )
            {
                break;
            }

            // because we don't really handle going from white to black.
            if( g_Zoomed) {

                if( !g_Drawing ) {

                    SendMessage( hWnd, WM_LBUTTONDOWN, 0, MAKELPARAM( cursorPos.x, cursorPos.y));
                }
                // Restore area where cursor was previously
                RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );
                PushDrawUndo( hdcScreenCompat, &drawUndoList, width, height );
                g_BlankedScreen = static_cast<int>(wParam);
                rc.top = rc.left = 0;
                rc.bottom = height;
                rc.right = width;
                BlankScreenArea( hdcScreenCompat, &rc, g_BlankedScreen );
                InvalidateRect( hWnd, NULL, FALSE );

                // Save area that's going to be occupied by new cursor position
                SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );
                SendMessage( hWnd, WM_MOUSEMOVE, 0, MAKELPARAM( prevPt.x, prevPt.y ));				
            }
            break;

        case 'E':

            if( g_HaveDrawn) {

                DeleteDrawUndoList( &drawUndoList );
                g_HaveDrawn = FALSE;
                OutputDebug(L"Erase\n");
                if(GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) {
                    SendMessage(hWnd, WM_KEYDOWN, 'K', 0);
                }
                else {
                    BitBlt(hdcScreenCompat, 0, 0, bmp.bmWidth,
                        bmp.bmHeight, hdcScreenSaveCompat, 0, 0, SRCCOPY | CAPTUREBLT);

                    if (g_Drawing) {

                        OutputDebug(L"Erase: draw cursor\n");
                        SaveCursorArea(hdcScreenCursorCompat, hdcScreenCompat, prevPt);
                        DrawCursor(hdcScreenCompat, prevPt, zoomLevel, width, height);
                        g_HaveDrawn = TRUE;
                    }
                }
                InvalidateRect( hWnd, NULL, FALSE );
                g_BlankedScreen = FALSE;
            } 
            break;

        case VK_UP:
            SendMessage( hWnd, WM_MOUSEWHEEL, 
                MAKEWPARAM( GetAsyncKeyState( VK_LCONTROL ) != 0 || GetAsyncKeyState( VK_RCONTROL ) != 0 ? 
                        MK_CONTROL: 0, WHEEL_DELTA), 0 );
            return TRUE;

        case VK_DOWN:
            SendMessage( hWnd, WM_MOUSEWHEEL, 
                MAKEWPARAM( GetAsyncKeyState( VK_LCONTROL ) != 0 || GetAsyncKeyState( VK_RCONTROL ) != 0 ? 
                        MK_CONTROL: 0, -WHEEL_DELTA), 0 );
            return TRUE;

        case VK_LEFT:
        case VK_RIGHT:
            if( wParam == VK_RIGHT ) delta = 10;
            else					  delta = -10;
            if( g_TimerActive && (breakTimeout > 0 || delta )) {

                if( breakTimeout < 0 ) breakTimeout = 0;
                breakTimeout += delta;
                breakTimeout -= (breakTimeout % 10);
                if( breakTimeout < 0 ) breakTimeout = 0;
                KillTimer( hWnd, 0 );
                SetTimer( hWnd, 0, 1000, NULL );
                InvalidateRect( hWnd, NULL, TRUE );
            }
            break;
            
        case VK_ESCAPE: 
            {

                forcePenResize = TRUE;
                PostMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );

                // In case we were in liveDraw
                if( GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) {

                    KillTimer(hWnd, 3);
                    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
                    SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
                    pMagSetWindowFilterList( g_hWndLiveZoomMag, MW_FILTERMODE_EXCLUDE, 1, &hWnd );
                    SendMessage( g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, TRUE, 0 );
                }
            }
            break;
        }
        return TRUE;

    case WM_RBUTTONDOWN:
        SendMessage( hWnd, WM_USER_EXIT_MODE, 0, 0 );
        break;

    case WM_MOUSEMOVE:
        OutputDebug(L"MOUSEMOVE: zoomed: %d drawing: %d tracing: %d\n",
            g_Zoomed, g_Drawing, g_Tracing);

        OutputDebug(L"Window visible: %d Topmost: %d\n", IsWindowVisible(hWnd), GetWindowLong(hWnd, GWL_EXSTYLE)& WS_EX_TOPMOST);

        if( g_Zoomed && !g_bSaveInProgress ) {

            if( g_Drawing ) {

                POINT currentPt;

                // Are we in pen mode on a tablet?
                lParam = ScalePenPosition( zoomLevel, &monInfo, boundRc, message, lParam);	
                currentPt.x = LOWORD(lParam);
                currentPt.y = HIWORD(lParam);

                if(lParam == 0) {

                    // Drop it
                    OutputDebug(L"Mousemove: Dropping\n");
                    break;

                } else if(g_DrawingShape) {

                    SetROP2(hdcScreenCompat, R2_NOTXORPEN); 
     
                    // If a previous target rectangle exists, erase 
                    // it by drawing another rectangle on top. 
                    if( g_rcRectangle.top != g_rcRectangle.bottom ||
                        g_rcRectangle.left != g_rcRectangle.right )
                    {
                        if( prevPenWidth != g_PenWidth )
                        {
                            auto penWidth = g_PenWidth;
                            g_PenWidth = prevPenWidth;

                            auto prevPen = CreatePen( PS_SOLID, g_PenWidth, g_PenColor & 0xFFFFFF );
                            SelectObject( hdcScreenCompat, prevPen );

                            DrawShape( g_DrawingShape, hdcScreenCompat, &g_rcRectangle );

                            g_PenWidth = penWidth;
                            SelectObject( hdcScreenCompat, hDrawingPen );
                            DeleteObject( prevPen );
                        }
                        else
                        {
                            DrawShape( g_DrawingShape, hdcScreenCompat, &g_rcRectangle );
                        }
                    }

                    // Save the coordinates of the target rectangle. 
                    // Avoid invalid rectangles by ensuring that the
                    // value of the left coordinate is greater than 
                    // that of the right, and that the value of the 
                    // bottom coordinate is greater than that of 
                    // the top. 	 
                    if( g_DrawingShape == DRAW_LINE ||
                        g_DrawingShape == DRAW_ARROW ) {

                        g_rcRectangle.right = static_cast<LONG>(LOWORD(lParam));
                        g_rcRectangle.bottom = static_cast<LONG>(HIWORD(lParam));

                    } else {

                        if ((g_RectangleAnchor.x < currentPt.x) && 
                                (g_RectangleAnchor.y > currentPt.y)) {

                            SetRect(&g_rcRectangle, g_RectangleAnchor.x, currentPt.y, 
                                currentPt.x, g_RectangleAnchor.y); 

                        } else if ((g_RectangleAnchor.x > currentPt.x) && 
                                (g_RectangleAnchor.y > currentPt.y )) {

                            SetRect(&g_rcRectangle, currentPt.x, 
                                currentPt.y, g_RectangleAnchor.x,g_RectangleAnchor.y); 

                        } else if ((g_RectangleAnchor.x > currentPt.x) && 
                                (g_RectangleAnchor.y < currentPt.y )) {

                            SetRect(&g_rcRectangle, currentPt.x, g_RectangleAnchor.y, 
                                g_RectangleAnchor.x, currentPt.y ); 
                        } else {

                            SetRect(&g_rcRectangle, g_RectangleAnchor.x, g_RectangleAnchor.y, 
                                currentPt.x, currentPt.y ); 
                        }
                    }

                    if (g_rcRectangle.left != g_rcRectangle.right ||
                        g_rcRectangle.top != g_rcRectangle.bottom) {

                        // Draw the new target rectangle. 
                        DrawShape(g_DrawingShape, hdcScreenCompat, &g_rcRectangle);
                        OutputDebug(L"SHAPE: (%d, %d) - (%d, %d)\n", g_rcRectangle.left, g_rcRectangle.top, 
                            g_rcRectangle.right, g_rcRectangle.bottom);
                    }

                    prevPenWidth = g_PenWidth;
                    SetROP2( hdcScreenCompat, R2_NOP );
                }
                else if (g_Tracing) {

                    OutputDebug(L"Mousemove: Tracing\n");

                    g_HaveDrawn = TRUE;
                    Gdiplus::Graphics	dstGraphics(hdcScreenCompat);
                    if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
                    {
                        dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    }
                    Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
                    Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
                    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

                    // If highlighting, use a double layer approach
                    OutputDebug(L"PenColor: %x\n", g_PenColor);
                    OutputDebug(L"Blur color: %x\n", COLOR_BLUR);
                    if( (g_PenColor & 0xFFFFFF) == COLOR_BLUR) {

                        OutputDebug(L"BLUR\n");

                        // Restore area where cursor was previously
                        RestoreCursorArea(hdcScreenCompat, hdcScreenCursorCompat, prevPt);

                        Gdiplus::Rect lineBounds = GetLineBounds(prevPt, currentPt, g_PenWidth);
                        DrawMaskedLine(hdcScreenCompat, hdcScreenCompat, lineBounds, prevPt, currentPt, &pen, true);

                        // Invalidate the updated rectangle
                        InvalidateGdiplusRect( hWnd, lineBounds );

                        // Save area that's going to be occupied by new cursor position
                        SaveCursorArea(hdcScreenCursorCompat, hdcScreenCompat, currentPt);

                        // Draw new cursor
                        DrawCursor(hdcScreenCompat, currentPt, zoomLevel, width, height);
                    } 
                    else if(PEN_COLOR_HIGHLIGHT(g_PenColor)) { 

                        Gdiplus::Rect lineBounds = GetLineBounds(prevPt, currentPt, g_PenWidth);
                        P_DRAW_UNDO oldestUndo = GetOldestUndo(drawUndoList);
                        DrawMaskedLine(hdcScreenCompat, oldestUndo ? oldestUndo->hDc : hdcScreenSaveCompat,
                                       lineBounds, prevPt, currentPt, &pen, false);

                        // Invalidate the updated rectangle
                        InvalidateGdiplusRect(hWnd, lineBounds);
                    }
                    else {

                        // Normal tracing
                        dstGraphics.DrawLine(&pen, static_cast<INT>(prevPt.x), static_cast<INT>(prevPt.y),
                                static_cast<INT>(currentPt.x), static_cast<INT>(currentPt.y));
                    }

                } else {

                    OutputDebug(L"Mousemove: Moving cursor\n");

                    // Restore area where cursor was previously
                    RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );
                    
                    // Save area that's going to be occupied by new cursor position
                    SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, currentPt );

                    // Draw new cursor
                    DrawCursor( hdcScreenCompat, currentPt, zoomLevel, width, height );
                }

                if( g_DrawingShape ) {

                    InvalidateRect( hWnd, NULL, FALSE );

                } else {

                    // Invalidate area just modified
                    InvalidateCursorMoveArea( hWnd, zoomLevel, width, height, currentPt, prevPt, cursorPos );
                }
                prevPt = currentPt;

                // In liveDraw we an miss the mouse up
                if( GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) {

                    if((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {

                        OutputDebug(L"LIVE_DRAW missed mouse up. Sending synthetic.\n");
                        SendMessage(hWnd, WM_LBUTTONUP, wParam, lParam);
                    }
                }

            } else {

                cursorPos.x = LOWORD( lParam );
                cursorPos.y = HIWORD( lParam );
                InvalidateRect( hWnd, NULL, FALSE );
            }
        } 
#if 0
        {
            static int index = 0;
            OutputDebug( L"%d: foreground: %x focus: %x (hwnd: %x)\n", 
                index++, (DWORD) PtrToUlong(GetForegroundWindow()),  PtrToUlong(GetFocus()), PtrToUlong(hWnd));
        }
#endif
        return TRUE;
    
    case WM_LBUTTONDOWN:
        g_StraightDirection = 0;

        if( g_Zoomed && zoomTelescopeTarget == zoomLevel ) {

            OutputDebug(L"LBUTTONDOWN: drawing\n");

            // Save current bitmap to undo history
            if( g_HaveDrawn ) {

                RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );
            }
            
            // don't push undo if we sent this to ourselves for a pen resize
            if( wParam != -1 ) {
                
                PushDrawUndo( hdcScreenCompat, &drawUndoList, width, height );

            } else {

                wParam = 0;
            }

            // Are we in pen mode on a tablet?
            lParam = ScalePenPosition( zoomLevel, &monInfo, boundRc,
                        message, lParam);

            if (lParam == 0) {

                // Drop it
                break;

            } else if( g_Drawing ) {

                // is the user drawing a rectangle?
                if( wParam & MK_CONTROL ||
                    wParam & MK_SHIFT ||
                    GetKeyState( VK_TAB ) < 0 ) {

                    // Restore area where cursor was previously
                    RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );

                    if( wParam & MK_SHIFT && wParam & MK_CONTROL )
                        g_DrawingShape = DRAW_ARROW;
                    else if( wParam & MK_CONTROL ) 
                        g_DrawingShape = DRAW_RECTANGLE;
                    else if( wParam & MK_SHIFT )
                        g_DrawingShape = DRAW_LINE;
                    else
                        g_DrawingShape = DRAW_ELLIPSE;
                    g_RectangleAnchor.x = LOWORD(lParam);
                    g_RectangleAnchor.y = HIWORD(lParam);
                    SetRect(&g_rcRectangle, g_RectangleAnchor.x, g_RectangleAnchor.y, 
                            g_RectangleAnchor.x, g_RectangleAnchor.y); 

                } else {

                    Gdiplus::Graphics	dstGraphics(hdcScreenCompat);
                    if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
                    {
                        dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    }
                    Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
                    Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
                    Gdiplus::GraphicsPath path;
                    pen.SetLineJoin(Gdiplus::LineJoinRound);
                    path.AddLine(static_cast<INT>(prevPt.x), prevPt.y, prevPt.x, prevPt.y);
                    dstGraphics.DrawPath(&pen, &path);
                }
                g_Tracing = TRUE;
                SetROP2( hdcScreenCompat, R2_COPYPEN );
                prevPt.x = LOWORD(lParam); 
                prevPt.y = HIWORD(lParam); 
                g_HaveDrawn = TRUE;
        
            } else {

                OutputDebug(L"Tracing on\n");

                // Turn on drawing
                if( !g_HaveDrawn ) {

                    // refresh drawing bitmap with original screen image
                    BitBlt(hdcScreenCompat, 0, 0, bmp.bmWidth,
                        bmp.bmHeight, hdcScreenSaveCompat, 0, 0, SRCCOPY|CAPTUREBLT );
                    g_HaveDrawn = TRUE;
                }
                DeleteSelectedPen(hdcScreenCompat, hDrawingPen);
                hDrawingPen = CreatePen(PS_SOLID, g_PenWidth, g_PenColor & 0xFFFFFF);
                SelectObject( hdcScreenCompat, hDrawingPen );

                // is the user drawing a rectangle?
                if( wParam & MK_CONTROL && g_Drawing ) {

                    // Restore area where cursor was previously
                    RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );

                    // Configure rectangle drawing
                    g_DrawingShape = TRUE;
                    g_RectangleAnchor.x = LOWORD(lParam);
                    g_RectangleAnchor.y = HIWORD(lParam);
                    SetRect(&g_rcRectangle, g_RectangleAnchor.x, g_RectangleAnchor.y,
                        g_RectangleAnchor.x, g_RectangleAnchor.y);
                    OutputDebug( L"RECTANGLE: %d, %d\n", prevPt.x, prevPt.y );

                } else {

                    prevPt.x = LOWORD( lParam );
                    prevPt.y = HIWORD( lParam );
                    SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );

                    Gdiplus::Graphics	dstGraphics(hdcScreenCursorCompat);
                    if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
                    {
                        dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    }
                    Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
                    Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
                    Gdiplus::GraphicsPath path;
                    pen.SetLineJoin(Gdiplus::LineJoinRound);
                    path.AddLine(static_cast<INT>(prevPt.x), prevPt.y, prevPt.x, prevPt.y);
                    dstGraphics.DrawPath(&pen, &path);
                }
                InvalidateRect( hWnd, NULL, FALSE );

                // If we're in live zoom, make the drawing pen larger to compensate
                if( g_ZoomOnLiveZoom && forcePenResize )
                {
                    forcePenResize = FALSE;

                    ResizePen( hWnd, hdcScreenCompat, hdcScreenCursorCompat, prevPt, g_Tracing,
                        &g_Drawing, g_LiveZoomLevel, FALSE, min( static_cast<int>(g_LiveZoomLevel * g_RootPenWidth),
                        static_cast<int>(g_LiveZoomLevel * MAX_PEN_WIDTH) ) );
                    OutputDebug( L"LIVEZOOM_DRAW: zoomLevel: %d rootPenWidth: %d penWidth: %d\n",
                        static_cast<int>(g_LiveZoomLevel), g_RootPenWidth, g_PenWidth );
                }
                else if( !g_ZoomOnLiveZoom && forcePenResize )
                {
                    forcePenResize = FALSE;
                    // Scale pen down to root for regular drawing mode
                    ResizePen( hWnd, hdcScreenCompat, hdcScreenCursorCompat, prevPt, g_Tracing,
                        &g_Drawing, g_LiveZoomLevel, FALSE, g_RootPenWidth );
                }
                g_Drawing = TRUE;

                EnableDisableStickyKeys( FALSE );
                OutputDebug( L"LBUTTONDOWN: %d, %d\n", prevPt.x, prevPt.y );

                // Constrain the mouse to the visible region
                boundRc = BoundMouse( zoomLevel, &monInfo, width, height, &cursorPos );
            }
        } 
        return TRUE;

    case WM_LBUTTONUP:
        OutputDebug(L"LBUTTONUP: zoomed: %d drawing: %d tracing: %d\n", 
            g_Zoomed, g_Drawing, g_Tracing);

        if( g_Zoomed && g_Drawing && g_Tracing ) {

            // Are we in pen mode on a tablet?
            lParam = ScalePenPosition( zoomLevel, &monInfo, boundRc,
                        message, lParam);
            OutputDebug(L"LBUTTONUP: %d, %d\n", LOWORD(lParam), HIWORD(lParam));
            if (lParam == 0) {

                // Drop it
                break;
            }

            POINT adjustPos;
            adjustPos.x = LOWORD(lParam);
            adjustPos.y = HIWORD(lParam);
            if( g_StraightDirection == -1 ) {

                adjustPos.x = prevPt.x;
            
            } else {

                adjustPos.y = prevPt.y;
            }
            lParam = MAKELPARAM( adjustPos.x, adjustPos.y );					

            if( !g_DrawingShape ) {

                Gdiplus::Graphics	dstGraphics(hdcScreenCompat);
                if( ( GetWindowLong( g_hWndMain, GWL_EXSTYLE ) & WS_EX_LAYERED ) == 0 )
                {
                    dstGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                }
                Gdiplus::Color	color = ColorFromColorRef(g_PenColor);
                Gdiplus::Pen pen(color, static_cast<Gdiplus::REAL>(g_PenWidth));
                Gdiplus::GraphicsPath path;
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                path.AddLine(prevPt.x, prevPt.y, LOWORD(lParam), HIWORD(lParam));
                dstGraphics.DrawPath(&pen, &path);

                prevPt.x = LOWORD( lParam );
                prevPt.y = HIWORD( lParam );

                if ((g_PenColor & 0xFFFFFF) == COLOR_BLUR) {

                    RestoreCursorArea(hdcScreenCompat, hdcScreenCursorCompat, prevPt);
                }
                SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );
                DrawCursor( hdcScreenCompat, prevPt, zoomLevel, width, height );
            
            } else if (g_rcRectangle.top != g_rcRectangle.bottom ||
                        g_rcRectangle.left != g_rcRectangle.right ) {

                // erase previous
                SetROP2(hdcScreenCompat, R2_NOTXORPEN); 
                DrawShape( g_DrawingShape, hdcScreenCompat, &g_rcRectangle );

                // Draw the final shape
                HBRUSH hBrush = static_cast<HBRUSH>(GetStockObject( NULL_BRUSH ));
                HBRUSH oldHbrush = static_cast<HBRUSH>(SelectObject( hdcScreenCompat, hBrush ));
                SetROP2( hdcScreenCompat, R2_COPYPEN );

                // smooth line
                if( g_SnapToGrid ) {

                    if( g_DrawingShape == DRAW_LINE ||
                        g_DrawingShape == DRAW_ARROW ) {

                        if( abs(g_rcRectangle.bottom - g_rcRectangle.top) <
                            abs(g_rcRectangle.right - g_rcRectangle.left)/10 ) {

                            g_rcRectangle.bottom = g_rcRectangle.top-1;
                        }
                        if( abs(g_rcRectangle.right - g_rcRectangle.left) <
                            abs(g_rcRectangle.bottom - g_rcRectangle.top)/10 ) {

                            g_rcRectangle.right = g_rcRectangle.left-1;
                        }
                    }
                }
                // Draw final one using Gdi+
                DrawShape( g_DrawingShape, hdcScreenCompat, &g_rcRectangle, true );

                InvalidateRect( hWnd, NULL, FALSE );
                DeleteObject( hBrush );
                SelectObject( hdcScreenCompat, oldHbrush );

                prevPt.x = LOWORD( lParam );
                prevPt.y = HIWORD( lParam );
                SaveCursorArea( hdcScreenCursorCompat, hdcScreenCompat, prevPt );
            }
            g_Tracing = FALSE;
            g_DrawingShape = FALSE;
            OutputDebug( L"LBUTTONUP:" );
        }
        return TRUE;

    case WM_GETMINMAXINFO:

        reinterpret_cast<MINMAXINFO *>(lParam)->ptMaxSize.x = width;
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxSize.y = height;
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxPosition.x = 0;
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxPosition.y = 0;
        return TRUE;

    case WM_USER_TRAY_ACTIVATE:

        switch( lParam ) {
        case WM_RBUTTONUP:
        case WM_LBUTTONUP:
        case WM_CONTEXTMENU:
        {
            // Set the foreground window so the menu can be closed by clicking elsewhere when
            // opened via right click, and so keyboard navigation works when opened with the menu
            // key or Shift-F10.
            SetForegroundWindow( hWndOptions ? hWndOptions : hWnd );

            // Pop up context menu
            POINT pt;
            GetCursorPos( &pt );
            hPopupMenu = CreatePopupMenu();
            if(!g_StartedByPowerToys) {
                // Exiting will happen through disabling in PowerToys, not the context menu.
                InsertMenu( hPopupMenu, 0, MF_BYPOSITION, IDCANCEL, L"E&xit" );
                InsertMenu( hPopupMenu, 0, MF_BYPOSITION|MF_SEPARATOR, 0, NULL );
            }
            InsertMenu( hPopupMenu, 0, MF_BYPOSITION, IDC_ZOOM, L"&Zoom" );
            InsertMenu( hPopupMenu, 0, MF_BYPOSITION, IDC_DRAW, L"&Draw" );
            InsertMenu( hPopupMenu, 0, MF_BYPOSITION, IDC_BREAK, L"&Break Timer" );
            if(!g_StartedByPowerToys) {
                // When started by PowerToys, options are configured through the PowerToys Settings.
                InsertMenu( hPopupMenu, 0, MF_BYPOSITION|MF_SEPARATOR, 0, NULL );
                InsertMenu( hPopupMenu, 0, MF_BYPOSITION, IDC_OPTIONS, L"&Options" );
            }
            TrackPopupMenu( hPopupMenu, 0, pt.x , pt.y, 0, hWnd, NULL );
            DestroyMenu( hPopupMenu );
            break;
        } 
        case WM_LBUTTONDBLCLK:
            if( !g_TimerActive ) {

                SendMessage( hWnd, WM_COMMAND, IDC_OPTIONS, 0 );
            
            } else {

                SetForegroundWindow( hWnd );
            }
            break;
        }
        break;


    case WM_USER_SAVE_CURSOR:
        if( g_Zoomed == TRUE )
        {
            GetCursorPos( &savedCursorPos );
            if( g_Drawing == TRUE )
            {
                ClipCursor( NULL );
            }
        }
        break;

    case WM_USER_RESTORE_CURSOR:
        if( g_Zoomed == TRUE )
        {
            if( g_Drawing == TRUE )
            {
                boundRc = BoundMouse( zoomLevel, &monInfo, width, height, &cursorPos );
            }
            SetCursorPos( savedCursorPos.x, savedCursorPos.y );
        }
        break;

    case WM_USER_EXIT_MODE:
        if( g_Zoomed )
        {
            // Turn off
            if( !g_Drawing )
            {
                // Turn off
                PostMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
            }
            else
            {
                if( !g_Tracing )
                {
                    RestoreCursorArea( hdcScreenCompat, hdcScreenCursorCompat, prevPt );

                    // Ensure the cursor area is painted before returning
                    InvalidateRect( hWnd, NULL, FALSE );
                    UpdateWindow( hWnd );

                }
                // Restore LiveDraw's pointer even if the user exits during an active stroke.
                if ((GetWindowLongPtr(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) &&
                    IsWindowVisible(g_hWndLiveZoom))
                    SendMessage(g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, TRUE, 0);
                if( zoomLevel != 1 )
                {
                    // Restore the cursor position to prevent moving the view in static zoom
                    SetCursorPos( monInfo.rcMonitor.left + cursorPos.x, monInfo.rcMonitor.top + cursorPos.y );
                }
                g_Drawing = FALSE;
                g_Tracing = FALSE;
                EnableDisableStickyKeys( TRUE );

                // Unclip cursor
                ClipCursor( NULL );
            }
        }
        else if( g_TimerActive )
        {
            // Turn off
            PostMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
        }
        break;

    case WM_USER_RELOAD_SETTINGS:
    {
        // Reload the settings. This message is called from PowerToys after a setting is changed by the user.
        reg.ReadRegSettings(RegSettings);
        ValidateSettings();

        // Apply tray icon setting
        EnableDisableTrayIcon(hWnd, g_ShowTrayIcon);

        // Apply hotkey settings
        UnregisterAllHotkeys(hWnd);
        g_ToggleMod = GetKeyMod(g_ToggleKey);
        g_LiveZoomToggleMod = GetKeyMod(g_LiveZoomToggleKey);
        g_DrawToggleMod = GetKeyMod(g_DrawToggleKey);
        g_BreakToggleMod = GetKeyMod(g_BreakToggleKey);

        g_SnipToggleMod = GetKeyMod(g_SnipToggleKey);
        BOOL showOptions = FALSE;
        if (g_ToggleKey)
        {
            if (!RegisterHotKey(hWnd, ZOOM_HOTKEY, g_ToggleMod, g_ToggleKey & 0xFF))
            {
                MessageBox(hWnd, L"The specified zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.", APPNAME, MB_ICONERROR);
                showOptions = TRUE;
            }
        }
        if (g_LiveZoomToggleKey)
        {
            if (!RegisterHotKey(hWnd, LIVE_HOTKEY, g_LiveZoomToggleMod, g_LiveZoomToggleKey & 0xFF) ||
                !RegisterHotKey(hWnd, LIVE_DRAW_HOTKEY, g_LiveZoomToggleMod ^ MOD_SHIFT, g_LiveZoomToggleKey & 0xFF))
            {
                MessageBox(hWnd, L"The specified live-zoom toggle hotkey is already in use.\nSelect a different zoom toggle hotkey.", APPNAME, MB_ICONERROR);
                showOptions = TRUE;
            }
        }
        if (g_DrawToggleKey)
        {
            if (!RegisterHotKey(hWnd, DRAW_HOTKEY, g_DrawToggleMod, g_DrawToggleKey & 0xFF))
            {
                MessageBox(hWnd, L"The specified draw w/out zoom hotkey is already in use.\nSelect a different draw w/out zoom hotkey.", APPNAME, MB_ICONERROR);
                showOptions = TRUE;
            }
        }
        if (g_BreakToggleKey)
        {
            if (!RegisterHotKey(hWnd, BREAK_HOTKEY, g_BreakToggleMod, g_BreakToggleKey & 0xFF))
            {
                MessageBox(hWnd, L"The specified break timer hotkey is already in use.\nSelect a different break timer hotkey.", APPNAME, MB_ICONERROR);
                showOptions = TRUE;
            }
        }
        
        if (g_SnipToggleKey)
        {
            if (!RegisterHotKey(hWnd, SNIP_HOTKEY, g_SnipToggleMod, g_SnipToggleKey & 0xFF) ||
                !RegisterHotKey(hWnd, SNIP_SAVE_HOTKEY, (g_SnipToggleMod ^ MOD_SHIFT), g_SnipToggleKey & 0xFF))
            {
                MessageBox(hWnd, L"The specified snip hotkey is already in use.\nSelect a different snip hotkey.", APPNAME, MB_ICONERROR);
                showOptions = TRUE;
            }
        }
        
        if (showOptions)
        {
            // To open the PowerToys settings in the ZoomIt page.
            SendMessage(hWnd, WM_COMMAND, IDC_OPTIONS, 0);
        }
        break;
    }
    case WM_COMMAND:

        switch(LOWORD( wParam )) {

        case IDC_SAVE_CROP:
        case IDC_SAVE:
        {
            POINT local_savedCursorPos{};
            if( lParam != SHALLOW_ZOOM )
            {
                GetCursorPos(&local_savedCursorPos);
            }

            HBITMAP     hInterimSaveBitmap = nullptr;
            HDC         hInterimSaveDc;
            HBITMAP     hSaveBitmap = nullptr;
            HDC         hSaveDc;
            int         copyX, copyY;
            int         copyWidth, copyHeight;

            if ( LOWORD( wParam ) == IDC_SAVE_CROP )
            {
                g_SelectionActive = TRUE;
                SelectRectangle selectRectangle;
                if( !selectRectangle.Start( hWnd ) )
                {
                    g_SelectionActive = FALSE;
                    if( lParam != SHALLOW_ZOOM )
                    {
                        SetCursorPos(local_savedCursorPos.x, local_savedCursorPos.y);
                    }
                    break;
                }
                auto copyRc = selectRectangle.SelectedRect();
                selectRectangle.Stop();
                g_SelectionActive = FALSE;
                copyX = copyRc.left;
                copyY = copyRc.top;
                copyWidth = copyRc.right - copyRc.left;
                copyHeight = copyRc.bottom - copyRc.top;
            }
            else
            {
                copyX = 0;
                copyY = 0;
                copyWidth = width;
                copyHeight = height;
            }
            OutputDebug( L"***x: %d, y: %d, width: %d, height: %d\n", copyX, copyY, copyWidth, copyHeight );

            RECT oldClipRect{};
            GetClipCursor( &oldClipRect );
            ClipCursor( NULL );

            // Capture the screen before displaying the save dialog
            hInterimSaveDc = CreateCompatibleDC( hdcScreen );
            hInterimSaveBitmap = CreateCompatibleBitmap( hdcScreen, copyWidth, copyHeight );
            SelectObject( hInterimSaveDc, hInterimSaveBitmap );

            hSaveDc = CreateCompatibleDC( hdcScreen );
            if (!hInterimSaveDc || !hInterimSaveBitmap || !hSaveDc) {
                DeleteDC(hInterimSaveDc); DeleteDC(hSaveDc); DeleteObject(hInterimSaveBitmap);
                ClipCursor(&oldClipRect);
                MessageBox(hWnd, L"Not enough graphics resources to save this image.", APPNAME, MB_ICONERROR);
                break;
            }
#if SCALE_HALFTONE
            SetStretchBltMode( hInterimSaveDc, HALFTONE );
            SetStretchBltMode( hSaveDc, HALFTONE );
#else
            SetStretchBltMode( hInterimSaveDc, COLORONCOLOR );
            SetStretchBltMode( hSaveDc, COLORONCOLOR );
#endif
            StretchBlt( hInterimSaveDc,
                        0, 0,
                        copyWidth, copyHeight,
                        hdcScreen,
                        monInfo.rcMonitor.left + copyX,
                        monInfo.rcMonitor.top + copyY,
                        copyWidth, copyHeight,
                        SRCCOPY|CAPTUREBLT );

            g_bSaveInProgress = true;
            memset( &openFileName, 0, sizeof(openFileName ));
            openFileName.lStructSize       = OPENFILENAME_SIZE_VERSION_400;
            openFileName.hwndOwner         = hWnd;
            openFileName.hInstance         = static_cast<HINSTANCE>(g_hInstance);
            openFileName.nMaxFile          = sizeof(filePath)/sizeof(filePath[0]);
            openFileName.Flags				= OFN_LONGNAMES|OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT;
            openFileName.lpstrTitle        = L"Save zoomed screen...";
            openFileName.lpstrDefExt       = L"png";
            openFileName.nFilterIndex      = 1;
            openFileName.lpstrFilter       = L"Zoomed PNG\0*.png\0"
                                             //"Zoomed BMP\0*.bmp\0"	
                                             "Actual size PNG\0*.png\0\0";
                                             //"Actual size BMP\0*.bmp\0\0";
            openFileName.lpstrFile			= filePath;
            BOOL saveAccepted;
#ifdef ZOOMIT_TESTING
            if (g_TestMode) {
                saveAccepted = g_TestSavePath != nullptr;
                if (saveAccepted) {
                    wcscpy_s(filePath, g_TestSavePath);
                    openFileName.nFilterIndex = g_TestSaveFilter;
                }
            } else
#endif
            saveAccepted = GetSaveFileName(&openFileName);
            if (saveAccepted)
            {
                DWORD saveResult = ERROR_SUCCESS;
                TCHAR targetFilePath[MAX_PATH];
                _tcscpy_s(targetFilePath, filePath);

                // Save image at screen size
                if( openFileName.nFilterIndex == 1 )
                {
                    saveResult = SavePng(targetFilePath, hInterimSaveBitmap);
                }
                // Save image scaled down to actual size
                else
                {
                    int saveWidth = (std::max)(1, static_cast<int>(copyWidth / zoomLevel));
                    int saveHeight = (std::max)(1, static_cast<int>(copyHeight / zoomLevel));

                    hSaveBitmap = CreateCompatibleBitmap( hdcScreen, saveWidth, saveHeight );
                    SelectObject( hSaveDc, hSaveBitmap );

                    StretchBlt( hSaveDc,
                                0, 0,
                                saveWidth, saveHeight,
                                hInterimSaveDc,
                                0,
                                0,
                                copyWidth, copyHeight,
                                SRCCOPY | CAPTUREBLT );
				
                    saveResult = SavePng(targetFilePath, hSaveBitmap);
                }
                if (saveResult != ERROR_SUCCESS)
                    MessageBox(hWnd, L"Unable to save this image. Check the destination and available resources.", APPNAME, MB_ICONERROR);
            }
            g_bSaveInProgress = false;

            DeleteDC( hInterimSaveDc );
            DeleteDC( hSaveDc );
            DeleteObject(hInterimSaveBitmap);
            if (hSaveBitmap) DeleteObject(hSaveBitmap);

            if( lParam != SHALLOW_ZOOM )
            {
                SetCursorPos(local_savedCursorPos.x, local_savedCursorPos.y);
            }
            ClipCursor( &oldClipRect );
            break;
        }

        case IDC_COPY_CROP:
        case IDC_COPY: {
            HBITMAP		hSaveBitmap;
            HDC			hSaveDc;
            int         copyX, copyY;
            int         copyWidth, copyHeight;

            if( LOWORD( wParam ) == IDC_COPY_CROP )
            {
                g_SelectionActive = TRUE;
                POINT local_savedCursorPos{};
                if( lParam != SHALLOW_ZOOM )
                {
                    GetCursorPos(&local_savedCursorPos);
                }
                SelectRectangle selectRectangle;
                if( !selectRectangle.Start( hWnd ) )
                {
                    g_SelectionActive = FALSE;
                    break;
                }
                auto copyRc = selectRectangle.SelectedRect();
                selectRectangle.Stop();
                if( lParam != SHALLOW_ZOOM )
                {
                    SetCursorPos(local_savedCursorPos.x, local_savedCursorPos.y);
                }
                g_SelectionActive = FALSE;

                copyX = copyRc.left;
                copyY = copyRc.top;
                copyWidth = copyRc.right - copyRc.left;
                copyHeight = copyRc.bottom - copyRc.top;
            }
            else
            {
                copyX = 0;
                copyY = 0;
                copyWidth = width;
                copyHeight = height;
            }
            OutputDebug( L"***x: %d, y: %d, width: %d, height: %d\n", copyX, copyY, copyWidth, copyHeight );

            hSaveBitmap = CreateCompatibleBitmap( hdcScreen, copyWidth, copyHeight );
            hSaveDc = CreateCompatibleDC( hdcScreen );
            if (!hSaveBitmap || !hSaveDc) {
                DeleteDC(hSaveDc); DeleteObject(hSaveBitmap);
                MessageBox(hWnd, L"Not enough graphics resources to copy this image.", APPNAME, MB_ICONERROR);
                break;
            }
            SelectObject( hSaveDc, hSaveBitmap );
#if SCALE_HALFTONE
            SetStretchBltMode( hSaveDc, HALFTONE );
#else
            SetStretchBltMode( hSaveDc, COLORONCOLOR );
#endif
			StretchBlt( hSaveDc,
                        0, 0,
                        copyWidth, copyHeight,
                        hdcScreen,
                        monInfo.rcMonitor.left + copyX,
                        monInfo.rcMonitor.top + copyY,
                        copyWidth, copyHeight,
                        SRCCOPY|CAPTUREBLT ); 

            DeleteDC(hSaveDc);
            hSaveDc = nullptr;
#ifdef ZOOMIT_TESTING
            if (g_TestMode) {
                if (g_TestSnipBitmap) DeleteObject(g_TestSnipBitmap);
                g_TestSnipBitmap = hSaveBitmap;
                hSaveBitmap = nullptr;
            } else
#endif
            if (OpenClipboard(hWnd)) {
                if (EmptyClipboard() && SetClipboardData(CF_BITMAP, hSaveBitmap))
                    hSaveBitmap = nullptr;
                CloseClipboard();
            }
            if (hSaveBitmap) DeleteObject(hSaveBitmap);
            }
            break;

        case IDC_DRAW: 
            PostMessage( hWnd, WM_HOTKEY, DRAW_HOTKEY, 1 );
            break;

        case IDC_ZOOM:
            PostMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 1 );
            break;


        case IDC_OPTIONS:
            // Don't show win32 forms options if started by PowerToys.
            // Show the PowerToys Settings application instead.

            if( g_StartedByPowerToys )
            {
            }
            else
            {
                DialogBox( g_hInstance, L"OPTIONS", hWnd, OptionsProc );
            }
            break;

        case IDC_BREAK:
        {
            // Manage handles, clean visual transitions, and Options delta
            if( g_TimerActive )
            {
                if( activeBreakShowBackgroundFile != g_BreakShowBackgroundFile ||
                    activeBreakShowDesktop != g_BreakShowDesktop )
                {
                    if( g_BreakShowBackgroundFile && !g_BreakShowDesktop )
                    {
                        SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_DESTROY );
                    }
                    else
                    {
                        SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
                    }
                }
                else
                {
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, SHALLOW_DESTROY );
                    g_TimerActive = TRUE;
                }
            }

            hdcScreen = CreateDC( L"DISPLAY", static_cast<PTCHAR>(NULL), static_cast<PTCHAR>(NULL), static_cast<CONST DEVMODE*>(NULL) );

            // toggle second monitor
            // FIX: we should save whether or not we've switched to a second monitor
            // rather than just assume that the setting hasn't changed since the break timer
            // became active
            if( g_BreakOnSecondary )
            {
                EnableDisableSecondaryDisplay( hWnd, TRUE, &secondaryDevMode );
            }

            // Determine what monitor we're on
            GetCursorPos( &cursorPos );
            UpdateMonitorInfo( cursorPos, &monInfo );
            width = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
            height = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;

            // Trigger desktop recapture as necessary when switching monitors
            if( g_TimerActive && g_BreakShowDesktop && !EqualRect(&lastMonInfo.rcMonitor, &monInfo.rcMonitor) )
            {
                lastMonInfo = monInfo;
                SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
                PostMessage( hWnd, WM_COMMAND, IDC_BREAK, 0 );
                break;
            }
            lastMonInfo = monInfo;

            // If the background is a file that hasn't been collected, grab it now
            if( g_BreakShowBackgroundFile && !g_BreakShowDesktop &&
                ( !g_TimerActive || wcscmp( activeBreakBackgroundFile, g_BreakBackgroundFile ) ) )
            {
                _tcscpy( activeBreakBackgroundFile, g_BreakBackgroundFile );
                
                DeleteDC( g_hDcBackgroundFile ); g_hDcBackgroundFile = nullptr;
                DeleteObject( g_hBackgroundBmp );
                
                g_hBackgroundBmp = NULL;
                g_hBackgroundBmp = LoadImageFile( g_BreakBackgroundFile );
                if( g_hBackgroundBmp == NULL )
                {
                    // Clean up hanging handles
                    SendMessage( hWnd, WM_HOTKEY, ZOOM_HOTKEY, 0 );
                    ErrorDialog( hWnd, L"Error loading background bitmap", GetLastError() );
                    break;
                }
                g_hDcBackgroundFile = CreateCompatibleDC( hdcScreen );
                SelectObject( g_hDcBackgroundFile, g_hBackgroundBmp );
            }
            // If the background is a desktop that hasn't been collected, grab it now
            else if( g_BreakShowBackgroundFile && g_BreakShowDesktop && !g_TimerActive )
            {
                g_hBackgroundBmp = CreateFadedDesktopBackground(hdcScreen, &monInfo.rcMonitor, NULL);
                g_hDcBackgroundFile = CreateCompatibleDC( hdcScreen );
                SelectObject( g_hDcBackgroundFile, g_hBackgroundBmp );
            }

            // Track Options.Break delta
            activeBreakShowBackgroundFile = g_BreakShowBackgroundFile;
            activeBreakShowDesktop = g_BreakShowDesktop;

            g_TimerActive = TRUE;

            breakTimeout = g_BreakTimeout * 60 + 1;

            // Create font
            g_LogFont.lfHeight = height / 5;
            hTimerFont = CreateFontIndirect( &g_LogFont );
            g_LogFont.lfHeight = height / 8;
            hNegativeTimerFont = CreateFontIndirect( &g_LogFont );

            // Create backing bitmap
            hdcScreenCompat = CreateCompatibleDC(hdcScreen); 
            bmp.bmBitsPixel = static_cast<BYTE>(GetDeviceCaps(hdcScreen, BITSPIXEL));
            bmp.bmPlanes = static_cast<BYTE>(GetDeviceCaps(hdcScreen, PLANES));
            bmp.bmWidth = width;
            bmp.bmHeight = height;
            bmp.bmWidthBytes = ((bmp.bmWidth + 15) &~15)/8; 
            hbmpCompat = CreateBitmap(bmp.bmWidth, bmp.bmHeight, 
                bmp.bmPlanes, bmp.bmBitsPixel, static_cast<CONST VOID *>(NULL)); 
             SelectObject(hdcScreenCompat, hbmpCompat); 

            if (!hdcScreen || !hdcScreenCompat || !hbmpCompat || !hTimerFont || !hNegativeTimerFont) {
                releaseSession(); g_TimerActive = FALSE;
                MessageBox(hWnd, L"Not enough graphics resources to enter timer mode.", APPNAME, MB_ICONERROR);
                break;
            }
            SetTextColor( hdcScreenCompat, g_BreakPenColor );
            SetBkMode( hdcScreenCompat, TRANSPARENT );
            SelectObject( hdcScreenCompat, hTimerFont );

            EnableDisableOpacity( hWnd, TRUE );
            EnableDisableScreenSaver( FALSE );

            SendMessage( hWnd, WM_TIMER, 0, 0 );
            SetTimer( hWnd, 0, 1000, NULL );

            BringWindowToTop( hWnd );
            SetForegroundWindow( hWnd );
            SetActiveWindow( hWnd );
            SetWindowPos( hWnd, HWND_NOTOPMOST, monInfo.rcMonitor.left, monInfo.rcMonitor.top, 
                    width, height, SWP_SHOWWINDOW );
        }
        break;

        case IDCANCEL:

            memset( &tNotifyIconData, 0, sizeof(tNotifyIconData));
            tNotifyIconData.cbSize = sizeof(NOTIFYICONDATA); 
            tNotifyIconData.hWnd = hWnd; 
            tNotifyIconData.uID = 1; 
            Shell_NotifyIcon(NIM_DELETE, &tNotifyIconData); 
            PersistSettings();

            if( hWndOptions )
            {
                DestroyWindow( hWndOptions );
            }
            DestroyWindow( hWnd );
            break;
        }
        break;

    case WM_TIMER:
        switch( wParam ) {
        case 0:
            //
            // Break timer
            //
            breakTimeout -= 1;
            InvalidateRect( hWnd, NULL, FALSE );
            if( breakTimeout == 0 && g_BreakPlaySoundFile ) {

                PlaySound( g_BreakSoundFile, NULL, SND_FILENAME|SND_ASYNC );
            } 
            break;

        case 2:
        case 1:
            //
            // Telescoping zoom timer
            //
            if( zoomTelescopeStep ) {

                zoomLevel *= zoomTelescopeStep;
                if( (zoomTelescopeStep > 1 && zoomLevel >= zoomTelescopeTarget ) ||
                    (zoomTelescopeStep < 1 && zoomLevel <= zoomTelescopeTarget )) {

                    zoomLevel = zoomTelescopeTarget;
                    KillTimer( hWnd, wParam );
                    OutputDebug( L"SETCURSOR mon_left: %x mon_top: %x x: %d y: %d\n",
                            monInfo.rcMonitor.left, monInfo.rcMonitor.top, cursorPos.x, cursorPos.y );
                    SetCursorPos( monInfo.rcMonitor.left + cursorPos.x, 
                                        monInfo.rcMonitor.top + cursorPos.y );
                } 

            } else {

                // Case where we didn't zoom at all
                KillTimer( hWnd, wParam );
            }
            if( wParam == 2 && zoomLevel == 1 ) {

                g_Zoomed = FALSE;
                if( g_ZoomOnLiveZoom )
                {
                    GetCursorPos( &cursorPos );
                    cursorPos = ScalePointInRects( cursorPos, monInfo.rcMonitor, g_LiveZoomSourceRect );
                    SetCursorPos( cursorPos.x, cursorPos.y );
                    SendMessage(hWnd, WM_HOTKEY, LIVE_HOTKEY, 0);
                }
                else if( lParam != SHALLOW_ZOOM )
                {
                    // Figure out where final unzoomed cursor should be
                    if (g_Drawing) {
                        cursorPos = prevPt;
                    }
                    OutputDebug(L"FINAL MOUSE: x: %d y: %d\n", cursorPos.x, cursorPos.y );
                    GetZoomedTopLeftCoordinates(zoomLevel, &cursorPos, &x, width, &y, height);
                    cursorPos.x = monInfo.rcMonitor.left + x + static_cast<int>((cursorPos.x - x) * zoomLevel);
                    cursorPos.y = monInfo.rcMonitor.top + y + static_cast<int>((cursorPos.y - y) * zoomLevel);		
                    SetCursorPos(cursorPos.x, cursorPos.y);
                }
                if( hTargetWindow ) {

                    SetWindowPos( hTargetWindow, HWND_BOTTOM, rcTargetWindow.left, rcTargetWindow.top,
                            rcTargetWindow.right - rcTargetWindow.left, 
                            rcTargetWindow.bottom - rcTargetWindow.top, 0 );
                    hTargetWindow = NULL;
                }
                DeleteDrawUndoList( &drawUndoList );

                // Restore live zoom if we came from that mode
                if( g_ZoomOnLiveZoom ) {

                    SendMessage( g_hWndLiveZoom, WM_USER_SET_ZOOM, EncodeZoomLevel(g_LiveZoomLevel), reinterpret_cast<LPARAM>(&g_LiveZoomSourceRect) );
                    g_ZoomOnLiveZoom = FALSE;
                    if (IsWindowVisible(g_hWndLiveZoom))
                        SendMessage(g_hWndLiveZoom, WM_USER_MAGNIFY_CURSOR, TRUE, 0);
                    else if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
                    forcePenResize = TRUE;
                }

                SetForegroundWindow( g_ActiveWindow );
                ClipCursor( NULL );
                g_HaveDrawn = FALSE;


                g_Drawing = FALSE;
                EnableDisableStickyKeys( TRUE );
                releaseSession();

                SetFocus( g_ActiveWindow );
                ShowWindow( hWnd, SW_HIDE );
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
            }
            InvalidateRect( hWnd, NULL, FALSE );
            break;

        case 3:
            POINT mousePos;
            GetCursorPos(&mousePos);
            if (mousePos.x != cursorPos.x || mousePos.y != cursorPos.y)
            {
                MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
                UpdateMonitorInfo(mousePos, &monitorInfo);

                mousePos.x -= monitorInfo.rcMonitor.left;
                mousePos.y -= monitorInfo.rcMonitor.top;

                OutputDebug(L"RETRACKING MOUSE: x: %d y: %d\n", mousePos.x, mousePos.y);
                SendMessage(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(mousePos.x, mousePos.y));
            }
            break;
        }
        break;

    case WM_PAINT:

        hDc = BeginPaint(hWnd, &ps); 

        if( ( ( g_SelectionActive == FALSE ) || ( zoomLevel == 1 ) ) && g_Zoomed ) {

            OutputDebug( L"PAINT x: %d y: %d width: %d height: %d zoomLevel: %g\n",
                    cursorPos.x, cursorPos.y, width, height, zoomLevel );
            GetZoomedTopLeftCoordinates( zoomLevel, &cursorPos, &x, width, &y, height );
#if SCALE_GDIPLUS
            if ( zoomLevel >= zoomTelescopeTarget )  {
                // do a high-quality render
                extern void ScaleImage( HDC hdcDst, float xDst, float yDst, float wDst, float hDst, 
                                        HBITMAP bmSrc, float xSrc, float ySrc, float wSrc, float hSrc );

                ScaleImage( ps.hdc, 
                            0, 0, 
                            (float)bmp.bmWidth, (float)bmp.bmHeight, 
                            hbmpCompat, 
                            (float)x, (float)y, 
                            width/zoomLevel, height/zoomLevel ); 
            } else {
                // do a fast, less accurate render
                SetStretchBltMode( hDc, HALFTONE );
                StretchBlt( ps.hdc, 
                        0, 0, 
                        bmp.bmWidth, bmp.bmHeight, 
                        hdcScreenCompat, 
                        x, y, 
                        (int) (width/zoomLevel), (int) (height/zoomLevel),
                        SRCCOPY); 
            }
#else
#if SCALE_HALFTONE
            SetStretchBltMode( hDc, zoomLevel == zoomTelescopeTarget ? HALFTONE : COLORONCOLOR );
#else
            SetStretchBltMode( hDc, COLORONCOLOR );
#endif
            StretchBlt( ps.hdc, 
                    0, 0, 
                    bmp.bmWidth, bmp.bmHeight, 
                    hdcScreenCompat, 
                    x, y, 
                    static_cast<int>(width/zoomLevel), static_cast<int>(height/zoomLevel),
                    SRCCOPY|CAPTUREBLT ); 
#endif
        } else if( g_TimerActive ) {

            // Fill bitmap with white
            rc.top = rc.left = 0;
            rc.bottom = height;
            rc.right = width;
            FillRect( hdcScreenCompat, &rc, GetSysColorBrush( COLOR_WINDOW ));

            // If there's a background bitmap, draw it in the center
            if( g_hBackgroundBmp ) {

                BITMAP local_bmp;
                GetObject(g_hBackgroundBmp, sizeof(local_bmp), &local_bmp);
                SetStretchBltMode( hdcScreenCompat, HALFTONE );
                if( g_BreakBackgroundStretch ) {
                    StretchBlt( hdcScreenCompat, 0, 0, width, height,
                        g_hDcBackgroundFile, 0, 0, local_bmp.bmWidth, local_bmp.bmHeight, SRCCOPY|CAPTUREBLT  );
                } else {
                    BitBlt( hdcScreenCompat, width/2 - local_bmp.bmWidth/2, height/2 - local_bmp.bmHeight/2, 
                        local_bmp.bmWidth, local_bmp.bmHeight, g_hDcBackgroundFile, 0, 0, SRCCOPY|CAPTUREBLT  );
                }
            }

            // Draw time
            if( breakTimeout > 0 ) {

                _stprintf( timerText, L"% 2d:%02d", breakTimeout/60, breakTimeout % 60 );
            
            } else {

                _tcscpy( timerText, L"0:00" );
            }
            rc.left = rc.top = 0;
            DrawText( hdcScreenCompat, timerText, -1, &rc, 
                DT_NOCLIP|DT_LEFT|DT_NOPREFIX|DT_CALCRECT );

            rc1.left = rc1.right = rc1.bottom = rc1.top = 0;
            if( g_ShowExpiredTime && breakTimeout < 0 ) {

                _stprintf( negativeTimerText, L"(-% 2d:%02d)",
                        -breakTimeout/60, -breakTimeout % 60 );
                HFONT prevFont = static_cast<HFONT>(SelectObject( hdcScreenCompat, hNegativeTimerFont ));
                DrawText( hdcScreenCompat, negativeTimerText, -1, &rc1, 
                    DT_NOCLIP|DT_LEFT|DT_NOPREFIX|DT_CALCRECT );
                SelectObject( hdcScreenCompat, prevFont );
            }

            // Position time vertically
            switch( g_BreakTimerPosition ) {
            case 0:
            case 1:
            case 2:
                rc.top = 50;
                break;
            case 3:
            case 4:
            case 5:
                rc.top = (height - (rc.bottom - rc.top))/2;
                break;
            case 6:
            case 7:
            case 8:
                rc.top = height - rc.bottom - 50 - rc1.bottom;
                break;
            }

            // Position time horizontally
            switch( g_BreakTimerPosition ) {
            case 0:
            case 3:
            case 6:
                rc.left = 50;
                break;
            case 1:
            case 4:
            case 7:
                rc.left = (width - (rc.right - rc.left))/2;
                break;
            case 2:
            case 5:
            case 8:
                rc.left = width - rc.right - 50;
                break;
            }
            rc.bottom += rc.top;
            rc.right += rc.left;

            DrawText( hdcScreenCompat, timerText, -1, &rc, DT_NOCLIP|DT_LEFT|DT_NOPREFIX );

            if( g_ShowExpiredTime && breakTimeout < 0 ) {

                rc1.top = rc.bottom + 10;
                rc1.left = rc.left + ((rc.right - rc.left)-(rc1.right-rc1.left))/2;
                HFONT prevFont = static_cast<HFONT>(SelectObject( hdcScreenCompat, hNegativeTimerFont ));
                DrawText( hdcScreenCompat, negativeTimerText, -1, &rc1, 
                    DT_NOCLIP|DT_LEFT|DT_NOPREFIX );
                SelectObject( hdcScreenCompat, prevFont );
            }

            // Copy to screen
            BitBlt( ps.hdc, 0, 0, width, height, hdcScreenCompat, 0, 0, SRCCOPY|CAPTUREBLT  );
        }
        EndPaint(hWnd, &ps); 
        return TRUE;

    case WM_DESTROY:

        KillTimer(hWnd, 0); KillTimer(hWnd, 1); KillTimer(hWnd, 2); KillTimer(hWnd, 3);
        if (g_TimerActive) EnableDisableScreenSaver(TRUE);
        EnableDisableStickyKeys(TRUE);
        ClipCursor(nullptr);
        if (IsWindow(g_hWndLiveZoom)) DestroyWindow(g_hWndLiveZoom);
        if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        releaseSession();
        DeleteDC(g_hDcBackgroundFile); g_hDcBackgroundFile = nullptr;
        DeleteObject(g_hBackgroundBmp); g_hBackgroundBmp = nullptr;
        UnregisterAllHotkeys(hWnd);
        PostQuitMessage( 0 );
        break;

    default:
        if( message == wmTaskbarCreated )
        {
            if( g_ShowTrayIcon )
            {
                EnableDisableTrayIcon( hWnd, TRUE );
            }
            return TRUE;
        }
       return DefWindowProc(hWnd, message, wParam, lParam );
    }
    return 0;
}


//----------------------------------------------------------------------------
//
// LiveZoomWndProc
//
//----------------------------------------------------------------------------
LRESULT CALLBACK LiveZoomWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    RECT		rc;
    POINT		cursorPos;
    static int	width, height;
    static MONITORINFO	monInfo;
#if 0
    int			delta;
    BOOLEAN		zoomIn;
#endif
    static POINT	lastCursorPos;
    POINT			adjustedCursorPos, zoomCenterPos;
    int				moveWidth, moveHeight;
    int				sourceRectHeight, sourceRectWidth;
    DWORD			curTickCount;
    RECT			sourceRect{};
    static RECT		lastSourceRect;
    static float	zoomLevel;
    static float	zoomTelescopeStep;
    static float	zoomTelescopeTarget;
    static DWORD	prevZoomStepTickCount = 0;
    static BOOL		dwmEnabled = FALSE;
    static BOOLEAN	startedInPresentationMode = FALSE;
    MAGTRANSFORM matrix;

    switch (message)  {
    case WM_CREATE:

        // Initialize
        // Magnification is initialized once by the application.
        if (pDwmIsCompositionEnabled) pDwmIsCompositionEnabled(&dwmEnabled);

        // Create the zoom window
        if( !g_fullScreenWorkaround ) {

            g_hWndLiveZoomMag = CreateWindowEx( 0,
                                        WC_MAGNIFIER,
                                        TEXT("MagnifierWindow"),
                                        WS_CHILD | MS_SHOWMAGNIFIEDCURSOR | WS_VISIBLE,
                                        0, 0, 0, 0, hWnd, NULL, g_hInstance, NULL );
        }

        if (!g_fullScreenWorkaround && !g_hWndLiveZoomMag) return -1;
        ShowWindow( hWnd, SW_SHOW );
        InvalidateRect( g_hWndLiveZoomMag, NULL, TRUE );

        if( !g_fullScreenWorkaround )
            SetForegroundWindow(static_cast<HWND>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));

        // If we're not on Win7+, then set a timer to go off two hours from
        // now
        if( g_OsVersion < WIN7_VERSION ) {

            startedInPresentationMode = IsPresentationMode();
            // if we're not in presentation mode, kill ourselves after a timeout
            if( !startedInPresentationMode ) {

                SetTimer( hWnd, 1, LIVEZOOM_WINDOW_TIMEOUT, NULL );
            } 
        }
        break;

    case WM_SHOWWINDOW:
        if( wParam == TRUE ) {

            // Determine what monitor we're on
            lastCursorPos.x = -1;
            GetCursorPos( &cursorPos );
            UpdateMonitorInfo( cursorPos, &monInfo );
            width = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
            height = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
            lastSourceRect.left = lastSourceRect.top = 0;
            lastSourceRect.right = width;
            lastSourceRect.bottom = height;

            // Set window size
            if( !g_fullScreenWorkaround ) {

                SetWindowPos( hWnd, NULL, monInfo.rcMonitor.left, monInfo.rcMonitor.top,
                    monInfo.rcMonitor.right - monInfo.rcMonitor.left,
                    monInfo.rcMonitor.bottom - monInfo.rcMonitor.top,
                    SWP_NOACTIVATE | SWP_NOZORDER );
                UpdateWindow(hWnd);
            }

            // Are we coming back from a static zoom that 
            // was started while we were live zoomed?
            if( g_ZoomOnLiveZoom ) {

                // Force a zoom to 2x without telescope
                prevZoomStepTickCount = 0;
                zoomLevel = static_cast<float>(1.9);
                zoomTelescopeTarget = 2.0;
                zoomTelescopeStep = 2.0;

            } else {

                zoomTelescopeStep = ZOOM_LEVEL_STEP_IN;
                zoomTelescopeTarget = g_ZoomLevels[g_SliderZoomLevel];

                prevZoomStepTickCount = 0;
                if( dwmEnabled ) {

                    zoomLevel = static_cast<float>(1);

                } else {

                    zoomLevel = static_cast<float>(1.9);
                }
            }
            RegisterHotKey( hWnd, 0, MOD_CONTROL, VK_UP );
            RegisterHotKey( hWnd, 1, MOD_CONTROL, VK_DOWN );

            // Hide hardware cursor
            if( !g_fullScreenWorkaround )
                if( pMagShowSystemCursor ) pMagShowSystemCursor( FALSE );

            GetCursorPos( &lastCursorPos );
            SetCursorPos( lastCursorPos.x, lastCursorPos.y );

            SendMessage( hWnd, WM_TIMER, 0, 0);
            SetTimer( hWnd, 0, ZOOM_LEVEL_STEP_TIME, NULL );
        
        } else {

            KillTimer( hWnd, 0 );

            if( !g_fullScreenWorkaround )
                if( pMagShowSystemCursor ) pMagShowSystemCursor( TRUE );

            // Reset the timer to expire two hours from now
            if( g_OsVersion < WIN7_VERSION && !IsPresentationMode()) {

                KillTimer( hWnd, 1 );
                SetTimer( hWnd, 1, LIVEZOOM_WINDOW_TIMEOUT, NULL );
            } else {

                DestroyWindow( hWnd );
                return 0;
            }
            UnregisterHotKey( hWnd, 0 );
            UnregisterHotKey( hWnd, 1 );
        }
        break;

    case WM_TIMER:
        switch( wParam ) {
        case 0: {
            // if we're cropping, do not move
            if( g_SelectionActive == TRUE )
            {
                // Still redraw to keep the contents live
                InvalidateRect( g_hWndLiveZoomMag, nullptr, TRUE );
                break;
            }

            GetCursorPos(&cursorPos);

            // Reclaim topmost status, to prevent unmagnified menus from remaining in view. 
            memset(&matrix, 0, sizeof(matrix));
            if( !g_fullScreenWorkaround ) {

                pSetLayeredWindowAttributes( hWnd, 0, 255, LWA_ALPHA );
                SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
                
                OutputDebug(L"LIVEZOOM RECLAIM\n");
            }

            sourceRectWidth = lastSourceRect.right - lastSourceRect.left;
            sourceRectHeight = lastSourceRect.bottom - lastSourceRect.top;
            moveWidth = sourceRectWidth/LIVEZOOM_MOVE_REGIONS;
            moveHeight = sourceRectHeight/LIVEZOOM_MOVE_REGIONS;
            curTickCount = GetTickCount();
            if( zoomLevel != zoomTelescopeTarget && 
                (prevZoomStepTickCount == 0 || (curTickCount - prevZoomStepTickCount > ZOOM_LEVEL_STEP_TIME)) ) {

                prevZoomStepTickCount = curTickCount;
                if( (zoomTelescopeStep > 1 && zoomLevel*zoomTelescopeStep >= zoomTelescopeTarget ) ||
                    (zoomTelescopeStep < 1 && zoomLevel*zoomTelescopeStep <= zoomTelescopeTarget )) {

                    zoomLevel = zoomTelescopeTarget;

                } else {

                    zoomLevel *= zoomTelescopeStep;
                }				
                // Time to exit zoom mode?
                if( zoomTelescopeTarget == 1 && zoomLevel == 1 ) {

                    ShowWindow( hWnd, SW_HIDE );
                    return 0;

                } else {

                    matrix.v[0][0] = zoomLevel;
                    matrix.v[0][2] = (static_cast<float>(-lastSourceRect.left) * zoomLevel);
                    matrix.v[1][1] = zoomLevel;
                    matrix.v[1][2] = (static_cast<float>(-lastSourceRect.top) * zoomLevel );
                    matrix.v[2][2] = 1.0f;
                }
                
                //
                // Pre-adjust for monitor boundary
                //
                adjustedCursorPos.x = cursorPos.x - monInfo.rcMonitor.left;
                adjustedCursorPos.y = cursorPos.y - monInfo.rcMonitor.top;
                GetZoomedTopLeftCoordinates( zoomLevel, &adjustedCursorPos, reinterpret_cast<int *>(&zoomCenterPos.x), width, 
                                reinterpret_cast<int *>(&zoomCenterPos.y), height );

                //
                // Add back monitor boundary
                //
                zoomCenterPos.x += monInfo.rcMonitor.left + static_cast<LONG>(width/zoomLevel/2);
                zoomCenterPos.y += monInfo.rcMonitor.top + static_cast<LONG>(height/zoomLevel/2);

            } else {

                int xOffset = cursorPos.x - lastSourceRect.left;
                int yOffset = cursorPos.y - lastSourceRect.top;
                zoomCenterPos.x = 0; 
                zoomCenterPos.y = 0; 
                if( xOffset < moveWidth ) 
                    zoomCenterPos.x = lastSourceRect.left + sourceRectWidth/2 - (moveWidth - xOffset);
                else if( xOffset > moveWidth * (LIVEZOOM_MOVE_REGIONS-1) ) 
                    zoomCenterPos.x = lastSourceRect.left + sourceRectWidth/2 + (xOffset - moveWidth*(LIVEZOOM_MOVE_REGIONS-1));
                if( yOffset < moveHeight )
                    zoomCenterPos.y = lastSourceRect.top + sourceRectHeight/2 - (moveHeight - yOffset);
                else if( yOffset > moveHeight * (LIVEZOOM_MOVE_REGIONS-1) )
                    zoomCenterPos.y = lastSourceRect.top + sourceRectHeight/2 + (yOffset - moveHeight*(LIVEZOOM_MOVE_REGIONS-1));
            }
            if( matrix.v[0][0] || zoomCenterPos.x || zoomCenterPos.y ) {
                
                if( zoomCenterPos.y == 0 ) 
                    zoomCenterPos.y = lastSourceRect.top + sourceRectHeight/2;
                if( zoomCenterPos.x == 0 )
                    zoomCenterPos.x = lastSourceRect.left + sourceRectWidth/2;

                int zoomWidth = static_cast<int>(width / zoomLevel);
                int zoomHeight = static_cast<int>(height/ zoomLevel);
                sourceRect.left = zoomCenterPos.x - zoomWidth / 2;
                sourceRect.top = zoomCenterPos.y -  zoomHeight / 2;

                // Don't scroll outside desktop area.
                if (sourceRect.left < monInfo.rcMonitor.left) 
                    sourceRect.left = monInfo.rcMonitor.left;
                else if (sourceRect.left > monInfo.rcMonitor.right - zoomWidth )
                    sourceRect.left = monInfo.rcMonitor.right - zoomWidth;
                sourceRect.right = sourceRect.left + zoomWidth;
                if (sourceRect.top < monInfo.rcMonitor.top) 
                    sourceRect.top = monInfo.rcMonitor.top;
                else if (sourceRect.top > monInfo.rcMonitor.bottom - zoomHeight) 
                    sourceRect.top = monInfo.rcMonitor.bottom - zoomHeight;
                sourceRect.bottom = sourceRect.top + zoomHeight;

                if( g_ZoomOnLiveZoom ) {

                    matrix.v[0][0] = static_cast<float>(1.0);
                    matrix.v[0][2] = (static_cast<float>(-monInfo.rcMonitor.left));

                    matrix.v[1][1] = static_cast<float>(1.0);
                    matrix.v[1][2] = (static_cast<float>(-monInfo.rcMonitor.top));

                    matrix.v[2][2] = 1.0f;

                } else if( lastSourceRect.left != sourceRect.left ||
                    lastSourceRect.top  != sourceRect.top ) {

                    matrix.v[0][0] = zoomLevel;
                    matrix.v[0][2] = (static_cast<float>(-sourceRect.left) * zoomLevel);

                    matrix.v[1][1] = zoomLevel;
                    matrix.v[1][2] = (static_cast<float>(-sourceRect.top) * zoomLevel);

                    matrix.v[2][2] = 1.0f;
                }
                lastSourceRect = sourceRect;
            }
            lastCursorPos = cursorPos;

            // Update source and zoom if necessary
            if( matrix.v[0][0] ) {

                OutputDebug(L"LIVEZOOM update\n");
                if( g_fullScreenWorkaround ) {

                    pMagSetFullscreenTransform(zoomLevel, sourceRect.left, sourceRect.top);
                    pMagSetInputTransform(TRUE, &sourceRect, &monInfo.rcMonitor);
                }
                else {

                    pMagSetWindowTransform(g_hWndLiveZoomMag, &matrix);
                }
            }

            if( !g_fullScreenWorkaround ) {

                // Force redraw to refresh screen contents
                InvalidateRect(g_hWndLiveZoomMag, NULL, TRUE);
            }

            // are we done zooming?
            if( zoomLevel == 1 ) {

                if( g_OsVersion < WIN7_VERSION ) {

                    ShowWindow( hWnd, SW_HIDE );

                } else {

                    DestroyWindow( hWnd );
                    return 0;
                }
            }
            }
            break;
        case 1: {

            if( !IsWindowVisible( hWnd )) {

                // This is the cached window timeout. If not in presentation mode,
                // time to exit
                if( !IsPresentationMode()) {

                    DestroyWindow( hWnd );
                    return 0;
                }
            } 
            }
            break;
        }
        break;

    case WM_SETTINGCHANGE:
        if( g_OsVersion < WIN7_VERSION ) {

            if( startedInPresentationMode && !IsPresentationMode()) {

                // Existing presentation mode
                DestroyWindow( hWnd );
            
            } else if( !startedInPresentationMode && IsPresentationMode()) {
        
                // Kill the timer if one was configured, because now
                // we're going to go away when they exit presentation mode
                KillTimer( hWnd, 1 );
            }
        }
        break;

    case WM_HOTKEY: {
        float newZoomLevel = zoomLevel;
        switch( wParam ) {
        case 0:
            // zoom in 
            if( newZoomLevel < ZOOM_LEVEL_MAX ) 
                newZoomLevel *= 2;
            zoomTelescopeStep = ZOOM_LEVEL_STEP_IN;
            break;

        case 1:
            if( newZoomLevel > 2 ) 
                newZoomLevel /= 2;
            else {

                newZoomLevel *= .75; 
                if( newZoomLevel < ZOOM_LEVEL_MIN ) 
                    newZoomLevel = ZOOM_LEVEL_MIN;
            }
            zoomTelescopeStep = ZOOM_LEVEL_STEP_OUT;
            break;
        }
        zoomTelescopeTarget = newZoomLevel;
        if( !dwmEnabled ) {

            zoomLevel = newZoomLevel;
        }
        }
        break;

    // NOTE: keyboard and mouse input actually don't get sent to us at all when in live zoom mode
    case WM_KEYDOWN:
        switch( wParam ) {
        case VK_ESCAPE:
            zoomTelescopeStep = ZOOM_LEVEL_STEP_OUT;
            zoomTelescopeTarget = 1.0;
            if( !dwmEnabled ) {

                zoomLevel = static_cast<float>(1.1);
            }
            break;

        case VK_UP:
            SendMessage( hWnd, WM_MOUSEWHEEL, 
                MAKEWPARAM( GetAsyncKeyState( VK_LCONTROL ) != 0 ? MK_CONTROL: 0, WHEEL_DELTA), 0 );
            return TRUE;

        case VK_DOWN:
            SendMessage( hWnd, WM_MOUSEWHEEL, 
                MAKEWPARAM( GetAsyncKeyState( VK_LCONTROL ) != 0 ? MK_CONTROL: 0, -WHEEL_DELTA), 0 );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer(hWnd, 0);
        KillTimer(hWnd, 1);
        UnregisterHotKey(hWnd, 0);
        UnregisterHotKey(hWnd, 1);
        if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
        if (g_fullScreenWorkaround) {
            pMagSetFullscreenTransform(1.0f, 0, 0);
            pMagSetInputTransform(FALSE, nullptr, nullptr);
        }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        g_hWndLiveZoomMag = nullptr;
        g_hWndLiveZoom = NULL;
        break;
        
    case WM_NCDESTROY:
        RestoreSystemPointer();
        if (IsWindow(g_hWndMain)) PostMessage(g_hWndMain, WM_USER_RESTORE_SYSTEM_POINTER, 0, 0);
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_SIZE:
        GetClientRect(hWnd, &rc);
        SetWindowPos(g_hWndLiveZoomMag, NULL, 
            rc.left, rc.top, rc.right, rc.bottom, 0 );
        break;

    case WM_USER_GET_ZOOM_LEVEL:
        return reinterpret_cast<LRESULT>(&zoomLevel);

    case WM_USER_GET_SOURCE_RECT:
        return reinterpret_cast<LRESULT>(&lastSourceRect);

    case WM_USER_MAGNIFY_CURSOR:
        {
            if (g_fullScreenWorkaround || !IsWindow(g_hWndLiveZoomMag)) {
                if (pMagShowSystemCursor) pMagShowSystemCursor(wParam != FALSE);
                break;
            }
            auto style = GetWindowLong( g_hWndLiveZoomMag, GWL_STYLE );
            if( wParam == TRUE )
            {
                style |= MS_SHOWMAGNIFIEDCURSOR;
            }
            else
            {
                style &= ~MS_SHOWMAGNIFIEDCURSOR;
            }
            SetWindowLong( g_hWndLiveZoomMag, GWL_STYLE, style );
            InvalidateRect( g_hWndLiveZoomMag, nullptr, TRUE );
            RedrawWindow( hWnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_UPDATENOW );
        }
        break;

    case WM_USER_SET_ZOOM:
        {
            

            if( lParam != NULL ) {

                lastSourceRect = *reinterpret_cast<RECT *>(lParam);
            }
            zoomLevel = DecodeZoomLevel(wParam);
            zoomTelescopeTarget = zoomLevel;
            matrix.v[0][0] = zoomLevel;
            matrix.v[0][2] = (static_cast<float>(-lastSourceRect.left) * zoomLevel);

            matrix.v[1][1] = zoomLevel;
            matrix.v[1][2] = (static_cast<float>(-lastSourceRect.top) * zoomLevel);

            matrix.v[2][2] = 1.0f;

            if( g_fullScreenWorkaround ) {

                pMagSetFullscreenTransform(zoomLevel, lastSourceRect.left, lastSourceRect.top);
                pMagSetInputTransform(TRUE, &lastSourceRect, &monInfo.rcMonitor);
            }
            else {

                pMagSetWindowTransform(g_hWndLiveZoomMag, &matrix);
            }
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;  
}


//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------
//
// InitInstance
//
//----------------------------------------------------------------------------
HWND InitInstance( HINSTANCE hInstance, int nCmdShow ) 
{
    WNDCLASS  wcZoomIt;
    HWND	  hWndMain;

    g_hInstance = hInstance;

    // If magnification, set default hotkey for live zoom
    if( pMagInitialize ) {

        // register live zoom host window
        wcZoomIt.style          = CS_HREDRAW | CS_VREDRAW;
        wcZoomIt.lpfnWndProc    = LiveZoomWndProc;
        wcZoomIt.cbClsExtra     = 0;
        wcZoomIt.cbWndExtra     = 0;
        wcZoomIt.hInstance      = hInstance;
        wcZoomIt.hIcon = LoadIcon(hInstance, L"APPICON"); 
        wcZoomIt.hCursor        = LoadCursor(NULL, IDC_ARROW);
        wcZoomIt.hbrBackground  = NULL;
        wcZoomIt.lpszMenuName   = NULL;
        wcZoomIt.lpszClassName  = L"ZoomItCustomMagnifier";
        RegisterClass(&wcZoomIt);

    } else {

        g_LiveZoomToggleKey = 0;
    }

    wcZoomIt.style = 0;                     
    wcZoomIt.lpfnWndProc	= (WNDPROC)MainWndProc; 
    wcZoomIt.cbClsExtra		= 0;              
    wcZoomIt.cbWndExtra		= 0;              
    wcZoomIt.hInstance		= hInstance;       wcZoomIt.hIcon = LoadIcon(hInstance, L"APPICON");
    wcZoomIt.hCursor		= LoadCursor(nullptr, IDC_ARROW);
    wcZoomIt.hbrBackground	= NULL;
    wcZoomIt.lpszMenuName	= NULL;  
    wcZoomIt.lpszClassName	= L"ZoomItCustomClass";
    if ( ! RegisterClass(&wcZoomIt) )
        return FALSE;

    hWndMain = CreateWindowEx( WS_EX_TOOLWINDOW, L"ZoomItCustomClass",           
                    L"Zoomit Zoom Window", 
                    WS_POPUP,
                    0, 0, 
                    0, 0,
                    NULL,               
                    NULL,               
                    hInstance,          
                    NULL);

    // If window could not be created, return "failure" 
    if (!hWndMain )
        return NULL;

    // Make the window visible; update its client area; and return "success" 
    ShowWindow(hWndMain, SW_HIDE);

    // Add tray icon
    EnableDisableTrayIcon( hWndMain, g_ShowTrayIcon );
    return hWndMain;      

} 

//----------------------------------------------------------------------------
//
// WinMain
//
//----------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ PWSTR lpCmdLine, _In_ int nCmdShow )
{
    MSG					msg{}; 	
    HACCEL				hAccel;

#ifndef _WIN64

    if(!g_StartedByPowerToys)
    {
        // Launch 64-bit version if necessary
        SetAutostartFilePath();
        if( RunningOnWin64()) {

            // Record where we are if we're the 32-bit version
            return Run64bitVersion();
        }
    }
#endif

    if (lpCmdLine && wcscmp(lpCmdLine, L"--quit") == 0) {
        if (HWND running = FindWindow(L"ZoomItCustomClass", nullptr))
            SendMessage(running, WM_COMMAND, IDCANCEL, 0);
        return 0;
    }

    // Single instance per desktop

    native::unique_handle instanceEvent(CreateEvent(nullptr, FALSE, FALSE, L"Local\\ZoomItCustomActive"));
    if (!instanceEvent.get()) return 1;
    const DWORD instanceError = GetLastError();
    if(instanceError == ERROR_ALREADY_EXISTS) {
        if (g_StartedByPowerToys)
        {
            MessageBox(NULL, L"We've detected another instance of ZoomIt is already running.\nCan't start a new ZoomIt instance from PowerToys.",
            APPNAME, MB_ICONERROR | MB_SETFOREGROUND);
            return 1;
        }

        // Tell the other instance to show the options dialog
        g_hWndMain = FindWindow( L"ZoomItCustomClass", NULL );
        if( g_hWndMain != NULL ) {

            PostMessage( g_hWndMain, WM_COMMAND, IDC_OPTIONS, 0 );
            int count = 0;
            while( count++ < 5 ) {

                HWND local_hWndOptions = FindWindow( NULL, L"ZoomIt Custom" );
                if( local_hWndOptions ) {

                    SetForegroundWindow( local_hWndOptions );
                    SetWindowPos( local_hWndOptions, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW ); 
                    break;
                }
                Sleep( 100 );
            }
        }
        return 0;
    }

    g_OsVersion = GetVersion() & 0xFFFF;

    // load accelerators
    hAccel = LoadAccelerators( hInstance, TEXT("ACCELERATORS"));

    if (FAILED(CoInitialize(0)))
    {
        return 0;
    }

    pEnableThemeDialogTexture = (type_pEnableThemeDialogTexture) GetProcAddress( GetModuleHandle( L"uxtheme.dll" ),
                    "EnableThemeDialogTexture" );
    pMonitorFromPoint = (type_MonitorFromPoint) GetProcAddress( LoadLibrarySafe( L"User32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MonitorFromPoint" );
    pGetMonitorInfo = (type_pGetMonitorInfo) GetProcAddress( LoadLibrarySafe( L"User32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "GetMonitorInfoA" );
    pSHAutoComplete = (type_pSHAutoComplete) GetProcAddress( LoadLibrarySafe(L"Shlwapi.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "SHAutoComplete" );
    pSetLayeredWindowAttributes = (type_pSetLayeredWindowAttributes) GetProcAddress( LoadLibrarySafe(L"user32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "SetLayeredWindowAttributes" );
    pMagSetWindowSource = (type_pMagSetWindowSource) GetProcAddress( LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagSetWindowSource" );
    pGetPointerType = (type_pGetPointerType)GetProcAddress(LoadLibrarySafe(L"user32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "GetPointerType" );
    pGetPointerPenInfo = (type_pGetPointerPenInfo)GetProcAddress(LoadLibrarySafe(L"user32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "GetPointerPenInfo" );
    pMagInitialize = (type_pMagInitialize)GetProcAddress(LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagInitialize");
    pMagSetWindowTransform = (type_pMagSetWindowTransform) GetProcAddress( LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagSetWindowTransform" );
    pMagSetFullscreenTransform = (type_pMagSetFullscreenTransform)GetProcAddress(LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagSetFullscreenTransform");
    pMagSetInputTransform = (type_pMagSetInputTransform)GetProcAddress(LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagSetInputTransform");
    pMagShowSystemCursor = (type_pMagShowSystemCursor)GetProcAddress(LoadLibrarySafe(L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "MagShowSystemCursor");
    pMagSetWindowFilterList = (type_pMagSetWindowFilterList)GetProcAddress( LoadLibrarySafe( L"magnification.dll", DLL_LOAD_LOCATION_SYSTEM ),
                    "MagSetWindowFilterList" );
    pSHQueryUserNotificationState = (type_pSHQueryUserNotificationState) GetProcAddress( LoadLibrarySafe(L"shell32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "SHQueryUserNotificationState" );
    pDwmIsCompositionEnabled = (type_pDwmIsCompositionEnabled) GetProcAddress( LoadLibrarySafe(L"dwmapi.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "DwmIsCompositionEnabled" );
    pSetProcessDPIAware = (type_pSetProcessDPIAware) GetProcAddress( LoadLibrarySafe(L"User32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "SetProcessDPIAware");
    pSystemParametersInfoForDpi = (type_pSystemParametersInfoForDpi)GetProcAddress(LoadLibrarySafe(L"User32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "SystemParametersInfoForDpi");
    pGetDpiForWindow = (type_pGetDpiForWindow)GetProcAddress(LoadLibrarySafe(L"User32.dll", DLL_LOAD_LOCATION_SYSTEM),
                    "GetDpiForWindow" );

    // Windows Server 2022 (and including Windows 11) introduced a bug where the cursor disappears
    // in live zoom. Use the full-screen magnifier as a workaround on those versions only. It is
    // currently impractical as a replacement; it requires calling MagSetInputTransform for all
    // input to be transformed. Otherwise, some hit-testing is misdirected. MagSetInputTransform
    // fails without token UI access, which is impractical; it requires copying the executable
    // under either %ProgramFiles% or %SystemRoot%, which requires elevation.
    //
    // TODO: Update the Windows 11 21H2 revision check when the final number is known. Also add a
    //       check for the Windows Server 2022 revision if that bug (https://task.ms/38611091) is
    //       fixed.
    DWORD windowsRevision, windowsBuild = GetWindowsBuild( &windowsRevision );
    if( ( windowsBuild == BUILD_WINDOWS_SERVER_2022 ) ||
        ( ( windowsBuild == BUILD_WINDOWS_11_21H2 ) && ( windowsRevision < 829 ) ) ) {

        if( pMagSetFullscreenTransform && pMagSetInputTransform )
            g_fullScreenWorkaround = TRUE;
    }

#if 1
    // Calling this causes Windows to mess with our query of monitor height and width
    if( pSetProcessDPIAware ) {

        pSetProcessDPIAware();
    }
#endif
    if (g_fullScreenWorkaround) {
        HANDLE rawToken{};
        DWORD uiAccess{}, tokenBytes{};
        native::unique_handle token;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) token.reset(rawToken);
        if (!token.get() || !GetTokenInformation(token.get(), TokenUIAccess, &uiAccess,
                                                sizeof(uiAccess), &tokenBytes) || !uiAccess) {
            g_LiveZoomNeedsWindowsUpdate = true;
            pMagInitialize = nullptr;
        }
    }
    bool magnificationInitialized = pMagInitialize && pMagSetWindowTransform &&
        pMagSetWindowSource && pMagSetWindowFilterList && pMagInitialize();
    if (!magnificationInitialized) pMagInitialize = nullptr;
    /* Perform initializations that apply to a specific instance */
    g_hWndMain = InitInstance(hInstance, nCmdShow);
    if (!g_hWndMain) {
        if (magnificationInitialized) MagUninitialize();
        CoUninitialize();
        return 1;
    }


    /* Acquire and dispatch messages until a WM_QUIT message is received. */
    BOOL messageResult;
    while ((messageResult = GetMessage(&msg, NULL, 0, 0)) > 0)  {
        if( !TranslateAccelerator( g_hWndMain, hAccel, &msg )) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    int retCode = messageResult < 0 ? 1 : static_cast<int>(msg.wParam);

    g_running = FALSE;


    if (IsWindow(g_hWndMain)) DestroyWindow(g_hWndMain);
    if (pMagShowSystemCursor) pMagShowSystemCursor(TRUE);
    if (magnificationInitialized) MagUninitialize();
    CoUninitialize();
    return retCode;
}

