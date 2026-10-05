// Resident sources (owner, 2026-10-04: keep what was loaded, like the game
// does, so switching testbeds and weapon projects does not read everything
// again). A development source is parked in memory when another source is
// installed; opening a project from a parked or the installed source checks
// the stamp of its files on the worker and installs the kept copy through the
// normal install path, so the result is the same as a fresh read. Parked
// sources are dropped least recently used first when there are too many or
// the process passes its memory budget (--resident-memory-mb, 16 GB by
// default; the suite runner splits it between its editors).
#include "ReloadMakerController.h"
#include "areas/sources/PackageLoad.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <windows.h>
#include <psapi.h>

namespace reloadmaker
{
    namespace
    {
        constexpr int kMaximumResidentSources = 16;
        constexpr qint64 kDefaultResidentMegabytes = 16 * 1024;

        qint64 processPrivateBytes()
        {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
                return 0;
            }
            return qint64(counters.PrivateUsage);
        }

        qint64 residentBudgetBytes()
        {
            bool ok = false;
            const qint64 megabytes = qApp->property("residentMemoryMb").toLongLong(&ok);
            return (ok && megabytes > 0 ? megabytes : kDefaultResidentMegabytes) * 1024 * 1024;
        }
    }

    QString referenceDataFolder(const QString& dataPath)
    {
        const QString data = QDir(dataPath).absolutePath();
        if (QFileInfo(QDir(data).filePath("Data")).isDir()) {
            return QDir(data).filePath("Data");
        }
        return data;
    }

    QString packageSourceKey(const QString& path, const QString& dataPath)
    {
        const QString source = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QString data = dataPath.isEmpty() ? QString() : QDir::cleanPath(referenceDataFolder(dataPath));
        return (source + QLatin1Char('|') + data).toLower();
    }

    QStringList packageStampFiles(const PreparedPackage& package, const QVector<PluginContent>& plugins, const QString& data)
    {
        QStringList files;
        if (!package.sourcePath.isEmpty()) {
            files << package.sourcePath;
        }
        files += package.plugins;
        files += package.archives;
        for (const PluginContent& plugin : plugins) {
            if (!plugin.filePath.isEmpty()) {
                const QFileInfo info(plugin.filePath);
                files << info.absoluteFilePath();
                for (const QString& archive : info.absoluteDir().entryList({ QStringLiteral("*.ba2") }, QDir::Files, QDir::Name)) {
                    files << info.absoluteDir().filePath(archive);
                }
            }
            if (!data.isEmpty()) {
                for (const QString& master : plugin.masters) {
                    files << QDir(data).filePath(master);
                }
            }
        }
        if (!data.isEmpty()) {
            for (const char* archive : { "Fallout4 - Meshes.ba2", "Fallout4 - Animations.ba2", "Fallout4 - Sounds.ba2" }) {
                files << QDir(data).filePath(QLatin1String(archive));
            }
        }
        files.removeDuplicates();
        files.sort(Qt::CaseInsensitive);
        return files;
    }

    QStringList libraryStampFiles(const LoadOrderSnapshot& snapshot)
    {
        QStringList files;
        for (const QString& plugin : snapshot.plugins) {
            files << QDir(snapshot.dataPath).filePath(plugin);
        }
        for (const QString& archive : snapshot.archives) {
            files << QDir(snapshot.dataPath).filePath(archive);
        }
        return files;
    }

    QByteArray sourceStamp(const QStringList& files)
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const QString& path : files) {
            const QFileInfo info(path);
            hash.addData(path.toUtf8());
            hash.addData(QByteArray::number(info.exists() ? info.size() : -1));
            hash.addData(QByteArray::number(info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0));
        }
        return hash.result();
    }

    // Moves the installed development source into `load`.
    void ReloadMakerController::takeCurrentPackage(PackageLoad& load)
    {
        load.package = _package;
        load.resources = std::move(_packageResources);
        load.meshes = std::move(_gameMeshes);
        load.animations = std::move(_gameAnimations);
        load.sounds = std::move(_gameSounds);
        load.data = _gameData;
        load.plugins = std::move(_plugins);
        load.referenceNames = std::move(_referenceNames);
        load.referenceSounds = std::move(_referenceSounds);
        load.referenceSoundMappings = std::move(_referenceSoundMappings);
        load.referenceSoundKeywords = std::move(_referenceSoundKeywords);
        load.stamp = std::move(_packageStamp);
        _plugins.clear();
        _packageStamp.clear();
    }

    // Keeps the installed development source before another one replaces it.
    // The caller has already detached the library resource service.
    void ReloadMakerController::parkCurrentPackage()
    {
        if (_libraryMode || !_packageResources.isOpen() || _packageStamp.isEmpty()) {
            return;
        }
        auto load = std::make_shared<PackageLoad>();
        takeCurrentPackage(*load);
        const QString key = packageSourceKey(load->package.sourcePath, load->data);
        _residentPackages.insert(key, load);
        _residentOrder.removeAll(key);
        _residentOrder.append(key);
        evictResidentSources();
    }

    std::shared_ptr<PackageLoad> ReloadMakerController::takeResidentPackage(const QString& key)
    {
        _residentOrder.removeAll(key);
        return _residentPackages.take(key);
    }

    void ReloadMakerController::evictResidentSources()
    {
        const qint64 budget = residentBudgetBytes();
        while (!_residentOrder.isEmpty() && (_residentOrder.size() > kMaximumResidentSources || processPrivateBytes() > budget)) {
            const QString key = _residentOrder.takeFirst();
            _residentPackages.remove(key);
            _loadTimings.add(QStringLiteral("resident.evicted ") + key, 0);
        }
    }

    void ReloadMakerController::clearResidentSources()
    {
        _residentPackages.clear();
        _residentOrder.clear();
    }

    // "Reload source files": nothing kept in memory is trusted.
    void ReloadMakerController::reloadSourceFiles()
    {
        clearResidentSources();
        if (_libraryMode) {
            startLibraryLoad();
        } else {
            openPackage(_packageText);
        }
    }

    QJsonObject ReloadMakerController::residentState() const
    {
        return { { "resident", QJsonArray::fromStringList(_residentOrder) },
                 { "privateBytes", double(processPrivateBytes()) },
                 { "budgetBytes", double(residentBudgetBytes()) } };
    }
}
