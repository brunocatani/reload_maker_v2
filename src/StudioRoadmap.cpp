#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "SourceEvidence.h"
#include "ReloadTemplates.h"
#include "CollisionPanel.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
namespace {
const QStringList titles{"Source and weapon","Assembly and coverage","Behavior template","Operating parts and followers",
    "Operating motion","Full and empty magazines","Magazine movement and guide","Magazine hands and operating fingers",
    "Moving grab points","Physical sounds","Collision and finished models","Test and export"};
const QStringList instructions{
    "Choose the effective source profile, then the weapon. Record the relevant input bytes so an animation-only replacement can be detected.",
    "Select the receiver, magazine and operating parts. Review every configuration you intend to cover; an unsupported variation should stay explicit.",
    "The template describes the mechanism. These five templates reflect the tested behaviors; unsupported feed systems need new behavior support.",
    "Inspect the actual moving parts. Keep the barrel, sights, cartridge and firing-pin followers that participate in this source mechanism.",
    "Use the native source motion and check both endpoints. Keep player input separate from visible bolt lift, rotation and locking.",
    "Review the installed, loose-full and loose-empty presentations. Exclude parked reload spares and retain the correct pre-feed cartridge state.",
    "Check the finite straight or angled input guide and the complete visible extraction/insertion curve at several intermediate positions.",
    "Magazine fits include wrist placement and all fingers. Operating grabs use fingers only with a live wrist. Configure both permitted magazine hands.",
    "Pick contact points on the grabbable surface and verify that they follow the part when open. Check allowed hands and pickup radii.",
    "Audition the physical event cues. Check holds, reversals, insertion, catch release and dry fire; each crossing should sound once.",
    "Generate and inspect bounded hulls for the selected magazine. New world assets must be cooked with the configured Creation Kit tools before publication.",
    "Exercise Operate, save the current draft and publish a complete validated pack. The installation builder creates the separate local object ESP."};
}
QString ReloadMakerController::assignedTemplate() const {return currentWeapon()?_reloadTemplateAssignments[poseRecordKey(currentWeapon()->formId).toLower()].toString():QString();}
QString ReloadMakerController::roadmapKey() const {
    if(!currentWeapon()||!configuration())return {};
    const auto choices=configuration()->savedChoices(*currentPlugin());
    return poseRecordKey(currentWeapon()->formId).toLower()+"/"+QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(choices).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
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
            if(!error.isEmpty())throw std::runtime_error(error.toStdString());add("animation",path,bytes,expected);
        }
    }
    QSet<QString> models{currentWeapon()->baseModelPath};for(const auto id:configuration()->activeMods())if(const auto* mod=configuration()->mod(id))if(!mod->modelPath.isEmpty())models.insert(mod->modelPath);
    for(auto path:models){
        path=normalizeNifPath(path);const auto& archive=_packageResources.contains(path)?_packageResources:_gameMeshes;
        const auto cacheKey=(&archive==&_gameMeshes?"game:":"mod:")+path;
        QString error;const auto* cached=_nifBytesCache.object(cacheKey);const auto bytes=cached?*cached:archive.read(path,error);
        if(!error.isEmpty())throw std::runtime_error(error.toStdString());add("model",path,bytes);
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
    }else if(!current.openPackages(_package.roots.isEmpty()?QStringList{_package.rootPath}:_package.roots,error))throw std::runtime_error(error.toStdString());
    auto result=inspectSourceEvidence(evidence,[&](const QString&,const QString& path){
        SourceBytes value;
        if(!_libraryMode&&!current.contains(path)&&!game.isOpen()&&!game.open(_gameData,value.error))return value;
        const auto& archive=(_libraryMode||current.contains(path))?current:game;
        value.bytes=archive.read(path,value.error);value.provider=archive.provider(path);return value;
    });
    if(_libraryMode&&profileFingerprint(_launchSnapshot.instancePath,_launchSnapshot.profilePath)!=_launchSnapshot.fingerprint)
        throw std::runtime_error("MO2 profile changed during the source check; reconnect before continuing");
    result["sourceMode"]=_libraryMode?"mo2-profile":"ordered-packages";
    result["profile"]=_libraryMode?_launchSnapshot.profileName:QString();return result;
}
QJsonObject ReloadMakerController::roadmapState() const {
    const auto key=roadmapKey();const auto saved=_authoringRoadmaps[key].toObject();QJsonArray steps;
    QJsonArray templates;for(const auto& item:reloadTemplates)templates.append(QJsonObject{{"id",item.id},{"label",item.label},{"category",item.category}});
    for(int i=0;i<titles.size();++i)steps.append(QJsonObject{{"index",i},{"title",titles[i]},{"instruction",instructions[i]}});
    return {{"key",key},{"weapon",currentWeapon()?poseRecordKey(currentWeapon()->formId):QString()},
        {"template",currentWeapon()?templateIdentity(simulationMechanism()):QString()},{"templates",templates},{"templateAssigned",!assignedTemplate().isEmpty()},
        {"steps",steps},{"selectedStep",saved["step"].toInt()},{"sourcesRecorded",saved["sources"].isObject()},
        {"project",_sessionPath},{"packProject",saved["packProject"].toString()}};
}
QJsonObject ReloadMakerController::roadmapCommand(const QJsonObject& request) {
    const auto operation=request["operation"].toString("state");if(operation=="state")return roadmapState();
    const auto key=roadmapKey();if(key.isEmpty()||_loadThread||_reloadCapturing||_reloadCaptureQueued||_rebuildTimer->isActive())throw std::runtime_error("Wait for a loaded weapon and completed capture");
    auto saved=_authoringRoadmaps.value(key).toObject();
    if(!saved.contains("step"))saved["step"]=0;
    if(operation=="template") {
        const auto id=request["id"].toString();if(!templateMechanism(id))throw std::runtime_error("Choose one of the supported behavior templates");
        stopSimulation();_reloadTemplateAssignments[poseRecordKey(currentWeapon()->formId).toLower()]=id;_assignedMechanism=templateMechanism(id);saved["step"]=2;
    } else if(operation=="record-sources") {
        const auto evidence=sourceEvidence();const auto audit=auditSources(evidence);
        if(!audit["compatible"].toBool())throw std::runtime_error("Loaded authoring data differs from the current resource bytes. Reconnect/reload the source before recording it.");
        saved["sources"]=evidence;
    } else if(operation=="check-sources") {
        if(!saved["sources"].isObject())throw std::runtime_error("Record the source inputs first");
        return auditSources(saved["sources"].toObject());
    } else if(operation=="step") {
        const int step=request["index"].toInt(-1);if(step<0||step>=titles.size())throw std::runtime_error("Unknown authoring step");saved["step"]=step;
    } else throw std::runtime_error("Unknown roadmap operation");
    _authoringRoadmaps[key]=saved;++_revision;if(operation=="template")prepareSimulation();if(!saveSession())throw std::runtime_error("Could not save the authoring roadmap");return roadmapState();
}
void ReloadMakerController::showAuthoringRoadmap() {
    QDialog dialog(_window);dialog.setWindowTitle("Reload authoring roadmap");dialog.resize(940,670);
    auto* layout=new QVBoxLayout(&dialog);auto* heading=new QLabel;heading->setWordWrap(true);layout->addWidget(heading);
    auto* templateRow=new QHBoxLayout;layout->addLayout(templateRow);auto* templates=new QComboBox;for(const auto& item:reloadTemplates)templates->addItem(item.label,item.id);templates->setCurrentIndex(templates->findData(roadmapState()["template"].toString()));templateRow->addWidget(templates,1);auto* assign=new QPushButton("Use this template");templateRow->addWidget(assign);
    auto* body=new QHBoxLayout;layout->addLayout(body,1);auto* list=new QListWidget;list->setMinimumWidth(320);body->addWidget(list);
    for(int i=0;i<titles.size();++i)list->addItem(QString("%1  %2").arg(i+1,2,10,QChar('0')).arg(titles[i]));
    auto* detail=new QPlainTextEdit;detail->setReadOnly(true);body->addWidget(detail,1);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* open=new QPushButton("Open this step");auto* record=new QPushButton("Record source inputs");auto* check=new QPushButton("Check for source changes");auto* save=new QPushButton("Save draft");
    for(auto* button:{open,record,check,save})actions->addWidget(button);
    const auto refresh=[&]{const auto state=roadmapState();heading->setText(state["weapon"].toString()+"\nCurrent behavior: "+state["template"].toString());check->setEnabled(state["sourcesRecorded"].toBool());};
    connect(assign,&QPushButton::clicked,&dialog,[&]{try{roadmapCommand({{"operation","template"},{"id",templates->currentData().toString()}});refresh();list->setCurrentRow(2);detail->appendPlainText("Template assigned to this weapon. Review its native operating roles and test Operate before export.");}catch(const std::exception& e){detail->setPlainText(e.what());}});
    connect(list,&QListWidget::currentRowChanged,&dialog,[&](int step){if(step<0)return;detail->setPlainText(instructions[step]);try{roadmapCommand({{"operation","step"},{"index",step}});}catch(const std::exception& e){detail->appendPlainText(e.what());}});
    const auto sourceAction=[&](const QString& operation){try{const auto result=roadmapCommand({{"operation",operation}});refresh();
        if(operation=="record-sources")detail->setPlainText("Source bytes recorded for this exact weapon assembly. A mod does not need an ESP for a changed animation or model to be detected.");
        else {QString text=result["compatible"].toBool()?"Relevant source bytes are unchanged.":"Source changes require review; your authored work is retained.";
            text+="\nSource view: "+result["sourceMode"].toString()+" "+result["profile"].toString();
            for(const auto& value:result["resources"].toArray()){const auto item=value.toObject();text+="\n\n"+item["status"].toString()+" · "+item["path"].toString();if(item["status"]!="unchanged")text+="\n"+item["provider"].toString()+"\n"+item["error"].toString();}detail->setPlainText(text);}
    }catch(const std::exception& e){detail->setPlainText(e.what());}};
    connect(record,&QPushButton::clicked,&dialog,[&]{sourceAction("record-sources");});connect(check,&QPushButton::clicked,&dialog,[&]{sourceAction("check-sources");});
    connect(save,&QPushButton::clicked,&dialog,[&]{if(saveSession())detail->appendPlainText("Draft saved.");});
    connect(open,&QPushButton::clicked,&dialog,[&]{const int step=list->currentRow();dialog.accept();
        QTimer::singleShot(0,this,[this,step]{if(step==0){showSetup();return;}if(step==11){setWorkspace(OperateWorkspace);showConfigurationSaver();return;}
            if(step==9){setWorkspace(SoundsWorkspace);return;}if(step==10){setWorkspace(CollisionsWorkspace);return;}
            if(step==3||step==4){setWorkspace(OperateWorkspace);showOperatingSetup();return;}
            if(step==6){setWorkspace(OperateWorkspace);showMagazineGuide();return;}
            if(step==5){setWorkspace(OperateWorkspace);showMagazineSetup();return;}
            if(step==7||step==8){setWorkspace(GripsWorkspace);if(step==8)(void)openGrabLocations();return;}
            if(step==1||step==2){setWorkspace(OperateWorkspace);return;}setWorkspace(PhasesWorkspace);});
    });
    refresh();list->setCurrentRow(roadmapState()["selectedStep"].toInt());dialog.exec();
}
}
