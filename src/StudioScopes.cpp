#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "GripViewport.h"
#include "glview.h"
#include <QDockWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QQuaternion>
#include <QFileDialog>
#include <QTimer>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::createScopesUi() {
    _scopesDock=new QDockWidget("SCOPES",_window);_scopesDock->setObjectName("ScopesDock");
    _scopesDock->setFeatures(QDockWidget::NoDockWidgetFeatures);_scopesDock->setMinimumWidth(340);_scopesDock->setMaximumWidth(430);
    auto* panel=new QWidget;auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(14,14,14,14);
    auto* hint=new QLabel("Select the installed optic. Pick the rear lens, fit the green ring to its clear opening, and point the camera arrow down the tube.");hint->setWordWrap(true);layout->addWidget(hint);
    _scopePart=new QComboBox;layout->addWidget(_scopePart);
    auto* pick=new QPushButton("PICK REAR LENS");pick->setObjectName("PrimaryButton");layout->addWidget(pick);
    auto* form=new QFormLayout;_scopeName=new QLineEdit;_scopeName->setMaxLength(128);form->addRow("Profile name",_scopeName);
    const auto spin=[&](const QString& label,double low,double high,int decimals){auto* value=new QDoubleSpinBox;value->setRange(low,high);value->setDecimals(decimals);value->setSingleStep(.1);value->setKeyboardTracking(false);form->addRow(label,value);return value;};
    _scopeRadius=spin("Lens radius",.02,32,3);_scopeAspect=spin("Height / width",.1,1,3);
    _scopeRelief=spin("Eye relief",1,99,2);_scopeCamera=spin("Camera forward",0,256,3);
    _scopeCamera->setToolTip("Distance from the rear lens to the rendering camera along the green forward axis, in this shape's model units.");
    for(int i=0;i<3;++i)_scopePosition[i]=spin(QString("Local %1").arg(QChar("XYZ"[i])),-10000,10000,4);
    for(int i=0;i<3;++i)_scopeRotation[i]=spin(QString("Rotation %1").arg(QChar("XYZ"[i])),-360,360,3);
    layout->addLayout(form);
    auto* save=new QPushButton("SAVE SCOPE");auto* exportButton=new QPushButton("EXPORT FOR TRUE SCOPES");
    auto* importButton=new QPushButton("IMPORT SCOPE PROFILE");auto* remove=new QPushButton("REMOVE CURRENT PROFILE");
    for(auto* b:{save,exportButton,importButton,remove})layout->addWidget(b);
    _scopeStatus=new QLabel;_scopeStatus->setWordWrap(true);_scopeStatus->setObjectName("Hint");layout->addWidget(_scopeStatus);layout->addStretch();
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(panel);_scopesDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_scopesDock);_scopesDock->hide();
    const auto command=[this](const QJsonObject& p){try{(void)configureScope(p);}catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}};
    _scopeViewport=new GripViewport(_window->getGLView(),this);
    _scopeViewport->setLensAuthoring(1,1,10);
    _scopeViewport->beginPick=[this]{if(!scopeShapeBlocks().isEmpty())_scopeViewport->setPicking(true);else setStatus("Choose an installed optic with visible rigid geometry.",true);};
    _scopeViewport->surfacePicked=[this](QPointF point){pickScopeLens(point);};
    _scopeViewport->edited=[this](const Transformf& world){editScopeWorld(world);};
    _scopeViewport->save=[command]{command({{"operation","save"}});};
    _scopeViewport->end=[this]{setWorkspace(OperateWorkspace);};
    connect(pick,&QPushButton::clicked,this,[this]{_scopeViewport->beginPick();});
    connect(_scopePart,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){selectScopeTarget(index);});
    connect(save,&QPushButton::clicked,this,[command]{command({{"operation","save"}});});
    connect(exportButton,&QPushButton::clicked,this,[command]{command({{"operation","export"}});});
    connect(remove,&QPushButton::clicked,this,[command]{command({{"operation","remove"}});});
    connect(importButton,&QPushButton::clicked,this,[this,command]{const auto path=QFileDialog::getOpenFileName(_window,"Import True Scopes profile",studioDataPath("exports"),"Scope profiles (*.json)");if(!path.isEmpty())command({{"operation","import"},{"path",path}});});
    const auto edit=[this,command](QObject* changed){if(_editingScopeUi||!_scopeProfiles.contains(currentScopeKey()))return;
        auto p=_scopeProfiles.value(currentScopeKey());
        if(changed==_scopeName)p.name=_scopeName->text();
        if(changed==_scopeRadius)p.radius=float(_scopeRadius->value());
        if(changed==_scopeAspect)p.aspect=float(_scopeAspect->value());
        if(changed==_scopeRelief)p.eyeRelief=float(_scopeRelief->value());
        if(changed==_scopeCamera)p.cameraForward=float(_scopeCamera->value());
        if(changed==_scopePosition[0])p.lens.translation.x=float(_scopePosition[0]->value());
        if(changed==_scopePosition[1])p.lens.translation.y=float(_scopePosition[1]->value());
        if(changed==_scopePosition[2])p.lens.translation.z=float(_scopePosition[2]->value());
        // Display rounding must never rewrite an untouched imported transform.
        if(changed==_scopeRotation[0]||changed==_scopeRotation[1]||changed==_scopeRotation[2]){const auto q=QQuaternion::fromEulerAngles(float(_scopeRotation[0]->value()),float(_scopeRotation[1]->value()),float(_scopeRotation[2]->value()));p.lens.rotation={q.x(),q.y(),q.z(),q.scalar()};}
        command({{"operation","edit"},{"profile",scopeProfileJson(p)}});
    };
    connect(_scopeName,&QLineEdit::editingFinished,this,[this,edit]{edit(_scopeName);});
    for(auto* v:{_scopeRadius,_scopeAspect,_scopeRelief,_scopeCamera,_scopePosition[0],_scopePosition[1],_scopePosition[2],_scopeRotation[0],_scopeRotation[1],_scopeRotation[2]})connect(v,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[v,edit]{edit(v);});
}
void ReloadMakerController::refreshScopeControls() {
    if(!_scopePart)return;_editingScopeUi=true;const auto key=currentScopeKey();const bool have=_scopeProfiles.contains(key);const auto p=_scopeProfiles.value(key);
    _scopeName->setEnabled(have);_scopeName->setText(have?p.name:QString());
    _scopeRadius->setValue(p.radius);_scopeAspect->setValue(p.aspect);_scopeRelief->setValue(p.eyeRelief);_scopeCamera->setValue(p.cameraForward);
    const float pos[]{p.lens.translation.x,p.lens.translation.y,p.lens.translation.z};const auto rot=QQuaternion(p.lens.rotation.w,p.lens.rotation.x,p.lens.rotation.y,p.lens.rotation.z).toEulerAngles();
    for(int i=0;i<3;++i){_scopePosition[i]->setValue(pos[i]);_scopeRotation[i]->setValue(rot[i]);}
    for(auto* v:{_scopeRadius,_scopeAspect,_scopeRelief,_scopeCamera,_scopePosition[0],_scopePosition[1],_scopePosition[2],_scopeRotation[0],_scopeRotation[1],_scopeRotation[2]})v->setEnabled(have);
    Transformf frame;_scopeStatus->setText(key.isEmpty()?"Choose an installed optic.":!have?"No profile for this optic. Pick its rear lens to begin.":scopeFrame(frame)?"Shape: "+p.shape+"\nLengths use this shape's model units. Save keeps the profile in your project; export creates the MO2 folder.":"The saved shape is missing or ambiguous in this configuration. Its profile is retained; placement and export are unavailable.");
    _editingScopeUi=false;
}
void ReloadMakerController::updateScopeViewport() {
    if(!_scopeViewport)return;const bool active=_workspace==ScopesWorkspace&&!_loadThread&&!_rebuildTimer->isActive();
    Transformf frame;const bool resolved=active&&scopeFrame(frame);_scopeViewport->configure(active,active&&!scopeShapeBlocks().isEmpty(),resolved);
    if(!resolved){_scopeViewport->clearGrip();return;}
    const auto p=_scopeProfiles.value(currentScopeKey());_scopeViewport->setLensAuthoring(p.radius,p.aspect,p.cameraForward);
    _scopeViewport->setGrip(frame,composed(frame,p.lens),{},std::nullopt);
}
void ReloadMakerController::showScopeTargets() {
    const QSignalBlocker block(_scopePart);_scopePart->clear();
    for(const auto& target:_scopeTargets)_scopePart->addItem(target.label,target.anchor);
    _scopePart->setCurrentIndex(_selectedScopeTarget);
}
void ReloadMakerController::showSelectedScopeTarget() {
    const QSignalBlocker block(_scopePart);_scopePart->setCurrentIndex(_selectedScopeTarget);
}
}
