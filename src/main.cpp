#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "data/nifvalue.h"
#include "model/nifmodel.h"
#include "gamemanager.h"
#include "InstallationSetup.h"
#include "StudioSession.h"
#include "UnattendedRun.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonDocument>
#include <QSaveFile>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUuid>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setOrganizationName("ReloadMakerV2");
    app.setApplicationName("ReloadMakerV2");
    app.setApplicationDisplayName("Reload Maker V2");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"project", "V2 workspace directory", "folder"});
    parser.addOption({"game-data", "Fallout 4 VR Data folder", "folder"});
    parser.addOption({"mo2-instance", "MO2 instance", "folder"});
    parser.addOption({"mo2-profile", "Expected MO2 profile; reject another active profile", "name"});
    parser.addOption({"source-receipt", "Write a verified MO2 source receipt and exit", "file"});
    parser.addOption({"handoff", "Verified source reconnect token", "token"});
    parser.addOption({"library", "Open installed library in MO2 context"});
    parser.addOption({"no-restore", "Start without restoring the last project"});
    parser.addOption({"unattended", "Automated run: open in the background and ignore the desktop's mouse, keyboard and focus"});
    parser.addOption({"resident-memory-mb", "Memory budget for sources kept loaded after switching away (default 16384)", "megabytes"});
    parser.addPositionalArgument("package", "Read-only mod folder or ZIP", "[package]");
    parser.process(app);
    if(parser.isSet("unattended")){app.setProperty("unattended",true);new reloadmaker::UnattendedRun(app);}
    app.setProperty("requestedMo2Profile",parser.value("mo2-profile"));
    if(parser.isSet("resident-memory-mb"))app.setProperty("residentMemoryMb",parser.value("resident-memory-mb").toLongLong());
    if(parser.isSet("source-receipt")) {
        const auto snapshot=reloadmaker::readLoadOrderSnapshot(parser.value("mo2-instance"),parser.value("game-data"),parser.value("mo2-profile"));
        const bool virtualized=reloadmaker::insideMo2VirtualFilesystem();
        QJsonObject value{{"ok",virtualized&&snapshot.valid()},{"virtualFilesystem",virtualized},{"instance",snapshot.instancePath},{"profile",snapshot.profileName},{"gameData",snapshot.dataPath},{"pluginCount",snapshot.plugins.size()},{"archiveCount",snapshot.archives.size()},{"fingerprint",QString::fromLatin1(snapshot.fingerprint.toHex())},{"error",snapshot.error}};
        QSaveFile file(parser.value("source-receipt"));const auto bytes=QJsonDocument(value).toJson();
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return 3;
        return virtualized&&snapshot.valid()?0:2;
    }
    QString selectedRoot=parser.value("project");
    if(selectedRoot.isEmpty()) {
        const auto bootstrap=QDir(app.applicationDirPath()).filePath("workspace.ini");
        QSettings choice(bootstrap,QSettings::IniFormat);selectedRoot=choice.value("workspace").toString();
        if(selectedRoot.isEmpty()||!QFileInfo(selectedRoot).isDir()) {
            selectedRoot=QFileDialog::getExistingDirectory(nullptr,"Select your Reload Maker workspace",app.applicationDirPath());
            if(selectedRoot.isEmpty())return 0;
            choice.setValue("workspace",selectedRoot);choice.sync();
            if(choice.status()!=QSettings::NoError){QMessageBox::warning(nullptr,"Workspace selection","The workspace can be used, but this installation cannot save its launcher preference. Use --project to select it on the next launch.");}
        }
    }
    const QString root = QDir(selectedRoot).absolutePath();
    app.setProperty("studioRoot", root);
    for (const auto* folder : {"local/settings", "local/tmp", "workspace", "exports", "evidence"}) if(!QDir().mkpath(QDir(root).filePath(folder))){QMessageBox::critical(nullptr,"Workspace unavailable","Choose a writable workspace with --project.");return 3;}
    const auto pipe=reloadmaker::workspacePipe(root);
    app.setProperty("commandPipe",pipe);
    app.setProperty("commandSession",QUuid::createUuid().toString(QUuid::WithoutBraces));
    // A workspace owns one document. Other workspaces must never share its
    // endpoint, revision sequence or current selection.
    QLocalSocket existing;
    existing.connectToServer(pipe);
    if(existing.waitForConnected(250)) {
        if(parser.isSet("handoff")) {
            const auto snapshot=reloadmaker::readLoadOrderSnapshot(parser.value("mo2-instance"),parser.value("game-data"),parser.value("mo2-profile"));
            if(!reloadmaker::insideMo2VirtualFilesystem()||!snapshot.valid())return 4;
            const auto request=QJsonDocument(QJsonObject{{"method","accept_source_session"},{"params",QJsonObject{{"token",parser.value("handoff")}}}}).toJson(QJsonDocument::Compact)+'\n';
            existing.write(request);existing.waitForBytesWritten(1000);
            if(!existing.waitForReadyRead(10000))return 4;
            const auto response=QJsonDocument::fromJson(existing.readAll()).object();
            if(!response["ok"].toBool()||!response["accepted"].toBool())return 4;
            if(existing.state()!=QLocalSocket::UnconnectedState&&!existing.waitForDisconnected(10000))return 4;
        } else {
            existing.write("{\"method\":\"focus\"}\n");
            existing.waitForBytesWritten(500);existing.waitForReadyRead(1000);
            return 0;
        }
    } else if(parser.isSet("handoff")) {
        // An expired/cancelled handoff must not open a competing document.
        return 4;
    }
    QSaveFile writable(QDir(root).filePath("local/tmp/workspace-write-check"));
    if(!writable.open(QIODevice::WriteOnly)){QMessageBox::critical(nullptr,"Workspace unavailable","The workspace is not writable. Choose another folder with --project.");return 3;}
    writable.cancelWriting();
    qputenv("TEMP", QDir(root).filePath("local/tmp").toUtf8());
    qputenv("TMP", QDir(root).filePath("local/tmp").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir(root).filePath("local/settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir(root).filePath("local/system-settings"));
    const auto setup=reloadmaker::loadInstallationSetup();
    const auto installation=reloadmaker::checkInstallation(setup);
    app.setProperty("launchMo2Instance",parser.isSet("mo2-instance")?parser.value("mo2-instance"):setup.mo2Instance);
    app.setProperty("launchGameData",parser.isSet("game-data")?parser.value("game-data"):installation.dataPath);
    if(installation.collisionReady)qputenv("ELRIC_EXE",installation.elric.toUtf8());
    QDir::setCurrent(app.applicationDirPath());
    qRegisterMetaType<NifValue>("NifValue");
    if (!NifModel::loadXML()) return 2;
    (void)Game::GameManager::get();
    auto* window = new StudioWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    reloadmaker::ReloadMakerController* controller = nullptr;
    try { controller = new reloadmaker::ReloadMakerController(window); }
    catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Reload Maker V2", QString::fromUtf8(e.what()));
        delete window;
        return 1;
    }
    window->show();
    QTimer::singleShot(0, controller, [controller, &parser, root] {
        if(parser.positionalArguments().isEmpty()&&!parser.isSet("library")&&QSettings().value("home/role").toString()=="player") {controller->showHome();return;}
        const QString project=QSettings().value("projects/lastProject",QDir(root).filePath("workspace/current.project.json")).toString();
        if (!parser.isSet("no-restore") && parser.positionalArguments().isEmpty() && !parser.isSet("library") && QFileInfo::exists(project)) {
            try { controller->executeCommand({{"method","open_project"},{"params",QJsonObject{{"path",project}}}}); }
            catch (const std::exception& e) { qWarning("Project restore: %s",e.what()); }
        } else controller->openInitialPaths(parser.positionalArguments().value(0), parser.value("game-data"), parser.value("mo2-instance"), parser.isSet("library"));
    });
    return app.exec();
}
