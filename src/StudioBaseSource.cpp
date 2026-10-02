#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "StudioPaths.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

namespace reloadmaker {
bool ReloadMakerController::openBaseGame() {
    if(_loadThread||_reloadCapturing||_packProcess){setStatus("Finish the current source or asset job first.",true);return false;}
    if(insideMo2VirtualFilesystem()){setStatus("Use the connected MO2 library and choose Fallout4.esm to author base weapons in that profile's effective file view.",true);return false;}
    const auto check=checkInstallation(loadInstallationSetup());if(!check.gameReady){setStatus(check.errors.join('\n'),true);return false;}
    if(currentWeapon()&&!saveSession()){setStatus("Save the current project before opening base game sources.",true);return false;}
    const QDir data(check.dataPath),game(check.gameRoot);QStringList plugins{"Fallout4.esm"};
    if(QFileInfo::exists(data.filePath("Fallout4_VR.esm")))plugins.append("Fallout4_VR.esm");
    const auto ini=game.filePath(QFileInfo::exists(game.filePath("Fallout4.ini"))?"Fallout4.ini":"Fallout4_Default.ini");
    QString error;const auto archives=resolveGameArchives(check.dataPath,plugins,{ini},error);if(!error.isEmpty()){setStatus(error,true);return false;}
    QJsonArray inputs;for(const auto& plugin:plugins)inputs.append(data.filePath(plugin));
    const auto directory=studioDataPath("local/source-sessions/"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    if(!QDir().mkpath(directory)){setStatus("Cannot create the base source snapshot.",true);return false;}
    const auto path=directory+"/base-game.reloadsources.json";
    const QJsonObject definition{{"format","reload-maker-source-pack"},{"version",1},{"name","Base game and loose Data"},
        {"plugins",inputs},{"resourceData",check.dataPath},{"archives",QJsonArray::fromStringList(archives)}};
    const auto bytes=QJsonDocument(definition).toJson();QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){setStatus("Cannot save the base source snapshot.",true);return false;}
    _packageText=path;_gameDataText=check.dataPath;startPackageLoad(path,check.dataPath,true);return true;
}
}
