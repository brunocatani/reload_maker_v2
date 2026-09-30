#pragma once
#include "HkxReader.h"
#include <QJsonObject>
namespace reloadmaker {
struct OperatingGrabLocation {
    QString id,name;
    Vec3f point{};
    float radius{3};
    int hands{3};
};
struct OperatingGrabLocations {
    QString weapon,group,sourceModel,sourcePath,sourceName;
    QVector<OperatingGrabLocation> points;
};
QString grabLocationsKey(const OperatingGrabLocations&);
QJsonObject grabLocationsJson(const OperatingGrabLocations&);
bool readGrabLocations(const QJsonObject&,OperatingGrabLocations&,QString& error);
}
