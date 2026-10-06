#pragma once

#include "model/nifmodel.h"

namespace reloadmaker
{
    // Authoring identity only. Native files can repeat a sibling's name;
    // its child-link position distinguishes those surfaces without relying
    // on block numbers assigned while assembling the weapon. Unique names
    // retain their existing keys; runtime node selectors keep native names.
    [[nodiscard]] inline QString authoringNodeName(const NifModel& model, const QModelIndex& node)
    {
        const auto name = model.get<QString>(node, "Name");
        const auto parent = model.getBlockIndex(model.getParent(node));
        if (!parent.isValid()) {
            return name;
        }
        const auto children = model.getChildLinks(model.getBlockNumber(parent));
        int matches = 0;
        for (const int child : children) {
            if (model.get<QString>(model.getBlockIndex(child), "Name") == name) {
                ++matches;
            }
        }
        return matches > 1 ? name + QString("[#%1]").arg(children.indexOf(model.getBlockNumber(node))) : name;
    }

    // Whether `node` is `ancestor` or below it in the model's block tree. The
    // walk stops after as many steps as there are blocks.
    [[nodiscard]] inline bool isWithinBlock(const NifModel& model, QModelIndex node, const QModelIndex& ancestor)
    {
        for (int depth = 0; node.isValid() && depth < model.getBlockCount(); ++depth) {
            if (node == ancestor) {
                return true;
            }
            node = model.getBlockIndex(model.getParent(node));
        }
        return false;
    }
}
