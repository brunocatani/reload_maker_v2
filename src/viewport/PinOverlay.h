#pragma once

#include "SlotOverlay.h"
#include "viewport/DepthOverlay.h"

#include <QObject>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector3D>
#include <QVector>

#include <functional>
#include <optional>

class GLView;

namespace reloadmaker
{
    // A pin as the viewport draws it: its anchor shape in the scene and the
    // offset in that shape's frame.
    struct PinMarker
    {
        QString id;
        int number{ 0 };
        QString text;
        bool resolved{ false };
        QPersistentModelIndex shape;
        QVector3D offset;
    };

    // Draws pins after the scene: a marker depth-tested against the weapon and
    // hands (dim where they cover it) and, for open pins, a label card with the
    // note. Clicking a marker or card selects the pin; a right click asks for
    // its menu. A press on a pin owns the whole pointer sequence (the
    // SlotPointerGesture rule), so it never orbits or picks the part below.
    class PinOverlay final : public QObject
    {
    public:
        explicit PinOverlay(GLView* view, QObject* parent);

        void setMarkers(QVector<PinMarker> markers);
        void setActive(bool active);
        void setSelected(const QString& id);
        // The pin's anchor point in world space, or nothing when its shape is
        // not in the scene or is hidden.
        [[nodiscard]] std::optional<QVector3D> worldPosition(const PinMarker& marker) const;
        // Where the pin is on screen, in logical viewport pixels.
        [[nodiscard]] std::optional<QPointF> screenPosition(const PinMarker& marker) const;

        std::function<void(const QString&)> selected;
        std::function<void(const QString&, QPoint)> contextRequested;

    protected:
        bool eventFilter(QObject* target, QEvent* event) override;

    private:
        void paint();
        [[nodiscard]] QString pinAt(QPointF point) const;

        GLView* _view;
        QVector<PinMarker> _markers;
        QString _selected;
        bool _active{ false };
        DepthOverlay _overlay;
        SlotPointerGesture _gesture;
        QString _pressPin;
        Qt::MouseButton _pressButton{ Qt::NoButton };
        QVector<QPair<QString, QRectF>> _hits;  // from the latest frame, top first
    };
}
