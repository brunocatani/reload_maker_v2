// Weapon progress for the weapon list. Each weapon project folder keeps a
// small status.json beside authoring.project.json with the checklist
// progress of each configuration, rewritten whenever the project is saved.
// Projects reach 17 MB, so the list reads these instead of opening projects.
// A save that cannot evaluate the checklist (reload not learned yet) keeps an
// evaluated entry, else records what its saved confirmations show. Projects
// saved before status files existed have authored work Maker cannot see
// without opening them, so they count as drafts until saved again.
#include "ReloadMakerController.h"
#include "AuthoringPaths.h"
#include "StudioPaths.h"
#include "domain/presets/WeaponPresets.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace reloadmaker
{
    namespace
    {
        constexpr char kStatusFormat[] = "reload-maker-weapon-status";

        QJsonObject readStatus(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
                return {};
            }
            const auto status = QJsonDocument::fromJson(file.readAll()).object();
            return status["format"] == QLatin1String(kStatusFormat) && status["version"].toInt() == 1 ? status : QJsonObject{};
        }

        WeaponProgress progressOf(const QJsonObject& status)
        {
            QVector<WeaponProgress> configurations;
            for (const auto& value : status["configurations"].toObject()) {
                WeaponProgress progress{};
                if (parseWeaponProgress(value.toObject()["progress"].toString(), progress)) {
                    configurations.append(progress);
                }
            }
            return combineWeaponProgress(configurations);
        }
    }

    void ReloadMakerController::writeWeaponStatus(const QString& projectPath)
    {
        // Only the weapon's own project, and only a checklist Maker evaluated.
        if (!currentWeapon() || QFileInfo(suggestedSessionPath()).absoluteFilePath().compare(projectPath, Qt::CaseInsensitive) != 0) {
            return;
        }
        const auto view = checklistView();
        if (!view.preset) {
            return;
        }
        const QString path = QFileInfo(projectPath).dir().filePath(QStringLiteral("status.json"));
        const auto previous = readStatus(path);
        auto configurations = previous["configurations"].toObject();
        const QString configuration = roadmapKey().section('/', 1);
        if (!view.evaluated && configurations[configuration].toObject()["evaluated"].toBool()) {
            return;
        }
        const auto& s = view.summary;
        configurations[configuration] = QJsonObject{ { "progress", weaponProgressName(weaponProgress(s)) },
                                                                    { "evaluated", view.evaluated },
                                                                    { "done", s.done },
                                                                    { "total", s.total },
                                                                    { "mandatory", s.mandatory },
                                                                    { "mandatoryDone", s.mandatoryDone },
                                                                    { "preset", view.preset->id },
                                                                    { "at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) } };
        const QJsonObject status{ { "format", kStatusFormat },
                                  { "version", 1 },
                                  { "weapon", poseRecordKey(currentWeapon()->formId) },
                                  { "configurations", configurations } };
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(status).toJson()) < 0 || !file.commit()) {
            qWarning("Cannot write the weapon status beside %s: %s", qPrintable(projectPath), qPrintable(file.errorString()));
            return;
        }
        if (previous.isEmpty() || progressOf(previous) != progressOf(status)) {
            refreshWeaponPicker();
        }
    }

    // Progress by lower-case "plugin:formid" for the loaded source's projects.
    QHash<QString, WeaponProgress> ReloadMakerController::weaponProgressIndex() const
    {
        QHash<QString, WeaponProgress> index;
        const QString context = authoringContextKey(sessionSourceContext());
        const QDir root(studioDataPath(QStringLiteral("workspace/projects")));
        QDirIterator files(root.absolutePath(), { QStringLiteral("authoring.project.json") }, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo project(files.next());
            const auto parts = root.relativeFilePath(project.filePath()).split('/');
            if (parts.size() != 4 || parts[2] != context) {
                continue;
            }
            const QString status = project.dir().filePath(QStringLiteral("status.json"));
            index.insert((parts[0] + ':' + parts[1]).toLower(), QFileInfo::exists(status) ? progressOf(readStatus(status)) : WeaponProgress::Draft);
        }
        return index;
    }
}
