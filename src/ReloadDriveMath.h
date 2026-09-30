#pragma once
#include <QPointF>
#include <QVector>

namespace reloadmaker {
    // Screen-space source-curve projection. Ties at crossings prefer continuity
    // from the current source time; no axis is inferred and no pose is refitted.
    [[nodiscard]] float reloadCurveTime(const QVector<QPointF>& points,const QVector<float>& times,QPointF cursor,float previous,float endpointSnapFraction=0);
}
