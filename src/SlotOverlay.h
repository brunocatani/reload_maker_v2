#pragma once

#include "WeaponConfiguration.h"
#include <QObject>
#include <QEvent>
#include <QPersistentModelIndex>
#include <QRectF>
#include <functional>

class GLView;

namespace reloadmaker
{
    // Pointer travel (logical pixels, Manhattan) below which a press and release
    // count as a click rather than a drag; GLView uses the same distance.
    inline constexpr int kClickThresholdPixels = 3;

    // A label owns the complete pointer sequence, not just its press/release.
    // Otherwise GLView's lost-press recovery sees a held-button move and latches
    // orbit, while the intercepted release never clears its button state.
    class SlotPointerGesture final
    {
    public:
        [[nodiscard]] bool captured() const { return _captured; }
        // captureButtons: the buttons whose press on a label starts the capture.
        bool consume(QEvent::Type type, Qt::MouseButton button,
                     Qt::MouseButtons buttons, bool labelHit = false,
                     Qt::MouseButtons captureButtons = Qt::LeftButton)
        {
            if (type == QEvent::FocusOut || type == QEvent::WindowDeactivate || type == QEvent::UngrabMouse) {
                _captured = false;
                return false;
            }
            if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
                if (!_captured && labelHit && captureButtons.testFlag(button)) { _captured = true; }
                return _captured;
            }
            if (!_captured) { return false; }
            if (type == QEvent::MouseMove || type == QEvent::MouseButtonRelease) {
                // Also recover when the OS reports no buttons after a lost release.
                if (buttons == Qt::NoButton) { _captured = false; }
                return true;
            }
            return false;
        }
    private:
        bool _captured{false};
    };

    struct ProjectedSlot { SlotKey slot; QPointF point; };
    struct PlacedSlot { SlotKey slot; QPointF point; QRectF card; bool above; };
    [[nodiscard]] QVector<PlacedSlot> layoutSlotLabels(QVector<ProjectedSlot> points,
                                                       QSizeF viewport, SlotKey selected);

    struct SlotCallout
    {
        SlotKey slot{0};
        QString label;
        QPersistentModelIndex anchor;
        QVector<QPersistentModelIndex> surfaces;
        QString detail;
        SlotKey owner{0};
        bool ownerAnchor{false};
    };

    // Paints after the existing renderer, in the same context. No second scene,
    // per-frame source reads, or input interception outside visible labels.
    class SlotOverlay final : public QObject
    {
    public:
        explicit SlotOverlay(GLView* view, QObject* parent);
        void setCallouts(QVector<SlotCallout> callouts);
        void setActive(bool active);
        void setSelected(SlotKey slot);
        std::function<void(SlotKey)> selected;

    protected:
        bool eventFilter(QObject* target, QEvent* event) override;

    private:
        void paint();
        GLView* _view;
        QVector<SlotCallout> _callouts;
        QVector<QPair<SlotKey, QRectF>> _hits;
        SlotKey _selected{0};
        bool _active{true};
        SlotPointerGesture _gesture;
    };
}
