#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showCollisionCooker() {
    QDialog dialog(_window);dialog.setWindowTitle("Cook prepared magazine model");dialog.resize(780,540);
    auto* layout=new QVBoxLayout(&dialog);
    auto* instruction=new QLabel("Select the prepared rigid magazine NIF in the same reference frame as this collision profile. It must contain only the intended full or empty magazine geometry and no existing physics. The current enabled hulls and physical settings will be cooked with your configured Creation Kit.");
    instruction->setWordWrap(true);layout->addWidget(instruction);
    auto* row=new QHBoxLayout;layout->addLayout(row);auto* source=new QLineEdit;row->addWidget(source,1);
    if(const auto prepared=preparedPartForCollision();!prepared.isEmpty())source->setText(prepared);
    auto* browse=new QPushButton("Prepared render NIF…");row->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto path=QFileDialog::getOpenFileName(&dialog,"Prepared magazine render model",studioDataPath("exports"),"Fallout 4 model (*.nif)");if(!path.isEmpty())source->setText(path);});
    layout->addWidget(new QLabel("New finished world model"));
    const auto destination=source->text().isEmpty()?studioDataPath("exports/objects/"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")+"/Magazine.nif"):QFileInfo(source->text()).absolutePath()+"/cooked/"+QFileInfo(source->text()).fileName();
    auto* output=new QLineEdit(destination);layout->addWidget(output);
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    auto* buttons=new QHBoxLayout;layout->addLayout(buttons);auto* cook=new QPushButton("Cook and validate model");auto* cancel=new QPushButton("Cancel job");buttons->addWidget(cook);buttons->addWidget(cancel);
    const auto update=[&]{const auto state=packCommand({});const auto phase=state["phase"].toString();const bool running=phase=="running"||phase=="cancelling";cook->setEnabled(!running);cancel->setEnabled(running);
        if(state["operation"]!="cook"){report->setPlainText("The native tools will verify rigid geometry, material bindings and final Fallout 4 collision. Your prepared source and prior exports are preserved.");return;}
        if(running)report->setPlainText(phase=="cancelling"?"Cancellation requested; waiting for the current native stage to finish…":"Building convex collision and cooking the world model…");
        else if(phase=="failed")report->setPlainText(state["error"].toString());
        else{const auto result=state["result"].toObject();report->setPlainText(QString("Last completed cook — finished model validated.\n%1 hulls · %2 support points\n\n%3\n\nGeometry, transforms and material references were preserved. Use this finished model in its full or empty magazine object definition; verify physical behavior in game.").arg(result["hulls"].toInt()).arg(result["supportPoints"].toInt()).arg(result["output"].toString()));}
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    connect(cook,&QPushButton::clicked,&dialog,[&]{try{startCollisionCook(source->text(),output->text());update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{try{packCommand({{"operation","cancel"}});}catch(const std::exception& e){report->setPlainText(e.what());}});
    update();dialog.exec();
}
}
