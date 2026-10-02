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

    QString sourceLanguage(const QStringList& iniPaths) {
        QString language="en";
        for(const auto& path:iniPaths){if(!QFileInfo::exists(path))continue;bool general=false;
            for(auto line:lines(path)){
                if(line.startsWith('[')){general=line.compare("[General]",Qt::CaseInsensitive)==0;continue;}
                const auto equal=line.indexOf('=');if(general&&equal>0&&line.left(equal).trimmed().compare("sLanguage",Qt::CaseInsensitive)==0)language=line.mid(equal+1).trimmed().toLower();
            }}
        if(language.isEmpty()||language.size()>32||!std::all_of(language.begin(),language.end(),[](QChar c){return c.isLetter()&&c.unicode()<128;}))
            throw std::runtime_error("Invalid sLanguage in the selected source INIs");
        return language;
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

    QStringList resolveGameArchives(const QString& dataPath,const QStringList& plugins,const QStringList& iniPaths,QString& error) {
        error.clear();QStringList result;
        try {
            QHash<QString,QString> values;for(const auto& path:iniPaths){const auto ini=archiveSettings(path);for(auto it=ini.cbegin();it!=ini.cend();++it)values[it.key()]=it.value();}
            if(values.isEmpty())throw std::runtime_error("Archive settings are missing from the selected source INIs");
            const QDir data(dataPath);QSet<QString> added;
            const auto add=[&](const QString& name){if(!leafName(name)||!name.endsWith(".ba2",Qt::CaseInsensitive))throw std::runtime_error("Invalid BA2 name in archive settings");
                if(added.contains(name.toLower()))return;const auto path=data.filePath(name);if(!QFileInfo(path).isFile())throw std::runtime_error(("Required archive is not visible: "+name).toStdString());added.insert(name.toLower());result.append(path);};
            for(const auto* key:{"sresourceindexfilelist","sresourcestartuparchivelist","sresourcearchivelist","sresourcearchivelist2"})for(auto name:values.value(key).split(',',Qt::SkipEmptyParts))add(name.trimmed());
            const auto available=data.entryList({"*.ba2"},QDir::Files,QDir::Name|QDir::IgnoreCase);
            // xEdit 4.1.5f wbHelpers.HasBSAs / xeMainForm resource loading uses
            // partial prefix matching for FO4/VR, after ordered INI archives.
            for(const auto& plugin:plugins){if(!leafName(plugin)||!QFileInfo(data.filePath(plugin)).isFile())throw std::runtime_error(("Source plugin is not visible in Data: "+plugin).toStdString());
                const auto stem=QFileInfo(plugin).completeBaseName();for(const auto& archive:available)if(archive.startsWith(stem,Qt::CaseInsensitive))add(archive);}
        }catch(const std::exception& e){error=e.what();result.clear();}
        return result;
    }
    LoadOrderSnapshot readLoadOrderSnapshot(const QString& instancePath, const QString& dataPath,const QString& requestedProfile) {
        LoadOrderSnapshot result;
        result.instancePath = QDir(instancePath).absolutePath();
        try {
            const QString configPath = QDir(instancePath).filePath("ModOrganizer.ini");
            if (!QFileInfo(configPath).isFile()) { throw std::runtime_error("Select the MO2 instance containing ModOrganizer.ini"); }
            QSettings settings(configPath, QSettings::IniFormat);
            result.profileName = iniText(settings.value("General/selected_profile", settings.value("selected_profile")));
            if (!leafName(result.profileName)) { throw std::runtime_error("MO2 selected_profile is missing or invalid"); }
            if(!requestedProfile.isEmpty()&&requestedProfile!=result.profileName)throw std::runtime_error(QString("Requested profile '%1', but MO2 is on '%2'. Reconnect through the requested profile.").arg(requestedProfile,result.profileName).toStdString());
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

            QString archiveError;
            result.archives=resolveGameArchives(result.dataPath,result.plugins,{QDir(result.profilePath).filePath("fallout4.ini"),QDir(result.profilePath).filePath("fallout4custom.ini")},archiveError);
            if(!archiveError.isEmpty())throw std::runtime_error(archiveError.toStdString());
            result.language=sourceLanguage({QDir(result.profilePath).filePath("fallout4.ini"),QDir(result.profilePath).filePath("fallout4custom.ini")});
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
        QHash<std::uint32_t, SoundRecord> sounds;
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
                    weapons.remove(id); sounds.remove(id);mods.remove(id); graphs.remove(id); result.keywordNames.remove(id);result.nodeRemaps.remove(id);result.ammoNames.remove(id); winner(id);
                }
                for (auto weapon : plugin.weapons) {
                    weapon.formId = resolve(weapon.formId);
                    remap(weapon.keywords); remap(weapon.defaultObjectMods); remap(weapon.attachParentSlots);
                    weapon.ammo=resolve(weapon.ammo);
                    weapon.attackFailSound=resolve(weapon.attackFailSound);
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
                for(auto sound:plugin.sounds){sound.formId=resolve(sound.formId);winner(sound.formId);sounds.insert(sound.formId,std::move(sound));}
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
            result.sounds=sounds.values();
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
        if (!result.resources.openLoadOrder(snapshot.dataPath, snapshot.archives, result.error, progress, cancelled)) {
            result.cancelled = stopped(); return result;
        }
        for(auto& plugin:contents){if(stopped()){result.cancelled=true;return result;}
            EspReader::resolveNames(plugin,snapshot.language,[&](const QString& path,QString& error){return result.resources.read(path,error);});}
        if (progress) { progress(QStringLiteral("Resolving winning records and cross-plugin references"), 0, 0); }
        result.catalog = mergeLoadOrder(contents, result.error);
        result.catalog.diagnostics.append(diagnostics);
        if (!result.error.isEmpty()) { return result; }
        if (profileFingerprint(snapshot.instancePath, snapshot.profilePath) != snapshot.fingerprint) {
            result.error = QStringLiteral("MO2 profile changed during indexing. Relaunch Reload Maker through MO2.");
        }
        result.cancelled = stopped();
        return result;
    }
}
