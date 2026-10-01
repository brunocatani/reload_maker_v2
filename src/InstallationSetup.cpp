#include "InstallationSetup.h"
#include "StudioPaths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>

namespace reloadmaker {
namespace {
QString iniString(const QVariant& value){return value.metaType().id()==QMetaType::QByteArray?QString::fromUtf8(value.toByteArray()):value.toString();}
QString path(const QString& value){return value.trimmed().isEmpty()?QString():QDir::cleanPath(QFileInfo(value.trimmed()).absoluteFilePath());}
QString gameRoot(const QString& value){const QFileInfo info(path(value));return info.fileName().compare("Data",Qt::CaseInsensitive)==0?info.dir().absolutePath():info.absoluteFilePath();}
bool leaf(const QString& s){return !s.isEmpty()&&s!="."&&s!=".."&&!s.contains('/')&&!s.contains('\\')&&!s.contains(':');}
}
QJsonObject installationJson(const InstallationSetup& s){return {{"version",1},{"gameRoot",s.gameRoot},{"creationKitRoot",s.creationKitRoot},{"sameToolsRoot",s.sameToolsRoot},{"mo2Instance",s.mo2Instance},{"profile",s.profile}};}
bool readInstallation(const QJsonObject& j,InstallationSetup& s,QString& error){
    error.clear();
    if(j["version"].toDouble()!=1||!j["sameToolsRoot"].isBool()){error="Unsupported installation settings.";return false;}
    for(const auto* k:{"gameRoot","creationKitRoot","mo2Instance","profile"})if(!j[k].isString()||j[k].toString().size()>32768){error="Invalid installation field.";return false;}
    if(!j["profile"].toString().isEmpty()&&!leaf(j["profile"].toString())){error="Select a profile folder name, not a path.";return false;}
    s={j["gameRoot"].toString().isEmpty()?QString():gameRoot(j["gameRoot"].toString()),path(j["creationKitRoot"].toString()),path(j["mo2Instance"].toString()),j["profile"].toString(),j["sameToolsRoot"].toBool()};return true;
}
InstallationSetup loadInstallationSetup(QString* problem){
    if(problem)problem->clear();
    QFile file(studioDataPath("local/settings/installation.json"));
    if(file.exists()) {
        if(!file.open(QIODevice::ReadOnly)||file.size()>128*1024){if(problem)*problem="Cannot read installation settings: "+file.fileName();return {};}
        InstallationSetup s;QString error;if(readInstallation(QJsonDocument::fromJson(file.readAll()).object(),s,error))return s;
        if(problem)*problem=error+" File: "+file.fileName()+". The original has been preserved.";
        return {};
    }
    // Read the old selection without rewriting it or moving the user's projects.
    QSettings old;InstallationSetup s;
    const auto game=old.value("paths/gameData").toString();if(!game.isEmpty())s.gameRoot=gameRoot(game);
    s.mo2Instance=old.value("paths/mo2Instance").toString();
    if(!s.mo2Instance.isEmpty()){QSettings ini(QDir(s.mo2Instance).filePath("ModOrganizer.ini"),QSettings::IniFormat);s.profile=iniString(ini.value("General/selected_profile",ini.value("selected_profile")));}
    return s;
}
bool saveInstallationSetup(const InstallationSetup& s,QString& error){
    InstallationSetup normalized;if(!readInstallation(installationJson(s),normalized,error))return false;
    QSaveFile file(studioDataPath("local/settings/installation.json"));file.setDirectWriteFallback(false);
    const auto bytes=QJsonDocument(installationJson(normalized)).toJson();
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error="Cannot save installation settings: "+file.errorString();return false;}return true;
}
QStringList mo2Profiles(const QString& instance,QString* directory){
    if(!QFileInfo::exists(QDir(instance).filePath("ModOrganizer.ini")))return {};
    QSettings ini(QDir(instance).filePath("ModOrganizer.ini"),QSettings::IniFormat);
    auto dir=iniString(ini.value("Settings/profiles_directory"));
    if(dir.isEmpty())dir=QDir(instance).filePath("profiles");
    dir.replace("%BASE_DIR%",QDir(instance).absolutePath(),Qt::CaseInsensitive);
    if(QDir::isRelativePath(dir))dir=QDir(instance).filePath(dir);
    dir=QDir::cleanPath(dir);if(directory)*directory=dir;
    QStringList profiles;for(const auto& name:QDir(dir).entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name|QDir::IgnoreCase))
        if(QFileInfo::exists(QDir(dir).filePath(name+"/modlist.txt"))&&QFileInfo::exists(QDir(dir).filePath(name+"/plugins.txt")))profiles.append(name);
    return profiles;
}
InstallationCheck checkInstallation(const InstallationSetup& s){
    InstallationCheck c;
    if(s.gameRoot.isEmpty())c.errors<<"Select the Fallout 4 or Fallout 4 VR installation folder.";
    else {
        c.gameRoot=gameRoot(s.gameRoot);c.dataPath=QDir(c.gameRoot).filePath("Data");
        const QDir game(c.gameRoot),data(c.dataPath);
        const bool vr=QFileInfo::exists(game.filePath("Fallout4VR.exe")),flat=QFileInfo::exists(game.filePath("Fallout4.exe"));
        c.gameKind=vr?"Fallout4VR":flat?"Fallout4":"unknown";
        c.gameReady=(vr||flat)&&QFileInfo::exists(data.filePath("Fallout4.esm"))&&(!vr||QFileInfo::exists(data.filePath("Fallout4_VR.esm")));
        if(!c.gameReady)c.errors<<"The selected game folder needs its executable, Data folder and base master files.";
        if(flat)c.notes<<"FO4 assets can be authored here; the current DIAMOND runtime targets Fallout 4 VR.";
    }
    const auto tools=s.sameToolsRoot?c.gameRoot:s.creationKitRoot;
    if(!tools.isEmpty())for(const auto& base:{QDir(tools).filePath("Tools/Elric"),QDir(tools).filePath("Elric"),tools}) {
        const QDir dir(base);const auto exe=dir.filePath("Elrich.exe"),settings=dir.filePath("Settings/PCMeshes.esf");
        if(QFileInfo::exists(exe)&&QFileInfo::exists(settings)){c.elric=exe;c.elricSettings=settings;c.collisionReady=true;break;}
    }
    if(!c.collisionReady)c.notes<<"New collision cooking needs Creation Kit Elric and PCMeshes.esf. Elric is not required for authoring or ESP serialization.";
    if(!s.mo2Instance.isEmpty()){
        QString dir;c.profiles=mo2Profiles(s.mo2Instance,&dir);
        c.profileReady=c.profiles.contains(s.profile);
        if(c.profileReady)c.profilePath=QDir(dir).filePath(s.profile);
        else c.errors<<"Select an existing MO2 profile with modlist.txt and plugins.txt.";
        QSettings ini(QDir(s.mo2Instance).filePath("ModOrganizer.ini"),QSettings::IniFormat);
        const auto selectedGame=iniString(ini.value("General/gamePath",ini.value("gamePath")));
        if(!selectedGame.isEmpty()&&c.gameReady&&gameRoot(selectedGame).compare(c.gameRoot,Qt::CaseInsensitive)!=0){c.profileReady=false;c.errors<<"The MO2 instance points to a different game folder.";}
        c.notes<<"Profile metadata is not virtual-file attachment. Connect through MO2 to load its winning assets.";
    }
    return c;
}
QJsonObject InstallationCheck::json()const{return {{"gameReady",gameReady},{"collisionReady",collisionReady},{"profileReady",profileReady},{"gameRoot",gameRoot},{"dataPath",dataPath},{"gameKind",gameKind},{"elric",elric},{"elricSettings",elricSettings},{"profilePath",profilePath},{"profiles",QJsonArray::fromStringList(profiles)},{"errors",QJsonArray::fromStringList(errors)},{"notes",QJsonArray::fromStringList(notes)}};}
}
