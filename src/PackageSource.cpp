#include "PackageSource.h"
#include "StudioPaths.h"

#include "ba2file.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScopeGuard>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace reloadmaker
{
    namespace
    {
        constexpr qint64 kMaximumZipBytes = 4LL * 1024LL * 1024LL * 1024LL;
        constexpr qint64 kMaximumExtractedBytes = 8LL * 1024LL * 1024LL * 1024LL;
        constexpr qsizetype kMaximumEntries = 50'000;
        constexpr int kProcessTimeoutMilliseconds = 180'000;

        [[nodiscard]] PreparedPackage packageError(QString sourcePath, QString error)
        {
            return PreparedPackage{ {}, std::move(sourcePath), std::move(error), false };
        }

        [[nodiscard]] PreparedPackage packageReady(QString rootPath, QString sourcePath,
                                                    const bool fromArchive = false)
        {
            return PreparedPackage{
                std::move(rootPath), std::move(sourcePath), {}, fromArchive
            };
        }

        [[nodiscard]] QString hashFile(const QString& path, QString& error)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("Cannot read ZIP file: %1").arg(file.errorString());
                return {};
            }

            QCryptographicHash hash(QCryptographicHash::Sha256);
            QByteArray block;
            block.resize(1024 * 1024);
            while (!file.atEnd()) {
                const qint64 count = file.read(block.data(), block.size());
                if (count < 0) {
                    error = QStringLiteral("Cannot hash ZIP file: %1").arg(file.errorString());
                    return {};
                }
                hash.addData(QByteArrayView(block.constData(), count));
            }
            return QString::fromLatin1(hash.result().toHex());
        }

        [[nodiscard]] bool safeArchiveEntry(const QString& entry, QString& reason)
        {
            if (entry.isEmpty()) {
                reason = QStringLiteral("empty archive entry");
                return false;
            }

            QString normalized = entry;
            normalized.replace('\\', '/');
            if (normalized.startsWith('/') || normalized.startsWith("//") ||
                QRegularExpression(QStringLiteral("^[A-Za-z]:")).match(normalized).hasMatch()) {
                reason = QStringLiteral("absolute archive path");
                return false;
            }

            const QString clean = QDir::cleanPath(normalized);
            if (clean == QStringLiteral("..") || clean.startsWith(QStringLiteral("../")) ||
                clean.contains(QStringLiteral("/../")) || clean.contains(':')) {
                reason = QStringLiteral("archive path escapes the package root");
                return false;
            }

            const QStringList segments = clean.split('/', Qt::SkipEmptyParts);
            if (segments.size() > 64) {
                reason = QStringLiteral("archive path is too deep");
                return false;
            }
            for (const QString& segment : segments) {
                const QString upper = segment.section('.', 0, 0).toUpper();
                static const QStringList devices{
                    QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                    QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
                    QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
                    QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
                    QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
                    QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
                    QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
                    QStringLiteral("LPT9")
                };
                if (devices.contains(upper)) {
                    reason = QStringLiteral("Windows device name in archive path");
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool extractedTreeIsSafe(const QString& root, QString& error)
        {
            const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
            if (canonicalRoot.isEmpty()) {
                error = QStringLiteral("Extracted package root is unavailable");
                return false;
            }

            qint64 totalBytes = 0;
            qsizetype entryCount = 0;
            QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                iterator.next();
                ++entryCount;
                if (entryCount > kMaximumEntries) {
                    error = QStringLiteral("ZIP contains more than %1 entries").arg(kMaximumEntries);
                    return false;
                }

                const QFileInfo info = iterator.fileInfo();
                if (info.isSymLink()) {
                    error = QStringLiteral("ZIP created a symbolic link: %1").arg(info.filePath());
                    return false;
                }
#ifdef Q_OS_WIN
                const std::wstring nativePath = QDir::toNativeSeparators(info.filePath()).toStdWString();
                const DWORD attributes = GetFileAttributesW(nativePath.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    error = QStringLiteral("ZIP created a reparse point: %1").arg(info.filePath());
                    return false;
                }
#endif
                const QString canonical = info.canonicalFilePath();
                if (canonical.isEmpty() ||
                    !(canonical == canonicalRoot ||
                      canonical.startsWith(canonicalRoot + QLatin1Char('/'), Qt::CaseInsensitive))) {
                    error = QStringLiteral("Extracted file escapes the cache root: %1").arg(info.filePath());
                    return false;
                }
                if (info.isFile()) {
                    totalBytes += info.size();
                    if (totalBytes > kMaximumExtractedBytes) {
                        error = QStringLiteral("Extracted ZIP is larger than 8 GiB");
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool runTar(const QStringList& arguments, QByteArray& standardOutput,
                                  QString& error)
        {
            QString tar;
#ifdef Q_OS_WIN
            std::array<wchar_t, MAX_PATH + 1> systemDirectory{};
            const UINT length = GetSystemDirectoryW(
                systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
            if (length > 0 && length < systemDirectory.size()) {
                tar = QDir(QString::fromWCharArray(systemDirectory.data(), length))
                          .filePath(QStringLiteral("tar.exe"));
            }
#else
            tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
#endif
            if (tar.isEmpty() || !QFileInfo(tar).isFile()) {
                error = QStringLiteral("Windows tar.exe is required for ZIP input");
                return false;
            }

            QProcess process;
            process.setProgram(tar);
            process.setArguments(arguments);
            process.setProcessChannelMode(QProcess::SeparateChannels);
            process.start();
            if (!process.waitForStarted(10'000)) {
                error = QStringLiteral("Cannot start tar.exe: %1").arg(process.errorString());
                return false;
            }
            if (!process.waitForFinished(kProcessTimeoutMilliseconds)) {
                process.kill();
                process.waitForFinished();
                error = QStringLiteral("ZIP operation timed out");
                return false;
            }
            standardOutput = process.readAllStandardOutput();
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
                error = QStringLiteral("ZIP operation failed: %1")
                            .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
                return false;
            }
            return true;
        }
    }

    PreparedPackage PackageSource::prepare(const QString& path)
    {
        const QFileInfo info(path);
        if (!info.exists()) {
            return packageError(path, QStringLiteral("Package path does not exist"));
        }
        if (info.isDir()) {
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty()) {
                return packageError(path, QStringLiteral("Package folder cannot be resolved"));
            }
            return packageReady(canonical, canonical);
        }
        if (info.isFile() && info.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) == 0) {
            return prepareZip(info.canonicalFilePath());
        }
        if(info.isFile()&&info.fileName().endsWith(".reloadsources.json",Qt::CaseInsensitive)) {
            QFile file(info.absoluteFilePath());
            if(file.size()>65536||!file.open(QIODevice::ReadOnly))return packageError(path,"Cannot read source pack");
            const auto doc=QJsonDocument::fromJson(file.readAll()).object();const auto inputs=doc["plugins"].toArray();
            if(doc["format"]!="reload-maker-source-pack"||doc["version"]!=1||inputs.isEmpty()||inputs.size()>16)return packageError(path,"Invalid source pack");
            auto result=packageReady(info.absolutePath(),info.canonicalFilePath());
            for(const auto& input:inputs) {
                const QFileInfo plugin(QDir(info.absolutePath()).absoluteFilePath(input.toString()));
                const auto resolved=plugin.canonicalFilePath();
                if(!input.isString()||!plugin.isFile()||resolved.isEmpty()||!(plugin.suffix().compare("esp",Qt::CaseInsensitive)==0||plugin.suffix().compare("esm",Qt::CaseInsensitive)==0||plugin.suffix().compare("esl",Qt::CaseInsensitive)==0))return packageError(path,"Source pack plugin is missing or invalid");
                for(const auto& old:result.plugins)if(QFileInfo(old).fileName().compare(plugin.fileName(),Qt::CaseInsensitive)==0)return packageError(path,"Source pack repeats a plugin identity");
                result.plugins.append(resolved);if(!result.roots.contains(plugin.canonicalPath()))result.roots.append(plugin.canonicalPath());
            }
            return result;
        }
        return packageError(path, QStringLiteral("Select a mod folder, ZIP or .reloadsources.json source pack"));
    }

    PreparedPackage PackageSource::prepareZip(const QString& path)
    {
        const QFileInfo sourceInfo(path);
        if (sourceInfo.size() < 0 || sourceInfo.size() > kMaximumZipBytes) {
            return packageError(path, QStringLiteral("ZIP file is larger than 4 GiB"));
        }

        QString error;
        const QString digest = hashFile(path, error);
        if (digest.isEmpty()) {
            return packageError(path, error);
        }

        QByteArray listing;
        if (!runTar({ QStringLiteral("-tf"), path }, listing, error)) {
            return packageError(path, error);
        }
        const QList<QByteArray> rawEntries = listing.split('\n');
        qsizetype entryCount = 0;
        for (const QByteArray& rawEntry : rawEntries) {
            const QString entry = QString::fromUtf8(rawEntry).trimmed();
            if (entry.isEmpty()) {
                continue;
            }
            ++entryCount;
            if (entryCount > kMaximumEntries) {
                return packageError(
                    path, QStringLiteral("ZIP contains more than %1 entries").arg(kMaximumEntries));
            }
            QString reason;
            if (!safeArchiveEntry(entry, reason)) {
                return packageError(
                    path, QStringLiteral("Unsafe ZIP entry '%1': %2").arg(entry, reason));
            }
        }

        QByteArray verboseListing;
        if (!runTar({ QStringLiteral("-tvf"), path }, verboseListing, error)) {
            return packageError(path, error);
        }
        qsizetype describedEntryCount = 0;
        for (const QByteArray& rawDescription : verboseListing.split('\n')) {
            const QByteArray description = rawDescription.trimmed();
            if (description.isEmpty()) {
                continue;
            }
            ++describedEntryCount;
            const char entryType = description.front();
            if (entryType != '-' && entryType != 'd') {
                return packageError(
                    path, QStringLiteral("ZIP contains a link or unsupported special entry"));
            }
        }
        if (describedEntryCount != entryCount) {
            return packageError(path, QStringLiteral("ZIP entry metadata is inconsistent"));
        }

        const QString cacheBase = studioDataPath("local/cache") +
                                  QStringLiteral("/packages-v1");
        if (!QDir().mkpath(cacheBase)) {
            return packageError(path, QStringLiteral("Cannot create the package cache"));
        }
        const QString finalRoot = QDir(cacheBase).filePath(digest);
        if (QFileInfo(finalRoot).isDir()) {
            if (!extractedTreeIsSafe(finalRoot, error)) {
                return packageError(path, error);
            }
            return packageReady(finalRoot, path, true);
        }

        const QString temporaryRoot = QDir(cacheBase).filePath(
            QStringLiteral(".extract-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(temporaryRoot)) {
            return packageError(path, QStringLiteral("Cannot create a temporary extraction folder"));
        }

        QByteArray ignored;
        if (!runTar({ QStringLiteral("-xf"), path, QStringLiteral("-C"), temporaryRoot }, ignored, error) ||
            !extractedTreeIsSafe(temporaryRoot, error)) {
            QDir(temporaryRoot).removeRecursively();
            return packageError(path, error);
        }
        if (!QDir().rename(temporaryRoot, finalRoot)) {
            QDir(temporaryRoot).removeRecursively();
            return packageError(
                path, QStringLiteral("Cannot finalize the extracted package cache"));
        }
        return packageReady(finalRoot, path, true);
    }

    ResourceArchive::ResourceArchive() = default;
    ResourceArchive::~ResourceArchive() = default;
    ResourceArchive::ResourceArchive(ResourceArchive&&) noexcept = default;
    ResourceArchive& ResourceArchive::operator=(ResourceArchive&&) noexcept = default;

    bool ResourceArchive::open(const QString& rootPath, QString& error)
    {
        error.clear();
        close();
        try {
            const QByteArray nativePath = QFile::encodeName(QDir::toNativeSeparators(rootPath));
            _archive = std::make_unique<BA2File>(nativePath.constData());
            std::vector<std::string_view> entries;
            _archive->getFileList(entries);
            _files.reserve(static_cast<qsizetype>(entries.size()));
            for (const std::string_view entry : entries) {
                QString path = QString::fromUtf8(
                    entry.data(), static_cast<qsizetype>(entry.size()));
                path.replace('\\', '/');
                while (path.startsWith('/')) {
                    path.remove(0, 1);
                }
                _files.append(normalizeResourcePath(path));
            }
            if (QFileInfo(rootPath).isDir()) {
                // The renderer library's directory scan omits HKX. Keep an
                // explicit loose-file index, with loose data winning over BA2
                // for the same logical path regardless of file extension.
                const QDir root(rootPath);
                QDirIterator iterator(root.absolutePath(), QDir::Files,
                                      QDirIterator::Subdirectories);
                while (iterator.hasNext()) {
                    const QString physicalPath = iterator.next();
                    const QString key = normalizeResourcePath(root.relativeFilePath(physicalPath));
                    const auto existing = _looseFiles.constFind(key);
                    if (existing != _looseFiles.cend()) {
                        error = QStringLiteral("Ambiguous loose resource '%1': '%2' and '%3'")
                                    .arg(key, existing.value(), physicalPath);
                        close();
                        return false;
                    }
                    _looseFiles.insert(key, physicalPath);
                    _files.append(key);
                }
            }
            _files.sort(Qt::CaseInsensitive);
            _files.removeDuplicates();
            return true;
        } catch (const std::exception& exception) {
            error = QStringLiteral("Cannot index package resources: %1")
                        .arg(QString::fromUtf8(exception.what()));
            close();
            return false;
        }
    }

    bool ResourceArchive::openPackages(const QStringList& roots,QString& error) {
        close();error.clear();
        if(roots.isEmpty()||roots.size()>17){error="Invalid package layer count";return false;}
        for(const auto& root:roots) {
            auto layer=std::make_unique<ResourceArchive>();
            if(!layer->open(root,error)){close();return false;}
            for(const auto& file:layer->files()){_files.append(file);_providers.insert(file,root);}
            _layers.push_back(std::move(layer));
        }
        _files.sort();_files.removeDuplicates();return true;
    }

    bool ResourceArchive::openLoadOrder(const QString& dataPath, const QStringList& archives,
        QString& error, const std::function<void(const QString&, int, int)>& progress,
        const std::function<bool()>& cancelled)
    {
        close();
        error.clear();
        try {
            const auto check = [&]() {
                if (cancelled && cancelled()) { throw std::runtime_error("Cancelled"); }
            };
            struct IndexArchive final : BA2File {
                void add(const char* path, bool (*filter)(void*, const std::string_view&), void* context) {
                    // The backend retains its filter pointers. Do not let a
                    // stack indexing context survive into the GUI read phase.
                    const auto clearFilter = qScopeGuard([this]() {
                        fileFilterFunction = nullptr;
                        fileFilterFunctionData = nullptr;
                    });
                    loadArchivePath(path, filter, context);
                }
            };
            auto archive = std::make_unique<IndexArchive>();
            check();
            const QDir root(dataPath);
            if (!root.exists()) { throw std::runtime_error("Virtual Data folder is missing"); }
            if (progress) { progress(QStringLiteral("Indexing winning loose resources"), 0, 0); }
            QDirIterator iterator(root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
            int count = 0;
            while (iterator.hasNext()) {
                check();
                const QString path = iterator.next();
                const QString key = normalizeResourcePath(root.relativeFilePath(path));
                if (++count > 2'000'000) { throw std::runtime_error("Too many loose resources"); }
                // Top-level plugins and archives are catalog inputs, not render assets.
                if (!key.contains('/')) { continue; }
                if (_looseFiles.contains(key)) { throw std::runtime_error("Duplicate virtual resource path"); }
                _looseFiles.insert(key, path);
                if (progress && count % 2048 == 0) {
                    progress(QStringLiteral("Indexed %1 winning loose files").arg(count), 0, 0);
                }
            }
            struct IndexContext {
                ResourceArchive* owner;
                QString archivePath;
                const std::function<bool()>* cancelled;
            } context{this, {}, &cancelled};
            const auto filter = +[](void* opaque, const std::string_view& fileName) -> bool {
                auto& c = *static_cast<IndexContext*>(opaque);
                if (*c.cancelled && (*c.cancelled)()) { throw std::runtime_error("Cancelled"); }
                const QString key = QString::fromUtf8(fileName.data(), fileName.size());
                // BA2File keeps the first occurrence. Capture exactly that same winner.
                if (!c.owner->_providers.contains(key)) { c.owner->_providers.insert(key, c.archivePath); }
                return true;
            };
            for (int i = archives.size() - 1; i >= 0; --i) {
                check();
                context.archivePath = archives[i];
                if (progress) { progress(QStringLiteral("Indexing %1").arg(QFileInfo(archives[i]).fileName()), archives.size() - i, archives.size()); }
                const QByteArray native = QDir::toNativeSeparators(archives[i]).toLocal8Bit();
                archive->add(native.constData(), filter, &context);
            }
            _files = _providers.keys();
            _files.append(_looseFiles.keys());
            _files.sort();
            _files.removeDuplicates();
            _archive = std::move(archive);
            return true;
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            close();
            return false;
        }
    }

    QString ResourceArchive::provider(const QString& resourcePath) const
    {
        const auto key = normalizeResourcePath(resourcePath);
        const auto loose = _looseFiles.constFind(key);
        return loose != _looseFiles.cend() ? QStringLiteral("MO2 / loose: %1").arg(loose.value()) : _providers.value(key);
    }

    void ResourceArchive::close()
    {
        _files.clear();
        _looseFiles.clear();
        _providers.clear();
        _layers.clear();
        _archive.reset();
    }

    bool ResourceArchive::isOpen() const noexcept
    {
        return static_cast<bool>(_archive)||!_layers.empty();
    }

    QStringList ResourceArchive::filesWithSuffix(const QString& suffix) const
    {
        QString normalizedSuffix = suffix.toLower();
        if (!normalizedSuffix.startsWith('.')) {
            normalizedSuffix.prepend('.');
        }
        QStringList result;
        for (const QString& file : _files) {
            if (file.endsWith(normalizedSuffix, Qt::CaseInsensitive)) {
                result.append(file);
            }
        }
        return result;
    }

    bool ResourceArchive::contains(const QString& resourcePath) const
    {
        if(!_layers.empty())return _providers.contains(normalizeResourcePath(resourcePath));
        if (!_archive) {
            return false;
        }
        const QString key = normalizeResourcePath(resourcePath);
        if (_looseFiles.contains(key)) {
            return true;
        }
        const QByteArray normalized = key.toUtf8();
        return _archive->findFile(std::string_view(normalized.constData(), normalized.size())) != nullptr;
    }

    QByteArray ResourceArchive::read(const QString& resourcePath, QString& error) const
    {
        error.clear();
        if(!_layers.empty()) {
            for(auto i=_layers.rbegin();i!=_layers.rend();++i)if((*i)->contains(resourcePath))return (*i)->read(resourcePath,error);
            error="Resource is missing from the source pack: "+resourcePath;return {};
        }
        if (!_archive) {
            error = QStringLiteral("Resource archive is not open");
            return {};
        }
        const QString key = normalizeResourcePath(resourcePath);
        const auto loose = _looseFiles.constFind(key);
        if (loose != _looseFiles.cend()) {
            QFile file(loose.value());
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("Cannot read loose resource '%1': %2")
                            .arg(loose.value(), file.errorString());
                return {};
            }
            const qint64 expectedSize = file.size();
            if (expectedSize <= 0 || expectedSize > std::numeric_limits<int>::max()) {
                error = QStringLiteral("Loose resource is empty or too large: %1").arg(loose.value());
                return {};
            }
            QByteArray bytes = file.read(expectedSize);
            if (bytes.size() != expectedSize || file.error() != QFileDevice::NoError) {
                error = QStringLiteral("Cannot read complete loose resource '%1': %2")
                            .arg(loose.value(), file.errorString());
                return {};
            }
            return bytes;
        }
        const QByteArray normalized = key.toUtf8();
        try {
            BA2File::UCharArray storage;
            const unsigned char* data = nullptr;
            const size_t size = _archive->extractFile(
                data, storage, std::string_view(normalized.constData(), normalized.size()));
            if (!data || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
                error = QStringLiteral("Resource is empty or too large: %1").arg(resourcePath);
                return {};
            }
            return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(size));
        } catch (const std::exception& exception) {
            error = QStringLiteral("Cannot read '%1': %2")
                        .arg(resourcePath, QString::fromUtf8(exception.what()));
            return {};
        }
    }

    QString ResourceArchive::normalizeResourcePath(const QString& path)
    {
        QString result = path.trimmed().toLower();
        result.replace('\\', '/');
        while (result.startsWith('/')) {
            result.remove(0, 1);
        }
        return QDir::cleanPath(result);
    }
}
