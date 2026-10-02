#include "CollisionPanel.h"
#include "StudioPaths.h"
#include <QtConcurrent>
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
namespace {void require(bool ok,const QString& text){if(!ok)throw std::runtime_error(text.toStdString());}}
CollisionPanel::CollisionPanel(QWidget* parent):QWidget(parent){
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(14,14,14,14);
    auto label=[&](const QString& text){auto* w=new QLabel(text);w->setWordWrap(true);layout->addWidget(w);return w;};
    label("Collision workshop")->setObjectName("Section");
    label("Choose a weapon part, then generate its convex shapes. Orbit the model to inspect gaps and overlaps.");
    _target=new QComboBox;layout->addWidget(_target);
    auto button=[&](const QString& title,const QString& operation){auto* b=new QPushButton(title);layout->addWidget(b);connect(b,&QPushButton::clicked,this,[this,operation]{try{command({{"operation",operation}});}catch(const std::exception& e){showError(QString::fromUtf8(e.what()));}});return b;};
    button("GENERATE · ROCK DEFAULTS","defaults")->setObjectName("PrimaryButton");
    auto* form=new QFormLayout;
    auto spin=[&](const QString& name,double lo,double hi,double value,int decimals){auto* s=new QDoubleSpinBox;s->setRange(lo,hi);s->setDecimals(decimals);s->setValue(value);form->addRow(name,s);return s;};
    _gap=spin("Gap to preserve · game units",.01,100,1,2);_error=spin("Maximum support error",0,10,.5,3);_points=spin("Target points per hull",4,252,96,0);
    _mass=spin("Mass · kg",.001,100,2,3);_radius=spin("Convex radius · Havok",0,.1,.01,5);layout->addLayout(form);
    button("REGENERATE WITH THESE SETTINGS","generate");
    auto* import=button("LOAD DIAMOND HULL INPUT…","browse_input");import->setToolTip("Pre-Elric JSON from DIAMOND/assets/collision. Choose the installed magazine model as the reference.");
    _overlay=new QCheckBox("Show hulls · x-ray");_overlay->setChecked(true);layout->addWidget(_overlay);
    _compare=new QCheckBox("Compare previous result · blue wireframe");layout->addWidget(_compare);
    _isolate=new QCheckBox("Show selected hull only");layout->addWidget(_isolate);
    _hulls=new QListWidget;_hulls->setMinimumHeight(160);layout->addWidget(_hulls);
    label("WORLD MODEL PIECES")->setObjectName("Section");
    label("Unchecked pieces are excluded from the prepared model and collision. Use separate captures for full and empty presentations. Regenerate hulls after re-including a piece.")->setObjectName("Hint");
    _pieces=new QListWidget;_pieces->setMinimumHeight(160);layout->addWidget(_pieces);
    label("Yellow: selected · teal: ROCK · orange: imported. Uncheck a hull to exclude it from export. Save before changing the source, target or animation.")->setObjectName("Hint");
    button("SAVE / REPLACE THIS PROFILE","save");button("OPEN SAVED PROFILE…","browse_profile");button("EXPORT COLLISION JSON…","browse_export");button("CANCEL GENERATION","cancel");
    button("CAPTURE VISIBLE PART MODEL…","prepare");button("COOK PREPARED MAGAZINE…","cook");
    _status=label("No collision snapshot yet.");_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label("Preview shows convex point envelopes. Cook prepared magazine uses your configured Creation Kit to produce a finished collision model. Live Havok contacts still require an in-game test.")->setObjectName("Hint");layout->addStretch();
    connect(_target,qOverload<int>(&QComboBox::activated),this,[this]{invalidate();});
    connect(_hulls,&QListWidget::currentRowChanged,this,[this]{if(changed)changed();});
    connect(_hulls,&QListWidget::itemChanged,this,[this](QListWidgetItem* item){if(_refreshing)return;const int index=_hulls->row(item);if(index>=0&&index<_profile.hulls.size()){_profile.hulls[index].enabled=item->checkState()==Qt::Checked;_dirty=true;refresh();}});
    connect(_pieces,&QListWidget::itemChanged,this,[this](QListWidgetItem* item){if(_refreshing)return;const int index=_pieces->row(item);if(index<0||index>=_profile.sources.size())return;auto& source=_profile.sources[index];source.worldIncluded=item->checkState()==Qt::Checked;if(!source.worldIncluded)for(auto& hull:_profile.hulls)if(hull.source==source.key)hull.enabled=false;_dirty=true;refresh();});
    for(auto* check:{_overlay,_compare,_isolate})connect(check,&QCheckBox::toggled,this,[this]{if(changed)changed();});
    for(auto* s:{_mass,_radius})connect(s,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{if(_refreshing||!_profile.valid())return;_profile.settings.mass=_mass->value();_profile.settings.radius=_radius->value();_dirty=true;refresh();});
    connect(&_worker,&QFutureWatcher<CollisionResult>::finished,this,[this]{
        const auto result=_worker.result();_generating=false;if(_cancel&&_cancel->load()){refresh();return;}
        if(!result.error.isEmpty()){showError(result.error);return;}_baseline=_profile;_profile=result.profile;_dirty=true;_lastError.clear();refresh();
    });
}
CollisionPanel::~CollisionPanel(){cancel();_worker.waitForFinished();}
void CollisionPanel::cancel(){if(_cancel)_cancel->store(true);}
void CollisionPanel::invalidate(){cancel();_profile={};_baseline={};_dirty=false;_lastPath.clear();_lastError.clear();refresh();}
void CollisionPanel::setTargets(const QStringList& names){if(_target->count()==names.size()){bool same=true;for(int i=0;i<names.size();++i)same&=_target->itemText(i)==names[i];if(same)return;}invalidate();_target->clear();_target->addItems(names);}
bool CollisionPanel::overlay()const{return _overlay->isChecked();}bool CollisionPanel::compare()const{return _compare->isChecked();}bool CollisionPanel::isolated()const{return _isolate->isChecked();}int CollisionPanel::selected()const{return _hulls->currentRow();}
void CollisionPanel::showError(const QString& error){_lastError=error;refresh();}
void CollisionPanel::refresh(){
    _refreshing=true;const int row=_hulls->currentRow();_hulls->clear();for(const auto& h:_profile.hulls){auto* item=new QListWidgetItem(QString("%1 · %2 points").arg(h.name).arg(h.points.size()),_hulls);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(h.enabled?Qt::Checked:Qt::Unchecked);}
    _hulls->setCurrentRow(_hulls->count()?std::clamp(row,0,_hulls->count()-1):-1);
    _pieces->clear();for(const auto& source:_profile.sources){auto* item=new QListWidgetItem(source.name+"\n"+QFileInfo(source.material).fileName(),_pieces);item->setToolTip(source.path+"\n"+source.material);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(source.worldIncluded?Qt::Checked:Qt::Unchecked);}
    if(_profile.valid()){_mass->setValue(_profile.settings.mass);_radius->setValue(_profile.settings.radius);}
    QString text=_generating?"Generating on a cancellable worker…":_profile.valid()?QString("%1 hulls · %2 source triangles\n%3\n%4").arg(_profile.hulls.size()).arg(_profile.inputTriangles).arg(_dirty?"Unsaved changes":"Saved profile").arg(_lastPath):"No collision snapshot yet.";
    if(!_profile.warnings.isEmpty())text+="\n"+_profile.warnings.join('\n');
    if(!_lastError.isEmpty())text+="\n"+_lastError;
    _status->setText(text);_refreshing=false;if(changed)changed();
}
CollisionProfile CollisionPanel::context(bool referenceOnly){require(bool(snapshot),"No scene provider");auto p=snapshot(_target->currentIndex(),referenceOnly);if(p.weapon==_profile.weapon&&p.configuration==_profile.configuration&&p.target==_profile.target)for(auto& source:p.sources)for(const auto& old:_profile.sources)if(source.key==old.key&&source.geometryHash==old.geometryHash)source.worldIncluded=old.worldIncluded;p.settings.gap=_gap->value();p.settings.error=_error->value();p.settings.targetPoints=int(_points->value());p.settings.mass=_mass->value();p.settings.radius=_radius->value();return p;}
QString CollisionPanel::savePath()const{QString name=_profile.weapon+"_"+_target->currentText();name.replace(QRegularExpression("[^A-Za-z0-9_-]+"),"_");name=name.left(80);const auto key=(_profile.weapon+"|"+_profile.configuration+"|"+_profile.target).toUtf8();return studioDataPath("collisions/"+name+"_"+QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".json");}
QJsonObject CollisionPanel::state()const{auto out=collisionSummary(_profile);QJsonArray targets;for(int i=0;i<_target->count();++i)targets.append(QJsonObject{{"index",i},{"name",_target->itemText(i)}});out["targets"]=targets;out["targetIndex"]=_target->currentIndex();out["busy"]=_generating;out["dirty"]=_dirty;out["error"]=_lastError;out["path"]=_lastPath;out["selectedHull"]=selected();out["overlay"]=overlay();out["compare"]=compare();out["isolated"]=isolated();return out;}
QJsonObject CollisionPanel::command(const QJsonObject& args){
    const auto op=args["operation"].toString();if(op=="cancel"){cancel();return state();}require(!_generating,"Collision generation is still running; wait or cancel");
    if(args.contains("target")){const auto index=args["target"].toInt(-1);require(index>=0&&index<_target->count(),"Unknown collision target");if(index!=_target->currentIndex()){invalidate();_target->setCurrentIndex(index);}}
    _lastError.clear();
    if(op=="select"){}
    else if(op=="prepare"){require(bool(prepare),"Visible part capture is unavailable");prepare();return state();}
    else if(op=="cook"){require(_profile.valid()&&bool(cook),"Generate a magazine collision profile first");cook();return state();}
    else if(op=="defaults"||op=="generate"){
        if(op=="defaults"){_gap->setValue(1);_error->setValue(.5);_points->setValue(96);_radius->setValue(.01);}
        for(auto pair:{std::pair{"gapGame",_gap},std::pair{"supportErrorGame",_error},std::pair{"targetPoints",_points}})if(args.contains(pair.first)){const auto v=args[pair.first];require(v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=pair.second->minimum()&&v.toDouble()<=pair.second->maximum(),"Invalid generation setting");pair.second->setValue(v.toDouble());}
        auto p=context(false);_cancel=std::make_shared<std::atomic<bool>>(false);auto stop=_cancel;_generating=true;_worker.setFuture(QtConcurrent::run([p=std::move(p),stop]()mutable{return bakeCollision(std::move(p),stop);}));
    }else if(op=="import"||op=="open"||op=="browse_input"||op=="browse_profile"){
        QString path=args["path"].toString();const bool legacy=op=="import"||op=="browse_input";
        if(op.startsWith("browse"))path=QFileDialog::getOpenFileName(this,legacy?"Choose pre-Elric hull JSON and verify its reference part":"Open collision profile",legacy?"F:/fo4dev/PROJECT_ROCK_V2/main_projects/DIAMOND/assets/collision":studioDataPath("collisions"),"Collision JSON (*.json)");
        if(path.isEmpty())return state();
        QFile file(path);require(file.size()<=32*1024*1024&&file.open(QIODevice::ReadOnly),"Cannot read collision JSON (maximum 32 MB)");const auto bytes=file.readAll();QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);require(parse.error==QJsonParseError::NoError&&doc.isObject(),"Invalid collision JSON");CollisionProfile next;QString error;
        if(legacy){auto result=importCollisionHullInput(doc.object(),context(true));require(result.error.isEmpty(),result.error);next=std::move(result.profile);next.sourceHash=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
        else{require(readCollisionJson(doc.object(),next,error),error);const auto current=context(next.origin=="diamond-pre-elric-input");require(next.weapon==current.weapon&&next.configuration==current.configuration&&next.target==current.target,"Profile belongs to a different weapon, loadout or target");for(auto& source:next.sources){auto it=std::find_if(current.sources.begin(),current.sources.end(),[&](const auto& s){return s.key==source.key&&s.geometryHash==source.geometryHash;});require(it!=current.sources.end(),"Source geometry/pose changed; regenerate or import against its explicit reference");source.frame=it->frame;source.material=it->material;}next.referencePose["rootToWorld"]=current.referencePose["rootToWorld"];}
        _baseline=_profile;_profile=std::move(next);_dirty=legacy;_lastPath=path;
    }else if(op=="save"||op=="export"||op=="browse_export"||op=="export-hulls"){
        require(_profile.valid(),"Generate or load a profile first");require(std::any_of(_profile.hulls.begin(),_profile.hulls.end(),[](const auto& h){return h.enabled;}),"Enable at least one hull");QString path=op=="save"?savePath():args["path"].toString();
        if(op=="browse_export")path=QFileDialog::getSaveFileName(this,"Export collision points",studioDataPath("exports/collision.json"),"Collision JSON (*.json)");
        if(path.isEmpty())return state();
        const auto full=QFileInfo(path).absoluteFilePath();require(full.startsWith(QDir(studioRoot()).absolutePath()+"/",Qt::CaseInsensitive),"Save collision profiles inside the Reload Maker V2 workspace");
        QString error;const auto document=op=="export-hulls"?collisionCookInput(_profile,error):collisionJson(_profile);require(error.isEmpty(),error);
        QDir().mkpath(QFileInfo(full).absolutePath());QSaveFile file(full);const auto bytes=QJsonDocument(document).toJson();require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Could not save collision profile");_lastPath=full;if(op!="export-hulls")_dirty=false;
    }else if(op=="piece"){
        const int i=args["index"].toInt(-1);require(i>=0&&i<_profile.sources.size()&&args["included"].isBool(),"Choose a source piece and included boolean");auto& source=_profile.sources[i];source.worldIncluded=args["included"].toBool();if(!source.worldIncluded)for(auto& hull:_profile.hulls)if(hull.source==source.key)hull.enabled=false;_dirty=true;
    }else if(op=="hull"){
        const int i=args["index"].toInt(-1);require(i>=0&&i<_profile.hulls.size(),"Unknown hull");_hulls->setCurrentRow(i);if(args.contains("enabled")){require(args["enabled"].isBool(),"Expected enabled boolean");_profile.hulls[i].enabled=args["enabled"].toBool();_dirty=true;}
    }else if(op=="view"){
        for(auto pair:{std::pair{"overlay",_overlay},std::pair{"compare",_compare},std::pair{"isolated",_isolate}})if(args.contains(pair.first)){require(args[pair.first].isBool(),"Expected view boolean");pair.second->setChecked(args[pair.first].toBool());}
    }else require(false,"Unknown collision operation");refresh();return state();
}
}
