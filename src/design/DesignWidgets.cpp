#include "design/DesignWidgets.h"
#include "design/Icons.h"
#include "design/Theme.h"

#include <QButtonGroup>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace reloadmaker::design
{
    PanelHeader::PanelHeader(const QString& section, const QString& title, QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("DockHeader"));
        setAttribute(Qt::WA_StyledBackground);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(2);
        _section = sectionLabel(section, this);
        _section->setStyleSheet(QStringLiteral("padding-top:0;"));
        _title = new QLabel(title, this);
        _title->setObjectName(QStringLiteral("PanelTitle"));
        _title->setWordWrap(true);
        layout->addWidget(_section);
        layout->addWidget(_title);
        setSection(section);
        setTitle(title);
    }

    void PanelHeader::setSection(const QString& section)
    {
        _section->setText(section.toUpper());
        _section->setVisible(!section.isEmpty());
    }

    void PanelHeader::setTitle(const QString& title)
    {
        _title->setText(title);
        _title->setVisible(!title.isEmpty());
    }

    namespace
    {
        constexpr int kTileHeight = 84;
        constexpr int kTileIconPx = 32;
        constexpr qreal kTileRadius = 8;
    }

    IconTile::IconTile(Icon icon, const QString& text, QWidget* parent) : QAbstractButton(parent), _icon(icon)
    {
        setText(text);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void IconTile::setIcon(Icon icon)
    {
        _icon = icon;
        update();
    }

    void IconTile::setPrimary(bool primary)
    {
        _primary = primary;
        update();
    }

    QSize IconTile::sizeHint() const
    {
        return { 84, kTileHeight };
    }

    QSize IconTile::minimumSizeHint() const
    {
        return { 64, kTileHeight };
    }

    void IconTile::enterEvent(QEnterEvent* event)
    {
        _hovered = true;
        update();
        QAbstractButton::enterEvent(event);
    }

    void IconTile::leaveEvent(QEvent* event)
    {
        _hovered = false;
        update();
        QAbstractButton::leaveEvent(event);
    }

    void IconTile::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool enabled = isEnabled();
        const bool lit = (_hovered || isDown()) && enabled;
        QRgb fill = lit ? color::buttonHover : color::button;
        QRgb border = color::buttonBorder;
        QRgb ink = color::text;
        if (!enabled) {
            // A choice that cannot change still shows which one holds.
            fill = isChecked() ? color::row : color::card;
            border = isChecked() ? color::section : color::border;
            ink = isChecked() ? color::textMuted : color::textDisabled;
        } else if (_primary) {
            fill = lit ? color::primaryHover : color::primary;
            border = fill;
            ink = color::onPrimary;
        } else if (isChecked()) {
            fill = color::selected;
            border = color::accent;
            ink = color::onPrimary;
        }
        painter.setPen(QPen(qcolor(border), isChecked() ? 1.5 : 1));
        painter.setBrush(qcolor(fill));
        painter.drawRoundedRect(frame, kTileRadius, kTileRadius);
        if (hasFocus()) {
            painter.setPen(QPen(qcolor(color::accent), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(frame.adjusted(2, 2, -2, -2), kTileRadius - 2, kTileRadius - 2);
        }
        const QRgb iconColor = !enabled ? ink : isChecked() && !_primary ? color::accent : ink;
        const QRect iconRect(int((width() - kTileIconPx) / 2), 10, kTileIconPx, kTileIconPx);
        design::icon(_icon, iconColor).paint(&painter, iconRect, Qt::AlignCenter, QIcon::Normal);
        painter.setPen(qcolor(ink));
        painter.setFont(uiFont(kSmallPx, isChecked() || _primary ? QFont::DemiBold : QFont::Normal));
        const QRectF label(4, iconRect.bottom() + 5, width() - 8, height() - iconRect.bottom() - 8);
        // Two lines at most; a longer label is elided on the second.
        const QFontMetricsF metrics(painter.font());
        QString shown = text();
        if (metrics.horizontalAdvance(shown) > label.width()) {
            const int cut = shown.lastIndexOf(' ', int(shown.size() * 0.6));
            if (cut > 0) {
                shown = shown.left(cut) + '\n' + metrics.elidedText(shown.mid(cut + 1), Qt::ElideRight, label.width());
            }
        }
        painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop, shown);
    }

    TileChoice::TileChoice(QWidget* parent) : QWidget(parent), _group(new QButtonGroup(this))
    {
        _group->setExclusive(true);
        _layout = new QHBoxLayout(this);
        _layout->setContentsMargins(0, 0, 0, 0);
        _layout->setSpacing(8);
    }

    int TileChoice::addChoice(Icon icon, const QString& text, const QString& toolTip)
    {
        auto* tile = new IconTile(icon, text, this);
        tile->setCheckable(true);
        tile->setToolTip(toolTip);
        const int index = int(_tiles.size());
        _group->addButton(tile, index);
        _layout->addWidget(tile);
        _tiles.append(tile);
        return index;
    }

    void TileChoice::setCurrentIndex(int index)
    {
        if (index >= 0 && index < _tiles.size()) {
            _tiles[index]->setChecked(true);
        }
    }

    int TileChoice::currentIndex() const
    {
        return _group->checkedId();
    }

    SegmentedControl::SegmentedControl(const QString& objectName, QWidget* parent) : QFrame(parent), _group(new QButtonGroup(this))
    {
        setObjectName(objectName);
        _layout = new QHBoxLayout(this);
        _layout->setContentsMargins(3, 3, 3, 3);
        _layout->setSpacing(2);
    }

    QPushButton* SegmentedControl::addSegment(const QString& text, int id)
    {
        auto* button = new QPushButton(text, this);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        _group->addButton(button, id);
        _layout->addWidget(button);
        return button;
    }

    ChecklistRow::ChecklistRow(const QString& title, QWidget* parent) : QAbstractButton(parent)
    {
        setText(title);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
    }

    void ChecklistRow::setSubtitle(const QString& subtitle)
    {
        _subtitle = subtitle;
        updateGeometry();
        update();
    }

    void ChecklistRow::setTrailing(const QString& trailing)
    {
        _trailing = trailing;
        update();
    }

    void ChecklistRow::setMarker(Marker marker, int number)
    {
        _marker = marker;
        _number = number;
        update();
    }

    void ChecklistRow::setIndent(int pixels)
    {
        _indent = pixels;
        updateGeometry();
        update();
    }

    void ChecklistRow::setTrailingColor(QRgb rgb)
    {
        _trailingColor = rgb;
        update();
    }

    QSize ChecklistRow::sizeHint() const
    {
        return { 240, !_subtitle.isEmpty() ? 52 : _indent > 0 ? 32 : 38 };
    }

    void ChecklistRow::enterEvent(QEnterEvent* event)
    {
        _hovered = true;
        update();
        QAbstractButton::enterEvent(event);
    }

    void ChecklistRow::leaveEvent(QEvent* event)
    {
        _hovered = false;
        update();
        QAbstractButton::leaveEvent(event);
    }

    void ChecklistRow::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (isChecked() || _hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(qcolor(isChecked() ? color::selected : color::rowHover));
            painter.drawRoundedRect(frame, 6, 6);
        }
        if (hasFocus()) {
            painter.setPen(QPen(qcolor(color::accent), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(frame, 6, 6);
        }
        const bool enabled = isEnabled();
        const qreal markerX = 10 + _indent;
        const qreal centerY = height() / 2.0;
        const QRectF marker(markerX, centerY - 9, 18, 18);
        if (_marker == Marker::Number) {
            painter.setPen(QPen(qcolor(isChecked() ? color::accent : color::textFaint), 1.4));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(marker.adjusted(1, 1, -1, -1));
            painter.setFont(uiFont(kLabelPx, QFont::DemiBold));
            painter.setPen(qcolor(isChecked() ? color::onPrimary : color::textSecondary));
            painter.drawText(marker, Qt::AlignCenter, QString::number(_number));
        } else {
            const auto [name, rgb] = [this]() -> std::pair<Icon, QRgb> {
                switch (_marker) {
                case Marker::Confirmed: return { Icon::Check, color::done };
                case Marker::Detected: return { Icon::DotCircle, color::detected };
                case Marker::NeedsYou: return { Icon::Diamond, color::needsYou };
                case Marker::NotApplicable: return { Icon::Minus, color::textFaint };
                default: return { Icon::Circle, color::textFaint };
                }
            }();
            design::icon(name, rgb).paint(&painter, marker.adjusted(1, 1, -1, -1).toRect(), Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
        }
        const qreal textX = markerX + 18 + 10;
        const qreal trailingWidth = _trailing.isEmpty() ? 0 : QFontMetricsF(uiFont(kSmallPx)).horizontalAdvance(_trailing) + 12;
        const QRectF textArea(textX, 0, width() - textX - trailingWidth - 10, height());
        painter.setFont(uiFont(_indent > 0 ? kSmallPx + 1 : kBodyPx, isChecked() ? QFont::DemiBold : QFont::Normal));
        painter.setPen(qcolor(enabled ? color::text : color::textDisabled));
        if (_subtitle.isEmpty()) {
            painter.drawText(textArea, Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(text(), Qt::ElideRight, int(textArea.width())));
        } else {
            const QRectF top(textArea.x(), centerY - 19, textArea.width(), 20);
            const QRectF bottom(textArea.x(), centerY + 1, textArea.width(), 18);
            painter.drawText(top, Qt::AlignBottom | Qt::AlignLeft, painter.fontMetrics().elidedText(text(), Qt::ElideRight, int(top.width())));
            painter.setFont(uiFont(kSmallPx));
            painter.setPen(qcolor(enabled ? color::textMuted : color::textDisabled));
            painter.drawText(bottom, Qt::AlignTop | Qt::AlignLeft, painter.fontMetrics().elidedText(_subtitle, Qt::ElideRight, int(bottom.width())));
        }
        if (!_trailing.isEmpty()) {
            painter.setFont(uiFont(kSmallPx));
            painter.setPen(qcolor(_trailingColor ? _trailingColor : color::textMuted));
            painter.drawText(QRectF(width() - trailingWidth - 10, 0, trailingWidth, height()), Qt::AlignVCenter | Qt::AlignRight, _trailing);
        }
    }

    QLabel* sectionLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        label->setObjectName(QStringLiteral("Section"));
        return label;
    }

    QPushButton* disclosure(const QString& text, QWidget* content, QWidget* parent)
    {
        auto* button = new QPushButton(text, parent);
        button->setObjectName(QStringLiteral("Disclosure"));
        button->setCheckable(true);
        button->setIcon(icon(Icon::ChevronRight, color::textMuted));
        content->hide();
        QObject::connect(button, &QPushButton::toggled, content, [content, button](bool open) {
            content->setVisible(open);
            button->setIcon(icon(open ? Icon::ChevronDown : Icon::ChevronRight, color::textMuted));
        });
        return button;
    }

    QLabel* hintLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text, parent);
        label->setObjectName(QStringLiteral("Hint"));
        label->setWordWrap(true);
        return label;
    }
}
