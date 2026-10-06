// Which weapon part a click in the viewport landed on: the visible weapon
// surface under the point, and the reference parts it can belong to.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"

namespace reloadmaker
{
    bool ReloadMakerController::operatingSurfaceCandidate(int candidate) const
    {
        const int operating=operatingAnchor();
        if(operating<0||candidate<0||candidate>=_handPoseAnchors.size())return false;
        const int target=_sceneNodes.indexOf(_handPoseAnchors[operating].node);
        int node=_sceneNodes.indexOf(_handPoseAnchors[candidate].node);
        for(int depth=0;node>=0&&depth<_assemblyNodes.size();++depth) {
            if(node==target)return true;
            // Static helper descendants belong to the part. Crossing another
            // animated bone would admit an independently moving surface.
            if(_assemblyNodes[node].boneIndex>=0)return false;
            node=_assemblyNodes[node].parent;
        }
        return false;
    }
    // Fills the hit point, the surface's block name and the candidate
    // reference parts. A rigid surface's candidates come from its nearest
    // anchored ancestor, a skinned surface's from its animated bones;
    // `automatic` is the candidate when there is exactly one unambiguous one.
    bool ReloadMakerController::pickWeaponPart(QPointF point, int& automatic, Vec3f& world, QString& surface, QVector<int>& candidates, QString& error)
    {
        automatic = -1;
        candidates.clear();
        error.clear();
        auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        if (!model || !scene || !currentWeapon()) {
            error = "Load a weapon first.";
            return false;
        }
        int block = -1;
        Vector3 hit;
        if (!_window->getGLView()->reloadMakerSurfaceAt(point, visibleWeaponShapes(), block, hit)) {
            error = "No visible weapon surface at that point.";
            return false;
        }
        world = { hit[0], hit[1], hit[2] };
        const auto hitNode = model->getBlockIndex(block);
        surface = model->get<QString>(hitNode, "Name");
        const auto* shape = dynamic_cast<Shape*>(scene->getNode(model, hitNode));
        const auto skin = shape ? shape->reloadMakerSkinBones() : QVector<int>{};
        for (const int bone : skin) {
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                if (model->getBlockNumber(_handPoseAnchors[i].node) == bone && _handPoseAnchors[i].description.anchorKind == "animated-part") {
                    candidates.append(i);
                }
            }
        }
        if (skin.size() == 1 && candidates.size() == 1) {
            automatic = candidates[0];
        }
        if (candidates.isEmpty()) {
            appendAncestorAnchor(hitNode, candidates);
            if (skin.isEmpty() && candidates.size() == 1) {
                automatic = candidates[0];
            }
        }
        return true;
    }

    // Block ids of the visible shapes under the weapon root or an installed
    // part root.
    QVector<int> ReloadMakerController::visibleWeaponShapes() const
    {
        auto* model = activeModel();
        QVector<int> allowed;
        for (auto* shape : _window->getGLView()->getScene()->shapes) {
            if (!shape || shape->isHidden()) {
                continue;
            }
            auto node = model->getBlockIndex(shape->id());
            bool weapon = false;
            for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                if (node == _weaponRoot) {
                    weapon = true;
                }
                for (const auto& root : _partRoots) {
                    if (root == node) {
                        weapon = true;
                    }
                }
                if (weapon) {
                    break;
                }
                node = model->getBlockIndex(model->getParent(node));
            }
            if (weapon) {
                allowed.append(shape->id());
            }
        }
        return allowed;
    }

    // The nearest ancestor of `node` (itself included) that is a reference
    // part, preferring an animated part among anchors on the same block.
    void ReloadMakerController::appendAncestorAnchor(QModelIndex node, QVector<int>& candidates) const
    {
        auto* model = activeModel();
        for (int depth = 0; node.isValid() && depth < model->getBlockCount() && candidates.isEmpty(); ++depth) {
            int selected = -1;
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                if (_handPoseAnchors[i].node != node) {
                    continue;
                }
                selected = i;
                if (_handPoseAnchors[i].description.anchorKind == "animated-part") {
                    break;
                }
            }
            if (selected >= 0) {
                candidates.append(selected);
            }
            node = model->getBlockIndex(model->getParent(node));
        }
    }
}
