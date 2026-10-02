// SPDX-FileCopyrightText: 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef GUI_MACALERTPLACEMENT_H
#define GUI_MACALERTPLACEMENT_H

#include <QtGlobal>

class QWidget;

// While alive, centres the next native alert (the NSAlert a QMessageBox becomes on
// macOS) on the given widget's window instead of where the system puts it. A
// window-modal sheet would do that too, but Qt then drops the native alert.
// No-op on other platforms.
class MacAlertCentering
{
public:
    explicit MacAlertCentering(QWidget* window);
    ~MacAlertCentering();

private:
    Q_DISABLE_COPY(MacAlertCentering)
    void* m_observer = nullptr;
};

#if !defined(Q_OS_MACOS)
inline MacAlertCentering::MacAlertCentering(QWidget*) {}
inline MacAlertCentering::~MacAlertCentering() {}
#endif

#endif
