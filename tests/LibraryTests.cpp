#include "LoadOrderLibrary.h"
#include "LibraryResources.h"
#include "WeaponConfiguration.h"
#include "areas/sources/SourceBrowserRows.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <stdexcept>

using namespace reloadmaker;
namespace {
    void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
    void write(const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "fixture create failed");
        require(file.write(bytes) == bytes.size(), "fixture write failed");
    }
    QByteArray record(const char* type, std::uint32_t id, std::uint32_t flags = 0, const QByteArray& payload = {}) {
        QByteArray result(24, '\0'); result.replace(qsizetype(0), 4, type, 4);
        qToLittleEndian(std::uint32_t(payload.size()), reinterpret_cast<uchar*>(result.data() + 4));
        qToLittleEndian(flags, reinterpret_cast<uchar*>(result.data() + 8));
        qToLittleEndian(id, reinterpret_cast<uchar*>(result.data() + 12));
        return result + payload;
    }
    void mergeTests() {
        PluginContent base; base.fileName = "Base.esm"; base.parsed = true;
        base.keywordNames.insert(0x123, "attach"); base.keywordNames.insert(0x124, "weapon");
        base.keywordNames.insert(0x125,"remapNode_Sights_P-Slide");base.nodeRemaps.insert(0x125,{"Sights","P-Slide"});
        base.ammoNames.insert(0x700,"Base ammunition");
        WeaponRecord weapon; weapon.formId = 0x456; weapon.name = "Base weapon";
        weapon.keywords = {0x124}; weapon.attachParentSlots = {0x123}; weapon.defaultObjectMods = {0x500};
        weapon.ammo=0x700;weapon.ammoCapacity=8;weapon.ammoDataKnown=true;
        base.weapons.append(weapon);
        ObjectModRecord mod; mod.formId = 0x500; mod.attachPoint = 0x123; mod.weaponModification = true;
        mod.targetKeywords = {0x124}; mod.modelPath = "base.model";
        base.objectMods.append(mod);
        AnimationSubgraphRecord graph; graph.formId = 0x600;
        graph.entries.append({{0x124}, "graph.hkx", {"animations/base"}, 0x10000});
        base.animationSubgraphs.append(graph);

        PluginContent extra; extra.fileName = "Unrelated.esl"; extra.parsed = true;
        auto unrelated = weapon; unrelated.name = "Different weapon, same local ID"; unrelated.defaultObjectMods.clear();
        extra.weapons.append(unrelated);
        PluginContent patch; patch.fileName = "Patch.esp"; patch.masters = {"Unrelated.esl", "BASE.esm"}; patch.parsed = true;
        mod.formId = 0x01000500; mod.attachPoint = 0x01000123; mod.targetKeywords = {0x01000124}; mod.modelPath = "winner.model";
        mod.ammoProperties={{61,4,0,0x02000800,0,0}};mod.removedKeywords={0x01000124};
        patch.ammoNames.insert(0x02000800,"Patched ammunition");
        patch.ammoNames.insert(0x01000700,"Winning base ammunition");
        patch.objectMods.append(mod); // No WEAP records in this attachment patch.
        graph.formId = 0x01000600; graph.entries[0].requiredKeywords = {0x01000124}; graph.entries[0].animationSearchPaths = {"animations/replacement"};
        patch.animationSubgraphs.append(graph);
        QString error;
        auto merged = mergeLoadOrder({base, extra, patch}, error);
        require(error.isEmpty() && merged.parsed, "valid master-aware merge failed");
        require(merged.weapons.size() == 2 && merged.weapons[0].formId != merged.weapons[1].formId, "local IDs collided across plugins");
        const WeaponRecord* target = nullptr;
        for (const auto& entry : merged.weapons) { if (entry.name == "Base weapon") { target = &entry; } }
        require(target, "base weapon lost");
        require(merged.nodeRemaps.size()==1&&merged.nodeRemaps.cbegin()->node=="Sights","Remap keyword identity lost during merge");
        WeaponConfiguration config; config.initialize(merged, *target);
        require(config.activeMods().size() == 1, "cross-plugin default not resolved");
        require(config.mod(config.activeMods().first())->modelPath == "winner.model", "patch-only ESP did not override base OMOD");
        require(merged.ammoNames.value(target->ammo)=="Winning base ammunition"&&target->ammoCapacity==8&&target->ammoDataKnown,"Weapon ammo link/winner was not remapped");
        const auto* winningMod=config.mod(config.activeMods().first());
        require(merged.ammoNames.value(winningMod->ammoProperties[0].value1)=="Patched ammunition","Ammo override link lost its plugin master mapping");
        require(merged.keywordNames.value(winningMod->removedKeywords[0])=="weapon","Removed keyword was not remapped");
        require(merged.resolveFirstPersonSubgraph(target->keywords)->animationSearchPaths.first() == "animations/replacement", "RACE patch lost");
        patch.animationSubgraphs[0].entries.clear();
        auto emptyOverride = mergeLoadOrder({base, extra, patch}, error);
        require(!emptyOverride.resolveFirstPersonSubgraph(target->keywords), "empty RACE resurrected earlier graph");
        patch.deletedRecords = {0x01000456,0x01000700};
        patch.keywordNames.insert(0x01000125,"No longer a function");
        patch.ammoNames.remove(0x01000700);
        auto deleted = mergeLoadOrder({base, extra, patch}, error);
        require(deleted.weapons.size() == 1 && deleted.weapons.first().name == unrelated.name, "deleted WEAP reappeared or deleted unrelated local ID");
        require(deleted.nodeRemaps.isEmpty(),"Keyword override resurrected an obsolete remap function");
        require(!deleted.ammoNames.values().contains("Winning base ammunition")&&!deleted.ammoNames.values().contains("Base ammunition"),"Deleted ammo label resurrected an earlier winner");
        auto invalid = mergeLoadOrder({patch}, error);
        require(!error.isEmpty() && !invalid.parsed, "missing master accepted");
        patch.keywordNames.insert(0x02000801, "SelfKeyword");
        patch.objectMods[0].attachPoint = 0x03000801;
        auto selfSlots = mergeLoadOrder({base, extra, patch}, error);
        require(error.isEmpty() && selfSlots.keywordNames.value(selfSlots.objectMods.first().attachPoint) == "SelfKeyword", "slot above MAST count did not resolve to self");
    }
    void readerTests() {
        QTemporaryDir dir;
        const auto path = dir.filePath("Deleted.esp");
        write(path, record("TES4", 0) + record("WEAP", 0x123, 0x20) + record("RACE", 0x456) +
                    record("CELL", 0x800, 0x40000, "not valid compressed data"));
        auto plugin = EspReader::read(path);
        require(plugin.parsed && plugin.deletedRecords == QVector<std::uint32_t>{0x123}, "deleted record not retained");
        require(plugin.animationSubgraphs.size() == 1 && plugin.animationSubgraphs.first().entries.isEmpty(), "empty RACE not retained");
        require(!EspReader::read(path, []() { return true; }).parsed, "parser cancellation ignored");
        write(path, record("TES4", 0) + QByteArray("truncated"));
        require(!EspReader::read(path).parsed, "truncated plugin accepted");
        const auto obts = QByteArray::fromHex("040000000000000000000000ffff010174ce0e000200d3c0020000000101bf020000000169ce0e0000000104bf0200000001");
        QByteArray field("OBTS", 4);
        const auto size = qToLittleEndian(static_cast<std::uint16_t>(obts.size()));
        field.append(reinterpret_cast<const char*>(&size), 2); field.append(obts);
        write(path, record("TES4", 0) + record("WEAP", 0x62aa6, 0, field));
        plugin = EspReader::read(path);
        require(plugin.weapons.first().defaultObjectMods == QVector<std::uint32_t>({0x2c0d3, 0x2bf01, 0xece69, 0x2bf04}), "OBTS keyword array shifted include IDs");
        const auto sub=[](const QByteArray& type,const QByteArray& data){QByteArray bytes=type;const auto size=qToLittleEndian(quint16(data.size()));bytes.append(reinterpret_cast<const char*>(&size),2);return bytes+data;};
        const auto graph=sub("EDID","NativeSubgraphs\0")+sub("SGNM","Actors/Character/_1stPerson/Behaviors/Pistol.hkx\0")+
            sub("SAPT","Actors/Character/_1stPerson/Animations/Pistol\0")+sub("STKD",QByteArray::fromHex("48000100"))+sub("SRAF",QByteArray::fromHex("01000100"));
        write(path,record("TES4",0)+record("RACE",0x166729,0,graph));plugin=EspReader::read(path);
        require(plugin.parsed&&plugin.animationSubgraphs.size()==1&&plugin.animationSubgraphs[0].entries.size()==1,"Native subgraphs incorrectly require optional SADD");
        require(plugin.resolveFirstPersonSubgraph({0x10048})!=nullptr,"Native first-person weapon subgraph was not resolved");
    }
    void weaponNamesAndAdmission() {
        QTemporaryDir dir;
        const auto sub=[](const QByteArray& type,const QByteArray& data){QByteArray bytes=type;const auto size=qToLittleEndian(quint16(data.size()));bytes.append(reinterpret_cast<const char*>(&size),2);return bytes+data;};
        const auto word=[](quint32 value){QByteArray data(4,'\0');qToLittleEndian(value,reinterpret_cast<uchar*>(data.data()));return data;};
        QByteArray dnam(132,'\0'); // Zero ammo/capacity is a valid melee weapon.
        const auto fields=sub("EDID","PlayerKnife\0")+sub("FULL",word(17))+sub("MODL","Weapons/Knife.nif\0");
        const auto path=dir.filePath("Localized.esm");
        write(path,record("TES4",0,0x80)+record("WEAP",0x123,0,fields+sub("DNAM",dnam)));
        auto player=EspReader::read(path);require(player.parsed&&player.weapons.size()==1,"localized player record missing");
        require(player.weapons[0].name=="PlayerKnife"&&player.localizedNameIds.value(0x123)==17,"unresolved names must use editor IDs, not String hex placeholders");
        require(weaponAuthoringExclusion(player.weapons[0]).isEmpty(),"zero-ammo melee weapon was filtered out");
        const QByteArray label=QString::fromUtf8("Café knife").toUtf8()+'\0';
        const QByteArray table=word(1)+word(quint32(label.size()))+word(17)+word(0)+label;
        auto named=player;
        EspReader::resolveNames(named,"en",[&](const QString& resource,QString&){require(resource=="strings/localized_en.strings","wrong plugin/language string table");return table;});
        require(named.weapons[0].name==QString::fromUtf8("Café knife"),"localized UTF-8 name did not resolve");
        auto missing=player;EspReader::resolveNames(missing,"de",[](const QString&,QString& error){error="table unavailable";return QByteArray();});
        require(missing.weapons[0].name=="PlayerKnife"&&!missing.diagnostics.isEmpty(),"missing table hid a player weapon or lost its diagnostic");
        auto invalid=player;auto broken=table;broken[12]=char(127);
        EspReader::resolveNames(invalid,"en",[&](const QString&,QString&){return broken;});
        require(invalid.weapons[0].name=="PlayerKnife"&&invalid.diagnostics.join(' ').contains("Invalid"),"invalid string offset was accepted");
        auto unknown=player;unknown.localizedNameIds[0x123]=18;
        EspReader::resolveNames(unknown,"en",[&](const QString&,QString&){return table;});
        require(unknown.weapons[0].name=="PlayerKnife"&&unknown.diagnostics.join(' ').contains("1 unresolved"),"missing ID lost its fallback/diagnostic");
        write(path,record("TES4",0)+record("WEAP",0x123,4,fields+sub("DNAM",dnam)));
        const auto hidden=EspReader::read(path);require(weaponAuthoringExclusion(hidden.weapons[0])=="Non-playable weapon","record Non-Playable flag ignored");
        qToLittleEndian(quint32(0x100000),reinterpret_cast<uchar*>(dnam.data()+48));
        write(path,record("TES4",0)+record("WEAP",0x123,0,fields+sub("DNAM",dnam)));
        require(weaponAuthoringExclusion(EspReader::read(path).weapons[0])=="Non-playable weapon","DNAM Not Playable flag ignored");
        auto noModel=player.weapons[0];noModel.baseModelPath.clear();require(!weaponAuthoringExclusion(noModel).isEmpty(),"model-less internal weapon accepted");
        PluginContent patch;patch.fileName="Patch.esp";patch.masters={player.fileName};patch.parsed=true;
        auto replacement=hidden.weapons[0];replacement.formId=0x123;patch.weapons={replacement};QString error;
        const auto merged=mergeLoadOrder({player,patch},error);
        require(error.isEmpty()&&merged.weapons.size()==1&&!weaponAuthoringExclusion(merged.weapons[0]).isEmpty(),"filter resurrected the playable losing record");
        replacement.recordFlags=0;patch.weapons={replacement};
        const auto enabled=mergeLoadOrder({hidden,patch},error);
        require(error.isEmpty()&&weaponAuthoringExclusion(enabled.weapons[0]).isEmpty(),"playable winning override stayed hidden");
        write(dir.filePath("fallout4.ini"),"[General]\nsLanguage=en\n");
        write(dir.filePath("fallout4custom.ini"),"[General]\nsLanguage=de\n");
        require(sourceLanguage({dir.filePath("fallout4.ini"),dir.filePath("fallout4custom.ini")})=="de","source profile language precedence ignored");
    }
    void dependencyBrowserTests() {
        QTemporaryDir dir;
        const auto masterField=[](const char* name){QByteArray data(name);data.append('\0');QByteArray sub("MAST");const auto size=qToLittleEndian(quint16(data.size()));sub.append(reinterpret_cast<const char*>(&size),2);return sub+data;};
        write(dir.filePath("Base.esm"),record("TES4",0)+record("WEAP",0x123));
        write(dir.filePath("Addon.esm"),record("TES4",0,0,masterField("Base.esm"))+record("WEAP",0x123));
        write(dir.filePath("Patch.esl"),record("TES4",0,0,masterField("Base.esm")+masterField("Addon.esm"))+record("WEAP",0x123));
        QString error;const auto root=EspReader::read(dir.filePath("Patch.esl"));
        const auto loaded=includePackageMasters({root},dir.filePath("unused Data"),{},error);
        require(error.isEmpty()&&loaded.size()==3,"sibling/transitive masters were not auto-loaded");
        require(loaded[0].fileName=="Base.esm"&&loaded[1].fileName=="Addon.esm"&&loaded[2].fileName=="Patch.esl","master order is not dependency-first");
        const auto catalog=mergeLoadOrder(loaded,error);
        require(error.isEmpty()&&catalog.weapons.size()==1,"winning override duplicated its base weapon");
        const auto origin=catalog.origins.value(catalog.weapons[0].formId);
        require(origin.winner=="Patch.esl"&&origin.contributors==QStringList({"Base.esm","Addon.esm","Patch.esl"}),"intermediate ESP membership or winning order lost");
        require(catalog.sourcePlugins==origin.contributors&&catalog.sourceMasters.value("patch.esl")==QStringList({"Base.esm","Addon.esm"}),"browser source/master metadata lost");
        for(const auto* archive:{"Base - Main.ba2","Addon - Main.ba2","Unrelated - Animations.ba2"})write(dir.filePath(archive),{});
        const QStringList roots{dir.filePath("explicit source")};const auto layers=packageResourceLayers(loaded,roots);
        require(layers==QStringList({dir.filePath("Base - Main.ba2"),dir.filePath("Addon - Main.ba2"),roots[0]}),"development source inherited unrelated Data assets or lost master priority");
        auto missing=root;missing.masters={"Absent.esm"};
        require(includePackageMasters({missing},dir.path(),{},error).isEmpty()&&error.contains("Absent.esm"),"missing master was silently skipped");
        auto cycle=root;cycle.masters={cycle.fileName};
        require(includePackageMasters({cycle},dir.path(),{},error).isEmpty()&&error.contains("cyclic"),"cyclic masters accepted");
        const auto package=PackageSource::prepare(dir.filePath("Patch.esl"));
        require(package.ok()&&package.plugins==QStringList({QFileInfo(root.filePath).canonicalFilePath()}),"single-plugin source cannot be opened");
    }
    // The rows the source bar and the source_browser command both list.
    void sourceBrowserRowTests() {
        PluginDependencies dependencies;
        dependencies.set("Patch.esl", { "Addon.esm", "Base.esm" });
        dependencies.set("addon.esm", { "Base.esm" });
        dependencies.set("Loop.esp", { "Loop.esp", "Other.esp" });
        dependencies.set("Other.esp", { "loop.esp" });
        require(dependencies.masters("PATCH.ESL") == QStringList({ "Addon.esm", "Base.esm" }), "masters lost order or case-insensitive lookup");
        require(dependencies.masters("Loop.esp") == QStringList({ "Other.esp" }), "a master cycle repeated a plugin");
        dependencies.set("ADDON.ESM", {});
        require(dependencies.masters("Addon.esm").isEmpty(), "a later entry did not replace the earlier one");

        dependencies.set("Addon.esm", { "Base.esm" });
        const auto rows = buildSourcePluginRows({ kAllWeaponsSource, "Base.esm", "Addon.esm", "Patch.esl", "Other.esp" },
                                                "patch.esl", dependencies, { { "*", 7 }, { "patch.esl", 2 } });
        require(rows.size() == 5 && rows[0].label == "Load all weapons" && rows[0].weaponCount == 7, "all-weapons entry changed");
        require(rows[1].role == SourcePluginRole::Master && rows[1].label == "Base.esm  · master", "required master not marked");
        require(rows[3].role == SourcePluginRole::Selected && rows[3].label == "Patch.esl" && rows[3].weaponCount == 2, "selected plugin not marked");
        require(rows[4].role == SourcePluginRole::Available && rows[4].masters == QStringList({ "loop.esp" }), "unrelated plugin marked");

        const auto weapon = [](int index, const char* name, const char* record, const char* coverage) {
            SourceWeaponRow row;
            row.index = index;
            row.name = name;
            row.record = record;
            row.coverage = coverage;
            return row;
        };
        const QVector<SourceWeaponRow> weapons{ weapon(0, "pistol", "B.esp:000801", ""), weapon(1, "Rifle", "A.esp:000802", "Draft · 1 editor project(s)"),
                                                weapon(2, "Pistol", "A.esp:000803", ""), weapon(3, "Pistol", "A.esp:000804", "") };
        const auto scoped = buildSourceWeaponRows(weapons, false);
        require(scoped[0].index == 2 && scoped[1].index == 3 && scoped[2].label == "Rifle · Draft · 1 editor project(s)" && scoped[3].index == 0,
                "weapons not sorted by case-sensitive label with stable ties");
        const auto all = buildSourceWeaponRows(weapons, true);
        require(all[0].label == "Pistol · A.esp" && all[3].label == "pistol · B.esp", "all-weapons labels lost their plugin");
    }

    void profileTests() {
        QTemporaryDir dir;
        const auto instance = dir.filePath("MO2");
        const auto game = dir.filePath("Game");
        const auto data = game + "/Data";
        const auto profile = instance + "/profiles/Test";
        const QByteArray ini = "[General]\nselected_profile=@ByteArray(Test)\ngamePath=@ByteArray(" + game.toUtf8() + ")\n";
        write(instance + "/ModOrganizer.ini", ini);
        write(profile + "/plugins.txt", "# Enabled\n*Base.esp\n*Patch.esl\nDisabled.esp\n");
        write(profile + "/loadorder.txt", "Fallout4.esm\nFallout4_VR.esm\nBase.esp\nPatch.esl\nDisabled.esp\n");
        write(profile + "/fallout4.ini", "[Archive]\nsResourceArchiveList=Startup.ba2\nsResourceArchiveList2=Animations.ba2\n");
        write(profile + "/fallout4custom.ini", "[Archive]\nsResourceArchiveList2=Replacement.ba2\n");
        for (const auto* name : {"Fallout4.esm", "Fallout4_VR.esm", "Base.esp", "Patch.esl"}) { write(data + '/' + name, record("TES4", 0)); }
        // Empty placeholder files are used ONLY for snapshot membership; never
        // passed to the archive decoder or asserted to be valid BA2 assets.
        for (const auto* name : {"Startup.ba2", "Animations.ba2", "Replacement.ba2", "Base - Main.ba2", "Patch - Main.ba2", "Disabled - Main.ba2", "Unused.ba2"}) { write(data + '/' + name, {}); }
        const auto snapshot = readLoadOrderSnapshot(instance, data);
        require(snapshot.valid(), qPrintable(snapshot.error));
        require(readLoadOrderSnapshot(instance,data,"Test").valid(),"requested current profile rejected");
        require(!readLoadOrderSnapshot(instance,data,"Different").valid(),"requested profile used another virtual view");
        require(snapshot.plugins.size() == 4, "implicit masters/enabled filter incorrect");
        require(snapshot.archives == QStringList({data + "/Startup.ba2", data + "/Replacement.ba2", data + "/Base - Main.ba2", data + "/Patch - Main.ba2"}), "INI override/active archive order incorrect");
        QString archiveError;
        const auto explicitArchives=resolveGameArchives(data,snapshot.plugins,{profile+"/fallout4.ini",profile+"/fallout4custom.ini"},archiveError);
        require(archiveError.isEmpty()&&explicitArchives==snapshot.archives,"Explicit and profile archive planners differ");
        QJsonArray files,archives;for(const auto& p:snapshot.plugins)files.append(data+'/'+p);for(const auto& a:snapshot.archives)archives.append(a);
        const auto source=dir.filePath("base.reloadsources.json");
        QJsonObject definition{{"format","reload-maker-source-pack"},{"version",1},{"name","Fixture base Data"},{"plugins",files},{"resourceData",data},{"archives",archives}};
        write(source,QJsonDocument(definition).toJson());const auto package=PackageSource::prepare(source);
        require(package.ok()&&package.resourceData==data&&package.archives==snapshot.archives&&package.displayName=="Fixture base Data","Explicit source view was not retained");
        archives.append(archives[0]);definition["archives"]=archives;write(source,QJsonDocument(definition).toJson());
        require(!PackageSource::prepare(source).ok(),"Duplicate explicit archive accepted");
        const auto firstFingerprint = snapshot.fingerprint;
        write(profile + "/modlist.txt", "+AnimationReplacer\n");
        require(profileFingerprint(instance, profile) != firstFingerprint, "MO2 mod change not detected");
        write(profile + "/plugins.txt", "*Patch.esl\n*Base.esp\n");
        require(!readLoadOrderSnapshot(instance, data).valid(), "conflicting order accepted");
        write(profile + "/plugins.txt", "*Base.esp\n*Patch.esl\n*Absent.esp\n");
        require(!readLoadOrderSnapshot(instance, data).valid(), "missing enabled plugin accepted");
    }
    void resourceTests() {
        QTemporaryDir dir;
        const auto key = QStringLiteral("meshes/actors/character/animations/reload.hkx");
        write(dir.filePath(key), "replacement clip bytes");
        ResourceArchive resources;
        QString error;
        int progress = 0;
        require(resources.openLoadOrder(dir.path(), {}, error, [&](const QString&, int, int) { ++progress; }), "loose library index failed");
        require(progress > 0 && resources.contains(key.toUpper()), "normalized loose resource absent");
        require(resources.files().count(key) == 1 && !resources.provider(key).isEmpty(), "winning resource not uniquely indexed");
        setLibraryResources(&resources);
        QByteArray data;
        require(readLibraryResource(data, key.toStdString()) && data == "replacement clip bytes", "renderer and animation provider disagree");
        write(dir.filePath(key), "updated clip bytes");
        require(resources.read(key, error) == "updated clip bytes", "loose read used stale data");
        require(QFile::remove(dir.filePath(key)), "fixture removal failed");
        require(!readLibraryResource(data, key.toStdString()) && data.isEmpty(), "missing winner silently succeeded");
        setLibraryResources(nullptr);
        require(!libraryResourcesActive(), "dangling renderer resource service");
        require(!resources.openLoadOrder(dir.path(), {}, error, {}, []() { return true; }), "index cancellation ignored");
        require(!resources.isOpen(), "cancelled index installed partial resources");
    }
    void realArchiveTest(const QStringList& arguments) {
        require(arguments.size() == 5, "usage: LibraryTests --archive archive entry bsa_mcp-reference");
        QFile reference(arguments[4]);
        require(reference.open(QIODevice::ReadOnly), "cannot read archive reference");
        const QByteArray expected = reference.readAll();
        require(!expected.isEmpty(), "empty reference");
        QTemporaryDir dir;
        QString error;
        ResourceArchive resources;
        require(resources.openLoadOrder(dir.path(), {arguments[2]}, error), qPrintable(error));
        require(resources.read(arguments[3], error) == expected, "library archive bytes disagree with bsa_mcp");
        require(resources.provider(arguments[3]) == arguments[2], "archive provenance incorrect");
        const auto normalized = ResourceArchive::normalizeResourcePath(arguments[3]);
        write(dir.filePath(normalized), "new winning animation");
        require(resources.openLoadOrder(dir.path(), {arguments[2]}, error), qPrintable(error));
        require(resources.read(normalized, error) == "new winning animation", "old archived animation beat loose replacement");
        require(resources.files().count(normalized) == 1, "old and new animation both listed");
        require(QFile::remove(dir.filePath(normalized)), "cannot remove fixture override");
        require(resources.read(normalized, error).isEmpty() && !error.isEmpty(), "failed loose override resurrected old archive clip");
        QTextStream(stdout) << "PASS: library BA2 bytes match bsa_mcp, loose replacement wins once, missing winner cannot resurrect old clip\n";
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        mergeTests(); readerTests(); profileTests(); weaponNamesAndAdmission(); dependencyBrowserTests(); sourceBrowserRowTests(); resourceTests();
        if (app.arguments().value(1) == "--archive") { realArchiveTest(app.arguments()); return 0; }
        QVector<PluginContent> parsed;
        const bool merge = app.arguments().contains("--merge");
        for (const auto& path : app.arguments().mid(1)) {
            if (path == "--merge") { continue; }
            const auto plugin = EspReader::read(path);
            require(plugin.parsed, qPrintable(plugin.diagnostics.join(';')));
            QTextStream(stdout) << plugin.fileName << ": " << plugin.weapons.size() << " WEAP, " << plugin.objectMods.size() << " OMOD, " << plugin.animationSubgraphs.size() << " RACE\n";
            parsed.append(plugin);
        }
        if (merge) {
            QString error;
            const auto catalog = mergeLoadOrder(parsed, error);
            require(error.isEmpty() && catalog.parsed, qPrintable(error));
            QTextStream(stdout) << "MERGED: " << catalog.weapons.size() << " WEAP, " << catalog.objectMods.size() << " OMOD, " << catalog.origins.size() << " identities\n";
        }
        QTextStream(stdout) << "PASS: load-order identities/overrides/deletions, profile/archive plan, cancellation and shared resource reads\n";
        return 0;
    } catch (const std::exception& exception) { QTextStream(stderr) << "FAIL: " << exception.what() << '\n'; return 1; }
}
