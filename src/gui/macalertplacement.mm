// SPDX-FileCopyrightText: 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/macalertplacement.h"

#include <QWidget>

#import <AppKit/AppKit.h>

MacAlertCentering::MacAlertCentering(QWidget* window)
{
    if (window == nullptr)
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->window()->winId());
    NSWindow* parentWindow = view.window;
    if (parentWindow == nil)
        return;

    __block bool placed = false;
    // The alert panel becomes key as soon as it is ordered in, before it is drawn,
    // so moving it here shows it at the final position without a jump.
    id observer = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowDidBecomeKeyNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
                    NSWindow* alertWindow = note.object;
                    if (placed || alertWindow == parentWindow
                        || ![alertWindow isKindOfClass:[NSPanel class]])
                        return;
                    placed = true;
                    const NSRect parentFrame = parentWindow.frame;
                    NSRect frame = alertWindow.frame;
                    frame.origin.x = NSMidX(parentFrame) - frame.size.width / 2;
                    frame.origin.y = NSMidY(parentFrame) - frame.size.height / 2;
                    if (NSScreen* screen = parentWindow.screen)
                        frame = [alertWindow constrainFrameRect:frame toScreen:screen];
                    [alertWindow setFrameOrigin:frame.origin];
                }];
    m_observer = (void*)CFBridgingRetain(observer);
}

MacAlertCentering::~MacAlertCentering()
{
    if (m_observer == nullptr)
        return;
    id observer = (id)CFBridgingRelease(m_observer);
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
}
