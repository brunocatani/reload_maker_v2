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
void ReloadMakerController::createGrabLocationsUi(){
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(14,14,14,14);
    auto* hint=new QLabel("Pick the exact places where the operating part can be grabbed in-game. The nearest allowed location wins. Points follow the animated part; hand poses stay independent.");hint->setWordWrap(true);layout->addWidget(hint);
    _grabLocationList=new QListWidget;_grabLocationList->setMinimumHeight(140);layout->addWidget(_grabLocationList);
    auto* row=new QHBoxLayout;auto* add=new QPushButton("Pick new point");auto* replace=new QPushButton("Replace selected point");row->addWidget(add);row->addWidget(replace);layout->addLayout(row);
    auto* form=new QFormLayout;_grabLocationName=new QLineEdit;_grabLocationName->setMaxLength(128);form->addRow("Name",_grabLocationName);
    for(int i=0;i<3;++i){auto* spin=new QDoubleSpinBox;spin->setRange(-10000,10000);spin->setDecimals(4);spin->setSingleStep(.1);spin->setKeyboardTracking(false);_grabLocationPosition[i]=spin;form->addRow(QString("Local %1").arg(QChar("XYZ"[i])),spin);}
    _grabLocationRadius=new QDoubleSpinBox;_grabLocationRadius->setRange(.1,64);_grabLocationRadius->setDecimals(2);_grabLocationRadius->setSuffix(" game units");_grabLocationRadius->setKeyboardTracking(false);form->addRow("Grab radius",_grabLocationRadius);
    _grabLocationHands=new QComboBox;_grabLocationHands->addItem("Either hand",3);_grabLocationHands->addItem("Left hand",1);_grabLocationHands->addItem("Right hand",2);form->addRow("Allowed hand",_grabLocationHands);layout->addLayout(form);
    auto* fromPalm=new QPushButton("Use picked palm contact");layout->addWidget(fromPalm);fromPalm->setToolTip("Copy the existing Part grip's explicitly picked contact, keeping its wrist and finger pose unchanged.");
    auto* remove=new QPushButton("Remove selected point");layout->addWidget(remove);
    auto* save=new QPushButton("Save grab locations");save->setObjectName("PrimaryButton");layout->addWidget(save);
    _grabLocationStatus=new QLabel;_grabLocationStatus->setWordWrap(true);layout->addWidget(_grabLocationStatus);layout->addStretch();
    auto* back=new QPushButton("Back to Operate");layout->addWidget(back);connect(back,&QPushButton::clicked,this,[this]{_pickingGrabLocation=false;_gripViewport->setPicking(false);setWorkspace(OperateWorkspace);});
    _handPosePages->addTab(page,"Grab locations");
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
    _grabLocationStatus->setText(g.weapon.isEmpty()?"Prepare the weapon in Operate first.":g.points.isEmpty()?"No locations. Pick a point on the operating part, then save.":QString("%1 locations on %2. Shared by compatible variants of this part model.").arg(g.points.size()).arg(g.sourceName));
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
