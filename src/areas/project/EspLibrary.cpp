#include "areas/project/EspLibrary.h"
#include "StudioPaths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace reloadmaker
{
    namespace
    {
        constexpr qint64 kIndexLimit = 32 * 1024 * 1024;
        constexpr qint64 kRecordLimit = 256 * 1024 * 1024;

        QJsonObject readJson(const QString& path, qint64 limit, QString& error)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
                error = QStringLiteral("Cannot read %1").arg(QDir::toNativeSeparators(path));
                return {};
            }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                error = QStringLiteral("Damaged library file %1").arg(QDir::toNativeSeparators(path));
                return {};
            }
            return document.object();
        }
    }

    QString espLibraryRoot()
    {
        return studioDataPath(QStringLiteral("workspace/library/plugins"));
    }

    QString espProjectKey(const QString& plugin)
    {
        static const QRegularExpression name(QStringLiteral(R"(^[^:/\\\x00-\x1f<>"|?*]+\.(?:esm|esp|esl)$)"),
                                             QRegularExpression::CaseInsensitiveOption);
        return name.match(plugin).hasMatch() ? plugin.toLower() : QString();
    }

    QString espProjectFolder(const QString& plugin)
    {
        const auto key = espProjectKey(plugin);
        return key.isEmpty() ? QString() : QDir(espLibraryRoot()).filePath(key);
    }

    QJsonObject readEspProject(const QString& plugin, QString& error)
    {
        const auto folder = espProjectFolder(plugin);
        if (folder.isEmpty()) {
            error = QStringLiteral("Not a plugin filename: %1").arg(plugin);
            return {};
        }
        const auto path = QDir(folder).filePath(QStringLiteral("project.json"));
        if (!QFile::exists(path)) {
            return {};
        }
        auto project = readJson(path, kIndexLimit, error);
        if (error.isEmpty() && (project["format"] != QLatin1String("reload-maker-esp-project") || project["version"].toInt() != 1 ||
                                project["key"].toString() != espProjectKey(plugin))) {
            error = QStringLiteral("Unsupported ESP project index for %1").arg(plugin);
            return {};
        }
        return project;
    }

    QJsonObject readEspSnapshot(const QString& plugin, const QString& save, QString& error)
    {
        static const QRegularExpression id(QStringLiteral(R"(^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_\d{4,}$)"));
        if (!id.match(save).hasMatch()) {
            error = QStringLiteral("Invalid saved version %1").arg(save);
            return {};
        }
        auto snapshot = readJson(QDir(espProjectFolder(plugin)).filePath(QStringLiteral("saves/%1/snapshot.json").arg(save)), kIndexLimit, error);
        if (error.isEmpty() && (snapshot["format"] != QLatin1String("reload-maker-esp-snapshot") || snapshot["id"].toString() != save)) {
            error = QStringLiteral("Damaged saved version %1").arg(save);
            return {};
        }
        return snapshot;
    }

    QJsonObject readEspRecord(const QString& plugin, const QString& reference, QString& error)
    {
        static const QRegularExpression name(QStringLiteral(R"(^records/[0-9a-f]{64}\.json$)"));
        if (!name.match(reference).hasMatch()) {
            error = QStringLiteral("Invalid library record reference");
            return {};
        }
        return readJson(QDir(espProjectFolder(plugin)).filePath(reference), kRecordLimit, error);
    }

    QJsonObject readDevelopmentSources()
    {
        const auto path = studioDataPath(QStringLiteral("workspace/library/development-sources.json"));
        if (!QFile::exists(path)) {
            return {};
        }
        QString error;
        auto sources = readJson(path, 1024 * 1024, error);
        return sources["format"] == QLatin1String("reload-maker-development-sources") ? sources : QJsonObject{};
    }
}
