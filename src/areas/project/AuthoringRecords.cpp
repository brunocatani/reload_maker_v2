// Authoring records: what a weapon's authoring session keeps per reload
// configuration (checklist confirmations, magazine guide/setup/reference,
// operating setup, collision drafts), the weapon's reload parts that key
// them, and the behaviour preset assigned to the weapon.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "commands/CommandSupport.h"

#include <algorithm>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        constexpr auto kWholeWeapon = "weapon";

        int roleOrder(const QJsonValue& value)
        {
            const auto role = value.toObject()["role"].toString();
            return role == QLatin1String("receiver") ? 0 : role == QLatin1String("operatingPart") ? 1 : 2;
        }
    }

    QString ReloadMakerController::assignedTemplate() const
    {
        return currentWeapon() ? _reloadTemplateAssignments[poseRecordKey(currentWeapon()->formId).toLower()].toString() : QString();
    }

    QString ReloadMakerController::configurationRoleName(ConfigurationRole role)
    {
        switch (role) {
        case ConfigurationRole::Receiver: return QStringLiteral("receiver");
        case ConfigurationRole::OperatingPart: return QStringLiteral("operatingPart");
        case ConfigurationRole::Magazine: break;
        }
        return QStringLiteral("magazine");
    }

    // "<weapon>/receiver=<record>;operatingPart=<record>;magazine=<record>",
    // from the weapon's recorded reload parts and the parts selected in them
    // ("none" for an empty slot). Every other part (barrel, grip, sights,
    // underbarrel, muzzle) leaves the key alone (owner, 2026-10-05). Before
    // Operate has recorded the parts, the weapon has one record, "<weapon>/weapon".
    QString ReloadMakerController::authoringRecordKey() const
    {
        if (!currentWeapon() || !configuration()) {
            return {};
        }
        const auto weapon = poseRecordKey(currentWeapon()->formId).toLower();
        const auto axes = _reloadAxes.value(weapon).toArray();
        if (axes.isEmpty()) {
            return weapon + '/' + kWholeWeapon;
        }
        const auto choices = configuration()->savedChoices(*currentPlugin());
        QStringList parts;
        for (const auto& value : axes) {
            const auto axis = value.toObject();
            const auto choice = choices.value(axis["slot"].toString()).toString();
            parts.append(axis["role"].toString() + '=' + (choice.isEmpty() ? QStringLiteral("none") : choice));
        }
        return weapon + '/' + parts.join(';');
    }

    // The reload parts Operate's rig shows: the receiver, the operating part
    // when its slot is separate, and the magazine, each once.
    ReloadMakerController::ReloadAxes ReloadMakerController::liveReloadAxes() const
    {
        ReloadAxes axes;
        if (!currentWeapon() || !configuration()) {
            return axes;
        }
        const auto slotOf = [this](const QString& record) -> SlotKey {
            for (const auto* mod : configuredObjectMods()) {
                if (poseRecordKey(mod->formId).compare(record, Qt::CaseInsensitive) == 0) {
                    return configurationSlotKey(*mod);
                }
            }
            return 0;
        };
        const auto add = [&](ConfigurationRole role, SlotKey slot) {
            if (slot && std::none_of(axes.cbegin(), axes.cend(), [&](const auto& axis) { return axis.second == slot; })) {
                axes.append({ role, slot });
            }
        };
        if (const auto* receiver = resolvedReceiverObjectMod()) {
            add(ConfigurationRole::Receiver, configurationSlotKey(*receiver));
        }
        if (const int anchor = operatingAnchor(); anchor >= 0) {
            add(ConfigurationRole::OperatingPart, slotOf(_handPoseAnchors[anchor].description.ownerRecord));
        }
        add(ConfigurationRole::Magazine, slotOf(simulationMagazineAnchor().ownerRecord));
        return axes;
    }

    // The recorded reload parts this configuration has, or Operate's live
    // reading before any are recorded.
    ReloadMakerController::ReloadAxes ReloadMakerController::reloadAxes() const
    {
        if (!currentWeapon() || !configuration()) {
            return {};
        }
        const auto recorded = _reloadAxes.value(poseRecordKey(currentWeapon()->formId).toLower()).toArray();
        if (recorded.isEmpty()) {
            return liveReloadAxes();
        }
        ReloadAxes axes;
        const auto& entries = configuration()->entries();
        for (const auto& value : recorded) {
            const auto axis = value.toObject();
            const auto slot = std::find_if(entries.cbegin(), entries.cend(), [&](const auto& entry) {
                return configuration()->slotId(*currentPlugin(), entry) == axis["slot"].toString();
            });
            if (slot == entries.cend()) {
                continue;
            }
            for (const auto role : { ConfigurationRole::Receiver, ConfigurationRole::OperatingPart, ConfigurationRole::Magazine }) {
                if (configurationRoleName(role) == axis["role"].toString()) {
                    axes.append({ role, slot->key });
                }
            }
        }
        return axes;
    }

    // Called when Operate's rig is ready. The weapon's first rig records its
    // reload parts. A later rig adds a part only when it was built from an
    // authored operating setup (the setup named the magazine or a separate
    // operating part), and never removes one, so a configuration Operate
    // reads from the preset alone cannot merge records. Records move to their
    // keys under the new parts; a part a record did not name takes this
    // configuration's choice.
    void ReloadMakerController::recordReloadAxes()
    {
        if (!currentWeapon() || !configuration()) {
            return;
        }
        const auto weapon = poseRecordKey(currentWeapon()->formId).toLower();
        const auto recorded = _reloadAxes.value(weapon).toArray();
        if (!recorded.isEmpty() && !_authoringRecords.value(authoringRecordKey()).toObject().contains("operating")) {
            return;
        }
        QJsonArray next = recorded;
        for (const auto& [role, slot] : liveReloadAxes()) {
            const auto* entry = configuration()->slot(slot);
            if (!entry) {
                continue;
            }
            const auto id = configuration()->slotId(*currentPlugin(), *entry);
            const auto name = configurationRoleName(role);
            if (std::none_of(next.cbegin(), next.cend(), [&](const QJsonValue& value) {
                    const auto axis = value.toObject();
                    return axis["role"].toString() == name || axis["slot"].toString() == id;
                })) {
                next.append(QJsonObject{ { "role", name }, { "slot", id } });
            }
        }
        if (next.size() == recorded.size()) {
            return;
        }
        QVector<QJsonValue> ordered(next.cbegin(), next.cend());
        std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonValue& a, const QJsonValue& b) { return roleOrder(a) < roleOrder(b); });
        next = {};
        for (const auto& value : ordered) {
            next.append(value);
        }
        const auto current = authoringRecordKey();
        const auto choices = configuration()->savedChoices(*currentPlugin());
        _reloadAxes[weapon] = next;
        const QString prefix = weapon + '/';
        QJsonObject moved;
        QStringList previous;
        for (auto it = _authoringRecords.constBegin(); it != _authoringRecords.constEnd(); ++it) {
            const auto suffix = it.key().mid(prefix.size());
            if (!it.key().startsWith(prefix) || (suffix != QLatin1String(kWholeWeapon) && !suffix.contains('='))) {
                continue;
            }
            QHash<QString, QString> named;
            if (suffix != QLatin1String(kWholeWeapon)) {
                for (const auto& part : suffix.split(';')) {
                    named.insert(part.section('=', 0, 0), part.section('=', 1));
                }
            }
            QStringList parts;
            for (const auto& value : next) {
                const auto axis = value.toObject();
                const auto role = axis["role"].toString();
                const auto choice = named.value(role, choices.value(axis["slot"].toString()).toString());
                parts.append(role + '=' + (choice.isEmpty() ? QStringLiteral("none") : choice));
            }
            const auto key = prefix + parts.join(';');
            if (!moved.contains(key) || it.key() == current) {
                moved.insert(key, it.value());
            }
            previous.append(it.key());
        }
        for (const auto& key : previous) {
            _authoringRecords.remove(key);
        }
        for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
            _authoringRecords.insert(it.key(), it.value());
        }
        _presetProposal = {};
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
}
