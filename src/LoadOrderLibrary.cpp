#include "LoadOrderLibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <algorithm>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace reloadmaker {
    namespace {
        QStringList lines(const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                throw std::runtime_error(QStringLiteral("Cannot read %1: %2").arg(path, file.errorString()).toStdString());
            }
            QString text = QString::fromUtf8(file.readAll());
            text.remove(QChar(0xfeff));
            QStringList result;
            for (auto line : text.split('\n')) {
                line = line.trimmed();
                if (!line.isEmpty() && !line.startsWith('#')) { result.append(line); }
            }
            return result;
        }
        bool leafName(const QString& name) {
            return !name.isEmpty() && name != "." && name != ".." &&
                   !name.contains('/') && !name.contains('\\') && !name.contains(':');
        }
        QString iniText(const QVariant& value) {
            return value.metaType().id() == QMetaType::QByteArray ? QString::fromUtf8(value.toByteArray()) : value.toString();
        }
        QHash<QString, QString> archiveSettings(const QString& path) {
            QHash<QString, QString> result;
            if (!QFileInfo::exists(path)) { return result; }
            bool archive = false;
            for (auto line : lines(path)) {
                if (line.startsWith(';')) { continue; }
                if (line.startsWith('[')) { archive = line.compare("[Archive]", Qt::CaseInsensitive) == 0; continue; }
                const int equal = line.indexOf('=');
                if (archive && equal > 0) { result.insert(line.left(equal).trimmed().toLower(), line.mid(equal + 1).trimmed()); }
            }
            return result;
        }
    }

    bool insideMo2VirtualFilesystem() {
#ifdef Q_OS_WIN
        return GetModuleHandleW(L"usvfs_x64.dll") != nullptr;
#else
        return false;
#endif
    }

    QByteArray profileFingerprint(const QString& instancePath, const QString& profilePath) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        // Only selected profile/game paths, not window geometry changed by MO2.
        QSettings ini(QDir(instancePath).filePath("ModOrganizer.ini"), QSettings::IniFormat);
        hash.addData(iniText(ini.value("General/selected_profile", ini.value("selected_profile"))).toUtf8());
        hash.addData(iniText(ini.value("General/gamePath", ini.value("gamePath"))).toUtf8());
        for (const auto* name : {"modlist.txt", "plugins.txt", "loadorder.txt", "archives.txt", "fallout4.ini", "fallout4custom.ini", "initweaks.ini"}) {
            QFile file(QDir(profilePath).filePath(QLatin1String(name)));
            hash.addData(QByteArray(name));
            if (file.open(QIODevice::ReadOnly)) { hash.addData(file.readAll()); }
        }
        return hash.result();
    }

    LoadOrderSnapshot readLoadOrderSnapshot(const QString& instancePath, const QString& dataPath) {
        LoadOrderSnapshot result;
        result.instancePath = QDir(instancePath).absolutePath();
        try {
            const QString configPath = QDir(instancePath).filePath("ModOrganizer.ini");
            if (!QFileInfo(configPath).isFile()) { throw std::runtime_error("Select the MO2 instance containing ModOrganizer.ini"); }
            QSettings settings(configPath, QSettings::IniFormat);
            result.profileName = iniText(settings.value("General/selected_profile", settings.value("selected_profile")));
            if (!leafName(result.profileName)) { throw std::runtime_error("MO2 selected_profile is missing or invalid"); }
            QString profiles = iniText(settings.value("Settings/profiles_directory"));
            if (profiles.isEmpty()) { profiles = QDir(instancePath).filePath("profiles"); }
            profiles.replace("%BASE_DIR%", result.instancePath, Qt::CaseInsensitive);
            if (QDir::isRelativePath(profiles)) { profiles = QDir(instancePath).filePath(profiles); }
            result.profilePath = QDir(profiles).filePath(result.profileName);
            const auto initialFingerprint = profileFingerprint(result.instancePath, result.profilePath);
            const QString game = iniText(settings.value("General/gamePath", settings.value("gamePath")));
            result.dataPath = dataPath.isEmpty() ? QDir(game).filePath("Data") : QDir(dataPath).absolutePath();
            if (!QDir(result.dataPath).exists()) { throw std::runtime_error("Reference Data folder is missing"); }
            if (!game.isEmpty() && QDir::cleanPath(result.dataPath).compare(QDir::cleanPath(QDir(game).filePath("Data")), Qt::CaseInsensitive) != 0) {
                throw std::runtime_error("Reference Data must match this MO2 instance's game Data folder");
            }
            QSet<QString> enabled;
            QStringList activeOrder;
            for (auto name : lines(QDir(result.profilePath).filePath("plugins.txt"))) {
                if (!name.startsWith('*')) { continue; }
                name.remove(0, 1);
                if (!leafName(name) || enabled.contains(name.toLower())) { throw std::runtime_error("Invalid or duplicate enabled plugin name"); }
                enabled.insert(name.toLower()); activeOrder.append(name.toLower());
            }
            // These two mandatory masters are unstarred in FO4VR MO2 profiles.
            enabled.insert("fallout4.esm"); enabled.insert("fallout4_vr.esm");
            QSet<QString> seen;
            for (const auto& name : lines(QDir(result.profilePath).filePath("loadorder.txt"))) {
                const auto key = name.toLower();
                if (!enabled.contains(key)) { continue; }
                if (!leafName(name) || seen.contains(key)) { throw std::runtime_error("Invalid or duplicate load-order entry"); }
                seen.insert(key); result.plugins.append(name);
            }
            if (seen != enabled) { throw std::runtime_error("Enabled plugins are absent from loadorder.txt; refresh the MO2 profile before launching"); }
            QStringList orderedActive;
            for (const auto& name : result.plugins) { if (activeOrder.contains(name.toLower())) { orderedActive.append(name.toLower()); } }
            if (orderedActive != activeOrder) { throw std::runtime_error("plugins.txt and loadorder.txt disagree; refresh the MO2 profile before launching"); }

            auto archiveValues = archiveSettings(QDir(result.profilePath).filePath("fallout4.ini"));
            const auto custom = archiveSettings(QDir(result.profilePath).filePath("fallout4custom.ini"));
            for (auto it = custom.cbegin(); it != custom.cend(); ++it) { archiveValues.insert(it.key(), it.value()); }
            if (archiveValues.isEmpty()) { throw std::runtime_error("Profile-local Fallout 4 archive settings are missing"); }
            const QDir data(result.dataPath);
            QSet<QString> added;
            const auto addArchive = [&](const QString& name) {
                if (!leafName(name) || !name.endsWith(".ba2", Qt::CaseInsensitive)) { throw std::runtime_error("Invalid BA2 name in archive settings"); }
                if (added.contains(name.toLower())) { return; }
                const QString path = data.filePath(name);
                if (!QFileInfo(path).isFile()) { throw std::runtime_error(QStringLiteral("Required archive is not visible: %1").arg(path).toStdString()); }
                added.insert(name.toLower()); result.archives.append(path);
            };
            // Same separation as the installed xEdit loader (wbHelpers/xeMainForm):
            // INI archives first, plugin-associated archives in plugin load order,
            // then the virtual loose folder. Never glob every BA2 as active.
            for (const auto* key : {"sresourceindexfilelist", "sresourcestartuparchivelist", "sresourcearchivelist", "sresourcearchivelist2"}) {
                for (auto name : archiveValues.value(QLatin1String(key)).split(',', Qt::SkipEmptyParts)) { addArchive(name.trimmed()); }
            }
            const auto available = data.entryList({"*.ba2"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
            for (const auto& plugin : result.plugins) {
                if (!QFileInfo(data.filePath(plugin)).isFile()) {
                    throw std::runtime_error(QStringLiteral("Enabled plugin is not visible in Data: %1. Launch Reload Maker through this MO2 instance.").arg(plugin).toStdString());
                }
                const QString base = QFileInfo(plugin).completeBaseName();
                for (const auto& archive : available) {
                    if (archive.startsWith(base, Qt::CaseInsensitive)) { addArchive(archive); }
                }
            }
            result.fingerprint = profileFingerprint(result.instancePath, result.profilePath);
            if (result.fingerprint != initialFingerprint) { throw std::runtime_error("MO2 profile changed while reading it; relaunch the viewer through MO2"); }
        } catch (const std::exception& exception) { result.error = QString::fromUtf8(exception.what()); }
        return result;
    }

    PluginContent mergeLoadOrder(const QVector<PluginContent>& plugins, QString& error) {
        PluginContent result;
        result.fileName = QStringLiteral("MO2 winning records");
        error.clear();
        QHash<QString, std::uint32_t> identities;
        QSet<QString> loaded;
        QHash<std::uint32_t, WeaponRecord> weapons;
        QHash<std::uint32_t, ObjectModRecord> mods;
        QHash<std::uint32_t, AnimationSubgraphRecord> graphs;
        try {
            for (const auto& plugin : plugins) {
                if (!plugin.parsed) { continue; }
                for (const auto& master : plugin.masters) {
                    if (!loaded.contains(master.toLower())) { throw std::runtime_error(QStringLiteral("%1 requires an earlier valid master: %2").arg(plugin.fileName, master).toStdString()); }
                }
                int noncanonicalSelfReferences = 0;
                const auto resolve = [&](std::uint32_t local) -> std::uint32_t {
                    if (!local) { return 0; }
                    const auto index = local >> 24;
                    // xEdit TwbFile::FileFileIDtoLoadOrderFileID treats slots
                    // at/above MAST count as self. SREP REDUX has one master,
                    // record 01:0042EC and references 02:0042EC to that keyword.
                    // Never interpret the high byte as a global load-order slot.
                    if (index > static_cast<std::uint32_t>(plugin.masters.size())) { ++noncanonicalSelfReferences; }
                    const QString origin = index >= static_cast<std::uint32_t>(plugin.masters.size()) ? plugin.fileName : plugin.masters[index];
                    const auto object = local & 0xffffffU;
                    const QString key = origin.toLower() + ':' + QString::number(object, 16);
                    auto found = identities.constFind(key);
                    if (found != identities.cend()) { return found.value(); }
                    const auto id = static_cast<std::uint32_t>(identities.size() + 1);
                    identities.insert(key, id);
                    result.origins.insert(id, {origin, {}, object});
                    return id;
                };
                const auto remap = [&](QVector<std::uint32_t>& values) { for (auto& id : values) { id = resolve(id); } };
                const auto winner = [&](std::uint32_t id) { result.origins[id].winner = plugin.fileName; };
                for (const auto raw : plugin.deletedRecords) {
                    const auto id = resolve(raw);
                    weapons.remove(id); mods.remove(id); graphs.remove(id); result.keywordNames.remove(id);result.nodeRemaps.remove(id);result.ammoNames.remove(id); winner(id);
                }
                for (auto weapon : plugin.weapons) {
                    weapon.formId = resolve(weapon.formId);
                    remap(weapon.keywords); remap(weapon.defaultObjectMods); remap(weapon.attachParentSlots);
                    weapon.ammo=resolve(weapon.ammo);
                    winner(weapon.formId); weapons.insert(weapon.formId, std::move(weapon));
                }
                for (auto mod : plugin.objectMods) {
                    mod.formId = resolve(mod.formId); mod.attachPoint = resolve(mod.attachPoint);
                    remap(mod.addedKeywords); remap(mod.attachParentSlots); remap(mod.targetKeywords);
                    remap(mod.removedKeywords);
                    for(auto& property:mod.ammoProperties)if(property.property==61&&property.valueType==4)property.value1=resolve(property.value1);
                    winner(mod.formId); mods.insert(mod.formId, std::move(mod));
                }
                for (auto graph : plugin.animationSubgraphs) {
                    graph.formId = resolve(graph.formId);
                    for (auto& entry : graph.entries) { remap(entry.requiredKeywords); }
                    winner(graph.formId); graphs.insert(graph.formId, std::move(graph));
                }
                for (auto it = plugin.keywordNames.cbegin(); it != plugin.keywordNames.cend(); ++it) {
                    const auto id = resolve(it.key()); winner(id); result.keywordNames.insert(id, it.value());
                    result.nodeRemaps.remove(id);
                    if(plugin.nodeRemaps.contains(it.key()))result.nodeRemaps.insert(id,plugin.nodeRemaps.value(it.key()));
                }
                for(auto it=plugin.ammoNames.cbegin();it!=plugin.ammoNames.cend();++it){const auto id=resolve(it.key());winner(id);result.ammoNames.insert(id,it.value());}
                loaded.insert(plugin.fileName.toLower());
                for (const auto& diagnostic : plugin.diagnostics) {
                    result.diagnostics.append(QStringLiteral("%1: %2").arg(plugin.fileName, diagnostic));
                }
                if (noncanonicalSelfReferences) {
                    result.diagnostics.append(QStringLiteral("%1: resolved %2 noncanonical self references using the file-local MAST boundary (xEdit rule)").arg(plugin.fileName).arg(noncanonicalSelfReferences));
                }
            }
            result.weapons = weapons.values(); result.objectMods = mods.values(); result.animationSubgraphs = graphs.values();
            const auto byId = [](const auto& a, const auto& b) { return a.formId < b.formId; };
            std::sort(result.weapons.begin(), result.weapons.end(), byId);
            std::sort(result.objectMods.begin(), result.objectMods.end(), byId);
            std::sort(result.animationSubgraphs.begin(), result.animationSubgraphs.end(), byId);
            result.parsed = true;
        } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
        return result;
    }

    LoadOrderLibrary loadOrderLibrary(const LoadOrderSnapshot& snapshot,
        const LibraryProgress& progress, const LibraryCancelled& cancelled) {
        LoadOrderLibrary result;
        result.snapshot = snapshot;
        const auto stopped = [&]() { return cancelled && cancelled(); };
        if (!snapshot.valid()) { result.error = snapshot.error; return result; }
        QVector<PluginContent> contents;
        QSet<QString> loaded;
        for (int i = 0; i < snapshot.plugins.size(); ++i) {
            if (stopped()) { result.cancelled = true; return result; }
            const QString name = snapshot.plugins[i];
            if (progress) { progress(QStringLiteral("Reading %1").arg(name), i + 1, snapshot.plugins.size()); }
            auto plugin = EspReader::read(QDir(snapshot.dataPath).filePath(name), cancelled);
            LibraryPlugin row{name, static_cast<int>(plugin.weapons.size()), static_cast<int>(plugin.objectMods.size()), {}};
            if (!plugin.parsed) { row.error = plugin.diagnostics.join('\n'); }
            for (const auto& master : plugin.masters) {
                if (!loaded.contains(master.toLower())) { row.error = QStringLiteral("Required earlier master is missing/invalid: %1").arg(master); break; }
            }
            if (row.error.isEmpty()) { contents.append(std::move(plugin)); loaded.insert(name.toLower()); }
            else { result.catalog.diagnostics.append(QStringLiteral("%1: %2").arg(name, row.error)); }
            result.plugins.append(row);
        }
        if (stopped()) { result.cancelled = true; return result; }
        const auto diagnostics = result.catalog.diagnostics;
        if (!diagnostics.isEmpty()) {
            result.error = QStringLiteral("Library not installed: %1 plugin errors. See Diagnostics. Ignoring an invalid patch could resurrect overridden records.").arg(diagnostics.size());
            return result;
        }
        if (progress) { progress(QStringLiteral("Resolving winning records and cross-plugin references"), 0, 0); }
        result.catalog = mergeLoadOrder(contents, result.error);
        result.catalog.diagnostics.append(diagnostics);
        if (!result.error.isEmpty()) { return result; }
        if (!result.resources.openLoadOrder(snapshot.dataPath, snapshot.archives, result.error, progress, cancelled)) {
            result.cancelled = stopped(); return result;
        }
        if (profileFingerprint(snapshot.instancePath, snapshot.profilePath) != snapshot.fingerprint) {
            result.error = QStringLiteral("MO2 profile changed during indexing. Relaunch Reload Maker through MO2.");
        }
        result.cancelled = stopped();
        return result;
    }
}
