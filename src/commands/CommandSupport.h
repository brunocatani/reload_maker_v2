#pragma once

// Parameter validation shared by command handlers. Every helper rejects bad
// input by throwing std::runtime_error with text shown to the caller.

#include "HkxReader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QString>

#include <cmath>
#include <stdexcept>

namespace reloadmaker::commands
{
    inline void require(bool condition, const QString& error)
    {
        if (!condition) {
            throw std::runtime_error(error.toStdString());
        }
    }

    inline float number(const QJsonValue& value)
    {
        require(value.isDouble() && std::isfinite(value.toDouble()), "Expected a finite number");
        return float(value.toDouble());
    }

    // Quaternions arrive as [x, y, z, w] and must already be normalized.
    inline Quaternionf unitQuaternion(const QJsonValue& value)
    {
        const auto components = value.toArray();
        require(components.size() == 4, "Expected quaternion x,y,z,w");
        const Quaternionf rotation{ number(components[0]), number(components[1]),
                                    number(components[2]), number(components[3]) };
        const float lengthSquared = rotation.x * rotation.x + rotation.y * rotation.y +
                                    rotation.z * rotation.z + rotation.w * rotation.w;
        require(std::abs(lengthSquared - 1) < .0005f, "Expected a unit quaternion");
        return rotation;
    }

    inline QJsonObject transformJson(const Transformf& transform)
    {
        return { { "translate", QJsonArray{ transform.translation.x, transform.translation.y, transform.translation.z } },
                 { "rotation", QJsonArray{ transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w } },
                 { "scale", QJsonArray{ transform.scale.x, transform.scale.y, transform.scale.z } } };
    }

    inline void writeFileAtomically(const QString& path, const QByteArray& bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        require(written, "Cannot save " + path + ": " + file.errorString());
    }
}
