#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "GripViewport.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileDialog>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QTimer>
#include <stdexcept>

namespace reloadmaker {
namespace { void requireScope(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());} }
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
    connect(_scopePart,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{_scopeViewport->setPicking(false);refreshScopeControls();updateScopeViewport();});
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
QString ReloadMakerController::currentScopeKey() const {
    const int index=_scopePart?_scopePart->currentData().toInt():-1;
    return index>0&&index<_handPoseAnchors.size()?scopeModelKey(_handPoseAnchors[index].description.ownerModel):QString();
}
void ReloadMakerController::refreshScopeTargets() {
    if(!_scopePart)return;const auto previous=_scopePart->currentText();const QSignalBlocker block(_scopePart);_scopePart->clear();int preferred=-1;
    for(int i=1;i<_handPoseAnchors.size();++i){const auto& anchor=_handPoseAnchors[i];
        if(anchor.description.anchorKind!="installed-part"||anchor.node==_weaponRoot||anchor.description.ownerModel.isEmpty())continue;
        const auto name=_handPoseAnchor->itemText(i);_scopePart->addItem(name,i);
        if(name.contains("sight",Qt::CaseInsensitive)||name.contains("scope",Qt::CaseInsensitive))preferred=_scopePart->count()-1;
    }
    const int old=_scopePart->findText(previous);_scopePart->setCurrentIndex(old>=0?old:preferred);
    refreshScopeControls();updateScopeViewport();
}
QVector<int> ReloadMakerController::scopeShapeBlocks() const {
    QVector<int> blocks;const int index=_scopePart?_scopePart->currentData().toInt():-1;
    const auto* model=activeModel();auto* scene=_window->getGLView()->getScene();
    if(!model||!scene||index<=0||index>=_handPoseAnchors.size()||_loadThread||_rebuildTimer->isActive())return blocks;
    const auto root=_handPoseAnchors[index].node;
    for(auto* shape:scene->shapes){if(!shape||shape->isHidden()||!shape->reloadMakerSkinBones().isEmpty())continue;
        auto node=model->getBlockIndex(shape->id());for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth){if(node==root){blocks.append(shape->id());break;}node=model->getBlockIndex(model->getParent(node));}}
    return blocks;
}
bool ReloadMakerController::scopeFrame(Transformf& world) const {
    const auto found=_scopeProfiles.constFind(currentScopeKey());if(found==_scopeProfiles.cend())return false;
    const auto* model=activeModel();auto* scene=_window->getGLView()->getScene();if(!model||!scene)return false;
    const auto allowed=scopeShapeBlocks();QModelIndex selected;int count=0;
    for(auto* shape:scene->shapes){if(!shape)continue;const auto node=model->getBlockIndex(shape->id());
        if(model->get<QString>(node,"Name")==found->shape){++count;if(allowed.contains(shape->id()))selected=node;}}
    QString error;return count==1&&selected.isValid()&&sceneNodeWorld(selected,world,error);
}
bool ReloadMakerController::pickScopeLens(QPointF point) {
    const auto fail=[this](const QString& message){setStatus(message,true);return false;};
    auto* view=_window->getGLView();auto* model=activeModel();const auto blocks=scopeShapeBlocks();
    int block=-1;Vector3 hit;
    if(!view->reloadMakerSurfaceAt(point,blocks,block,hit))return fail("Pick the rear lens on the selected optic's visible rigid mesh.");
    const auto node=model->getBlockIndex(block);const auto shape=model->get<QString>(node,"Name");int duplicates=0;
    for(auto* candidate:view->getScene()->shapes)if(candidate&&model->get<QString>(model->getBlockIndex(candidate->id()),"Name")==shape)++duplicates;
    if(duplicates!=1)return fail("This shape name is ambiguous in the assembled weapon. Choose a uniquely named lens or housing shape.");
    Transformf world,inverse,part;QString error;const int index=_scopePart->currentData().toInt();
    if(!sceneNodeWorld(node,world,error)||!invertUniformTransform(world,inverse)||!sceneNodeWorld(_handPoseAnchors[index].node,part,error))return fail("The optic's frame is invalid.");
    const auto& source=_handPoseAnchors[index].description;auto p=_scopeProfiles.value(currentScopeKey());
    const bool existing=!p.model.isEmpty();Transformf oldWorld;const bool haveOld=existing&&scopeFrame(oldWorld);
    if(haveOld){const float ratio=oldWorld.scale.x/world.scale.x;p.radius*=ratio;p.eyeRelief*=ratio;p.cameraForward*=ratio;}
    Transformf target;target.translation={hit[0],hit[1],hit[2]};target.rotation=haveOld?composed(oldWorld,p.lens).rotation:part.rotation;
    p.lens=composed(inverse,target);p.lens.scale={1,1,1};p.model=currentScopeKey();p.shape=shape;
    if(!existing)p.name=_scopePart->currentText();p.weapon=poseRecordKey(currentWeapon()->formId);p.attachment=source.ownerRecord;
    ScopeProfile checked;if(!readScopeProfile(scopeProfileJson(p),checked,error))return fail(error);
    if(!_scopeProfiles.contains(p.model)&&_scopeProfiles.size()>=128)return fail("A project supports at most 128 optic profiles.");
    _scopeProfiles[p.model]=p;_scopeViewport->setPicking(false);++_revision;refreshScopeControls();updateScopeViewport();return true;
}
void ReloadMakerController::editScopeWorld(const Transformf& target) {
    Transformf frame,inverse;if(!scopeFrame(frame)||!invertUniformTransform(frame,inverse))return;
    auto p=_scopeProfiles.value(currentScopeKey());p.lens=composed(inverse,target);p.lens.scale={1,1,1};
    ScopeProfile checked;QString error;if(!readScopeProfile(scopeProfileJson(p),checked,error)){setStatus(error,true);return;}
    _scopeProfiles[p.model]=p;++_revision;refreshScopeControls();updateScopeViewport();
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
QJsonObject ReloadMakerController::scopeState() const {
    QJsonArray targets;for(int i=0;i<_scopePart->count();++i){const int anchor=_scopePart->itemData(i).toInt();if(anchor<=0||anchor>=_handPoseAnchors.size())continue;
        targets.append(QJsonObject{{"index",i},{"name",_scopePart->itemText(i)},{"model",scopeModelKey(_handPoseAnchors[anchor].description.ownerModel)}});}
    QJsonArray shapes;const auto* model=activeModel();for(int b:scopeShapeBlocks())shapes.append(model->get<QString>(model->getBlockIndex(b),"Name"));
    Transformf frame;const auto key=currentScopeKey();const bool configured=_scopeProfiles.contains(key);
    return {{"targets",targets},{"selected",_scopePart->currentIndex()},{"shapes",shapes},{"configured",configured},{"resolved",scopeFrame(frame)},{"profile",configured?scopeProfileJson(_scopeProfiles.value(key)):QJsonObject()}};
}
QJsonObject ReloadMakerController::exportScopeProfile() {
    Transformf frame;requireScope(scopeFrame(frame),"The optic's unique rigid shape must be present before exporting.");
    const auto p=_scopeProfiles.value(currentScopeKey());ScopeProfile checked;QString error;requireScope(readScopeProfile(scopeProfileJson(p),checked,error),error);
    auto name=QFileInfo(p.model).completeBaseName();name.replace(QRegularExpression("[^a-zA-Z0-9_-]"),"_");
    const auto hash=QString::fromLatin1(QCryptographicHash::hash(p.model.toUtf8(),QCryptographicHash::Sha256).toHex().left(12));
    const auto root=studioDataPath("exports/TrueScopesProfiles");const auto path=root+"/F4SE/Plugins/TrueScopesVR/Scopes/"+name+"-"+hash+".json";
    requireScope(QDir().mkpath(QFileInfo(path).absolutePath()),"Cannot create scope export folder.");
    QSaveFile file(path);const auto bytes=QJsonDocument(scopeProfileJson(p)).toJson();requireScope(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot save scope export: "+file.errorString());
    setStatus("Exported · "+path);return {{"path",path},{"modFolder",root},{"kind","scope"},{"deployed",false}};
}
QJsonObject ReloadMakerController::configureScope(const QJsonObject& params) {
    requireScope(!_loadThread&&!_rebuildTimer->isActive(),"Wait for weapon assembly.");_lastError.clear();const auto op=params["operation"].toString();
    if(op=="open"){setWorkspace(ScopesWorkspace);return scopeState();}
    if(op=="export")return exportScopeProfile();
    if(op=="import"){
        QFile file(params["path"].toString());requireScope(file.open(QIODevice::ReadOnly)&&file.size()<=32768,"Cannot read scope profile (maximum 32 KiB).");
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);ScopeProfile p;QString error;
        requireScope(parse.error==QJsonParseError::NoError&&doc.isObject()&&readScopeProfile(doc.object(),p,error),"Invalid scope profile: "+error);
        requireScope(!_scopeProfiles.contains(p.model)||scopeProfileJson(_scopeProfiles.value(p.model))==scopeProfileJson(p),"A different profile already owns this optic. Remove it explicitly before importing a replacement.");
        requireScope(_scopeProfiles.contains(p.model)||_scopeProfiles.size()<128,"A project supports at most 128 optic profiles.");_scopeProfiles[p.model]=p;
    }else if(op=="select"){
        const auto i=params["index"].toInt(-1);requireScope(i>=0&&i<_scopePart->count(),"Select an installed optic.");_scopePart->setCurrentIndex(i);
    }else if(op=="pick"){
        requireScope(_workspace==ScopesWorkspace,"Open Scopes before picking.");requireScope(pickScopeLens({params["x"].toDouble(-1),params["y"].toDouble(-1)}),_lastError);
    }else if(op=="edit"){
        ScopeProfile p;QString error;requireScope(readScopeProfile(params["profile"].toObject(),p,error),error);
        const auto key=currentScopeKey();requireScope(_scopeProfiles.contains(key)&&p.model==key&&p.shape==_scopeProfiles.value(key).shape,"Pick a lens before editing; model and shape cannot be changed by an edit.");_scopeProfiles[key]=p;
    }else if(op=="save"){
        Transformf frame;requireScope(scopeFrame(frame),"Pick a valid lens first.");requireScope(saveSession(),_lastError);
    }else if(op=="remove")_scopeProfiles.remove(currentScopeKey());
    else requireScope(false,"Unknown scope-profile operation.");
    ++_revision;refreshScopeControls();updateScopeViewport();return scopeState();
}
}
