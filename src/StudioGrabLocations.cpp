#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "design/DesignWidgets.h"
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
void ReloadMakerController::createGrabLocationsUi(){
    // The Grab points inspector: the points, the selected point, save.
    using namespace design;
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(16,10,16,16);layout->setSpacing(8);
    layout->addWidget(hintLabel("Pick the places where a hand may grab the operating part in game. The nearest allowed point wins. Points follow the animated part; hand poses stay independent.",page));
    layout->addSpacing(8);layout->addWidget(sectionLabel("Points",page));
    _grabLocationList=new QListWidget;_grabLocationList->setMinimumHeight(120);layout->addWidget(_grabLocationList);
    auto* row=new QHBoxLayout;row->setSpacing(6);auto* add=new QPushButton("Pick new point");auto* replace=new QPushButton("Replace selected");row->addWidget(add);row->addWidget(replace);layout->addLayout(row);
    add->setToolTip("Click the operating part on the model to add a point");replace->setToolTip("Click the model to move the selected point");
    layout->addSpacing(8);layout->addWidget(sectionLabel("Selected point",page));
    auto* form=new QFormLayout;form->setSpacing(8);_grabLocationName=new QLineEdit;_grabLocationName->setMaxLength(128);form->addRow("Name",_grabLocationName);
    _grabLocationRadius=new QDoubleSpinBox;_grabLocationRadius->setRange(.1,64);_grabLocationRadius->setDecimals(2);_grabLocationRadius->setSuffix(" game units");_grabLocationRadius->setKeyboardTracking(false);form->addRow("Grab radius",_grabLocationRadius);
    _grabLocationHands=new QComboBox;_grabLocationHands->addItem("Either hand",3);_grabLocationHands->addItem("Left hand",1);_grabLocationHands->addItem("Right hand",2);form->addRow("Allowed hand",_grabLocationHands);layout->addLayout(form);
    auto* fromPalm=new QPushButton("Use picked palm contact");fromPalm->setToolTip("Copy the existing Part grip's explicitly picked contact, keeping its wrist and finger pose unchanged.");
    auto* remove=new QPushButton("Remove");remove->setToolTip("Remove the selected point");
    auto* pointActions=new QHBoxLayout;pointActions->setSpacing(6);pointActions->addWidget(fromPalm,1);pointActions->addWidget(remove);layout->addLayout(pointActions);
    auto* exact=new QWidget(page);auto* exactForm=new QFormLayout(exact);exactForm->setContentsMargins(0,0,0,0);exactForm->setSpacing(8);
    for(int i=0;i<3;++i){auto* spin=new QDoubleSpinBox;spin->setRange(-10000,10000);spin->setDecimals(4);spin->setSingleStep(.1);spin->setKeyboardTracking(false);_grabLocationPosition[i]=spin;exactForm->addRow(QString("Local %1").arg(QChar("XYZ"[i])),spin);}
    layout->addWidget(disclosure("Exact position",exact,page));layout->addWidget(exact);
    layout->addSpacing(8);
    auto* save=new QPushButton("Save grab points");save->setObjectName("PrimaryButton");layout->addWidget(save);
    _grabLocationStatus=hintLabel({},page);layout->addWidget(_grabLocationStatus);
    // The fingers used when a hand grabs the operating part; the wrist stays live.
    layout->addSpacing(8);layout->addWidget(sectionLabel("Operating fingers",page));
    _operatingGripStatus=hintLabel({},page);layout->addWidget(_operatingGripStatus);
    _operatingGripStatus->setToolTip("Saved finger shape only. In game your wrist follows your approach. Mouse preview applies the fingers to the native wrist; it does not run the VR contact solver.");
    _operatingHandPicker=new QComboBox;_operatingHandPicker->addItem("Left hand");_operatingHandPicker->addItem("Right hand");layout->addWidget(_operatingHandPicker);
    connect(_operatingHandPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)setInteractionHand(PoseHand(i));updateInteractionGripUi();});
    _operatingFingerPicker=new QComboBox;layout->addWidget(_operatingFingerPicker);
    _operatingFingerPicker->setToolTip("Replace this hand's operating finger pose. If the other hand is unassigned it uses a skeleton-aware mirror. Saved wrist placement is never applied.");
    connect(_operatingFingerPicker,qOverload<int>(&QComboBox::activated),this,[this](int index){if(index>0)(void)useOperatingFingers(_operatingFingerPicker->itemData(index).toString());});
    layout->addStretch();
    auto* back=new QPushButton("Back to Operate");layout->addWidget(back);connect(back,&QPushButton::clicked,this,[this]{_pickingGrabLocation=false;_gripViewport->setPicking(false);setWorkspace(OperateWorkspace);});
    _handPosePages->addTab(page,"Grab points");
    connect(add,&QPushButton::clicked,this,[this]{beginGrabLocationPick();});connect(replace,&QPushButton::clicked,this,[this]{beginGrabLocationPick(true);});
    connect(save,&QPushButton::clicked,this,[this]{saveGrabLocations();});
    const auto action=[this](const QJsonObject& p){try{configureGrabLocation(p);}catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}};
    connect(remove,&QPushButton::clicked,this,[action]{action({{"operation","remove"}});});
    connect(fromPalm,&QPushButton::clicked,this,[action]{action({{"operation","use_palm"}});});
    connect(_grabLocationList,&QListWidget::currentRowChanged,this,[this](int row){selectGrabLocation(row);});
    const auto edit=[this,action]{if(_editingGrabLocationUi)return;action({{"operation","edit"},{"name",_grabLocationName->text()},{"pointInSource",QJsonArray{_grabLocationPosition[0]->value(),_grabLocationPosition[1]->value(),_grabLocationPosition[2]->value()}},{"radiusGame",_grabLocationRadius->value()},{"handMask",_grabLocationHands->currentData().toInt()}});};
    connect(_grabLocationName,&QLineEdit::editingFinished,this,edit);connect(_grabLocationHands,qOverload<int>(&QComboBox::activated),this,edit);
    for(auto* spin:{_grabLocationPosition[0],_grabLocationPosition[1],_grabLocationPosition[2],_grabLocationRadius})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,edit);
    refreshGrabLocationsUi();
}
void ReloadMakerController::refreshGrabLocationsUi(){
    if(!_grabLocationList||_editingGrabLocationUi)return;
    _editingGrabLocationUi=true;
    normalizeGrabLocationSelection();
    const auto g=currentGrabLocations();const int index=std::max(0,_selectedGrabLocation);
    {const QSignalBlocker b(_grabLocationList);_grabLocationList->clear();for(const auto& p:g.points)_grabLocationList->addItem(p.name);if(!g.points.isEmpty())_grabLocationList->setCurrentRow(index);}
    const bool selected=index<g.points.size();_grabLocationName->setEnabled(selected);_grabLocationRadius->setEnabled(selected);_grabLocationHands->setEnabled(selected);
    for(auto* spin:_grabLocationPosition)spin->setEnabled(selected);
    if(selected){const auto& p=g.points[index];_grabLocationName->setText(p.name);_grabLocationPosition[0]->setValue(p.point.x);_grabLocationPosition[1]->setValue(p.point.y);_grabLocationPosition[2]->setValue(p.point.z);_grabLocationRadius->setValue(p.radius);_grabLocationHands->setCurrentIndex(_grabLocationHands->findData(p.hands));}
    else _grabLocationName->clear();
    _grabLocationStatus->setText(g.weapon.isEmpty()?"Prepare the weapon in Operate first.":g.points.isEmpty()?"No points yet. Pick a point on the operating part, then save.":QString("%1 points on %2, shared by compatible variants of this part model.").arg(g.points.size()).arg(g.sourceName));
    _editingGrabLocationUi=false;
}
void ReloadMakerController::showSelectedGrabLocation(){
    if(!_grabLocationList)return;
    const QSignalBlocker block(_grabLocationList);_grabLocationList->setCurrentRow(_selectedGrabLocation);
}
void ReloadMakerController::showGrabLocationStatus(const QString& text){
    _grabLocationStatus->setText(text);
}
}
