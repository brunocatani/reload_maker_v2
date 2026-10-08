#include "ReloadMakerController.h"
#include "MagazineReference.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QQuaternion>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showMagazineReference() {
    if(auto* existing=_window->findChild<QDialog*>("MagazineReferenceDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=magazineReferenceState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    const auto initial=state["settings"].toObject();const auto recipe=_reloadRecipe->id;const auto selection=authoringRecordKey();
    auto* dialog=new QDialog(_window);dialog->setObjectName("MagazineReferenceDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Magazine reference and connection");dialog->resize(630,570);
    auto* layout=new QVBoxLayout(dialog);auto* info=new QLabel("Choose the magazine frame around which the hands are posed. The connection controls the guide's origin and orientation. Changing a hand reference keeps existing fits saved, but they need review before reuse.");info->setWordWrap(true);layout->addWidget(info);auto* form=new QFormLayout;layout->addLayout(form);
    auto* reference=new QComboBox;for(const auto& value:state["anchors"].toArray()){const auto a=value.toObject();reference->addItem(a["name"].toString()+" · "+a["kind"].toString(),a["key"].toString());reference->setItemData(reference->count()-1,a["path"].toString()+"\n"+a["model"].toString(),Qt::ToolTipRole);}reference->setCurrentIndex(reference->findData(initial["anchorKey"].toString()));form->addRow("Hand reference",reference);
    connect(reference,qOverload<int>(&QComboBox::activated),dialog,[=,this](int){if(!_reloadRecipe||authoringRecordKey()!=selection)return;for(int i=1;i<_handPoseAnchors.size();++i)if(magazineReferenceKey(_handPoseAnchors[i].description)==reference->currentData().toString())_window->getGLView()->setCurrentIndex(_handPoseAnchors[i].node);});
    auto* mode=new QComboBox;mode->addItem("Native mount connection","source");mode->addItem("Full magazine carrier pivot","carrier");mode->addItem("Custom frame in magazine carrier","custom");mode->setCurrentIndex(mode->findData(initial["connectionMode"].toString()));form->addRow("Connection",mode);
    Transformf transform;QString error;(void)decodePoseTransform(initial["connectionInCarrier"],transform,error);const auto& q=transform.rotation;const auto euler=QQuaternion(q.w,q.x,q.y,q.z).toEulerAngles();
    QVector<QDoubleSpinBox*> fields;QVector<double> displayed;const QVector<double> values{transform.translation.x,transform.translation.y,transform.translation.z,euler.x(),euler.y(),euler.z()};
    for(int i=0;i<6;++i){auto* spin=new QDoubleSpinBox;spin->setDecimals(4);spin->setRange(i<3?-1000:-360,i<3?1000:360);spin->setSingleStep(i<3?.1:1);spin->setValue(values[i]);fields.append(spin);displayed.append(spin->value());form->addRow(QString(i<3?"Position %1":"Rotation %1").arg(QChar('X'+i%3)),spin);}
    const auto enable=[=]{for(auto* f:fields)f->setEnabled(mode->currentData()=="custom");};connect(mode,qOverload<int>(&QComboBox::currentIndexChanged),dialog,enable);enable();
    auto* status=new QLabel(state["error"].toString());status->setWordWrap(true);layout->addWidget(status);auto* buttons=new QHBoxLayout;layout->addLayout(buttons);auto* save=new QPushButton("Save and preview");auto* reset=new QPushButton("Restore source reference");buttons->addWidget(save);buttons->addWidget(reset);
    auto* guide=new QPushButton("Edit the insertion guide");layout->addWidget(guide);connect(guide,&QPushButton::clicked,dialog,[this,dialog]{dialog->close();showMagazineStep(MagazineStep::Guide);});
    connect(save,&QPushButton::clicked,dialog,[=,this]{try{auto value=initial;value["anchorKey"]=reference->currentData().toString();value["connectionMode"]=mode->currentData().toString();
        const bool changed=std::any_of(fields.cbegin(),fields.cend(),[&,i=0](auto* field)mutable{return field->value()!=displayed[i++];});
        if(changed){const auto r=QQuaternion::fromEulerAngles(fields[3]->value(),fields[4]->value(),fields[5]->value());Transformf frame{{float(fields[0]->value()),float(fields[1]->value()),float(fields[2]->value())},{r.x(),r.y(),r.z(),r.scalar()},transform.scale};QJsonObject json;QString why;if(!encodePoseTransform(frame,json,why))throw std::runtime_error(why.toStdString());value["connectionInCarrier"]=json;}
        const auto result=configureMagazineReference({{"operation","save"},{"settings",value}});status->setText(result["error"].toString().isEmpty()?"Saved. Test the magazine guide and hand fits in Operate.":result["error"].toString());
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{if(!_reloadRecipe||authoringRecordKey()!=selection)throw std::runtime_error("Source changed; reopen reference settings");configureMagazineReference({{"operation","reset"}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});dialog->show();
}
}
