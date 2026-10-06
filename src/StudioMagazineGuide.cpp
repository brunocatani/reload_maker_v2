#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "StudioWindow.h"
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showMagazineGuide() {
    if(auto* existing=_window->findChild<QDialog*>("MagazineGuideDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=magazineGuideState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("MagazineGuideDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Magazine insertion guide");dialog->resize(580,420);
    auto* layout=new QVBoxLayout(dialog);auto* info=new QLabel("Set the straight path your hand follows. The magazine keeps its complete visible insertion/extraction motion, including rocking and rotation. Direction points outward from the seated position, relative to this magazine's connection.");info->setWordWrap(true);layout->addWidget(info);
    const auto initial=state["settings"].toObject();const auto recipe=_reloadRecipe->id;const auto selection=authoringRecordKey();auto* form=new QFormLayout;layout->addLayout(form);
    auto* preset=new QComboBox;for(const auto* text:{"Custom direction","Connection +X","Connection -X","Connection +Y","Connection -Y","Connection +Z","Connection -Z"})preset->addItem(text);form->addRow("Direction",preset);
    QVector<QDoubleSpinBox*> axes;const auto direction=initial["outwardInConnection"].toArray();
    for(int i=0;i<3;++i){auto* spin=new QDoubleSpinBox;spin->setRange(-1,1);spin->setDecimals(5);spin->setSingleStep(.05);spin->setValue(direction[i].toDouble());form->addRow(QString("Connection %1").arg(QChar('X'+i)),spin);axes.append(spin);}
    connect(preset,qOverload<int>(&QComboBox::activated),dialog,[axes](int i){if(i<1)return;for(int a=0;a<3;++a)axes[a]->setValue(a==(i-1)/2?(i%2?1:-1):0);});
    const auto field=[&](const char* key,const QString& title){auto* spin=new QDoubleSpinBox;spin->setRange(.001,999);spin->setDecimals(4);spin->setSingleStep(.1);spin->setSuffix(" game units");spin->setValue(initial[key].toDouble());form->addRow(title,spin);return spin;};
    auto* travel=field("travel","Hand travel");auto* capture=field("captureRadius","Pickup distance from line");auto* release=field("releaseMargin","Pull-out release margin");
    auto* status=new QLabel(state["error"].toString());status->setWordWrap(true);layout->addWidget(status);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* save=new QPushButton("Save and preview guide");auto* reset=new QPushButton("Restore source guide");auto* done=new QPushButton("Done");actions->addWidget(save);actions->addWidget(reset);actions->addWidget(done);
    save->setEnabled(state["canConfigure"].toBool());
    if(!state["ready"].toBool()&&!state["saved"].toBool())status->setText("No usable native extraction was found. These are manual starting values: choose an outward direction and hand travel. Geometry determines the initial visible clearance; a saved movement can replace that path.");
    connect(save,&QPushButton::clicked,dialog,[=,this]{try{
        QVector3D axis(float(axes[0]->value()),float(axes[1]->value()),float(axes[2]->value()));if(axis.lengthSquared()<1e-8f)throw std::runtime_error("Choose a nonzero outward direction");axis.normalize();
        MagazineGuide guide{recipe,axis,float(travel->value()),float(capture->value()),float(release->value())};
        configureMagazineGuide({{"operation","save"},{"settings",magazineGuideJson(guide)}});setWorkspace(OperateWorkspace);
        if(_simulationActive&&_magazineRail){_simulation.magazine.animateOut();_simulation.magazine.tick(*_magazineRail,1);updateMagazinePose();}
        status->setText(_magazineRail?"Saved for this assembly. The guide is visible; use GRAB to test entry anywhere along it.":_magazineError);
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{if(!_reloadRecipe||authoringRecordKey()!=selection)throw std::runtime_error("Source changed; reopen guide settings");configureMagazineGuide({{"operation","reset"}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});
    connect(done,&QPushButton::clicked,dialog,&QDialog::close);dialog->show();
}
}
