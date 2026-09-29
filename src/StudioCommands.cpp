#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "ReloadGraph.h"
#include "glview.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQuaternion>
#include <QSaveFile>
#include <QSlider>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace reloadmaker {
namespace {
void require(bool condition, const QString& error) { if (!condition) throw std::runtime_error(error.toStdString()); }
float number(const QJsonValue& v) { require(v.isDouble() && std::isfinite(v.toDouble()), "Expected a finite number"); return float(v.toDouble()); }
Quaternionf rotation(const QJsonValue& v) {
    const auto a = v.toArray(); require(a.size()==4,"Expected quaternion x,y,z,w");
    Quaternionf q{number(a[0]),number(a[1]),number(a[2]),number(a[3])};
    require(std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1) < .0005f,"Expected a unit quaternion");
    return q;
}
QJsonObject frame(const Transformf& f) {
    return {{"translate",QJsonArray{f.translation.x,f.translation.y,f.translation.z}},
            {"rotation",QJsonArray{f.rotation.x,f.rotation.y,f.rotation.z,f.rotation.w}},
            {"scale",QJsonArray{f.scale.x,f.scale.y,f.scale.z}}};
}
void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit(),"Cannot save "+path+": "+file.errorString());
}
}

void ReloadMakerController::startCommandServer() {
    auto* server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    require(server->listen("ReloadMakerV2.local"),"Another Reload Maker V2 owns the local command pipe");
    connect(server,&QLocalServer::newConnection,this,[this,server] {
        while(server->hasPendingConnections()) {
            auto* socket=server->nextPendingConnection(); socket->setParent(this);
            auto data=std::make_shared<QByteArray>();
            connect(socket,&QLocalSocket::readyRead,this,[this,socket,data] {
                data->append(socket->readAll()); if(data->size()>4*1024*1024){socket->abort();return;}
                int newline;
                while((newline=data->indexOf('\n'))>=0) {
                    const auto line=data->left(newline);data->remove(0,newline+1);
                    QJsonObject response;
                    try {
                        QJsonParseError error;const auto doc=QJsonDocument::fromJson(line,&error);
                        require(error.error==QJsonParseError::NoError&&doc.isObject(),"Invalid command JSON");
                        response=executeCommand(doc.object());response["ok"]=true;
                    } catch(const std::exception& e) {response={{"ok",false},{"error",QString::fromUtf8(e.what())}};}
                    // Saved mechanisms/recipes own their revision field. Do
                    // not overwrite it with the live document generation.
                    response["documentRevision"]=double(_revision);
                    if(!response.contains("revision"))response["revision"]=double(_revision);
                    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');socket->flush();
                }
            });
            connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
        }
    });
    // UI edits and command edits share the same generation; clients must read
    // current state instead of reusing selections from an earlier scene.
    for(auto* w:_window->findChildren<QComboBox*>()) connect(w,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{++_revision;});
    for(auto* w:_window->findChildren<QDoubleSpinBox*>()) connect(w,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{++_revision;});
    for(auto* w:_window->findChildren<QLineEdit*>()) connect(w,&QLineEdit::textEdited,this,[this]{++_revision;});
    for(auto* w:_window->findChildren<QCheckBox*>()) connect(w,&QCheckBox::toggled,this,[this]{++_revision;});
    for(auto* w:_window->findChildren<QListWidget*>()) connect(w,&QListWidget::currentRowChanged,this,[this]{++_revision;});
}
QJsonObject ReloadMakerController::studioState() const {
    QJsonArray weapons,actions,sets,configurationSlots,anchors,poses,grips;
    for(int i=0;i<_weaponSelections.size();++i){const auto& selection=_weaponSelections[i];const auto& plugin=_plugins[selection.pluginIndex];const auto& w=plugin.weapons[selection.weaponIndex];weapons.append(QJsonObject{{"index",i},{"name",w.name},{"editorId",w.editorId},{"formId",QString::number(w.formId,16)},{"plugin",plugin.fileName}});}
    for(int i=0;i<_animationList->count();++i)actions.append(QJsonObject{{"index",i},{"name",_animationList->item(i)->text()},{"path",_animationList->item(i)->data(Qt::UserRole).toString()}});
    for(int i=0;i<_animationSetCombo->count();++i)sets.append(QJsonObject{{"index",i},{"name",_animationSetCombo->itemText(i)}});
    if(const auto* config=configuration())for(const auto& slot:config->entries()){
        QJsonArray choices;for(auto id:slot.choices)if(const auto* m=config->mod(id))choices.append(QJsonObject{{"id",QString::number(id)},{"name",m->name},{"editorId",m->editorId},{"model",m->modelPath}});
        configurationSlots.append(QJsonObject{{"key",QString::number(slot.key)},{"name",slot.label},{"selected",QString::number(config->selected(slot.key))},{"choices",choices}});
    }
    for(int i=0;i<_handPoseAnchors.size();++i)anchors.append(QJsonObject{{"index",i},{"name",_handPoseAnchor->itemText(i)},{"kind",_handPoseAnchors[i].description.anchorKind}});
    for(const auto& p:_savedHandPoses)poses.append(QJsonObject{{"id",p.id},{"name",p.name},{"hand",p.hand==PoseHand::Left?"left":"right"}});
    for(const auto& p:_partGrips)grips.append(QJsonObject{{"id",p.pose.id},{"name",p.pose.name},{"part",p.pose.placement.anchorName}});
    return {{"revision",double(_revision)},{"busy",bool(_loadThread)||_reloadCapturing||_rebuildTimer->isActive()},{"loading",bool(_loadThread)},
        {"capturing",_reloadCapturing},{"source",_package.sourcePath},{"gameData",_gameData},{"library",_libraryMode},
        {"workspace",_workspace},{"weaponIndex",_weaponCombo->currentIndex()},{"weapons",weapons},{"slots",configurationSlots},
        {"body",_bodyCombo->currentData().toString()},{"animationSets",sets},{"animationSet",_animationSetCombo->currentIndex()},
        {"actions",actions},{"action",_activeAction},{"animation",_activeAnimationPath},{"seconds",_playbackTime},{"duration",playbackDuration()},
        {"playing",_playing||_reloadPlaying},{"motionMode",_motionMode->currentIndex()},{"anchors",anchors},
        {"handPoses",poses},{"partGrips",grips},{"recipeWindows",_reloadRecipe?int(_reloadRecipe->windows.size()):0},
        {"recipeTracks",_reloadRecipe?int(_reloadRecipe->tracks.size()):0},{"status",_window->statusBar()->currentMessage()},
        {"lastError",_lastError},{"project",_sessionPath},{"nativeApplicationShell",false}};
}
QJsonObject ReloadMakerController::executeCommand(const QJsonObject& request) {
    const QString method=request["method"].toString();const auto p=request["params"].toObject();
    const bool reading=method=="state"||method=="pose"||method=="render"||method=="reload"||method=="diagnostics"||method=="simulation_state";
    if(!reading&&request.contains("expectedRevision"))require(request["expectedRevision"].toDouble()==double(_revision),"Stale document revision; read state again");
    if(method=="state")return studioState();
    if(method=="simulation_state")return simulationState();
    if(method=="focus") {if(_window->isMinimized())_window->showNormal();_window->raise();_window->activateWindow();return studioState();}
    if(method=="diagnostics") {
        QJsonArray remaps;
        for(const auto& r:_remappedAttachments) {
            Transformf world,parent,root;QString error;
            if(!sceneNodeWorld(r.node,world,error)||!sceneNodeWorld(r.parent,parent,error)||!sceneNodeWorld(_partRoots.value(r.slot),root,error))continue;
            remaps.append(QJsonObject{{"keyword",r.keyword},{"point",r.point},{"slot",QString::number(r.slot)},
                {"world",frame(world)},{"parentWorld",frame(parent)},{"attachmentWorld",frame(root)}});
        }
        return {{"text",_details->toPlainText()},{"viewport",_window->getGLView()->studioDiagnostics()},{"attachmentRemaps",remaps}};
    }
    if(method=="pose") {
        QJsonArray bones;
        for(int i=0;i<_localPose.size();++i)bones.append(QJsonObject{{"index",i},{"name",_skeleton.bones[i].name},{"local",frame(_localPose[i])},{"world",frame(_globalPose[i])}});
        return {{"source",currentPoseSource()},{"bones",bones}};
    }
    if(method=="reload") {require(_reloadRecipe&&!_reloadCapturing,"No captured reload");QString error;const auto bytes=encodeReloadRecipe(*_reloadRecipe,error);require(error.isEmpty(),error);return QJsonDocument::fromJson(bytes).object();}
    if(method=="cancel") {if(_loadThread)_loadThread->requestInterruption();if(_reloadCapturing)invalidateReloadCapture();return studioState();}
    require(!_loadThread&&!_reloadCapturing&&!_rebuildTimer->isActive(),"Source loading, assembly or reload capture is in progress; read state or cancel");
    _lastError.clear();
    if(method=="open_source") {
        const QString path=p["path"].toString();require(QFileInfo(path).exists(),"Source folder or ZIP does not exist");
        startPackageLoad(path,p["gameData"].toString(_gameDataPath->text()));
    } else if(method=="open_library") {
        if(p.contains("instance")) { _mo2Path->setText(p["instance"].toString()); }
        startLibraryLoad();
    } else if(method=="workspace") {const int index=p["index"].toInt(-1);require(index>=0&&index<=5,"Unknown workspace");setWorkspace(index);}
    else if(method=="simulate") {
        require(_workspace==5&&_simulationActive,"Open the Simulate workspace and wait until ready");
        const auto operation=p["operation"].toString();
        if(operation=="pull") {
            const float progress=number(p["progress"]);require(progress>=0&&progress<=1.08f,"Slide pull must be between 0 and 1.08; above 1 releases the catch");
            require(_simulation.action==SimulationAction::Held||_simulation.grab(),"Finish the native action before pulling");
            _simulationTimer->stop();dragSimulation(progress);
        } else if(operation=="release")releaseSimulation(p["cancel"].toBool());
        else if(operation=="move_magazine") {
            require(_magazineRail.has_value(),"Magazine rail is unavailable");
            if(p.contains("distance")){require(_simulation.magazine.phase==MagazinePhase::Rail,"The magazine is not on the rail");moveMagazineRail(number(p["distance"]));}
            else {require(_simulation.magazine.phase==MagazinePhase::Free,"Grab a magazine before moving it freely");const auto a=p["position"].toArray();require(a.size()==3,"Position needs three weapon-local offset components");moveFreeMagazine({number(a[0]),number(a[1]),number(a[2])});}
        } else if(operation=="cancel_magazine"){require(_simulation.magazine.carried(),"No cursor-held magazine to cancel");cancelMagazine();}
        else if(operation=="spring") {_simulationSpring->setChecked(p["enabled"].toBool(true));}
        else {require(operation=="fire"||operation=="reload"||operation=="drop_magazine"||operation=="reset"||operation=="mag_out"||operation=="mag_in"||operation=="grab_magazine"||operation=="slide_release","Unknown simulation operation");runSimulationAction(operation);}
        require(_lastError.isEmpty(),_lastError);++_revision;return simulationState();
    }
    else if(method=="select_weapon") {
        const int index=p["index"].toInt(-1);require(index>=0&&index<_weaponCombo->count(),"Unknown weapon index");_weaponCombo->setCurrentIndex(index);rebuildWeapon();
    } else if(method=="configure") {
        bool slotOk=false,modOk=false;
        const auto slot=p["slot"].toString().toULongLong(&slotOk);
        const auto mod=p["mod"].toString().toUInt(&modOk);
        if(p.contains("slot"))require(slotOk&&modOk&&p["slot"].isString()&&p["mod"].isString(),"Slot and modification IDs must be decimal strings from the current state");
        if(p.contains("body"))require(_bodyCombo->findData(p["body"].toString())>=0,"Unknown body");
        if(p.contains("animationSet"))require(p["animationSet"].toInt(-1)>=0&&p["animationSet"].toInt(-1)<_animationSetCombo->count(),"Invalid animation set");
        if(p.contains("body")){const int index=_bodyCombo->findData(p["body"].toString());require(index>=0,"Unknown body");_bodyCombo->setCurrentIndex(index);}
        if(p.contains("slot")){auto* config=configuration();require(config&&config->select(slot,mod),"Invalid slot or modification");refreshAttachments();refreshConfigurationChoices();}
        if(p.contains("animationSet")){int index=p["animationSet"].toInt(-1);require(index>=0&&index<_animationSetCombo->count(),"Invalid animation set");_animationSetCombo->setCurrentIndex(index);}
        rebuildWeapon();
    } else if(method=="action") {
        int index=p["index"].toInt(-1);require(index>=0&&index<_animationList->count(),"Invalid action index");_animationList->setCurrentRow(index);selectAnimation(_animationList->item(index));
    } else if(method=="playback") {
        require(_clip.valid(),"Load a configured animation first");
        if(p.contains("mode")){const int mode=p["mode"].toInt(-1);require(mode==0||mode==1,"Mode must be 0 fixed or 1 native");_motionMode->setCurrentIndex(mode);}
        if(p.contains("seconds")){float t=number(p["seconds"]);const float duration=_reloadWorkspaceActive&&_reloadRecipe?_reloadRecipe->duration:playbackDuration();require(t>=0&&t<=duration,"Time is outside this animation");_playing=false;_playbackTimer->stop();if(_reloadWorkspaceActive){_reloadPlaying=false;_reloadPreviewTimer->stop();seekReload(t);}else{_playbackTime=t;require(applyPose(t),_poseError);updateTimelineText();}}
        if(p.contains("playing")) {
            if(_reloadWorkspaceActive){require(_reloadRecipe.has_value(),"Learn the reload first");if(p["playing"].toBool()!=_reloadPlaying)_reloadPlay->click();}
            else if(p["playing"].toBool()!=_playing)togglePlayback();
        }
        if(p.contains("loop"))_loopCheck->setChecked(p["loop"].toBool());
    } else if(method=="view"||method=="render") {
        const QString view=p["view"].toString("current");
        const QHash<QString,GLView::ViewState> views{{"front",GLView::ViewFront},{"back",GLView::ViewBack},{"side",GLView::ViewRight},{"top",GLView::ViewTop},{"iso",GLView::ViewDefault}};
        if(view!="current"){
            require(views.contains(view),"Unknown camera view");
            _window->getGLView()->setOrientation(views[view],false);
            if(view=="iso")_window->getGLView()->setRotation(-65,0,135);
        }
        if(p["fit"].toBool())_window->getGLView()->center();
        if(p.contains("zoom")){float z=number(p["zoom"]);require(z>.05f&&z<20,"Zoom outside supported range");_window->getGLView()->setZoom(z);}
        if(method=="render"){
            const QString path=studioDataPath("evidence/view-"+QString::number(_revision)+"-"+view+".png");
            auto image=_window->getGLView()->studioCaptureFrame();require(!image.isNull()&&image.save(path),"Cannot capture current native frame");return {{"path",path},{"seconds",_playbackTime},{"width",image.width()},{"height",image.height()}};
        }
    } else if(method=="restore_hand") {
        const QString side=p["hand"].toString();require(side=="left"||side=="right","Choose left or right");
        const auto hand=side=="left"?PoseHand::Left:PoseHand::Right;_fingerDrafts[int(hand)].reset();
        if(_handPosePreview&&_handPosePreview->hand==hand)_handPosePreview.reset();
        if(_clip.valid())require(applyPose(_playbackTime),_poseError);
        refreshFingerEditor();
    } else if(method=="edit_fingers") {
        const QString side=p["hand"].toString();require(side=="left"||side=="right","Choose left or right");
        const auto edits=p["joints"].toArray();require(!edits.isEmpty()&&edits.size()<=15,"Expected 1–15 joint edits");
        QVector<std::pair<int,Quaternionf>> valid;
        for(auto v:edits){const auto e=v.toObject();const int i=e["index"].toInt(-1);require(i>=0&&i<15,"Invalid finger joint");valid.append({i,rotation(e["rotation"])});}
        setWorkspace(2);_handPosePages->setCurrentIndex(0);
        _handPoseSide->setCurrentIndex(_handPoseSide->findData(int(side=="left"?PoseHand::Left:PoseHand::Right)));
        for(const auto& [i,q]:valid)editFinger(i,q);
    } else if(method=="save_hand_pose") {
        const auto hand=p["hand"].toString();require(hand=="left"||hand=="right","Choose left or right");require(!p["name"].toString().trimmed().isEmpty(),"Name the pose");
        setWorkspace(2);_handPosePages->setCurrentIndex(0);
        _handPoseSide->setCurrentIndex(_handPoseSide->findData(int(hand=="left"?PoseHand::Left:PoseHand::Right)));
        _handPoseName->setText(p["name"].toString());_handPoseNotes->setPlainText(p["notes"].toString());
        updateHandPoseControls();require(_saveHandPose->isEnabled(),"The current native hand cannot be captured");saveCurrentHandPose();
    } else if(method=="start_grip") {
        int pose=_gripPoseCombo->findData(p["poseId"].toString());int anchor=_gripReferenceCombo->findData(p["anchor"].toInt(-1));require(pose>=0&&anchor>=0,"Unknown saved pose or reference part");
        setWorkspace(2);_handPosePages->setCurrentIndex(1);_gripPoseCombo->setCurrentIndex(pose);_gripReferenceCombo->setCurrentIndex(anchor);startPartGrip(p["captured"].toBool(true));
    } else if(method=="set_grip") {
        require(_partGripDraft.has_value(),"Start a part grip first");auto candidate=*_partGripDraft;QString error;
        require(decodePoseTransform(p["transform"],candidate.pose.placement.handInAnchor,error),error);
        const auto previous=_partGripDraft;const auto previousPreview=_handPosePreview;
        _partGripDraft=candidate;_gripName->setText(p["name"].toString(candidate.pose.name));_partGripDraft->pose.name=_gripName->text();
        if(!applyPose(_playbackTime)) {
            const QString failure=_poseError;_partGripDraft=previous;_handPosePreview=previousPreview;
            (void)applyPose(_playbackTime);showPartGripValues();require(false,failure);
        }
        showPartGripValues();
    } else if(method=="save_grip") {require(_partGripDraft.has_value(),"No grip draft");require(savePartGrip(),_lastError);}
    else if(method=="select_part") {int anchor=p["anchor"].toInt(-1);require(anchor>0&&anchor<_handPoseAnchors.size(),"Choose a concrete part");setWorkspace(3);selectWorkshopPart(anchor);}
    else if(method=="mechanism") {
        require(_mechanismDraft.has_value(),"Select a part in Mechanisms first");
        if(p.contains("record")){PartMechanism candidate;QString error;require(decodeMechanism(QJsonDocument(p["record"].toObject()).toJson(),candidate,error),error);require(sameMechanismPart(candidate.anchor,_mechanismDraft->anchor),"Mechanism targets another part");_mechanismDraft=candidate;displayMechanism();}
        const QString operation=p["operation"].toString("read");
        if(operation=="set_stage") {
            auto candidate=*_mechanismDraft;const int index=p["stage"].toInt(0);
            require(index>=0&&index<=candidate.stages.size()&&index<32,"Unknown mechanism stage");
            if(index==candidate.stages.size())candidate.stages.append(MechanismStage{});
            auto& stage=candidate.stages[index];
            if(p.contains("axis")){const int axis=p["axis"].toInt(-1);require(axis>=0&&axis<=2,"Axis must be 0, 1 or 2");stage.axis=axis;}
            if(p.contains("rotation"))stage.rotation=p["rotation"].toBool();
            if(p.contains("from"))stage.from=number(p["from"]);
            if(p.contains("to"))stage.to=number(p["to"]);
            if(p.contains("returnRate"))stage.returnRate=number(p["returnRate"]);
            if(p.contains("release")){const QString rule=p["release"].toString();require(rule=="hold"||rule=="start"||rule=="rest","Unknown release rule");stage.release=rule=="hold"?ReleaseRule::Hold:rule=="start"?ReleaseRule::StageStart:ReleaseRule::Rest;}
            if(p.contains("name"))stage.forwardName=p["name"].toString();
            if(p.contains("pivot")){const auto a=p["pivot"].toArray();require(a.size()==3,"Pivot needs three components");stage.pivot={number(a[0]),number(a[1]),number(a[2])};}
            QString error;require(validateMechanism(candidate,error),error);_mechanismDraft=candidate;displayMechanism();
            if(_mechanismPreview)require(applyPose(_playbackTime),_poseError);
        } else if(operation=="preview")beginMechanismPreview();else if(operation=="stop"){stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);}
        else if(operation=="seek"){require(_mechanismPreview,"Start mechanism preview first");const float path=number(p["path"]);require(path>=0&&path<=_mechanismDraft->stages.size(),"Path outside mechanism stages");_mechanismState.path=path;_mechanismState.activeStage=std::min(int(path),int(_mechanismDraft->stages.size())-1);_mechanismState.returning=false;require(applyPose(_playbackTime),_poseError);}
        else if(operation=="release")releaseMechanismGrab();else if(operation=="save")saveWorkshop();else require(operation=="read","Unknown mechanism operation");
        QString error;const auto bytes=encodeMechanism(*_mechanismDraft,error);require(error.isEmpty(),error);++_revision;return QJsonDocument::fromJson(bytes).object();
    } else if(method=="learn_reload") {setWorkspace(4);if(!_reloadCapturing&&!_reloadRecipe)startReloadCapture();}
    else if(method=="edit_reload") {
        require(_reloadRecipe&&!_reloadCapturing,"Learn a reload first");auto candidate=*_reloadRecipe;QString error;
        const int index=p["window"].toInt(-1);require(index>=0&&index<candidate.windows.size(),"Invalid reload window");
        const auto operation=p["operation"].toString("update");
        if(operation=="split")require(splitReloadWindow(candidate,index,p["frame"].toInt(-1),error),error);
        else if(operation=="merge")require(mergeReloadWindow(candidate,index,error),error);
        else {require(operation=="update","Unknown reload edit");auto& w=candidate.windows[index];if(p.contains("name")){require(!p["name"].toString().trimmed().isEmpty(),"Window needs a name");w.label=p["name"].toString().trimmed();}if(p.contains("returnOnRelease"))w.returnOnRelease=p["returnOnRelease"].toBool();
            if(p.contains("track")){const int track=p["track"].toInt(-1);auto it=std::find_if(w.members.begin(),w.members.end(),[track](const auto& m){return m.track==track;});require(it!=w.members.end(),"Track is not in this window");if(p.contains("grabbable")){require(!p["grabbable"].toBool()||candidate.tracks[track].surface,"This track has no grabbable surface");it->grabbable=p["grabbable"].toBool();}if(p.contains("gripId")){const auto id=p["gripId"].toString();require(id.isEmpty()||std::any_of(_partGrips.begin(),_partGrips.end(),[&](const auto& g){return g.pose.id==id;}),"Unknown part grip");it->gripId=id;}}
        }
        require(!encodeReloadRecipe(candidate,error).isEmpty(),error);_reloadRecipe=candidate;refreshReloadWindows();selectReloadWindow(std::min(index,int(candidate.windows.size())-1));
    } else if(method=="import_records") {
        const auto paths=p["paths"].toArray();require(!paths.isEmpty()&&paths.size()<=1000,"Choose 1–1000 authored JSON records");
        struct Pending {QString path;QByteArray bytes;};QVector<Pending> pending;QJsonArray destinations;
        for(const auto& entry:paths) {
            QFile input(entry.toString());require(input.open(QIODevice::ReadOnly)&&input.size()<=64*1024*1024,"Cannot read authored record: "+entry.toString());
            const auto bytes=input.readAll();QString error,id,directory;QByteArray validated;
            const auto kind=QJsonDocument::fromJson(bytes).object()["format"].toString();
            if(kind=="reload-maker-hand-pose"){HandPose v;require(decodeHandPose(bytes,v,error),error);id=v.id;directory=HandPoseLibrary::defaultDirectory();validated=encodeHandPose(v,error);}
            else if(kind=="reload-maker-part-mechanism"){PartMechanism v;require(decodeMechanism(bytes,v,error),error);id=v.id;directory=MechanismLibrary::defaultDirectory();validated=encodeMechanism(v,error);}
            else if(kind=="reload-maker-reload-recipe"){ReloadRecipe v;require(decodeReloadRecipe(bytes,v,error),error);id=v.id;directory=ReloadRecipeLibrary::defaultDirectory();validated=encodeReloadRecipe(v,error);}
            else {PartGrip v;require(decodePartGrip(bytes,v,error),error);id=v.pose.id;directory=PartGripLibrary::defaultDirectory();validated=encodePartGrip(v,error);}
            require(error.isEmpty()&&!validated.isEmpty(),error);
            // Decoding/encoding verifies the contract, but importing preserves
            // the original record. Re-normalizing its float matrices would
            // otherwise turn an identical re-import into a false ID conflict.
            validated=bytes;
            const QString destination=QDir(directory).filePath(id+".json");QFile existing(destination);
            if(existing.exists()){require(existing.open(QIODevice::ReadOnly)&&QJsonDocument::fromJson(existing.readAll())==QJsonDocument::fromJson(validated),"A different record already owns this ID: "+destination);}
            else {
                const auto staged=std::find_if(pending.begin(),pending.end(),[&](const auto& v){return v.path==destination;});
                if(staged!=pending.end())require(QJsonDocument::fromJson(staged->bytes)==QJsonDocument::fromJson(validated),"Conflicting IDs within the import batch: "+destination);
                else pending.append({destination,validated});
            }
            destinations.append(destination);
        }
        for(const auto& file:pending)write(file.path,file.bytes);
        refreshHandPoseLibrary();++_revision;return {{"files",destinations},{"imported",pending.size()}};
    } else if(method=="save_reload") {require(_reloadRecipe.has_value(),"No reload recipe");QString error;require(_reloadRecipeLibrary.save(*_reloadRecipe,error),error);}
    else if(method=="save_project") {require(saveSession(p["path"].toString()),_lastError);}
    else if(method=="open_project") {openSession(p["path"].toString());}
    else if(method=="export") {
        QString error;QByteArray bytes;const QString kind=p["kind"].toString();QString id;
        if(kind=="simulation"){const auto record=exportSimulation();require(!record.isEmpty(),"Prepare a simulation before exporting");bytes=QJsonDocument(record).toJson();id=_reloadRecipe->id;}
        else if(kind=="reload"){require(_reloadRecipe.has_value(),"No reload recipe");bytes=encodeReloadRecipe(*_reloadRecipe,error);id=_reloadRecipe->id;}
        else if(kind=="grip"){require(_partGripDraft.has_value(),"No grip draft");bytes=encodePartGrip(*_partGripDraft,error);id=_partGripDraft->pose.id;}
        else if(kind=="mechanism"){require(_mechanismDraft.has_value(),"No mechanism draft");bytes=encodeMechanism(*_mechanismDraft,error);id=_mechanismDraft->id;}
        else if(kind=="hand"){const auto* pose=selectedHandPose();require(pose,"Choose a saved hand pose");bytes=encodeHandPose(*pose,error);id=pose->id;}
        else require(false,"Export kind must be hand, grip, mechanism, reload or simulation");
        require(error.isEmpty()&&!bytes.isEmpty(),error);const QString path=studioDataPath("exports/"+kind+"-"+id+".json");write(path,bytes);return {{"path",path},{"kind",kind},{"deployed",false}};
    } else require(false,"Unknown studio command: "+method);
    require(_lastError.isEmpty(),_lastError);++_revision;return studioState();
}
}
