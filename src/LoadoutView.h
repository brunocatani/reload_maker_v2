#pragma once
#include <QStyledItemDelegate>

namespace reloadmaker {
// Item roles of the flat loadout list. kFormIdRole is shared with the
// controller's other form-id lists.
constexpr int kFormIdRole = Qt::UserRole + 1;
constexpr int kCaptionRole = Qt::UserRole + 20;   // slot header right-hand note (option count)
constexpr int kInstalledRole = Qt::UserRole + 21; // option currently installed in its slot
constexpr int kSectionRole = Qt::UserRole + 22;   // row kind: 0 option, kCategoryHeader, kSlotHeader
constexpr int kFocusedRole = Qt::UserRole + 23;   // slot header of the slot highlighted in the viewport
constexpr int kSlotRole = Qt::UserRole + 24;      // slot key owning the row
constexpr int kSelectionRole = Qt::UserRole + 25; // installed choice on the slot button
constexpr int kCategoryHeader = 1;
constexpr int kSlotHeader = 2;

// Compact slot buttons in the workbench and option rows in its picker dialogs.
class LoadoutDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// Mod names commonly repeat their slot as a "[Barrel] " prefix; the loadout
// already shows the slot, so the prefix is dropped for display only.
[[nodiscard]] QString cleanPartName(const QString& name);
}
