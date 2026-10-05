// Authoring records: what a weapon's authoring session keeps per assembled
// configuration (checklist confirmations, recorded source inputs, magazine
// guide/setup/reference, operating setup, collision drafts), and the
// behaviour preset assigned to the weapon.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "commands/CommandSupport.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace reloadmaker
{
    using commands::require;

    QString ReloadMakerController::assignedTemplate() const
    {
        return currentWeapon() ? _reloadTemplateAssignments[poseRecordKey(currentWeapon()->formId).toLower()].toString() : QString();
    }

    // "<weapon record>/<sha256 of the saved part choices>": one record per
    // assembled configuration of the weapon.
    QString ReloadMakerController::authoringRecordKey() const
    {
        if (!currentWeapon() || !configuration()) {
            return {};
        }
        const auto choices = configuration()->savedChoices(*currentPlugin());
        return poseRecordKey(currentWeapon()->formId).toLower() + "/" +
               QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(choices).toJson(QJsonDocument::Compact),
                                                            QCryptographicHash::Sha256)
                                       .toHex());
    }

    void ReloadMakerController::assignReloadPreset(const QString& id)
    {
        require(!authoringRecordKey().isEmpty(), "Load a weapon first");
        const auto mechanism = templateMechanism(id);
        require(mechanism.has_value(), "Choose one of the supported behavior templates");
        stopSimulation();
        _reloadTemplateAssignments[poseRecordKey(currentWeapon()->formId).toLower()] = id;
        _assignedMechanism = mechanism;
        ++_revision;
        prepareSimulation();
        require(saveSession(), "Could not save the assigned preset");
    }

    // Records the bytes of every source file the configuration was authored
    // from; a later audit compares them before export.
    void ReloadMakerController::recordSourceEvidence()
    {
        const auto key = authoringRecordKey();
        require(!key.isEmpty(), "Load a weapon first");
        const auto evidence = sourceEvidence();
        require(auditSources(evidence)["compatible"].toBool(),
                "Loaded authoring data differs from the current resource bytes. Reconnect/reload the source before recording it.");
        auto record = _authoringRecords.value(key).toObject();
        record["sources"] = evidence;
        _authoringRecords[key] = record;
        ++_revision;
    }
}
