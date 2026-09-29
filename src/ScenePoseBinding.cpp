#include "ScenePoseBinding.h"

#include <algorithm>
#include <functional>

namespace reloadmaker
{
    bool suppliedAnchorPlacement(const Transformf& authoredRoot, const Transformf& anchorInMount,
                                 const Transformf& connection, Transformf& rootPlacement)
    {
        Transformf inverse;
        if (!invertUniformTransform(anchorInMount, inverse)) { return false; }
        rootPlacement = composed(composed(composed(anchorInMount, connection), inverse), authoredRoot);
        return true;
    }

    bool compileSceneBindings(const HkxSkeleton& skeleton, const int weaponBone,
                              const QVector<ScenePoseNode>& nodes, const QVector<int>& canonicalNodes,
                              QVector<ScenePoseBinding>& bindings, QString& error)
    {
        bindings.clear();
        error.clear();
        if (weaponBone < 0 || weaponBone >= skeleton.bones.size() ||
            canonicalNodes.size() != skeleton.bones.size()) {
            error = QStringLiteral("Invalid assembly rig or Weapon binding");
            return false;
        }
        QVector<Transformf> restGlobal(nodes.size());
        QVector<int> visited(nodes.size(), 0);
        const std::function<bool(int)> visit = [&](int index) {
            if (index < 0 || index >= nodes.size() || visited.at(index) == 1) { return false; }
            if (visited.at(index) == 2) { return true; }
            visited[index] = 1;
            const auto& node = nodes.at(index);
            if (node.parent < -1 || (node.parent >= 0 && !visit(node.parent))) { return false; }
            restGlobal[index] = node.parent >= 0
                ? composed(restGlobal.at(node.parent), node.restLocal) : node.restLocal;
            visited[index] = 2;
            return true;
        };
        for (int node = 0; node < nodes.size(); ++node) {
            if (!visit(node)) {
                error = QStringLiteral("Invalid/cyclic scene parent at assembly node %1").arg(node);
                return false;
            }
            const int bone = nodes.at(node).boneIndex;
            if (bone < 0) { continue; }
            if (bone >= skeleton.bones.size()) {
                error = QStringLiteral("Assembly node %1 maps outside skeleton").arg(node);
                return false;
            }
            int ancestor = bone;
            bool part = false;
            for (int depth = 0; depth < skeleton.bones.size() && ancestor >= 0; ++depth) {
                if (ancestor == weaponBone) { part = bone != weaponBone; break; }
                const int parent = skeleton.bones.at(ancestor).parentIndex;
                if (parent >= ancestor || parent < -1) {
                    error = QStringLiteral("Invalid skeleton parent at bone %1").arg(ancestor);
                    return false;
                }
                ancestor = parent;
            }
            ScenePoseBinding binding;
            binding.nodeIndex = node;
            binding.boneIndex = bone;
            binding.weaponPart = part;
            bindings.append(binding);
        }
        std::stable_sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
            return a.boneIndex < b.boneIndex;
        });
        QVector<int> nodeToBinding(nodes.size(), -1);
        for (int i = 0; i < bindings.size(); ++i) { nodeToBinding[bindings.at(i).nodeIndex] = i; }
        for (auto& binding : bindings) {
            const int parentBone = skeleton.bones.at(binding.boneIndex).parentIndex;
            int semanticParent = -1;
            int nearestBoundParent = -1;
            for (int p = nodes.at(binding.nodeIndex).parent; p >= 0; p = nodes.at(p).parent) {
                if (nodeToBinding.at(p) >= 0 && nearestBoundParent < 0) { nearestBoundParent = p; }
                if (binding.weaponPart && nodes.at(p).boneIndex == binding.boneIndex) {
                    error = QStringLiteral("Part %1 is nested under another binding of the same bone")
                                .arg(skeleton.bones.at(binding.boneIndex).name);
                    return false;
                }
                if (parentBone >= 0 && nodes.at(p).boneIndex == parentBone) { semanticParent = p; break; }
            }
            if (binding.weaponPart) {
                if (semanticParent < 0 && parentBone >= 0) {
                    const auto count = std::count_if(bindings.cbegin(), bindings.cend(), [parentBone](const auto& candidate) {
                        return candidate.boneIndex == parentBone;
                    });
                    if (count != 1) {
                        error = QStringLiteral("Missing/ambiguous cross-attachment parent for part %1")
                                    .arg(skeleton.bones.at(binding.boneIndex).name);
                        return false;
                    }
                    semanticParent = canonicalNodes.at(parentBone);
                }
                if (semanticParent < 0 || semanticParent >= nodes.size() ||
                    nodes.at(semanticParent).boneIndex != parentBone || nodeToBinding.at(semanticParent) < 0) {
                    error = QStringLiteral("Missing semantic parent for part %1")
                                .arg(skeleton.bones.at(binding.boneIndex).name);
                    return false;
                }
                Transformf inverse;
                if (!invertUniformTransform(restGlobal.at(semanticParent), inverse)) {
                    error = QStringLiteral("Non-invertible part bind parent");
                    return false;
                }
                binding.semanticParentBinding = nodeToBinding.at(semanticParent);
                binding.restInBoneParent = composed(inverse, restGlobal.at(binding.nodeIndex));
                binding.alignmentBoneIndex = binding.boneIndex;
                const int anchor = nodes.at(binding.nodeIndex).alignmentNode;
                if (anchor >= 0) {
                    if (anchor >= nodes.size() || nodeToBinding.at(anchor) < 0) {
                        error = QStringLiteral("Missing attachment alignment anchor for %1")
                                    .arg(skeleton.bones.at(binding.boneIndex).name);
                        return false;
                    }
                    const int anchorBone = nodes.at(anchor).boneIndex;
                    if (anchorBone < 0 || skeleton.bones.at(anchorBone).parentIndex != parentBone) {
                        error = QStringLiteral("Attachment alignment anchor has a different animation parent");
                        return false;
                    }
                    binding.alignmentBoneIndex = anchorBone;
                }
            }
            const int physicalParent = nodes.at(binding.nodeIndex).parent;
            const Transformf parentWorld = physicalParent >= 0 ? restGlobal.at(physicalParent) : Transformf{};
            binding.sceneParentBridge = parentWorld;
            if (nearestBoundParent >= 0) {
                Transformf inverse;
                if (!invertUniformTransform(restGlobal.at(nearestBoundParent), inverse)) {
                    error = QStringLiteral("Non-invertible scene bind parent");
                    return false;
                }
                binding.sceneParentBinding = nodeToBinding.at(nearestBoundParent);
                binding.sceneParentBridge = composed(inverse, parentWorld);
            }
        }
        return true;
    }

    bool evaluateSceneBindings(const HkxSkeleton& skeleton,
                               const QVector<ScenePoseBinding>& bindings,
                               const QVector<Transformf>& sourceLocal, const QVector<Transformf>& stateLocal,
                               const QVector<Transformf>& posedGlobals,
                               QVector<Transformf>& boundLocal, QVector<Transformf>& boundGlobal, QString& error)
    {
        error.clear();
        if (sourceLocal.size() != skeleton.bones.size() || stateLocal.size() != sourceLocal.size() ||
            posedGlobals.size() != sourceLocal.size()) {
            error = QStringLiteral("Assembly pose size mismatch");
            return false;
        }
        boundLocal.resize(bindings.size());
        boundGlobal.resize(bindings.size());
        for (int i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings.at(i);
            const int bone = binding.boneIndex;
            if (!binding.weaponPart) { boundGlobal[i] = posedGlobals.at(bone); continue; }
            Transformf inverse;
            if (binding.semanticParentBinding < 0 || binding.semanticParentBinding >= i ||
                binding.alignmentBoneIndex < 0 || binding.alignmentBoneIndex >= stateLocal.size() ||
                !invertUniformTransform(stateLocal.at(binding.alignmentBoneIndex), inverse)) {
                error = QStringLiteral("Invalid part baseline/order: %1").arg(skeleton.bones.at(bone).name);
                return false;
            }
            boundGlobal[i] = composed(boundGlobal.at(binding.semanticParentBinding),
                composed(composed(sourceLocal.at(bone), inverse), binding.restInBoneParent));
        }
        // Use actual destination ancestry, which can differ from the HKX hierarchy.
        // All targets exist before this pass, including parents on other branches.
        for (int i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings.at(i);
            const Transformf parent = binding.sceneParentBinding >= 0
                ? composed(boundGlobal.at(binding.sceneParentBinding), binding.sceneParentBridge)
                : binding.sceneParentBridge;
            Transformf inverse;
            if (!invertUniformTransform(parent, inverse)) {
                error = QStringLiteral("Non-invertible destination parent for %1")
                            .arg(skeleton.bones.at(binding.boneIndex).name);
                return false;
            }
            boundLocal[i] = composed(inverse, boundGlobal.at(i));
        }
        return true;
    }
}
