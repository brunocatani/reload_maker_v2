#pragma once
#include <QJsonObject>
#include <QStringList>

namespace reloadmaker {
struct InstallationSetup {
    QString gameRoot,creationKitRoot,mo2Instance,profile;
    bool sameToolsRoot{true};
};
struct InstallationCheck {
    QString gameRoot,dataPath,gameKind,elric,elricSettings,profilePath;
    QStringList profiles,errors,notes;
    bool gameReady{},collisionReady{},profileReady{};
    QJsonObject json() const;
};
InstallationSetup loadInstallationSetup(QString* error=nullptr);
bool saveInstallationSetup(const InstallationSetup&,QString& error);
InstallationCheck checkInstallation(const InstallationSetup&);
QStringList mo2Profiles(const QString& instance,QString* directory=nullptr);
QJsonObject installationJson(const InstallationSetup&);
bool readInstallation(const QJsonObject&,InstallationSetup&,QString& error);
}
