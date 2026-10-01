#include "InstallationSetup.h"
#include "StudioSession.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
void file(const QString& p,const QByteArray& bytes={}){QDir().mkpath(QFileInfo(p).absolutePath());QFile f(p);require(f.open(QIODevice::WriteOnly),"write fixture");f.write(bytes);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);QTemporaryDir temp;require(temp.isValid(),"workspace");app.setProperty("studioRoot",temp.path());
    try {
        QDir().mkpath(temp.filePath("local/settings"));QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.filePath("local/settings"));
        require(workspacePipe(temp.path())==workspacePipe(temp.path()+"/local/.."),"same workspace has different endpoints");
        require(workspacePipe(temp.path())!=workspacePipe(temp.filePath("Other")),"workspaces share an endpoint");
        InstallationSetup s{temp.filePath("Game/Data"),temp.filePath("CK"),temp.filePath("MO2"),"Second",false};
        file(temp.filePath("Game/Fallout4VR.exe"));file(temp.filePath("Game/Data/Fallout4.esm"));
        require(!checkInstallation(s).gameReady,"missing VR master accepted");file(temp.filePath("Game/Data/Fallout4_VR.esm"));
        file(temp.filePath("MO2/ModOrganizer.ini"),"[General]\nselected_profile=First\n[Settings]\nprofiles_directory=%BASE_DIR%/CustomProfiles\n");
        for(const auto* p:{"First","Second"}){file(temp.filePath(QString("MO2/CustomProfiles/%1/modlist.txt").arg(p)));file(temp.filePath(QString("MO2/CustomProfiles/%1/plugins.txt").arg(p)));}
        auto c=checkInstallation(s);require(c.gameReady&&!c.collisionReady&&c.profileReady,"independent capabilities");require(c.profiles==QStringList{"First","Second"},"profile enumeration");
        file(temp.filePath("CK/Tools/Elric/Elrich.exe"));require(!checkInstallation(s).collisionReady,"missing cooker settings accepted");file(temp.filePath("CK/Tools/Elric/Settings/PCMeshes.esf"));
        require(checkInstallation(s).collisionReady,"separate CK root");s.sameToolsRoot=true;require(!checkInstallation(s).collisionReady,"same folder ignored");s.sameToolsRoot=false;
        QString error;require(saveInstallationSetup(s,error),"save settings");auto restored=loadInstallationSetup();require(restored.gameRoot==temp.filePath("Game")&&restored.profile=="Second"&&!restored.sameToolsRoot,"round trip/normalized Data");
        auto bad=installationJson(s);bad["profile"]="../escape";require(!readInstallation(bad,restored,error),"profile traversal accepted");
        bad=installationJson(s);bad["version"]=1.5;require(!readInstallation(bad,restored,error),"fractional version accepted");
        s.profile="Missing";require(!checkInstallation(s).profileReady,"missing profile accepted");
        file(temp.filePath("local/settings/installation.json"),"broken settings");
        loadInstallationSetup(&error);require(!error.isEmpty(),"malformed settings silently ignored");
        file(temp.filePath("MO2/ModOrganizer.ini"),("[General]\ngamePath="+temp.filePath("DifferentGame").toUtf8()+"\n[Settings]\nprofiles_directory=%BASE_DIR%/CustomProfiles\n"));s.profile="First";require(!checkInstallation(s).profileReady,"different MO2 game accepted");
        QTextStream(stdout)<<"PASS separate game/CK paths, optional cooking, MO2 profiles, settings persistence and invalid input\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}
}
