// SPDX-FileCopyrightText: 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later


#ifndef GUI_TEXTMETRICS_H
#define GUI_TEXTMETRICS_H

#include <QFont>
#include <QFontMetricsF>
#include <QtGlobal>

namespace TextMetrics {

// Extra top padding that centres a line of text optically in a box with equal
// padding above and below the line. Some fonts (Helvetica) reserve almost no
// room above their capitals but the usual room below the baseline, which puts
// their text against the top edge. Zero for fonts that already reserve at least
// as much room above their capitals as below the baseline.
inline int opticalTopInset(const QFont& font)
{
    const QFontMetricsF metrics(font);
    const qreal aboveCaps = metrics.ascent() - metrics.capHeight();
    return qMax(0, qRound(metrics.descent() - aboveCaps));
}

}

#endif
