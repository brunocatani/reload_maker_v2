#include "MechanismScene.h"
#include <functional>

namespace reloadmaker {
    bool compileMechanismScene(const QVector<ScenePoseNode>& nodes,const QVector<ScenePoseBinding>& bindings,
        const QVector<int>& roots,MechanismScenePlan& plan,QString& error){
        error.clear();MechanismScenePlan p;p.nodes=nodes;p.roots=roots;p.group.fill(-1,nodes.size());QVector<int> state(nodes.size(),0);
        for(const auto& b:bindings)if(b.nodeIndex<0||b.nodeIndex>=nodes.size()||b.semanticParentBinding< -1||b.semanticParentBinding>=bindings.size()){
            error="Invalid mechanism scene binding";return false;
        }
        const std::function<bool(int)> visit=[&](int node){
            if(node<0||node>=nodes.size()||state[node]==1){return false;}
            if(state[node]==2){return true;}
            state[node]=1;
            if(nodes[node].parent< -1||(nodes[node].parent>=0&&!visit(nodes[node].parent)))return false;
            state[node]=2;p.order.append(node);return true;
        };
        for(int i=0;i<nodes.size();++i)if(!visit(i)){error="Invalid scene ancestry";return false;}
        for(int i=0;i<roots.size();++i){if(roots[i]<0||roots[i]>=nodes.size()||p.group[roots[i]]>=0){error="Missing or duplicate mechanism root";return false;}p.group[roots[i]]=i;}
        const auto assign=[&](int child,int parent){
            if(parent<0||p.group[parent]<0)return true;
            if(p.group[child]>=0&&p.group[child]!=p.group[parent])return false;
            p.group[child]=p.group[parent];return true;
        };
        for(int pass=0;pass<=nodes.size();++pass){
            const auto before=p.group;
            for(int node:p.order)if(!assign(node,nodes[node].parent)){error="Follower groups overlap a parent/child group";return false;}
            for(const auto& b:bindings)if(b.weaponPart&&b.semanticParentBinding>=0){
                if(b.semanticParentBinding>=bindings.size()||b.nodeIndex<0||b.nodeIndex>=nodes.size()||!assign(b.nodeIndex,bindings[b.semanticParentBinding].nodeIndex)){error="Follower groups overlap an inherited animation group";return false;}
            }
            if(before==p.group){plan=std::move(p);return true;}
        }
        error="Mechanism dependency graph did not settle";return false;
    }
    bool nativeMechanismScene(const MechanismScenePlan& plan,const QVector<ScenePoseBinding>& bindings,
        const QVector<Transformf>& boundLocal,QVector<Transformf>& local,QVector<Transformf>& world,QString& error){
        error.clear();
        if(bindings.size()!=boundLocal.size()){error="Scene pose size mismatch";return false;}
        local.resize(plan.nodes.size());world.resize(plan.nodes.size());
        for(int i=0;i<local.size();++i)local[i]=plan.nodes[i].restLocal;
        for(int i=0;i<bindings.size();++i){if(bindings[i].nodeIndex<0||bindings[i].nodeIndex>=local.size()){error="Invalid native scene binding";return false;}local[bindings[i].nodeIndex]=boundLocal[i];}
        for(int node:plan.order){const int parent=plan.nodes[node].parent;world[node]=parent<0?local[node]:composed(world[parent],local[node]);}
        return true;
    }
    bool moveMechanismScene(const MechanismScenePlan& plan,const QVector<Transformf>& targets,
        QVector<Transformf>& local,QVector<Transformf>& world,QString& error){
        error.clear();
        if(targets.size()!=plan.roots.size()||world.size()!=plan.nodes.size()||local.size()!=world.size()){error="Mechanism scene size mismatch";return false;}
        QVector<Transformf> delta(targets.size());
        for(int i=0;i<targets.size();++i){Transformf inverse;if(!invertUniformTransform(world[plan.roots[i]],inverse)){error="Mechanism root is hidden or non-invertible at this animation frame";return false;}delta[i]=composed(targets[i],inverse);}
        auto posed=world;auto locals=local;
        for(int node:plan.order)if(plan.group[node]>=0)posed[node]=composed(delta[plan.group[node]],world[node]);
        for(int node:plan.order){
            const int parent=plan.nodes[node].parent;
            if(plan.group[node]<0||(parent>=0&&plan.group[parent]==plan.group[node]))continue;
            Transformf inverse;if(parent>=0&&!invertUniformTransform(posed[parent],inverse)){error="Non-invertible mechanism parent";return false;}
            locals[node]=parent>=0?composed(inverse,posed[node]):posed[node];
        }
        local=std::move(locals);world=std::move(posed);return true;
    }
}
