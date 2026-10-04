#include "ReloadGraph.h"
#include "design/Theme.h"
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

    // Drawn as the canvas timeline's tracks: a ruler, one row per moving
    // part with its motion spans, and the amber playhead.
    void ReloadGraph::paintEvent(QPaintEvent*)
    {
        using namespace design;
        QPainter p(this);
        p.fillRect(rect(), qcolor(color::card));
        p.setRenderHint(QPainter::Antialiasing);
        if (!_recipe || _rows.isEmpty()) {
            p.setFont(uiFont(kBodyPx));
            p.setPen(qcolor(color::textMuted));
            const QString text = _recipe ? "No weapon part moves measurably in this reload." : "The reload's moving parts appear here after it is learned.";
            p.drawText(rect().adjusted(18, 12, -18, -12), Qt::AlignCenter, text);
            return;
        }
        const auto& r = *_recipe;
        QFont heading = uiFont(kLabelPx, QFont::DemiBold);
        heading.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        p.setFont(heading);
        p.setPen(qcolor(color::section));
        p.drawText(QRect(12, 3, labelWidth - 20, 22), Qt::AlignVCenter, QStringLiteral("PART"));
        p.setFont(monoFont(kLabelPx));
        for (int i = 0; i <= 10; ++i) {
            const float time = r.duration * i / 10.f;
            const float pixel = xAt(time);
            p.setPen(qcolor(color::divider));
            p.drawLine(QPointF(pixel, 26), QPointF(pixel, height()));
            p.setPen(qcolor(color::textFaint));
            // The last label sits left of its line so the edge never clips it.
            const QString tick = QString::number(time, 'f', 1) + "s";
            const float offset = i == 10 ? -3.f - float(p.fontMetrics().horizontalAdvance(tick)) : 3.f;
            p.drawText(QPointF(pixel + offset, 19), tick);
        }
        p.setFont(uiFont(kSmallPx));
        for (int row = 0; row < _rows.size(); ++row) {
            const int y = rowTop + rowHeight * row;
            if (row % 2 == 0) {
                p.fillRect(QRect(0, y - 3, width(), rowHeight), qcolor(color::panel));
            }
            const auto& label = r.tracks[_rows[row].track].label;
            p.setPen(qcolor(color::textSecondary));
            p.drawText(QRect(12, y, labelWidth - 24, 19), Qt::AlignVCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, labelWidth - 24));
            p.setPen(QPen(qcolor(color::accent), 1));
            p.setBrush(qcolor(color::accent, 64));
            for (const auto& span : _rows[row].spans) {
                const float left = xAt(r.duration * span.first / (r.frames - 1));
                const float right = xAt(r.duration * span.last / (r.frames - 1));
                p.drawRoundedRect(QRectF(left, y + 1, std::max(2.f, right - left), 15), 4, 4);
            }
        }
        p.setPen(QPen(qcolor(color::playhead), 2));
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
