#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QJsonArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {

void ReloadMakerController::showOperatingSetup() {
    if(auto* existing=_window->findChild<QDialog*>("OperatingSetupDialog")){existing->show();existing->raise();return;}
    QJsonObject state;try{state=operatingSetupState();}catch(const std::exception& e){setStatus(e.what(),true);return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("OperatingSetupDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Operating parts and movement");dialog->resize(860,820);
    const auto initial=state["settings"].toObject();const auto selection=authoringRecordKey();auto* outer=new QVBoxLayout(dialog);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);outer->addWidget(scroll,1);auto* content=new QWidget;scroll->setWidget(content);auto* layout=new QVBoxLayout(content);
    auto* info=new QLabel("This setup belongs to the selected assembly. Pick the operating part and every moving follower. Native curves retain independent barrel, pin and cartridge motion. The viewport remains available while this window is open.");info->setWordWrap(true);layout->addWidget(info);
    auto* form=new QFormLayout;layout->addLayout(form);
    const auto trackPicker=[&](const QString& label,const char* key){auto* box=new QComboBox;box->addItem("Select a native part…",-1);
        for(const auto& value:state["tracks"].toArray()){const auto t=value.toObject();if(!t["surface"].toBool())continue;box->addItem(t["name"].toString()+" · "+t["label"].toString(),t["index"].toInt());box->setItemData(box->count()-1,t["model"].toString()+"\n"+t["path"].toString(),Qt::ToolTipRole);}
        box->setCurrentIndex(box->findData(initial[key].toInt(-1)));form->addRow(label,box);
        connect(box,qOverload<int>(&QComboBox::activated),dialog,[this,box,selection,initial](int){const int track=_reloadRecipe?mapRecipeTrack(initial["trackAnchors"].toArray(),*_reloadRecipe,box->currentData().toInt()):-1;if(_reloadRecipe&&authoringRecordKey()==selection&&track>=0&&track<_reloadTrackNodes.size())_window->getGLView()->setCurrentIndex(_sceneNodes[_reloadTrackNodes[track]]);});return box;};
    auto* operating=trackPicker("Part the player pulls","operating");auto* bolt=trackPicker("Bolt / slide","bolt");auto* magazine=trackPicker("Full magazine carrier","magazine");
    auto* source=new QComboBox;source->addItem("Verified source behavior","preset");source->addItem("Current reload clip","reload");source->addItem("Another native clip","clip");source->addItem("Saved Move / Rotate movements","manual");source->setCurrentIndex(source->findData(initial["mode"].toString()));form->addRow("Movement source",source);
    auto* clip=new QComboBox;clip->setEditable(true);for(const auto& value:state["clips"].toArray()){const auto a=value.toObject();clip->addItem(a["path"].toString(),a["baseline"].toString());}clip->setEditText(initial["clip"].toString());form->addRow("Native movement clip",clip);
    auto* baseline=new QComboBox;baseline->setEditable(true);baseline->addItem(initial["baseline"].toString());form->addRow("Clip baseline",baseline);
    connect(clip,qOverload<int>(&QComboBox::activated),dialog,[clip,baseline](int){if(!clip->currentData().toString().isEmpty())baseline->setEditText(clip->currentData().toString());});
    auto* derive=new QCheckBox("Translate a static charging handle using the bolt's measured travel");derive->setChecked(initial["deriveHandle"].toBool());layout->addWidget(derive);
    const bool initiallyManual=initial["mode"]=="manual";
    auto* automatic=new QCheckBox("Detect the final open-to-closed window");automatic->setChecked(initiallyManual?initial["openFrame"].toInt(-1)<0:initial["openSeconds"].toDouble(-1)<0);layout->addWidget(automatic);
    auto* times=new QFormLayout;layout->addLayout(times);auto* open=new QDoubleSpinBox;auto* closed=new QDoubleSpinBox;
    for(auto* field:{open,closed}){field->setRange(0,120);field->setDecimals(6);field->setSingleStep(.01);field->setSuffix(" seconds");}
    open->setValue(initial["openSeconds"].toDouble(-1)>=0?initial["openSeconds"].toDouble():state["suggestedOpenSeconds"].toDouble());
    closed->setValue(initial["closedSeconds"].toDouble(-1)>=0?initial["closedSeconds"].toDouble():state["suggestedClosedSeconds"].toDouble());
    times->addRow("Open source time",open);times->addRow("Closed source time",closed);
    auto* manualOpen=new QSpinBox;auto* manualClosed=new QSpinBox;
    for(auto* field:{manualOpen,manualClosed})field->setRange(0,state["frames"].toInt()-1);
    manualOpen->setValue(initial["openFrame"].toInt(-1)>=0?initial["openFrame"].toInt():state["suggestedOpenFrame"].toInt());
    manualClosed->setValue(initial["closedFrame"].toInt(-1)>=0?initial["closedFrame"].toInt():state["suggestedClosedFrame"].toInt());
    times->addRow("Manual open sample",manualOpen);times->addRow("Manual closed sample",manualClosed);
    auto* timeHelp=new QLabel("Times select poses inside the native source clip. They do not set how quickly you must move your controller.");timeHelp->setWordWrap(true);layout->addWidget(timeHelp);
    if(!state["windowNote"].toString().isEmpty()){auto* note=new QLabel(state["windowNote"].toString());note->setWordWrap(true);layout->addWidget(note);}
    auto* duration=new QDoubleSpinBox;duration->setRange(.01,120);duration->setDecimals(3);duration->setValue(initial["duration"].toDouble(.2));duration->setSuffix(" seconds");times->addRow("Manual movement duration",duration);
    auto* eject=new QDoubleSpinBox;eject->setRange(-1,120);eject->setDecimals(3);eject->setSpecialValueText("Source marker / manual full opening");eject->setValue(initial["ejectSeconds"].toDouble(-1));times->addRow("Bolt-cycle ejection time",eject);
    auto* fire=new QComboBox;fire->addItem("Verified firing behavior","preset");fire->addItem("Native firing clip","native");fire->addItem("Replay measured bolt stroke at firing cadence","stroke");fire->setCurrentIndex(fire->findData(initial["fireMode"].toString()));times->addRow("Firing bolt movement",fire);
    layout->addWidget(new QLabel("OPERATING FOLLOWERS · selected parts keep their complete curves"));    auto* cycle=new QComboBox;cycle->addItem("Behavior template","preset");cycle->addItem("Closed bolt","closed-bolt");cycle->addItem("Open bolt","open-bolt");cycle->setCurrentIndex(std::max(0,cycle->findData(initial["cycleMode"].toString("preset"))));times->addRow("Firing action",cycle);
    auto* catchMode=new QComboBox;catchMode->addItem("Behavior template","preset");catchMode->addItem("Hold open","hold-open");catchMode->addItem("Close","close");catchMode->setCurrentIndex(std::max(0,catchMode->findData(initial["catchMode"].toString("preset"))));times->addRow("After last round",catchMode);
    auto* followers=new QListWidget;layout->addWidget(followers,1);
    for(const auto& value:state["tracks"].toArray()){const auto t=value.toObject();if(!t["surface"].toBool())continue;auto* row=new QListWidgetItem(t["name"].toString()+" · "+t["label"].toString(),followers);row->setData(Qt::UserRole,t["index"]);row->setFlags(row->flags()|Qt::ItemIsUserCheckable);row->setCheckState(initial["followers"].toArray().contains(t["index"])?Qt::Checked:Qt::Unchecked);row->setToolTip(t["model"].toString()+"\n"+t["path"].toString());}
    auto* movements=new QFormLayout;layout->addLayout(movements);QVector<QPair<int,QComboBox*>> assignments;
    for(const auto& value:state["tracks"].toArray()){const auto track=value.toObject();if(!track["surface"].toBool())continue;auto* choices=new QComboBox;choices->addItem("No saved movement",QString());
        for(const auto& m:_mechanisms)if(compatibleMechanismPart(m.anchor,_reloadRecipe->tracks[track["index"].toInt()].anchor))choices->addItem(m.name,m.id);
        if(choices->count()==1){delete choices;continue;}for(const auto& entry:initial["movements"].toArray())if(entry.toObject()["track"]==track["index"])choices->setCurrentIndex(choices->findData(entry.toObject()["id"].toString()));
        movements->addRow(track["name"].toString()+" movement",choices);assignments.append({track["index"].toInt(),choices});}
    auto* manualHelp=new QLabel("Create named movements with the existing Mechanism editor's Move/Rotate gizmos. Spring mechanisms use a closed-to-open path; manual bolts use the complete opening-and-closing sequence. Link rigid followers there, or assign independent movements here.");manualHelp->setWordWrap(true);layout->addWidget(manualHelp);
    auto* status=new QLabel(state["error"].toString());status->setWordWrap(true);outer->addWidget(status);
    const auto enable=[=]{const auto mode=source->currentData().toString();const bool manual=mode=="manual";clip->setEnabled(mode=="clip");baseline->setEnabled(mode=="clip");duration->setEnabled(manual);for(const auto& pair:assignments)pair.second->setEnabled(manual);manualHelp->setVisible(manual);open->setEnabled(!automatic->isChecked());closed->setEnabled(!automatic->isChecked());manualOpen->setEnabled(!automatic->isChecked());manualClosed->setEnabled(!automatic->isChecked());times->setRowVisible(open,!manual);times->setRowVisible(closed,!manual);times->setRowVisible(manualOpen,manual);times->setRowVisible(manualClosed,manual);timeHelp->setVisible(!manual);};
    // A different movement source has a different clock; do not carry an
    // explicit native-time window into a newly selected manual movement.
    connect(source,qOverload<int>(&QComboBox::currentIndexChanged),dialog,[automatic](int){automatic->setChecked(true);});
    connect(clip,&QComboBox::currentTextChanged,dialog,[automatic](const QString&){automatic->setChecked(true);});
    connect(source,qOverload<int>(&QComboBox::currentIndexChanged),dialog,enable);connect(automatic,&QCheckBox::toggled,dialog,enable);enable();
    auto* actions=new QHBoxLayout;outer->addLayout(actions);auto* save=new QPushButton("Save and preview in Operate");auto* reset=new QPushButton("Restore source defaults");auto* author=new QPushButton("Open movement editor");actions->addWidget(save);actions->addWidget(reset);actions->addWidget(author);
    connect(save,&QPushButton::clicked,dialog,[=,this]{try{auto value=initial;value["operating"]=operating->currentData().toInt();value["bolt"]=bolt->currentData().toInt();value["magazine"]=magazine->currentData().toInt();value["mode"]=source->currentData().toString();value["clip"]=clip->currentText();value["baseline"]=baseline->currentText();value["deriveHandle"]=derive->isChecked();value["fireMode"]=fire->currentData().toString();value["cycleMode"]=cycle->currentData().toString();value["catchMode"]=catchMode->currentData().toString();value["duration"]=duration->value();value["ejectSeconds"]=eject->value();
        const bool manual=source->currentData()=="manual";value["version"]=manual?1:2;
        value.remove("openFrame");value.remove("closedFrame");value.remove("openSeconds");value.remove("closedSeconds");
        if(manual){value["openFrame"]=automatic->isChecked()?-1:manualOpen->value();value["closedFrame"]=automatic->isChecked()?-1:manualClosed->value();}
        else{value["openSeconds"]=automatic->isChecked()?-1:open->value();value["closedSeconds"]=automatic->isChecked()?-1:closed->value();}
        QJsonArray list,moves;for(int i=0;i<followers->count();++i){const auto* row=followers->item(i);if(row->checkState()==Qt::Checked)list.append(row->data(Qt::UserRole).toInt());}for(const auto& pair:assignments)if(!pair.second->currentData().toString().isEmpty())moves.append(QJsonObject{{"track",pair.first},{"id",pair.second->currentData().toString()}});value["followers"]=list;value["movements"]=moves;
        configureOperatingSetup({{"operation","save"},{"settings",value}});setWorkspace(OperateWorkspace);status->setText(_simulationError.isEmpty()?"Saved for this assembly. Test the complete stroke in the viewport.":_simulationError);
    }catch(const std::exception& e){status->setText(e.what());}});
    connect(reset,&QPushButton::clicked,dialog,[=,this]{try{if(!_reloadRecipe||authoringRecordKey()!=selection)throw std::runtime_error("Source changed; reopen operating setup for the current assembly");configureOperatingSetup({{"operation","reset"}});dialog->close();}catch(const std::exception& e){status->setText(e.what());}});
    connect(author,&QPushButton::clicked,dialog,[=,this]{setWorkspace(GripsWorkspace);setHandEditorPage(HandEditorPage::Mechanism);dialog->close();});
    dialog->show();
}
}
