#include "design/Icons.h"
#include "design/Theme.h"
#include "StudioPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace reloadmaker::design
{
    namespace
    {
        // Each icon is a stroked path; `filled` icons are filled instead.
        struct Shape
        {
            QPainterPath path;
            bool filled{ false };
        };

        QPainterPath polyline(std::initializer_list<QPointF> points)
        {
            QPainterPath path;
            auto it = points.begin();
            path.moveTo(*it);
            for (++it; it != points.end(); ++it) {
                path.lineTo(*it);
            }
            return path;
        }

        Shape shape(Icon name)
        {
            QPainterPath path;
            switch (name) {
            case Icon::ChevronDown:
                return { polyline({ { 6, 9 }, { 12, 15 }, { 18, 9 } }) };
            case Icon::ChevronUp:
                return { polyline({ { 6, 15 }, { 12, 9 }, { 18, 15 } }) };
            case Icon::ChevronRight:
                return { polyline({ { 9, 6 }, { 15, 12 }, { 9, 18 } }) };
            case Icon::Check:
                return { polyline({ { 20, 6 }, { 9, 17 }, { 4, 12 } }) };
            case Icon::Play:
                path = polyline({ { 7, 4 }, { 7, 20 }, { 20, 12 } });
                path.closeSubpath();
                return { path, true };
            case Icon::Pause:
                path.addRect(6, 4, 4, 16);
                path.addRect(14, 4, 4, 16);
                return { path, true };
            case Icon::Restart:
                // Counter-clockwise from the left round to the top, then back
                // towards the arrow head at the upper left.
                path.moveTo(3, 12);
                path.arcTo(QRectF(3, 3, 18, 18), 180, 270);
                path.lineTo(6.3, 5.7);
                path.lineTo(3, 8);
                path.addPath(polyline({ { 3, 3 }, { 3, 8 }, { 8, 8 } }));
                return { path };
            case Icon::Loop:
                path = polyline({ { 17, 2 }, { 21, 6 }, { 17, 10 } });
                path.addPath(polyline({ { 3, 11 }, { 3, 9 }, { 5, 6 }, { 21, 6 } }));
                path.addPath(polyline({ { 7, 22 }, { 3, 18 }, { 7, 14 } }));
                path.addPath(polyline({ { 21, 13 }, { 21, 15 }, { 19, 18 }, { 3, 18 } }));
                return { path };
            case Icon::Menu:
                path = polyline({ { 4, 6 }, { 20, 6 } });
                path.addPath(polyline({ { 4, 12 }, { 20, 12 } }));
                path.addPath(polyline({ { 4, 18 }, { 20, 18 } }));
                return { path };
            case Icon::Search:
                path.addEllipse(QPointF(11, 11), 7, 7);
                path.addPath(polyline({ { 16.5, 16.5 }, { 20, 20 } }));
                return { path };
            case Icon::Fit:
                path = polyline({ { 4, 9 }, { 4, 4 }, { 9, 4 } });
                path.addPath(polyline({ { 15, 4 }, { 20, 4 }, { 20, 9 } }));
                path.addPath(polyline({ { 20, 15 }, { 20, 20 }, { 15, 20 } }));
                path.addPath(polyline({ { 9, 20 }, { 4, 20 }, { 4, 15 } }));
                return { path };
            case Icon::Grid:
                path.addRect(4, 4, 16, 16);
                path.addPath(polyline({ { 9.33, 4 }, { 9.33, 20 } }));
                path.addPath(polyline({ { 14.67, 4 }, { 14.67, 20 } }));
                path.addPath(polyline({ { 4, 9.33 }, { 20, 9.33 } }));
                path.addPath(polyline({ { 4, 14.67 }, { 20, 14.67 } }));
                return { path };
            case Icon::Tag:
                path = polyline({ { 3, 3 }, { 12, 3 }, { 21, 12 }, { 12, 21 }, { 3, 12 } });
                path.closeSubpath();
                path.addEllipse(QPointF(7.5, 7.5), 1.2, 1.2);
                return { path };
            case Icon::Close:
                path = polyline({ { 6, 6 }, { 18, 18 } });
                path.addPath(polyline({ { 18, 6 }, { 6, 18 } }));
                return { path };
            case Icon::Circle:
                path.addEllipse(QPointF(12, 12), 7, 7);
                return { path };
            case Icon::Diamond:
                path = polyline({ { 12, 2 }, { 22, 12 }, { 12, 22 }, { 2, 12 } });
                path.closeSubpath();
                return { path, true };
            case Icon::DotCircle:
                path.addEllipse(QPointF(12, 12), 7, 7);
                path.addEllipse(QPointF(12, 12), 1.6, 1.6);
                return { path };
            case Icon::Minus:
                return { polyline({ { 6, 12 }, { 18, 12 } }) };
            }
            return {};
        }

        void paintShape(QPainter& painter, const QRectF& rect, Icon name, const QColor& color)
        {
            const auto s = shape(name);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.translate(rect.topLeft());
            painter.scale(rect.width() / 24.0, rect.height() / 24.0);
            if (s.filled) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
            } else {
                painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
            }
            painter.drawPath(s.path);
            painter.restore();
        }

        class PathIconEngine final : public QIconEngine
        {
        public:
            PathIconEngine(Icon name, QRgb color) : _name(name), _color(color) {}
            void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
            {
                const QRgb color = mode == QIcon::Disabled ? color::textDisabled : _color;
                const qreal side = std::min(rect.width(), rect.height());
                const QRectF square(rect.x() + (rect.width() - side) / 2, rect.y() + (rect.height() - side) / 2, side, side);
                paintShape(*painter, square, _name, qcolor(color));
            }
            QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
            {
                return scaledPixmap(size, mode, state, 1.0);
            }
            QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
            {
                QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    paint(&painter, QRect(QPoint(0, 0), image.size()), mode, state);
                }
                auto result = QPixmap::fromImage(image);
                result.setDevicePixelRatio(scale);
                return result;
            }
            QIconEngine* clone() const override { return new PathIconEngine(_name, _color); }

        private:
            Icon _name;
            QRgb _color;
        };
    }

    QIcon icon(Icon name, QRgb color)
    {
        return QIcon(new PathIconEngine(name, color));
    }

    QString iconFile(Icon name, QRgb color, int px)
    {
        const QString base = studioDataPath(QStringLiteral("local/ui-cache/icon-%1-%2-%3").arg(int(name)).arg(color & 0xffffff, 6, 16, QLatin1Char('0')).arg(px));
        QDir().mkpath(QFileInfo(base).absolutePath());
        for (const auto& [suffix, scale] : { std::pair{ QStringLiteral(".png"), 1 }, std::pair{ QStringLiteral("@2x.png"), 2 } }) {
            const QString path = base + suffix;
            if (QFileInfo::exists(path)) {
                continue;
            }
            QImage image(px * scale, px * scale, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                paintShape(painter, QRectF(0, 0, image.width(), image.height()), name, qcolor(color));
            }
            image.save(path);
        }
        return QDir::fromNativeSeparators(base + QStringLiteral(".png"));
    }
}
