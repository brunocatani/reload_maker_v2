#pragma once

#include "MechanismScene.h"
#include "PoseEvaluator.h"
#include "ScenePoseBinding.h"

namespace reloadmaker
{
    // Samples the native scene in Weapon-local space: one moment of a clip
    // (or an already sampled pose) becomes every scene node's world pose, and
    // node poses are read relative to the skeleton's Weapon bone. Buffers are
    // reused between samples. The skeleton, bindings and plan must outlive
    // the sampler.
    class WeaponLocalSampler final
    {
    public:
        WeaponLocalSampler(const HkxSkeleton& skeleton, const QVector<ScenePoseBinding>& bindings, const MechanismScenePlan& plan, int weaponBone);

        // The local pose that missing and additive tracks are composed against.
        void setBaseline(const QVector<Transformf>& baselineLocal);
        // The baseline is the start of a clip, itself composed against
        // `reference`.
        [[nodiscard]] bool setBaselineClip(const HkxClip& base, const QVector<Transformf>& reference, QString& error);

        // The scene at `seconds` of the clip. False with `error` when the
        // clip, the bindings or the scene cannot be evaluated.
        [[nodiscard]] bool sample(const HkxClip& clip, float seconds, QString& error);
        // The scene of a pose sampled elsewhere.
        [[nodiscard]] bool evaluate(const QVector<Transformf>& local, const QVector<Transformf>& global, QString& error);

        // Whether the last sample's Weapon frame can be inverted. nodePose is
        // only meaningful when it can.
        [[nodiscard]] bool weaponFrameValid() const noexcept { return _weaponFrameValid; }
        // A scene node's pose relative to the Weapon bone.
        [[nodiscard]] Transformf nodePose(int sceneNode) const;
        [[nodiscard]] const QVector<Transformf>& sceneWorld() const noexcept { return _sceneWorld; }

    private:
        const HkxSkeleton& _skeleton;
        const QVector<ScenePoseBinding>& _bindings;
        const MechanismScenePlan& _plan;
        int _weaponBone;
        QVector<Transformf> _baseline;
        QVector<Transformf> _tracks;
        QVector<Transformf> _local;
        QVector<Transformf> _global;
        QVector<Transformf> _boundLocal;
        QVector<Transformf> _boundGlobal;
        QVector<Transformf> _sceneLocal;
        QVector<Transformf> _sceneWorld;
        Transformf _weaponInverse;
        bool _weaponFrameValid{ false };
    };
}
