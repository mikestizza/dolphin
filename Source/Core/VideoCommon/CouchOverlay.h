// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Couch overlay (Couch Potato Edition): a controller-driven in-game menu (pause, states, disc,
// settings, exit) drawn with the on-screen ImGui layer, modelled on xemu's quick menu.
//
// Threads:
//  - Host (hotkey poll loop): Toggle(), PollHost(), TakeHostAction().
//  - Video (inside the ImGui frame, from OnScreenUI::Finalize): Draw().
// Everything else is internal and guarded by the overlay's own mutex.

#pragma once

struct ImFont;

namespace Core
{
class System;
}

namespace CouchOverlay
{
// Actions that need the Qt main window. The host drains these and forwards them to the same
// slots the regular hotkeys use, so behaviour (netplay, movies, OSD messages) stays identical.
enum class HostAction
{
  Exit,        // stop emulation without the "are you sure" dialog (the menu already confirmed)
  Reset,       // console reset button
  Screenshot,  // Core screenshot of the next game frame
  FullScreen,  // toggle fullscreen
  StateSave,   // arg = slot (1-based)
  StateLoad,   // arg = slot (1-based)
  StateUndoLoad,
};

struct PendingHostAction
{
  HostAction action;
  int arg = 0;
};

// Host thread.
void Toggle(Core::System& system);
bool IsOpen();
void PollHost(Core::System& system);
bool TakeHostAction(PendingHostAction* out);

// Host thread: edge-triggered pad presses for driving desktop dialogs (message boxes, prompts)
// with the controller. Returns a mask of DialogNav bits.
enum DialogNav : unsigned
{
  DialogNav_Accept = 1u << 0,
  DialogNav_Back = 1u << 1,
  DialogNav_Prev = 1u << 2,
  DialogNav_Next = 1u << 3,
};
unsigned PollDialogNav();

// Video thread, with the ImGui lock held, between NewFrame and Render.
void Draw();

// Video thread, right after the ImGui context and its default font are created. Adds the menu's
// UI font: <User>/Load/CouchOverlay_Font.ttf if present, else Segoe UI on Windows, else none
// (the OSD font is used).
void LoadFonts();
// Uses the given font for the menu (nullptr = the default ImGui font).
void UseFont(ImFont* font);

// UI thread, at startup: writes the Couch Potato baseline settings into the global config the
// first time this build runs with it (or when the baseline is revised), then saves.
void ApplyBaselineIfNeeded();
}  // namespace CouchOverlay
