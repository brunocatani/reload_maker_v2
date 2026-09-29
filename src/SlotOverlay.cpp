#include "SlotOverlay.h"
#include "glview.h"
#include "gl/glcontext.hpp"
#include "gl/glnode.h"
#include "gl/gltools.h"
#include <QMouseEvent>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <cmath>
#include <algorithm>

namespace reloadmaker
{
    SlotOverlay::SlotOverlay(GLView* view, QObject* parent) : QObject(parent), _view(view)
    {
        _view->installEventFilter(this);
        connect(_view, &GLView::paintUpdate, this, [this]() { paint(); });
    }

    void SlotOverlay::setCallouts(QVector<SlotCallout> callouts)
    {
        _callouts = std::move(callouts);
        _hits.clear();
        _view->update();
    }

    void SlotOverlay::setActive(bool active)
    {
        _active = active;
        _hits.clear();
        _view->update();
    }

    void SlotOverlay::setSelected(SlotKey slot)
    {
        _selected = slot;
        _view->update();
    }

    bool SlotOverlay::eventFilter(QObject* target, QEvent* event)
    {
        if (target != _view) { return QObject::eventFilter(target, event); }
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick ||
            type == QEvent::MouseMove || type == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            SlotKey hitSlot = 0;
            if (_active && !_gesture.captured() && mouse->button() == Qt::LeftButton &&
                (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)) {
                for (const auto& hit : _hits) {
                    if (hit.second.contains(mouse->position())) {
                        hitSlot = hit.first;
                        break;
                    }
                }
            }
            if (_gesture.consume(type, mouse->button(), mouse->buttons(), hitSlot != 0)) {
                if (hitSlot && selected) { selected(hitSlot); }
                return true;
            }
        } else {
            (void)_gesture.consume(type, Qt::NoButton, Qt::NoButton);
        }
        return QObject::eventFilter(target, event);
    }

    void SlotOverlay::paint()
    {
        _hits.clear();
        auto* scene = _view->getScene();
        if (!_active || _callouts.isEmpty() || !scene || !scene->nifModel || !_view->glContext) { return; }
        const auto* uniforms = _view->glContext->globalUniforms;
        if (!uniforms) { return; }
        struct Label { const SlotCallout* callout; QPointF point; };
        QVector<Label> labels;
        const float width = float(_view->width()), height = float(_view->height());
        if (width < 320 || height < 160) { return; }
        for (const auto& callout : _callouts) {
            if (!callout.anchor.isValid()) { continue; }
            const auto* node = scene->getNode(scene->nifModel, callout.anchor);
            if (!node) { continue; }
            BoundSphere bounds;
            for (const auto& surface : callout.surfaces) {
                const auto* mesh = surface.isValid() ? scene->getNode(scene->nifModel, surface) : nullptr;
                if (mesh && !mesh->isHidden()) { bounds |= mesh->bounds(); }
            }
            const auto p = bounds.radius > 0 ? scene->view * bounds.center : node->viewTrans().translation;
            float clip[4]{};
            for (int row = 0; row < 4; ++row) {
                clip[row] = uniforms->projectionMatrix[0][row] * p[0] + uniforms->projectionMatrix[1][row] * p[1]
                          + uniforms->projectionMatrix[2][row] * p[2] + uniforms->projectionMatrix[3][row];
            }
            if (!std::isfinite(clip[3]) || clip[3] <= 0.00001f) { continue; }
            const float x = clip[0] / clip[3], y = clip[1] / clip[3];
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1.2f || std::abs(y) > 1.2f) { continue; }
            labels.append({&callout, QPointF((x + 1.0f) * width * 0.5f, (1.0f - y) * height * 0.5f)});
        }
        if (labels.isEmpty()) { return; }
        const qreal dpr = _view->devicePixelRatio();
        QOpenGLPaintDevice device(QSize(qRound(width * dpr), qRound(height * dpr)));
        device.setDevicePixelRatio(dpr);
        QPainter painter(&device);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        QVector<ProjectedSlot> projected;
        QHash<SlotKey, const SlotCallout*> lookup;
        for (const auto& label : labels) {
            projected.append({label.callout->slot, label.point});
            lookup.insert(label.callout->slot, label.callout);
        }
        const auto layout = layoutSlotLabels(std::move(projected), QSizeF(width, height), _selected);
        QHash<SlotKey, QRectF> cards;
        for (const auto& item : layout) { cards.insert(item.slot, item.card); }
        // Draw leaders behind all cards. Empty slots connect to the owner card,
        // not a fabricated point on the weapon.
        for (const auto& item : layout) {
            const auto* callout = lookup.value(item.slot);
            const bool focus = item.slot == _selected;
            const bool related = callout->owner == _selected;
            const QColor accent(focus ? "#f0c785" : related ? "#bfa57d" : "#67777d");
            const qreal x = std::clamp(item.point.x(), item.card.left() + 12, item.card.right() - 12);
            const QPointF start(x, item.above ? item.card.bottom() : item.card.top());
            const QPointF elbow(x, start.y() + (item.above ? 12 : -12));
            QPointF target = item.point;
            if (callout->ownerAnchor && cards.contains(callout->owner)) {
                const auto parent = cards.value(callout->owner);
                target = QPointF(std::clamp(start.x(), parent.left() + 8, parent.right() - 8),
                                 start.y() < parent.center().y() ? parent.top() : parent.bottom());
            }
            QPen pen(accent, focus ? 1.6 : .8);
            if (callout->ownerAnchor) { pen.setStyle(Qt::DashLine); }
            painter.setPen(pen);
            painter.drawPolyline(QPolygonF{start, elbow, target});
            if (!callout->ownerAnchor) {
                painter.setBrush(accent);
                painter.drawEllipse(target, focus ? 4.0 : 2.5, focus ? 4.0 : 2.5);
            }
        }
        for (const auto& item : layout) {
            const auto* callout = lookup.value(item.slot);
            const bool focus = item.slot == _selected;
            painter.setPen(QPen(QColor(focus ? "#f0c785" : "#59646a"), focus ? 1.5 : .8));
            painter.setBrush(QColor(focus ? "#443522" : "#1b2023"));
            painter.drawRoundedRect(item.card, 3, 3);
            font.setPixelSize(12);
            font.setBold(focus);
            painter.setFont(font);
            painter.setPen(QColor("#eee8df"));
            painter.drawText(item.card.adjusted(10, 3, -8, -20), Qt::AlignVCenter | Qt::AlignLeft,
                painter.fontMetrics().elidedText(callout->label, Qt::ElideRight, int(item.card.width() - 18)));
            font.setPixelSize(10);
            font.setBold(false);
            painter.setFont(font);
            painter.setPen(QColor(callout->ownerAnchor ? "#c3a67a" : "#a6afb4"));
            painter.drawText(item.card.adjusted(10, 22, -8, -3), Qt::AlignVCenter | Qt::AlignLeft,
                painter.fontMetrics().elidedText(callout->detail, Qt::ElideRight, int(item.card.width() - 18)));
            _hits.append({item.slot, item.card});
        }
        if (layout.size() < labels.size()) {
            painter.setPen(QColor("#c3a67a"));
            painter.drawText(QRectF(16, height - 18, width - 32, 16), Qt::AlignCenter,
                QStringLiteral("%1 more slots in the sidebar · select one to locate it").arg(labels.size() - layout.size()));
        }
        painter.end();
    }
}
