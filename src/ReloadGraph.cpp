#include "ReloadGraph.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

namespace reloadmaker {
    ReloadGraph::ReloadGraph(QWidget* parent):QWidget(parent)
    {
        setMinimumHeight(130);
        setMinimumWidth(420);
    }

    void ReloadGraph::setRecipe(const ReloadRecipe* recipe)
    {
        _recipe = recipe;
        _rows.clear();
        if (recipe) {
            for (int track = 0; track < recipe->tracks.size(); ++track) {
                auto spans = reloadMotionSpans(*recipe, track);
                if (!spans.isEmpty()) {
                    _rows.append({ track, std::move(spans) });
                }
            }
        }
        setMinimumHeight(std::max(130, rowTop + int(_rows.size()) * rowHeight + 10));
        update();
    }

    void ReloadGraph::setTime(float seconds)
    {
        _seconds = seconds;
        update();
    }

    float ReloadGraph::xAt(float seconds) const
    {
        const float span = float(std::max(1, width() - labelWidth - rightMargin));
        return labelWidth + span * seconds / _recipe->duration;
    }

    float ReloadGraph::timeAt(float x) const
    {
        const float span = float(std::max(1, width() - labelWidth - rightMargin));
        return std::clamp((x - labelWidth) / span, 0.f, 1.f) * _recipe->duration;
    }

    void ReloadGraph::paintEvent(QPaintEvent*)
    {
        QPainter p(this);
        p.fillRect(rect(), QColor("#111719"));
        p.setRenderHint(QPainter::Antialiasing);
        if (!_recipe || _rows.isEmpty()) {
            p.setPen(QColor("#a6b0b5"));
            const QString text = _recipe ? "No weapon part moves measurably in this reload." : "The reload's moving parts appear here after it is learned.";
            p.drawText(rect().adjusted(18, 12, -18, -12), Qt::AlignCenter, text);
            return;
        }
        const auto& r = *_recipe;
        p.setPen(QColor("#a7b6bc"));
        p.drawText(QRect(12, 3, labelWidth - 20, 22), Qt::AlignVCenter, "PARTS / RELOAD TIME");
        for (int i = 0; i <= 10; ++i) {
            const float time = r.duration * i / 10.f;
            const float pixel = xAt(time);
            p.setPen(QColor("#28343a"));
            p.drawLine(QPointF(pixel, 26), QPointF(pixel, height()));
            p.setPen(QColor("#91a3ad"));
            p.drawText(QPointF(pixel + 3, 19), QString::number(time, 'f', 1) + "s");
        }
        const QColor bar("#548981");
        for (int row = 0; row < _rows.size(); ++row) {
            const int y = rowTop + rowHeight * row;
            if (row % 2 == 0) {
                p.fillRect(QRect(0, y - 3, width(), rowHeight), QColor("#182023"));
            }
            const auto& label = r.tracks[_rows[row].track].label;
            p.setPen(QColor("#d5dfdf"));
            p.drawText(QRect(12, y, labelWidth - 24, 19), Qt::AlignVCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, labelWidth - 24));
            p.setPen(bar.lighter(140));
            p.setBrush(bar.darker(160));
            for (const auto& span : _rows[row].spans) {
                const float left = xAt(r.duration * span.first / (r.frames - 1));
                const float right = xAt(r.duration * span.last / (r.frames - 1));
                p.drawRoundedRect(QRectF(left, y, std::max(2.f, right - left), 17), 2, 2);
            }
        }
        p.setPen(QPen(QColor("#f7e1ba"), 1.5));
        p.drawLine(QPointF(xAt(_seconds), 25), QPointF(xAt(_seconds), height()));
    }

    void ReloadGraph::mousePressEvent(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton || !_recipe || !sought || event->position().x() < labelWidth) {
            return;
        }
        sought(timeAt(float(event->position().x())));
    }
}
