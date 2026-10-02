#pragma once

#include "EspReader.h"
#include "PackageSource.h"
#include <functional>

namespace reloadmaker {
    using LibraryProgress = std::function<void(const QString&, int, int)>;
    using LibraryCancelled = std::function<bool()>;

    struct LoadOrderSnapshot {
        QString instancePath;
        QString profilePath;
        QString profileName;
        QString dataPath;
        QStringList plugins; // enabled, low to high priority
        QStringList archives; // explicit INI archives then enabled plugin archives
        QString language{"en"};
        QByteArray fingerprint;
        QString error;
        [[nodiscard]] bool valid() const { return error.isEmpty() && !plugins.isEmpty(); }
    };

    struct LibraryPlugin {
        QString name;
        int weapons{0};
        int objectMods{0};
        QString error;
    };

    struct LoadOrderLibrary {
        LoadOrderSnapshot snapshot;
        PluginContent catalog;
        QVector<LibraryPlugin> plugins;
        ResourceArchive resources;
        QString error;
        bool cancelled{false};
    };

    [[nodiscard]] bool insideMo2VirtualFilesystem();
    [[nodiscard]] LoadOrderSnapshot readLoadOrderSnapshot(const QString& instancePath,
                                                         const QString& dataPath,const QString& requestedProfile = {});
    [[nodiscard]] QByteArray profileFingerprint(const QString& instancePath, const QString& profilePath);
    [[nodiscard]] QString sourceLanguage(const QStringList& iniPaths);
    [[nodiscard]] QStringList resolveGameArchives(const QString& dataPath,const QStringList& plugins,const QStringList& iniPaths,QString& error);
    // The merger owns stable internal IDs, never guessed runtime FormIDs. A
    // source FormID is resolved using THAT plugin's MAST table, including ESLs.
    [[nodiscard]] PluginContent mergeLoadOrder(const QVector<PluginContent>& plugins, QString& error);
    [[nodiscard]] QVector<PluginContent> includePackageMasters(const QVector<PluginContent>& plugins,
        const QString& dataPath, const LibraryCancelled& cancelled, QString& error);
    [[nodiscard]] QStringList packageResourceLayers(const QVector<PluginContent>& plugins,const QStringList& roots);
    [[nodiscard]] LoadOrderLibrary loadOrderLibrary(const LoadOrderSnapshot& snapshot,
        const LibraryProgress& progress, const LibraryCancelled& cancelled);
}
