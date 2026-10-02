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
        const bool enabled = index.flags() & Qt::ItemIsEnabled;
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const QRect button = option.rect.adjusted(4,3,-4,-3);
        painter->setPen(QPen(QColor(enabled&&focused?"#4c928b":"#344951"),1));
        painter->setBrush(QColor(enabled?(hovered?"#284347":"#203038"):"#19242a"));painter->drawRoundedRect(button,5,5);
        const QRect r=button.adjusted(12,8,-12,-8);
        QFont font = option.font; font.setPointSizeF(font.pointSizeF() * .85); font.setBold(true);
        painter->setFont(font);
        const auto caption=index.data(kCaptionRole).toString();const int captionWidth=QFontMetrics(font).horizontalAdvance(caption)+12;
        painter->setPen(QColor(enabled?"#9fd3c9":"#66777c"));
        painter->drawText(r,Qt::AlignLeft|Qt::AlignTop,QFontMetrics(font).elidedText(text.toUpper(),Qt::ElideRight,std::max(0,r.width()-captionWidth)));
        painter->drawText(r, Qt::AlignRight | Qt::AlignTop,caption);
        painter->setFont(option.font);painter->setPen(QColor(enabled?"#e2eeee":"#66777c"));
        painter->drawText(r,Qt::AlignLeft|Qt::AlignBottom,option.fontMetrics.elidedText(index.data(kSelectionRole).toString(),Qt::ElideRight,r.width()-18));
        if(enabled)painter->drawText(r,Qt::AlignRight|Qt::AlignBottom,QStringLiteral("›"));
        painter->restore();
        return;
    }
    const QRect r = option.rect.adjusted(4, 1, -4, -1);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool installed = index.data(kInstalledRole).toBool();
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    const bool none = index.data(kFormIdRole).toUInt() == 0;
    if (enabled && (selected || hovered || installed)) {
        painter->setPen(selected ? QPen(QColor("#e8c17c"), 1.5) : installed ? QPen(QColor("#4c928b"), 1.2) : Qt::NoPen);
        painter->setBrush(selected ? QColor("#493c27") : installed ? QColor("#1b3336") : QColor("#1d2a31"));
        painter->drawRoundedRect(r, 5, 5);
    }
    QRect textRect = r.adjusted(12, 0, -10, 0);
    if (selected || installed) {
        const QString badge=selected?(installed?QStringLiteral("Selected · Installed"):QStringLiteral("Selected")):QStringLiteral("Installed");
        QFont label=option.font;label.setPointSizeF(label.pointSizeF()*.8);painter->setFont(label);
        painter->setPen(QColor(selected?"#f6d89c":"#74c4b3"));
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,badge);
        textRect.adjust(0, 0, -(QFontMetrics(label).horizontalAdvance(badge)+16), 0);
    }
    QFont main = option.font;
    main.setWeight(selected || installed ? QFont::DemiBold : QFont::Normal);
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
    case kSlotHeader: return {option.rect.width(), line * 2 + 28};
    default: return {option.rect.width(), line + 12};
    }
}
}
