#include "LoadOrderLibrary.h"
#include "LibraryResources.h"
#include "WeaponConfiguration.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
        patch.ammoNames.remove(0x01000700);
        auto deleted = mergeLoadOrder({base, extra, patch}, error);
        require(deleted.weapons.size() == 1 && deleted.weapons.first().name == unrelated.name, "deleted WEAP reappeared or deleted unrelated local ID");
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
        require(snapshot.plugins.size() == 4, "implicit masters/enabled filter incorrect");
        require(snapshot.archives == QStringList({data + "/Startup.ba2", data + "/Replacement.ba2", data + "/Base - Main.ba2", data + "/Patch - Main.ba2"}), "INI override/active archive order incorrect");
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
        mergeTests(); readerTests(); profileTests(); resourceTests();
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
