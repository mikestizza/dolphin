// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace QtUtils
{
// Couch overlay: applies controller presses (a CouchOverlay::DialogNav mask) to the active modal
// dialog, if there is one. D-pad moves focus between buttons, A presses the focused button and
// B cancels. Does nothing when no modal dialog is open. GUI thread only.
void DriveModalDialogWithController(unsigned nav);
}  // namespace QtUtils
