#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <functional>
#include <vector>

class BA2File;

namespace reloadmaker
{
    struct PreparedPackage
    {
        QString rootPath;
        QString sourcePath;
        QString error;
        bool fromArchive{ false };
        QStringList plugins; // Optional ordered source-pack inputs, masters first.
        QStringList roots;

        [[nodiscard]] bool ok() const noexcept { return error.isEmpty(); }
    };

    class PackageSource final
    {
    public:
        [[nodiscard]] static PreparedPackage prepare(const QString& path);

    private:
        [[nodiscard]] static PreparedPackage prepareZip(const QString& path);
    };

    class ResourceArchive final
    {
    public:
        ResourceArchive();
        ~ResourceArchive();

        ResourceArchive(const ResourceArchive&) = delete;
        ResourceArchive& operator=(const ResourceArchive&) = delete;
        ResourceArchive(ResourceArchive&&) noexcept;
        ResourceArchive& operator=(ResourceArchive&&) noexcept;

        [[nodiscard]] bool open(const QString& rootPath, QString& error);
        [[nodiscard]] bool openPackages(const QStringList& roots, QString& error);
        // Archives are in game load order (last wins); loose files are the
        // already-resolved MO2 virtual view, not physical mod directories.
        [[nodiscard]] bool openLoadOrder(const QString& dataPath, const QStringList& archives,
            QString& error, const std::function<void(const QString&, int, int)>& progress = {},
            const std::function<bool()>& cancelled = {});
        void close();

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] const QStringList& files() const noexcept { return _files; }
        [[nodiscard]] QStringList filesWithSuffix(const QString& suffix) const;
        [[nodiscard]] bool contains(const QString& resourcePath) const;
        [[nodiscard]] QByteArray read(const QString& resourcePath, QString& error) const;
        [[nodiscard]] QString provider(const QString& resourcePath) const;

        [[nodiscard]] static QString normalizeResourcePath(const QString& path);

    private:
        std::unique_ptr<BA2File> _archive;
        QHash<QString, QString> _looseFiles;
        QHash<QString, QString> _providers;
        QStringList _files;
        std::vector<std::unique_ptr<ResourceArchive>> _layers;
    };
}
