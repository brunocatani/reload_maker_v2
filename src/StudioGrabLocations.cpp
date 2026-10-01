#include "ReloadMakerController.h"
#include "GripViewport.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QUuid>
#include <stdexcept>
#include <algorithm>

namespace reloadmaker {
bool ReloadMakerController::reloadCaptureAllowed() const noexcept {
    return _reloadWorkspaceActive||simulationWorkspace(_workspace)||
        (_workspace==GripsWorkspace&&_handPosePages&&_handPosePages->currentIndex()==3);
}
OperatingGrabLocations ReloadMakerController::currentGrabLocations() const {
    const auto a=operatingAnchor();if(a<0||!currentWeapon())return {};
    const auto& source=_handPoseAnchors[a].description;
    OperatingGrabLocations g{poseRecordKey(currentWeapon()->formId),operatingGroup(),source.ownerModel,source.anchorPath,source.anchorName,{}};
    return _grabLocations.value(grabLocationsKey(g),g);
}
void ReloadMakerController::createGrabLocationsUi(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(14,14,14,14);
    auto* hint=new QLabel("Pick the exact places where the operating part can be grabbed in-game. The nearest allowed location wins. Points follow the animated part; hand poses stay independent.");hint->setWordWrap(true);layout->addWidget(hint);
    _grabLocationList=new QListWidget;_grabLocationList->setMinimumHeight(140);layout->addWidget(_grabLocationList);
    auto* row=new QHBoxLayout;auto* add=new QPushButton("PICK NEW POINT");auto* replace=new QPushButton("REPLACE SELECTED POINT");row->addWidget(add);row->addWidget(replace);layout->addLayout(row);
    auto* form=new QFormLayout;_grabLocationName=new QLineEdit;_grabLocationName->setMaxLength(128);form->addRow("Name",_grabLocationName);
    for(int i=0;i<3;++i){auto* spin=new QDoubleSpinBox;spin->setRange(-10000,10000);spin->setDecimals(4);spin->setSingleStep(.1);spin->setKeyboardTracking(false);_grabLocationPosition[i]=spin;form->addRow(QString("Local %1").arg(QChar("XYZ"[i])),spin);}
    _grabLocationRadius=new QDoubleSpinBox;_grabLocationRadius->setRange(.1,64);_grabLocationRadius->setDecimals(2);_grabLocationRadius->setSuffix(" game units");_grabLocationRadius->setKeyboardTracking(false);form->addRow("Grab radius",_grabLocationRadius);
    _grabLocationHands=new QComboBox;_grabLocationHands->addItem("Either hand",3);_grabLocationHands->addItem("Left hand",1);_grabLocationHands->addItem("Right hand",2);form->addRow("Allowed hand",_grabLocationHands);layout->addLayout(form);
    auto* fromPalm=new QPushButton("USE PICKED PALM CONTACT");layout->addWidget(fromPalm);fromPalm->setToolTip("Copy the existing Part grip's explicitly picked contact, keeping its wrist and finger pose unchanged.");
    auto* remove=new QPushButton("REMOVE SELECTED POINT");layout->addWidget(remove);
    auto* save=new QPushButton("SAVE GRAB LOCATIONS");save->setObjectName("PrimaryButton");layout->addWidget(save);
    _grabLocationStatus=new QLabel;_grabLocationStatus->setWordWrap(true);layout->addWidget(_grabLocationStatus);layout->addStretch();
    auto* back=new QPushButton("BACK TO OPERATE");layout->addWidget(back);connect(back,&QPushButton::clicked,this,[this]{_pickingGrabLocation=false;_gripViewport->setPicking(false);setWorkspace(OperateWorkspace);});
    _handPosePages->addTab(page,"Grab locations");
    connect(add,&QPushButton::clicked,this,[this]{beginGrabLocationPick();});connect(replace,&QPushButton::clicked,this,[this]{beginGrabLocationPick(true);});
    connect(save,&QPushButton::clicked,this,[this]{saveGrabLocations();});
    const auto action=[this](const QJsonObject& p){try{configureGrabLocation(p);}catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}};
    connect(remove,&QPushButton::clicked,this,[action]{action({{"operation","remove"}});});
    connect(fromPalm,&QPushButton::clicked,this,[action]{action({{"operation","use_palm"}});});
    connect(_grabLocationList,&QListWidget::currentRowChanged,this,[this]{refreshGrabLocationsUi();updateGripViewport();});
    const auto edit=[this,action]{if(_editingGrabLocationUi)return;action({{"operation","edit"},{"name",_grabLocationName->text()},{"pointInSource",QJsonArray{_grabLocationPosition[0]->value(),_grabLocationPosition[1]->value(),_grabLocationPosition[2]->value()}},{"radiusGame",_grabLocationRadius->value()},{"handMask",_grabLocationHands->currentData().toInt()}});};
    connect(_grabLocationName,&QLineEdit::editingFinished,this,edit);connect(_grabLocationHands,qOverload<int>(&QComboBox::activated),this,edit);
    for(auto* spin:{_grabLocationPosition[0],_grabLocationPosition[1],_grabLocationPosition[2],_grabLocationRadius})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,edit);
    refreshGrabLocationsUi();
}
bool ReloadMakerController::openGrabLocations(){
    if(!currentWeapon()||_loadThread){setStatus("Load a weapon first.",true);return false;}
    rememberMagazineGrip();setWorkspace(GripsWorkspace);_handPosePages->setCurrentIndex(3);prepareGrabLocationFrame();return true;
}
void ReloadMakerController::prepareGrabLocationFrame(){
    if(_loadThread||_reloadCapturing||_reloadCaptureQueued)return;
    if(!_reloadRecipe){startReloadCapture();return;}
    if(!_simulationRig){
        SimulationRig rig;QString error;
        if(!buildCurrentSimulationRig(rig,error)){setStatus(error,true);return;}
        _simulationRig=rig;
    }
    refreshGrabLocationsUi();updateGripViewport();
}
void ReloadMakerController::refreshGrabLocationsUi(){
    if(!_grabLocationList||_editingGrabLocationUi)return;
    _editingGrabLocationUi=true;
    const auto g=currentGrabLocations();const int index=std::clamp(_grabLocationList->currentRow(),0,std::max(0,int(g.points.size())-1));
    {const QSignalBlocker b(_grabLocationList);_grabLocationList->clear();for(const auto& p:g.points)_grabLocationList->addItem(p.name);if(!g.points.isEmpty())_grabLocationList->setCurrentRow(index);}
    const bool selected=index<g.points.size();_grabLocationName->setEnabled(selected);_grabLocationRadius->setEnabled(selected);_grabLocationHands->setEnabled(selected);
    for(auto* spin:_grabLocationPosition)spin->setEnabled(selected);
    if(selected){const auto& p=g.points[index];_grabLocationName->setText(p.name);_grabLocationPosition[0]->setValue(p.point.x);_grabLocationPosition[1]->setValue(p.point.y);_grabLocationPosition[2]->setValue(p.point.z);_grabLocationRadius->setValue(p.radius);_grabLocationHands->setCurrentIndex(_grabLocationHands->findData(p.hands));}
    else _grabLocationName->clear();
    _grabLocationStatus->setText(g.weapon.isEmpty()?"Prepare the weapon in Operate first.":g.points.isEmpty()?"No locations. Pick a point on the operating part, then save.":QString("%1 locations on %2. Shared by compatible variants of this part model.").arg(g.points.size()).arg(g.sourceName));
    _editingGrabLocationUi=false;
}
bool ReloadMakerController::beginGrabLocationPick(bool replace){
    if(!openGrabLocations())return false;
    if(operatingAnchor()<0){setStatus("Wait for the operating part to finish loading.",true);return false;}
    if(replace&&_grabLocationList->currentRow()<0){setStatus("Select a grab location to replace.",true);return false;}
    _replaceGrabLocation=replace;_pickingGrabLocation=true;_gripViewport->setPicking(true);
    _grabLocationStatus->setText("Click the exact surface location on "+currentGrabLocations().sourceName+". Esc cancels.");return true;
}
bool ReloadMakerController::pickGrabLocation(QPointF pixel){
    if(!_pickingGrabLocation||_loadThread)return false;
    int automatic=-1;Vec3f hit;QString surface,error;QVector<int> candidates;
    if(!pickWeaponPart(pixel,automatic,hit,surface,candidates,error)){setStatus(error,true);return false;}
    const int a=operatingAnchor();if(a<0||!candidates.contains(a)){setStatus("Pick a surface on the selected operating part.",true);return false;}
    Transformf world,inverse;if(!sceneNodeWorld(_handPoseAnchors[a].node,world,error)||!invertUniformTransform(world,inverse)){setStatus("Operating part frame is unavailable.",true);return false;}
    auto g=currentGrabLocations();const auto p=composed(inverse,Transformf{hit}).translation;const int selected=_grabLocationList->currentRow();
    if(_replaceGrabLocation&&selected>=0&&selected<g.points.size())g.points[selected].point=p;
    else {if(g.points.size()>=32){setStatus("A group supports up to 32 locations.",true);return false;}g.points.append({QUuid::createUuid().toString(QUuid::WithoutBraces),QString("%1 point %2").arg(g.sourceName).arg(g.points.size()+1),p,3,3});}
    _grabLocations[grabLocationsKey(g)]=g;_pickingGrabLocation=false;_lastError.clear();_gripViewport->setPicking(false);++_revision;refreshGrabLocationsUi();if(!_replaceGrabLocation)_grabLocationList->setCurrentRow(g.points.size()-1);updateGripViewport();return true;
}
void ReloadMakerController::editGrabLocationWorld(const Transformf& target){
    const auto a=operatingAnchor();const auto index=_grabLocationList->currentRow();auto g=currentGrabLocations();if(a<0||index<0||index>=g.points.size())return;
    Transformf world,inverse;QString error;if(!sceneNodeWorld(_handPoseAnchors[a].node,world,error)||!invertUniformTransform(world,inverse))return;
    g.points[index].point=composed(inverse,target).translation;_grabLocations[grabLocationsKey(g)]=g;++_revision;refreshGrabLocationsUi();updateGripViewport();
}
bool ReloadMakerController::saveGrabLocations(){
    const auto g=currentGrabLocations();OperatingGrabLocations checked;QString error;if(!readGrabLocations(grabLocationsJson(g),checked,error)){setStatus(error,true);return false;}
    _grabLocations[grabLocationsKey(g)]=g;if(!saveSession())return false;_grabLocationStatus->setText("Grab locations saved. Export them for the game's point-target registration.");return true;
}
QJsonObject ReloadMakerController::exportGrabLocations() const {
    const auto g=currentGrabLocations();if(!_grabLocations.contains(grabLocationsKey(g)))return {};
    auto json=grabLocationsJson(g);if(_simulationRig)json["inputTrack"]=_simulationRig->slide;return json;
}
QJsonObject ReloadMakerController::grabLocationsState() const {const auto g=currentGrabLocations();return {{"configured",_grabLocations.contains(grabLocationsKey(g))},{"profile",grabLocationsJson(g)},{"selected",_grabLocationList?_grabLocationList->currentRow():-1}};}
QJsonObject ReloadMakerController::configureGrabLocation(const QJsonObject& p){
    const auto fail=[](const QString& e){throw std::runtime_error(e.toStdString());};const auto operation=p["operation"].toString();
    if(operation=="open"){if(!openGrabLocations())fail(_lastError);return grabLocationsState();}
    if(operation=="pick"){if(!beginGrabLocationPick(p["replace"].toBool())||!pickGrabLocation({p["x"].toDouble(-1),p["y"].toDouble(-1)}))fail(_lastError);return grabLocationsState();}
    if(operation=="save"){if(!saveGrabLocations())fail(_lastError);return grabLocationsState();}
    auto g=currentGrabLocations();if(g.weapon.isEmpty())fail("Prepare the weapon in Operate first.");int i=_grabLocationList->currentRow();
    if(p.contains("id")){i=-1;for(int n=0;n<g.points.size();++n)if(g.points[n].id==p["id"].toString())i=n;}
    if(operation=="use_palm") {
        const auto a=operatingAnchor();if(a<0||!_partGripDraft||!_partGripDraft->surfaceContact||!sameGripAnchor(_partGripDraft->pose.placement,_handPoseAnchors[a].description))fail("Pick a palm contact on this operating part in Part grip first.");
        g.points.append({QUuid::createUuid().toString(QUuid::WithoutBraces),"Picked palm contact",_partGripDraft->surfaceContact->pointInAnchor,3,3});i=g.points.size()-1;
    }else {
        if(i<0||i>=g.points.size())fail("Select a grab location first.");
        if(operation=="select"){_grabLocationList->setCurrentRow(i);return grabLocationsState();}
        if(operation=="remove")g.points.removeAt(i);
        else if(operation=="edit") {
            auto j=grabLocationsJson(g);auto points=j["points"].toArray();auto point=points[i].toObject();for(const auto* key:{"name","pointInSource","radiusGame","handMask"})if(p.contains(key))point[key]=p[key];points[i]=point;j["points"]=points;QString error;
            if(!readGrabLocations(j,g,error))fail(error);
        }else fail("Unknown grab-location operation.");
    }
    OperatingGrabLocations checked;QString error;if(!readGrabLocations(grabLocationsJson(g),checked,error))fail(error);
    _grabLocations[grabLocationsKey(g)]=g;++_revision;refreshGrabLocationsUi();if(i<g.points.size())_grabLocationList->setCurrentRow(i);updateGripViewport();return grabLocationsState();
}
}
