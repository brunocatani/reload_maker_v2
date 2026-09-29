#include "StudioPaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QVariant>
namespace reloadmaker {
QString studioRoot() {
    return QCoreApplication::instance()->property("studioRoot").toString();
}
QString studioDataPath(const QString& relative) {
    return QDir(studioRoot()).filePath(relative);
}
}
