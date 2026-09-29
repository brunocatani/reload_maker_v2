#pragma once
#include "ScenePoseBinding.h"

namespace reloadmaker {
    struct MechanismScenePlan {
        QVector<ScenePoseNode> nodes;
        QVector<int> order, group, roots;
    };
    [[nodiscard]] bool compileMechanismScene(const QVector<ScenePoseNode>& nodes,const QVector<ScenePoseBinding>& bindings,
        const QVector<int>& roots,MechanismScenePlan& plan,QString& error);
    [[nodiscard]] bool nativeMechanismScene(const MechanismScenePlan& plan,const QVector<ScenePoseBinding>& bindings,
        const QVector<Transformf>& boundLocal,QVector<Transformf>& local,QVector<Transformf>& world,QString& error);
    [[nodiscard]] bool moveMechanismScene(const MechanismScenePlan& plan,const QVector<Transformf>& targets,
        QVector<Transformf>& local,QVector<Transformf>& world,QString& error);
}
