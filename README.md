[Italiano](README.it.md) | [English](README.md)

# ZoomIt Custom

Portable screen magnification and annotation for Windows presentations. This edition focuses on reliable cursor behaviour, quick operation and a small memory footprint.

## Getting started

Extract the [portable ZIP](https://github.com/lellomele/ZoomItCustom/releases/latest) and keep both executables together. Launch **ZoomItCustom.exe** for everyday use. For automatic restart after an app crash, launch **ZoomItCustomSupervisor.exe** instead. It starts the app and tries to recover the last completed drawing in paused mode. An unfinished stroke and undo history are lost. Use the tray menu to close both or configure startup at sign-in.

## Default shortcuts

| Shortcut | Action |
| --- | --- |
| Ctrl+1 | Freeze and magnify the screen |
| Ctrl+2 | LiveZoom. Adjust with Ctrl+wheel. Esc exits, including at 1× |
| Ctrl+3 / Ctrl+Shift+3 | Draw / draw over the live desktop |
| Ctrl+4 | Break timer |
| Ctrl+5 / Ctrl+Shift+5 | Copy a selection / save it as PNG |
| Ctrl+6 | Open or close the whiteboard. C switches background and Ctrl+wheel adjusts the grid |

Change shortcuts in the tray options. In Draw, right-click to pause, left-click to resume or right-click again to exit. On the whiteboard, Esc ends Draw and clears its annotations.

## Recovery and requirements

After a graphics reset, LiveZoom tries to restore the zoom and position. A change in screen geometry can end a drawing. Only serious errors and crashes generate reports in `%LOCALAPPDATA%\ZoomItCustom\Logs` (fallback `%TEMP%\ZoomItCustom-Logs`).

Use updated 64-bit Windows 10 or 11. The executables are unsigned. A separate timer on the second screen requires an extended desktop. Supervised recovery cannot restore a drawing image larger than 64 MiB or restart a frozen process.

## Build and licence

Run `.\build.ps1` with Visual Studio 2022 or later, C++ tools, Windows SDK and CMake. Both executables are written to `build-Release`.

Based on [Microsoft PowerToys ZoomIt](https://github.com/microsoft/PowerToys/tree/21fd5092b3e062ca6c8dd6b8c772a236f90b3b42/src/modules/ZoomIt/ZoomIt). Original code © Mark Russinovich / Microsoft Corporation. Custom modifications © 2026 Prof. ing. Raffaele Mele. [MIT licence](LICENSE).
