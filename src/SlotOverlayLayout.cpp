#include "SlotOverlay.h"
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    QVector<PlacedSlot> layoutSlotLabels(QVector<ProjectedSlot> points, QSizeF viewport, SlotKey selected)
    {
        QVector<PlacedSlot> result;
        const double width = viewport.width(), height = viewport.height();
        if (!std::isfinite(width) || !std::isfinite(height) || width < 300 || height < 200) { return result; }
        points.erase(std::remove_if(points.begin(), points.end(), [](const auto& point) {
            return !std::isfinite(point.point.x()) || !std::isfinite(point.point.y());
        }), points.end());
        if (points.isEmpty()) { return result; }
        constexpr double margin = 16, gap = 10, cardHeight = 42, pitch = cardHeight + gap;
        const double cardWidth = std::clamp(width / 5.0, 128.0, 180.0);
        const int columns = std::max(1, int((width - 2 * margin + gap) / (cardWidth + gap)));
        const int maxRows = std::max(1, int((height * .5 - 36) / pitch));
        const int capacity = columns * maxRows * 2;
        if (points.size() > capacity) {
            const auto focus = std::find_if(points.cbegin() + capacity, points.cend(),
                [selected](const auto& point) { return point.slot == selected; });
            if (focus != points.cend()) { points[capacity - 1] = *focus; }
            points.resize(capacity);
        }
        std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
            if (a.point.y() != b.point.y()) { return a.point.y() < b.point.y(); }
            if (a.point.x() != b.point.x()) { return a.point.x() < b.point.x(); }
            return a.slot < b.slot;
        });
        const double highest = points.first().point.y(), lowest = points.last().point.y();
        const int upperCount = (int(points.size()) + 1) / 2;
        result.reserve(points.size());
        for (int side = 0; side < 2; ++side) {
            const int begin = side ? upperCount : 0;
            const int end = side ? int(points.size()) : upperCount;
            if (begin == end) { continue; }
            std::stable_sort(points.begin() + begin, points.begin() + end, [](const auto& a, const auto& b) {
                return a.point.x() == b.point.x() ? a.slot < b.slot : a.point.x() < b.point.x();
            });
            const int rows = (end - begin + columns - 1) / columns;
            const double totalHeight = rows * pitch - gap;
            const double top = side
                ? std::clamp(lowest + 48, height * .5 + 10, height - margin - totalHeight)
                : std::clamp(highest - 48 - totalHeight, margin, height * .5 - 10 - totalHeight);
            for (int row = 0; row < rows; ++row) {
                const int first = begin + row * columns, last = std::min(end, first + columns);
                const int firstResult = int(result.size());
                double nextX = margin;
                for (int i = first; i < last; ++i) {
                    const auto& point = points.at(i);
                    const double x = std::max(nextX, std::clamp(point.point.x() - cardWidth * .5, margin, width - margin - cardWidth));
                    result.append({point.slot, point.point, QRectF(x, top + row * pitch, cardWidth, cardHeight), side == 0});
                    nextX = x + cardWidth + gap;
                }
                // Preserve projected left-to-right order while resolving collisions.
                double right = width - margin;
                for (int i = int(result.size()) - 1; i >= firstResult; --i) {
                    auto& card = result[i].card;
                    if (card.right() > right) { card.moveRight(right); }
                    right = card.left() - gap;
                }
            }
        }
        return result;
    }
}
