# Pip-Net 0.1.0 — Pip-Boy web-browser proof of concept

**Status: SOURCE-ONLY, NOT BUILT / NOT TESTED IN FALLOUTNV.** This is an implementation attempt, not a verified working mod. Windows/game validation is still required. The initial display is a 2D Win32 overlay above FalloutNV, **not a rendered surface inside the 3D Pip-Boy model**.

## Intended behavior
- Launch FNV (runtime `1.4.0.525`) with modern xNVSE.
- Open Pip-Boy using the usual Tab key; while it is open, press **F9**.
- A chromium-based window opens over the right-hand side of the game client. It has Back/Forward/Reload, an address bar and Go button, links and mouse input.
- **F9** or **X** closes the browser; closing Pip-Boy or returning to the main menu should also hide it.
- The browser preserves its session in `%LOCALAPPDATA%\PipNet\WebView2`.
- No ESP, UIO, SUP NVSE, external network server, injected JavaScript, plugin opcodes or engine hooks required in this first proof of concept.

## Important distinction
SUP NVSE implements specific online functions (checking versions, APIs etc.) but does **not** render arbitrary HTML/CSS/JavaScript web pages. For that, this PoC uses Microsoft's **WebView2** (Edge/Chromium). Using UIO to integrate Pip-Net as an actual Pip-Boy tab remains a future phase.

## Requirements
1. Windows 10/11, FalloutNV.exe **32-bit**, runtime `1.4.0.525`, latest supported xNVSE.
2. Visual Studio 2022 or newer with C++ desktop build tools; CMake; Git; PowerShell.
3. The [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) installed on the gaming PC.
4. Run FNV in **windowed/borderless** mode. Exclusive fullscreen can prevent Win32 overlay windows from appearing.
5. The [xNVSE source tree](https://github.com/xNVSE/NVSE), required only for compilation.
6. WebView2 SDK from NuGet (the PowerShell build script downloads this automatically).

## Build (PowerShell, Windows, with network access)
```powershell
cd PipNet-MVP
mkdir external -ErrorAction SilentlyContinue
git clone https://github.com/xNVSE/NVSE.git external/NVSE
powershell -ExecutionPolicy Bypass -File .\scripts\build-win32.ps1
```

The build uses `cmake -A Win32`, not x64. It intentionally includes no hardcoded plugin opcodes, avoiding conflicts with other NVSE plugins.

## Install
Copy these two files into the **game's actual / virtual** `Data\NVSE\Plugins` folder, preferably using MO2:
```
Data/NVSE/Plugins/PipNet.dll              <- build/Release/PipNet.dll
Data/NVSE/Plugins/PipNet.ini              <- config/PipNet.ini
```

The runtime stores its user profile under `%LOCALAPPDATA%\PipNet\WebView2`, not beside your mod installation.

## First test checklist
1. Make a backup of your save. Use a test game instance, preferably with other overlays disabled.
2. Start FNV in windowed/borderless mode. Confirm xNVSE is loaded.
3. Open Pip-Boy and press F9 once. **Expected:** a web panel appears with navigation controls.
4. Enter `https://example.com/`, press Enter or Go. Test a link and Back/Forward.
5. Press F9, then reopen. Check that the game still responds to keyboard/mouse.
6. Close Pip-Boy, verify that the panel disappears; return to the menu; reload a save.
7. Repeat with UI scale / screen resolutions such as 1920×1080 and 3440×1440.
8. Check `PipNet` messages with DebugView or a debugger. Chromium/WebView2 initialization can fail if COM, graphics hooking or multi-process policies conflict.

## Known technical limitations / risks
- **Not compiled or tested here.** Local environment is Linux and has neither the game's process nor Win32/VS/WebView2 SDK, so no DLL or live interaction was validated.
- The WebView2 controller runs on a dedicated STA thread with a top-level owned Win32 popup. Behavior when the underlying game loses focus must be tested. FNV may pause, lose device or minimize depending on window mode and other mods.
- Detection uses the native game's menu visibility flags (`0x011F308F`, menu IDs `1002`, `1003`, `1023`), which are valid **only** for the supported runtime. The plugin rejects incompatible runtimes, and this address should be verified by a runtime test.
- If the address bar receives focus, the F9 hotkey behavior may vary by OS message dispatch. The code handles WebView2 accelerator events and standard Win32 key messages.
- Some sites may require logins, modern WebView2 security settings and popup management. File URLs and non-HTTP schemes are deliberately blocked; permission requests are denied.
- No browser texture mapping, no native Pip-Boy data tab, no 3D perspective transformation, no touch/controller support, no sandbox beyond WebView2's standard browser process model.
- Chromium inside the 32-bit game can increase memory pressure. A future more robust version should move WebView2 to a separate 64-bit helper process and use shared memory / IPC plus texture capture for actual Pip-Boy surface integration.
- Existing mods injecting overlays, DXVK, third-party borderless/fullscreen tools may interact with window ownership/focus.

## Next milestones if this smoke test succeeds
1. Native Pip-Boy tab via UIO, customizable keybinds, theme consistent with Pip-Boy, mouse focus ownership.
2. External helper browser process (x64), 32-bit NVSE broker, browser crash isolation.
3. Offscreen surface rendering into game's Pip-Boy 3D screen texture (D3D9), transform UV/projection and route mouse events.
4. Bookmarks, history, offline fallback, text-only mode backed by SUP NVSE requests when needed.

## License and attribution

Pip-Net original source is licensed under the **MIT License** (copyright (c) 2026 Infest-SBT). See [LICENSE](LICENSE).

This archive contains only newly written prototype source and config. It does not bundle xNVSE sources, SUP NVSE binaries, Microsoft's SDK or WebView2 Runtime. Their respective original licenses apply.

## Optional GitHub Actions compile check

The repository includes `.github/workflows/build.yml` for an **automated Windows x86 build** and downloadable DLL artifact. It has not been run by the author of this prototype and may expose integration/SDK errors to fix before gameplay testing.