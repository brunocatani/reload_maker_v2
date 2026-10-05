#pragma once

#include "EspReader.h"
#include "LoadOrderLibrary.h"
#include "LoadTimings.h"
#include "PackageSource.h"
#include "SoundProfile.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace reloadmaker
{
    // Everything a development source load produces and the controller
    // installs. A worker fills one; after the switch to another source the
    // installed one is kept resident in this form (performance work,
    // 2026-10-04) and installed again unchanged while its files are.
    struct PackageLoad
    {
        PreparedPackage package;
        ResourceArchive resources;
        ResourceArchive meshes;
        ResourceArchive animations;
        ResourceArchive sounds;
        QVector<PluginContent> plugins;
        QHash<QString, QString> referenceNames;          // ammunition and keyword names by lower-case record
        QHash<QString, SoundDescriptor> referenceSounds; // by lower-case record
        QVector<SoundMapping> referenceSoundMappings;
        QSet<QString> referenceSoundKeywords;            // lower-case records
        LoadTimings timings;                             // filled by the worker only
        QString data;
        QByteArray stamp;           // sourceStamp of sourceStampFiles when it was read
        bool reuseCurrent{ false }; // the installed source is unchanged; install it again
        QString error;
        bool cancelled{ false };
    };

    // The game Data folder for a folder that is either Data itself or the game root.
    [[nodiscard]] QString referenceDataFolder(const QString& dataPath);
    // Identity of a development source: its path and the game data it reads.
    [[nodiscard]] QString packageSourceKey(const QString& path, const QString& dataPath);
    // The files a development source was read from: the package, its plugins
    // and masters, archives beside them and the base game archives. Loose
    // files are listed when the source is read; "Reload source files" reads
    // everything again.
    [[nodiscard]] QStringList packageStampFiles(const PreparedPackage& package, const QVector<PluginContent>& plugins,
                                                const QString& data);
    // The enabled plugins and archives of an MO2 profile.
    [[nodiscard]] QStringList libraryStampFiles(const LoadOrderSnapshot& snapshot);
    // Path, size and modification time of each file; safe on worker threads.
    [[nodiscard]] QByteArray sourceStamp(const QStringList& files);
}
