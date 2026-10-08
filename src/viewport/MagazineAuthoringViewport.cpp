#include "viewport/MagazineAuthoringViewport.h"

#include "design/Theme.h"
#include "glview.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // Screen length of the handle arrows, as in the grip handles.
        constexpr float kArrowPixels = 76.f;
        constexpr float kHitPixels = 9.f;
        constexpr float kBoxPixels = 7.f;
        // Ctrl+drag length step, in game units, and Shift+drag precision.
        constexpr float kSnapUnits = .5f;
        constexpr float kPrecision = .1f;
        // Smallest lengths a drag may produce; the controller applies the
        // guide's own limits (pickup and release below the travel).
        constexpr float kMinimumTravel = .1f;
        constexpr float kMinimumMargin = .05f;
        // Opacity of guide parts behind the weapon or the hands.
        constexpr float kHiddenAlpha = .45f;
        constexpr int kRingSegments = 48;

        float snapped(float value) { return std::round(value / kSnapUnits) * kSnapUnits; }

        float lineDistance(QPointF p, QPointF a, QPointF b)
        {
            const auto d = b - a;
            const auto n = d.x() * d.x() + d.y() * d.y();
            if (n < .01) {
                return float(QLineF(p, a).length());
            }
            const auto t = std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / n, 0., 1.);
            return float(QLineF(p, a + d * t).length());
        }

        QPolygonF box(QPointF center)
        {
            const QRectF r(center - QPointF(kBoxPixels, kBoxPixels), QSizeF(2 * kBoxPixels, 2 * kBoxPixels));
            return { r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft() };
        }

        const QColor kLine("#6cc7b4");
        const QColor kTravel("#6ba5e8");
        const QColor kSwingA("#ef7468");
        const QColor kSwingB("#9cca75");
        const QColor kEnd("#efcf99");
        const QColor kRelease("#f0a35e");
    }

    MagazineAuthoringViewport::MagazineAuthoringViewport(GLView* view, QObject* parent) : QObject(parent), _view(view)
    {
        _view->installEventFilter(this);
        connect(_view, &GLView::paintUpdate, this, [this] { paint(); });
    }

    void MagazineAuthoringViewport::setMode(Mode mode)
    {
        if (_mode == mode) {
            return;
        }
        if (_drag) {
            finishDrag(true);
        }
        _mode = mode;
        _press.reset();
        _handles.clear();
        refreshHighlights();
        _view->update();
    }

    void MagazineAuthoringViewport::setPieces(QVector<MagazinePieceMark> pieces)
    {
        _pieces = std::move(pieces);
        refreshHighlights();
    }

    void MagazineAuthoringViewport::setGuide(std::optional<GuideShape> guide)
    {
        _guide = guide;
        if (!_guide && _drag) {
            _drag.reset();
        }
        _view->update();
    }

    // Pieces are outlined in their role colour, the selected one thicker in
    // the selection colour. The outlines are cleared once when the step
    // ends, so the loadout's part outlines take over again.
    void MagazineAuthoringViewport::refreshHighlights()
    {
        QVector<GLView::ReloadMakerHighlight> highlights;
        if (_mode == Mode::Pieces) {
            for (const auto& piece : _pieces) {
                if (!piece.selected) {
                    highlights.append({ { piece.block }, piece.color, 2.5f, .12f });
                }
            }
            for (const auto& piece : _pieces) {
                if (piece.selected) {
                    highlights.append({ { piece.block }, QColor::fromRgba(design::color::selection), 4.f, .18f });
                }
            }
        }
        if (highlights.isEmpty() && !_highlighted) {
            return;
        }
        _highlighted = !highlights.isEmpty();
        _view->reloadMakerSetHighlights(std::move(highlights));
    }

    // Two directions across the guide line, stable while it turns slowly.
    void MagazineAuthoringViewport::basis(QVector3D& a, QVector3D& b) const
    {
        const auto u = _guide->outward.normalized();
        a = QVector3D::crossProduct(u, QVector3D(0, 0, 1));
        if (a.lengthSquared() < .01f) {
            a = QVector3D::crossProduct(u, QVector3D(0, 1, 0));
        }
        a.normalize();
        b = QVector3D::crossProduct(u, a).normalized();
    }

    bool MagazineAuthoringViewport::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched != _view || _mode == Mode::Off) {
            return false;
        }
        const auto type = event->type();
        if (blocked && blocked() && !_gesture.captured()) {
            return false;
        }
        if (type == QEvent::FocusOut || type == QEvent::WindowDeactivate || type == QEvent::UngrabMouse) {
            if (_drag) {
                finishDrag(true);
            }
            _press.reset();
            (void)_gesture.consume(type, Qt::NoButton, Qt::NoButton);
            return false;
        }
        if (type == QEvent::KeyPress && _drag && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishDrag(true);
            return true;
        }
        if (type == QEvent::Wheel) {
            return _gesture.captured();
        }
        if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) {
            return false;
        }
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto position = mouse->position();
        const bool press = (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) && mouse->button() == Qt::LeftButton;

        if (_mode == Mode::Pieces) {
            // Clicks select a piece; anything that moves stays a camera drag.
            if (press) {
                _press = position;
            } else if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton && _press) {
                const bool click = (position - *_press).manhattanLength() <= kClickThresholdPixels;
                _press.reset();
                QVector<int> blocks;
                for (const auto& piece : _pieces) {
                    blocks.append(piece.block);
                }
                int block = -1;
                Vector3 world;
                if (click && !blocks.isEmpty() && _view->reloadMakerSurfaceAt(position, blocks, block, world) && pieceClicked) {
                    pieceClicked(block);
                }
            }
            return false;
        }

        int hit = -1;
        if (press && !_gesture.captured() && _guide) {
            float nearest = kHitPixels;
            for (int i = 0; i < _handles.size(); ++i) {
                for (int segment = 1; segment < _handles[i].line.size(); ++segment) {
                    const float d = lineDistance(position, _handles[i].line[segment - 1], _handles[i].line[segment]);
                    if (d < nearest) {
                        nearest = d;
                        hit = i;
                    }
                }
            }
        }
        if (!_gesture.consume(type, mouse->button(), mouse->buttons(), hit >= 0)) {
            return false;
        }
        if (press && hit >= 0) {
            _dragPrecision = mouse->modifiers().testFlag(Qt::ShiftModifier) ? kPrecision : 1.f;
            (void)beginDrag(_handles[hit], position);
        } else if (type == QEvent::MouseMove && _drag && mouse->buttons().testFlag(Qt::LeftButton)) {
            drag(position, mouse->modifiers().testFlag(Qt::ControlModifier));
        }
        if ((type == QEvent::MouseButtonRelease || mouse->buttons() == Qt::NoButton) && _drag) {
            finishDrag(false);
        }
        return true;
    }

    bool MagazineAuthoringViewport::beginDrag(const HandleShape& handle, QPointF point)
    {
        _dragCamera = _view->reloadMakerWorldToClip();
        _dragSize = QSizeF(_view->width(), _view->height());
        GripRay ray;
        if (!_guide || !gripRay(_dragCamera, _dragSize, point, ray)) {
            return false;
        }
        const auto& g = *_guide;
        const auto end = g.seated + g.outward * g.travel;
        QVector3D a, b;
        basis(a, b);
        _dragAxis = handle.direction;
        _dragPivot = handle.handle == Handle::Pickup    ? end - b * g.pickup
                     : handle.handle == Handle::Release ? end + g.outward * g.release
                                                        : end;
        if (handle.handle == Handle::End) {
            GripRay center;
            if (!gripRay(_dragCamera, _dragSize, { _dragSize.width() / 2, _dragSize.height() / 2 }, center)) {
                return false;
            }
            _dragAxis = center.direction;
            if (!gripPlanePoint(ray, _dragPivot, _dragAxis, _dragPlane)) {
                return false;
            }
        } else if (!gripAxisParameter(ray, _dragPivot, _dragAxis, _dragParameter)) {
            return false;
        }
        _dragStart = g;
        _drag = handle.handle;
        return true;
    }

    void MagazineAuthoringViewport::drag(QPointF point, bool snap)
    {
        GripRay ray;
        if (!_drag || !gripRay(_dragCamera, _dragSize, point, ray)) {
            return;
        }
        auto shape = _dragStart;
        const auto startEnd = _dragStart.seated + _dragStart.outward * _dragStart.travel;
        const auto aim = [&](QVector3D end) {
            const auto line = end - shape.seated;
            if (line.length() < kMinimumTravel) {
                return;
            }
            shape.outward = line.normalized();
            shape.travel = snap ? std::max(kMinimumTravel, snapped(line.length())) : line.length();
        };
        if (*_drag == Handle::End) {
            QVector3D plane;
            if (!gripPlanePoint(ray, _dragPivot, _dragAxis, plane)) {
                return;
            }
            auto delta = (plane - _dragPlane) * _dragPrecision;
            if (snap) {
                delta = { snapped(delta.x()), snapped(delta.y()), snapped(delta.z()) };
            }
            aim(startEnd + delta);
        } else {
            float parameter = 0;
            if (!gripAxisParameter(ray, _dragPivot, _dragAxis, parameter)) {
                return;
            }
            float delta = (parameter - _dragParameter) * _dragPrecision;
            switch (*_drag) {
            case Handle::Travel:
                shape.travel = std::max(kMinimumTravel, snap ? snapped(_dragStart.travel + delta) : _dragStart.travel + delta);
                break;
            case Handle::SwingA:
            case Handle::SwingB:
                aim(startEnd + _dragAxis * (snap ? snapped(delta) : delta));
                break;
            case Handle::Pickup:
                shape.pickup = std::max(kMinimumMargin, snap ? snapped(_dragStart.pickup + delta) : _dragStart.pickup + delta);
                break;
            case Handle::Release:
                shape.release = std::max(kMinimumMargin, snap ? snapped(_dragStart.release + delta) : _dragStart.release + delta);
                break;
            case Handle::End:
                break;
            }
        }
        if (guideEdited) {
            guideEdited(shape, false);
        }
    }

    // A finished drag reports the guide as the controller last applied it; a
    // cancelled one reports where it started.
    void MagazineAuthoringViewport::finishDrag(bool cancel)
    {
        if (!_drag) {
            return;
        }
        _drag.reset();
        const auto shape = cancel || !_guide ? _dragStart : *_guide;
        if (guideEdited) {
            guideEdited(shape, true);
        }
    }

    void MagazineAuthoringViewport::paint()
    {
        _handles.clear();
        if (_mode != Mode::Guide || !_guide || !_view->glContext) {
            return;
        }
        const QSizeF size(_view->width(), _view->height());
        if (size.width() < 300 || size.height() < 180) {
            return;
        }
        const auto dpr = _view->devicePixelRatioF();
        QOpenGLPaintDevice device(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)));
        device.setDevicePixelRatio(dpr);
        QPainter painter(&device);
        painter.setRenderHint(QPainter::Antialiasing);
        paintGuide(painter);
    }

    void MagazineAuthoringViewport::paintGuide(QPainter& painter)
    {
        const QSizeF size(_view->width(), _view->height());
        const auto dpr = float(_view->devicePixelRatioF());
        const auto camera = _view->reloadMakerWorldToClip();
        const auto& g = *_guide;
        const auto u = g.outward.normalized();
        const auto end = g.seated + u * g.travel;
        const auto release = end + u * g.release;
        QVector3D a, b;
        basis(a, b);
        const bool carried = blocked && blocked();

        design::paintViewportHint(painter, { 16, 14 }, size.width() - 32,
                                  carried ? QStringLiteral("Testing the guide · bring the magazine to the line and push it in · Esc cancels")
                                          : QStringLiteral("Drag the outer end to aim the guide · its blue arrow sets the travel · the ring knob sets pickup · "
                                                           "the far knob sets release · Shift for precision · Ctrl snaps to 0.5 units"));

        QPointF endScreen;
        float depth = 0;
        if (!projectGripPoint(camera, size, end, endScreen, &depth)) {
            return;
        }
        QVector3D edge;
        if (!unprojectGripPoint(camera, size, endScreen + QPointF(kArrowPixels, 0), depth, edge)) {
            return;
        }
        const float arrow = (edge - end).length();
        if (!(arrow > 0)) {
            return;
        }

        _overlay.begin(camera, QSizeF(size.width() * dpr, size.height() * dpr));
        const float stroke = 2.f * dpr;
        // The line the hand follows, then the stretch past it before release.
        _overlay.addPolyline({ g.seated, end }, 3.f * dpr, kLine);
        _overlay.addPolyline({ end, release }, 1.5f * dpr, kRelease);
        // The pickup cylinder: rings at both ends joined along the line.
        QColor cylinder = kLine;
        cylinder.setAlphaF(.55f);
        for (const auto& center : { g.seated, end }) {
            QVector<QVector3D> ring;
            for (int i = 0; i < kRingSegments; ++i) {
                const float angle = float(i) * 6.28318530718f / kRingSegments;
                ring.append(center + (a * std::cos(angle) + b * std::sin(angle)) * g.pickup);
            }
            _overlay.addPolyline(ring, 1.2f * dpr, cylinder, true);
        }
        for (int i = 0; i < 4; ++i) {
            const float angle = float(i) * 1.57079632679f;
            const auto side = (a * std::cos(angle) + b * std::sin(angle)) * g.pickup;
            _overlay.addPolyline({ g.seated + side, end + side }, 1.f * dpr, cylinder);
        }
        _overlay.addBox(g.seated, 4.f * dpr, kLine);

        const auto arrowHandle = [&](Handle handle, QVector3D direction, const QColor& color) {
            QPointF tip;
            if (!projectGripPoint(camera, size, end + direction * arrow, tip) || QLineF(endScreen, tip).length() < 15) {
                return;
            }
            _overlay.addPolyline({ end, end + direction * arrow }, stroke, color);
            _overlay.addArrowHead(end, end + direction * arrow, 11.f * dpr, 4.f * dpr, color);
            const auto unit = (tip - endScreen) / QLineF(endScreen, tip).length();
            _handles.append({ handle, direction, QPolygonF{ endScreen + unit * 12, tip } });
        };
        const auto knob = [&](Handle handle, QVector3D at, QVector3D direction, const QColor& color) {
            QPointF screen;
            if (!projectGripPoint(camera, size, at, screen)) {
                return;
            }
            _overlay.addBox(at, kBoxPixels * dpr, color);
            _handles.append({ handle, direction, box(screen) });
        };
        if (!carried) {
            arrowHandle(Handle::Travel, u, kTravel);
            arrowHandle(Handle::SwingA, a, kSwingA);
            arrowHandle(Handle::SwingB, b, kSwingB);
            knob(Handle::Pickup, end - b * g.pickup, -b, kLine);
            knob(Handle::Release, release, u, kRelease);
            knob(Handle::End, end, {}, kEnd);
        }
        painter.beginNativePainting();
        _overlay.draw(kHiddenAlpha);
        painter.endNativePainting();

        painter.setFont(design::uiFont(design::kSmallPx));
        // Each label sits on its own side: the outer end, release and pickup
        // points are close together when the margins are short.
        const auto label = [&](QVector3D at, const QString& text, const QColor& color, QPointF offset, bool right) {
            QPointF screen;
            if (projectGripPoint(camera, size, at, screen)) {
                painter.setPen(color);
                const auto width = painter.fontMetrics().horizontalAdvance(text);
                painter.drawText(screen + offset - QPointF(right ? 0 : width, 0), text);
            }
        };
        label(g.seated, QStringLiteral("Seated"), kLine, { 12, -8 }, true);
        label(end, QStringLiteral("Outer end · travel %1").arg(double(g.travel), 0, 'f', 2), kEnd, { 14, -10 }, true);
        label(release, QStringLiteral("Release · +%1").arg(double(g.release), 0, 'f', 2), kRelease, { 14, 18 }, true);
        label(end - b * g.pickup, QStringLiteral("Pickup · %1").arg(double(g.pickup), 0, 'f', 2), kLine, { -14, 4 }, false);
    }
}
