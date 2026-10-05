// The weapon's detected properties (rework 14): what Maker read from the
// configuration's records, scene and clips, each with its evidence. Code
// reads these properties instead of weapon names.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "WeaponAmmunition.h"
#include "areas/properties/PropertyInputs.h"
#include "commands/CommandSupport.h"
#include "domain/detectors/PresetProposal.h"

#include <QJsonArray>

namespace reloadmaker
{
    namespace
    {
        QJsonArray eventsJson(const HkxClip& clip)
        {
            QJsonArray events;
            for (const auto& event : clip.annotations) {
                events.append(QJsonObject{ { "seconds", event.timeSeconds }, { "text", event.text } });
            }
            return events;
        }

        QJsonObject eventJson(float seconds, const QString& text)
        {
            return seconds < 0 ? QJsonObject{} : QJsonObject{ { "seconds", seconds }, { "event", text } };
        }
    }

    using commands::require;

    void ReloadMakerController::refreshPresetProposal()
    {
        _presetProposal = {};
        if (!currentWeapon() || !currentPlugin() || !configuration()) {
            return;
        }
        const auto ammunition = resolveWeaponAmmunition(*currentPlugin(), *currentWeapon(), *configuration(), _referenceNames);
        const auto proposal = proposePreset({ currentWeapon()->boltAction, ammunition.pistol }, reloadPresets());
        _presetProposal = { proposal.preset ? proposal.preset->id : QString(), proposal.evidence, proposal.problem };
    }

    const WeaponPreset* ReloadMakerController::proposedPreset() const
    {
        if (_presetProposal.preset.isEmpty()) {
            return nullptr;
        }
        for (const auto& preset : reloadPresets()) {
            if (preset.id == _presetProposal.preset) {
                return &preset;
            }
        }
        return nullptr;
    }

    WeaponOverrides ReloadMakerController::currentWeaponOverrides() const
    {
        return currentWeapon() ? _weaponOverrides.value(poseRecordKey(currentWeapon()->formId).toLower()) : WeaponOverrides{};
    }

    // The author's choice, else the preset's.
    InsertionPath ReloadMakerController::magazineInsertionPath() const
    {
        if (const auto chosen = currentWeaponOverrides().insertion) {
            return *chosen;
        }
        const auto* preset = checklistPreset();
        return preset ? preset->model.feed.insertion : InsertionPath::Straight;
    }

    // Sets or clears (null) one per-weapon property and prepares Operate again.
    void ReloadMakerController::setWeaponProperty(const QString& name, const QJsonValue& value)
    {
        require(currentWeapon() != nullptr, "Load a weapon first");
        const auto key = poseRecordKey(currentWeapon()->formId).toLower();
        auto overrides = _weaponOverrides.value(key);
        if (name == QLatin1String("insertion")) {
            if (value.isNull()) {
                overrides.insertion.reset();
            } else {
                overrides.insertion = insertionPathFromName(value.toString());
                require(overrides.insertion.has_value(), "Choose straight or native-curve");
            }
        } else {
            require(false, "Unknown weapon property");
        }
        stopSimulation();
        if (overrides.empty()) {
            _weaponOverrides.remove(key);
        } else {
            _weaponOverrides.insert(key, overrides);
        }
        noteDocumentChange("weapon property");
        ++_revision;
        prepareSimulation();
        require(saveSession(), "Could not save the weapon property");
    }

    // The native clips of the configuration with their events, read from the
    // sources on request, and the properties the code reads.
    QJsonObject ReloadMakerController::weaponPropertiesState() const
    {
        QJsonArray clips;
        if (_reloadRecipe) {
            clips.append(QJsonObject{ { "role", "reload" }, { "animation", _reloadRecipe->source["animation"].toString() },
                                      { "duration", _reloadClip.durationSeconds }, { "events", eventsJson(_reloadClip) } });
        }
        for (const auto& action : _animationActions) {
            if (action.family != ActionFamily::Fire && action.family != ActionFamily::Cycle) {
                continue;
            }
            QString error;
            const auto clip = HkxReader::readClip(readAnimationResource(action.resourcePath, error), error);
            clips.append(QJsonObject{ { "role", action.family == ActionFamily::Fire ? (action.automatic ? "fire-automatic" : "fire") : "cycle" },
                                      { "label", action.label }, { "animation", action.resourcePath }, { "duration", clip.durationSeconds },
                                      { "events", eventsJson(clip) }, { "error", clip.valid() ? QString() : error } });
        }
        QJsonObject result{ { "clips", clips } };
        if (currentWeapon()) {
            result["preset"] = QJsonObject{ { "assigned", assignedTemplate() }, { "proposed", _presetProposal.preset },
                                            { "evidence", _presetProposal.evidence }, { "problem", _presetProposal.problem } };
        }
        if (currentWeapon()) {
            const auto chosen = currentWeaponOverrides().insertion;
            result["insertion"] = QJsonObject{ { "value", insertionPathName(magazineInsertionPath()) },
                                               { "source", chosen ? "weapon" : checklistPreset() ? "preset" : "default" } };
        }
        if (_reloadRecipe) {
            const auto magazine = detectMagazineEvents(clipEvents(_reloadClip));
            result["magazineEvents"] = QJsonObject{ { "entry", eventJson(magazine.entrySeconds, magazine.entry) },
                                                    { "removal", eventJson(magazine.removalSeconds, magazine.removal) },
                                                    { "ambiguities", QJsonArray::fromStringList(magazine.ambiguities) } };
        }
        return result;
    }
}
