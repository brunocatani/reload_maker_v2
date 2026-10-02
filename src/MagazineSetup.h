#pragma once
#include "ReloadRecipe.h"
#include <QMap>
namespace reloadmaker {
struct MagazineSetup {
    QString recipeId,motion{"preset"};
    int emptyTrack{-1},loadedFrame{},emptyFrame{};
    bool sampledPayload{};
    float entrySeconds{};
    QMap<QString,QString> pieces; // Complete native shape path -> body, cartridge or spare.
};
bool readMagazineSetup(const QJsonObject&,MagazineSetup&,QString&);
QJsonObject magazineSetupJson(const MagazineSetup&);
}
