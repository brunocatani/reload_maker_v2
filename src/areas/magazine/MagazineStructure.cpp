// How the magazine is built into the weapon, read from the assembled scene
// rather than from weapon names (rework 14, items 4, 5 and 7).
#include "ReloadMakerController.h"
#include "areas/loadout/SceneTree.h"
#include "model/nifmodel.h"

namespace reloadmaker
{
    // A part attached inside the carrier bone is an installed magazine. With
    // none, the carrier's own model decides: the receiver (or the weapon,
    // without a receiver part) means the magazine is built into it; any other
    // model is a magazine that brings its carrier bone with it.
    ReloadMakerController::MagazineMountInfo ReloadMakerController::magazineMount(const QModelIndex& carrier) const
    {
        MagazineMountInfo info;
        auto* model = activeModel();
        if (!model || !carrier.isValid()) {
            info.evidence = QStringLiteral("No magazine carrier bone");
            return info;
        }
        const auto carrierName = model->get<QString>(carrier, "Name");
        const auto* config = configuration();
        for (auto it = _partRoots.cbegin(); it != _partRoots.cend(); ++it) {
            if (it.value().isValid() && it.value() != carrier && isWithinBlock(*model, it.value(), carrier)) {
                const auto* mod = config ? config->mod(config->selected(it.key())) : nullptr;
                info.mount = MagazineMount::Installed;
                info.evidence = QStringLiteral("%1 is attached inside the %2 bone").arg(mod ? mod->editorId : model->get<QString>(it.value(), "Name"), carrierName);
                return info;
            }
        }
        QString owner;
        for (const auto& anchor : _handPoseAnchors) {
            if (anchor.node == carrier) {
                owner = anchor.description.ownerRecord;
                break;
            }
        }
        const auto* receiver = resolvedReceiverObjectMod();
        const auto* weapon = currentWeapon();
        const QString receiverKey = receiver ? poseRecordKey(receiver->formId) : weapon ? poseRecordKey(weapon->formId) : QString();
        if (!owner.isEmpty() && owner.compare(receiverKey, Qt::CaseInsensitive) == 0) {
            info.mount = MagazineMount::BuiltIn;
            info.evidence = QStringLiteral("No magazine part attaches to the %1 bone; the %2 model holds the magazine geometry")
                                .arg(carrierName, receiver ? QStringLiteral("receiver") : QStringLiteral("weapon"));
        } else {
            info.evidence = QStringLiteral("The magazine model (%1) brings its own %2 bone").arg(owner.isEmpty() ? QStringLiteral("unknown record") : owner, carrierName);
        }
        return info;
    }

    // For the prepared rig's magazine carrier.
    bool ReloadMakerController::magazineBuiltIn() const
    {
        if (!_simulationRig || !_reloadRecipe || _simulationRig->magazine < 0 || _simulationRig->magazine >= _reloadTrackNodes.size()) {
            return false;
        }
        return magazineMount(_sceneNodes[_reloadTrackNodes[_simulationRig->magazine]]).mount == MagazineMount::BuiltIn;
    }

    PieceRoleProposal ReloadMakerController::magazinePieceProposal(const QModelIndex& shape) const
    {
        auto* model = activeModel();
        if (!model || !shape.isValid()) {
            return {};
        }
        const auto shader = model->getBlockIndex(model->getLink(shape, "Shader Property"));
        return proposeMagazinePieceRole(model->get<QString>(shape, "Name"), shader.isValid() ? model->get<QString>(shader, "Name") : QString());
    }
}
