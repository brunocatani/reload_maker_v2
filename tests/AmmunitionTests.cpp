#include "WeaponAmmunition.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <stdexcept>
using namespace reloadmaker;
namespace {
void check(bool ok,const QString& error){if(!ok)throw std::runtime_error(error.toStdString());}
void put(QByteArray& b,int p,std::uint32_t v){qToLittleEndian(v,reinterpret_cast<uchar*>(b.data()+p));}
QByteArray sub(const char* type,const QByteArray& b){QByteArray s(type,4);auto n=qToLittleEndian(std::uint16_t(b.size()));s.append(reinterpret_cast<const char*>(&n),2);return s+b;}
QByteArray record(const char* type,std::uint32_t id,const QByteArray& b){QByteArray s(24,'\0');s.replace(qsizetype(0),4,type,4);put(s,4,b.size());put(s,12,id);return s+b;}
void parser() {
    QTemporaryDir dir;QByteArray dnam(55,'\0');put(dnam,0,0xabc);dnam[52]=24;
    QByteArray data(28+3*24,'\0');put(data,4,3);data.replace(10,4,"WEAP",4);
    put(data,28+8,12);put(data,28+12,33);
    put(data,52,4);put(data,52+8,61);put(data,52+12,0xdef);
    put(data,76,4);put(data,76+4,1);put(data,76+8,31);put(data,76+12,0x222);
    QFile file(dir.filePath("fixture.esp"));check(file.open(QIODevice::WriteOnly),"fixture write");
    file.write(record("TES4",0,{})+record("WEAP",1,sub("DNAM",dnam))+record("WEAP",2,sub("DNAM",dnam.left(53)))+
        record("OMOD",3,sub("DATA",data))+record("AMMO",0xabc,sub("EDID","AmmoFixture"))+record("KYWD",0x222,sub("EDID","AnimsGripPistol")));file.close();
    auto p=EspReader::read(file.fileName());check(p.parsed&&p.weapons.size()==2,"Parser failed");
    check(p.weapons[0].ammoDataKnown&&p.weapons[0].ammo==0xabc&&p.weapons[0].ammoCapacity==24,"WEAP ammo fields wrong");
    check(!p.weapons[1].ammoDataKnown,"Truncated DNAM accepted");
    const auto& m=p.objectMods[0];check(m.ammoProperties.size()==2&&m.ammoProperties[0].value1==33&&m.ammoProperties[1].value1==0xdef,"OMOD ammo fields wrong");
    check(m.removedKeywords==QVector<std::uint32_t>{0x222},"Keyword removal lost");
    auto catalog=EspReader::read(file.fileName(),{},true);
    check(catalog.parsed&&catalog.weapons.isEmpty()&&catalog.objectMods.isEmpty()&&catalog.ammoNames.value(0xabc)=="AmmoFixture"&&catalog.keywordNames.value(0x222)=="AnimsGripPistol","Reference catalog filter wrong");
}
void resolution() {
    PluginContent p;p.fileName="Local.esp";p.masters={"Base.esm"};p.ammoNames[0xabc]="Base ammo";p.ammoNames[0x01000def]="New ammo";
    WeaponRecord w;w.formId=0x01000001;w.ammo=0xabc;w.ammoCapacity=8;w.ammoDataKnown=true;w.keywords={99};w.attachParentSlots={10};w.defaultObjectMods={1};
    ObjectModRecord m;m.formId=1;m.attachPoint=10;m.weaponModification=true;m.targetKeywords={99};m.ammoProperties={{12,0,0,17,0,0},{61,4,0,0x01000def,0,0}};m.addedKeywords={100};
    p.keywordNames[100]="AnimsGripPistol";p.objectMods={m};WeaponConfiguration c;
    const auto resolve=[&]{c.initialize(p,w);return resolveWeaponAmmunition(p,w,c);};
    auto a=resolve();check(a.valid()&&a.capacity==17&&a.ammoName=="New ammo"&&a.ammoRecord=="Local.esp:000def"&&a.pistol,"Selected overrides failed");
    p.objectMods[0].ammoProperties[1].value2=605;
    a=resolve();check(a.valid()&&a.ammoRecord=="Local.esp:000def"&&a.capacity==17,"FormID,Int companion changed ammo selection");
    p.objectMods[0].ammoProperties[0].value2=18;check(!resolve().valid(),"Ranged capacity accepted");
    p.objectMods[0].ammoProperties[0].value2=0;
    check(recordIdentity(p,0xabc)=="Base.esm:000abc","Master identity wrong");
    p.objectMods[0].ammoProperties[0].function=2;p.objectMods[0].ammoProperties[0].value1=2;
    check(resolve().capacity==10,"Additive capacity failed");
    p.objectMods[0].ammoProperties[0].function=1;check(!resolve().valid(),"Unverified multiply operation accepted");
    p.objectMods[0].ammoProperties[0].function=0;p.objectMods[0].ammoProperties[0].stepBits=1;check(!resolve().valid(),"Rank-scaled property accepted");
    p.objectMods[0].ammoProperties[0].stepBits=0;p.objectMods[0].metadataWarning="Includes cannot be resolved";check(!resolve().valid(),"Partial metadata accepted");
    p.objectMods[0].metadataWarning.clear();p.objectMods[0].ammoProperties[0].value1=0;check(!resolve().valid(),"Zero capacity accepted");
    p.objectMods[0]=m;auto other=m;other.formId=2;other.attachPoint=20;other.ammoProperties[0].value1=33;p.objectMods.append(other);
    w.defaultObjectMods.append(2);check(resolve().capacity==17,"Unavailable part affected ammo");
    w.attachParentSlots.append(20);check(!resolve().valid(),"Conflicting capacity overrides accepted");
    p.objectMods[1].ammoProperties[0].function=2;check(!resolve().valid(),"Unknown set/add ordering accepted");
    p.objectMods[1].ammoProperties={{61,4,0,0xabc,0,0}};check(!resolve().valid(),"Conflicting ammo types accepted");
    p.objectMods[1].ammoProperties.clear();p.objectMods[1].addedKeywords.clear();p.objectMods[0].addedKeywords.clear();w.keywords.append(100);p.objectMods[0].removedKeywords={100};check(!resolve().pistol,"Removed grip keyword still locks");
    p.objectMods[1].addedKeywords={100};check(!resolve().valid(),"Conflicting grip edits depended on display sorting");
    w.ammoDataKnown=false;check(!resolve().valid(),"Unknown base data accepted");
}
void glock(const QString& path) {
    const auto p=EspReader::read(path);check(p.parsed,"Glock parsing failed");const WeaponRecord* w=nullptr;
    for(const auto& weapon:p.weapons)if(weapon.editorId=="WPNGlock19x")w=&weapon;
    check(w&&w->ammoDataKnown&&w->ammoCapacity==8&&w->ammo==0x1f276,"Actual Glock base ammo mismatch");
    WeaponConfiguration c;c.initialize(p,*w);
    const QHash<QString,QString> names{{"fallout4.esm:01f276","Ammo10mm"},{"fallout4.esm:01f948","AnimsGripPistol"}};
    for(auto pair:{std::pair{0x01000fa4u,17},std::pair{0x01000fa5u,22},std::pair{0x0100176fu,24},std::pair{0x01001770u,29},std::pair{0x01001779u,33},std::pair{0x0100177au,38},std::pair{0x01002e6cu,50}}) {
        const auto* mod=c.mod(pair.first);check(mod,"Actual Glock magazine absent");c.select(configurationSlotKey(*mod),pair.first);
        auto a=resolveWeaponAmmunition(p,*w,c,names);check(a.valid(),a.error);
        check(a.capacity==pair.second&&a.ammoName=="Ammo10mm"&&a.pistol,"Actual Glock capacity/type/grip mismatch");
        QTextStream(stdout)<<mod->editorId<<": "<<a.capacity<<" rounds, "<<a.ammoName<<'\n';
    }
    int receivers=0,automatic=0;
    for(const auto& mod:p.objectMods)if(mod.editorId.startsWith("mod_Glock19x_Receiver_")) {
        c.select(configurationSlotKey(mod),mod.formId);const auto a=resolveWeaponAmmunition(p,*w,c,names);
        check(a.valid(),a.error);const bool expected=mod.editorId.contains("Automatic1");
        check(a.automatic==expected,"Actual receiver firing property mismatch");++receivers;if(a.automatic)++automatic;
    }
    check(receivers==28&&automatic==10,"Receiver coverage omitted a Glock configuration");
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{parser();resolution();check(argc==2,"Pass the original Glock ESP");glock(app.arguments()[1]);QTextStream(stdout)<<"PASS ammunition parsing, property resolution, fail-closed unsupported modifiers, and actual configured Glock magazines\n";return 0;}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
