#include "ReloadMakerController.h"
#include "CollisionPanel.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QUuid>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showCollisionCooker() {
    QDialog dialog(_window);dialog.setWindowTitle("Cook prepared magazine model");dialog.resize(780,540);
    auto* layout=new QVBoxLayout(&dialog);
    auto* instruction=new QLabel("Select the prepared rigid magazine NIF in the same reference frame as this collision profile. It must contain only the intended full or empty magazine geometry and no existing physics. The current enabled hulls and physical settings will be cooked with your configured Creation Kit.");
    instruction->setWordWrap(true);layout->addWidget(instruction);
    auto* row=new QHBoxLayout;layout->addLayout(row);auto* source=new QLineEdit;row->addWidget(source,1);
    QJsonArray pieces;for(const auto& piece:_collisions->profile().sources)if(piece.worldIncluded)pieces.append(piece.key);
    if(_preparedPart["target"].toString()==_collisions->profile().target&&_preparedPart["configuration"].toString()==_collisions->profile().configuration&&
       _preparedPart["pieces"].toArray()==pieces&&_preparedPart["referencePose"].toObject()==_collisions->profile().referencePose)source->setText(_preparedPart["output"].toString());
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
    connect(cook,&QPushButton::clicked,&dialog,[&]{try{
        const auto folder=studioDataPath("local/asset-jobs/"+QUuid::createUuid().toString(QUuid::WithoutBraces));QDir().mkpath(folder);
        const auto hulls=folder+"/hulls.json";_collisions->command({{"operation","export-hulls"},{"path",hulls}});
        const auto reference=[](const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>64*1024*1024)throw std::runtime_error("Cannot read prepared asset input (maximum 64 MiB)");return QJsonObject{{"path",path},{"sha256",QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex())}};};
        const QJsonObject job{{"format","reload-maker-world-object-job"},{"version",1},{"geometry",reference(source->text())},{"collision",reference(hulls)}};
        const auto path=folder+"/job.json";const auto bytes=QJsonDocument(job).toJson();QSaveFile file(path);
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())throw std::runtime_error("Could not freeze the asset cook job");
        packCommand({{"operation","cook"},{"job",path},{"output",output->text()}});update();
    }catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{try{packCommand({{"operation","cancel"}});}catch(const std::exception& e){report->setPlainText(e.what());}});
    update();dialog.exec();
}
}
