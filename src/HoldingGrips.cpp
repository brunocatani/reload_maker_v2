#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "model/nifmodel.h"
#include "physics-interaction/weapon/HoldingGripMath.h"
#include <QQuaternion>
#include <QJsonArray>
#include <QDir>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFileDialog>
#include <QDialog>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QUuid>
#include <QDateTime>
namespace reloadmaker {
namespace {
namespace hm=rock::holding_grip_math;
hm::Pose mathPose(const Transformf& t){hm::Pose p;const auto m=QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z).toRotationMatrix();for(int i=0;i<3;++i)for(int j=0;j<3;++j)p.r[i*3+j]=m(i,j);p.t={t.translation.x,t.translation.y,t.translation.z};p.s=t.scale.x;return p;}
Transformf previewPose(const hm::Pose& p){QMatrix3x3 m;for(int i=0;i<3;++i)for(int j=0;j<3;++j)m(i,j)=p.r[i*3+j];const auto q=QQuaternion::fromRotationMatrix(m).normalized();return {{p.t[0],p.t[1],p.t[2]},{q.x(),q.y(),q.z(),q.scalar()},{p.s,p.s,p.s}};}
QJsonObject jsonPose(const Transformf& t){const auto p=mathPose(t);QJsonArray r,a;for(auto x:p.r)r.append(x);for(auto x:p.t)a.append(x);return {{"rotate",r},{"translate",a},{"scale",p.s}};}
Transformf readPose(const QJsonValue& value){const auto j=value.toObject();const auto r=j["rotate"].toArray(),t=j["translate"].toArray();if(r.size()!=9||t.size()!=3||!j["scale"].isDouble())throw std::runtime_error("Invalid holding pose");hm::Pose p;for(int i=0;i<9;++i){if(!r[i].isDouble())throw std::runtime_error("Holding rotation entries must be numbers");p.r[i]=float(r[i].toDouble());}for(int i=0;i<3;++i){if(!t[i].isDouble())throw std::runtime_error("Holding translation entries must be numbers");p.t[i]=float(t[i].toDouble());}p.s=float(j["scale"].toDouble());if(!hm::valid(p))throw std::runtime_error("Holding pose must be finite with a proper rotation and positive scale");return previewPose(p);}
QJsonObject poseRecord(const HandPose& p){QJsonArray fingers;for(const auto& f:p.fingers)fingers.append(jsonPose(f));return {{"wrist",jsonPose(p.placement.handInWeapon)},{"fingers",fingers},{"hand",p.hand==PoseHand::Left?"left":"right"}};}
void readRecord(const QJsonObject& j,HandPose& p){p.placement.handInWeapon=p.placement.handInAnchor=readPose(j["wrist"]);const auto fingers=j["fingers"].toArray();if(fingers.size()!=15)throw std::runtime_error("A holding grip needs fifteen finger locals");for(int i=0;i<15;++i)p.fingers[i]=readPose(fingers[i]);}
}
QString ReloadMakerController::holdingGripVariant() const {
 auto* model=activeModel();if(!model)return {};for(int i=0;i<_sceneNodes.size();++i)if(_sceneNodes[i].isValid()&&model->get<QString>(_sceneNodes[i],"Name")=="P-Grip")for(int j=0;j<_assemblyNodes.size();++j)if(_assemblyNodes[j].parent==i&&_sceneNodes[j].isValid())return model->get<QString>(_sceneNodes[j],"Name");if(currentWeapon()&&_holdingCapture.value("weapon").toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0)return _holdingCapture.value("gripNode").toString();return {};
}
QString ReloadMakerController::holdingGripKey(PoseHand hand,bool specific) const {const auto weapon=currentWeapon()?poseRecordKey(currentWeapon()->formId).toLower():QString();const bool pa=_holdingCapture.value("weapon").toString().compare(weapon,Qt::CaseInsensitive)==0&&_holdingCapture.value("powerArmor").toBool();return weapon+'|'+(specific?holdingGripVariant():QString())+'|'+(pa?"pa|":"normal|")+(hand==PoseHand::Left?"left":"right");}
QJsonObject ReloadMakerController::holdingGripsState() const {
 QJsonObject out{{"enabled",_holdingPreview},{"hand",_holdingHand==PoseHand::Left?"left":"right"},{"support",_holdingSupport},{"editing",_holdingEditing},{"replay",_holdingReplay},{"specific",_holdingSpecific},{"gripNode",holdingGripVariant()},{"error",_holdingError},{"calibrated",_holdingCapture.contains("rightBoneInWand")&&_holdingCapture.contains("leftBoneInWand")},{"capture",_holdingCapture},{"grips",_holdingGrips}};
 QJsonObject drafts=_holdingWorking;if(_holdingEditing&&_partGripDraft){auto pose=_partGripDraft->pose;pose.placement.handInWeapon=pose.placement.handInAnchor;if(_fingerDrafts[int(pose.hand)])pose.fingers=_fingerDrafts[int(pose.hand)]->fingers;drafts[holdingGripKey(pose.hand,_holdingSpecific)]=poseRecord(pose);}out["drafts"]=drafts;
 if(_holdingPreview&&_holdingError.isEmpty()){
  const auto expected=_holdingCapture.value(_holdingHand==PoseHand::Left?"renderedLeft":"renderedRight").toObject();
  if(!_holdingReplay&&!expected.isEmpty()&&currentWeapon()&&_holdingCapture.value("weapon").toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0&&_holdingCapture.value("holdingHand")==(_holdingHand==PoseHand::Left?"left":"right"))try{const auto a=mathPose(_holdingSolved[int(_holdingHand)].placement.handInWeapon),b=mathPose(readPose(expected["wrist"]));float distance=0,rotation=0;for(int i=0;i<3;++i)distance+=(a.t[i]-b.t[i])*(a.t[i]-b.t[i]);for(int i=0;i<9;++i)rotation=std::max(rotation,std::abs(a.r[i]-b.r[i]));out["wristDifferenceGameUnits"]=std::sqrt(distance);out["rotationMatrixDifference"]=rotation;float trace=0;for(int i=0;i<9;++i)trace+=a.r[i]*b.r[i];out["wristAngleDegrees"]=std::acos(std::clamp((trace-1)*.5f,-1.f,1.f))*57.295779513;}catch(const std::exception&){}
  out["left"]=poseRecord(_holdingSolved[0]);out["right"]=poseRecord(_holdingSolved[1]);}return out;
}
bool ReloadMakerController::applyHoldingPreview(){
 if(auto* b=_window->findChild<QPushButton*>("HoldingGripButton"))b->setText(_holdingPreview?(_holdingHand==PoseHand::Left?"Holding: Left":"Holding: Right"):"Holding grip");
 if(!_holdingPreview||_reloadCapturing||!_clip.valid()||_weaponBoneIndex<0)return true;
 _holdingError.clear();
 try{
  // The native source is immutable. Always derive both sides from Ready, never
  // from the previously rendered hand or a mirrored authoring preview.
  std::array<HandPose,2> source;
  const auto& locals=_readyLocalPose;const auto& globals=_readyGlobalPose;
  if(locals.size()!=_skeleton.bones.size()||globals.size()!=locals.size())throw std::runtime_error("Ready pose unavailable");
  for(auto hand:{PoseHand::Left,PoseHand::Right}){auto& p=source[int(hand)];p.hand=hand;p.name="Firing grip";p.placement.anchorKind="weapon";p.placement.anchorName="Weapon";QString error;if(!captureHandPose(_skeleton,locals,globals,globals[_weaponBoneIndex],p,error))throw std::runtime_error(error.toStdString());}
  const bool matchingCapture=currentWeapon()&&_holdingCapture.value("weapon").toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0&&(_holdingCapture.value("gripNode").toString().isEmpty()||_holdingCapture.value("gripNode").toString()==holdingGripVariant());
  if(matchingCapture&&_holdingCapture.contains("nativeRight")){
   source[1].placement.handInWeapon=source[1].placement.handInAnchor=readPose(_holdingCapture.value("nativeRight"));const auto f=_holdingCapture.value("nativeFingers").toArray();if(f.size()==15)for(int i=0;i<15;++i)source[1].fingers[i]=readPose(f[i]);
   if(_holdingCapture.contains("nativeSupport")){source[0].placement.handInWeapon=source[0].placement.handInAnchor=readPose(_holdingCapture.value("nativeSupport"));const auto sf=_holdingCapture.value("supportFingers").toArray();if(sf.size()==15)for(int i=0;i<15;++i)source[0].fingers[i]=readPose(sf[i]);}
  }
  _holdingSolved=source;
  if(_holdingHand==PoseHand::Left){
   HandPose firing,support;QString error;
   if(!mirrorHandPose(_skeleton,source[1],{},firing,error)||!mirrorHandPose(_skeleton,source[0],{},support,error))throw std::runtime_error(error.toStdString());
   // Offline reference-basis preview remains available. Imported ROCK frames
   // replace its wrist calculation; no guessed wand basis is called calibrated.
   if(_holdingCapture.contains("rightBoneInWand")&&_holdingCapture.contains("leftBoneInWand")){
    const auto br=mathPose(readPose(_holdingCapture.value("rightBoneInWand"))),bl=mathPose(readPose(_holdingCapture.value("leftBoneInWand")));
    const auto array=[](QJsonValue value,hm::V fallback){const auto a=value.toArray();if(a.size()==3)for(int i=0;i<3;++i)fallback[i]=float(a[i].toDouble());return fallback;};
    const auto rp=array(_holdingCapture.value("palmRight"),{6,-2,.2f}),lp=array(_holdingCapture.value("palmLeft"),{6,-2,-.2f});
    const auto right=mathPose(source[1].placement.handInWeapon);const auto grip=hm::point(right,rp);
    auto left=hm::leftFiring(right,br,bl,grip);
    const auto offsets=array(_holdingCapture.value("leftGripOffset"),{});hm::V calibration{offsets[0],offsets[1]+.6f,-offsets[2]};left.t=hm::add(left.t,hm::sub(hm::point(left,calibration),left.t));
    firing.placement.handInWeapon=firing.placement.handInAnchor=previewPose(left);
    auto sr=hm::rightSupport(mathPose(source[0].placement.handInWeapon),bl,br,lp,rp);const auto so=array(_holdingCapture.value("rightSupportOffset"),{});hm::V shift{so[0]-.9f,so[1],-so[2]};sr.t=hm::point(sr,shift);support.placement.handInWeapon=support.placement.handInAnchor=previewPose(sr);
   }
   for(int i=0;i<15;++i){firing.fingers[i]=previewPose(hm::finger(mathPose(source[1].fingers[i])));support.fingers[i]=previewPose(hm::finger(mathPose(source[0].fingers[i])));}
   _holdingSolved[0]=firing;_holdingSolved[1]=support;
  }
  const int firing=int(_holdingHand),other=1-firing;
  const auto key=_holdingGrips.contains(holdingGripKey(_holdingHand))?holdingGripKey(_holdingHand):holdingGripKey(_holdingHand,false);
  if(_holdingGrips.contains(key))readRecord(_holdingGrips[key].toObject(),_holdingSolved[firing]);
  const auto workingKey=holdingGripKey(_holdingHand,_holdingSpecific);if(_holdingWorking.contains(workingKey))readRecord(_holdingWorking.value(workingKey).toObject(),_holdingSolved[firing]);
  if(_holdingEditing&&_partGripDraft&&_partGripDraft->pose.hand==_holdingHand){_holdingSolved[firing]=_partGripDraft->pose;_holdingSolved[firing].placement.handInWeapon=_partGripDraft->pose.placement.handInAnchor;}
  if(_holdingEditing&&_fingerDrafts[firing])_holdingSolved[firing].fingers=_fingerDrafts[firing]->fingers;
  if(!_holdingSupport){auto& t=_holdingSolved[other].placement.handInWeapon.translation;t.x+=(other==0?-18:18);t.z-=18;}
  if(_holdingReplay){
   if(!currentWeapon()||_holdingCapture.value("weapon").toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)!=0)throw std::runtime_error("This capture belongs to another weapon; turn off captured-pose display to preview this weapon");
   for(int h=0;h<2;++h){const auto j=_holdingCapture.value(h==0?"renderedLeft":"renderedRight").toObject();if(!j.isEmpty()){_holdingSolved[h].placement.handInWeapon=readPose(j["wrist"]);if(j["fingers"].toArray().size()==15)for(int i=0;i<15;++i)_holdingSolved[h].fingers[i]=readPose(j["fingers"].toArray()[i]);}}
  }
  const auto weapon=_globalPose[_weaponBoneIndex];
  for(int h=0;h<2;++h){if(_simulationActive&&_simulation.action==SimulationAction::Held&&h==int(_interactionHand))continue;const auto& p=_holdingSolved[h];const auto target=composed(weapon,p.placement.handInWeapon);if(!solvePartGrip(_skeleton,_localPose,_globalPose,p,target,_localPose,_globalPose,_holdingError,GripReach::FollowTarget,true))return true;}
  return evaluateSceneBindings(_skeleton,_nodeBindings,_sourceLocalPose,_stateBaselineLocalPose,_globalPose,_boundLocalPose,_boundGlobalPose,_poseError)&&applySimulationPose();
 }catch(const std::exception& e){_holdingError=QString::fromUtf8(e.what());return true;}
}
QJsonObject ReloadMakerController::holdingGripsCommand(const QJsonObject& p){
 const auto op=p["operation"].toString("preview");
 const auto stash=[this]{if(_holdingEditing&&_partGripDraft){auto pose=_partGripDraft->pose;pose.placement.handInWeapon=pose.placement.handInAnchor;if(_fingerDrafts[int(pose.hand)])pose.fingers=_fingerDrafts[int(pose.hand)]->fingers;_holdingWorking[holdingGripKey(pose.hand,_holdingSpecific)]=poseRecord(pose);_partGripDraft.reset();_fingerDrafts[int(pose.hand)].reset();}_holdingEditing=false;};
 if(op=="open"){showHoldingGrips();return holdingGripsState();}
 if(_loadThread||_reloadCapturing||espBusy())throw std::runtime_error("Wait for the current source or capture operation before editing holding grips");
 if(op=="preview"){
  if(!p["enabled"].toBool(true)||(p.contains("hand")&&(p["hand"]=="left")!=(_holdingHand==PoseHand::Left))||(p.contains("specific")&&p["specific"].toBool()!=_holdingSpecific))stash();
  _holdingPreview=p["enabled"].toBool(true);if(p.contains("hand")){if(p["hand"]!="left"&&p["hand"]!="right")throw std::runtime_error("Choose left or right");_holdingHand=p["hand"]=="left"?PoseHand::Left:PoseHand::Right;}
  if(p.contains("support"))_holdingSupport=p["support"].toBool();if(p.contains("replay"))_holdingReplay=p["replay"].toBool();if(p.contains("specific"))_holdingSpecific=p["specific"].toBool();
  if(_holdingReplay&&(_holdingCapture.value("holdingHand")=="left"||_holdingCapture.value("holdingHand")=="right"))_holdingHand=_holdingCapture.value("holdingHand")=="left"?PoseHand::Left:PoseHand::Right;
 }else if(op=="clear-capture"){_holdingCapture={};_holdingReplay=false;
 }else if(op=="import-capture"){
  QFile f(p["path"].toString());if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot open runtime capture");const auto j=QJsonDocument::fromJson(f.readAll()).object();if(j["format"]!="rock-holding-grip-capture"||j["version"]!=1)throw std::runtime_error("Unknown holding capture");(void)readPose(j["rightBoneInWand"]);(void)readPose(j["leftBoneInWand"]);_holdingCapture=j;
 }else if(op=="edit"){
  if(!_holdingPreview||!currentWeapon()||!_clip.valid())throw std::runtime_error("Load a native pose and enable the holding preview first");(void)applyPose(_playbackTime);if(!_holdingError.isEmpty())throw std::runtime_error(_holdingError.toStdString());
  auto pose=_holdingSolved[int(_holdingHand)];pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);pose.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);pose.placement.handInAnchor=pose.placement.handInWeapon;pose.source=currentPoseSource();
  setWorkspace(2);setHandEditorPage(HandEditorPage::PartGrip);int anchor=-1;for(int i=0;i<_handPoseAnchors.size();++i)if(_handPoseAnchors[i].description.anchorKind=="weapon"){anchor=i;break;}if(anchor<0)throw std::runtime_error("Weapon reference is unavailable");
  QString error;if(!preparePartGripFrame(anchor,error))throw std::runtime_error(error.toStdString());setGripAnchor(anchor);_editingMagazine=false;_partGripDraft=PartGrip{pose,poseRecordKey(currentWeapon()->formId),pose.id};setGripDraftName(_holdingHand==PoseHand::Left?"Left firing grip":"Right firing grip");setEditedHand(_holdingHand);_fingerDrafts[int(_holdingHand)].reset();_holdingEditing=true;showPartGripValues();
 }else if(op=="fingers"){
  if(!_holdingEditing||!_partGripDraft)holdingGripsCommand({{"operation","edit"}});setEditedHand(_holdingHand);setWorkspace(2);setHandEditorPage(HandEditorPage::FingerPoses);
 }else if(op=="save"){
  if(!_holdingEditing||!_partGripDraft||_partGripDraft->pose.hand!=_holdingHand||!currentWeapon()||_partGripDraft->weaponRecord.compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)!=0)throw std::runtime_error("Use Edit placement before saving a firing grip");auto pose=_partGripDraft->pose;pose.placement.handInWeapon=pose.placement.handInAnchor;if(_fingerDrafts[int(pose.hand)])pose.fingers=_fingerDrafts[int(pose.hand)]->fingers;auto j=poseRecord(pose);j["weapon"]=poseRecordKey(currentWeapon()->formId);j["gripNode"]=_holdingSpecific?holdingGripVariant():QString();j["powerArmor"]=_holdingCapture.value("weapon").toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0&&_holdingCapture.value("powerArmor").toBool();_holdingGrips[holdingGripKey(pose.hand,_holdingSpecific)]=j;_holdingWorking.remove(holdingGripKey(pose.hand,_holdingSpecific));_partGripDraft->pose.fingers=pose.fingers;_fingerDrafts[int(pose.hand)].reset();
 }else if(op=="reset"){
  _holdingGrips.remove(holdingGripKey(_holdingHand,_holdingSpecific));_holdingWorking.remove(holdingGripKey(_holdingHand,_holdingSpecific));_holdingEditing=false;_partGripDraft.reset();_fingerDrafts[int(_holdingHand)].reset();
 }else if(op=="export"){
  QJsonArray grips;for(const auto& v:_holdingGrips)if(currentWeapon()&&v.toObject()["weapon"].toString().compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0)grips.append(v);if(!currentWeapon())throw std::runtime_error("Load a weapon before exporting firing grips");QFileInfo info(p["path"].toString());QDir().mkpath(info.absolutePath());QSaveFile f(info.absoluteFilePath());if(!f.open(QIODevice::WriteOnly)||f.write(QJsonDocument(QJsonObject{{"format","rock-holding-grips"},{"version",1},{"space","weapon-local"},{"fingerSpace","parent-local"},{"matrixConvention","active-row-major"},{"units","game"},{"grips",grips}}).toJson())<0||!f.commit())throw std::runtime_error("Cannot save holding grips");
 }else throw std::runtime_error("Unknown holding-grip operation");
 if(_clip.valid())(void)applyPose(_playbackTime);if(auto* b=_window->findChild<QPushButton*>("HoldingGripButton"))b->setText(_holdingPreview?(_holdingHand==PoseHand::Left?"Holding: Left":"Holding: Right"):"Holding grip");noteDocumentChange("holding grip");return holdingGripsState();
}
void ReloadMakerController::showHoldingGrips(){
 auto* d=new QDialog(_window);d->setAttribute(Qt::WA_DeleteOnClose);d->setWindowTitle("Holding hand and firing grips");auto* layout=new QVBoxLayout(d);auto* side=new QComboBox(d);side->addItem("Right firing hand","right");side->addItem("Left firing hand","left");side->setCurrentIndex(_holdingHand==PoseHand::Left?1:0);layout->addWidget(side);auto* enabled=new QCheckBox("Preview holding grip",d);enabled->setChecked(_holdingPreview);layout->addWidget(enabled);auto* support=new QCheckBox("Place the other hand on its support grip",d);support->setChecked(_holdingSupport);layout->addWidget(support);auto* specific=new QCheckBox("Use captured grip variant when available (off: weapon default)",d);specific->setChecked(_holdingSpecific);layout->addWidget(specific);auto* replay=new QCheckBox("Show captured runtime hands",d);replay->setToolTip("Uses the capture's holding hand. Turn this off to switch prediction sides.");replay->setChecked(_holdingReplay);layout->addWidget(replay);auto* status=new QLabel(d);status->setWordWrap(true);layout->addWidget(status);
 const auto refresh=[=,this]{side->setCurrentIndex(_holdingHand==PoseHand::Left?1:0);status->setText(!_holdingError.isEmpty()?_holdingError:!_holdingCapture.contains("rightBoneInWand")?"Offline skeleton preview. Import a ROCK capture to reproduce its controller calibration. Reload-hand selection remains independent.":"ROCK calibration loaded. Toggle captured runtime hands to compare with the prediction.");const auto state=holdingGripsState();if(state.contains("wristDifferenceGameUnits"))status->setText(status->text()+QString("\nWrist difference: %1 game units, %2 degrees.").arg(state["wristDifferenceGameUnits"].toDouble(),0,'f',3).arg(state["wristAngleDegrees"].toDouble(),0,'f',2));};
 const auto invoke=[=,this](QJsonObject p){try{holdingGripsCommand(p);}catch(const std::exception& e){status->setText(e.what());return;}refresh();};
 const auto preview=[=,this]{invoke({{"operation","preview"},{"enabled",enabled->isChecked()},{"hand",side->currentData().toString()},{"support",support->isChecked()},{"replay",replay->isChecked()},{"specific",specific->isChecked()}});};
 connect(side,qOverload<int>(&QComboBox::activated),d,[=,this](int){preview();});connect(enabled,&QCheckBox::toggled,d,[=,this](bool){preview();});connect(support,&QCheckBox::toggled,d,[=,this](bool){preview();});connect(replay,&QCheckBox::toggled,d,[=,this](bool){preview();});connect(specific,&QCheckBox::toggled,d,[=,this](bool){preview();});
 for(const auto& pair:std::initializer_list<std::pair<QString,QString>>{{"Use source-only preview","clear-capture"},{"Edit wrist placement","edit"},{"Edit finger joints","fingers"},{"Save this hand's firing grip","save"},{"Reset this hand in selected scope","reset"}}){auto* b=new QPushButton(pair.first,d);layout->addWidget(b);connect(b,&QPushButton::clicked,d,[=,this]{invoke({{"operation",pair.second}});});}
 auto* capture=new QPushButton("Import ROCK capture…",d);layout->addWidget(capture);connect(capture,&QPushButton::clicked,d,[=,this]{const auto path=QFileDialog::getOpenFileName(d,"ROCK holding capture","F:/fo4dev/misc/rock-holding-grip-captures","JSON (*.json)");if(!path.isEmpty())invoke({{"operation","import-capture"},{"path",path}});});
 auto* exportButton=new QPushButton("Export firing grips for ROCK…",d);layout->addWidget(exportButton);connect(exportButton,&QPushButton::clicked,d,[=,this]{const auto path=QFileDialog::getSaveFileName(d,"Save under your ROCK mod's F4SE/Plugins/ROCK/HoldingGrips folder",QString("D:/FO4/mods/ROCK/F4SE/Plugins/ROCK/HoldingGrips/")+(currentWeapon()?poseRecordKey(currentWeapon()->formId).replace(':','_'):QString("holding-grips"))+".json","JSON (*.json)");if(!path.isEmpty())invoke({{"operation","export"},{"path",path}});});refresh();d->resize(450,360);d->show();
}
}
