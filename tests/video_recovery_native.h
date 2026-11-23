#pragma once
inline bool failNextRepairTransform{};
BOOL WINAPI RepairTestTransform(HWND window, PMAGTRANSFORM transform) {
    if (failNextRepairTransform) {
        failNextRepairTransform = false;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return MagSetWindowTransform(window, transform);
}
void RunVideoRecoveryRegression(HWND host) {
    unsigned repairs = 0, bursts = 0, failures = 0, cancels = 0, draws = 0;
    const auto current = []() {
        LiveViewport view{};
        require(SendMessage(g_hWndLiveZoom, WM_USER_GET_LIVE_VIEWPORT, 0, reinterpret_cast<LPARAM>(&view)) != 0,
                "Read saved/active viewport");
        return view;
    };
    const auto clean = [&]() {
        zoomit::runtime::Clear();
        SendMessage(g_hWndMain, recovery::ResetMessage, 0, 0);
        pump(30);
        ActivateTestHost(host);
    };
    const auto enter = [&](float factor) {
        clean();
        g_AnimateLiveZoom = FALSE;
        g_ShowZoomIndicator = TRUE;
        SetCursorPos(200, 200);
        SendMessage(g_hWndMain, WM_HOTKEY, LIVE_HOTKEY, MAKELPARAM(MOD_CONTROL, '2'));
        pump(80);
        require(IsWindowVisible(g_hWndLiveZoom), "Enter native LiveZoom");
        auto view = current();
        const LONG width = static_cast<LONG>((view.monitor.right - view.monitor.left) / factor),
                   height = static_cast<LONG>((view.monitor.bottom - view.monitor.top) / factor);
        RECT source{view.monitor.left + (std::min)(LONG(143), view.monitor.right - view.monitor.left - width),
                    view.monitor.top + (std::min)(LONG(97), view.monitor.bottom - view.monitor.top - height), 0, 0};
        source.right = source.left + width;
        source.bottom = source.top + height;
        require(SendMessage(g_hWndLiveZoom, WM_USER_SET_ZOOM, EncodeZoomLevel(factor),
                            reinterpret_cast<LPARAM>(&source)) != 0,
                "Set exact native view");
        pump(25);
        return current();
    };
    const auto waitRepair = [&]() {
        const auto end = GetTickCount64() + 5000;
        while (g_VideoRepairPending && GetTickCount64() < end)
            pump(20);
        require(!g_VideoRepairPending, "Repair completes within bounded deadline");
    };
    const auto verify = [&](const LiveViewport &before) {
        std::cout << "verify zoom=" << before.factor << " pending=" << g_VideoRepairPending
                  << " ready=" << g_MagnificationReady << " host=" << IsWindow(g_hWndLiveZoom)
                  << " child=" << IsWindow(g_hWndLiveZoomMag) << std::endl;
        require(IsWindowVisible(g_hWndLiveZoom) && IsWindow(g_hWndLiveZoomMag), "Repair keeps LiveZoom active");
        const auto after = current();
        require(after.factor == before.factor && EqualRect(&after.source, &before.source),
                "Repair preserves exact zoom and source position");
        MAGTRANSFORM transform{};
        require(MagGetWindowTransform(g_hWndLiveZoomMag, &transform) != FALSE &&
                    transform.v[0][0] == before.factor &&
                    std::abs(transform.v[0][2] + before.source.left * before.factor) < 0.01f,
                "Native magnifier matrix matches restored viewport");
        require(after.generation > before.generation, "Repaired graphics have a fresh generation");
        ++repairs;
    };
    for (float factor : {1.0f, 1.5f, 2.75f, 4.0f}) {
        std::cout << "case=zoom factor=" << factor << std::endl;
        const auto before = enter(factor);
        const HWND foreground = GetForegroundWindow(), old = g_hWndLiveZoomMag;
        SendMessage(
            g_hWndMain, WM_DISPLAYCHANGE, 32,
            MAKELPARAM(before.monitor.right - before.monitor.left, before.monitor.bottom - before.monitor.top));
        require(g_VideoRepairPending && GetForegroundWindow() == foreground, "Repair starts without taking focus");
        SendMessage(g_hWndMain, WM_HOTKEY, ZOOM_HOTKEY, MAKELPARAM(MOD_CONTROL, '1'));
        waitRepair();
        verify(before);
        require(g_hWndLiveZoomMag != old, "A fresh native magnifier replaces the old control");
        if (factor == 1)
            require(g_ZoomIndicator.Visible(), "Persistent 1x indicator survives repair");
    }
    {
        const auto before = enter(1.75f);
        for (int i = 0; i < 16; ++i) {
            SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
            SendMessage(g_hWndLiveZoom, WM_DISPLAYCHANGE, 32, 0);
            pump(5);
        }
        waitRepair();
        verify(before);
        const auto generation = g_LiveSessionGeneration;
        pump(400);
        require(g_LiveSessionGeneration == generation, "Notification burst does not create a repair loop");
        ++bursts;
    }
    {
        const auto before = enter(2);
        SendMessage(g_hWndLiveZoom, zoomit::video::DriverReset, 0, 0);
        require(!g_VideoRepairPending, "A stale driver notification cannot start a repair");
        require(SendMessage(g_hWndLiveZoom, zoomit::video::DriverReset, static_cast<WPARAM>(before.generation), 0),
                "The current subscription routes a driver reset notification to the active session");
        waitRepair();
        verify(before);
        const auto generation = g_LiveSessionGeneration;
        SendMessage(g_hWndLiveZoom, zoomit::video::VideoStep, static_cast<WPARAM>(before.generation), 0);
        pump(40);
        require(g_LiveSessionGeneration == generation && !g_VideoRepairPending,
                "Old queued rebuild cannot affect a recovered session");
    }
    for (bool retry : {false, true}) {
        const auto before = enter(2);
        const auto savedInfo = pGetMonitorInfo;
        pGetMonitorInfo = ScenarioMonitorInfo;
        g_TestChangedMonitorGeometry = true;
        if (retry)
            zoomit::runtime::FailAt(zoomit::runtime::Api::Graphics, 1);
        SendMessage(g_hWndMain, WM_DISPLAYCHANGE, 32, 0);
        waitRepair();
        const auto after = current();
        zoomit::runtime::Clear();
        g_TestChangedMonitorGeometry = false;
        pGetMonitorInfo = savedInfo;
        require(after.factor == 2 && after.monitor.right == before.monitor.right - 64 &&
                    after.source.right - after.source.left == (before.monitor.right - before.monitor.left - 64) / 2,
                "Changed monitor retains zoom with a fitted source");
        require(std::abs((after.source.left + after.source.right) - (before.source.left + before.source.right)) <=
                    1,
                "Resized view retains its center");
        ++repairs;
    }
    for (bool layered : {false, true})
        for (bool suspended : {false, true}) {
            const auto before = enter(1.5f);
            SendMessage(g_hWndMain, WM_HOTKEY, layered ? LIVE_DRAW_HOTKEY : DRAW_HOTKEY,
                        MAKELPARAM(layered ? MOD_CONTROL | MOD_SHIFT : MOD_CONTROL, '3'));
            pump(50);
            require(IsWindowVisible(g_hWndMain) &&
                        ((GetWindowLongPtr(g_hWndMain, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) == layered,
                    "Draw/LiveDraw canvas before reset");
            HDC canvas = reinterpret_cast<HDC>(SendMessage(g_hWndMain, WM_TEST_QUERY_CANVAS, 0, 0));
            require(canvas != nullptr, "LiveDraw canvas");
            SetPixel(canvas, 32, 48, RGB(29, 97, 193));
            SendMessage(g_hWndMain, WM_TEST_MARK_CANVAS_DIRTY, 0, 0);
            if (suspended) {
                SendMessage(g_hWndMain, WM_RBUTTONDOWN, 0, MAKELPARAM(100, 100));
                SendMessage(g_hWndMain, WM_RBUTTONUP, 0, MAKELPARAM(100, 100));
                pump(20);
            }
            const auto mode = SendMessage(g_hWndMain, WM_TEST_QUERY_MODE, 0, 0);
            require(((mode & 16) != 0) == suspended && ((mode & 2) != 0) != suspended,
                    "Canvas has the requested drawing or suspended state");
            SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
            waitRepair();
            if (layered)
                verify(before);
            else
                require(!IsWindow(g_hWndLiveZoom),
                        "Frozen Draw has already released Magnification; a video event must not reopen it");
            require(SendMessage(g_hWndMain, WM_TEST_QUERY_MODE, 0, 0) == mode &&
                        GetPixel(canvas, 32, 48) == RGB(29, 97, 193),
                    "Draw/LiveDraw annotations and suspension state survive video notification");
            if (!layered) {
                SendMessage(g_hWndMain, WM_KEYDOWN, VK_ESCAPE, 0);
                pump(100);
                verify(before);
            }
            ++draws;
        }
    for (bool escape : {false, true}) {
        enter(2);
        NativeLiveCursorRemovalProbe removal(g_hWndLiveZoom, g_hWndLiveZoomMag);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        require(g_VideoRepairPending && IsWindow(g_hWndLiveZoomMag), "Exercise cancellation before queued detach");
        if (escape)
            SendMessage(g_hWndLiveZoom, WM_KEYDOWN, VK_ESCAPE, 0);
        else
            SendMessage(g_hWndMain, WM_HOTKEY, LIVE_HOTKEY, MAKELPARAM(MOD_CONTROL, '2'));
        pump(700);
        require(!IsWindow(g_hWndLiveZoom) && !g_VideoRepairPending,
                "Explicit exit cancels pending repair without reopening");
        require(removal.ReleasedBeforeRemoval(),
                "Cancelling before detach must release the actual child's cursor before native parent removal");
        CURSORINFO cursor{sizeof(cursor)};
        const BOOL read = GetCursorInfo(&cursor);
        if (!read || !cursor.hCursor || !(cursor.flags & CURSOR_SHOWING))
            std::cerr << "Cancelled repair cursor: exit=" << (escape ? "Esc" : "toggle")
                      << " read=" << read << " flags=" << cursor.flags << " handle=" << cursor.hCursor
                      << " mag_show_requested=" << systemCursorShown << '\n';
        require(read && cursor.hCursor && (cursor.flags & CURSOR_SHOWING),
                "Pointer remains logically visible after cancelling a repair");
        ++cancels;
    }
    {
        const auto before = enter(2);
        SendMessage(g_hWndMain, WM_HOTKEY, LIVE_DRAW_HOTKEY, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, '3'));
        pump(50);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        pump(60);
        require(g_VideoRepairPending && !IsWindow(g_hWndLiveZoomMag),
                "Magnifier is detached during the quiet period");
        SendMessage(g_hWndMain, WM_KEYDOWN, VK_ESCAPE, 0);
        waitRepair();
        verify(before);
        DWORD filterMode{};
        HWND excluded[8]{};
        const int count = MagGetWindowFilterList(g_hWndLiveZoomMag, &filterMode, 8, excluded);
        require(count > 0 && count <= 8 && filterMode == MW_FILTERMODE_EXCLUDE &&
                    std::find(excluded, excluded + count, g_hWndMain) != excluded + count,
                "Leaving LiveDraw during a repair restores the main window exclusion");
    }
    {
        const auto before = enter(2);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        zoomit::runtime::FailAt(zoomit::runtime::Api::Graphics, 1);
        waitRepair();
        verify(before);
        zoomit::runtime::Clear();
    }
    {
        const auto before = enter(2);
        const auto original = pMagSetWindowTransform;
        pMagSetWindowTransform = RepairTestTransform;
        failNextRepairTransform = true;
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        waitRepair();
        pMagSetWindowTransform = original;
        verify(before);
    }
    for (auto failure : {zoomit::runtime::Api::Graphics, zoomit::runtime::Api::Timer}) {
        enter(2);
        zoomit::runtime::FailAlways(failure);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        waitRepair();
        zoomit::runtime::Clear();
        require(!IsWindow(g_hWndLiveZoom) && !IsWindowVisible(g_hWndMain),
                "Exhausted or unarmable repair fails closed on desktop");
        ++failures;
    }
    {
        const auto before = enter(1.5f);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        waitRepair();
        verify(before);
    }
    clean();
    // Warm graphics resources before checking repeated-cycle ownership.
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
                user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 6; ++i) {
        const auto before = enter(1.5f);
        SendMessage(g_hWndMain, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        waitRepair();
        verify(before);
        clean();
    }
    require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi + 2 &&
                GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user + 2,
            "Repeated repairs do not accumulate GDI/USER objects");
    std::cout << "{\"video_recovery_passed\":true,\"native_repairs\":" << repairs
              << ",\"notification_bursts\":" << bursts << ",\"preserved_drawing_states\":" << draws
              << ",\"bounded_failures\":" << failures << ",\"cancel_cases\":" << cancels
              << ",\"physical_gpu_reset_forced\":false}\n";
}
