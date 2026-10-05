#include "viewport/PinOverlay.h"

#include "GripViewportMath.h"
#include "design/Theme.h"
#include "glview.h"
#include "gl/glnode.h"
#include "gl/glscene.h"

#include <QMouseEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace reloadmaker
{
    namespace
    {
        // Opacity of a marker where the weapon or the hands cover it.
        constexpr float kHiddenPinAlpha = 0.4f;
        // Markers are pulled this far toward the camera (game units) so a marker
        // on a slanted surface is not half buried in it.
        constexpr float kMarkerLift = 0.3f;
        constexpr float kMarkerHalfPixels = 6.0f;
        constexpr float kResolvedHalfPixels = 4.0f;
        constexpr qreal kCardMaximumWidth = 240;
        constexpr qreal kHitMargin = 4;
    }

    PinOverlay::PinOverlay(GLView* view, QObject* parent) : QObject(parent), _view(view)
    {
        _view->installEventFilter(this);
        connect(_view, &GLView::paintUpdate, this, [this] { paint(); });
    }

    void PinOverlay::setMarkers(QVector<PinMarker> markers)
    {
        _markers = std::move(markers);
        _hits.clear();
        _view->update();
    }

    void PinOverlay::setActive(bool active)
    {
        _active = active;
        _hits.clear();
        _pressPin.clear();
        _view->update();
    }

    void PinOverlay::setSelected(const QString& id)
    {
        if (_selected != id) {
            _selected = id;
            _view->update();
        }
    }

    std::optional<QVector3D> PinOverlay::worldPosition(const PinMarker& marker) const
    {
        auto* scene = _view->getScene();
        if (!scene || !scene->nifModel || !marker.shape.isValid() || marker.shape.model() != scene->nifModel) {
            return std::nullopt;
        }
        const auto* node = scene->getNode(scene->nifModel, marker.shape);
        if (!node || node->isHidden()) {
            return std::nullopt;
        }
        const auto world = node->worldTrans() * Vector3(marker.offset.x(), marker.offset.y(), marker.offset.z());
        return QVector3D(world[0], world[1], world[2]);
    }

    std::optional<QPointF> PinOverlay::screenPosition(const PinMarker& marker) const
    {
        const auto world = worldPosition(marker);
        QPointF point;
        if (!world || !projectGripPoint(_view->reloadMakerWorldToClip(), QSizeF(_view->width(), _view->height()), *world, point)) {
            return std::nullopt;
        }
        return point;
    }

    QString PinOverlay::pinAt(QPointF point) const
    {
        for (const auto& hit : _hits) {
            if (hit.second.adjusted(-kHitMargin, -kHitMargin, kHitMargin, kHitMargin).contains(point)) {
                return hit.first;
            }
        }
        return {};
    }

    bool PinOverlay::eventFilter(QObject* target, QEvent* event)
    {
        if (target != _view) {
            return QObject::eventFilter(target, event);
        }
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove ||
            type == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const bool wasCaptured = _gesture.captured();
            QString hit;
            if (_active && !wasCaptured && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)) {
                hit = pinAt(mouse->position());
            }
            if (!_gesture.consume(type, mouse->button(), mouse->buttons(), !hit.isEmpty(), Qt::LeftButton | Qt::RightButton)) {
                return QObject::eventFilter(target, event);
            }
            if (!wasCaptured) {
                _pressPin = hit;
                _pressButton = mouse->button();
            } else if (type == QEvent::MouseButtonRelease && mouse->button() == _pressButton && !_pressPin.isEmpty()) {
                const auto id = std::exchange(_pressPin, QString());
                if (pinAt(mouse->position()) == id) {
                    // Menus run a nested event loop; open them after the
                    // viewport has finished handling this release.
                    const bool menu = _pressButton == Qt::RightButton;
                    const auto global = mouse->globalPosition().toPoint();
                    QTimer::singleShot(0, this, [this, id, menu, global] {
                        if (menu && contextRequested) {
                            contextRequested(id, global);
                        } else if (!menu && selected) {
                            selected(id);
                        }
                    });
                }
            }
            return true;
        }
        (void)_gesture.consume(type, Qt::NoButton, Qt::NoButton);
        if (type == QEvent::FocusOut) {
            _pressPin.clear();
        }
        return QObject::eventFilter(target, event);
    }

    void PinOverlay::paint()
    {
        _hits.clear();
        auto* scene = _view->getScene();
        if (!_active || _markers.isEmpty() || !scene) {
            return;
        }
        const QSizeF size(_view->width(), _view->height());
        if (size.width() < 1 || size.height() < 1) {
            return;
        }
        const auto camera = _view->reloadMakerWorldToClip();
        const auto eye = scene->view.inverted().translation;
        const QVector3D cameraPosition(eye[0], eye[1], eye[2]);
        struct Placed
        {
            const PinMarker* marker;
            QVector3D world;
            QPointF point;
        };
        QVector<Placed> placed;
        for (const auto& marker : _markers) {
            const auto world = worldPosition(marker);
            QPointF point;
            if (!world || !projectGripPoint(camera, size, *world, point)) {
                continue;
            }
            const auto toCamera = cameraPosition - *world;
            const auto lifted = toCamera.length() > kMarkerLift ? *world + toCamera.normalized() * kMarkerLift : *world;
            placed.append({ &marker, lifted, point });
        }
        if (placed.isEmpty()) {
            return;
        }
        const qreal dpr = _view->devicePixelRatioF();
        QOpenGLPaintDevice device(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)));
        device.setDevicePixelRatio(dpr);
        QPainter painter(&device);

        const QColor open = QColor::fromRgba(design::color::selection);
        const QColor muted = QColor::fromRgba(design::color::text);
        const QColor ink("#1b2023");
        _overlay.begin(camera, QSizeF(size.width() * dpr, size.height() * dpr));
        for (const auto& item : placed) {
            const bool focus = item.marker->id == _selected;
            const float half = (item.marker->resolved ? kResolvedHalfPixels : kMarkerHalfPixels) * float(dpr) * (focus ? 1.3f : 1.0f);
            _overlay.addBox(item.world, half, item.marker->resolved ? muted : open);
            _overlay.addBox(item.world, half * 0.55f, ink);
        }
        painter.beginNativePainting();
        _overlay.draw(kHiddenPinAlpha);
        painter.endNativePainting();

        painter.setRenderHint(QPainter::Antialiasing);
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        const auto metrics = painter.fontMetrics();
        // Selected pin last, so its card is on top; hits are tested top first.
        std::stable_sort(placed.begin(), placed.end(), [this](const Placed& a, const Placed& b) {
            return (a.marker->id == _selected) < (b.marker->id == _selected);
        });
        for (const auto& item : placed) {
            const auto& marker = *item.marker;
            const bool focus = marker.id == _selected;
            const qreal half = (marker.resolved ? kResolvedHalfPixels : kMarkerHalfPixels) * (focus ? 1.3 : 1.0);
            QRectF hit(item.point - QPointF(half, half), QSizeF(2 * half, 2 * half));
            const QString number = QString::number(marker.number);
            if (marker.resolved) {
                // Resolved pins keep their number only.
                const QRectF tag(item.point + QPointF(half + 2, -8), QSizeF(metrics.horizontalAdvance(number) + 8, 16));
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(27, 32, 35, 200));
                painter.drawRoundedRect(tag, 3, 3);
                painter.setPen(muted);
                painter.drawText(tag, Qt::AlignCenter, number);
                hit = hit.united(tag);
            } else {
                const QString text = metrics.elidedText(number + QStringLiteral("  ") + marker.text, Qt::ElideRight, int(kCardMaximumWidth));
                QRectF card(item.point + QPointF(half + 6, -24), QSizeF(metrics.horizontalAdvance(text) + 16, 20));
                if (card.right() > size.width() - 4) {
                    card.moveRight(item.point.x() - half - 6);
                }
                card.moveTop(std::clamp(card.top(), 4.0, size.height() - card.height() - 4));
                // The leader starts outside the marker so it never crosses it.
                const QLineF leader(item.point, QPointF(card.left() < item.point.x() ? card.right() : card.left(), card.center().y()));
                painter.setPen(QPen(open, focus ? 1.5 : 0.8));
                if (leader.length() > 1.5 * half) {
                    painter.drawLine(leader.pointAt(1.5 * half / leader.length()), leader.p2());
                }
                painter.setBrush(QColor(focus ? "#443522" : "#1b2023"));
                painter.drawRoundedRect(card, 3, 3);
                painter.setPen(QColor("#eee8df"));
                painter.drawText(card.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
                hit = hit.united(card);
            }
            _hits.prepend({ marker.id, hit });
        }
        painter.end();
    }
}
