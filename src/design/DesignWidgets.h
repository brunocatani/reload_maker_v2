#pragma once

#include "design/Icons.h"

#include <QAbstractButton>
#include <QFrame>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QPushButton;

// Canvas building blocks shared by every page.
namespace reloadmaker::design
{
    // A panel's heading: a small section label over the panel title. Used as
    // the title bar of docks, replacing the plain dock caption.
    class PanelHeader final : public QWidget
    {
    public:
        explicit PanelHeader(const QString& section, const QString& title, QWidget* parent = nullptr);
        void setSection(const QString& section);
        void setTitle(const QString& title);

    private:
        QLabel* _section;
        QLabel* _title;
    };

    // The canvas segmented control: checkable buttons in one rounded frame,
    // exclusive through `group()`.
    class SegmentedControl final : public QFrame
    {
    public:
        explicit SegmentedControl(const QString& objectName, QWidget* parent = nullptr);
        QPushButton* addSegment(const QString& text, int id);
        [[nodiscard]] QButtonGroup* group() const { return _group; }

    private:
        QButtonGroup* _group;
        QHBoxLayout* _layout;
    };

    // One row of a checklist: a leading marker (a step number or a status
    // icon), a title, an optional second line and a trailing note.
    class ChecklistRow final : public QAbstractButton
    {
    public:
        enum class Marker
        {
            Number,
            Confirmed,
            Detected,
            NeedsYou,
            NotApplicable,
            Open,
        };
        explicit ChecklistRow(const QString& title, QWidget* parent = nullptr);
        void setSubtitle(const QString& subtitle);
        void setTrailing(const QString& trailing);
        void setMarker(Marker marker, int number = 0);
        // Item rows sit indented under their section, one line high.
        void setIndent(int pixels);
        void setTrailingColor(QRgb rgb);
        [[nodiscard]] QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent* event) override;
        void enterEvent(QEnterEvent* event) override;
        void leaveEvent(QEvent* event) override;

    private:
        QString _subtitle;
        QString _trailing;
        Marker _marker{ Marker::Number };
        int _indent{ 0 };
        QRgb _trailingColor{ 0 };
        int _number{ 0 };
        bool _hovered{ false };
    };

    // A square button: a large icon over a short label of up to two lines.
    // Actions use it plain or as the primary action; options and choices use
    // it checkable, shown selected while checked (owner, 2026-10-05: squared
    // icon buttons instead of lists and rectangular buttons).
    class IconTile final : public QAbstractButton
    {
    public:
        IconTile(Icon icon, const QString& text, QWidget* parent = nullptr);
        void setIcon(Icon icon);
        void setPrimary(bool primary);
        [[nodiscard]] QSize sizeHint() const override;
        [[nodiscard]] QSize minimumSizeHint() const override;

    protected:
        void paintEvent(QPaintEvent* event) override;
        void enterEvent(QEnterEvent* event) override;
        void leaveEvent(QEvent* event) override;

    private:
        Icon _icon;
        bool _primary{ false };
        bool _hovered{ false };
    };

    // An exclusive choice shown as a row of checkable tiles, used where a short
    // list of options was a drop-down. Indexes follow the order of addChoice;
    // group()'s idClicked reports a choice the person made, never one set by
    // setCurrentIndex.
    class TileChoice final : public QWidget
    {
    public:
        explicit TileChoice(QWidget* parent = nullptr);
        int addChoice(Icon icon, const QString& text, const QString& toolTip = {});
        void setCurrentIndex(int index);
        [[nodiscard]] int currentIndex() const;
        [[nodiscard]] QButtonGroup* group() const { return _group; }

    private:
        QButtonGroup* _group;
        QHBoxLayout* _layout;
        QVector<IconTile*> _tiles;
    };

    [[nodiscard]] QLabel* sectionLabel(const QString& text, QWidget* parent = nullptr);
    // A fold-out heading: a chevron button that shows or hides content,
    // which starts hidden.
    QPushButton* disclosure(const QString& text, QWidget* content, QWidget* parent);
    [[nodiscard]] QLabel* hintLabel(const QString& text, QWidget* parent = nullptr);
}
