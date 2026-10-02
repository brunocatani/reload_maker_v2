#pragma once
#include "MagazineInteraction.h"

namespace reloadmaker {
struct MagazineGuide {
    QString recipeId;
    QVector3D outwardInConnection;
    float travel{},captureRadius{},releaseMargin{};
};
bool readMagazineGuide(const QJsonObject&,MagazineGuide&,QString&);
QJsonObject magazineGuideJson(const MagazineGuide&);
bool applyMagazineGuide(const MagazineGuide&,MagazineRail&,QString&);
}
