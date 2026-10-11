#pragma once
#include "ReloadRecipe.h"
#include <QMap>
namespace reloadmaker {
struct MagazineSetup {
    QString recipeId,motion{"preset"},movementId;
    int emptyTrack{-1},loadedFrame{},emptyFrame{};
    bool sampledPayload{};
    float entrySeconds{};
    float seatSeconds{-1}; // Fixed-feed round loading ends here, before the shell is culled.
    QMap<QString,QString> pieces; // Complete native shape path -> body, cartridge or spare.
    QVector<int> loadingFollowers; // Lifter/gate tracks driven by each inserted round.
    QJsonArray trackAnchors; // The node of each track index (recipeTrackAnchors).
};
bool readMagazineSetup(const QJsonObject&,MagazineSetup&,QString&);
QJsonObject magazineSetupJson(const MagazineSetup&);
}
