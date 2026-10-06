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
void ReloadMakerController::showSourcePolicy() {
    if(auto* existing=_window->findChild<QDialog*>("SourcePolicyDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=sourcePolicyState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("SourcePolicyDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Source compatibility rules");dialog->resize(820,690);
    auto* layout=new QVBoxLayout(dialog);auto* intro=new QLabel("These rules describe when this weapon's reload applies. Keep nonphysical magazine groups separate from actual magazines, and block conversions this reload does not support. Saving explicit rules applies them to every configuration of this weapon when you save to its pack. Animation and hand poses stay separate.");intro->setWordWrap(true);layout->addWidget(intro);
    const auto initial=state["policy"].toObject(),condition=initial["sourceOverride"].toObject();const auto key=state["key"].toString();
    auto* form=new QFormLayout;layout->addLayout(form);auto* marker=new QLineEdit(condition["marker"].toString());marker->setPlaceholderText("Optional · Addon.esp:000800");form->addRow("Add-on marker record",marker);
    auto* present=new QCheckBox("This marker must be installed (unchecked: must be absent)");present->setChecked(condition.value("present").toBool(true));layout->addWidget(present);
    QStringList equivalentRecords;for(const auto& value:initial["weaponAliases"].toArray())equivalentRecords.append(value.toString());
    auto* equivalents=new QLineEdit(equivalentRecords.join(", "));equivalents->setPlaceholderText("Same reload assembly only · Plugin.esp:001234, Plugin.esp:001235");form->addRow("Equivalent weapon records",equivalents);
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
        QJsonArray aliases;for(const auto& value:equivalents->text().split(',',Qt::SkipEmptyParts))aliases.append(value.trimmed());if(!aliases.isEmpty())policy["weaponAliases"]=aliases;
        const auto rows=[](QListWidget* list){QJsonArray result;for(int i=0;i<list->count();++i)result.append(list->item(i)->text());return result;};
        if(blocked->count())policy["blockedMods"]=rows(blocked);if(collections->count())policy["magazineCollections"]=rows(collections);
        const auto result=configureSourcePolicy({{"operation","save"},{"key",key},{"policy",policy}});expected=result["policy"].toObject();status->setText("Saved. Capture and validate this source context before exporting.");
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{configureSourcePolicy({{"operation","reset"},{"key",key}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});dialog->show();
}
}
