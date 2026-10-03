#pragma once

#include "HkxReader.h"
#include "data/niftypes.h"

#include <QJsonArray>
#include <QMatrix4x4>
#include <QQuaternion>

namespace reloadmaker
{
    // Collision and world-model frames are 4x4 matrices in game units.
    [[nodiscard]] inline QMatrix4x4 frameMatrix(const Transformf& t)
    {
        QMatrix4x4 m;
        m.translate(t.translation.x, t.translation.y, t.translation.z);
        m.rotate(QQuaternion(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z));
        m.scale(t.scale.x, t.scale.y, t.scale.z);
        return m;
    }

    // The native renderer's shape frame, from its basis vectors.
    [[nodiscard]] inline QMatrix4x4 frameMatrix(const Transform& t)
    {
        QMatrix4x4 m;
        const auto o = t * Vector3(0, 0, 0);
        const Vector3 axes[]{ t * Vector3(1, 0, 0) - o, t * Vector3(0, 1, 0) - o, t * Vector3(0, 0, 1) - o };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                m(r, c) = axes[c][r];
            }
            m(r, 3) = o[r];
        }
        return m;
    }

    // Row-major, as the saved formats store frames.
    [[nodiscard]] inline QJsonArray frameJson(const QMatrix4x4& m)
    {
        QJsonArray out;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out.append(m(r, c));
            }
        }
        return out;
    }
}
