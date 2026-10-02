#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QDir>
#include <QProcess>
#include <QCoreApplication>
#include <QUuid>
#include <QTimer>

namespace reloadmaker {
QJsonObject ReloadMakerController::installationState() const {
    QString error;const auto s=loadInstallationSetup(&error);auto result=checkInstallation(s).json();result["settingsError"]=error;
    result["settings"]=installationJson(s);result["workspace"]=studioRoot();
    result["virtualFilesystem"]=insideMo2VirtualFilesystem();return result;
}
bool ReloadMakerController::configureInstallation(const QJsonObject& json) {
    InstallationSetup s;QString error;
    if(!readInstallation(json,s,error)||!saveInstallationSetup(s,error)){setStatus(error,true);return false;}
    const auto check=checkInstallation(s);
    // Setup changes future loads, never the active source under an open project.
    if(check.collisionReady)qputenv("ELRIC_EXE",check.elric.toUtf8());else qunsetenv("ELRIC_EXE");
    ++_revision;setStatus(check.gameReady?"Setup saved. Connect or open a source when ready.":"Setup draft saved. Select a valid game folder to load sources.");return true;
}
void ReloadMakerController::showSetup() {
    QDialog dialog(_window);dialog.setWindowTitle("Setup & tools");dialog.resize(720,570);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Choose your game and tools. Creation Kit tools are needed only for new cooked collision assets. AI clients connect externally through MCP.",&dialog);intro->setWordWrap(true);layout->addWidget(intro);
    auto* form=new QFormLayout;layout->addLayout(form);QString settingsError;auto current=loadInstallationSetup(&settingsError);
    const auto browse=[&](const QString& title,const QString& text){
        auto* row=new QWidget(&dialog);auto* h=new QHBoxLayout(row);h->setContentsMargins(0,0,0,0);
        auto* edit=new QLineEdit(text,row);auto* button=new QPushButton("Browse…",row);h->addWidget(edit,1);h->addWidget(button);
        connect(button,&QPushButton::clicked,&dialog,[&,edit,title]{const auto selected=QFileDialog::getExistingDirectory(&dialog,title,edit->text());if(!selected.isEmpty()){edit->setText(selected);emit edit->editingFinished();}});
        form->addRow(title,row);return edit;
    };
    auto* game=browse("Game folder",current.gameRoot);
    auto* same=new QCheckBox("Creation Kit is in the game folder",&dialog);same->setChecked(current.sameToolsRoot);form->addRow(QString(),same);
    auto* kit=browse("Creation Kit folder",current.creationKitRoot);kit->parentWidget()->setEnabled(!same->isChecked());
    auto* mo2=browse("MO2 instance folder",current.mo2Instance);
    auto* profiles=new QComboBox(&dialog);form->addRow("MO2 profile",profiles);
    auto* workspace=new QLabel(QDir::toNativeSeparators(studioRoot()),&dialog);workspace->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow("Workspace",workspace);
    auto* mcp=new QLineEdit('"'+QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("ReloadMakerMcp.cmd"))+"\" --workspace \""+QDir::toNativeSeparators(studioRoot())+'"',&dialog);
    mcp->setReadOnly(true);form->addRow("External AI · MCP command",mcp);
    auto* skills=new QLabel(QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("skills")),&dialog);skills->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow("Bundled AI instructions",skills);
    auto* report=new QPlainTextEdit(&dialog);report->setReadOnly(true);layout->addWidget(report,1);
    const auto value=[&]{return InstallationSetup{game->text(),kit->text(),mo2->text(),profiles->currentText(),same->isChecked()};};
    const auto refresh=[&]{const auto c=checkInstallation(value());QStringList text;
        text<<QString("Game: %1").arg(c.gameReady?c.gameKind:"Needs configuration")<<QString("New collision export: %1").arg(c.collisionReady?"Elric detected":"Elric not configured")<<QString("MO2 profile: %1").arg(c.profileReady?profiles->currentText():"Not connected");
        if(!settingsError.isEmpty())text<<settingsError;
        if(c.collisionReady)text<<c.elric;
        text<<c.errors<<c.notes;report->setPlainText(text.join('\n'));
    };
    const auto refreshProfiles=[&]{const auto previous=profiles->currentText().isEmpty()?current.profile:profiles->currentText();const QSignalBlocker block(profiles);profiles->clear();profiles->addItems(mo2Profiles(mo2->text()));const auto index=profiles->findText(previous);if(index>=0)profiles->setCurrentIndex(index);refresh();};
    connect(game,&QLineEdit::editingFinished,&dialog,refresh);connect(kit,&QLineEdit::editingFinished,&dialog,refresh);
    connect(mo2,&QLineEdit::editingFinished,&dialog,refreshProfiles);connect(profiles,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[&]{refresh();});
    connect(same,&QCheckBox::toggled,&dialog,[&](bool enabled){kit->parentWidget()->setEnabled(!enabled);refresh();});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);
    auto* connectSource=buttons->addButton("Save and connect MO2",QDialogButtonBox::ActionRole);
    auto* packs=buttons->addButton("Build installed packs…",QDialogButtonBox::ActionRole);
    connect(packs,&QPushButton::clicked,&dialog,[&]{dialog.accept();QTimer::singleShot(0,this,&ReloadMakerController::showPackBuilder);});
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(configureInstallation(installationJson(value())))dialog.accept();else report->setPlainText(_lastError);});
    connect(connectSource,&QPushButton::clicked,&dialog,[&]{const auto c=checkInstallation(value());if(!c.gameReady||!c.profileReady){refresh();return;}
        if(!configureInstallation(installationJson(value()))||!connectInstalledProfile()){report->setPlainText(_lastError);return;}dialog.accept();});
    refreshProfiles();dialog.exec();
}
bool ReloadMakerController::connectInstalledProfile() {
    const auto s=loadInstallationSetup();const auto c=checkInstallation(s);
    if(!c.gameReady||!c.profileReady){setStatus(c.errors.join('\n'),true);return false;}
    if(!insideMo2VirtualFilesystem()||!_launchSnapshot.valid()||_launchSnapshot.profileName!=s.profile||QDir::cleanPath(_launchSnapshot.instancePath).compare(QDir::cleanPath(s.mo2Instance),Qt::CaseInsensitive)!=0||
       QDir::cleanPath(_launchSnapshot.dataPath).compare(QDir::cleanPath(c.dataPath),Qt::CaseInsensitive)!=0||_launchSnapshot.fingerprint!=profileFingerprint(s.mo2Instance,c.profilePath)){
        if(!_profileHandoff.isEmpty()){setStatus("An MO2 connection is already starting.",true);return false;}
        if(_loadThread||_reloadCapturing||!saveSession()){setStatus("Finish the current load/capture and save the project before connecting.",true);return false;}
        const auto executable=QDir(s.mo2Instance).filePath("ModOrganizer.exe");
        if(!QFileInfo::exists(executable)){setStatus("ModOrganizer.exe is missing from the selected instance.",true);return false;}
        const auto token=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto quote=[](const QString& value){auto v=value;v.replace('"',"\\\"");return '"'+v+'"';};
        const QString arguments="--project "+quote(studioRoot())+" --mo2-instance "+quote(s.mo2Instance)+" --mo2-profile "+quote(s.profile)+" --game-data "+quote(c.dataPath)+" --handoff "+token+" --library";
        auto* process=new QProcess(this);process->setProgram(executable);process->setArguments({"--profile",s.profile,"run","--arguments",arguments,QCoreApplication::applicationFilePath()});process->setWorkingDirectory(s.mo2Instance);
        _profileHandoff=token;
        connect(process,&QProcess::errorOccurred,this,[this,process,token]{if(_profileHandoff==token){_profileHandoff.clear();setStatus("MO2 connection failed: "+process->errorString()+". Your project is still open.",true);}});
        connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,token](int code,QProcess::ExitStatus status){if(_profileHandoff==token&&(code!=0||status!=QProcess::NormalExit)){_profileHandoff.clear();setStatus("MO2 rejected the connection. Your project is still open.",true);}process->deleteLater();});
        process->start();
        QTimer::singleShot(20000,this,[this,token]{if(_profileHandoff==token){_profileHandoff.clear();setStatus("MO2 did not connect a verified editor session. Your current project was retained.",true);}});
        setStatus("Opening the selected MO2 profile. This project will close only after the new source session verifies its virtual files.");return true;
    }
    _mo2Instance=s.mo2Instance;_requestedMo2Profile=s.profile;_gameDataText=c.dataPath;startLibraryLoad();return true;
}
}
