#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "data/nifvalue.h"
#include "model/nifmodel.h"
#include "gamemanager.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>

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
    parser.addOption({"library", "Open installed library in MO2 context"});
    parser.addOption({"no-restore", "Start without restoring the last project"});
    parser.addPositionalArgument("package", "Read-only mod folder or ZIP", "[package]");
    parser.process(app);
    // One document owns the local automation pipe. Reopening the launcher
    // should return to that editor instead of constructing a competing one.
    QLocalSocket existing;
    existing.connectToServer("ReloadMakerV2.local");
    if (existing.waitForConnected(250)) {
        existing.write("{\"method\":\"focus\"}\n");
        existing.waitForBytesWritten(500);
        existing.waitForReadyRead(1000);
        return 0;
    }
    const QString root = QDir(parser.isSet("project") ? parser.value("project") : QDir(app.applicationDirPath()).absoluteFilePath("../../..")).absolutePath();
    app.setProperty("studioRoot", root);
    for (const auto* folder : {"local/settings", "local/tmp", "workspace", "exports", "evidence"}) QDir().mkpath(QDir(root).filePath(folder));
    qputenv("TEMP", QDir(root).filePath("local/tmp").toUtf8());
    qputenv("TMP", QDir(root).filePath("local/tmp").toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir(root).filePath("local/settings"));
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
        const QString project=QSettings().value("projects/lastProject",QDir(root).filePath("workspace/current.project.json")).toString();
        if (!parser.isSet("no-restore") && parser.positionalArguments().isEmpty() && !parser.isSet("library") && QFileInfo::exists(project)) {
            try { controller->executeCommand({{"method","open_project"},{"params",QJsonObject{{"path",project}}}}); }
            catch (const std::exception& e) { qWarning("Project restore: %s",e.what()); }
        } else controller->openInitialPaths(parser.positionalArguments().value(0), parser.value("game-data"), parser.value("mo2-instance"), parser.isSet("library"));
    });
    return app.exec();
}
