// SPDX-FileCopyrightText: 2007-2011, 2013-2016, 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later


#include "gui/resultdisplay.h"

#include "gui/displayformatutils.h"
#include "gui/resultlineformatutils.h"
#include "gui/oklchutils.h"
#include "core/functions.h"
#include "core/numberformatter.h"
#include "core/regexpatterns.h"
#include "core/settings.h"
#include "core/units.h"
#include "core/unicodechars.h"
#include "core/mathdsl.h"
#include "gui/simplifiedexpressionutils.h"
#include "gui/syntaxhighlighter.h"
#include "gui/textmetrics.h"
#include "gui/tooltipstyleutils.h"
#include "gui/uiconfig.h"
#include "math/cmath.h"
#include "math/floatnum/floatconfig.h"
#include "core/evaluator.h"
#include "core/session.h"
#include "core/sessionhistory.h"

#include <QLatin1String>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFrame>
#include <QHoverEvent>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QLinearGradient>
#include <QPlainTextDocumentLayout>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>

#include <limits>

namespace {
constexpr int kResultDisplayHorizontalPadding = 14;
// Classic (0.12) appearance: history text left edge should line up with the input
// editor text (Editor::textLeftInset() == classic h-padding 4 + doc margin 2 == 6).
constexpr int kResultDisplayClassicLeftInset = 6;

// Separators (empty blocks) get a constant 1 px font so their own line never
// reaches into the next entry; their height comes from HistoryDocumentLayout.
QTextCharFormat separatorCharFormat()
{
    QFont font;
    font.setPixelSize(1);
    QTextCharFormat format;
    format.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
    return format;
}

struct ResultDisplayScrollBarColors
{
    QColor track;
    QColor thumb;
    QColor hoverThumb;
    QColor pressedThumb;
};

struct ScrollToBottomButtonColors
{
    QColor background;
    QColor foreground;
    QColor hoverBackground;
    QColor hoverForeground;
    QColor outline;
};

QPointF badgeCenter(const QRect& rect)
{
    return QPointF(rect.x() + rect.width() * 0.5, rect.y() + rect.height() * 0.5);
}

QColor hoverColorForBackground(const QColor& background)
{
    const bool isLightBackground = background.lightnessF() >= 0.5;
    const QColor target = isLightBackground ? QColor(Qt::black) : QColor(Qt::white);
    const qreal blendFactor = 0.12; // subtle, but always visible even on pure black/white backgrounds

    auto mixChannel = [blendFactor](int from, int to) {
        return qBound(0, static_cast<int>(from + (to - from) * blendFactor), 255);
    };

    return QColor(mixChannel(background.red(), target.red()),
                  mixChannel(background.green(), target.green()),
                  mixChannel(background.blue(), target.blue()));
}

QVector<QColor> resultDisplayShadesFromBackground(const QColor& background)
{
    const QColor base = background.isValid()
        ? background
        : QApplication::palette().color(QPalette::Base);
    return generateOklchShades(base, UiConfig::Shade500 + 1, themePolarityForBackground(base));
}

QColor shadeOrFallback(const QVector<QColor>& shades, int index, const QColor& fallback)
{
    return shades.value(index, fallback);
}

ResultDisplayScrollBarColors scrollBarColorsForResultBackground(const QColor& background)
{
    const QVector<QColor> shades = resultDisplayShadesFromBackground(background);
    // Result-display scrollbar colors are generated from the result background
    // itself: the track stays on the result surface, while the thumb advances
    // through the same OKLCH shade steps used elsewhere for normal, hovered,
    // and pressed scrollbar states. This keeps scrollbars independent from any
    // obsolete color-scheme role while preserving the existing shade semantics.
    const QColor track = shadeOrFallback(shades, UiConfig::ResultDisplayShade, background);
    const QColor thumb = shadeOrFallback(shades, UiConfig::ResultDisplayScrollbarShade, track);
    const QColor hoverThumb = shadeOrFallback(shades, UiConfig::ResultDisplayScrollbarHoverShade, thumb);
    const QColor pressedThumb = shadeOrFallback(shades, UiConfig::ResultDisplayScrollbarPressedShade, hoverThumb);
    return { track, thumb, hoverThumb, pressedThumb };
}

ScrollToBottomButtonColors scrollToBottomButtonColorsForResultBackground(const QColor& background)
{
    const QVector<QColor> shades = resultDisplayShadesFromBackground(background);
    const QColor normalBackground =
        shadeOrFallback(shades, UiConfig::ScrollToBottomButtonBackgroundShade, background);
    const QColor hoverBackground =
        shadeOrFallback(shades, UiConfig::ScrollToBottomButtonHoverBackgroundShade, normalBackground);
    const QColor outline =
        shadeOrFallback(shades, UiConfig::ScrollToBottomButtonOutlineShade, hoverBackground);
    return {
        normalBackground,
        aaForegroundForBackground(normalBackground),
        hoverBackground,
        aaForegroundForBackground(hoverBackground),
        outline
    };
}

QIcon downArrowIcon(const QColor& color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(QPolygonF{
        QPointF(3.5, 5.5),
        QPointF(12.5, 5.5),
        QPointF(8.0, 11.0)
    });
    return QIcon(pixmap);
}

void applyContextMenuTheme(QMenu* menu,
                           const QColor& background,
                           const QColor& foreground,
                           const QColor& hoverBackground,
                           const QColor& hoverForeground)
{
    if (menu == nullptr || !background.isValid() || !foreground.isValid())
        return;

    const QColor effectiveHoverBackground = hoverBackground.isValid()
        ? hoverBackground
        : background;
    const QColor effectiveHoverForeground = hoverForeground.isValid()
        ? hoverForeground
        : foreground;
    QPalette palette = menu->palette();
    for (const QPalette::ColorGroup group : {QPalette::Active,
                                             QPalette::Inactive,
                                             QPalette::Disabled}) {
        palette.setColor(group, QPalette::Window, background);
        palette.setColor(group, QPalette::Base, background);
        palette.setColor(group, QPalette::Text, foreground);
        palette.setColor(group, QPalette::WindowText, foreground);
        palette.setColor(group, QPalette::ButtonText, foreground);
        palette.setColor(group, QPalette::Highlight, effectiveHoverBackground);
        palette.setColor(group, QPalette::HighlightedText, effectiveHoverForeground);
    }
    menu->setPalette(palette);
    menu->setStyleSheet(QStringLiteral(
        "QMenu {"
        " background-color: %1;"
        " color: %2;"
        " border: 1px solid %3;"
        " border-radius: 8px;"
        "}"
        "QMenu::item:selected {"
        " background-color: %4;"
        " color: %5;"
        "}")
                            .arg(background.name(),
                                 foreground.name(),
                                 effectiveHoverBackground.name(),
                                 effectiveHoverBackground.name(),
                                 effectiveHoverForeground.name()));

    for (QAction* action : menu->actions()) {
        if (QMenu* submenu = action->menu())
            applyContextMenuTheme(submenu,
                                  background,
                                  foreground,
                                  effectiveHoverBackground,
                                  effectiveHoverForeground);
    }
}

int maxRenderedHistoryEntries(const Session* session)
{
    static const int kDefaultMaxRenderedHistoryEntries = 800;
    const int configuredLimit = session != nullptr ? session->historyLimit() : 0;
    if (configuredLimit > 0)
        return qMin(configuredLimit, kDefaultMaxRenderedHistoryEntries);
    return kDefaultMaxRenderedHistoryEntries;
}

int firstDisplayedHistoryIndexForCount(int historyCount, const Session* session)
{
    return qMax(0, historyCount - maxRenderedHistoryEntries(session));
}

void cloneMenuActions(const QMenu* sourceMenu, QMenu* targetMenu)
{
    const QList<QAction*> sourceActions = sourceMenu->actions();
    for (QAction* sourceAction : sourceActions) {
        if (sourceAction->isSeparator()) {
            targetMenu->addSeparator();
            continue;
        }

        QMenu* sourceSubmenu = sourceAction->menu();
        if (sourceSubmenu != 0) {
            QMenu* clonedSubmenu = targetMenu->addMenu(sourceSubmenu->title());
            clonedSubmenu->setEnabled(sourceAction->isEnabled());
            cloneMenuActions(sourceSubmenu, clonedSubmenu);
            continue;
        }

        targetMenu->addAction(sourceAction);
    }
}

QString formatResultForClipboard(const Quantity& value)
{
    QString textToCopy = NumberFormatter::format(value);
    textToCopy.replace(
        RegExpPatterns::missingQuantSpBeforeUnit(),
        QStringLiteral("\\1") + QString(MathDsl::QuantSp) + QString(MathDsl::UnitStart));
    textToCopy.replace(UnicodeChars::MinusSign, MathDsl::SubOpAl1);
    return textToCopy;
}

QStringList formatResultLines(const HistoryEntry& entry, const Evaluator* evaluator)
{
    Settings* settings = Settings::instance();
    const bool oldComplexNumbers = settings->complexNumbers;
    const char oldImaginaryUnit = settings->imaginaryUnit;
    const char oldAngleUnit = settings->angleUnit;
    const char oldResultFormat = settings->resultFormat;
    const int oldResultPrecision = settings->resultPrecision;
    const char oldResultComplexForm = settings->resultComplexForm;
    const char oldUnitExp = settings->unitNegativeExponentStyle;
    const char oldRound = settings->resultRoundingMode;
    const bool oldMultiple = settings->multipleResultLinesEnabled;
    const bool oldSecondaryEnabled = settings->secondaryResultEnabled;
    const bool oldTertiaryEnabled = settings->tertiaryResultEnabled;
    const bool oldQuaternaryEnabled = settings->quaternaryResultEnabled;
    const bool oldQuinaryEnabled = settings->quinaryResultEnabled;
    const char oldSecondaryFormat = settings->alternativeResultFormat;
    const char oldTertiaryFormat = settings->tertiaryResultFormat;
    const char oldQuaternaryFormat = settings->quaternaryResultFormat;
    const char oldQuinaryFormat = settings->quinaryResultFormat;
    const int oldSecondaryPrecision = settings->secondaryResultPrecision;
    const int oldTertiaryPrecision = settings->tertiaryResultPrecision;
    const int oldQuaternaryPrecision = settings->quaternaryResultPrecision;
    const int oldQuinaryPrecision = settings->quinaryResultPrecision;
    const char oldSecondaryComplex = settings->secondaryResultComplexForm;
    const char oldTertiaryComplex = settings->tertiaryResultComplexForm;
    const char oldQuaternaryComplex = settings->quaternaryResultComplexForm;
    const char oldQuinaryComplex = settings->quinaryResultComplexForm;

    const EvaluationContext& ctx = entry.contextRef();
    settings->complexNumbers = ctx.complexOn;
    settings->imaginaryUnit = (ctx.unit == 'j') ? 'j' : 'i';
    settings->angleUnit = ctx.angle;
    settings->resultFormat = ctx.main.fmt;
    settings->resultPrecision = ctx.main.prec;
    settings->resultComplexForm = ctx.main.cplx;
    settings->unitNegativeExponentStyle = isValidUnitNegativeExponentStyle(ctx.unitExp)
        ? ctx.unitExp
        : Settings::UnitNegativeExponentSuperscript;
    settings->resultRoundingMode = isValidResultRoundingMode(ctx.round)
        ? ctx.round
        : Settings::ResultRoundingHalfAwayFromZero;
    setRuntimeUnitNegativeExponentStyle(settings->unitNegativeExponentStyle);
    setRuntimeResultRoundingMode(settings->resultRoundingMode);

    settings->multipleResultLinesEnabled = !ctx.extras.isEmpty();
    settings->secondaryResultEnabled = false;
    settings->tertiaryResultEnabled = false;
    settings->quaternaryResultEnabled = false;
    settings->quinaryResultEnabled = false;
    if (ctx.extras.size() > 0) {
        settings->secondaryResultEnabled = true;
        settings->alternativeResultFormat = ctx.extras.at(0).fmt;
        settings->secondaryResultPrecision = ctx.extras.at(0).prec;
        settings->secondaryResultComplexForm = ctx.extras.at(0).cplx;
    }
    if (ctx.extras.size() > 1) {
        settings->tertiaryResultEnabled = true;
        settings->tertiaryResultFormat = ctx.extras.at(1).fmt;
        settings->tertiaryResultPrecision = ctx.extras.at(1).prec;
        settings->tertiaryResultComplexForm = ctx.extras.at(1).cplx;
    }
    if (ctx.extras.size() > 2) {
        settings->quaternaryResultEnabled = true;
        settings->quaternaryResultFormat = ctx.extras.at(2).fmt;
        settings->quaternaryResultPrecision = ctx.extras.at(2).prec;
        settings->quaternaryResultComplexForm = ctx.extras.at(2).cplx;
    }
    if (ctx.extras.size() > 3) {
        settings->quinaryResultEnabled = true;
        settings->quinaryResultFormat = ctx.extras.at(3).fmt;
        settings->quinaryResultPrecision = ctx.extras.at(3).prec;
        settings->quinaryResultComplexForm = ctx.extras.at(3).cplx;
    }

    const QStringList lines = ResultLineFormatUtils::formatResultLinesForDisplay(
        entry.expr(),
        entry.interpretedExpr(),
        entry.result(),
        false,
        true,
        evaluator);

    settings->complexNumbers = oldComplexNumbers;
    settings->imaginaryUnit = oldImaginaryUnit;
    settings->angleUnit = oldAngleUnit;
    settings->resultFormat = oldResultFormat;
    settings->resultPrecision = oldResultPrecision;
    settings->resultComplexForm = oldResultComplexForm;
    settings->unitNegativeExponentStyle = oldUnitExp;
    settings->resultRoundingMode = oldRound;
    settings->multipleResultLinesEnabled = oldMultiple;
    settings->secondaryResultEnabled = oldSecondaryEnabled;
    settings->tertiaryResultEnabled = oldTertiaryEnabled;
    settings->quaternaryResultEnabled = oldQuaternaryEnabled;
    settings->quinaryResultEnabled = oldQuinaryEnabled;
    settings->alternativeResultFormat = oldSecondaryFormat;
    settings->tertiaryResultFormat = oldTertiaryFormat;
    settings->quaternaryResultFormat = oldQuaternaryFormat;
    settings->quinaryResultFormat = oldQuinaryFormat;
    settings->secondaryResultPrecision = oldSecondaryPrecision;
    settings->tertiaryResultPrecision = oldTertiaryPrecision;
    settings->quaternaryResultPrecision = oldQuaternaryPrecision;
    settings->quinaryResultPrecision = oldQuinaryPrecision;
    settings->secondaryResultComplexForm = oldSecondaryComplex;
    settings->tertiaryResultComplexForm = oldTertiaryComplex;
    settings->quaternaryResultComplexForm = oldQuaternaryComplex;
    settings->quinaryResultComplexForm = oldQuinaryComplex;
    setRuntimeUnitNegativeExponentStyle(settings->unitNegativeExponentStyle);
    setRuntimeResultRoundingMode(settings->resultRoundingMode);
    return lines;
}

QString formattedExpressionForDisplay(const HistoryEntry& entry, const Evaluator* evaluator)
{
    return ResultLineFormatUtils::formattedExpressionLineForDisplay(
        entry.expr(),
        entry.interpretedExpr(),
        evaluator);
}

QString simplifiedExpressionLineForDisplay(const HistoryEntry& entry, const Evaluator* evaluator)
{
    const QString simplifiedLine = ResultLineFormatUtils::simplifiedExpressionLineForDisplay(
        entry.interpretedExpr(),
        entry.expr(),
        Settings::instance()->simplifyResultExpressions,
        evaluator);
    return simplifiedLine.isEmpty() ? QString() : QStringLiteral("= ") + simplifiedLine;
}

bool isSimplifiedExpressionRenderLine(const QStringList& renderedLines, int lineIndex, const QString& simplifiedLine)
{
    if (lineIndex <= 0 || lineIndex >= renderedLines.size())
        return false;

    const QString line = renderedLines.at(lineIndex);
    if (!line.startsWith(QLatin1String("= ")))
        return false;

    return !simplifiedLine.isEmpty() && line == simplifiedLine;
}

QStringList renderedHistoryLinesForDisplay(const HistoryEntry& entry, const Evaluator* evaluator)
{
    if (entry.hasRenderedLines())
        return entry.renderedLines();

    QStringList lines;
    lines.append(formattedExpressionForDisplay(entry, evaluator));
    if (!entry.result().isNan())
        lines.append(formatResultLines(entry, evaluator));
    return lines;
}

QString formattedExpressionForDisplay(const QString& expression,
                                     const QString& interpretedExpression,
                                     const Evaluator* evaluator)
{
    return ResultLineFormatUtils::formattedExpressionLineForDisplay(
        expression,
        interpretedExpression,
        evaluator);
}

const Session* displaySession(const ResultDisplay* display)
{
    return display != nullptr ? display->session() : nullptr;
}

}

// QPlainTextDocumentLayout ignores QTextBlockFormat margins and line height
// (QTBUG-51891), so the gap between history entries is set here instead: every
// empty (separator) block is as tall as a fraction of the line height, and the
// trailing separator above the input gets half of that. Changing the fraction or
// the font needs no document edits, so it costs the same for any history length.
class HistoryDocumentLayout : public QPlainTextDocumentLayout
{
public:
    using QPlainTextDocumentLayout::QPlainTextDocumentLayout;

    void setGapFraction(qreal fraction)
    {
        if (qFuzzyCompare(fraction, m_gapFraction))
            return;
        m_gapFraction = fraction;
        emit documentSizeChanged(documentSize()); // QPlainTextEdit recomputes its scroll range
        emit update();
    }

    QRectF blockBoundingRect(const QTextBlock& block) const override
    {
        QRectF rect = QPlainTextDocumentLayout::blockBoundingRect(block);
        if (rect.isNull() || block.length() != 1)
            return rect;
        const bool trailing = !block.next().isValid();
        const qreal bottomMargin = trailing ? document()->documentMargin() : 0;
        rect.setHeight(gapPixels(trailing) + bottomMargin);
        return rect;
    }

private:
    int gapPixels(bool trailing) const
    {
        const QFont font = document()->defaultFont(); // follows zoom
        if (m_lineHeight <= 0 || font != m_metricsFont) {
            const QFontMetricsF metrics(font);
            m_lineHeight = qCeil(metrics.ascent() + metrics.descent() + qMax(0.0, metrics.leading()));
            m_metricsFont = font;
        }
        return qMax(1, qRound(m_lineHeight * m_gapFraction * (trailing ? 0.5 : 1.0)));
    }

    qreal m_gapFraction = 0.5;
    mutable QFont m_metricsFont;
    mutable int m_lineHeight = 0;
};

ResultDisplay::ResultDisplay(QWidget* parent)
    : QPlainTextEdit(parent)
    , m_highlighter(new SyntaxHighlighter(this))
    , m_scrolledLines(0)
    , m_scrollDirection(0)
    , m_isScrollingPageOnly(false)
    , m_hoverHighlightEnabled(true)
    , m_scrollBarHovered(false)
    , m_historyBlockIndexCacheDirty(true)
    , m_hoveredHistoryIndex(-1)
    , m_editingHistoryIndex(-1)
    , m_count(0)
    , m_firstDisplayedHistoryIndex(0)
    , m_loadedSessionCount(1)
    , m_closeSessionEnabled(false)
    , m_session(nullptr)
    , m_themeSurfaceColor()
    , m_toolTipBackgroundColor()
    , m_toolTipForegroundColor()
    , m_toolTipOutlineColor()
    , m_hoverActionPopup(nullptr)
    , m_hoverActionPopupLabel(nullptr)
    , m_hoverHighlightColor()
    , m_primaryColor()
    , m_contextMenuBackgroundColor()
    , m_contextMenuForegroundColor()
    , m_contextMenuHoverBackgroundColor()
    , m_contextMenuHoverForegroundColor()
    , m_hoveredActionBadge(NoActionBadge)
    , m_scrollToBottomButtonHovered(false)
    , m_scrollToBottomButton(new QToolButton(this))
{
    QTextDocument* historyDocument = new QTextDocument(this);
    m_historyLayout = new HistoryDocumentLayout(historyDocument);
    historyDocument->setDocumentLayout(m_historyLayout);
    historyDocument->setUndoRedoEnabled(false); // read-only history: no undo stack growth
    setDocument(historyDocument);
    m_highlighter->setDocument(historyDocument);
    applyHistorySpacing();

    for (QWidget** line : { &m_topEdgeLine, &m_bottomEdgeLine }) {
        *line = new QWidget(this);
        (*line)->setAttribute(Qt::WA_TransparentForMouseEvents);
        (*line)->setAutoFillBackground(true);
        (*line)->hide();
    }

    updateViewportMargins();
    setBackgroundRole(QPalette::Base);
    setLayoutDirection(Qt::LeftToRight);
    setMinimumWidth(150);
    setReadOnly(true);
    viewport()->setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
    setWordWrapMode(QTextOption::WrapAnywhere);
    setMouseTracking(true);

    QScrollBar* bar = verticalScrollBar();
    bar->setAttribute(Qt::WA_Hover, true);
    bar->setMouseTracking(true);
    bar->installEventFilter(this);
    connect(bar, &QScrollBar::valueChanged, this, [this]() {
        updateScrollToBottomButtonVisibility();
        viewport()->update();
    });
    connect(bar, &QScrollBar::rangeChanged, this, [this]() {
        repositionScrollToBottomButton();
        updateScrollToBottomButtonVisibility();
        viewport()->update();
    });

    m_scrollToBottomButton->setFocusPolicy(Qt::NoFocus);
    m_scrollToBottomButton->setObjectName(QStringLiteral("ScrollToBottomButton"));
    m_scrollToBottomButton->setCursor(Qt::ArrowCursor);
    m_scrollToBottomButton->setToolTip(QString());
    m_scrollToBottomButton->setIconSize(QSize(16, 16));
    m_scrollToBottomButton->setFixedSize(30, 30);
    m_scrollToBottomButton->setAttribute(Qt::WA_Hover, true);
    m_scrollToBottomButton->setMouseTracking(true);
    m_scrollToBottomButton->installEventFilter(this);
    updateScrollToBottomButtonStyle();
    connect(m_scrollToBottomButton, &QToolButton::clicked, this, &ResultDisplay::scrollToBottom);
    m_scrollToBottomButton->hide();
    repositionScrollToBottomButton();
}

bool ResultDisplay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == verticalScrollBar()) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
            if (!m_scrollBarHovered) {
                m_scrollBarHovered = true;
                updateScrollBarStyleSheet();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
            if (m_scrollBarHovered) {
                m_scrollBarHovered = false;
                updateScrollBarStyleSheet();
            }
        }
    } else if (watched == m_scrollToBottomButton) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
            if (!m_scrollToBottomButtonHovered) {
                m_scrollToBottomButtonHovered = true;
                updateScrollToBottomButtonStyle();
            }
            showHoverActionPopup(tr("Scroll to bottom"),
                                 m_scrollToBottomButton,
                                 m_scrollToBottomButton->mapToGlobal(
                                     m_scrollToBottomButton->rect().center()));
        } else if (event->type() == QEvent::MouseMove) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            showHoverActionPopup(tr("Scroll to bottom"),
                                 m_scrollToBottomButton,
                                 mouseEvent->globalPosition().toPoint());
        } else if (event->type() == QEvent::HoverMove) {
            showHoverActionPopup(tr("Scroll to bottom"),
                                 m_scrollToBottomButton,
                                 m_scrollToBottomButton->mapToGlobal(
                                     static_cast<QHoverEvent*>(event)->position().toPoint()));
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
            if (m_scrollToBottomButtonHovered) {
                m_scrollToBottomButtonHovered = false;
                updateScrollToBottomButtonStyle();
            }
            hideHoverActionPopup();
        } else if (event->type() == QEvent::Hide
                   || event->type() == QEvent::MouseButtonPress
                   || event->type() == QEvent::Wheel) {
            hideHoverActionPopup();
        }
    }

    return QPlainTextEdit::eventFilter(watched, event);
}

void ResultDisplay::setHoverHighlightEnabled(bool enabled)
{
    if (m_hoverHighlightEnabled == enabled)
        return;

    m_hoverHighlightEnabled = enabled;
    if (!m_hoverHighlightEnabled)
        m_hoveredHistoryIndex = -1;
    updateHoverHighlightSelection();
}

void ResultDisplay::setEditingHistoryIndex(int index)
{
    if (m_editingHistoryIndex == index)
        return;

    const int previousEditingHistoryIndex = m_editingHistoryIndex;
    m_editingHistoryIndex = index;

    if (m_editingHistoryIndex >= 0 && m_hoveredHistoryIndex >= 0) {
        const int previousHoveredHistoryIndex = m_hoveredHistoryIndex;
        m_hoveredHistoryIndex = -1;
        updateHoverHighlightSelection();
        viewport()->update(hoverActionRectForHistoryIndex(previousHoveredHistoryIndex));
    }
    if (m_editingHistoryIndex >= 0)
        updateViewportCursorAtMouse();

    updateHoverHighlightSelection();

    if (previousEditingHistoryIndex >= 0)
        viewport()->update(viewport()->rect());
    if (m_editingHistoryIndex >= 0)
        viewport()->update(viewport()->rect());
}

void ResultDisplay::setLoadedSessionCount(int count)
{
    const int normalizedCount = qMax(1, count);
    if (m_loadedSessionCount == normalizedCount)
        return;

    m_loadedSessionCount = normalizedCount;
    viewport()->update();
}

void ResultDisplay::setCloseSessionEnabled(bool enabled)
{
    if (m_closeSessionEnabled == enabled)
        return;

    m_closeSessionEnabled = enabled;
    viewport()->update();
}

void ResultDisplay::setSession(const Session* session)
{
    if (m_session == session)
        return;

    m_session = session;
    m_highlighter->setEvaluator(m_session ? m_session->evaluator() : nullptr);
    clear();
    refresh();
    viewport()->update();
}

void ResultDisplay::append(const QString& expression, Quantity& value,
                           const QString& interpretedExpression)
{
    ++m_count;
    const Evaluator* evaluator = m_session ? m_session->evaluator() : nullptr;

    appendLine(formattedExpressionForDisplay(expression, interpretedExpression, evaluator));
    if (!value.isNan()) {
        const Settings* settings = Settings::instance();
        EvaluationContext ctx;
        ctx.main.fmt = settings->resultFormat;
        ctx.main.prec = settings->resultPrecision;
        ctx.main.cplx = settings->resultComplexForm;
        ctx.complexOn = settings->complexNumbers;
        ctx.unit = settings->imaginaryUnit;
        ctx.angle = settings->angleUnit;
        ctx.unitExp = settings->unitNegativeExponentStyle;
        ctx.round = settings->resultRoundingMode;
        const HistoryEntry entry(expression, value, interpretedExpression, ctx);
        const QStringList resultLines = formatResultLines(entry, evaluator);
        const QString simplifiedLine = simplifiedExpressionLineForDisplay(entry, evaluator);
        const QStringList renderedLines = QStringList({ formattedExpressionForDisplay(entry, evaluator) }) + resultLines;
        for (int i = 0; i < resultLines.size(); ++i) {
            const QString& line = resultLines.at(i);
            appendLine(line);
            if (isSimplifiedExpressionRenderLine(renderedLines, i + 1, simplifiedLine))
                markSimplifiedExpressionBlock(document()->lastBlock().blockNumber());
        }
    }
    appendLine(QLatin1String(""));
    markHistoryBlockIndexCacheDirty();
    alignContentToBottom();
}

int ResultDisplay::count() const
{
    return m_count;
}

QPair<int, int> ResultDisplay::viewportTopAnchor() const
{
    const QTextBlock block = firstVisibleBlock();
    if (!block.isValid())
        return qMakePair(-1, 0);

    const QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
    const int offsetInBlock = qMax(0, qRound(-blockRect.top()));
    return qMakePair(block.blockNumber(), offsetInBlock);
}

void ResultDisplay::restoreViewportTopAnchor(const QPair<int, int>& anchor)
{
    if (anchor.first < 0 || document()->blockCount() <= 0)
        return;

    const int blockNumber = qBound(0, anchor.first, document()->blockCount() - 1);
    const QTextBlock block = document()->findBlockByNumber(blockNumber);
    if (!block.isValid())
        return;

    QScrollBar* bar = verticalScrollBar();
    const int targetValue = qRound(blockBoundingGeometry(block).top()) + anchor.second;
    bar->setValue(qBound(bar->minimum(), targetValue, bar->maximum()));
}

void ResultDisplay::restoreScrollValue(int value)
{
    QScrollBar* bar = verticalScrollBar();
    const int targetValue = value == std::numeric_limits<int>::max()
        ? bar->maximum()
        : qBound(bar->minimum(), value, bar->maximum());
    bar->setValue(targetValue);
    updateScrollToBottomButtonVisibility();
}

QString ResultDisplay::exportHtml() const
{
    QString str;
    m_highlighter->asHtml(str);
    return str;
}

void ResultDisplay::rehighlight()
{
    m_highlighter->update();
    const QColor backgroundColor = themeSurfaceBackground();
    QPalette palette = this->palette();
    palette.setColor(QPalette::Active, QPalette::Base, backgroundColor);
    palette.setColor(QPalette::Inactive, QPalette::Base, backgroundColor);
    palette.setColor(QPalette::Disabled, QPalette::Base, backgroundColor);
    palette.setColor(QPalette::Active, QPalette::Window, backgroundColor);
    palette.setColor(QPalette::Inactive, QPalette::Window, backgroundColor);
    palette.setColor(QPalette::Disabled, QPalette::Window, backgroundColor);
    setPalette(palette);
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);

    QPalette viewportPalette = viewport()->palette();
    viewportPalette.setColor(QPalette::Active, QPalette::Base, backgroundColor);
    viewportPalette.setColor(QPalette::Inactive, QPalette::Base, backgroundColor);
    viewportPalette.setColor(QPalette::Disabled, QPalette::Base, backgroundColor);
    viewportPalette.setColor(QPalette::Active, QPalette::Window, backgroundColor);
    viewportPalette.setColor(QPalette::Inactive, QPalette::Window, backgroundColor);
    viewportPalette.setColor(QPalette::Disabled, QPalette::Window, backgroundColor);
    viewport()->setPalette(viewportPalette);
    viewport()->setAutoFillBackground(true);
    viewport()->setAttribute(Qt::WA_StyledBackground, true);

    // Classic mode: reduce the left inset so history text is flush-left with the
    // input editor text; keep the right inset for scrollbar spacing. Non-classic
    // keeps the original symmetric padding.
    updateViewportMargins();

    updateSurfaceStyleSheet();
    updateScrollBarStyleSheet();
}

void ResultDisplay::setThemeSurfaceColor(const QColor& color)
{
    m_themeSurfaceColor = color;
    updateSurfaceStyleSheet();
    updateScrollToBottomButtonStyle();
    updateScrollBarStyleSheet();
    updateScrollEdgeLines();
}

void ResultDisplay::setThemeToolTipColors(const QColor& background,
                                          const QColor& foreground,
                                          const QColor& outline)
{
    m_toolTipBackgroundColor = background;
    m_toolTipForegroundColor = foreground;
    m_toolTipOutlineColor = outline;
    applyHoverActionPopupTheme();
}

void ResultDisplay::updateSurfaceStyleSheet()
{
    const QString background = themeSurfaceBackground().name();
    setStyleSheet(QStringLiteral("QPlainTextEdit { background-color: %1; }")
                      .arg(background));
    viewport()->setStyleSheet(QStringLiteral("QWidget { background-color: %1; }")
                                  .arg(background));
}

void ResultDisplay::setThemeInteractionColors(const QColor& hoverBackground,
                                              const QColor& primaryColor,
                                              const QColor& menuBackground,
                                              const QColor& menuForeground,
                                              const QColor& menuHoverBackground,
                                              const QColor& menuHoverForeground)
{
    m_hoverHighlightColor = hoverBackground;
    m_primaryColor = primaryColor;
    m_contextMenuBackgroundColor = menuBackground;
    m_contextMenuForegroundColor = menuForeground;
    m_contextMenuHoverBackgroundColor = menuHoverBackground;
    m_contextMenuHoverForegroundColor = menuHoverForeground;
    updateHoverHighlightSelection();
}

void ResultDisplay::clear()
{
    m_count = 0;
    m_firstDisplayedHistoryIndex = 0;
    setPlainText(QLatin1String(""));
    markHistoryBlockIndexCacheDirty();
    clearHoverFeedback();
    updateScrollToBottomButtonVisibility();
}

void ResultDisplay::clearHoverFeedback()
{
    const int previousHoveredHistoryIndex = m_hoveredHistoryIndex;
    m_hoveredHistoryIndex = -1;
    setHoveredActionBadge(NoActionBadge);
    setHoverActionToolTip(QString());
    updateHoverHighlightSelection();
    if (previousHoveredHistoryIndex >= 0)
        viewport()->update(hoverActionRectForHistoryIndex(previousHoveredHistoryIndex));
}

QRect ResultDisplay::actionBadgeRect(HoveredActionBadge badge) const
{
    if (badge == CancelActionBadge)
        return cancelGlyphBadgeRectForEditingIndex();
    if (m_hoveredHistoryIndex < 0)
        return QRect();

    switch (badge) {
    case CopyActionBadge:
        return copyGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    case EditActionBadge:
        return editGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    case SettingsActionBadge:
        return settingsGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    case RemoveActionBadge:
        return removeGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    default:
        return QRect();
    }
}

ResultDisplay::HoveredActionBadge ResultDisplay::actionBadgeAtPosition(int historyIndex, const QPoint& pos) const
{
    if (historyIndex < 0)
        return NoActionBadge;
    if (copyGlyphBadgeRectForHistoryIndex(historyIndex).contains(pos))
        return CopyActionBadge;
    if (editGlyphBadgeRectForHistoryIndex(historyIndex).contains(pos))
        return EditActionBadge;
    if (settingsGlyphBadgeRectForHistoryIndex(historyIndex).contains(pos))
        return SettingsActionBadge;
    if (removeGlyphBadgeRectForHistoryIndex(historyIndex).contains(pos))
        return RemoveActionBadge;
    return NoActionBadge;
}

int ResultDisplay::historyIndexForActionBadgeAtPosition(const QPoint& pos) const
{
    ensureHistoryBlockIndexCache();
    const int historyCount = m_firstDisplayedHistoryIndex + m_historyBlockRanges.size();
    for (int historyIndex = m_firstDisplayedHistoryIndex; historyIndex < historyCount; ++historyIndex) {
        if (actionBadgeAtPosition(historyIndex, pos) != NoActionBadge)
            return historyIndex;
    }
    return -1;
}

void ResultDisplay::setHoveredActionBadge(HoveredActionBadge badge)
{
    if (m_hoveredActionBadge == badge)
        return;

    const QRect previousRect = actionBadgeRect(m_hoveredActionBadge);
    m_hoveredActionBadge = badge;
    const QRect currentRect = actionBadgeRect(m_hoveredActionBadge);
    if (previousRect.isValid())
        viewport()->update(previousRect.adjusted(-1, -1, 1, 1));
    if (currentRect.isValid())
        viewport()->update(currentRect.adjusted(-1, -1, 1, 1));
}

// True when pos is over a line's glyphs (first to last character), not over the
// blank space beside, between or below the lines.
bool ResultDisplay::isTextAtPosition(const QPoint& pos) const
{
    const QTextBlock block = cursorForPosition(pos).block();
    const QTextLayout* layout = block.isValid() ? block.layout() : nullptr;
    if (layout == nullptr)
        return false;

    // Same origin QPlainTextEdit paints the block's layout at.
    const QPointF origin = blockBoundingGeometry(block).translated(contentOffset()).topLeft()
        + layout->position();
    for (int i = 0; i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        if (line.textLength() > 0 && line.naturalTextRect().translated(origin).contains(pos))
            return true;
    }
    return false;
}

// Browser-style cursor for read-only text: the I-beam over the glyphs and for the
// whole of a selection drag, the arrow over blank space and over the hover
// buttons (macOS buttons keep the arrow).
void ResultDisplay::updateViewportCursor(const QPoint& pos, bool overControl)
{
    const bool showIBeam = m_mouseSelecting || (!overControl && isTextAtPosition(pos));
    viewport()->setCursor(showIBeam ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void ResultDisplay::updateViewportCursorAtMouse()
{
    if (!viewport()->underMouse()) {
        viewport()->setCursor(Qt::ArrowCursor);
        return;
    }
    updateViewportCursor(viewport()->mapFromGlobal(QCursor::pos()),
                         m_hoveredActionBadge != NoActionBadge);
}

void ResultDisplay::setHoverActionToolTip(const QString& text)
{
    if (text.isEmpty())
        hideHoverActionPopup();
    else
        showHoverActionPopup(text);
}

void ResultDisplay::applyHoverActionPopupTheme()
{
    ToolTipStyleUtils::applyPopupTheme(
        m_hoverActionPopup,
        m_hoverActionPopupLabel,
        this,
        {m_toolTipBackgroundColor,
         m_toolTipForegroundColor,
         m_toolTipOutlineColor,
         UiConfig::ResultTooltipCornerRadius});
}

void ResultDisplay::ensureHoverActionPopup()
{
    if (m_hoverActionPopup != nullptr)
        return;

    m_hoverActionPopup = ToolTipStyleUtils::createPopup(this,
                                                        QStringLiteral("resultActionPopup"),
                                                        QStringLiteral("resultActionPopupLabel"),
                                                        Qt::RichText,
                                                        &m_hoverActionPopupLabel,
                                                        true);
    applyHoverActionPopupTheme();
}

void ResultDisplay::hideHoverActionPopup()
{
    if (m_hoverActionPopup != nullptr)
        m_hoverActionPopup->hide();
}

void ResultDisplay::showHoverActionPopup(const QString& text)
{
    showHoverActionPopup(text, viewport(), QCursor::pos());
}

void ResultDisplay::showHoverActionPopup(const QString& text, QWidget* anchor, const QPoint& globalPos)
{
    ensureHoverActionPopup();
    ToolTipStyleUtils::showPopup(m_hoverActionPopup,
                                 m_hoverActionPopupLabel,
                                 text,
                                 anchor,
                                 globalPos,
                                 UiConfig::ResultTooltipCornerRadius);
}

void ResultDisplay::updateHoverActionPopupMask()
{
    if (m_hoverActionPopup == nullptr)
        return;

    ToolTipStyleUtils::applyRoundedPopupMask(m_hoverActionPopup,
                                             qMax(0, UiConfig::ResultTooltipCornerRadius));
}

QColor ResultDisplay::hoverActionBadgeFillColor() const
{
    // Classic appearance: the round hover buttons blend into the row by using the
    // display/editor surface background (the darker plum) as their fill.
    if (Settings::instance()->classicAppearance)
        return themeSurfaceBackground();
    const QColor hoverColor = m_hoverHighlightColor.isValid()
        ? m_hoverHighlightColor
        : hoverColorForBackground(themeSurfaceBackground());
    return aaForegroundForBackground(hoverColor, 7.0);
}

QColor ResultDisplay::hoverActionIconColor(HoveredActionBadge badge) const
{
    if (badge == m_hoveredActionBadge && m_primaryColor.isValid())
        return m_primaryColor;
    // Classic: keep the glyph legible against the darker plum badge fill.
    if (Settings::instance()->classicAppearance)
        return aaForegroundForBackground(themeSurfaceBackground());
    return m_hoverHighlightColor.isValid()
        ? m_hoverHighlightColor
        : hoverColorForBackground(themeSurfaceBackground());
}


void ResultDisplay::reRenderAll()
{
    // Force a full rebuild of all history lines through the formatter so that a
    // change like the Classic Appearance toggle re-tightens/re-spaces existing
    // operators. clear() resets the render counters; refresh() then rebuilds.
    clear();
    refresh();
    scrollToBottom();
}

void ResultDisplay::refresh()
{
    refreshDocument();
    alignContentToBottom();
}

void ResultDisplay::applyHistorySpacing()
{
    static const qreal kGapFraction[] = { 0.25, 0.5, 1.0 }; // Small, Medium, Large
    const int spacing = qBound(0, int(Settings::instance()->historySpacing), 2);
    QScrollBar* bar = verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();
    m_historyLayout->setGapFraction(kGapFraction[spacing]);
    if (atBottom)
        bar->setValue(bar->maximum());
    alignContentToBottom();
}

void ResultDisplay::appendLine(const QString& text)
{
    // Like appendPlainText(), but through a private cursor with explicit formats:
    // appendPlainText() takes the widget cursor's format, which after a click into
    // a gap is the 1 px separator font, so the next entry would come out tiny.
    QScrollBar* bar = verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();
    const QTextCharFormat format = text.isEmpty() ? separatorCharFormat() : QTextCharFormat();
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (document()->isEmpty())
        cursor.setBlockCharFormat(format);
    else
        cursor.insertBlock(QTextBlockFormat(), format);
    if (!text.isEmpty())
        cursor.insertText(text, format);
    if (atBottom)
        bar->setValue(bar->maximum());
}

void ResultDisplay::updateViewportMargins()
{
    // Classic mode: reduce the left inset so history text is flush-left with the
    // input editor text; keep the right inset for scrollbar spacing. Non-classic
    // keeps the original symmetric padding. Top and bottom margins come from
    // alignContentToBottom().
    const int docMargin = qRound(document()->documentMargin());
    const int leftPadding = Settings::instance()->classicAppearance
        ? qMax(0, kResultDisplayClassicLeftInset - docMargin)
        : kResultDisplayHorizontalPadding;
    setViewportMargins(leftPadding, m_bottomAlignTop, kResultDisplayHorizontalPadding, m_bottomReserve);
}

int ResultDisplay::inputTooltipReserve() const
{
    // Height of the input's status tooltip (MainWindow::showStateLabel), which pops
    // up directly above the input: the newest result keeps clear of it.
    const bool classic = Settings::instance()->classicAppearance;
    const QFont tooltipFont = classic ? ToolTipStyleUtils::compactToolTipFont(font()) : font();
    const int padding = classic ? 2 : 6;
    const int border = classic ? 2 : 2 * UiConfig::PopupOutlineStrokeWidth;
    return QFontMetrics(tooltipFont).height() + TextMetrics::opticalTopInset(tooltipFont)
        + padding + border;
}

void ResultDisplay::alignContentToBottom()
{
    // QPlainTextEdit scrolls in whole lines with the top line flush, so at the
    // bottom a variable remainder (up to a line) would sit between the newest entry
    // and the input. Instead the viewport is extended upwards by that remainder (a
    // negative top margin): the remainder goes above the visible area, the top line
    // is cut there, and the newest result always sits at the same distance above
    // the input: room for the input tooltip plus the trailing separator gap.
    if (m_aligningBottom)
        return;
    QScopedValueRollback<bool> guard(m_aligningBottom, true);
    QScrollBar* bar = verticalScrollBar();
    const int margin = qRound(document()->documentMargin());
    // Below the content Qt keeps documentMargin (inside the last block) + margin + 1.
    const int reserve = qMax(0, inputTooltipReserve() - (2 * margin + 1));
    const bool wasAtBottom = bar->value() >= bar->maximum();
    if (reserve != m_bottomReserve) {
        m_bottomReserve = reserve;
        updateViewportMargins();
    }
    if (!wasAtBottom) {
        updateScrollEdgeLines();
        return; // scrolled up: leave the view alone
    }

    // Qt's rule: from the bottom, the top line is the highest one from which all
    // remaining lines fit into (viewport height - margin - 1). Find the smallest
    // such run that fills the visible height, then make the viewport exactly that
    // tall, hiding the excess above the widget.
    const int visibleHeight = contentsRect().height() - m_bottomReserve;
    const int target = visibleHeight - margin - 1;
    int run = 0;
    bool filled = false;
    for (QTextBlock block = document()->lastBlock(); block.isValid() && !filled; block = block.previous()) {
        const int blockHeight = qCeil(blockBoundingRect(block).height());
        const QTextLayout* layout = block.layout();
        const int lineCount = layout != nullptr ? layout->lineCount() : 0;
        if (block.length() == 1 || lineCount <= 1) {
            run += blockHeight;
            filled = run >= target;
            continue;
        }
        int lineBottom = blockHeight;
        for (int i = lineCount - 1; i >= 0 && !filled; --i) {
            const int lineTop = qFloor(layout->lineAt(i).y());
            run += lineBottom - lineTop;
            lineBottom = lineTop;
            filled = run >= target;
        }
    }
    const int top = filled ? -(run - target) : 0;
    if (top != m_bottomAlignTop) {
        m_bottomAlignTop = top;
        updateViewportMargins();
    }
    bar->setValue(bar->maximum());
    updateScrollEdgeLines();
}

void ResultDisplay::refreshDocument()
{
    const Session* session = displaySession(this);
    if (session == nullptr) {
        clear();
        return;
    }
    const int historyCount = session->historySize();
    const int previousScrollValue = verticalScrollBar()->value();
    const int firstDisplayedHistoryIndex = firstDisplayedHistoryIndexForCount(historyCount, session);

    const auto appendRenderedHistoryEntry = [this, session](int historyIndex) {
        const HistoryEntry& lastEntry = session->historyEntryAtRef(historyIndex);
        const Evaluator* evaluator = session->evaluator();
        const QStringList renderedLines = renderedHistoryLinesForDisplay(lastEntry, evaluator);
        const QString simplifiedLine = simplifiedExpressionLineForDisplay(lastEntry, evaluator);
        for (int i = 0; i < renderedLines.size(); ++i) {
            const QString& line = renderedLines.at(i);
            appendLine(line);
            if (isSimplifiedExpressionRenderLine(renderedLines, i, simplifiedLine))
                markSimplifiedExpressionBlock(document()->lastBlock().blockNumber());
        }
        appendLine(QLatin1String(""));
    };

    // Fast path for the common "new evaluation added one history entry" case.
    if (historyCount == m_count + 1
        && historyCount > 0
        && firstDisplayedHistoryIndex == m_firstDisplayedHistoryIndex) {
        clearHoverFeedback();
        appendRenderedHistoryEntry(historyCount - 1);
        m_count = historyCount;
        m_firstDisplayedHistoryIndex = firstDisplayedHistoryIndex;
        markHistoryBlockIndexCacheDirty();
        updateHoverHighlightSelection();
        updateScrollToBottomButtonVisibility();
        return;
    }

    const auto secondDisplayedHistoryBlock = [this]() {
        QTextBlock block = document()->firstBlock();
        while (block.isValid()) {
            if (block.text().isEmpty())
                return block.next();
            block = block.next();
        }
        return QTextBlock();
    };

    const auto removeFirstDisplayedHistoryEntry = [this, &secondDisplayedHistoryBlock]() {
        QTextBlock firstBlock = document()->firstBlock();
        if (!firstBlock.isValid())
            return false;

        QTextBlock nextEntryBlock = secondDisplayedHistoryBlock();
        QTextCursor cursor(document());
        cursor.setPosition(firstBlock.position());
        if (nextEntryBlock.isValid())
            cursor.setPosition(nextEntryBlock.position(), QTextCursor::KeepAnchor);
        else
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        return true;
    };

    const bool appendedPastRenderedWindow =
        historyCount == m_count + 1
        && firstDisplayedHistoryIndex == m_firstDisplayedHistoryIndex + 1;
    const bool historyLimitReplacedOldestEntry =
        historyCount == m_count
        && historyCount > 0
        && firstDisplayedHistoryIndex == m_firstDisplayedHistoryIndex;

    if (appendedPastRenderedWindow || historyLimitReplacedOldestEntry) {
        const bool singleRenderedEntry = document()->firstBlock().isValid()
            && !secondDisplayedHistoryBlock().isValid();
        const HistoryEntry& firstEntry = session->historyEntryAtRef(firstDisplayedHistoryIndex);
        const QString expectedFirstLine = formattedExpressionForDisplay(firstEntry, session->evaluator());
        const QString currentFirstLine = document()->firstBlock().isValid()
            ? document()->firstBlock().text()
            : QString();
        const QTextBlock secondBlock = secondDisplayedHistoryBlock();
        const bool canRollRenderedWindow =
            currentFirstLine != expectedFirstLine
            && (singleRenderedEntry || (secondBlock.isValid() && secondBlock.text() == expectedFirstLine));

        if (canRollRenderedWindow) {
            clearHoverFeedback();
            removeFirstDisplayedHistoryEntry();
            appendRenderedHistoryEntry(historyCount - 1);
            m_count = historyCount;
            m_firstDisplayedHistoryIndex = firstDisplayedHistoryIndex;
            markHistoryBlockIndexCacheDirty();
            updateHoverHighlightSelection();
            updateScrollToBottomButtonVisibility();
            return;
        }
    }

    clearHoverFeedback();
    m_count = historyCount;
    m_firstDisplayedHistoryIndex = firstDisplayedHistoryIndex;

    QStringList allLines;
    allLines.reserve(qMax(1, (m_count - m_firstDisplayedHistoryIndex) * 3));
    for (int i = m_firstDisplayedHistoryIndex; i < m_count; ++i) {
        const HistoryEntry& historyEntry = session->historyEntryAtRef(i);
        allLines.append(renderedHistoryLinesForDisplay(historyEntry, session->evaluator()));
        allLines.append(QLatin1String(""));
    }

    setPlainText(allLines.join(QLatin1String("\n")));
    markSimplifiedExpressionBlocks();
    verticalScrollBar()->setValue(previousScrollValue);

    markHistoryBlockIndexCacheDirty();
    updateHoverHighlightSelection();
    updateScrollToBottomButtonVisibility();
}

void ResultDisplay::refreshLastHistoryEntry()
{
    const Session* session = displaySession(this);
    if (session == nullptr) {
        clear();
        return;
    }
    const int historyCount = session->historySize();
    const int firstDisplayedHistoryIndex = firstDisplayedHistoryIndexForCount(historyCount, session);
    if (historyCount == 0) {
        clear();
        return;
    }

    if (m_count != historyCount
        || m_firstDisplayedHistoryIndex != firstDisplayedHistoryIndex
        || blockCount() <= 0) {
        refresh();
        return;
    }

    QTextBlock endBlock = document()->lastBlock();
    while (endBlock.isValid() && endBlock.text().isEmpty() && endBlock.previous().isValid())
        endBlock = endBlock.previous();

    if (!endBlock.isValid()) {
        refresh();
        return;
    }

    QTextBlock startBlock = endBlock;
    while (startBlock.previous().isValid() && !startBlock.previous().text().isEmpty())
        startBlock = startBlock.previous();

    const HistoryEntry& lastEntry = session->historyEntryAtRef(historyCount - 1);
    QStringList updatedLines = renderedHistoryLinesForDisplay(lastEntry, session->evaluator());
    updatedLines.append(QLatin1String(""));

    clearHoverFeedback();

    QTextCursor cursor(document());
    cursor.setPosition(startBlock.position());
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(updatedLines.join(QLatin1String("\n")));
    QTextCursor(document()->lastBlock()).setBlockCharFormat(separatorCharFormat());
    markSimplifiedExpressionBlocks();
    markHistoryBlockIndexCacheDirty();
    updateHoverHighlightSelection();
    updateScrollToBottomButtonVisibility();
    alignContentToBottom();
}

void ResultDisplay::scrollLines(int numberOfLines)
{
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->value() + numberOfLines);
}

void ResultDisplay::scrollLineUp()
{
    if (m_scrollDirection != 0) {
        stopActiveScrollingAnimation();
        return;
    }

    scrollLines(-1);
}

void ResultDisplay::scrollLineDown()
{
    if (m_scrollDirection != 0) {
        stopActiveScrollingAnimation();
        return;
    }

    scrollLines(1);
}

void ResultDisplay::scrollToDirection(int direction)
{
    m_scrolledLines = 0;
    bool mustStartTimer = (m_scrollDirection == 0);
    m_scrollDirection = direction;
    if (mustStartTimer)
        m_scrollTimer.start(16, this);
}

void ResultDisplay::scrollPageUp()
{
    if (verticalScrollBar()->value() == 0)
        return;

    m_isScrollingPageOnly = true;
    scrollToDirection(-1);
}

void ResultDisplay::scrollPageDown()
{
    if (verticalScrollBar()->value() == verticalScrollBar()->maximum())
        return;

    m_isScrollingPageOnly = true;
    scrollToDirection(1);
}

void ResultDisplay::scrollToTop()
{
    if (verticalScrollBar()->value() == 0)
        return;

    m_isScrollingPageOnly = false;
    scrollToDirection(-1);
}


void ResultDisplay::scrollToBottom()
{
    if (verticalScrollBar()->value() == verticalScrollBar()->maximum())
        return;

    m_isScrollingPageOnly = false;
    scrollToDirection(1);
}

void ResultDisplay::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    // Separator heights follow the font (HistoryDocumentLayout); re-align the bottom.
    if (event->type() == QEvent::FontChange)
        alignContentToBottom();
}

void ResultDisplay::increaseFontPointSize()
{
    QFont newFont = font();
    const int newSize = newFont.pointSize() + 1;
    if (newSize > 96)
        return;
    newFont.setPointSize(newSize);
    setFont(newFont);
}

void ResultDisplay::decreaseFontPointSize()
{
    QFont newFont = font();
    const int newSize = newFont.pointSize() - 1;
    if (newSize < 8)
        return;
    newFont.setPointSize(newSize);
    setFont(newFont);
}

void ResultDisplay::mouseDoubleClickEvent(QMouseEvent* event)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    QString text = cursor.selectedText();
    QString resultMarker = QLatin1String("= ");
    if (text.startsWith(resultMarker)) {
        // Display lines strip unit brackets for readability, but editor input
        // requires canonical expression syntax ("value[unit]") with QuantSp.
        // For result lines (all blocks after the expression block in the same
        // history entry), rehydrate from the underlying Quantity instead of
        // copying the visually simplified text.
        const int historyIndex = historyIndexAtPosition(event->pos());
        if (historyIndex >= 0) {
            int startBlock = -1;
            int endBlock = -1;
            if (blockRangeForHistoryIndex(historyIndex, startBlock, endBlock)
                && cursor.blockNumber() > startBlock)
            {
                const Session* session = displaySession(this);
                if (session != nullptr && historyIndex < session->historySize()) {
                    const Quantity value = session->historyEntryAtRef(historyIndex).result();
                    if (!value.isNan()) {
                        QString clipboardText = formatResultForClipboard(value);
                        const QString displayedResultLine = text.mid(resultMarker.size());
                        if (!clipboardText.contains(MathDsl::UnitStart)
                            && !clipboardText.endsWith(MathDsl::Deg)
                            && !clipboardText.endsWith(UnicodeChars::Prime)
                            && !clipboardText.endsWith(UnicodeChars::DoublePrime)) {
                            if (displayedResultLine.endsWith(MathDsl::Deg))
                                clipboardText += MathDsl::Deg;
                            else if (displayedResultLine.endsWith(UnicodeChars::Prime))
                                clipboardText += UnicodeChars::Prime;
                            else if (displayedResultLine.endsWith(UnicodeChars::DoublePrime))
                                clipboardText += UnicodeChars::DoublePrime;
                        }
                        emit expressionSelected(clipboardText);
                        return;
                    }
                }
            }
        }
        text.remove(resultMarker);
    }
    emit expressionSelected(text);
}

void ResultDisplay::mousePressEvent(QMouseEvent* event)
{
    emit clicked();

    if (m_editingHistoryIndex >= 0) {
        const QRect cancelRect = cancelGlyphBadgeRectForEditingIndex();
        if (event->button() == Qt::LeftButton && cancelRect.isValid() && cancelRect.contains(event->pos())) {
            emit cancelHistoryEditRequested();
            event->accept();
            return;
        }
        m_mouseSelecting = event->button() == Qt::LeftButton;
        QPlainTextEdit::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton && m_hoverHighlightEnabled) {
        const int actionHistoryIndex = m_hoveredHistoryIndex >= 0
            && actionBadgeAtPosition(m_hoveredHistoryIndex, event->pos()) != NoActionBadge
                ? m_hoveredHistoryIndex
                : historyIndexForActionBadgeAtPosition(event->pos());
        const HoveredActionBadge actionBadge = actionBadgeAtPosition(actionHistoryIndex, event->pos());

        if (actionBadge == CopyActionBadge) {
            const Session* session = displaySession(this);
            if (session != nullptr && actionHistoryIndex >= 0 && actionHistoryIndex < session->historySize()) {
                const Quantity value = session->historyEntryAtRef(actionHistoryIndex).result();
                if (!value.isNan())
                    QApplication::clipboard()->setText(formatResultForClipboard(value), QClipboard::Clipboard);
            }
            event->accept();
            return;
        }

        if (actionBadge == EditActionBadge) {
            emit editHistoryEntryRequested(actionHistoryIndex);
            event->accept();
            return;
        }

        if (actionBadge == SettingsActionBadge) {
            emit editHistoryEntryContextRequested(actionHistoryIndex);
            event->accept();
            return;
        }

        if (actionBadge == RemoveActionBadge) {
            emit removeHistoryEntryRequested(actionHistoryIndex);
            event->accept();
            return;
        }
    }

    m_mouseSelecting = event->button() == Qt::LeftButton;
    QPlainTextEdit::mousePressEvent(event);
}

void ResultDisplay::mouseReleaseEvent(QMouseEvent* event)
{
    QPlainTextEdit::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && m_mouseSelecting) {
        m_mouseSelecting = false;
        updateViewportCursor(event->pos(), m_hoveredActionBadge != NoActionBadge);
    }
}

QMenu* ResultDisplay::createContextMenu(const QPoint& pos)
{
    QMenu* menu = createStandardContextMenu();
    const int historyIndex = historyIndexAtPosition(pos);
    if (historyIndex >= 0) {
        const Session* session = displaySession(this);
        if (session == nullptr)
            return menu;
        menu->addSeparator();
        QAction* copyExpressionAction = menu->addAction(tr("Copy Expression"));
        connect(copyExpressionAction, &QAction::triggered, this, [session, historyIndex]() {
            if (historyIndex < 0 || historyIndex >= session->historySize())
                return;

            QApplication::clipboard()->setText(session->historyEntryAtRef(historyIndex).expr(), QClipboard::Clipboard);
        });
        QAction* copyResultAction = menu->addAction(tr("Copy Result"));
        const bool canCopyResult = historyIndex >= 0
            && historyIndex < session->historySize()
            && !session->historyEntryAtRef(historyIndex).result().isNan();
        copyResultAction->setEnabled(canCopyResult);
        connect(copyResultAction, &QAction::triggered, this, [session, historyIndex]() {
            if (historyIndex < 0 || historyIndex >= session->historySize())
                return;

            const Quantity value = session->historyEntryAtRef(historyIndex).result();
            if (value.isNan())
                return;

            QApplication::clipboard()->setText(formatResultForClipboard(value), QClipboard::Clipboard);
        });
        menu->addSeparator();
        QAction* editAction = menu->addAction(tr("Edit Expression"));
        connect(editAction, &QAction::triggered, this, [this, historyIndex]() {
            emit editHistoryEntryRequested(historyIndex);
        });
        QAction* editSettingsAction = menu->addAction(tr("Change Settings"));
        connect(editSettingsAction, &QAction::triggered, this, [this, historyIndex]() {
            emit editHistoryEntryContextRequested(historyIndex);
        });
        QAction* removeAction = menu->addAction(tr("Remove Calculation"));
        connect(removeAction, &QAction::triggered, this, [this, historyIndex]() {
            emit removeHistoryEntryRequested(historyIndex);
        });
        menu->addSeparator();
        QAction* removeAboveAction = menu->addAction(tr("Remove All Calculations Above"));
        connect(removeAboveAction, &QAction::triggered, this, [this, historyIndex]() {
            emit removeHistoryEntriesAboveRequested(historyIndex);
        });
        QAction* removeBelowAction = menu->addAction(tr("Remove All Calculations Below"));
        connect(removeBelowAction, &QAction::triggered, this, [this, historyIndex]() {
            emit removeHistoryEntriesBelowRequested(historyIndex);
        });
    }

    menu->addSeparator();
    QAction* newSessionAction = menu->addAction(tr("New Tab"));
    connect(newSessionAction, &QAction::triggered, this, [this]() {
        emit newSessionRequested();
    });
    QAction* openSessionAction = menu->addAction(tr("Open Session"));
    connect(openSessionAction, &QAction::triggered, this, [this]() {
        emit openSessionRequested();
    });
    menu->addSeparator();
    QAction* splitLeftAction = menu->addAction(tr("Split Left"));
    connect(splitLeftAction, &QAction::triggered, this, [this]() {
        emit splitLeftRequested();
    });
    QAction* splitRightAction = menu->addAction(tr("Split Right"));
    connect(splitRightAction, &QAction::triggered, this, [this]() {
        emit splitRightRequested();
    });
    QAction* splitUpAction = menu->addAction(tr("Split Up"));
    connect(splitUpAction, &QAction::triggered, this, [this]() {
        emit splitUpRequested();
    });
    QAction* splitDownAction = menu->addAction(tr("Split Down"));
    connect(splitDownAction, &QAction::triggered, this, [this]() {
        emit splitDownRequested();
    });
    menu->addSeparator();
    QAction* importSessionAction = menu->addAction(tr("&Import..."));
    connect(importSessionAction, &QAction::triggered, this, [this]() {
        emit importSessionRequested();
    });
    QMenu* exportSessionMenu = menu->addMenu(tr("&Export"));
    QAction* exportJsonAction = exportSessionMenu->addAction(QStringLiteral("JSON"));
    connect(exportJsonAction, &QAction::triggered, this, [this]() {
        emit exportSessionJsonRequested();
    });
    QAction* exportPlainTextAction = exportSessionMenu->addAction(tr("Plain &text"));
    connect(exportPlainTextAction, &QAction::triggered, this, [this]() {
        emit exportSessionPlainTextRequested();
    });
    QAction* exportHtmlAction = exportSessionMenu->addAction(QStringLiteral("&HTML"));
    connect(exportHtmlAction, &QAction::triggered, this, [this]() {
        emit exportSessionHtmlRequested();
    });
    menu->addSeparator();
    QAction* duplicateSessionAction = menu->addAction(tr("Duplicate Session"));
    connect(duplicateSessionAction, &QAction::triggered, this, [this]() {
        emit duplicateSessionRequested();
    });
    QAction* renameSessionAction = menu->addAction(tr("Rename Session"));
    connect(renameSessionAction, &QAction::triggered, this, [this]() {
        emit renameSessionRequested();
    });
    menu->addSeparator();
    QAction* clearSessionAction = menu->addAction(tr("Clear Session"));
    connect(clearSessionAction, &QAction::triggered, this, [this]() {
        emit clearSessionRequested();
    });
    QAction* deleteSessionAction = menu->addAction(tr("Delete Session"));
    connect(deleteSessionAction, &QAction::triggered, this, [this]() {
        emit deleteSessionRequested();
    });
    menu->addSeparator();
    QAction* closeSessionAction = menu->addAction(tr("Close Session"));
    connect(closeSessionAction, &QAction::triggered, this, [this]() {
        emit closeSessionRequested();
    });
    QAction* closePaneAction = menu->addAction(tr("Close Pane"));
    connect(closePaneAction, &QAction::triggered, this, [this]() {
        emit closePaneRequested();
    });

    QMainWindow* mainWindow = qobject_cast<QMainWindow*>(window());
    if (mainWindow != 0 && mainWindow->menuBar() != 0 && !mainWindow->menuBar()->isVisible()) {
        menu->addSeparator();
        QMenu* mainMenu = menu->addMenu(tr("Main Menu"));
        const QList<QAction*> topLevelActions = mainWindow->menuBar()->actions();
        for (QAction* topLevelAction : topLevelActions) {
            if (topLevelAction->isSeparator()) {
                mainMenu->addSeparator();
                continue;
            }

            QMenu* sourceSubmenu = topLevelAction->menu();
            if (sourceSubmenu != 0) {
                QMenu* clonedTopLevelSubmenu = mainMenu->addMenu(sourceSubmenu->title());
                clonedTopLevelSubmenu->setEnabled(topLevelAction->isEnabled());
                cloneMenuActions(sourceSubmenu, clonedTopLevelSubmenu);
            }
        }
    }

    applyContextMenuTheme(menu,
                          m_contextMenuBackgroundColor,
                          m_contextMenuForegroundColor,
                          m_contextMenuHoverBackgroundColor,
                          m_contextMenuHoverForegroundColor);
    return menu;
}

void ResultDisplay::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = createContextMenu(event->pos());
    menu->exec(event->globalPos());
    delete menu;
}

void ResultDisplay::leaveEvent(QEvent* event)
{
    QPlainTextEdit::leaveEvent(event);
    viewport()->setCursor(Qt::ArrowCursor);
    setHoveredActionBadge(NoActionBadge);
    setHoverActionToolTip(QString());
    if (m_hoveredHistoryIndex >= 0) {
        const int previousHoveredHistoryIndex = m_hoveredHistoryIndex;
        m_hoveredHistoryIndex = -1;
        updateHoverHighlightSelection();
        viewport()->update(hoverActionRectForHistoryIndex(previousHoveredHistoryIndex));
    }
}

void ResultDisplay::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != m_scrollTimer.timerId()) {
        QWidget::timerEvent(event);
        return;
    }

    if (m_isScrollingPageOnly)
        pageScrollEvent();
    else
        fullContentScrollEvent();
}

void ResultDisplay::pageScrollEvent()
{
    if (m_scrolledLines >= linesPerPage()) {
        stopActiveScrollingAnimation();
        return;
    }

    scrollLines(m_scrollDirection * 2);
    m_scrolledLines += 2;
}

void ResultDisplay::fullContentScrollEvent()
{
    QScrollBar* bar = verticalScrollBar();
    int value = bar->value();
    bool shouldStop = (m_scrollDirection == -1 && value <= 0) || (m_scrollDirection == 1 && value >= bar->maximum());

    if (shouldStop && m_scrollDirection != 0) {
        stopActiveScrollingAnimation();
        return;
    }

    scrollLines(m_scrollDirection * 10);
}

void ResultDisplay::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() == (Qt::ShiftModifier | Qt::ControlModifier)) {
        if (event->angleDelta().y() > 0)
            emit shiftControlWheelUp();
        else
            emit shiftControlWheelDown();
    } else if (event->modifiers() == Qt::ShiftModifier) {
        if (event->angleDelta().y() > 0)
            emit shiftWheelUp();
        else
            emit shiftWheelDown();
    } else if (event->modifiers() == Qt::ControlModifier) {
        if (event->angleDelta().y() > 0)
            emit controlWheelUp();
        else
            emit controlWheelDown();
    } else {
        QPlainTextEdit::wheelEvent(event);
        return;
    }

    event->accept();
}

void ResultDisplay::mouseMoveEvent(QMouseEvent* event)
{
    QPlainTextEdit::mouseMoveEvent(event);

    if (event->buttons() & Qt::LeftButton) {
        setHoverActionToolTip(QString());
        setHoveredActionBadge(NoActionBadge);
        updateViewportCursor(event->pos());
        return;
    }

    if (m_editingHistoryIndex >= 0) {
        const QRect cancelRect = cancelGlyphBadgeRectForEditingIndex();
        const bool overCancelGlyph = cancelRect.isValid() && cancelRect.contains(event->pos());
        setHoveredActionBadge(overCancelGlyph ? CancelActionBadge : NoActionBadge);
        setHoverActionToolTip(overCancelGlyph ? tr("Cancel editing") : QString());
        updateViewportCursor(event->pos(), overCancelGlyph);
        return;
    }

    if (!m_hoverHighlightEnabled) {
        if (m_hoveredHistoryIndex >= 0) {
            const int previousHoveredHistoryIndex = m_hoveredHistoryIndex;
            m_hoveredHistoryIndex = -1;
            setHoveredActionBadge(NoActionBadge);
            updateHoverHighlightSelection();
            viewport()->update(hoverActionRectForHistoryIndex(previousHoveredHistoryIndex));
        }
        setHoverActionToolTip(QString());
        updateViewportCursor(event->pos());
        return;
    }

    int hoveredHistoryIndex = historyIndexAtPosition(event->pos());
    if (hoveredHistoryIndex < 0)
        hoveredHistoryIndex = historyIndexForActionBadgeAtPosition(event->pos());
    if (hoveredHistoryIndex >= 0
            && (historyBlockOverlapsSessionBadge(hoveredHistoryIndex)
                || historyBlockOverlapsScrollToBottomButton(hoveredHistoryIndex))) {
        hoveredHistoryIndex = -1;
    }
    if (hoveredHistoryIndex != m_hoveredHistoryIndex) {
        const int previousHoveredHistoryIndex = m_hoveredHistoryIndex;
        m_hoveredHistoryIndex = hoveredHistoryIndex;
        updateHoverHighlightSelection();
        if (previousHoveredHistoryIndex >= 0)
            viewport()->update(hoverActionRectForHistoryIndex(previousHoveredHistoryIndex));
        if (m_hoveredHistoryIndex >= 0)
            viewport()->update(hoverActionRectForHistoryIndex(m_hoveredHistoryIndex));
    }

    const HoveredActionBadge actionBadge = actionBadgeAtPosition(m_hoveredHistoryIndex, event->pos());
    const bool overActionGlyph = actionBadge != NoActionBadge;
    setHoveredActionBadge(actionBadge);
    updateViewportCursor(event->pos(), overActionGlyph);

    setHoverActionToolTip(actionBadge == CopyActionBadge ? tr("Copy result")
        : actionBadge == EditActionBadge ? tr("Edit expression")
        : actionBadge == SettingsActionBadge ? tr("Change settings")
        : actionBadge == RemoveActionBadge ? tr("Remove calculation")
        : QString());
}

void ResultDisplay::paintEvent(QPaintEvent* event)
{
    QPlainTextEdit::paintEvent(event);

    QPainter painter(viewport());

    if (m_editingHistoryIndex >= 0) {
        const QRect cancelRect = cancelGlyphBadgeRectForEditingIndex();
        if (!cancelRect.isValid())
            return;

        const QColor badgeFill = hoverActionBadgeFillColor();
        const QColor iconColor = hoverActionIconColor(CancelActionBadge);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(badgeFill);
        painter.drawEllipse(cancelRect);
        painter.setPen(QPen(iconColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF center = badgeCenter(cancelRect);
        const qreal side = cancelRect.width() * 0.22;
        painter.drawRect(QRectF(center.x() - side, center.y() - side, side * 2.0, side * 2.0));
        return;
    }

    if (!m_hoverHighlightEnabled || m_hoveredHistoryIndex < 0)
        return;

    const QRect copyRect = copyGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    const QRect removeRect = removeGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    const QRect editRect = editGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    const QRect settingsRect = settingsGlyphBadgeRectForHistoryIndex(m_hoveredHistoryIndex);
    if (!copyRect.isValid() || !removeRect.isValid() || !editRect.isValid() || !settingsRect.isValid())
        return;

    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor badgeFill = hoverActionBadgeFillColor();
    painter.setPen(Qt::NoPen);
    painter.setBrush(badgeFill);
    painter.drawEllipse(copyRect);
    painter.setPen(QPen(hoverActionIconColor(CopyActionBadge), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF copyCenter = badgeCenter(copyRect);
    const qreal pageSize = copyRect.width() * 0.33;
    const QRectF backPage(copyCenter.x() - pageSize * 0.72,
                          copyCenter.y() - pageSize * 0.72,
                          pageSize,
                          pageSize);
    const QRectF frontPage(copyCenter.x() - pageSize * 0.28,
                           copyCenter.y() - pageSize * 0.28,
                           pageSize,
                           pageSize);
    const qreal radius = qMax(0.8, copyRect.width() * 0.07);
    painter.drawRoundedRect(backPage, radius, radius);
    painter.drawRoundedRect(frontPage, radius, radius);

    painter.setPen(Qt::NoPen);
    painter.setBrush(badgeFill);
    painter.drawEllipse(editRect);
    painter.setPen(QPen(hoverActionIconColor(EditActionBadge), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF editCenter = badgeCenter(editRect);
    const qreal editHalf = editRect.width() * 0.20;
    painter.drawLine(QPointF(editCenter.x() - editHalf, editCenter.y() + editHalf),
                     QPointF(editCenter.x() + editHalf, editCenter.y() - editHalf));
    painter.setPen(QPen(hoverActionIconColor(EditActionBadge), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(editCenter.x() + editHalf * 0.62, editCenter.y() - editHalf * 0.62),
                     QPointF(editCenter.x() + editHalf * 1.08, editCenter.y() - editHalf * 1.08));

    painter.setPen(Qt::NoPen);
    painter.setBrush(badgeFill);
    painter.drawEllipse(settingsRect);
    painter.setPen(QPen(hoverActionIconColor(SettingsActionBadge), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF settingsCenter = badgeCenter(settingsRect);
    const qreal gear = settingsRect.width() * 0.16;
    painter.drawEllipse(QRectF(settingsCenter.x() - gear, settingsCenter.y() - gear, gear * 2.0, gear * 2.0));
    painter.drawLine(QPointF(settingsCenter.x() - gear * 1.9, settingsCenter.y()),
                     QPointF(settingsCenter.x() - gear * 1.2, settingsCenter.y()));
    painter.drawLine(QPointF(settingsCenter.x() + gear * 1.2, settingsCenter.y()),
                     QPointF(settingsCenter.x() + gear * 1.9, settingsCenter.y()));
    painter.drawLine(QPointF(settingsCenter.x(), settingsCenter.y() - gear * 1.9),
                     QPointF(settingsCenter.x(), settingsCenter.y() - gear * 1.2));
    painter.drawLine(QPointF(settingsCenter.x(), settingsCenter.y() + gear * 1.2),
                     QPointF(settingsCenter.x(), settingsCenter.y() + gear * 1.9));

    painter.setPen(Qt::NoPen);
    painter.setBrush(badgeFill);
    painter.drawEllipse(removeRect);
    painter.setPen(QPen(hoverActionIconColor(RemoveActionBadge), 1.8, Qt::SolidLine, Qt::RoundCap));
    const QPointF center = badgeCenter(removeRect);
    const qreal half = removeRect.width() * 0.22;
    painter.drawLine(QPointF(center.x() - half, center.y() - half),
                     QPointF(center.x() + half, center.y() + half));
    painter.drawLine(QPointF(center.x() - half, center.y() + half),
                     QPointF(center.x() + half, center.y() - half));
}

void ResultDisplay::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    repositionScrollToBottomButton();
    alignContentToBottom();
    updateScrollEdgeLines();
}

void ResultDisplay::scrollContentsBy(int dx, int dy)
{
    QPlainTextEdit::scrollContentsBy(dx, dy);
    updateScrollToBottomButtonVisibility();
    updateScrollEdgeLines();
    // Scrolled back down to the end: re-align once the scroll has settled.
    if (!m_aligningBottom && verticalScrollBar()->value() == verticalScrollBar()->maximum())
        QTimer::singleShot(0, this, &ResultDisplay::alignContentToBottom);
    // Text moved under a resting mouse: re-evaluate text vs. blank space.
    if (viewport()->underMouse())
        updateViewportCursorAtMouse();
    viewport()->update();
}

void ResultDisplay::stopActiveScrollingAnimation()
{
    m_scrollTimer.stop();
    m_scrolledLines = 0;
    m_scrollDirection = 0;
    updateScrollToBottomButtonVisibility();
}

void ResultDisplay::updateScrollEdgeLines()
{
    // A full-width 1 px rule at an edge only while content continues past it (the
    // macOS scroll-edge cue); unlike a fade it costs no space and dims no text.
    // Child widgets rather than viewport painting, so the rule spans the padding
    // and scroll bar too.
    if (m_topEdgeLine == nullptr || m_bottomEdgeLine == nullptr)
        return;
    QScrollBar* bar = verticalScrollBar();
    const bool scrollable = bar->maximum() > bar->minimum();
    QColor lineColor = aaForegroundForBackground(themeSurfaceBackground());
    lineColor.setAlpha(48);
    const QRect area = contentsRect();
    const QRect viewportRect = viewport()->geometry();
    const QPair<QWidget*, int> lines[] = {
        { m_topEdgeLine, qMax(area.top(), viewportRect.top()) },
        { m_bottomEdgeLine, qMin(area.bottom(), viewportRect.bottom()) },
    };
    for (const auto& line : lines) {
        QPalette palette = line.first->palette();
        palette.setColor(QPalette::Window, lineColor);
        line.first->setPalette(palette);
        line.first->setGeometry(area.left(), line.second, area.width(), 1);
        line.first->raise();
    }
    m_topEdgeLine->setVisible(scrollable && bar->value() > bar->minimum());
    m_bottomEdgeLine->setVisible(scrollable && bar->value() < bar->maximum());
}

void ResultDisplay::repositionScrollToBottomButton()
{
    if (!m_scrollToBottomButton)
        return;

    const int rightMargin = kResultDisplayHorizontalPadding;
    const int bottomMargin = 10;
    const QRect contentRect = contentsRect();
    const int x = contentRect.left()
        + qMax(0, contentRect.width() - m_scrollToBottomButton->width() - rightMargin);
    const int y = contentRect.top()
        + qMax(0, contentRect.height() - m_scrollToBottomButton->height() - bottomMargin);
    m_scrollToBottomButton->move(x, y);
    m_scrollToBottomButton->raise();
}

void ResultDisplay::updateScrollToBottomButtonVisibility()
{
    if (!m_scrollToBottomButton)
        return;

    QScrollBar* bar = verticalScrollBar();
    const bool hasScrollableContent = bar->maximum() > bar->minimum();
    const bool nearBottom = bar->value() >= bar->maximum();
    m_scrollToBottomButton->setVisible(hasScrollableContent && !nearBottom);
}

void ResultDisplay::updateScrollToBottomButtonStyle()
{
    if (!m_scrollToBottomButton)
        return;

    const ScrollToBottomButtonColors colors =
        scrollToBottomButtonColorsForResultBackground(themeSurfaceBackground());
    const QColor foreground = m_scrollToBottomButtonHovered
        ? colors.hoverForeground
        : colors.foreground;

    m_scrollToBottomButton->setIcon(downArrowIcon(foreground));
    m_scrollToBottomButton->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  border: %4px solid %3;"
        "  border-radius: 15px;"
        "  background: %1;"
        "}"
        "QToolButton:hover {"
        "  background: %2;"
        "}")
        .arg(colors.background.name(),
             colors.hoverBackground.name(),
             colors.outline.name())
        .arg(UiConfig::OutlineStrokeWidth));
}

void ResultDisplay::updateScrollBarStyleSheet()
{
    static const int kBaseScrollBarWidthAt96Dpi = 5;
    static const qreal kReferenceDpi = 96.0;
    const int baseScrollBarWidth = qMax(
        1,
        qRound(kBaseScrollBarWidthAt96Dpi * (logicalDpiX() / kReferenceDpi)));
    const int scrollBarWidth = baseScrollBarWidth * 2;
    const int handleHorizontalMargin = m_scrollBarHovered ? 0 : baseScrollBarWidth / 2;
    const ResultDisplayScrollBarColors colors =
        scrollBarColorsForResultBackground(themeSurfaceBackground());

    verticalScrollBar()->setStyleSheet(QString(
        "QScrollBar:vertical {"
        "   border: 0;"
        "   margin: 0 0 0 0;"
        "   background: %1;"
        "   width: %3px;"
        "}"
        "QScrollBar::handle:vertical {"
        "   background: %2;"
        "   margin: 0 %4px 0 %4px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "   background: %5;"
        "}"
        "QScrollBar::handle:vertical:pressed {"
        "   background: %6;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "   border: 0;"
        "   width: 0;"
        "   height: 0;"
        "   background: %1;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "   background: %1;"
        "}"
    ).arg(colors.track.name(),
          colors.thumb.name())
      .arg(scrollBarWidth)
      .arg(handleHorizontalMargin)
      .arg(colors.hoverThumb.name(),
           colors.pressedThumb.name()));
}

int ResultDisplay::historyIndexAtPosition(const QPoint& pos) const
{
    const QTextCursor cursor = cursorForPosition(pos);
    const int blockNumber = cursor.blockNumber();
    if (blockNumber < 0)
        return -1;

    ensureHistoryBlockIndexCache();
    if (blockNumber >= m_blockToHistoryIndex.size())
        return -1;

    const int historyIndex = m_blockToHistoryIndex.at(blockNumber);
    const int localHistoryIndex = historyIndex - m_firstDisplayedHistoryIndex;
    if (localHistoryIndex < 0 || localHistoryIndex >= m_historyBlockRanges.size())
        return -1;

    const QPair<int, int> range = m_historyBlockRanges.at(localHistoryIndex);
    // Separator blank lines are intentionally mapped in the cache; keep them
    // non-interactive while still allowing genuine blank lines inside entries.
    if (blockNumber < range.first || blockNumber > range.second)
        return -1;

    return historyIndex;
}

bool ResultDisplay::blockRangeForHistoryIndex(int historyIndex, int& startBlock, int& endBlock) const
{
    startBlock = -1;
    endBlock = -1;
    const int localHistoryIndex = historyIndex - m_firstDisplayedHistoryIndex;
    if (localHistoryIndex < 0)
        return false;

    ensureHistoryBlockIndexCache();
    if (localHistoryIndex >= m_historyBlockRanges.size())
        return false;

    const QPair<int, int> range = m_historyBlockRanges.at(localHistoryIndex);
    startBlock = range.first;
    endBlock = range.second;
    return startBlock >= 0 && endBlock >= startBlock;
}

QRect ResultDisplay::removeGlyphRectForHistoryIndex(int historyIndex) const
{
    int startBlock = -1;
    int endBlock = -1;
    if (!blockRangeForHistoryIndex(historyIndex, startBlock, endBlock))
        return QRect();

    QTextBlock start = document()->findBlockByNumber(startBlock);
    QTextBlock end = document()->findBlockByNumber(endBlock);
    if (!start.isValid() || !end.isValid())
        return QRect();

    const QRectF startRect = blockBoundingGeometry(start).translated(contentOffset());
    const QRectF endRect = blockBoundingGeometry(end).translated(contentOffset());
    const int top = qRound(startRect.top());
    const int bottom = qRound(endRect.bottom());
    if (bottom <= top)
        return QRect();

    const int side = qMax(12, fontMetrics().height());
    const int spacing = 4;
    const int rightPadding = 6;
    const int left = viewport()->width() - side - rightPadding;
    const int minLeft = side + spacing;
    if (left < minLeft)
        return QRect();

    return QRect(left, top, side, bottom - top);
}

QRect ResultDisplay::removeGlyphBadgeRectForHistoryIndex(int historyIndex) const
{
    const QRect removeLaneRect = removeGlyphRectForHistoryIndex(historyIndex);
    if (!removeLaneRect.isValid())
        return QRect();

    const int diameter = qMin(removeLaneRect.width(), qMax(12, fontMetrics().height() - 2));
    const int left = removeLaneRect.left() + (removeLaneRect.width() - diameter) / 2;
    const int top = removeLaneRect.top() + (removeLaneRect.height() - diameter) / 2;
    return QRect(left, top, diameter, diameter);
}

QRect ResultDisplay::editGlyphRectForHistoryIndex(int historyIndex) const
{
    const QRect removeRect = removeGlyphRectForHistoryIndex(historyIndex);
    if (!removeRect.isValid())
        return QRect();

    const int spacing = 4;
    const int left = removeRect.left() - removeRect.width() - spacing;
    if (left < 0)
        return QRect();

    return QRect(left, removeRect.top(), removeRect.width(), removeRect.height());
}

QRect ResultDisplay::editGlyphBadgeRectForHistoryIndex(int historyIndex) const
{
    const QRect editLaneRect = editGlyphRectForHistoryIndex(historyIndex);
    if (!editLaneRect.isValid())
        return QRect();

    const int diameter = qMin(editLaneRect.width(), qMax(12, fontMetrics().height() - 2));
    const int left = editLaneRect.left() + (editLaneRect.width() - diameter) / 2;
    const int top = editLaneRect.top() + (editLaneRect.height() - diameter) / 2;
    return QRect(left, top, diameter, diameter);
}

QRect ResultDisplay::copyGlyphRectForHistoryIndex(int historyIndex) const
{
    const QRect settingsRect = settingsGlyphRectForHistoryIndex(historyIndex);
    if (!settingsRect.isValid())
        return QRect();

    const int spacing = 4;
    const int left = settingsRect.left() - settingsRect.width() - spacing;
    if (left < 0)
        return QRect();

    return QRect(left, settingsRect.top(), settingsRect.width(), settingsRect.height());
}

QRect ResultDisplay::copyGlyphBadgeRectForHistoryIndex(int historyIndex) const
{
    const QRect copyLaneRect = copyGlyphRectForHistoryIndex(historyIndex);
    if (!copyLaneRect.isValid())
        return QRect();

    const int diameter = qMin(copyLaneRect.width(), qMax(12, fontMetrics().height() - 2));
    const int left = copyLaneRect.left() + (copyLaneRect.width() - diameter) / 2;
    const int top = copyLaneRect.top() + (copyLaneRect.height() - diameter) / 2;
    return QRect(left, top, diameter, diameter);
}

QRect ResultDisplay::settingsGlyphRectForHistoryIndex(int historyIndex) const
{
    const QRect editRect = editGlyphRectForHistoryIndex(historyIndex);
    if (!editRect.isValid())
        return QRect();

    const int spacing = 4;
    const int left = editRect.left() - editRect.width() - spacing;
    if (left < 0)
        return QRect();

    return QRect(left, editRect.top(), editRect.width(), editRect.height());
}

QRect ResultDisplay::settingsGlyphBadgeRectForHistoryIndex(int historyIndex) const
{
    const QRect settingsLaneRect = settingsGlyphRectForHistoryIndex(historyIndex);
    if (!settingsLaneRect.isValid())
        return QRect();

    const int diameter = qMin(settingsLaneRect.width(), qMax(12, fontMetrics().height() - 2));
    const int left = settingsLaneRect.left() + (settingsLaneRect.width() - diameter) / 2;
    const int top = settingsLaneRect.top() + (settingsLaneRect.height() - diameter) / 2;
    return QRect(left, top, diameter, diameter);
}

QRect ResultDisplay::hoverActionRectForHistoryIndex(int historyIndex) const
{
    return copyGlyphRectForHistoryIndex(historyIndex)
        .united(settingsGlyphRectForHistoryIndex(historyIndex))
        .united(editGlyphRectForHistoryIndex(historyIndex))
        .united(removeGlyphRectForHistoryIndex(historyIndex));
}

QRect ResultDisplay::cancelGlyphBadgeRectForEditingIndex() const
{
    if (m_editingHistoryIndex < 0)
        return QRect();
    return removeGlyphBadgeRectForHistoryIndex(m_editingHistoryIndex);
}

bool ResultDisplay::historyBlockOverlapsSessionBadge(int historyIndex) const
{
    int startBlock = -1;
    int endBlock = -1;
    if (!blockRangeForHistoryIndex(historyIndex, startBlock, endBlock))
        return false;
    return false;
}

bool ResultDisplay::historyBlockOverlapsScrollToBottomButton(int historyIndex) const
{
    if (m_scrollToBottomButton == nullptr || !m_scrollToBottomButton->isVisible())
        return false;

    int startBlock = -1;
    int endBlock = -1;
    if (!blockRangeForHistoryIndex(historyIndex, startBlock, endBlock))
        return false;

    const QTextBlock start = document()->findBlockByNumber(startBlock);
    const QTextBlock end = document()->findBlockByNumber(endBlock);
    if (!start.isValid() || !end.isValid())
        return false;

    const QRectF startRect = blockBoundingGeometry(start).translated(contentOffset());
    const QRectF endRect = blockBoundingGeometry(end).translated(contentOffset());
    const QRect historyRect(0,
                            qRound(startRect.top()),
                            viewport()->width(),
                            qMax(1, qRound(endRect.bottom() - startRect.top())));
    const QRect buttonRect(viewport()->mapFrom(this, m_scrollToBottomButton->pos()),
                           m_scrollToBottomButton->size());
    return historyRect.intersects(buttonRect);
}

void ResultDisplay::updateHoverHighlightSelection()
{
    QList<QTextEdit::ExtraSelection> selections;
    if (m_hoveredHistoryIndex < 0 && m_editingHistoryIndex < 0) {
        setExtraSelections(selections);
        return;
    }

    const QColor hoverColor = m_hoverHighlightColor.isValid()
        ? m_hoverHighlightColor
        : hoverColorForBackground(themeSurfaceBackground());

    auto appendSelectionForHistoryIndex = [this, &selections, &hoverColor](int historyIndex) {
        int startBlock = -1;
        int endBlock = -1;
        if (!blockRangeForHistoryIndex(historyIndex, startBlock, endBlock))
            return;

        for (int blockNumber = startBlock; blockNumber <= endBlock; ++blockNumber) {
            QTextBlock block = document()->findBlockByNumber(blockNumber);
            if (!block.isValid())
                continue;

            QTextLayout* layout = block.layout();
            if (layout == nullptr)
                continue;

            const int lineCount = layout->lineCount();
            if (lineCount <= 0)
                continue;

            const int blockPosition = block.position();
            for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
                const QTextLine line = layout->lineAt(lineIndex);
                QTextEdit::ExtraSelection selection;
                selection.cursor = QTextCursor(document());
                selection.cursor.setPosition(blockPosition + line.textStart());
                selection.format.setProperty(QTextFormat::FullWidthSelection, true);
                selection.format.setBackground(hoverColor);
                selections.append(selection);
            }
        }
    };

    // Classic appearance suppresses the hover row background band, but keeps
    // m_hoveredHistoryIndex (and thus the hover action buttons/popup) working.
    // The editing-row highlight is unaffected.
    const bool showHoverBackground = !Settings::instance()->classicAppearance;
    if (m_hoverHighlightEnabled && m_hoveredHistoryIndex >= 0 && showHoverBackground)
        appendSelectionForHistoryIndex(m_hoveredHistoryIndex);
    if (m_editingHistoryIndex >= 0 && (!m_hoverHighlightEnabled || m_editingHistoryIndex != m_hoveredHistoryIndex))
        appendSelectionForHistoryIndex(m_editingHistoryIndex);

    setExtraSelections(selections);
}

QColor ResultDisplay::themeSurfaceBackground() const
{
    return m_themeSurfaceColor.isValid()
        ? m_themeSurfaceColor
        : m_highlighter->colorForRole(ColorScheme::Background);
}

void ResultDisplay::markHistoryBlockIndexCacheDirty()
{
    m_historyBlockIndexCacheDirty = true;
}

void ResultDisplay::markSimplifiedExpressionBlock(int blockNumber)
{
    QTextBlock block = document()->findBlockByNumber(blockNumber);
    if (!block.isValid())
        return;

    auto data = new SyntaxHighlightBlockData;
    data->highlightResultExpressionSyntax = true;
    block.setUserData(data);
    m_highlighter->rehighlightBlock(block);
}

void ResultDisplay::markSimplifiedExpressionBlocks()
{
    const Session* session = displaySession(this);
    if (session == nullptr)
        return;
    const int historySize = session->historySize();
    const int firstDisplayedHistoryIndex = firstDisplayedHistoryIndexForCount(historySize, session);

    QTextBlock block = document()->firstBlock();
    for (int i = firstDisplayedHistoryIndex; i < historySize && block.isValid(); ++i) {
        const HistoryEntry& entry = session->historyEntryAtRef(i);
        const QStringList renderedLines = renderedHistoryLinesForDisplay(entry, session->evaluator());
        const QString simplifiedLine = simplifiedExpressionLineForDisplay(entry, session->evaluator());
        int lineIndex = 0;

        while (block.isValid()) {
            const QString blockText = block.text();
            if (isSimplifiedExpressionRenderLine(renderedLines, lineIndex, simplifiedLine))
                markSimplifiedExpressionBlock(block.blockNumber());
            block = block.next();
            ++lineIndex;
            if (blockText.isEmpty())
                break;
        }
    }
}

void ResultDisplay::ensureHistoryBlockIndexCache() const
{
    if (!m_historyBlockIndexCacheDirty)
        return;

    m_blockToHistoryIndex.clear();
    m_historyBlockRanges.clear();

    const Session* session = displaySession(this);
    if (session == nullptr)
        return;
    const int historySize = session->historySize();
    const int firstDisplayedHistoryIndex = firstDisplayedHistoryIndexForCount(historySize, session);
    const int displayedHistoryCount = historySize - firstDisplayedHistoryIndex;
    m_historyBlockRanges.reserve(qMax(0, displayedHistoryCount));

    QTextBlock block = document()->firstBlock();
    for (int i = firstDisplayedHistoryIndex; i < historySize; ++i) {
        const int startBlock = block.isValid() ? block.blockNumber() : -1;
        int endBlock = -1;

        // Reconstruct ranges from the currently displayed text blocks so hover
        // hit-testing always matches what is visible, even after format mode
        // changes that intentionally do not rewrite history lines.
        while (block.isValid()) {
            const int blockNumber = block.blockNumber();
            if (block.text().isEmpty()) {
                // Preserve existing behavior: separator blocks map to this
                // history entry, but are excluded from highlight ranges.
                m_blockToHistoryIndex.append(i);
                block = block.next();
                break;
            }

            m_blockToHistoryIndex.append(i);
            endBlock = blockNumber;
            block = block.next();
        }

        m_historyBlockRanges.append(qMakePair(startBlock, endBlock));
    }

    // Keep the cache shape aligned with the rendered document even if there
    // are stale trailing blocks.
    while (block.isValid()) {
        m_blockToHistoryIndex.append(historySize > 0 ? historySize - 1 : -1);
        block = block.next();
    }

    m_historyBlockIndexCacheDirty = false;
}
