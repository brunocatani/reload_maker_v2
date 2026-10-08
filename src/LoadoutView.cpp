#include "LoadoutView.h"
#include "design/Theme.h"
#include <QPainter>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace reloadmaker {
QString cleanPartName(const QString& name) {
    static const QRegularExpression prefix(QStringLiteral("^\\s*\\[[^\\]]*\\]\\s*"));
    QString result = name;
    result.remove(prefix);
    return result.trimmed().isEmpty() ? name.trimmed() : result.trimmed();
}

// Canvas loadout rows: a section label, then one row per slot with its
// current choice; the slot selected in the viewport is amber with its count.
void LoadoutDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    using namespace design;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const int section = index.data(kSectionRole).toInt();
    const QString text = index.data(Qt::DisplayRole).toString();
    if (section == kCategoryHeader) {
        QFont font = uiFont(kLabelPx, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        painter->setFont(font);
        painter->setPen(qcolor(color::section));
        painter->drawText(option.rect.adjusted(8, 0, -8, -6), Qt::AlignLeft | Qt::AlignBottom, text.toUpper());
        painter->restore();
        return;
    }
    if (section == kSlotHeader) {
        const bool focused = index.data(kFocusedRole).toBool();
        const bool enabled = index.flags() & Qt::ItemIsEnabled;
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const QRectF row = QRectF(option.rect).adjusted(0.5, 2.5, -0.5, -2.5);
        const bool amber = enabled && focused;
        painter->setPen(amber ? QPen(qcolor(color::selection), 1) : Qt::NoPen);
        painter->setBrush(qcolor(!enabled ? color::card : amber ? color::selectionFill : hovered ? color::rowHover : color::row));
        painter->drawRoundedRect(row, 6, 6);
        const QRect r = row.toRect().adjusted(10, 0, -10, 0);
        const QRect top = amber ? QRect(r.x(), r.y() + 7, r.width(), 20) : r;
        const QString choice = index.data(kSelectionRole).toString();
        painter->setFont(uiFont(kBodyPx, amber ? QFont::DemiBold : QFont::Normal));
        const QFontMetrics nameMetrics(painter->font());
        const int nameWidth = std::min(int(std::ceil(QFontMetricsF(painter->font()).horizontalAdvance(text))) + 2, int(top.width() * .6));
        painter->setPen(qcolor(enabled ? color::text : color::textDisabled));
        painter->drawText(QRect(top.x(), top.y(), nameWidth, top.height()), Qt::AlignLeft | Qt::AlignVCenter, nameMetrics.elidedText(text, Qt::ElideRight, nameWidth));
        painter->setFont(uiFont(kBodyPx));
        painter->setPen(qcolor(!enabled ? color::textDisabled : amber ? 0xfff0dcbc : color::textMuted));
        const QRect value(top.x() + nameWidth + 12, top.y(), top.width() - nameWidth - 12, top.height());
        painter->drawText(value, Qt::AlignRight | Qt::AlignVCenter, painter->fontMetrics().elidedText(choice, Qt::ElideRight, value.width()));
        if (amber) {
            painter->setFont(uiFont(kSmallPx));
            painter->setPen(qcolor(0xffe8d3b0));
            painter->drawText(QRect(r.x(), top.bottom() + 2, r.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
                              QStringLiteral("Selected in the viewport · %1").arg(index.data(kCaptionRole).toString()));
        }
        painter->restore();
        return;
    }
    const QRectF r = QRectF(option.rect).adjusted(0.5, 1.5, -0.5, -1.5);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool installed = index.data(kInstalledRole).toBool();
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    const bool none = index.data(kFormIdRole).toUInt() == 0;
    if (enabled && (selected || hovered || installed)) {
        painter->setPen(selected ? QPen(qcolor(color::selection), 1.2) : installed ? QPen(qcolor(color::accent), 1) : Qt::NoPen);
        painter->setBrush(qcolor(selected ? color::selectionFill : installed ? color::chipFill : color::rowHover));
        painter->drawRoundedRect(r, 6, 6);
    }
    QRect textRect = r.toRect().adjusted(12, 0, -10, 0);
    if (selected || installed) {
        const QString badge = selected ? (installed ? QStringLiteral("Selected · installed") : QStringLiteral("Selected")) : QStringLiteral("Installed");
        painter->setFont(uiFont(kSmallPx));
        painter->setPen(qcolor(selected ? 0xfff0dcbc : color::chipText));
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, badge);
        textRect.adjust(0, 0, -(painter->fontMetrics().horizontalAdvance(badge) + 16), 0);
    }
    QFont main = uiFont(kBodyPx, selected || installed ? QFont::DemiBold : QFont::Normal);
    main.setItalic(none);
    painter->setFont(main);
    painter->setPen(qcolor(!enabled ? color::textDisabled : selected || installed ? color::text : none ? color::textFaint : color::textSecondary));
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(main).elidedText(text, Qt::ElideRight, textRect.width()));
    painter->restore();
}

// Rows take the list's width and never ask for more: a wider hint made the
// list scroll sideways and pushed the values under its scroll bar.
QSize LoadoutDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const {
    switch (index.data(kSectionRole).toInt()) {
    case kCategoryHeader: return {0, 34};
    case kSlotHeader: return {0, index.data(kFocusedRole).toBool() && (index.flags() & Qt::ItemIsEnabled) ? 56 : 42};
    default: return {0, 36};
    }
}
}
