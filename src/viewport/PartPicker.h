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
    // priority. Picking renders every visible shape offscreen through the native
    // renderer and resolves only part surfaces, so the hit is what is drawn:
    // a part behind the hands is not picked through them.
    class PartPicker final : public QObject
    {
    public:
        explicit PartPicker(GLView* view, QObject* parent);

        void setActive(bool active);
        void setParts(QVector<PartSurfaces> parts);
        [[nodiscard]] std::optional<SlotKey> partAt(QPointF point) const;

        // Left click selects, double-click opens the slot's choices and a right
        // click asks for the part's context menu at a global screen position.
        std::function<void(SlotKey)> selected;
        std::function<void(SlotKey)> activated;
        std::function<void(SlotKey, QPoint)> contextRequested;
        // Short description shown while hovering a part.
        std::function<QString(SlotKey)> describe;

    protected:
        bool eventFilter(QObject* target, QEvent* event) override;

    private:
        void showHoverDescription();

        GLView* _view;
        QHash<int, SlotKey> _slotByBlock;
        std::optional<QPointF> _press;
        Qt::MouseButton _pressButton{ Qt::NoButton };
        QPointF _hoverPoint;
        QTimer _hoverTimer;
        bool _active{ false };
    };
}
