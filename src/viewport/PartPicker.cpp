#include "viewport/PartPicker.h"

#include "SlotOverlay.h"
#include "glview.h"
#include "gl/glscene.h"
#include "gl/glshape.h"

#include <QMouseEvent>
#include <QToolTip>

namespace reloadmaker
{
    namespace
    {
        // Hover descriptions wait until the pointer rests; each pick renders the
        // part surfaces offscreen, which is too costly for every mouse move.
        constexpr int kHoverDelayMilliseconds = 250;
    }

    PartPicker::PartPicker(GLView* view, QObject* parent) : QObject(parent), _view(view)
    {
        _view->installEventFilter(this);
        _hoverTimer.setSingleShot(true);
        _hoverTimer.setInterval(kHoverDelayMilliseconds);
        connect(&_hoverTimer, &QTimer::timeout, this, [this] { showHoverDescription(); });
    }

    void PartPicker::setActive(bool active)
    {
        _active = active;
        _press.reset();
        _pressDouble = false;
        _hoverTimer.stop();
        if (!active) {
            QToolTip::hideText();
        }
    }

    void PartPicker::setParts(QVector<PartSurfaces> parts)
    {
        _slotByBlock.clear();
        for (const auto& part : parts) {
            for (const int block : part.blocks) {
                _slotByBlock.insert(block, part.slot);
            }
        }
    }

    std::optional<SlotKey> PartPicker::partAt(QPointF point) const
    {
        auto* scene = _view->getScene();
        if (_slotByBlock.isEmpty() || !scene) {
            return std::nullopt;
        }
        QVector<int> visible;
        for (const auto* shape : scene->shapes) {
            if (shape && !shape->isHidden()) {
                visible.append(shape->id());
            }
        }
        int block = -1;
        Vector3 world;
        if (!_view->reloadMakerSurfaceAt(point, visible, block, world)) {
            return std::nullopt;
        }
        const auto found = _slotByBlock.constFind(block);
        if (found == _slotByBlock.cend()) {
            return std::nullopt;
        }
        return found.value();
    }

    void PartPicker::click(QPointF point)
    {
        if (!_active || !selected) {
            return;
        }
        if (const auto slot = partAt(point)) {
            selected(*slot);
        }
    }

    // Called when the second click of a double click is released; the first
    // click has already selected the part.
    void PartPicker::doubleClick(QPointF point)
    {
        if (!_active || !activated) {
            return;
        }
        if (const auto slot = partAt(point)) {
            // Dialogs run a nested event loop. They open after the viewport has
            // finished handling this event, so its button state is never latched.
            QTimer::singleShot(0, this, [this, slot = *slot] { activated(slot); });
        }
    }

    // Never consumes events: the camera and the native viewport keep their own
    // handling of every press, drag and release.
    bool PartPicker::eventFilter(QObject* target, QEvent* event)
    {
        if (target != _view || !_active) {
            return QObject::eventFilter(target, event);
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            _hoverTimer.stop();
            QToolTip::hideText();
            if (mouse->button() == Qt::LeftButton || mouse->button() == Qt::RightButton) {
                _press = mouse->position();
                _pressButton = mouse->button();
                _pressDouble = false;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const bool completed = _press && mouse->button() == _pressButton &&
                                   (mouse->position() - *_press).manhattanLength() <= kClickThresholdPixels;
            const bool twice = _pressDouble;
            _press.reset();
            _pressDouble = false;
            if (!completed) {
                break;
            }
            if (mouse->button() == Qt::LeftButton) {
                if (twice) {
                    doubleClick(mouse->position());
                } else {
                    click(mouse->position());
                }
                break;
            }
            const auto slot = partAt(mouse->position());
            if (slot && contextRequested) {
                // Menus and dialogs run a nested event loop. They open after the
                // viewport has finished handling this release, so its button
                // state never stays latched behind them.
                const auto position = mouse->globalPosition().toPoint();
                QTimer::singleShot(0, this, [this, slot = *slot, position] { contextRequested(slot, position); });
            }
            break;
        }
        case QEvent::MouseButtonDblClick: {
            // A window receives the second press of a double click as a press
            // followed by this event. It only marks the gesture: the action runs
            // on release, after the viewport has seen the matching release.
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                _press = mouse->position();
                _pressButton = Qt::LeftButton;
                _pressDouble = true;
            }
            break;
        }
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->buttons() == Qt::NoButton) {
                _hoverPoint = mouse->position();
                _hoverTimer.start();
            }
            break;
        }
        case QEvent::Leave:
        case QEvent::FocusOut:
            _press.reset();
            _hoverTimer.stop();
            QToolTip::hideText();
            break;
        default:
            break;
        }
        return QObject::eventFilter(target, event);
    }

    void PartPicker::showHoverDescription()
    {
        if (!_active || !describe) {
            return;
        }
        const auto slot = partAt(_hoverPoint);
        if (!slot) {
            QToolTip::hideText();
            return;
        }
        QToolTip::showText(_view->mapToGlobal(_hoverPoint).toPoint(), describe(*slot));
    }
}
