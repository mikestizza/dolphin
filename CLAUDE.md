# Dolphin couch overlay fork (mikestizza/dolphin, branch `couch-overlay`)

This is a fork of the Dolphin GameCube/Wii emulator that adds a controller-driven in-game menu
("couch overlay"), modelled on xemu's quick menu. The owner plays from a couch with an Xbox 360
controller, launching games from ES-DE (portable, `C:\Games\ES-DE`, GameCube games under
`ROMs_ALL\gc`) into standalone Dolphin. Goal: never need a keyboard or mouse while a game runs.

Ground rule: **do not change emulation behaviour.** Core changes must stay small, additive, and
marked with a `// Couch overlay:` comment. Nothing new may run unless the menu is open or a desktop
dialog is up.

## Repo and remotes

- Fork: https://github.com/mikestizza/dolphin, work branch `couch-overlay`.
- `git remote add upstream https://github.com/dolphin-emu/dolphin` (base is upstream `master`).
- Every push to `couch-overlay` runs `.github/workflows/couch-build.yml` (Windows x64, MSVC +
  Ninja, ~25 min) and replaces the `couch-latest` prerelease with `Dolphin-couch-x64.zip`.
  Put `[skip ci]` in a commit message for docs-only pushes.

## One-time setup on Windows

Prerequisites: Git, Visual Studio 2022 (17.4+) or 2026 with the **Desktop development with C++**
workload, including **C++ CMake tools for Windows** and a Windows 11 SDK. CMake 3.25+ and Ninja
ship with that workload.

```powershell
git clone --recursive https://github.com/mikestizza/dolphin C:\Games\Dolphin-src
cd C:\Games\Dolphin-src
git checkout couch-overlay
git remote add upstream https://github.com/dolphin-emu/dolphin
git submodule update --init --recursive
```

The `--recursive` submodules are required (Qt for Windows, FFmpeg, SDL, etc. are submodules).

## Build

From **Developer PowerShell for VS** (so MSVC is on PATH), in the repo root:

```powershell
cmake --preset ninja-release-x64
cmake --build --preset ninja-build-release-x64 --target dolphin-emu
```

Output: `build\release\x64\Binaries\Dolphin.exe`. Incremental rebuilds after editing overlay files
take a minute or two. For debugging use `ninja-debug-x64` / `ninja-build-debug-x64`
(output `build\debug\x64\Binaries`).

Visual Studio IDE: File > Open > Folder on the repo root. VS reads `CMakePresets.json`; pick
"Configure: Release - Ninja - x64" (or the Debug one) and set `Dolphin.exe` as the startup item.
The `visualstudio-*` presets target the "Visual Studio 18 2026" generator and toolset v145, so only
use them with VS 2026; the Ninja presets work with VS 2022 or 2026.

## Running a test build without touching the real setup

Run the dev build against a **copy** of the owner's Dolphin user folder so testing can't damage
his saves or config:

```powershell
# one-time: copy the real user folder (path depends on his install; portable installs keep
# a "User" folder next to Dolphin.exe, otherwise it is Documents\Dolphin Emulator)
robocopy "<real user folder>" C:\Games\Dolphin-src\testuser /E
build\release\x64\Binaries\Dolphin.exe -u C:\Games\Dolphin-src\testuser -b -e "<path to a game>"
```

`-b -e` matches how ES-DE launches games (batch mode, so Exit returns to the frontend).
Logs: enable in View > Show Log / Log Configuration, or read `testuser\Logs\dolphin.log`.

## What the overlay does

- Open/close: hotkey "Toggle Couch Menu" (Hotkeys > General). Default binding is the Xbox guide
  button `` `XInput/0/Gamepad:Guide` `` or Ctrl+M; it is auto-filled for Hotkeys.ini files that
  predate it. Start or B on the top page also closes it.
- Opening pauses emulation. Closing resumes only after the closing button is released, so the
  press never reaches the game.
- Pages: Main (Resume, Screenshot, Save State, Load State, Change Disc, Settings, Restart, Exit),
  Save/Load State (10 slots, overwrite confirm, Undo Load State), Change Disc (m3u set, or sibling
  files whose names differ only by a "(Disc N)" tag), Settings (aspect ratio, widescreen hack,
  internal resolution, post-processing shader, V-Sync, Show FPS, volume, speed limit, menu theme,
  fullscreen).
- Themes: Blue (default), Dark, Purple, Green, Red, Gold, Light. Stored in Dolphin.ini
  `[CouchOverlay] Theme`.
- Font: `<User>\Load\CouchOverlay_Font.ttf` (or .otf) if present, else Segoe UI Semibold from
  `%WINDIR%\Fonts`, else Dolphin's OSD font.
- Desktop dialogs (panic alerts, yes/no questions, the stop confirmation) can be answered with the
  pad: D-pad moves focus, A presses, B cancels. Exit from the menu skips Dolphin's own "stop
  emulation?" box because the menu already asked.

## Code map

| File | Role |
|---|---|
| `Source/Core/VideoCommon/CouchOverlay.{h,cpp}` | All menu state, input, drawing, themes, actions |
| `Source/Core/VideoCommon/OnScreenUI.cpp` | Calls `CouchOverlay::Draw()` in `Finalize()`, `LoadFonts()` in `Initialize()` |
| `Source/Core/DolphinQt/HotkeyScheduler.{h,cpp}` | Toggle hotkey, `PollHost()`, drains `HostAction`s into existing signals, emits `CouchDialogNav` |
| `Source/Core/DolphinQt/MainWindow.{h,cpp}` | `RequestStopFromCouchOverlay()` (stop without confirm), `OnCouchDialogNav()` |
| `Source/Core/DolphinQt/QtUtils/CouchDialogNav.{h,cpp}` | Drives the active modal dialog with pad presses (kept out of MainWindow.cpp because X11 headers there define `KeyPress`) |
| `Source/Core/Core/HW/CPU.{h,cpp}` | `CPUManager::QueueJobIfPaused()`: run a job on the idle paused CPU thread without pausing |
| `Source/Core/Core/HW/DVD/DVDInterface.{h,cpp}`, `Core/Boot/Boot.cpp` | Remember the current disc path for Change Disc |
| `Source/Core/Core/HotkeyManager.{h,cpp}` | `HK_TOGGLE_COUCH_MENU` and its default binding |

## Threading rules (important)

- **Host thread** (HotkeyScheduler loop, every 5 ms): `Toggle`, `PollHost`, `PollDialogNav`,
  `TakeHostAction`. Reads the pad: raw `XInput/0/Gamepad` if present (so A/B match the controller),
  else GameCube port 1 via `Pad::GetStatus(0)`. Also does file I/O for slot labels and disc lists,
  because `State::GetInfoStringOfSlot` takes the save-state lock and must never be waited on from
  the video thread.
- **Video thread**: `Draw()` runs inside the ImGui frame with the ImGui lock held. It must not
  block or do I/O. Anything that changes emulator state goes out via `Core::QueueHostJob` (UI
  thread) or a `HostAction` (Qt signal through HotkeyScheduler).
- All shared overlay state is behind one mutex (`s_mutex`); `s_open` is atomic.
- **Redraw while paused**: Dolphin only presents when the game produces a frame. While paused, the
  host asks `CPUManager::QueueJobIfPaused` to run a job on the idle CPU thread, which pushes
  `g_presenter->Present()` through `AsyncRequests` (GPU thread in dual core, inline in single core).
  Throttled to ~60 Hz with a 250 ms stale-request guard. Not wired for deterministic GPU-thread
  mode.
- Mouse clicks in `Draw()` are deferred until after drawing so the page stack never changes
  mid-frame.

## Conventions

- Format with the repo's `Source/.clang-format` (100 columns). Match Dolphin style.
- New code in the overlay files where possible; core edits minimal and commented.
- Keep the CI build green. Commit messages: short imperative subject, wrapped body.
- Periodically rebase on upstream: `git fetch upstream && git rebase upstream/master`, then rebuild.

## Status and next steps

Done: everything above compiles on MSVC (CI) and GCC; menu visuals verified offline with a
software-rendered harness. **Not yet tested at runtime with real games.** Priority test checklist:

1. Guide opens the menu in windowed and fullscreen; game audio and gameplay pause; menu animates
   while paused (selection moves smoothly). Check both Dual Core on and off (Config > General).
2. B / Start / guide close it and the game resumes without the closing press leaking into the game.
3. Save to a slot, overwrite confirm, Load (closes and resumes), Undo Load State.
4. Settings change live behind the menu (aspect, internal resolution, shader); values persist after
   restarting Dolphin; Volume takes effect immediately.
5. Change Disc on a two-disc game (e.g. a "(Disc 1)/(Disc 2)" pair or an .m3u).
6. Restart and Exit (Exit with `-b` should return to ES-DE without any dialog).
7. Trigger a dialog (e.g. Stop from the menu bar with Confirm on Stop enabled) and answer it with the
   pad.
8. Try each theme, including Light, at 1080p and 4K.

Known gaps / ideas:
- Windows "Open Xbox Game Bar with the controller button" can also fire on guide; turn it off in
  Windows Settings > Gaming > Xbox Game Bar if it pops up. The owner's AutoHotkey script kills
  emulators on a guide double press; a single press opens this menu, so they coexist.
- Wii games driven only by a Wii Remote fall back to GameCube port 1 for navigation.
- Exclusive fullscreen (D3D) is untested.
- Possible extras: save-state thumbnails, per-game settings, controller profile switching, a
  "Pause when menu opens" option.
