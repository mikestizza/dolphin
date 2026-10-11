// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QtUtils/CouchDialogNav.h"

#include <QAbstractButton>
#include <QApplication>
#include <QKeyEvent>
#include <QWidget>

#include "VideoCommon/CouchOverlay.h"

namespace QtUtils
{
namespace
{
void SendKey(QWidget* target, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
  QKeyEvent press(QEvent::KeyPress, key, mods);
  QApplication::sendEvent(target, &press);
  QKeyEvent release(QEvent::KeyRelease, key, mods);
  QApplication::sendEvent(target, &release);
}
}  // namespace

void DriveModalDialogWithController(unsigned nav)
{
  // Only acts while a modal dialog (alert, question, confirmation) is up, so pad presses during
  // normal play are ignored.
  QWidget* const modal = QApplication::activeModalWidget();
  if (!modal)
    return;

  // Bring the dialog forward; in fullscreen it can open behind the render window.
  modal->raise();
  modal->activateWindow();

  QWidget* focus = QApplication::focusWidget();
  if (!focus || (focus != modal && !modal->isAncestorOf(focus)))
    focus = modal;

  if (nav & CouchOverlay::DialogNav_Prev)
    SendKey(focus, Qt::Key_Backtab, Qt::ShiftModifier);
  if (nav & CouchOverlay::DialogNav_Next)
    SendKey(focus, Qt::Key_Tab);

  if (nav & CouchOverlay::DialogNav_Accept)
  {
    auto* const button = qobject_cast<QAbstractButton*>(QApplication::focusWidget());
    if (button && modal->isAncestorOf(button))
      button->click();
    else
      SendKey(focus, Qt::Key_Return);
  }
  else if (nav & CouchOverlay::DialogNav_Back)
  {
    // Escape picks the "No"/"Cancel" button on message boxes and rejects other dialogs.
    SendKey(focus, Qt::Key_Escape);
  }
}
}  // namespace QtUtils
