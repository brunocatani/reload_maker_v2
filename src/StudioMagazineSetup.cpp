#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include "model/nifmodel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker {
QString ReloadMakerController::magazinePiecePath(const QModelIndex& leaf) const {
    auto* model=activeModel();if(!model)return {};QStringList path;auto node=leaf;
    for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth){auto name=model->get<QString>(node,"Name");if(name.isEmpty())name=QString("unnamed:%1").arg(model->getBlockNumber(node));path.prepend(name);node=model->getBlockIndex(model->getParent(node));}
    return path.join('/');
}
bool ReloadMakerController::sampledMagazinePayload() const {return _magazineSetup?_magazineSetup->sampledPayload:isM1AConfiguration();}
QJsonObject ReloadMakerController::magazineSetupState() const {
    if(!_reloadRecipe||!_simulationRig||_loadThread||_reloadCapturing||_reloadCaptureQueued||_rebuildTimer->isActive())throw std::runtime_error("Prepare the operating roles and native magazine first");
    const auto saved=_authoringRoadmaps.value(roadmapKey()).toObject().value("magazineSetup").toObject();
    if(!_magazineRail&&saved.isEmpty())throw std::runtime_error((_magazineError.isEmpty()?QString("The magazine source is not ready"):_magazineError).toStdString());
    QJsonArray pieces,tracks,movements;MagazineSetup value;value.recipeId=_reloadRecipe->id;
    value.emptyTrack=_magazineRail?_magazineRail->emptyTrack:_simulationRig->magazine;value.sampledPayload=isM1AConfiguration();
    value.loadedFrame=value.sampledPayload?_m1aLoadedMagazineFrame:0;value.emptyFrame=value.sampledPayload?_m1aEmptyMagazineFrame:0;
    value.entrySeconds=_reloadRecipe->duration*value.loadedFrame/(_reloadRecipe->frames-1);
    QSet<QString> seen;
    for(const auto& v:_magazineVisuals){const auto path=magazinePiecePath(v.node);if(seen.contains(path))throw std::runtime_error("Magazine pieces have ambiguous native paths");seen.insert(path);
        const auto role=v.nativeOnly?"spare":v.cartridge?"cartridge":"body";value.pieces[path]=role;
        pieces.append(QJsonObject{{"path",path},{"name",activeModel()->get<QString>(v.node,"Name")},{"role",role},{"empty",v.empty},{"track",v.track}});}
    for(int i=0;i<_reloadRecipe->tracks.size();++i){const auto& track=_reloadRecipe->tracks[i];if(track.surface&&std::any_of(_magazineVisuals.cbegin(),_magazineVisuals.cend(),[&](const auto& v){return v.track==i&&!v.cartridge;}))tracks.append(QJsonObject{{"index",i},{"name",track.anchor.anchorName},{"model",track.anchor.ownerModel}});}
    for(const auto& m:_mechanisms)if(sameMechanismPart(m.anchor,_reloadRecipe->tracks[_simulationRig->magazine].anchor))movements.append(QJsonObject{{"id",m.id},{"name",m.name}});
    return {{"movements",movements},{"settings",saved.isEmpty()?magazineSetupJson(value):saved},{"saved",!saved.isEmpty()},{"stale",!saved.isEmpty()&&saved["recipeId"]!=_reloadRecipe->id},
        {"fullTrack",_simulationRig->magazine},{"fullName",_reloadRecipe->tracks[_simulationRig->magazine].anchor.anchorName},{"tracks",tracks},{"pieces",pieces},
        {"frames",_reloadRecipe->frames},{"duration",_reloadRecipe->duration},{"error",_magazineError}};
}
QJsonObject ReloadMakerController::configureMagazineSetup(const QJsonObject& request) {
    if(!_reloadRecipe||!_simulationRig||_loadThread||_reloadCapturing||_reloadCaptureQueued||_rebuildTimer->isActive())throw std::runtime_error("Wait for the native magazine source");
    const auto key=roadmapKey();const auto previous=_authoringRoadmaps.value(key);auto plan=previous.toObject();if(!plan.contains("step"))plan["step"]=5;
    if(request["operation"]=="reset")plan.remove("magazineSetup");
    else if(request["operation"]=="save") {
        MagazineSetup value;QString error;if(!readMagazineSetup(request["settings"].toObject(),value,error))throw std::runtime_error(error.toStdString());
        if(value.recipeId!=_reloadRecipe->id||value.emptyTrack>=_reloadRecipe->tracks.size()||value.loadedFrame>=_reloadRecipe->frames||value.emptyFrame>=_reloadRecipe->frames)throw std::runtime_error("Magazine recipe changed; review the current parts and samples");
        const auto state=magazineSetupState();bool body=false;for(const auto& row:state["tracks"].toArray())body|=row.toObject()["index"].toInt()==value.emptyTrack;
        if(!body)throw std::runtime_error("Choose an empty body from this magazine assembly");
        const auto descendant=[&](int child,int parent){for(int depth=0;child>=0&&depth<_reloadRecipe->tracks.size();child=_reloadRecipe->tracks[child].parent,++depth)if(child==parent)return true;return false;};
        if(value.emptyTrack!=_simulationRig->magazine&&(descendant(value.emptyTrack,_simulationRig->magazine)||descendant(_simulationRig->magazine,value.emptyTrack)))throw std::runtime_error("Separate full and empty carriers must be independent branches; choose the full carrier for a shared body");
        if(value.motion=="native"&&(value.entrySeconds<=0||value.entrySeconds>=_reloadRecipe->duration))throw std::runtime_error("Native insertion must begin inside the reload clip");
        const auto pieces=state["pieces"].toArray();if(pieces.size()!=value.pieces.size())throw std::runtime_error("Review all native magazine pieces");
        for(const auto& row:pieces)if(!value.pieces.contains(row.toObject()["path"].toString()))throw std::runtime_error("A saved piece is not in this source");
        plan["magazineSetup"]=magazineSetupJson(value);
    }else throw std::runtime_error("Choose save or reset for magazine presentation");
    stopSimulation();_authoringRoadmaps[key]=plan;prepareSimulation();
    if(!_magazineError.isEmpty()||!_simulationError.isEmpty()) {
        const auto failure=_magazineError.isEmpty()?_simulationError:_magazineError;stopSimulation();
        if(previous.isUndefined())_authoringRoadmaps.remove(key);else _authoringRoadmaps[key]=previous;
        prepareSimulation();throw std::runtime_error(failure.toStdString());
    }
    ++_revision;if(!saveSession())throw std::runtime_error("Could not save magazine presentation");return magazineSetupState();
}
void ReloadMakerController::showMagazineSetup() {
    if(auto* existing=_window->findChild<QDialog*>("MagazineSetupDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=magazineSetupState();}catch(const std::exception& e){setStatus(e.what(),true);if(_simulationRig&&_reloadRecipe)showMagazineReference();return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("MagazineSetupDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Magazine models and payload");dialog->resize(880,710);
    auto* outer=new QVBoxLayout(dialog);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);outer->addWidget(scroll);auto* content=new QWidget;scroll->setWidget(content);auto* layout=new QVBoxLayout(content);auto* info=new QLabel("Review the installed magazine's full and empty presentations. Mark cartridges separately from the body and animation-only spares. Full magazine carrier: "+state["fullName"].toString()+" (selected in Operating parts). Select a piece below to highlight it in the viewport.");info->setWordWrap(true);layout->addWidget(info);
    const auto initial=state["settings"].toObject();const auto recipe=_reloadRecipe->id;auto* form=new QFormLayout;layout->addLayout(form);
    auto* empty=new QComboBox;for(const auto& row:state["tracks"].toArray()){const auto t=row.toObject();empty->addItem(t["name"].toString()+(t["index"]==state["fullTrack"]?" · shared full/empty body":""),t["index"].toInt());}empty->setCurrentIndex(empty->findData(initial["emptyTrack"].toInt()));form->addRow("Empty magazine body",empty);
    auto* motion=new QComboBox;motion->addItem("Verified source behavior","preset");motion->addItem("Linear extraction","linear");motion->addItem("Native insertion, including rotation","native");motion->addItem("Saved Move / Rotate path","manual");motion->setCurrentIndex(motion->findData(initial["motion"].toString()));form->addRow("Visible movement",motion);
    auto* movement=new QComboBox;movement->addItem("Select a saved movement…",QString());for(const auto& v:state["movements"].toArray()){const auto m=v.toObject();movement->addItem(m["name"].toString(),m["id"].toString());}movement->setCurrentIndex(movement->findData(initial["movementId"].toString()));form->addRow("Authored magazine path",movement);
    auto* entry=new QDoubleSpinBox;entry->setDecimals(4);entry->setRange(0,state["duration"].toDouble());entry->setValue(initial["entrySeconds"].toDouble());entry->setSuffix(" seconds");form->addRow("Native approach begins",entry);
    auto* sampled=new QCheckBox("Sample payload relative to its carrier (pre-feed full / empty follower)");sampled->setChecked(initial["sampledPayload"].toBool());layout->addWidget(sampled);
    const auto sample=[&](const char* key,const QString& title){auto* spin=new QSpinBox;spin->setRange(0,state["frames"].toInt()-1);spin->setValue(initial[key].toInt());form->addRow(title,spin);return spin;};
    auto* loaded=sample("loadedFrame","Full pre-feed sample");auto* depleted=sample("emptyFrame","Empty payload sample");
    auto* tree=new QTreeWidget;tree->setColumnCount(3);tree->setHeaderLabels({"Native piece","Presentation","Role"});layout->addWidget(tree,1);QVector<QPair<QString,QComboBox*>> roles;
    for(const auto& row:state["pieces"].toArray()){const auto p=row.toObject();auto* item=new QTreeWidgetItem(tree,QStringList{p["name"].toString(),p["empty"].toBool()?"Empty":"Full"});item->setToolTip(0,p["path"].toString());item->setData(0,Qt::UserRole,p["path"]);
        auto* choices=new QComboBox;choices->addItem("Magazine body / follower","body");choices->addItem("Cartridge · hidden when empty","cartridge");choices->addItem("Animation-only spare · excluded","spare");choices->setCurrentIndex(choices->findData(p["role"].toString()));tree->setItemWidget(item,2,choices);roles.append({p["path"].toString(),choices});}
    tree->setColumnWidth(0,260);tree->setColumnWidth(1,95);
    connect(tree,&QTreeWidget::currentItemChanged,dialog,[=,this](QTreeWidgetItem* item,QTreeWidgetItem*){if(!item||!_reloadRecipe||_reloadRecipe->id!=recipe)return;for(const auto& v:_magazineVisuals)if(magazinePiecePath(v.node)==item->data(0,Qt::UserRole).toString()){_window->getGLView()->setCurrentIndex(v.node);break;}});
    auto* status=new QLabel(state["error"].toString());status->setWordWrap(true);layout->addWidget(status);auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* save=new QPushButton("Save presentation");auto* reset=new QPushButton("Restore source");auto* fullPreview=new QPushButton("Preview full");auto* emptyPreview=new QPushButton("Preview empty");for(auto* button:{save,reset,fullPreview,emptyPreview})actions->addWidget(button);
    auto* carrier=new QPushButton("Change full carrier in Operating parts…");layout->addWidget(carrier);connect(carrier,&QPushButton::clicked,dialog,[this,dialog]{dialog->close();showOperatingSetup();});
    auto* createMovement=new QPushButton("Edit magazine Move / Rotate movement…");layout->addWidget(createMovement);connect(createMovement,&QPushButton::clicked,dialog,[=,this]{if(!_reloadRecipe||_reloadRecipe->id!=recipe)return;for(int i=1;i<_handPoseAnchors.size();++i)if(sameMechanismPart(_handPoseAnchors[i].description,_reloadRecipe->tracks[_simulationRig->magazine].anchor)){dialog->close();setWorkspace(GripsWorkspace);selectWorkshopPart(i);return;}setStatus("The magazine carrier has no editable native anchor.",true);});
    auto* reference=new QPushButton("Configure hand reference and connection…");layout->addWidget(reference);connect(reference,&QPushButton::clicked,dialog,[this]{showMagazineReference();});
    connect(save,&QPushButton::clicked,dialog,[=,this]{try{auto value=initial;value["emptyTrack"]=empty->currentData().toInt();value["motion"]=motion->currentData().toString();value["movementId"]=movement->currentData().toString();value["entrySeconds"]=entry->value();value["sampledPayload"]=sampled->isChecked();value["loadedFrame"]=loaded->value();value["emptyFrame"]=depleted->value();QJsonArray pieces;for(const auto& pair:roles)pieces.append(QJsonObject{{"path",pair.first},{"role",pair.second->currentData().toString()}});value["pieces"]=pieces;configureMagazineSetup({{"operation","save"},{"settings",value}});status->setText("Saved. Inspect both states before recapturing the full and empty world models.");}catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{if(!_reloadRecipe||_reloadRecipe->id!=recipe)throw std::runtime_error("Source changed; reopen magazine settings");configureMagazineSetup({{"operation","reset"}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});
    const auto preview=[=,this](bool isEmpty){if(!_reloadRecipe||_reloadRecipe->id!=recipe||!_magazineRail){status->setText("Source changed; reopen magazine settings");return;}setWorkspace(OperateWorkspace);_simulationTimer->stop();_simulation.reset();_simulation.magazine.phase=MagazinePhase::OutsideEmpty;_simulation.magazine.distance=_magazineRail->travel;_simulation.magazine.offset=magazineInputOffset(*_magazineRail,_magazineRail->travel);_simulation.magazineRounds=isEmpty?0:_simulationAmmo.capacity;updateMagazinePose();status->setText("Previewing the saved presentation. Save first to preview edited settings.");};
    connect(fullPreview,&QPushButton::clicked,dialog,[=]{preview(false);});connect(emptyPreview,&QPushButton::clicked,dialog,[=]{preview(true);});dialog->show();
}
}
