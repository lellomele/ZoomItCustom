# ZoomIt Custom

[Italiano](README.it.md)

ZoomIt Custom is a portable Windows app for screen magnification, drawing, a break timer and screen snips. The Custom edition was created to address longstanding ZoomIt bugs, especially the disappearing mouse pointer after transitions between LiveZoom and drawing, and to improve robustness, responsiveness and memory use.

Video/audio recording, Type and DemoType are excluded to keep the app focused on these everyday tools.

## Get started

Download and extract a [portable package from Releases](https://github.com/lellomele/ZoomItCustom/releases) containing both executables. If a package with the supervisor is not yet available, follow the build instructions below. Keep the two executables in the same folder and close other ZoomIt instances that use the same shortcuts.

| What you want | What to launch |
| --- | --- |
| Use the app with its internal protections | ZoomItCustom.exe |
| Also restart automatically after a crash | ZoomItCustomSupervisor.exe |

The **supervisor** starts ZoomIt Custom for you and stays in the background. Launch it once; there is no need to open the app separately. If the app is already running on its own, exit it first, then launch the supervisor. Exit ZoomIt Custom from its notification-area menu to close both.

After a crash, the supervisor restarts the app and tries to restore the last valid state: mode, magnification, drawing colour and completed annotations. Drawing returns paused; left-click to continue. The unfinished stroke and undo history cannot be restored. If recovery is unavailable, the app returns to the desktop. A second crash within a minute causes a restart on the desktop; a third stops automatic restarts.

When launched directly, the app still has its internal protections, but you must reopen it yourself after a crash.

## Start with Windows

Open the options from the notification-area icon. Under **Avvio automatico (all'accesso a Windows)**, choose:

- **Disattivato**: start manually.
- **Avvia ZoomIt Custom con Windows**: start the app without the supervisor.
- **Avvia ZoomIt Custom con supervisore**: start the supervisor, which opens and supervises the app.

Confirm with **OK**. The choice applies at your next Windows sign-in; it does not change the current session. The options show whether the current session is supervised. The supervisor choice is unavailable if its executable is missing from the app's folder.

If you move the folder, confirm the choice again to update the startup location. If Windows has disabled the app under Startup apps, enable it there too.

## Everyday use

Shortcuts can be changed in the options. LiveDraw always uses the configured Draw shortcut with Shift added; its current combination is shown in the Draw tab. Choose a Draw shortcut without Shift so the two actions remain distinct.

| Default shortcut | Action |
| --- | --- |
| Ctrl+1 | Static Zoom |
| Ctrl+2 | LiveZoom |
| Ctrl+3 | Draw |
| Ctrl+4 | Break timer |
| Ctrl+5 | Copy a snip |
| Ctrl+Shift+3 | LiveDraw (Draw shortcut + Shift) |
| Ctrl+Shift+5 | Save a PNG snip |

Static Zoom freezes the enlarged image. LiveZoom updates it in real time; LiveDraw lets you draw over that live view.

A large translucent zoom factor appears in the lower-right corner of the **primary monitor** for about **1.2 seconds** on entry and after each adjustment. During animations it shows the requested final value. It stays the same size, lets clicks pass through, and disappears before drawing, Snip or copy/save operations. Disable it with **Show zoom factor briefly (Zoom and LiveZoom)** in the **Zoom** tab.

Each mode has its own **Animate zoom in and zoom out** checkbox. In LiveZoom, checking it makes entry, exit and zoom-level changes gradual; leaving it unchecked applies them instantly. The LiveZoom option starts unchecked and is saved independently of the Zoom option.

Choose the initial magnification for both Zoom and LiveZoom in the **Zoom** tab; the current value appears beside the slider. From **1.25x to 4x**, the controls move in **0.25x steps**. In Zoom, use the wheel or Up/Down; in LiveZoom, use Ctrl+Up / Ctrl+Down or Ctrl+mouse wheel when drawing is inactive. Above 4x, the levels are 8x, 16x and 32x. Reducing below 1.25x returns to 1x and keeps LiveZoom active. Further Ctrl+wheel or Ctrl+Down input stays within LiveZoom at that minimum; use the LiveZoom hotkey (Ctrl+2 by default) to exit. Existing initial magnification settings are preserved when upgrading.

In LiveZoom, **Ctrl+Up / Ctrl+Down** adjust magnification. **Ctrl+mouse wheel** does the same when drawing is inactive, without also zooming the underlying app. The wheel shortcut is disabled during Draw/LiveDraw, including paused drawing, Snip and options. The wheel without Ctrl keeps its usual action. High-resolution wheel input accumulates to one step per full notch. **Ctrl+3** starts Draw on a frozen image; **Ctrl+Shift+3** starts LiveDraw without freezing the view. **Esc** ends drawing and returns to LiveZoom. **Ctrl+2** exits LiveZoom only when drawing is inactive.

Mode shortcuts follow these rules, also during zoom animations:

- From the desktop, any mode can start. In static Zoom, Draw, LiveDraw, LiveZoom and timer shortcuts are ignored; the Zoom shortcut exits Zoom.
- In LiveZoom without drawing, Draw, LiveDraw and Snip are available; Zoom and the timer are ignored.
- While Draw or LiveDraw is active or paused, Zoom, LiveZoom, timer and drawing-mode shortcuts make no changes. Use the mouse controls or Esc to end drawing.
- In the timer, only its own mode shortcut is accepted; pressing it again restarts the countdown.
- During Snip selection, saving or options, mode shortcuts are ignored. Ignored shortcuts are never queued for later.

In Draw and LiveDraw, **Ctrl+Z** undoes and **E** clears annotations. The first **right-click** pauses drawing and shows a small ring in the active colour with a dark centre. Moving the mouse adds no strokes. **Left-click** resumes drawing; a second **right-click** exits. **Esc** also ends drawing. LiveDraw does not support highlighting or blur. In the timer, Up/Down and the mouse wheel adjust minutes; Left/Right adjust ten seconds and **Esc** ends the break.

Snip works on the desktop, in static Zoom, in LiveZoom and in Draw, including paused drawing. It preserves annotations, magnification and the drawing state after capture or cancellation. With LiveZoom it temporarily freezes the image, then restores the live view. Snip is ignored during an unfinished stroke, the timer, or LiveDraw over LiveZoom; it remains available for LiveDraw on the desktop. **Ctrl+Shift+5** saves a PNG under the same rules. When saving, choose **Zoomed PNG** for the displayed size or **Actual size PNG** to remove the magnification.

## If something goes wrong

Reports are created **only for crashes or serious errors**, never during normal use. To find them, paste the following path into File Explorer or the Windows Run dialog:

    %LOCALAPPDATA%\ZoomItCustom\Logs

If that folder is not writable, the fallback is **%TEMP%\ZoomItCustom-Logs**. The text report describes the operation and error; a diagnostic dump may accompany it.

## Requirements and limits

- Updated 64-bit Windows 10 or Windows 11. No separate C++ runtime installation is needed. The executables are unsigned.
- Memory use grows with screen resolution. Undo history is limited to control its footprint. Optional supervised recovery can use up to 128 MiB for saved images; annotations cannot be recovered from images larger than 64 MiB each.
- Recovery covers app crashes, not an app that remains running but stops responding, or a Windows restart/shutdown.
- The second-screen timer requires an extended desktop. With duplicated screens it stays on the current monitor. A display change or resume from sleep may end the active mode; activate it again to continue.
- LiveZoom is unavailable on Windows Server 2022 and Windows 11 21H2 builds older than revision 829. Use an updated Windows version.
- Mixed-DPI monitors, Remote Desktop, touch and pen are not guaranteed to work correctly.

## Build from source

Install Visual Studio 2022 or later with C++ tools, Windows SDK and CMake. From the source folder, run:

    .\build.ps1

The two executables are created in **build-Release**.

## Copyright and licence

Based on the [Microsoft PowerToys ZoomIt sources](https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt).

Custom modifications: copyright © 2026 Prof. ing. Raffaele Mele. Original code: Mark Russinovich / Microsoft Corporation. Distributed under the [MIT licence](LICENSE); preserve the licence and copyright notices when redistributing.
