#pragma once

#include "WeaponConfiguration.h"

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>

class GLView;

namespace reloadmaker
{
    // The rendered mesh blocks that one installed loadout slot contributed.
    struct PartSurfaces
    {
        SlotKey slot{ 0 };
        QVector<int> blocks;
    };

    // Turns clicks on the rendered weapon into loadout slot selections.
    //
    // A press that moves further than the native click threshold stays a camera
    // drag. Presses consumed by tools installed later (operating-part drags,
    // grip handles, slot labels) never reach this filter, so those tools keep
    // priority. The Operate drive captures every weapon-surface press so a drag
    // there does not orbit; it hands back clicks on parts it does not move
    // through click() and doubleClick(). Picking renders every visible shape offscreen through the native
    // renderer and resolves only part surfaces, so the hit is what is drawn:
    // a part behind the hands is not picked through them.
    class PartPicker final : public QObject
    {
    public:
        explicit PartPicker(GLView* view, QObject* parent);

        void setActive(bool active);
        void setParts(QVector<PartSurfaces> parts);
        // The focused part, outlined in the selection colour while picking is
        // active. A part under the resting pointer gets a lighter outline.
        void setSelected(std::optional<SlotKey> slot);
        [[nodiscard]] std::optional<SlotKey> partAt(QPointF point) const;
        // A completed click at a viewport point. Also called by tools that
        // capture weapon-surface presses to block orbit but do not use them.
        void click(QPointF point);
        void doubleClick(QPointF point);

        // Left click selects, double-click opens the slot's choices and a right
        // click asks for the part's context menu at a global screen position;
        // it also passes the viewport point that was clicked.
        std::function<void(SlotKey)> selected;
        std::function<void(SlotKey)> activated;
        std::function<void(SlotKey, QPoint, QPointF)> contextRequested;
        // Short description shown while hovering a part.
        std::function<QString(SlotKey)> describe;

    protected:
        bool eventFilter(QObject* target, QEvent* event) override;

    private:
        void showHoverDescription();
        void setHovered(std::optional<SlotKey> slot);
        void refreshHighlights();

        GLView* _view;
        QHash<int, SlotKey> _slotByBlock;
        QHash<SlotKey, QVector<int>> _blocksBySlot;
        std::optional<SlotKey> _selected;
        std::optional<SlotKey> _hovered;
        std::optional<QPointF> _press;
        Qt::MouseButton _pressButton{ Qt::NoButton };
        bool _pressDouble{ false };
        QPointF _hoverPoint;
        QTimer _hoverTimer;
        bool _active{ false };
    };
}
