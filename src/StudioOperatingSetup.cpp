#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker {
bool ReloadMakerController::buildConfiguredSimulationRig(const OperatingSetup& setup,SimulationRig& rig,QString& error) {
    const auto fail=[&](const QString& why){error=why;return false;};
    if(!_reloadRecipe||setup.recipeId!=_reloadRecipe->id)return fail("The source assembly changed. Review and save its operating setup again.");
    const auto& recipe=*_reloadRecipe;auto parts=setup.parts;SimulationRig preset;
    const auto valid=[&](int track){return track>=0&&track<recipe.tracks.size();};
    if(!valid(parts.operating)||!valid(parts.bolt)||!valid(parts.magazine))return fail("An operating role is outside the current source tracks");
    if(parts.openFrame>=recipe.frames||parts.closedFrame>=recipe.frames)return fail("The selected movement window exceeds this capture");
    float duration=recipe.duration,ejection=setup.ejectSeconds;QJsonObject provenance;
    if(setup.mode=="preset") {
        if(!buildSourceSimulationRig(preset,error))return false;
        if(!preset.derivedHandle.isEmpty())parts.curves[preset.slide]=preset.derivedHandle;
        if(!preset.derivedBolt.isEmpty())parts.curves[preset.bolt]=preset.derivedBolt;
        provenance=preset.mechanicalSource;
    }else if(setup.mode=="clip") {
        const auto bytes=readAnimationResource(setup.clip,error),baseBytes=readAnimationResource(setup.baseline,error);
        const auto clip=HkxReader::readClip(bytes,error),base=HkxReader::readClip(baseBytes,error);
        if(!clip.valid()||!base.valid())return false;
        QVector<int> selected=parts.followers;if(!selected.contains(parts.bolt))selected.append(parts.bolt);
        for(int track:selected)if(!valid(track))return fail("A follower is missing from the current reload");
        QVector<Transformf> sampled,baseline,local,global,boundLocal,boundGlobal,sceneLocal,sceneWorld;
        if(!samplePose(_skeleton,base,0,&_readyLocalPose,sampled,baseline,global,error))return false;
        for(int frame=0;frame<recipe.frames;++frame){
            if(!samplePose(_skeleton,clip,clip.durationSeconds*frame/(recipe.frames-1),&baseline,sampled,local,global,error)||
               !evaluateSceneBindings(_skeleton,_nodeBindings,local,baseline,global,boundLocal,boundGlobal,error)||
               !nativeMechanismScene(_reloadScenePlan,_nodeBindings,boundLocal,sceneLocal,sceneWorld,error))return false;
            Transformf inverse;if(!invertUniformTransform(global[_weaponBoneIndex],inverse))return fail("Invalid native movement reference frame");
            for(int track:selected)parts.curves[track].append(composed(inverse,sceneWorld[_reloadTrackNodes[track]]));
        }
        duration=clip.durationSeconds;const auto hash=[](const QByteArray& data){return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());};
        if(ejection<0)for(const auto& event:clip.annotations)if(event.text.compare("EjectShellCasing",Qt::CaseInsensitive)==0){ejection=event.timeSeconds;break;}
        provenance={{"animation",setup.clip},{"animationSha256",hash(bytes)},{"baseline",setup.baseline},{"baselineSha256",hash(baseBytes)},{"duration",duration}};
    }else if(setup.mode=="manual") {
        QJsonArray definitions;
        for(auto it=setup.movements.cbegin();it!=setup.movements.cend();++it){
            if(!valid(it.key()))return fail("An authored movement names a missing part");
            const auto movement=std::find_if(_mechanisms.cbegin(),_mechanisms.cend(),[&](const auto& m){return m.id==it.value();});
            if(movement==_mechanisms.cend()||movement->stages.isEmpty()||!sameMechanismPart(movement->anchor,recipe.tracks[it.key()].anchor))return fail("Choose a saved movement authored on this exact native part");
            if(!validateMechanism(*movement,error))return false;
            QHash<int,Transformf> followers;
            for(const auto& follower:movement->followers){int found=-1;for(int i=0;i<recipe.tracks.size();++i)if(sameMechanismPart(follower.anchor,recipe.tracks[i].anchor)){if(found>=0)return fail("An authored follower is ambiguous");found=i;}
                if(found<0)return fail("An authored movement follower is not installed");followers[found]=follower.inPartAtRest;if(!parts.followers.contains(found))parts.followers.append(found);}
            if(parts.curves.contains(it.key()))return fail("Two authored movements control the same part");
            for(int track:followers.keys())if(parts.curves.contains(track)||setup.movements.contains(track))return fail("A rigid follower also has an independent movement; remove the duplicate ownership");
            const auto rest=recipe.tracks[it.key()].poses.back();
            for(int frame=0;frame<recipe.frames;++frame){
                const float u=float(frame)/(recipe.frames-1);const float path=movement->stages.size()*(simulationMechanism()==WeaponMechanism::BoltAction?u:1-u);
                const auto pose=composed(rest,mechanismDelta(*movement,path));parts.curves[it.key()].append(pose);
                for(auto f=followers.cbegin();f!=followers.cend();++f)parts.curves[f.key()].append(composed(pose,f.value()));
            }
            definitions.append(QJsonDocument::fromJson(encodeMechanism(*movement,error)).object());
        }
        for(int track:parts.followers)if(!parts.curves.contains(track))return fail("Assign a movement to every manual follower, or link it in the operating part's movement");
        duration=setup.duration;provenance={{"method","authored-part-local-stages"},{"definitions",definitions},{"duration",duration}};
    }
    const bool reversed=parts.openFrame>=0&&parts.closedFrame<parts.openFrame;
    if(reversed){auto tracks=parts.followers;if(!tracks.contains(parts.bolt))tracks.append(parts.bolt);
        for(int track:tracks){if(!valid(track))return fail("A follower is outside this capture");auto curve=parts.curves.value(track,recipe.tracks[track].poses);std::reverse(curve.begin(),curve.end());parts.curves[track]=std::move(curve);}
        parts.openFrame=recipe.frames-1-parts.openFrame;parts.closedFrame=recipe.frames-1-parts.closedFrame;provenance["reversedSourceWindow"]=true;}
    if(!buildSimulationRig(recipe,rig,error,simulationMechanism(),setup.deriveHandle,{},&parts))return false;
    if(setup.mode=="preset"&&!reversed){
        if(rig.slide==preset.slide)rig.derivedHandle=preset.derivedHandle;
        if(rig.bolt==preset.bolt)rig.derivedBolt=preset.derivedBolt;
    }
    rig.customSetup=true;rig.manualCycle=(setup.mode=="manual"||setup.mode=="clip")&&simulationMechanism()==WeaponMechanism::BoltAction;
    rig.fireFromStroke=setup.fireMode=="stroke";rig.presetFire=setup.fireMode=="preset";rig.manualEjectSeconds=ejection;
    rig.motionDuration=duration;rig.mechanicalSource=provenance;rig.authoring=operatingSetupJson(setup);return true;
}

QJsonObject ReloadMakerController::operatingSetupState() {
    if(!_reloadRecipe||_loadThread||_reloadCapturing||_reloadCaptureQueued||_rebuildTimer->isActive())throw std::runtime_error("Capture the current reload before configuring operating parts");
    QJsonArray tracks,movements,clips;for(int i=0;i<_reloadRecipe->tracks.size();++i){const auto& t=_reloadRecipe->tracks[i];tracks.append(QJsonObject{{"index",i},{"name",t.anchor.anchorName},{"label",t.label},{"path",t.anchor.anchorPath},{"model",t.anchor.ownerModel},{"surface",t.surface}});}
    for(const auto& m:_mechanisms)movements.append(QJsonObject{{"id",m.id},{"name",m.name},{"part",m.anchor.anchorName},{"model",m.anchor.ownerModel}});
    for(const auto& a:_animationActions)clips.append(QJsonObject{{"name",a.label},{"path",a.resourcePath},{"baseline",a.baselineResourcePath}});
    auto saved=_authoringRoadmaps.value(roadmapKey()).toObject().value("operating").toObject();const bool stale=!saved.isEmpty()&&saved["recipeId"]!=_reloadRecipe->id;
    QString error;OperatingSetup setup;setup.recipeId=_reloadRecipe->id;setup.baseline=_reloadRecipe->source["baseline"].toString();setup.clip=_reloadRecipe->source["animation"].toString();
    SimulationRig source;if(_simulationRig&&_simulationRig->recipeId==_reloadRecipe->id&&_simulationRig->mechanism==simulationMechanism())source=*_simulationRig;else if(!buildSourceSimulationRig(source,error))setup.mode="reload";
    setup.parts.operating=source.slide;setup.parts.bolt=source.bolt;setup.parts.magazine=source.magazine;setup.parts.followers=source.slideTracks;
    if(simulationMechanism()==WeaponMechanism::PistolSlide&&setup.parts.bolt<0)setup.parts.bolt=setup.parts.operating;
    if(!saved.isEmpty()&&!stale)(void)readOperatingSetup(saved,setup,error);
    return {{"settings",operatingSetupJson(setup)},{"saved",!saved.isEmpty()},{"stale",stale},{"tracks",tracks},{"movements",movements},{"clips",clips},
        {"frames",_reloadRecipe->frames},{"duration",_reloadRecipe->duration},{"suggestedOpenFrame",source.openFrame},{"suggestedClosedFrame",source.closedFrame},{"error",error}};
}
QJsonObject ReloadMakerController::configureOperatingSetup(const QJsonObject& request) {
    const auto key=roadmapKey();if(!_reloadRecipe||_loadThread||_reloadCapturing||_reloadCaptureQueued||_rebuildTimer->isActive()||key.isEmpty())throw std::runtime_error("Wait for the current source capture");
    auto plan=_authoringRoadmaps.value(key).toObject();if(!plan.contains("step"))plan["step"]=3;
    if(request["operation"]=="reset")plan.remove("operating");
    else if(request["operation"]=="save") {
        OperatingSetup setup;QString error;SimulationRig checked;
        if(!readOperatingSetup(request["settings"].toObject(),setup,error)||!buildConfiguredSimulationRig(setup,checked,error))throw std::runtime_error(error.toStdString());
        plan["operating"]=operatingSetupJson(setup);
    }else throw std::runtime_error("Choose save or reset for the operating setup");
    stopSimulation();_authoringRoadmaps[key]=plan;_simulationRig.reset();++_revision;prepareSimulation();
    if(!saveSession())throw std::runtime_error("Could not save operating setup");return operatingSetupState();
}

void ReloadMakerController::showOperatingSetup() {
    if(auto* existing=_window->findChild<QDialog*>("OperatingSetupDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=operatingSetupState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("OperatingSetupDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Operating parts and movement");dialog->resize(860,820);
    const auto initial=state["settings"].toObject();const auto recipe=initial["recipeId"].toString();auto* outer=new QVBoxLayout(dialog);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);outer->addWidget(scroll,1);auto* content=new QWidget;scroll->setWidget(content);auto* layout=new QVBoxLayout(content);
    auto* info=new QLabel("This setup belongs to the selected assembly. Pick the operating part and every moving follower. Native curves retain independent barrel, pin and cartridge motion. The viewport remains available while this window is open.");info->setWordWrap(true);layout->addWidget(info);
    auto* form=new QFormLayout;layout->addLayout(form);
    const auto trackPicker=[&](const QString& label,const char* key){auto* box=new QComboBox;box->addItem("Select a native part…",-1);
        for(const auto& value:state["tracks"].toArray()){const auto t=value.toObject();if(!t["surface"].toBool())continue;box->addItem(t["name"].toString()+" · "+t["label"].toString(),t["index"].toInt());box->setItemData(box->count()-1,t["model"].toString()+"\n"+t["path"].toString(),Qt::ToolTipRole);}
        box->setCurrentIndex(box->findData(initial[key].toInt(-1)));form->addRow(label,box);
        connect(box,qOverload<int>(&QComboBox::activated),dialog,[this,box,recipe](int){const int track=box->currentData().toInt();if(_reloadRecipe&&_reloadRecipe->id==recipe&&track>=0&&track<_reloadTrackNodes.size())_window->getGLView()->setCurrentIndex(_sceneNodes[_reloadTrackNodes[track]]);});return box;};
    auto* operating=trackPicker("Part the player pulls","operating");auto* bolt=trackPicker("Bolt / slide","bolt");auto* magazine=trackPicker("Full magazine carrier","magazine");
    auto* source=new QComboBox;source->addItem("Verified source behavior","preset");source->addItem("Current reload clip","reload");source->addItem("Another native clip","clip");source->addItem("Saved Move / Rotate movements","manual");source->setCurrentIndex(source->findData(initial["mode"].toString()));form->addRow("Movement source",source);
    auto* clip=new QComboBox;clip->setEditable(true);for(const auto& value:state["clips"].toArray()){const auto a=value.toObject();clip->addItem(a["path"].toString(),a["baseline"].toString());}clip->setEditText(initial["clip"].toString());form->addRow("Native movement clip",clip);
    auto* baseline=new QComboBox;baseline->setEditable(true);baseline->addItem(initial["baseline"].toString());form->addRow("Clip baseline",baseline);
    connect(clip,qOverload<int>(&QComboBox::activated),dialog,[clip,baseline](int){if(!clip->currentData().toString().isEmpty())baseline->setEditText(clip->currentData().toString());});
    auto* derive=new QCheckBox("Translate a static charging handle using the bolt's measured travel");derive->setChecked(initial["deriveHandle"].toBool());layout->addWidget(derive);
    auto* automatic=new QCheckBox("Detect the final open-to-closed window");automatic->setChecked(initial["openFrame"].toInt(-1)<0);layout->addWidget(automatic);
    auto* times=new QFormLayout;layout->addLayout(times);auto* open=new QSpinBox;auto* closed=new QSpinBox;
    for(auto* field:{open,closed})field->setRange(0,state["frames"].toInt()-1);
    open->setValue(automatic->isChecked()?state["suggestedOpenFrame"].toInt():initial["openFrame"].toInt());closed->setValue(automatic->isChecked()?state["suggestedClosedFrame"].toInt():initial["closedFrame"].toInt());times->addRow("Open sample",open);times->addRow("Closed sample",closed);
    auto* duration=new QDoubleSpinBox;duration->setRange(.01,120);duration->setDecimals(3);duration->setValue(initial["duration"].toDouble(.2));duration->setSuffix(" seconds");times->addRow("Manual movement duration",duration);
    auto* eject=new QDoubleSpinBox;eject->setRange(-1,120);eject->setDecimals(3);eject->setSpecialValueText("Source marker / manual full opening");eject->setValue(initial["ejectSeconds"].toDouble(-1));times->addRow("Bolt-cycle ejection time",eject);
    auto* fire=new QComboBox;fire->addItem("Verified firing behavior","preset");fire->addItem("Native firing clip","native");fire->addItem("Replay measured bolt stroke at firing cadence","stroke");fire->setCurrentIndex(fire->findData(initial["fireMode"].toString()));times->addRow("Firing bolt movement",fire);
    layout->addWidget(new QLabel("OPERATING FOLLOWERS · selected parts keep their complete curves"));auto* followers=new QListWidget;layout->addWidget(followers,1);
    for(const auto& value:state["tracks"].toArray()){const auto t=value.toObject();if(!t["surface"].toBool())continue;auto* row=new QListWidgetItem(t["name"].toString()+" · "+t["label"].toString(),followers);row->setData(Qt::UserRole,t["index"]);row->setFlags(row->flags()|Qt::ItemIsUserCheckable);row->setCheckState(initial["followers"].toArray().contains(t["index"])?Qt::Checked:Qt::Unchecked);row->setToolTip(t["model"].toString()+"\n"+t["path"].toString());}
    auto* movements=new QFormLayout;layout->addLayout(movements);QVector<QPair<int,QComboBox*>> assignments;
    for(const auto& value:state["tracks"].toArray()){const auto track=value.toObject();if(!track["surface"].toBool())continue;auto* choices=new QComboBox;choices->addItem("No saved movement",QString());
        for(const auto& m:_mechanisms)if(sameMechanismPart(m.anchor,_reloadRecipe->tracks[track["index"].toInt()].anchor))choices->addItem(m.name,m.id);
        if(choices->count()==1){delete choices;continue;}for(const auto& entry:initial["movements"].toArray())if(entry.toObject()["track"]==track["index"])choices->setCurrentIndex(choices->findData(entry.toObject()["id"].toString()));
        movements->addRow(track["name"].toString()+" movement",choices);assignments.append({track["index"].toInt(),choices});}
    auto* manualHelp=new QLabel("Create named movements with the existing Mechanism editor's Move/Rotate gizmos. Spring mechanisms use a closed-to-open path; manual bolts use the complete opening-and-closing sequence. Link rigid followers there, or assign independent movements here.");manualHelp->setWordWrap(true);layout->addWidget(manualHelp);
    auto* status=new QLabel(state["stale"].toBool()?"The previous setup belongs to an earlier source capture. Review these current tracks before saving.":state["error"].toString());status->setWordWrap(true);outer->addWidget(status);
    const auto enable=[=]{const auto mode=source->currentData().toString();clip->setEnabled(mode=="clip");baseline->setEnabled(mode=="clip");duration->setEnabled(mode=="manual");for(const auto& pair:assignments)pair.second->setEnabled(mode=="manual");manualHelp->setVisible(mode=="manual");open->setEnabled(!automatic->isChecked());closed->setEnabled(!automatic->isChecked());};
    connect(source,qOverload<int>(&QComboBox::currentIndexChanged),dialog,enable);connect(automatic,&QCheckBox::toggled,dialog,enable);enable();
    auto* actions=new QHBoxLayout;outer->addLayout(actions);auto* save=new QPushButton("Save and preview in Operate");auto* reset=new QPushButton("Restore source defaults");auto* author=new QPushButton("Open movement editor");actions->addWidget(save);actions->addWidget(reset);actions->addWidget(author);
    connect(save,&QPushButton::clicked,dialog,[=,this]{try{auto value=initial;value["operating"]=operating->currentData().toInt();value["bolt"]=bolt->currentData().toInt();value["magazine"]=magazine->currentData().toInt();value["mode"]=source->currentData().toString();value["clip"]=clip->currentText();value["baseline"]=baseline->currentText();value["deriveHandle"]=derive->isChecked();value["fireMode"]=fire->currentData().toString();value["duration"]=duration->value();value["ejectSeconds"]=eject->value();value["openFrame"]=automatic->isChecked()?-1:open->value();value["closedFrame"]=automatic->isChecked()?-1:closed->value();
        QJsonArray list,moves;for(int i=0;i<followers->count();++i){const auto* row=followers->item(i);if(row->checkState()==Qt::Checked)list.append(row->data(Qt::UserRole).toInt());}for(const auto& pair:assignments)if(!pair.second->currentData().toString().isEmpty())moves.append(QJsonObject{{"track",pair.first},{"id",pair.second->currentData().toString()}});value["followers"]=list;value["movements"]=moves;
        configureOperatingSetup({{"operation","save"},{"settings",value}});setWorkspace(OperateWorkspace);status->setText(_simulationError.isEmpty()?"Saved for this assembly. Test the complete stroke in the viewport.":_simulationError);
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{if(!_reloadRecipe||_reloadRecipe->id!=recipe)throw std::runtime_error("Source changed; reopen operating setup for the current assembly");configureOperatingSetup({{"operation","reset"}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});
    connect(author,&QPushButton::clicked,dialog,[=,this]{setWorkspace(GripsWorkspace);_handPosePages->setCurrentIndex(2);dialog->close();});
    dialog->show();
}
}
