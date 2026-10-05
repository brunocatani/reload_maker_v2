#include "domain/detectors/PresetProposal.h"

#include <functional>

namespace reloadmaker
{
    namespace
    {
        // The one preset that satisfies `matches`, or nothing when none or
        // several do.
        const WeaponPreset* single(const QVector<WeaponPreset>& presets, const std::function<bool(const WeaponModel&)>& matches)
        {
            const WeaponPreset* found = nullptr;
            for (const auto& preset : presets) {
                if (matches(preset.model)) {
                    if (found) {
                        return nullptr;
                    }
                    found = &preset;
                }
            }
            return found;
        }

        bool hasSlide(const WeaponModel& model)
        {
            for (const auto& mechanism : model.mechanisms) {
                if (mechanism.kind == MechanismKind::Slide) {
                    return true;
                }
            }
            return false;
        }
    }

    PresetProposal proposePreset(const ActionEvidence& evidence, const QVector<WeaponPreset>& presets)
    {
        PresetProposal proposal;
        if (evidence.boltAction) {
            proposal.preset = single(presets, [](const WeaponModel& model) { return model.fire.afterShot == AfterShot::ManualCycle; });
            proposal.evidence = QStringLiteral("The weapon record marks a bolt action, cycled by hand after each shot.");
        } else if (evidence.pistol) {
            proposal.preset = single(presets, hasSlide);
            proposal.evidence = QStringLiteral("A pistol keyword selects pistol animations; pistols cycle a slide.");
        } else {
            proposal.problem = QStringLiteral("The records do not tell which action this rifle has. Choose the preset that matches how it cycles.");
            return proposal;
        }
        if (!proposal.preset) {
            proposal.problem = QStringLiteral("No single bundled preset matches: ") + proposal.evidence;
            proposal.evidence.clear();
        }
        return proposal;
    }
}
