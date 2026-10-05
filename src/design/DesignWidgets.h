#pragma once

#include <QAbstractButton>
#include <QFrame>
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

    [[nodiscard]] QLabel* sectionLabel(const QString& text, QWidget* parent = nullptr);
    // A fold-out heading: a chevron button that shows or hides content,
    // which starts hidden.
    QPushButton* disclosure(const QString& text, QWidget* content, QWidget* parent);
    [[nodiscard]] QLabel* hintLabel(const QString& text, QWidget* parent = nullptr);
}
