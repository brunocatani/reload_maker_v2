#include "ReloadMakerController.h"
#include "SourcePolicy.h"
#include "StudioWindow.h"
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
QString ReloadMakerController::sourcePolicyKey() const {
    return currentWeapon()?poseRecordKey(currentWeapon()->formId).toLower()+"|"+QFileInfo(suggestedSessionPath()).dir().dirName():QString();
}
QJsonObject ReloadMakerController::sourcePolicyState() const {
    if(!currentWeapon())throw std::runtime_error("Select the source weapon first");
    const auto key=sourcePolicyKey(),weapon=poseRecordKey(currentWeapon()->formId);
    QJsonObject policy;QString preset;
    if(_sourcePolicies.contains(key))policy=_sourcePolicies.value(key).toObject();
    else {
        QFile file(QCoreApplication::applicationDirPath()+"/assets/source-policies.json");
        if(!file.open(QIODevice::ReadOnly)||file.size()>1024*1024)throw std::runtime_error("Bundled source compatibility defaults are unavailable");
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse).object();
        if(parse.error!=QJsonParseError::NoError||doc["version"]!=1||!doc["policies"].isArray()||doc["policies"].toArray().size()>256)throw std::runtime_error("Bundled source compatibility defaults are invalid");
        for(const auto& value:doc["policies"].toArray()) {
            const auto entry=value.toObject();if(entry["weapon"].toString().compare(weapon,Qt::CaseInsensitive)!=0)continue;
            const auto candidate=entry["policy"].toObject();QString error;if(!validSourcePolicy(candidate,error))throw std::runtime_error(error.toStdString());
            if(candidate.contains("sourceOverride")) {
                const auto marker=candidate["sourceOverride"].toObject();const auto plugin=marker["marker"].toString().section(':',0,0);bool loaded=false;
                if(_libraryMode)loaded=_launchSnapshot.plugins.contains(plugin,Qt::CaseInsensitive);
                for(const auto& path:_package.plugins)loaded|=QFileInfo(path).fileName().compare(plugin,Qt::CaseInsensitive)==0;
                for(const auto& p:_plugins)loaded|=p.fileName.compare(plugin,Qt::CaseInsensitive)==0;
                if(loaded!=marker["present"].toBool())continue;
            }
            if(!preset.isEmpty())throw std::runtime_error("Several compatibility defaults match this source; resolve the policy definitions");
            policy=candidate;preset=entry["id"].toString();
        }
    }
    QMap<QString,QString> mods;const auto* config=configuration();
    if(config)for(const auto& slot:config->entries())for(auto id:slot.choices)if(const auto* mod=config->mod(id))mods[poseRecordKey(id)]=mod->name.isEmpty()?mod->editorId:mod->name;
    QJsonArray choices;for(auto it=mods.cbegin();it!=mods.cend();++it)choices.append(QJsonObject{{"record",it.key()},{"name",it.value()}});
    return {{"key",key},{"weapon",weapon},{"policy",policy},{"saved",_sourcePolicies.contains(key)},{"preset",preset},{"modifications",choices}};
}
QJsonObject ReloadMakerController::configureSourcePolicy(const QJsonObject& request) {
    if(_loadThread||_reloadCapturing||!currentWeapon())throw std::runtime_error("Wait for the current source load");
    const auto key=sourcePolicyKey();if(request["key"]!=key)throw std::runtime_error("Source context changed; reopen compatibility rules");
    if(request["operation"]=="reset")_sourcePolicies.remove(key);
    else if(request["operation"]=="save") {
        QString error;if(!request["policy"].isObject()||!validSourcePolicy(request["policy"].toObject(),error))throw std::runtime_error(error.isEmpty()?"Choose valid compatibility rules":error.toStdString());
        _sourcePolicies[key]=request["policy"];
    }else throw std::runtime_error("Choose save or reset for source compatibility");
    ++_revision;if(!saveSession())throw std::runtime_error("Cannot save source compatibility rules");return sourcePolicyState();
}
void ReloadMakerController::showSourcePolicy() {
    if(auto* existing=_window->findChild<QDialog*>("SourcePolicyDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=sourcePolicyState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("SourcePolicyDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Source compatibility rules");dialog->resize(820,690);
    auto* layout=new QVBoxLayout(dialog);auto* intro=new QLabel("These rules describe when this weapon's reload applies. Keep nonphysical magazine groups separate from actual magazines, and block conversions this reload does not support. Saving explicit rules applies them to every configuration of this weapon when you save to its pack. Animation and hand poses stay separate.");intro->setWordWrap(true);layout->addWidget(intro);
    const auto initial=state["policy"].toObject(),condition=initial["sourceOverride"].toObject();const auto key=state["key"].toString();
    auto* form=new QFormLayout;layout->addLayout(form);auto* marker=new QLineEdit(condition["marker"].toString());marker->setPlaceholderText("Optional · Addon.esp:000800");form->addRow("Add-on marker record",marker);
    auto* present=new QCheckBox("This marker must be installed (unchecked: must be absent)");present->setChecked(condition.value("present").toBool(true));layout->addWidget(present);
    auto* chooser=new QComboBox;chooser->setEditable(true);for(const auto& row:state["modifications"].toArray()){const auto item=row.toObject();chooser->addItem(item["name"].toString()+" · "+item["record"].toString(),item["record"]);}form->addRow("Available modification",chooser);
    const auto selected=[=]{return chooser->currentIndex()>=0&&chooser->currentText()==chooser->itemText(chooser->currentIndex())?chooser->currentData().toString():chooser->currentText().trimmed();};
    auto* useMarker=new QPushButton("Use selected modification as marker");layout->addWidget(useMarker);connect(useMarker,&QPushButton::clicked,dialog,[=]{marker->setText(selected());});
    auto* lists=new QHBoxLayout;layout->addLayout(lists,1);
    const auto list=[&](const QString& title,const char* field){auto* column=new QVBoxLayout;lists->addLayout(column);column->addWidget(new QLabel(title));auto* box=new QListWidget;column->addWidget(box,1);
        for(const auto& value:initial[field].toArray())box->addItem(value.toString());
        auto* add=new QPushButton("Add selected modification");auto* remove=new QPushButton("Remove selected rule");column->addWidget(add);column->addWidget(remove);
        connect(add,&QPushButton::clicked,dialog,[=]{const auto value=selected();if(!value.isEmpty()&&box->findItems(value,Qt::MatchFixedString).isEmpty())box->addItem(value);});
        connect(remove,&QPushButton::clicked,dialog,[=]{delete box->takeItem(box->currentRow());});return box;};
    auto* blocked=list("Block reload with these conversions","blockedMods");auto* collections=list("Magazine groups without a physical body","magazineCollections");
    auto* status=new QLabel(state["saved"].toBool()?"Saved source rules":"Source defaults · "+state["preset"].toString());status->setWordWrap(true);layout->addWidget(status);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* save=new QPushButton("Save rules");auto* reset=new QPushButton("Restore source defaults");actions->addWidget(save);actions->addWidget(reset);
    connect(save,&QPushButton::clicked,dialog,[=,this,expected=initial]()mutable{try{
        if(sourcePolicyState()["policy"].toObject()!=expected)throw std::runtime_error("Compatibility rules changed while editing; reopen them before saving");
        QJsonObject policy;if(!marker->text().trimmed().isEmpty())policy["sourceOverride"]=QJsonObject{{"marker",marker->text().trimmed()},{"present",present->isChecked()}};
        const auto rows=[](QListWidget* list){QJsonArray result;for(int i=0;i<list->count();++i)result.append(list->item(i)->text());return result;};
        if(blocked->count())policy["blockedMods"]=rows(blocked);if(collections->count())policy["magazineCollections"]=rows(collections);
        const auto result=configureSourcePolicy({{"operation","save"},{"key",key},{"policy",policy}});expected=result["policy"].toObject();status->setText("Saved. Capture and validate this source context before exporting.");
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{configureSourcePolicy({{"operation","reset"},{"key",key}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});dialog->show();
}
}
