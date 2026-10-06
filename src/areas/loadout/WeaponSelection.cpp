// Which weapon and arms body are being authored. Both are document fields:
// the weapon picker, the coverage dialog and MCP change the weapon through
// selectWeapon(); session restores assign the fields directly.
#include "ReloadMakerController.h"
#include "areas/loadout/LoadoutNames.h"

#include <QComboBox>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QTimer>

namespace reloadmaker
{
    int ReloadMakerController::validWeaponIndex(int index) const
    {
        return index >= 0 && index < _weaponSelections.size() ? index : -1;
    }

    void ReloadMakerController::selectWeapon(int index)
    {
        index = validWeaponIndex(index);
        if (index == _selectedWeapon) {
            return;
        }
        _selectedWeapon = index;
        alignPluginToWeapon();
        // Switching to a weapon with its own saved project reopens that
        // project instead of reassembling here.
        if (!beginWeaponProject(index)) {
            refreshWeaponPicker();
            refreshAttachments();
            scheduleRebuild();
            markDocumentClean();
        }
        noteDocumentChange("weapon selection");
    }

    bool ReloadMakerController::isArmsBody(const QString& body)
    {
        return body == QLatin1String("male") || body == QLatin1String("female");
    }

    // The caller reassembles the weapon with the new arms.
    void ReloadMakerController::setArmsBody(const QString& body)
    {
        if (body == _armsBody) {
            return;
        }
        _armsBody = body;
        showArmsBody();
        noteDocumentChange("arms body");
    }

    void ReloadMakerController::showArmsBody()
    {
        if (!_bodyCombo) {
            return;
        }
        const QSignalBlocker block(_bodyCombo);
        _bodyCombo->setCurrentIndex(_bodyCombo->findData(_armsBody));
    }

    // Lists the source's authorable weapons. Excluded records are counted by
    // reason; the first 512 are kept for the diagnostics report.
    void ReloadMakerController::refreshWeapons()
    {
        constexpr qsizetype kReportedExclusions = 512;
        // Source -> plugin -> weapon: a new list never selects an unrelated first record.
        _selectedWeapon = -1;
        _weaponSelections.clear();
        _configurations.clear();
        QJsonArray excluded;
        QJsonObject reasons;
        int total = 0;
        int hidden = 0;
        for (int pluginIndex = 0; pluginIndex < _plugins.size(); ++pluginIndex) {
            const PluginContent& plugin = _plugins.at(pluginIndex);
            for (int weaponIndex = 0; weaponIndex < plugin.weapons.size(); ++weaponIndex) {
                const WeaponRecord& weapon = plugin.weapons.at(weaponIndex);
                ++total;
                QString reason = weaponAuthoringExclusion(weapon);
                const QString model = normalizeNifPath(weapon.baseModelPath);
                if (reason.isEmpty() && !_packageResources.contains(model) && !_gameMeshes.contains(model)) {
                    reason = "Model not available in this source";
                }
                if (!reason.isEmpty()) {
                    ++hidden;
                    reasons[reason] = reasons[reason].toInt() + 1;
                    if (excluded.size() < kReportedExclusions) {
                        excluded.append(QJsonObject{
                            { "record", recordIdentity(plugin, weapon.formId) },
                            { "editorId", weapon.editorId },
                            { "name", weapon.name },
                            { "reason", reason },
                            { "model", model },
                        });
                    }
                    continue;
                }
                _weaponSelections.append(WeaponSelection{ pluginIndex, weaponIndex });
            }
        }
        _weaponLibraryReport = {
            { "total", total },
            { "shown", _weaponSelections.size() },
            { "hidden", hidden },
            { "reasons", reasons },
            { "excluded", excluded },
            { "excludedTruncated", hidden > excluded.size() },
        };
        QStringList counts;
        for (auto it = reasons.begin(); it != reasons.end(); ++it) {
            counts.append(it.key() + "=" + QString::number(it.value().toInt()));
        }
        appendDiagnostic(QString("WEAPON LIBRARY: %1 shown / %2 records; %3 excluded. %4. Playable melee retained.")
                             .arg(_weaponSelections.size())
                             .arg(total)
                             .arg(hidden)
                             .arg(counts.join("; ")));
        refreshAttachments();
        updateSimulationControls();
    }

    WeaponConfiguration* ReloadMakerController::configuration()
    {
        return const_cast<WeaponConfiguration*>(std::as_const(*this).configuration());
    }

    const WeaponConfiguration* ReloadMakerController::configuration() const
    {
        const int index = _selectedWeapon;
        if (index < 0) { return nullptr; }
        auto found = _configurations.find(index);
        if (found == _configurations.end()) {
            const auto* plugin = currentPlugin();
            const auto* weapon = currentWeapon();
            if (!plugin || !weapon) { return nullptr; }
            WeaponConfiguration config;
            config.initialize(*plugin, *weapon);
            found = _configurations.insert(index, std::move(config));
        }
        return &found.value();
    }

    void ReloadMakerController::scheduleRebuild()
    {
        _rebuildTimer->start();
        updateHandPoseControls();
        setStatus(QStringLiteral("Updating selected configuration…"));
    }

    ReloadMakerController::WeaponSelection ReloadMakerController::currentWeaponSelection() const
    {
        return _selectedWeapon >= 0 && _selectedWeapon < _weaponSelections.size()
            ? _weaponSelections.at(_selectedWeapon)
            : WeaponSelection{};
    }

    const PluginContent* ReloadMakerController::currentPlugin() const
    {
        const WeaponSelection selection = currentWeaponSelection();
        return selection.pluginIndex >= 0 && selection.pluginIndex < _plugins.size()
            ? &_plugins.at(selection.pluginIndex)
            : nullptr;
    }

    const WeaponRecord* ReloadMakerController::currentWeapon() const
    {
        const WeaponSelection selection = currentWeaponSelection();
        const PluginContent* plugin = currentPlugin();
        return plugin && selection.weaponIndex >= 0 && selection.weaponIndex < plugin->weapons.size()
            ? &plugin->weapons.at(selection.weaponIndex)
            : nullptr;
    }

    const ObjectModRecord* ReloadMakerController::resolvedReceiverObjectMod() const
    {
        for (const auto* mod : configuredObjectMods()) {
            if (isReceiverObjectMod(*mod)) { return mod; }
        }
        return nullptr;
    }

    // The installed modelled part that brings the WeaponMagazine bone along
    // with other animated bones: the body the reload parts hang on. A mod that
    // names no receiver (Project 1911: frame, slide, magazine; the slide
    // carries WeaponBolt and WeaponMagazine) gets its reload body from this
    // structure. A magazine model that brings its own WeaponMagazine* bones
    // (the 1911's Mag does) brings nothing else, so it is never the body.
    // Null when the weapon's own model brings the bones or no installed part
    // does. Read from the hand anchors only, so the magazine choice may
    // depend on it without a cycle.
    const ObjectModRecord* ReloadMakerController::reloadHostObjectMod() const
    {
        const auto* weapon = currentWeapon();
        if (!weapon) { return nullptr; }
        const QString weaponKey = poseRecordKey(weapon->formId);
        QStringList owners;
        QHash<QString, bool> magazineBone;
        QHash<QString, bool> otherBone;
        for (const auto& anchor : _handPoseAnchors) {
            const auto& placement = anchor.description;
            if (placement.anchorKind != QLatin1String("animated-part") || placement.ownerRecord.isEmpty() ||
                placement.ownerRecord.compare(weaponKey, Qt::CaseInsensitive) == 0) {
                continue;
            }
            const QString owner = placement.ownerRecord.toLower();
            if (!owners.contains(owner)) { owners.append(owner); }
            if (placement.anchorName.compare(QLatin1String("WeaponMagazine"), Qt::CaseInsensitive) == 0) {
                magazineBone[owner] = true;
            } else if (!placement.anchorName.startsWith(QLatin1String("WeaponMagazine"), Qt::CaseInsensitive)) {
                otherBone[owner] = true;
            }
        }
        for (const QString& owner : owners) {
            if (!magazineBone.value(owner) || !otherBone.value(owner)) { continue; }
            for (const auto* mod : configuredObjectMods()) {
                if (mod && !mod->modelPath.isEmpty() && poseRecordKey(mod->formId).compare(owner, Qt::CaseInsensitive) == 0) {
                    return mod;
                }
            }
        }
        return nullptr;
    }

    // The receiver the authoring axes and the export key: the named one,
    // else the reload body.
    const ObjectModRecord* ReloadMakerController::reloadReceiverObjectMod() const
    {
        if (const auto* receiver = resolvedReceiverObjectMod()) { return receiver; }
        return reloadHostObjectMod();
    }

    QVector<const ObjectModRecord*> ReloadMakerController::configuredObjectMods() const
    {
        QVector<const ObjectModRecord*> result;
        const auto* config = configuration();
        const auto* plugin = currentPlugin();
        if (!config || !plugin) { return result; }
        for (auto id : config->activeMods()) {
            if (const auto* mod = plugin->findObjectMod(id)) { result.append(mod); }
        }
        return result;
    }
}
