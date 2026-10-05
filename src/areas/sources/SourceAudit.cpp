// Source evidence: the bytes of every animation and model a configuration is
// authored from, and the audit that compares them with the current source
// view before recording, packing or exporting.
#include "ReloadMakerController.h"
#include "SourceEvidence.h"
#include "StudioPaths.h"
#include <QCryptographicHash>
#include <QMap>
#include <QSet>
#include <stdexcept>

namespace reloadmaker {
QJsonObject ReloadMakerController::sourceEvidence() const {
    if(!currentWeapon()||!_reloadRecipe)throw std::runtime_error("Prepare the selected reload in Operate before recording its source inputs");
    QMap<QString,QJsonObject> resources;
    const auto add=[&](const QString& kind,QString path,const QByteArray& bytes,const QString& expected=QString()){
        path=ResourceArchive::normalizeResourcePath(path);if(path.isEmpty())return;
        if(bytes.isEmpty()&&expected.isEmpty())throw std::runtime_error(("Cannot record missing source: "+path).toStdString());
        const auto hash=expected.isEmpty()?QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()):expected;
        resources[path]={{"kind",kind},{"path",path},{"sha256",hash}};
    };
    const auto recipe=_reloadRecipe->source;
    for(const auto* field:{"animation","baseline"})if(!recipe[field].toString().isEmpty())add("animation",recipe[field].toString(),{},recipe[QString(field)+"Sha256"].toString());
    if(_simulationRig)for(const auto* field:{"animation","baseline"}){const auto source=_simulationRig->mechanicalSource;const auto path=source[field].toString();if(!path.isEmpty())add("animation",path,{},source[QString(field)+"Sha256"].toString());}
    for(const auto& action:_animationActions)if(action.family==ActionFamily::Fire||action.family==ActionFamily::Cycle){
        for(const auto& path:{action.resourcePath,action.baselineResourcePath})if(!path.isEmpty()) {
            const auto key=ResourceArchive::normalizeResourcePath(path);const auto expected=_animationReadHashes.value(key);
            QString error;const auto bytes=expected.isEmpty()?readAnimationResource(path,error):QByteArray();
            if(!error.isEmpty())throw std::runtime_error(error.toStdString());
            add("animation",path,bytes,expected);
        }
    }
    QSet<QString> models{currentWeapon()->baseModelPath};for(const auto id:configuration()->activeMods())if(const auto* mod=configuration()->mod(id))if(!mod->modelPath.isEmpty())models.insert(mod->modelPath);
    for(auto path:models){
        path=normalizeNifPath(path);const auto& archive=_packageResources.contains(path)?_packageResources:_gameMeshes;
        const auto cacheKey=(&archive==&_gameMeshes?"game:":"mod:")+path;
        QString error;const auto* cached=_nifBytesCache.object(cacheKey);const auto bytes=cached?*cached:archive.read(path,error);
        if(!error.isEmpty())throw std::runtime_error(error.toStdString());
        add("model",path,bytes);
    }
    QJsonArray array;for(const auto& item:resources)array.append(item);
    return {{"version",1},{"weapon",poseRecordKey(currentWeapon()->formId)},{"resources",array}};
}
QJsonObject ReloadMakerController::auditSources(const QJsonObject& evidence) const {
    if(_loadThread||_closing)throw std::runtime_error("Wait for the current source view to finish loading");
    // A loaded BA2 index or loose-file census can predate an added override.
    // Reopen the effective view for this explicit authoring check; never use
    // the renderer's cached archive mapping as proof of unchanged input bytes.
    ResourceArchive current,game;QString error;
    if(_libraryMode){
        if(!insideMo2VirtualFilesystem()||profileFingerprint(_launchSnapshot.instancePath,_launchSnapshot.profilePath)!=_launchSnapshot.fingerprint)
            throw std::runtime_error("The active MO2 source snapshot changed; reconnect the selected profile");
        if(!current.openLoadOrder(_launchSnapshot.dataPath,_launchSnapshot.archives,error))throw std::runtime_error(error.toStdString());
    }else if(!_package.resourceData.isEmpty()){if(!current.openLoadOrder(_package.resourceData,_package.archives,error))throw std::runtime_error(error.toStdString());}
    else if(!current.openPackages(_package.roots.isEmpty()?QStringList{_package.rootPath}:_package.roots,error))throw std::runtime_error(error.toStdString());
    auto result=inspectSourceEvidence(evidence,[&](const QString&,const QString& path){
        SourceBytes value;
        if(!_libraryMode&&_package.resourceData.isEmpty()&&!current.contains(path)&&!game.isOpen()&&!game.open(_gameData,value.error))return value;
        const auto& archive=(_libraryMode||!_package.resourceData.isEmpty()||current.contains(path))?current:game;
        value.bytes=archive.read(path,value.error);value.provider=archive.provider(path);return value;
    });
    if(_libraryMode&&profileFingerprint(_launchSnapshot.instancePath,_launchSnapshot.profilePath)!=_launchSnapshot.fingerprint)
        throw std::runtime_error("MO2 profile changed during the source check; reconnect before continuing");
    result["sourceMode"]=_libraryMode?"mo2-profile":!_package.resourceData.isEmpty()?"explicit-data":"ordered-packages";
    result["profile"]=_libraryMode?_launchSnapshot.profileName:QString();return result;
}
}
