// Source browser logic: which plugin the weapon picker has selected, which
// weapons it lists, and the projection MCP reads. The picker widgets in
// StudioSourceBrowser.cpp only render this state.
#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "ReloadTemplates.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "areas/project/EspLibrary.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QMenu>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>

namespace reloadmaker
{
    // The developer's testbeds and dev bench weapons: where each loads
    // from. A consumer workspace has none.
    void ReloadMakerController::refreshDevelopmentSources()
    {
        _testbedSources = readDevelopmentSources()["sources"].toArray();
        _testbedSourcesLoaded = true;
    }

    QStringList ReloadMakerController::selectionPlugins(int index) const
    {
        if (index < 0 || index >= _weaponSelections.size()) {
            return {};
        }
        const WeaponSelection& selection = _weaponSelections[index];
        const PluginContent& plugin = _plugins[selection.pluginIndex];
        const auto origin = plugin.origins.constFind(plugin.weapons[selection.weaponIndex].formId);
        if (origin != plugin.origins.cend() && !origin->contributors.isEmpty()) {
            return origin->contributors;
        }
        return { plugin.fileName };
    }

    bool ReloadMakerController::allWeaponsSelected() const
    {
        return _libraryMode && _browserPlugin == QLatin1String(kAllWeaponsSource);
    }

    QStringList ReloadMakerController::sourcePluginOrder() const
    {
        if (_libraryMode) {
            return _launchSnapshot.plugins;
        }
        QStringList result;
        const auto add = [&result](const QString& name) {
            if (!result.contains(name, Qt::CaseInsensitive)) {
                result.append(name);
            }
        };
        for (const PluginContent& plugin : _plugins) {
            if (plugin.sourcePlugins.isEmpty()) {
                add(plugin.fileName);
                continue;
            }
            for (const QString& name : plugin.sourcePlugins) {
                add(name);
            }
        }
        for (const auto& value : _testbedSources) {
            add(value.toObject()["plugin"].toString());
        }
        return result;
    }

    QStringList ReloadMakerController::browserPluginNames() const
    {
        QStringList names;
        if (_libraryMode) {
            names.append(kAllWeaponsSource);
        }
        names.append(sourcePluginOrder());
        return names;
    }

    QString ReloadMakerController::browserPluginEntry(const QString& name) const
    {
        for (const QString& entry : browserPluginNames()) {
            if (entry.compare(name, Qt::CaseInsensitive) == 0) {
                return entry;
            }
        }
        return {};
    }

    PluginDependencies ReloadMakerController::pluginDependencies() const
    {
        PluginDependencies dependencies;
        for (const PluginContent& plugin : _plugins) {
            dependencies.set(plugin.fileName, plugin.masters);
            for (auto it = plugin.sourceMasters.cbegin(); it != plugin.sourceMasters.cend(); ++it) {
                dependencies.set(it.key(), it.value());
            }
        }
        return dependencies;
    }

    QVector<SourcePluginRow> ReloadMakerController::sourcePluginRows() const
    {
        QHash<QString, int> counts;
        for (int index = 0; index < _weaponSelections.size(); ++index) {
            for (const QString& name : selectionPlugins(index)) {
                ++counts[name.toLower()];
            }
        }
        counts.insert(kAllWeaponsSource, int(_weaponSelections.size()));
        return buildSourcePluginRows(browserPluginNames(), _browserPlugin, pluginDependencies(), counts);
    }

    QVector<SourceWeaponRow> ReloadMakerController::sourceWeaponRows() const
    {
        if (_browserPlugin.isEmpty()) {
            return {};
        }
        const bool all = allWeaponsSelected();
        const auto progress = weaponProgressIndex();
        QVector<SourceWeaponRow> rows;
        for (int index = 0; index < _weaponSelections.size(); ++index) {
            if (!all && !selectionPlugins(index).contains(_browserPlugin, Qt::CaseInsensitive)) {
                continue;
            }
            const WeaponSelection& selection = _weaponSelections[index];
            const PluginContent& plugin = _plugins[selection.pluginIndex];
            const auto& weapon = plugin.weapons[selection.weaponIndex];
            const QString winner = plugin.origins.value(weapon.formId).winner;
            SourceWeaponRow row;
            row.index = index;
            row.name = weapon.name.isEmpty() ? weapon.editorId : weapon.name;
            row.editorId = weapon.editorId;
            row.record = selectionRecordKey(index);
            row.winner = winner.isEmpty() ? plugin.fileName : winner;
            row.coverage = coverageLabel(row.record);
            row.progress = weaponProgressName(progress.value(row.record.toLower(), WeaponProgress::NotStarted));
            rows.append(row);
        }
        return buildSourceWeaponRows(rows, all);
    }

    void ReloadMakerController::resolveBrowserPlugin()
    {
        // The selection survives a refresh of the same source; a different
        // source starts unselected unless a reopening project names its plugin.
        const QString key = _libraryMode ? "mo2:" + _launchSnapshot.instancePath + "/" + _launchSnapshot.profileName
                                         : "package:" + _package.sourcePath;
        QString chosen = _browserSourceKey == key ? _browserPlugin : QString();
        _browserSourceKey = key;
        if (!_pendingSession.isEmpty()) {
            const QString recordPlugin = _pendingSession["weapon"].toString().section(':', 0, 0);
            chosen = _pendingSession.value("browserPlugin").toString(recordPlugin);
        }
        _browserPlugin = browserPluginEntry(chosen);
        if (_browserPlugin.isEmpty()) {
            alignBrowserPlugin();
        }
    }

    void ReloadMakerController::alignBrowserPlugin()
    {
        if (allWeaponsSelected()) {
            return;
        }
        const int selection = _selectedWeapon;
        const QStringList allowed = selectionPlugins(selection);
        if (allowed.isEmpty() || allowed.contains(_browserPlugin, Qt::CaseInsensitive)) {
            return;
        }
        const QString origin = selectionRecordKey(selection).section(':', 0, 0);
        _browserPlugin = browserPluginEntry(allowed.contains(origin, Qt::CaseInsensitive) ? origin : allowed.first());
    }

    void ReloadMakerController::alignPluginToWeapon()
    {
        alignBrowserPlugin();
        renderPluginPicker();
    }

    bool ReloadMakerController::selectSourceMode(const QString& mode)
    {
        if (_loadThread || _reloadCapturing || _packProcess) {
            setStatus("Finish the current source or authoring job first.", true);
            return false;
        }
        if (mode == "dynamic") {
            const InstallationCheck check = checkInstallation(loadInstallationSetup());
            if (!check.gameReady || !check.profileReady) {
                showSetup();
                return false;
            }
            return connectInstalledProfile();
        }
        if (mode != "development") {
            setStatus("Choose dynamic or development source.", true);
            return false;
        }
        refreshDevelopmentSources();
        if (!_libraryMode && !_package.sourcePath.isEmpty()) {
            refreshSourceBar();
            return true;
        }
        if (_testbedSources.isEmpty()) {
            setStatus("No saved testbeds in this workspace. Use MENU > Source to open a plugin or mod folder.", true);
            return false;
        }
        return selectTestbed(_testbedSources.first().toObject()["id"].toString());
    }

    bool ReloadMakerController::selectSourcePlugin(const QString& name)
    {
        if (_loadThread || _reloadCapturing || _packProcess || _rebuildTimer->isActive()) {
            alignPluginToWeapon();
            setStatus("Wait for the current source or capture.", true);
            return false;
        }
        const QString entry = browserPluginEntry(name);
        if (entry.isEmpty()) {
            renderPluginPicker(); // the picker may already show the rejected choice
            setStatus("Choose a plugin from this source.", true);
            return false;
        }
        _browserPlugin = entry;
        renderPluginPicker();
        refreshWeaponPicker();

        // A development plugin that belongs to another testbed's source opens
        // that testbed instead of filtering this one.
        if (!_libraryMode) {
            for (const auto& value : _testbedSources) {
                const QJsonObject testbed = value.toObject();
                const bool samePlugin = testbed["plugin"].toString().compare(name, Qt::CaseInsensitive) == 0;
                const bool otherSource = QDir::cleanPath(testbed["source"].toString())
                                             .compare(QDir::cleanPath(_package.sourcePath), Qt::CaseInsensitive) != 0;
                if (!samePlugin || !otherSource) {
                    continue;
                }
                const bool started = selectTestbed(testbed["id"].toString());
                if (started && !_pendingSession.isEmpty()) {
                    _pendingSession["browserPlugin"] = name;
                }
                return started;
            }
        }

        // The open weapon does not come from the new plugin: leave and close it.
        const bool weaponOutsidePlugin = !selectionPlugins(_selectedWeapon).contains(name, Qt::CaseInsensitive);
        if (currentWeapon() && !allWeaponsSelected() && weaponOutsidePlugin) {
            if (!leaveDocument(QStringLiteral("changing plugins"))) {
                alignPluginToWeapon();
                return false;
            }
            _selectedWeapon = -1;
            clearAssetCaches();
            _esp.reset();
            refreshAttachments();
        }
        refreshWeaponPicker();
        ++_revision;
        return true;
    }

    bool ReloadMakerController::selectSourceWeapon(int index)
    {
        const bool busy = _loadThread || _reloadCapturing || _packProcess;
        const bool listed = index >= 0 && index < _weaponSelections.size() &&
                            (allWeaponsSelected() || selectionPlugins(index).contains(_browserPlugin, Qt::CaseInsensitive));
        if (busy || !listed) {
            setStatus("Choose a weapon from the selected plugin.", true);
            return false;
        }
        selectWeapon(index);
        return _loadThread || _lastError.isEmpty();
    }

    SourceBrowserProjection ReloadMakerController::sourceBrowserProjection() const
    {
        SourceBrowserProjection projection;
        projection.library = _libraryMode;
        projection.plugin = _browserPlugin;
        projection.plugins = sourcePluginRows();
        projection.weapons = sourceWeaponRows();
        projection.developmentSources = _testbedSources;
        if (const auto* menu = _window->findChild<QMenu*>("MainMenu")) {
            for (const QAction* action : menu->actions()) {
                if (!action->isSeparator()) {
                    projection.menu.append(action->text());
                }
            }
        }
        return projection;
    }

    QJsonObject ReloadMakerController::sourceBrowserState() const
    {
        return sourceBrowserProjection().toJson();
    }
}
