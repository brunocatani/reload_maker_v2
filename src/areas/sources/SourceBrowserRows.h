#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace reloadmaker
{
    // Browser entry that lists every editable weapon of an MO2 profile.
    inline constexpr const char* kAllWeaponsSource = "*";

    // Masters each plugin needs, resolved transitively. Built once per browser
    // refresh so every row resolves against the same table.
    class PluginDependencies
    {
    public:
        // A later entry for the same plugin replaces the earlier one.
        void set(const QString& plugin, const QStringList& masters);

        // Breadth-first, each master once, never the plugin itself. Resolution
        // stops at 256 plugins so a corrupt master cycle cannot run away.
        [[nodiscard]] QStringList masters(const QString& plugin) const;

    private:
        QHash<QString, QStringList> _direct; // keyed by lower-case file name
    };

    enum class SourcePluginRole
    {
        Selected,
        Master,     // required by the selected plugin
        Available,
    };

    struct SourcePluginRow
    {
        QString name;   // plugin file name, or kAllWeaponsSource
        QString label;
        SourcePluginRole role{ SourcePluginRole::Available };
        QStringList masters;
        int weaponCount{ 0 }; // editable weapon records this plugin contributes to
    };

    struct SourceWeaponRow
    {
        int index{ -1 };    // weapon selection index
        QString name;       // display name, or the editor id when unnamed
        QString editorId;
        QString record;     // "Plugin.esp:000F9B"
        QString winner;     // plugin whose record wins the load order
        QString coverage;   // authored-coverage note, may be empty
        QString progress;   // checklist progress: not-started, draft or complete
        QString label;      // filled by buildSourceWeaponRows
    };

    // `names` is the browser order. Roles and the master suffix are relative
    // to `selected`; `weaponCounts` is keyed by lower-case plugin name.
    [[nodiscard]] QVector<SourcePluginRow> buildSourcePluginRows(const QStringList& names, const QString& selected,
                                                                 const PluginDependencies& dependencies,
                                                                 const QHash<QString, int>& weaponCounts);

    // Labels the weapons of one browser entry and sorts them by label the way
    // the weapon picker lists them. Across all plugins each label names the
    // record's plugin, since names repeat between mods.
    [[nodiscard]] QVector<SourceWeaponRow> buildSourceWeaponRows(QVector<SourceWeaponRow> weapons, bool allWeapons);

    [[nodiscard]] QString sourcePluginRoleName(SourcePluginRole role);

    // What the source bar shows; MCP reads the same rows the pickers list.
    struct SourceBrowserProjection
    {
        bool library{ false };  // an MO2 profile, otherwise a development source
        QString plugin;         // selected browser entry, empty when none
        QVector<SourcePluginRow> plugins;
        QVector<SourceWeaponRow> weapons;
        QStringList menu;
        QJsonArray developmentSources;

        [[nodiscard]] QJsonObject toJson() const;
    };
}
