#include "areas/operate/WeaponLocalSampler.h"

namespace reloadmaker
{
    WeaponLocalSampler::WeaponLocalSampler(const HkxSkeleton& skeleton, const QVector<ScenePoseBinding>& bindings, const MechanismScenePlan& plan,
                                           int weaponBone)
        : _skeleton(skeleton)
        , _bindings(bindings)
        , _plan(plan)
        , _weaponBone(weaponBone)
    {
    }

    void WeaponLocalSampler::setBaseline(const QVector<Transformf>& baselineLocal)
    {
        _baseline = baselineLocal;
    }

    bool WeaponLocalSampler::setBaselineClip(const HkxClip& base, const QVector<Transformf>& reference, QString& error)
    {
        return samplePose(_skeleton, base, 0, &reference, _tracks, _baseline, _global, error);
    }

    bool WeaponLocalSampler::sample(const HkxClip& clip, float seconds, QString& error)
    {
        _weaponFrameValid = false;
        return samplePose(_skeleton, clip, seconds, &_baseline, _tracks, _local, _global, error) && evaluate(_local, _global, error);
    }

    // The skeleton pose drives the bound scene nodes, and the native scene
    // gives every node its world pose.
    bool WeaponLocalSampler::evaluate(const QVector<Transformf>& local, const QVector<Transformf>& global, QString& error)
    {
        _weaponFrameValid = false;
        if (!evaluateSceneBindings(_skeleton, _bindings, local, _baseline, global, _boundLocal, _boundGlobal, error) ||
            !nativeMechanismScene(_plan, _bindings, _boundLocal, _sceneLocal, _sceneWorld, error)) {
            return false;
        }
        _weaponFrameValid = _weaponBone >= 0 && _weaponBone < global.size() && invertUniformTransform(global[_weaponBone], _weaponInverse);
        return true;
    }

    Transformf WeaponLocalSampler::nodePose(int sceneNode) const
    {
        return composed(_weaponInverse, _sceneWorld[sceneNode]);
    }
}
