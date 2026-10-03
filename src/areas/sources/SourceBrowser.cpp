// Source browser logic: which plugin the source bar has selected, which
// weapons it lists, and the projection MCP reads. The picker widgets in
// StudioSourceBrowser.cpp only render this state.
#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "StudioWindow.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>

namespace reloadmaker
{
    namespace
    {
        constexpr const char* kTestbedIds[] = {
            "glock", "ru556", "m1a", "timberwolf", "l118a2", "thanatos",
            "mk18", "mk18-empire", "srep-service", "srep-honeybadger", "srep-ar10", "srep-xm16e1",
        };
        constexpr qint64 kProjectSizeLimit = 64 * 1024 * 1024;
        constexpr qint64 kSourceDefinitionSizeLimit = 64 * 1024;

        // A source-pack testbed names its plugins in load order; the last one
        // is the weapon's own plugin.
        QString testbedPlugin(const QString& weapon, const QString& source)
        {
            QString plugin = weapon.section(':', 0, 0);
            if (!source.endsWith(".reloadsources.json", Qt::CaseInsensitive)) {
                return plugin;
            }
            QFile definition(source);
            if (definition.size() > kSourceDefinitionSizeLimit || !definition.open(QIODevice::ReadOnly)) {
                return plugin;
            }
            const QJsonArray inputs = QJsonDocument::fromJson(definition.readAll()).object()["plugins"].toArray();
            if (!inputs.isEmpty()) {
                plugin = QFileInfo(inputs.last().toString()).fileName();
            }
            return plugin;
        }
    }

    void ReloadMakerController::refreshDevelopmentSources()
    {
        _testbedSources = {};
        _testbedSourcesLoaded = true;
        QSettings settings;
        for (const char* id : kTestbedIds) {
            const QString project = settings.value("testbeds/" + QString(id), testbedProject(id)).toString();
            QFile file(project);
            if (file.size() > kProjectSizeLimit || !file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
            const QString weapon = document["weapon"].toString();
            if (document["library"].toBool() || weapon.isEmpty()) {
                continue;
            }
            const QString source = document["package"].toString();
            _testbedSources.append(QJsonObject{
                { "id", id },
                { "plugin", testbedPlugin(weapon, source) },
                { "weapon", weapon },
                { "project", project },
                { "source", source },
                { "name", document["weaponName"].toString(document["weaponEditorId"].toString(id)) },
            });
        }
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
        const int selection = _weaponCombo->currentIndex();
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

        // The open weapon does not come from the new plugin: save and close it.
        const bool weaponOutsidePlugin = !selectionPlugins(_weaponCombo->currentIndex()).contains(name, Qt::CaseInsensitive);
        if (currentWeapon() && !allWeaponsSelected() && weaponOutsidePlugin) {
            if (!saveSession()) {
                alignPluginToWeapon();
                return false;
            }
            {
                const QSignalBlocker block(_weaponCombo);
                _weaponCombo->setCurrentIndex(-1);
            }
            clearAssetCaches();
            _sessionPath.clear();
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
        if (!_libraryMode) {
            for (const auto& value : _testbedSources) {
                const QJsonObject testbed = value.toObject();
                const bool samePlugin = testbed["plugin"].toString().compare(_browserPlugin, Qt::CaseInsensitive) == 0;
                const bool sameWeapon = testbed["weapon"].toString().compare(selectionRecordKey(index), Qt::CaseInsensitive) == 0;
                if (samePlugin && sameWeapon) {
                    return selectTestbed(testbed["id"].toString());
                }
            }
        }
        _weaponCombo->setCurrentIndex(index);
        return true;
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
