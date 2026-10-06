#pragma once

#include "PoseEvaluator.h"

namespace reloadmaker
{
    // A connect-point offset is expressed in its named anchor's local frame.
    // If the incoming model supplies that anchor, relocate the root without
    // introducing a second animated copy of the anchor above it.
    [[nodiscard]] bool suppliedAnchorPlacement(const Transformf& authoredRoot,
                                                const Transformf& anchorInMount,
                                                const Transformf& ownerInMount, const Transformf& connection,
                                                Transformf& rootPlacement);
    // Immutable assembly snapshot. Node indices are assembly-scoped, not names;
    // the adapter assigns boneIndex only to nodes actually driven by this rig.
    struct ScenePoseNode
    {
        int parent{ -1 };
        int boneIndex{ -1 };
        Transformf restLocal{};
        // An attachment's explicitly supplied connection bone can align its
        // sibling animation branches. -1 keeps this node's own state alignment.
        int alignmentNode{ -1 };
    };

    struct ScenePoseBinding
    {
        int nodeIndex{ -1 };
        int boneIndex{ -1 };
        int semanticParentBinding{ -1 };
        int sceneParentBinding{ -1 };
        bool weaponPart{ false };
        Transformf restInBoneParent{};
        Transformf sceneParentBridge{};
        int alignmentBoneIndex{ -1 };
    };

    // Canonical node indices identify semantic parents when a part is attached
    // under a different physical branch. Output is ordered by skeleton bone.
    [[nodiscard]] bool compileSceneBindings(const HkxSkeleton& skeleton, int weaponBone,
                                            const QVector<ScenePoseNode>& nodes,
                                            const QVector<int>& canonicalNodes,
                                            QVector<ScenePoseBinding>& bindings, QString& error);
    [[nodiscard]] bool evaluateSceneBindings(const HkxSkeleton& skeleton,
                                             const QVector<ScenePoseBinding>& bindings,
                                             const QVector<Transformf>& sourceLocal,
                                             const QVector<Transformf>& stateLocal,
                                             const QVector<Transformf>& posedGlobals,
                                             QVector<Transformf>& boundLocal,
                                             QVector<Transformf>& boundGlobal, QString& error);
}
