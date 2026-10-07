// Saved versions of the open ESP project, and a magazine's finished world
// models. Loading, saving and deleting go through the same controller
// operations as MCP (areas/project/EspProject.cpp).
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showEspVersions() {
    if(!_esp){setStatus("Select a weapon to open its ESP project first.",true);return;}
    QDialog dialog(_window);dialog.setWindowTitle("Saved versions");dialog.resize(860,520);
    auto* layout=new QVBoxLayout(&dialog);
    auto* heading=new QLabel;heading->setWordWrap(true);layout->addWidget(heading);
    auto* list=new QTreeWidget;list->setColumnCount(4);list->setHeaderLabels({"Saved","Weapons","Exportable","Note"});list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(3,QHeaderView::Stretch);list->setColumnWidth(0,230);list->setColumnWidth(1,90);list->setColumnWidth(2,100);layout->addWidget(list,1);
    auto* status=new QLabel;status->setWordWrap(true);layout->addWidget(status);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* save=new QPushButton("Save");auto* load=new QPushButton("Load selected");auto* remove=new QPushButton("Delete selected");auto* close=new QPushButton("Close");
    for(auto* button:{save,load,remove,close})actions->addWidget(button);
    const auto populate=[&]{
        const auto state=espState();list->clear();
        const auto unsaved=state["unsavedWeapons"].toArray();QStringList names;for(const auto& n:unsaved)names.append(n.toString());
        heading->setText(QString("<b>%1</b> · %2").arg(state["plugin"].toString(),names.isEmpty()?QString("no unsaved changes"):"unsaved changes: "+names.join(", ")));
        const auto saves=state["saves"].toArray();
        for(int i=saves.size()-1;i>=0;--i){const auto info=saves[i].toObject();int ready=0;const auto weapons=info["weapons"].toArray();for(const auto& w:weapons)ready+=w.toObject()["exportable"].toBool();
            QString when=QDateTime::fromString(info["savedAt"].toString(),Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
            if(info["id"]==state["latest"])when+="  (latest)";
            if(info["id"]==state["base"])when+="  · open";
            auto* item=new QTreeWidgetItem(list,QStringList{when,QString::number(weapons.size()),QString::number(ready),info["note"].toString()});
            item->setData(0,Qt::UserRole,info["id"].toString());item->setToolTip(0,info["id"].toString());
            QStringList tip;for(const auto& w:weapons)tip.append(w.toObject()["name"].toString());item->setToolTip(1,tip.join("\n"));}
        save->setEnabled(!state["saving"].toBool());
    };
    connect(save,&QPushButton::clicked,&dialog,[&]{try{startEspSave({});status->setText("Saving… the new version appears here when it is written.");}catch(const std::exception& e){status->setText(e.what());}});
    connect(load,&QPushButton::clicked,&dialog,[&]{auto* item=list->currentItem();if(!item)return;
        try{loadEspVersion(item->data(0,Qt::UserRole).toString());populate();status->setText("Loaded. The next Save creates a new latest version; this one is kept.");}catch(const std::exception& e){status->setText(e.what());}});
    connect(remove,&QPushButton::clicked,&dialog,[&]{auto* item=list->currentItem();if(!item)return;
        if(QMessageBox::question(&dialog,"Delete saved version",QString("Delete the version saved %1 from %2? Export IDs and other versions are kept. This cannot be undone.").arg(item->text(0),_esp->plugin))!=QMessageBox::Yes)return;
        try{deleteEspVersion(item->data(0,Qt::UserRole).toString());status->setText("Deleting…");}catch(const std::exception& e){status->setText(e.what());}});
    connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    connect(this,&ReloadMakerController::packJobChanged,&dialog,[&]{const auto job=packCommand({});
        if(job["phase"]=="running")return;
        if(job["phase"]=="failed")status->setText(job["error"].toString());
        populate();});
    populate();dialog.exec();
}

void ReloadMakerController::showMagazineModels() {
    if(!currentWeapon()){setStatus("Select a weapon first.",true);return;}
    QDialog dialog(_window);dialog.setWindowTitle("Magazine world models");dialog.resize(760,300);
    auto* layout=new QVBoxLayout(&dialog);
    const auto magazine=simulationMagazineAnchor().ownerRecord;
    auto* intro=new QLabel(magazine.isEmpty()?QString("Prepare the magazine in Operate first."):
        QString("Finished full and empty world models for %1, cooked in Maker. Save copies them into the ESP project with this weapon's reload.").arg(magazine));
    intro->setWordWrap(true);layout->addWidget(intro);
    const auto current=_magazineModels.value(weaponRecordKey()+"|"+magazine.toLower());
    QLineEdit* fields[2]{};
    for(int i=0;i<2;++i){const char* presentation=i==0?"full":"empty";
        layout->addWidget(new QLabel(i==0?"Full model":"Empty model"));auto* row=new QHBoxLayout;layout->addLayout(row);
        fields[i]=new QLineEdit(current[QLatin1String(presentation)].toString());row->addWidget(fields[i],1);auto* browse=new QPushButton("Choose…");row->addWidget(browse);
        connect(browse,&QPushButton::clicked,&dialog,[&dialog,field=fields[i]]{const auto path=QFileDialog::getOpenFileName(&dialog,"Finished magazine model with cook receipt",studioDataPath("local/world-models"),"Cooked magazine (*.nif)");if(!path.isEmpty())field->setText(path);});}
    auto* status=new QLabel;status->setWordWrap(true);layout->addWidget(status);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* assign=new QPushButton("Assign");auto* clear=new QPushButton("Clear assignment");auto* close=new QPushButton("Close");
    for(auto* button:{assign,clear,close})actions->addWidget(button);
    assign->setEnabled(!magazine.isEmpty());clear->setEnabled(!current.isEmpty());
    connect(assign,&QPushButton::clicked,&dialog,[&]{try{assignMagazineModels({{"full",fields[0]->text()},{"empty",fields[1]->text()}});status->setText("Assigned. Save to add the models to the ESP project.");}catch(const std::exception& e){status->setText(e.what());}});
    connect(clear,&QPushButton::clicked,&dialog,[&]{try{assignMagazineModels({{"operation","clear"}});status->setText("Cleared. Saved versions keep the models they already hold.");}catch(const std::exception& e){status->setText(e.what());}});
    connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    dialog.exec();
}
}
