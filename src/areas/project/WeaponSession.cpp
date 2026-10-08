// Per-weapon session: where the author left each weapon (loadout, arms,
// camera, page, clip and hand). It is kept automatically in one local file
// per ESP, never enters a saved version or an export and never marks a
// weapon unsaved; Save and the reload build do not read it.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "areas/project/EspLibrary.h"
#include "commands/CommandSupport.h"

#include <QFile>
#include <QJsonDocument>

namespace reloadmaker
{
    namespace
    {
        constexpr qint64 kMaximumSessionBytes = 8 * 1024 * 1024;

        QJsonObject readSessions(const QString& path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumSessionBytes) {
                return {};
            }
            const auto document = QJsonDocument::fromJson(file.readAll()).object();
            return document["format"] == QLatin1String("reload-maker-weapon-sessions") ? document["weapons"].toObject() : QJsonObject{};
        }
    }

    QString ReloadMakerController::sessionFile(const QString& plugin) const
    {
        const auto key = espProjectKey(plugin);
        return key.isEmpty() ? QString() : studioDataPath("local/sessions/" + key + ".json");
    }

    QJsonObject ReloadMakerController::weaponSession(const QString& record) const
    {
        const auto path = sessionFile(record.section(':', 0, 0));
        return path.isEmpty() ? QJsonObject{} : readSessions(path)[record.toLower()].toObject();
    }

    // Writes the shown weapon's session when it changed since it was last
    // kept. Called before another weapon is shown, on a short timer and on
    // close; a failed write only loses where the author left off.
    void ReloadMakerController::keepWeaponSession()
    {
        if (!currentWeapon() || _loadThread || _openSettling) {
            return;
        }
        const auto record = weaponRecordKey();
        const auto session = sessionPart(projectRecord());
        if (record == _keptSessionWeapon && session == _keptSession) {
            return;
        }
        const auto path = sessionFile(record.section(':', 0, 0));
        if (path.isEmpty()) {
            return;
        }
        auto weapons = readSessions(path);
        weapons[record] = session;
        try {
            commands::writeFileAtomically(path, QJsonDocument(QJsonObject{ { "format", "reload-maker-weapon-sessions" },
                                                                           { "version", 1 },
                                                                           { "weapons", weapons } })
                                                    .toJson(QJsonDocument::Compact));
            _keptSessionWeapon = record;
            _keptSession = session;
        } catch (const std::exception& error) {
            appendDiagnostic("SESSION: cannot keep where this weapon was left: " + QString::fromUtf8(error.what()));
        }
    }
}
