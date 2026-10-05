#include "areas/sources/SourceBrowserRows.h"

#include <QSet>

#include <algorithm>

namespace reloadmaker
{
    void PluginDependencies::set(const QString& plugin, const QStringList& masters)
    {
        _direct.insert(plugin.toLower(), masters);
    }

    QStringList PluginDependencies::masters(const QString& plugin) const
    {
        QStringList result;
        QStringList remaining = _direct.value(plugin.toLower());
        QSet<QString> seen{ plugin.toLower() };
        while (!remaining.isEmpty() && seen.size() < 256) {
            const QString master = remaining.takeFirst();
            if (seen.contains(master.toLower())) {
                continue;
            }
            seen.insert(master.toLower());
            result.append(master);
            remaining.append(_direct.value(master.toLower()));
        }
        return result;
    }

    QVector<SourcePluginRow> buildSourcePluginRows(const QStringList& names, const QString& selected,
                                                   const PluginDependencies& dependencies,
                                                   const QHash<QString, int>& weaponCounts)
    {
        const QStringList selectedMasters = dependencies.masters(selected);
        QVector<SourcePluginRow> rows;
        rows.reserve(names.size());
        for (const QString& name : names) {
            SourcePluginRow row;
            row.name = name;
            row.masters = dependencies.masters(name);
            row.weaponCount = weaponCounts.value(name.toLower());
            if (name.compare(selected, Qt::CaseInsensitive) == 0) {
                row.role = SourcePluginRole::Selected;
            } else if (selectedMasters.contains(name, Qt::CaseInsensitive)) {
                row.role = SourcePluginRole::Master;
            }
            if (name == QLatin1String(kAllWeaponsSource)) {
                row.label = QStringLiteral("Load all weapons");
            } else if (row.role == SourcePluginRole::Master) {
                row.label = name + QStringLiteral("  · master");
            } else {
                row.label = name;
            }
            rows.append(row);
        }
        return rows;
    }

    QVector<SourceWeaponRow> buildSourceWeaponRows(QVector<SourceWeaponRow> weapons, bool allWeapons)
    {
        for (SourceWeaponRow& weapon : weapons) {
            weapon.label = weapon.name;
            if (allWeapons) {
                weapon.label += QStringLiteral(" · ") + weapon.record.section(':', 0, 0);
            }
            if (!weapon.coverage.isEmpty()) {
                weapon.label += QStringLiteral(" · ") + weapon.coverage;
            }
        }
        // Same order as sorting the picker's item model: stable and case-sensitive.
        std::stable_sort(weapons.begin(), weapons.end(), [](const SourceWeaponRow& left, const SourceWeaponRow& right) {
            return left.label.compare(right.label, Qt::CaseSensitive) < 0;
        });
        return weapons;
    }

    QString sourcePluginRoleName(SourcePluginRole role)
    {
        switch (role) {
        case SourcePluginRole::Selected:
            return QStringLiteral("selected");
        case SourcePluginRole::Master:
            return QStringLiteral("master");
        case SourcePluginRole::Available:
            break;
        }
        return QStringLiteral("available");
    }

    QJsonObject SourceBrowserProjection::toJson() const
    {
        QJsonArray pluginRows;
        for (const SourcePluginRow& row : plugins) {
            pluginRows.append(QJsonObject{
                { "name", row.name },
                { "label", row.label },
                { "role", sourcePluginRoleName(row.role) },
                { "masters", QJsonArray::fromStringList(row.masters) },
            });
        }
        QJsonArray weaponRows;
        for (const SourceWeaponRow& row : weapons) {
            weaponRows.append(QJsonObject{
                { "index", row.index },
                { "name", row.label },
                { "record", row.record },
                { "progress", row.progress },
            });
        }
        return {
            { "mode", library ? "dynamic" : "development" },
            { "plugin", plugin },
            { "plugins", pluginRows },
            { "weapons", weaponRows },
            { "menu", QJsonArray::fromStringList(menu) },
            { "developmentSources", developmentSources },
        };
    }
}
