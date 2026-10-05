#include "LoadOrderLibrary.h"

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
        QHash<std::uint32_t, SoundKeywordMapping> soundMappings;
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
                const auto winner = [&](std::uint32_t id) { auto& origin=result.origins[id];origin.winner = plugin.fileName;
                    if(!origin.contributors.contains(plugin.fileName,Qt::CaseInsensitive))origin.contributors.append(plugin.fileName); };
                for (const auto raw : plugin.deletedRecords) {
                    const auto id = resolve(raw);
                    weapons.remove(id); sounds.remove(id);soundMappings.remove(id);result.soundKeywords.remove(id);mods.remove(id); graphs.remove(id); result.keywordNames.remove(id);result.nodeRemaps.remove(id);result.ammoNames.remove(id); winner(id);
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
                    result.soundKeywords.remove(id);
                    if(plugin.soundKeywords.contains(it.key()))result.soundKeywords.insert(id);
                }
                for(auto sound:plugin.sounds){sound.formId=resolve(sound.formId);remap(sound.children);winner(sound.formId);sounds.insert(sound.formId,std::move(sound));}
                for(auto mapping:plugin.soundMappings){mapping.formId=resolve(mapping.formId);mapping.descriptor=resolve(mapping.descriptor);remap(mapping.keywords);
                    winner(mapping.formId);soundMappings.insert(mapping.formId,std::move(mapping));}
                for(auto it=plugin.ammoNames.cbegin();it!=plugin.ammoNames.cend();++it){const auto id=resolve(it.key());winner(id);result.ammoNames.insert(id,it.value());}
                loaded.insert(plugin.fileName.toLower());
                result.sourcePlugins.append(plugin.fileName);
                result.sourceMasters.insert(plugin.fileName.toLower(),plugin.masters);
                for (const auto& diagnostic : plugin.diagnostics) {
                    result.diagnostics.append(QStringLiteral("%1: %2").arg(plugin.fileName, diagnostic));
                }
                if (noncanonicalSelfReferences) {
                    result.diagnostics.append(QStringLiteral("%1: resolved %2 noncanonical self references using the file-local MAST boundary (xEdit rule)").arg(plugin.fileName).arg(noncanonicalSelfReferences));
                }
            }
            result.weapons = weapons.values(); result.objectMods = mods.values(); result.animationSubgraphs = graphs.values();
            result.sounds=sounds.values();result.soundMappings=soundMappings.values();
            const auto byId = [](const auto& a, const auto& b) { return a.formId < b.formId; };
            std::sort(result.weapons.begin(), result.weapons.end(), byId);
            std::sort(result.objectMods.begin(), result.objectMods.end(), byId);
            std::sort(result.animationSubgraphs.begin(), result.animationSubgraphs.end(), byId);
            result.parsed = true;
        } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
        return result;
    }

    QVector<PluginContent> includePackageMasters(const QVector<PluginContent>& plugins,const QString& dataPath,
        const QString& dependencyPath,const LibraryCancelled& cancelled,QString& error) {
        QVector<PluginContent> result;QHash<QString,PluginContent> available;QSet<QString> done,visiting;error.clear();
        for(const auto& plugin:plugins)available.insert(plugin.fileName.toLower(),plugin);
        std::function<void(const QString&,const QString&)> add=[&](const QString& name,const QString& parentFolder){
            if(cancelled&&cancelled())throw std::runtime_error("Cancelled");
            const auto key=name.toLower();if(done.contains(key))return;
            if(!leafName(name)||visiting.contains(key)||visiting.size()>64||result.size()>=256)throw std::runtime_error("Invalid or cyclic source master dependencies");
            visiting.insert(key);auto plugin=available.value(key);
            if(!available.contains(key)) {
                // Data stays authoritative: a dependency copy only fills a gap.
                QString path=QDir(dataPath).filePath(name);
                for(const auto& folder:{parentFolder,dataPath,dependencyPath}) {
                    if(!folder.isEmpty()&&QFileInfo::exists(QDir(folder).filePath(name))){path=QDir(folder).filePath(name);break;}
                }
                plugin=EspReader::read(path,cancelled);
            }
            if(!plugin.parsed)throw std::runtime_error(("Required source master is unavailable: "+name+". "+plugin.diagnostics.join(';')).toStdString());
            for(const auto& master:plugin.masters)add(master,QFileInfo(plugin.filePath).absolutePath());
            visiting.remove(key);done.insert(key);result.append(std::move(plugin));
        };
        try{for(const auto& plugin:plugins)add(plugin.fileName,QFileInfo(plugin.filePath).absolutePath());}catch(const std::exception& e){error=e.what();result.clear();}
        return result;
    }

    QStringList packageResourceLayers(const QVector<PluginContent>& plugins,const QStringList& roots) {
        QStringList layers;
        // A saved development source inherits only its masters' archives. Scanning
        // the whole virtual Data directory would silently import unrelated MO2
        // skeleton/animation replacements into the saved testbed.
        for(const auto& plugin:plugins) {
            const QFileInfo file(plugin.filePath);const auto stem=file.completeBaseName();
            const QDir folder=file.absoluteDir();
            for(const auto& archive:folder.entryList({"*.ba2"},QDir::Files,QDir::Name)) {
                const auto name=QFileInfo(archive).completeBaseName();
                if(name.compare(stem,Qt::CaseInsensitive)!=0&&!name.startsWith(stem+" - ",Qt::CaseInsensitive))continue;
                const auto path=folder.filePath(archive);if(!layers.contains(path,Qt::CaseInsensitive))layers.append(path);
            }
        }
        for(const auto& root:roots)if(!layers.contains(root,Qt::CaseInsensitive))layers.append(root);
        return layers;
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
        result.cancelled = stopped();
        return result;
    }
}
