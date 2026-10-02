#pragma once
#include "HandPoseLibrary.h"

namespace reloadmaker {
struct MagazineReference {
    QString recipeId, anchorKey, connectionMode{"source"};
    Transformf connectionInCarrier;
};
QString magazineReferenceKey(const HandPosePlacement&);
bool readMagazineReference(const QJsonObject&,MagazineReference&,QString&);
QJsonObject magazineReferenceJson(const MagazineReference&);
}
