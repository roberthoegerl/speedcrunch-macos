// SPDX-FileCopyrightText: 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later


#include "gui/displayfontdialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGridLayout>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const QString PreviewText = QStringLiteral("12 + 34 × 5 = 182");
const int StandardSizes[] = { 9, 10, 11, 12, 13, 14, 18, 24, 30, 36, 48, 64, 72, 96 };
const int SeparatorSpace = 9;
const int CornerRadius = 8;     // lists, fields and preview
const int RowRadius = 5;        // selection inside a list
const int ListPadding = 5;
const int TextIndent = 7;

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

QColor borderColor(const QPalette& palette)
{
    return withAlpha(palette.color(QPalette::Text), 0.14);
}

// Rows of the three lists, drawn as in current macOS lists: an inset, rounded
// selection in the accent colour while the list has focus, else a neutral one.
// Rows have one height and each name sits on the baseline of the list's font,
// whatever the ascent of the family it is drawn in. In the family list,
// "System Font" is followed by a separator line as in a menu.
class FontListDelegate : public QStyledItemDelegate {
public:
    FontListDelegate(QListWidget* list, bool separatorAfterFirst)
        : QStyledItemDelegate(list)
        , m_list(list)
        , m_separatorAfterFirst(separatorAfterFirst)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        // Measured with the list's own font, not the row's: rows stay even, and
        // sizing all families does not open hundreds of font files.
        const QFontMetrics metrics = m_list->fontMetrics();
        const int height = metrics.height() + 9;
        return QSize(metrics.averageCharWidth() * 8,
                     hasSeparator(index) ? height + SeparatorSpace : height);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        QRect row = item.rect;
        if (hasSeparator(index))
            row.setBottom(row.bottom() - SeparatorSpace);

        const QPalette& palette = m_list->palette();
        const bool selected = item.state & QStyle::State_Selected;
        const bool emphasized = selected && m_list->hasFocus() && m_list->isActiveWindow();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (selected) {
            QPainterPath path;
            path.addRoundedRect(QRectF(row), RowRadius, RowRadius);
            painter->fillPath(path, emphasized
                                  ? palette.color(QPalette::Active, QPalette::Highlight)
                                  : withAlpha(palette.color(QPalette::Text), 0.10));
        }

        const QFontMetrics listMetrics = m_list->fontMetrics();
        const int baseline =
            row.top() + (row.height() - listMetrics.height()) / 2 + listMetrics.ascent();
        const QRect textRect = row.adjusted(TextIndent, 0, -TextIndent, 0);
        painter->setFont(item.font);
        painter->setPen(emphasized ? palette.color(QPalette::Active, QPalette::HighlightedText)
                                   : palette.color(QPalette::Text));
        painter->drawText(textRect.left(), baseline,
                          QFontMetrics(item.font).elidedText(item.text, Qt::ElideRight,
                                                             textRect.width()));

        if (hasSeparator(index)) {
            const qreal y = item.rect.bottom() - SeparatorSpace / 2.0 + 0.5;
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setPen(borderColor(palette));
            painter->drawLine(QPointF(row.left() + TextIndent, y),
                              QPointF(row.right() - TextIndent, y));
        }
        painter->restore();
    }

private:
    bool hasSeparator(const QModelIndex& index) const
    {
        // Only while families are listed below it, i.e. not when a search hides them.
        if (!m_separatorAfterFirst || index.row() != 0)
            return false;
        for (int row = 1; row < m_list->count(); ++row) {
            if (!m_list->isRowHidden(row))
                return true;
        }
        return false;
    }

    QListWidget* m_list;
    bool m_separatorAfterFirst;
};

// Lists and fields share the preview's look: a rounded, hairline-bordered
// surface in the text background colour. Selection rows are painted by
// FontListDelegate. Colours come from the palette, so light and dark
// appearance and the accent colour follow the system.
QString listStyleSheet(const QPalette& palette)
{
    return QStringLiteral("QListWidget { background-color: %1; border: 1px solid %2;"
                          " border-radius: %3px; padding: %4px; outline: 0; }")
        .arg(palette.color(QPalette::Base).name(), borderColor(palette).name(QColor::HexArgb))
        .arg(CornerRadius)
        .arg(ListPadding - 1);
}

QString fieldStyleSheet(const QPalette& palette)
{
    // The focus border is one pixel wider; the padding gives it back, so the
    // text does not move when the field gains focus.
    return QStringLiteral("QLineEdit { background-color: %1; color: %2; border: 1px solid %3;"
                          " border-radius: %4px; padding: 3px 6px; }"
                          "QLineEdit:focus { border: 2px solid %5; padding: 2px 5px; }")
        .arg(palette.color(QPalette::Base).name(), palette.color(QPalette::Text).name(),
             borderColor(palette).name(QColor::HexArgb))
        .arg(CornerRadius - 2)
        .arg(withAlpha(palette.color(QPalette::Active, QPalette::Highlight), 0.6)
                 .name(QColor::HexArgb));
}

QIcon searchIcon(const QPalette& palette, qreal devicePixelRatio)
{
    const int size = 16;
    QPixmap pixmap(QSize(size, size) * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette.color(QPalette::PlaceholderText), 1.5, Qt::SolidLine,
                        Qt::RoundCap));
    painter.drawEllipse(QRectF(3, 3, 7.5, 7.5));
    painter.drawLine(QPointF(9.5, 9.5), QPointF(13, 13));
    return QIcon(pixmap);
}

QLabel* columnHeader(const QString& text, QWidget* parent)
{
    QLabel* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() - 2);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    return label;
}

} // namespace

// The sample in the selected font. Sizes that do not fit are shown scaled down,
// so the dialog keeps its size while the user browses.
class FontPreview : public QWidget {
public:
    using QWidget::QWidget;

    void setPreviewFont(const QFont& font)
    {
        m_font = font;
        update();
    }

    QSize sizeHint() const override { return QSize(400, 88); }
    QSize minimumSizeHint() const override { return QSize(200, 88); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(frame, CornerRadius, CornerRadius);
        painter.fillPath(path, palette().color(QPalette::Base));
        painter.setPen(borderColor(palette()));
        painter.drawPath(path);

        const QRectF area = QRectF(rect()).adjusted(12, 8, -12, -8);
        QFont font = m_font;
        const QFontMetricsF metrics(font);
        const qreal scale = std::min({ qreal(1), area.width() / metrics.horizontalAdvance(PreviewText),
                                       area.height() / metrics.height() });
        if (scale < 1.0)
            font.setPointSizeF(font.pointSizeF() * scale);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(area, Qt::AlignCenter, PreviewText);
    }

private:
    QFont m_font;
};

DisplayFontDialog::DisplayFontDialog(const QFont& current, QWidget* parent)
    : QDialog(parent)
    , m_preview(new FontPreview(this))
    , m_search(new QLineEdit(this))
    , m_families(new QListWidget(this))
    , m_typefaces(new QListWidget(this))
    , m_sizeEdit(new QLineEdit(this))
    , m_sizes(new QListWidget(this))
{
    setWindowTitle(tr("Display Font"));

    m_search->setPlaceholderText(tr("Search"));
    m_search->setClearButtonEnabled(true);

    // "System Font" first, then every installed family, each drawn in its own
    // typeface. Families starting with '.' are the platform's private UI fonts
    // (macOS hides them from users too); symbol fonts keep the list's font so
    // that their names stay readable.
    m_families->setItemDelegate(new FontListDelegate(m_families, true));
    const qreal listPointSize = m_families->font().pointSizeF();
    QListWidgetItem* systemItem = new QListWidgetItem(tr("System Font"), m_families);
    systemItem->setFont(systemFont(listPointSize));
    const QStringList families = QFontDatabase::families();
    for (const QString& family : families) {
        if (family.startsWith(QLatin1Char('.')) || QFontDatabase::isPrivateFamily(family))
            continue;
        QListWidgetItem* item = new QListWidgetItem(family, m_families);
        item->setData(Qt::UserRole, family);
        if (QFontDatabase::writingSystems(family).contains(QFontDatabase::Latin)) {
            QFont font(family);
            font.setPointSizeF(listPointSize);
            item->setFont(font);
        }
    }

    m_typefaces->setItemDelegate(new FontListDelegate(m_typefaces, false));
    m_sizes->setItemDelegate(new FontListDelegate(m_sizes, false));
    for (QListWidget* list : { m_families, m_typefaces, m_sizes }) {
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setAttribute(Qt::WA_MacShowFocusRect, false);
        // The selection is drawn in the accent colour only while the list has focus.
        list->installEventFilter(this);
    }
    m_search->addAction(searchIcon(palette(), devicePixelRatio()), QLineEdit::LeadingPosition);
    for (int size : StandardSizes)
        new QListWidgetItem(QString::number(size), m_sizes);
    m_sizeEdit->setValidator(new QIntValidator(MinimumPointSize, MaximumPointSize, m_sizeEdit));
    const int sizeColumnWidth = m_sizeEdit->fontMetrics().horizontalAdvance(QStringLiteral("0000")) + 44;
    m_sizeEdit->setFixedWidth(sizeColumnWidth);
    m_sizes->setFixedWidth(sizeColumnWidth);

    // Preselect the current font as it really is.
    QListWidgetItem* currentItem = systemItem;
    if (!isSystemFont(current)) {
        QList<QListWidgetItem*> matches = m_families->findItems(current.family(), Qt::MatchExactly);
        if (matches.isEmpty())
            matches = m_families->findItems(QFontInfo(current).family(), Qt::MatchExactly);
        if (!matches.isEmpty()) {
            currentItem = matches.first();
        } else {
            // Keep a family that is not (or no longer) installed selectable as is.
            currentItem = new QListWidgetItem(current.family(), m_families);
            currentItem->setData(Qt::UserRole, current.family());
        }
    }
    m_families->setCurrentItem(currentItem);
    populateTypefaces(QFontInfo(current).styleName());
    const int pointSize = qBound(MinimumPointSize, qRound(current.pointSizeF()), MaximumPointSize);
    m_sizeEdit->setText(QString::number(pointSize));
    selectSize(pointSize);

    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QGridLayout* columns = new QGridLayout;
    columns->setHorizontalSpacing(12);
    columns->setVerticalSpacing(8);
    columns->addWidget(columnHeader(tr("Family"), this), 0, 0);
    columns->addWidget(columnHeader(tr("Typeface"), this), 0, 1);
    columns->addWidget(columnHeader(tr("Size"), this), 0, 2);
    columns->addWidget(m_search, 1, 0);
    columns->addWidget(m_sizeEdit, 1, 2);
    columns->addWidget(m_families, 2, 0);
    columns->addWidget(m_typefaces, 1, 1, 2, 1);
    columns->addWidget(m_sizes, 2, 2);
    columns->setColumnStretch(0, 3);
    columns->setColumnStretch(1, 2);
    columns->setRowStretch(2, 1);

    applyColors();

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);
    layout->addWidget(m_preview);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);
    resize(600, 480);

    connect(m_search, &QLineEdit::textChanged, this, &DisplayFontDialog::filterFamilies);
    connect(m_families, &QListWidget::currentRowChanged, this, [this]() {
        const QListWidgetItem* typeface = m_typefaces->currentItem();
        populateTypefaces(typeface != nullptr ? typeface->text() : QString());
        updatePreview();
    });
    connect(m_typefaces, &QListWidget::currentRowChanged, this, &DisplayFontDialog::updatePreview);
    connect(m_sizes, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item != nullptr && item->text() != m_sizeEdit->text())
            m_sizeEdit->setText(item->text());
    });
    connect(m_sizeEdit, &QLineEdit::textChanged, this, [this]() {
        selectSize(currentPointSize());
        updatePreview();
    });

    // Arrow keys in the search field move through the families.
    m_search->installEventFilter(this);
    m_families->scrollToItem(currentItem, QAbstractItemView::PositionAtCenter);
    m_families->setFocus();
    updatePreview();
}

QFont DisplayFontDialog::selectedFont() const
{
    const int pointSize = currentPointSize();
    const QListWidgetItem* typeface = m_typefaces->currentItem();
    const QString style = typeface != nullptr ? typeface->text() : QString();

    if (systemFontSelected()) {
        QFont font = systemFont(pointSize);
        if (!style.isEmpty()) {
            const QString family = currentFamily();
            font.setWeight(QFont::Weight(QFontDatabase::weight(family, style)));
            font.setItalic(QFontDatabase::italic(family, style));
        }
        return font;
    }
    QFont font = QFontDatabase::font(currentFamily(), style, pointSize);
    font.setFamily(currentFamily());
    font.setPointSize(pointSize);
    return font;
}

bool DisplayFontDialog::systemFontSelected() const
{
    return m_families->currentRow() == 0;
}

QFont DisplayFontDialog::systemFont(qreal pointSize)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    if (pointSize > 0)
        font.setPointSizeF(pointSize);
    return font;
}

bool DisplayFontDialog::isSystemFont(const QFont& font)
{
    return QFontInfo(font).family() == QFontInfo(systemFont(-1)).family();
}

bool DisplayFontDialog::eventFilter(QObject* watched, QEvent* event)
{
    if ((event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        && qobject_cast<QListWidget*>(watched) != nullptr)
        static_cast<QListWidget*>(watched)->viewport()->update();
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp
            || key == Qt::Key_PageDown) {
            QCoreApplication::sendEvent(m_families, event);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void DisplayFontDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // Light/dark appearance or accent colour changed while the dialog is open.
    if (event->type() == QEvent::PaletteChange)
        applyColors();
}

void DisplayFontDialog::applyColors()
{
    const QPalette colors = palette();
    const QString lists = listStyleSheet(colors);
    for (QListWidget* list : { m_families, m_typefaces, m_sizes })
        list->setStyleSheet(lists);
    const QString fields = fieldStyleSheet(colors);
    m_search->setStyleSheet(fields);
    m_sizeEdit->setStyleSheet(fields);
    const QList<QAction*> actions = m_search->actions();
    if (!actions.isEmpty())
        actions.first()->setIcon(searchIcon(colors, devicePixelRatio()));
}

QString DisplayFontDialog::currentFamily() const
{
    if (systemFontSelected())
        return QFontInfo(systemFont(-1)).family();
    const QListWidgetItem* item = m_families->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
}

int DisplayFontDialog::currentPointSize() const
{
    bool ok = false;
    const int size = m_sizeEdit->text().toInt(&ok);
    return ok ? qBound(MinimumPointSize, size, MaximumPointSize) : MinimumPointSize;
}

void DisplayFontDialog::filterFamilies(const QString& text)
{
    const QString needle = text.trimmed();
    QListWidgetItem* firstVisible = nullptr;
    for (int row = 0; row < m_families->count(); ++row) {
        QListWidgetItem* item = m_families->item(row);
        const bool hidden = !item->text().contains(needle, Qt::CaseInsensitive);
        item->setHidden(hidden);
        if (!hidden && firstVisible == nullptr)
            firstVisible = item;
    }
    // The separator below "System Font" depends on what else is shown.
    m_families->doItemsLayout();

    QListWidgetItem* current = m_families->currentItem();
    if (current != nullptr && !current->isHidden())
        m_families->scrollToItem(current);
    else if (firstVisible != nullptr)
        m_families->setCurrentItem(firstVisible);
}

void DisplayFontDialog::populateTypefaces(const QString& preferredStyle)
{
    const QSignalBlocker blocker(m_typefaces);
    m_typefaces->clear();
    const QString family = currentFamily();
    QStringList styles = QFontDatabase::styles(family);
    if (styles.isEmpty())
        styles << QStringLiteral("Regular");
    m_typefaces->addItems(styles);

    // Keep the typeface across families where possible, else take the upright,
    // normal-weight one, as the font panel does.
    int row = styles.indexOf(preferredStyle);
    for (int i = 0; row < 0 && i < styles.size(); ++i) {
        if (QFontDatabase::weight(family, styles.at(i)) == QFont::Normal
            && !QFontDatabase::italic(family, styles.at(i)))
            row = i;
    }
    m_typefaces->setCurrentRow(qMax(row, 0));
}

void DisplayFontDialog::selectSize(int pointSize)
{
    const QSignalBlocker blocker(m_sizes);
    const QList<QListWidgetItem*> matches =
        m_sizes->findItems(QString::number(pointSize), Qt::MatchExactly);
    if (matches.isEmpty()) {
        m_sizes->clearSelection();
        m_sizes->setCurrentItem(nullptr);
        return;
    }
    m_sizes->setCurrentItem(matches.first());
    m_sizes->scrollToItem(matches.first());
}

void DisplayFontDialog::updatePreview()
{
    m_preview->setPreviewFont(selectedFont());
}
