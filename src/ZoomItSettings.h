#pragma once
#include "zoomit.h"
#include "Registry.h"

DWORD	g_ToggleKey = (HOTKEYF_CONTROL << 8)| '1';
DWORD	g_LiveZoomToggleKey = ((HOTKEYF_CONTROL) << 8)| '2';
DWORD	g_DrawToggleKey = ((HOTKEYF_CONTROL) << 8)| '3';
DWORD	g_BreakToggleKey = ((HOTKEYF_CONTROL) << 8)| '4';
DWORD   g_SnipToggleKey = ((HOTKEYF_CONTROL) << 8) | '5';

DWORD	g_ShowExpiredTime = 1;
DWORD	g_SliderZoomLevel = 3;
BOOLEAN g_AnimateZoom = TRUE;
DWORD	g_PenColor = COLOR_RED;
DWORD	g_BreakPenColor = COLOR_RED;
DWORD   g_RootPenWidth = PEN_WIDTH;
DWORD	g_BreakTimeout = 10;
DWORD	g_BreakOpacity = 100;
DWORD	g_BreakTimerPosition = 4;
BOOLEAN	g_BreakPlaySoundFile = FALSE;
TCHAR	g_BreakSoundFile[MAX_PATH] = {0};
BOOLEAN g_BreakShowDesktop = TRUE;
BOOLEAN	g_BreakShowBackgroundFile = FALSE;
BOOLEAN	g_BreakBackgroundStretch = FALSE;
TCHAR	g_BreakBackgroundFile[MAX_PATH] = {0};
BOOLEAN	g_OptionsShown = FALSE;
BOOLEAN	g_ShowTrayIcon = TRUE;
BOOLEAN g_SnapToGrid = TRUE;
BOOLEAN	g_TelescopeZoomOut = TRUE;
BOOLEAN	g_BreakOnSecondary = FALSE;
LOGFONT	g_LogFont;
// Divide by 100 to get actual scaling

DWORD g_SettingsSchema=0;

REG_SETTING RegSettings[] = {
    {L"SettingsSchema",SETTING_TYPE_DWORD,0,&g_SettingsSchema,0},
    { L"ToggleKey", SETTING_TYPE_DWORD, 0, &g_ToggleKey, static_cast<DOUBLE>(g_ToggleKey) },
    { L"LiveZoomToggleKey", SETTING_TYPE_DWORD, 0, &g_LiveZoomToggleKey, static_cast<DOUBLE>(g_LiveZoomToggleKey) },
    { L"DrawToggleKey", SETTING_TYPE_DWORD, 0, &g_DrawToggleKey, static_cast<DOUBLE>(g_DrawToggleKey) },
    { L"SnipToggleKey", SETTING_TYPE_DWORD, 0, &g_SnipToggleKey, static_cast<DOUBLE>(g_SnipToggleKey) },
    { L"PenColor", SETTING_TYPE_DWORD, 0, &g_PenColor, static_cast<DOUBLE>(g_PenColor) },
    { L"PenWidth", SETTING_TYPE_DWORD, 0, &g_RootPenWidth, static_cast<DOUBLE>(g_RootPenWidth) },
    { L"OptionsShown", SETTING_TYPE_BOOLEAN, 0, &g_OptionsShown, static_cast<DOUBLE>(g_OptionsShown) },
    { L"BreakPenColor", SETTING_TYPE_DWORD, 0, &g_BreakPenColor, static_cast<DOUBLE>(g_BreakPenColor) },
    { L"BreakTimerKey", SETTING_TYPE_DWORD, 0, &g_BreakToggleKey, static_cast<DOUBLE>(g_BreakToggleKey) },
    { L"BreakTimeout", SETTING_TYPE_DWORD, 0, &g_BreakTimeout, static_cast<DOUBLE>(g_BreakTimeout) },
    { L"BreakOpacity", SETTING_TYPE_DWORD, 0, &g_BreakOpacity, static_cast<DOUBLE>(g_BreakOpacity) },
    { L"BreakPlaySoundFile", SETTING_TYPE_BOOLEAN, 0, &g_BreakPlaySoundFile, static_cast<DOUBLE>(0) },
    { L"BreakSoundFile", SETTING_TYPE_STRING, sizeof(g_BreakSoundFile), g_BreakSoundFile, static_cast<DOUBLE>(0) },
    { L"BreakShowBackgroundFile", SETTING_TYPE_BOOLEAN, 0, &g_BreakShowBackgroundFile, static_cast<DOUBLE>(g_BreakShowBackgroundFile) },
    { L"BreakBackgroundStretch", SETTING_TYPE_BOOLEAN, 0, &g_BreakBackgroundStretch,static_cast<DOUBLE>(g_BreakBackgroundStretch) },
    { L"BreakBackgroundFile", SETTING_TYPE_STRING, sizeof(g_BreakBackgroundFile), g_BreakBackgroundFile, static_cast<DOUBLE>(0) },
    { L"BreakTimerPosition", SETTING_TYPE_DWORD, 0, &g_BreakTimerPosition, static_cast<DOUBLE>(g_BreakTimerPosition) },
    { L"BreakShowDesktop", SETTING_TYPE_BOOLEAN, 0, &g_BreakShowDesktop, static_cast<DOUBLE>(g_BreakShowDesktop) },
    { L"BreakOnSecondary", SETTING_TYPE_BOOLEAN, 0, &g_BreakOnSecondary,static_cast<DOUBLE>(g_BreakOnSecondary) },
    { L"ShowExpiredTime", SETTING_TYPE_BOOLEAN, 0, &g_ShowExpiredTime, static_cast<DOUBLE>(g_ShowExpiredTime) },
    { L"ShowTrayIcon", SETTING_TYPE_BOOLEAN, 0, &g_ShowTrayIcon, static_cast<DOUBLE>(g_ShowTrayIcon) },
    { L"AnimnateZoom", SETTING_TYPE_BOOLEAN, 0, &g_AnimateZoom, static_cast<DOUBLE>(g_AnimateZoom) },
    { L"TelescopeZoomOut", SETTING_TYPE_BOOLEAN, 0, &g_TelescopeZoomOut, static_cast<DOUBLE>(g_TelescopeZoomOut) },
    { L"SnapToGrid", SETTING_TYPE_BOOLEAN, 0, &g_SnapToGrid, static_cast<DOUBLE>(g_SnapToGrid) },
    { L"ZoominSliderLevel", SETTING_TYPE_DWORD, 0, &g_SliderZoomLevel, static_cast<DOUBLE>(g_SliderZoomLevel) },
    { L"Font", SETTING_TYPE_BINARY, sizeof g_LogFont, &g_LogFont, static_cast<DOUBLE>(0) },
    { NULL, SETTING_TYPE_DWORD, 0, NULL, static_cast<DOUBLE>(0) }
};
