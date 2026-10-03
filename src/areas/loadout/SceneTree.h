#pragma once

#include "model/nifmodel.h"

namespace reloadmaker
{
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
