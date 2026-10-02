// SPDX-FileCopyrightText: 2026 SpeedCrunch developers
// SPDX-License-Identifier: GPL-2.0-or-later


#ifndef GUI_DISPLAYFONTDIALOG_H
#define GUI_DISPLAYFONTDIALOG_H

#include <QDialog>
#include <QFont>

class FontPreview;
class QLineEdit;
class QListWidget;

// Chooses the display font, laid out like the macOS font panel: a preview on
// top, then Family, Typeface and Size columns. The family list starts with
// "System Font" (the platform UI font) and shows every family in its own
// typeface. Replaces the native font panel, which on macOS cannot list (nor
// preselect) the system font San Francisco, and then pretends its first family
// is the current font.
class DisplayFontDialog : public QDialog {
    Q_OBJECT

public:
    explicit DisplayFontDialog(const QFont& current, QWidget* parent = nullptr);

    QFont selectedFont() const;
    bool systemFontSelected() const;

    static QFont systemFont(qreal pointSize);
    static bool isSystemFont(const QFont& font);

    static constexpr int MinimumPointSize = 8;   // ResultDisplay zoom limits
    static constexpr int MaximumPointSize = 96;

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyColors();
    QString currentFamily() const;
    int currentPointSize() const;
    void filterFamilies(const QString& text);
    void populateTypefaces(const QString& preferredStyle);
    void selectSize(int pointSize);
    void updatePreview();

    FontPreview* m_preview;
    QLineEdit* m_search;
    QListWidget* m_families;
    QListWidget* m_typefaces;
    QLineEdit* m_sizeEdit;
    QListWidget* m_sizes;
};

#endif // GUI_DISPLAYFONTDIALOG_H
