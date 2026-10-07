#pragma once

#include <QJsonObject>
#include <QString>

// Read access to the ESP-owned project library. The bundled product tool
// (tools/product_library.py) is its only writer: saves, deletions, imports and
// exports all run there, so one implementation owns the storage format.
namespace reloadmaker
{
    // workspace/library/plugins
    [[nodiscard]] QString espLibraryRoot();
    // The project key: the originating plugin's lower-case filename, or empty
    // when `plugin` is not a plugin filename.
    [[nodiscard]] QString espProjectKey(const QString& plugin);
    [[nodiscard]] QString espProjectFolder(const QString& plugin);
    // project.json; an empty object (and no error) when the ESP has no saves.
    [[nodiscard]] QJsonObject readEspProject(const QString& plugin, QString& error);
    [[nodiscard]] QJsonObject readEspSnapshot(const QString& plugin, const QString& save, QString& error);
    // A records/<sha256>.json document of the project.
    [[nodiscard]] QJsonObject readEspRecord(const QString& plugin, const QString& reference, QString& error);
    // workspace/library/development-sources.json: where the developer's
    // testbeds and dev bench weapons load from. Empty in a consumer workspace.
    [[nodiscard]] QJsonObject readDevelopmentSources();
}
