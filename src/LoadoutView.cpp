#include "LoadoutView.h"
#include <QPainter>
#include <QRegularExpression>

namespace reloadmaker {
QString cleanPartName(const QString& name) {
    static const QRegularExpression prefix(QStringLiteral("^\\s*\\[[^\\]]*\\]\\s*"));
    QString result = name;
    result.remove(prefix);
    return result.trimmed().isEmpty() ? name.trimmed() : result.trimmed();
}

void LoadoutDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const int section = index.data(kSectionRole).toInt();
    const QString text = index.data(Qt::DisplayRole).toString();
    if (section == kCategoryHeader) {
        QFont font = option.font; font.setPointSizeF(font.pointSizeF() * 1.02); font.setBold(true); font.setLetterSpacing(QFont::PercentageSpacing, 112);
        painter->setFont(font); painter->setPen(QColor("#9fd3c9"));
        const QRect r = option.rect.adjusted(4, 0, -4, -6);
        painter->drawText(r, Qt::AlignLeft | Qt::AlignBottom, text.toUpper());
        painter->setPen(QPen(QColor("#2c4448"), 1));
        painter->drawLine(r.left(), r.bottom() + 3, r.right(), r.bottom() + 3);
        painter->restore();
        return;
    }
    if (section == kSlotHeader) {
        const bool focused = index.data(kFocusedRole).toBool();
        QFont font = option.font; font.setPointSizeF(font.pointSizeF() * .82); font.setBold(true); font.setLetterSpacing(QFont::PercentageSpacing, 110);
        painter->setFont(font);
        const QRect r = option.rect.adjusted(8, 0, -8, -2);
        if (focused) {
            painter->setPen(Qt::NoPen); painter->setBrush(QColor("#74c4b3"));
            painter->drawRoundedRect(QRect(r.left() - 6, r.bottom() - 13, 3, 12), 1.5, 1.5);
        }
        painter->setPen(QColor(focused ? "#bfe9df" : "#6f8f8c"));
        painter->drawText(r, Qt::AlignLeft | Qt::AlignBottom, text.toUpper());
        painter->setPen(QColor("#4f666a"));
        painter->drawText(r, Qt::AlignRight | Qt::AlignBottom, index.data(kCaptionRole).toString());
        painter->restore();
        return;
    }
    const QRect r = option.rect.adjusted(4, 1, -4, -1);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool installed = index.data(kInstalledRole).toBool();
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    const bool none = index.data(kFormIdRole).toUInt() == 0;
    if (enabled && (hovered || installed)) {
        painter->setPen(installed ? QPen(QColor("#4c928b"), 1.2) : Qt::NoPen);
        painter->setBrush(installed ? QColor("#1b3336") : QColor("#1d2a31"));
        painter->drawRoundedRect(r, 5, 5);
    }
    QRect textRect = r.adjusted(12, 0, -10, 0);
    if (installed) {
        painter->setPen(QColor("#74c4b3"));
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("✓"));
        textRect.adjust(0, 0, -18, 0);
    }
    QFont main = option.font;
    main.setWeight(installed ? QFont::DemiBold : QFont::Normal);
    main.setItalic(none);
    painter->setFont(main);
    painter->setPen(QColor(!enabled ? "#56646b" : installed ? "#f0ffff" : selected ? "#dce7e7" : none ? "#7f9699" : "#c4d2d3"));
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(main).elidedText(text, Qt::ElideRight, textRect.width()));
    painter->restore();
}

QSize LoadoutDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int line = option.fontMetrics.height();
    switch (index.data(kSectionRole).toInt()) {
    case kCategoryHeader: return {option.rect.width(), line * 2 + 12};
    case kSlotHeader: return {option.rect.width(), line + 12};
    default: return {option.rect.width(), line + 12};
    }
}
}
