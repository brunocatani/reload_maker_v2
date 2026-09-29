#include "PoseEvaluator.h"

#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    bool invertUniformTransform(const Transformf& value,
                                              Transformf& result) noexcept
    {
        const float quaternionLengthSquared =
            value.rotation.x * value.rotation.x +
            value.rotation.y * value.rotation.y +
            value.rotation.z * value.rotation.z +
            value.rotation.w * value.rotation.w;
        const float scale = (value.scale.x + value.scale.y + value.scale.z) / 3.0f;
        const float scaleTolerance = 1.0e-4f * (std::max)(1.0f, std::abs(scale));
        if (!std::isfinite(value.translation.x) ||
            !std::isfinite(value.translation.y) ||
            !std::isfinite(value.translation.z) ||
            !std::isfinite(quaternionLengthSquared) ||
            !(quaternionLengthSquared > 1.0e-12f) ||
            !std::isfinite(scale) || !(std::abs(scale) > 1.0e-6f) ||
            std::abs(value.scale.x - scale) > scaleTolerance ||
            std::abs(value.scale.y - scale) > scaleTolerance ||
            std::abs(value.scale.z - scale) > scaleTolerance) {
            return false;
        }

        const Quaternionf inverseRotation = normalized(Quaternionf{
            -value.rotation.x,
            -value.rotation.y,
            -value.rotation.z,
            value.rotation.w,
        });
        const Vec3f inverseTranslation = rotated(inverseRotation, Vec3f{
            -value.translation.x,
            -value.translation.y,
            -value.translation.z,
        });
        const float inverseScale = 1.0f / scale;
        result = Transformf{
            Vec3f{
                inverseTranslation.x * inverseScale,
                inverseTranslation.y * inverseScale,
                inverseTranslation.z * inverseScale,
            },
            inverseRotation,
            Vec3f{ inverseScale, inverseScale, inverseScale },
        };
        return true;
    }


    Transformf interpolatedTransform(
        const Transformf& from, const Transformf& to, const float weight)
    {
        const float t = std::clamp(weight, 0.0f, 1.0f);
        const auto interpolate = [t](const float first, const float second) {
            return first + (second - first) * t;
        };

        Quaternionf first = normalized(from.rotation);
        Quaternionf second = normalized(to.rotation);
        float dot = first.x * second.x + first.y * second.y +
                    first.z * second.z + first.w * second.w;
        if (dot < 0.0f) {
            second = Quaternionf{
                -second.x, -second.y, -second.z, -second.w
            };
            dot = -dot;
        }
        dot = std::clamp(dot, 0.0f, 1.0f);

        Quaternionf rotation;
        if (dot > 0.9995f) {
            rotation = normalized(Quaternionf{
                interpolate(first.x, second.x),
                interpolate(first.y, second.y),
                interpolate(first.z, second.z),
                interpolate(first.w, second.w),
            });
        } else {
            const float angle = std::acos(dot);
            const float inverseSine = 1.0f / std::sin(angle);
            const float fromWeight = std::sin((1.0f - t) * angle) * inverseSine;
            const float toWeight = std::sin(t * angle) * inverseSine;
            rotation = normalized(Quaternionf{
                first.x * fromWeight + second.x * toWeight,
                first.y * fromWeight + second.y * toWeight,
                first.z * fromWeight + second.z * toWeight,
                first.w * fromWeight + second.w * toWeight,
            });
        }

        return Transformf{
            Vec3f{
                interpolate(from.translation.x, to.translation.x),
                interpolate(from.translation.y, to.translation.y),
                interpolate(from.translation.z, to.translation.z),
            },
            rotation,
            Vec3f{
                interpolate(from.scale.x, to.scale.x),
                interpolate(from.scale.y, to.scale.y),
                interpolate(from.scale.z, to.scale.z),
            },
        };
    }


    bool composeLocalPose(const HkxSkeleton& skeleton,
                          const QVector<Transformf>& tracks,
                          const QVector<std::int16_t>& mapping,
                          const bool additive, const QVector<Transformf>* base,
                          QVector<Transformf>& local, QString& error)
    {
        error.clear();
        if (!skeleton.valid() || tracks.size() != mapping.size() ||
            (base && (base == &local || base->size() != skeleton.bones.size())) ||
            (additive && !base)) {
            error = QStringLiteral("Pose input mismatch: additive clips require a separate explicit base");
            return false;
        }
        for (qsizetype track = 0; track < mapping.size(); ++track) {
            if (mapping.at(track) < 0 || mapping.at(track) >= skeleton.bones.size()) {
                error = QStringLiteral("Pose track %1 maps outside skeleton: %2")
                            .arg(track).arg(mapping.at(track));
                return false;
            }
        }
        local.resize(skeleton.bones.size());
        for (int bone = 0; bone < local.size(); ++bone) {
            local[bone] = base ? base->at(bone) : skeleton.bones.at(bone).referenceLocal;
        }
        for (qsizetype track = 0; track < tracks.size(); ++track) {
            const int bone = mapping.at(track);
            const Transformf& delta = tracks.at(track);
            if (!additive) {
                local[bone] = delta;
                continue;
            }
            const Transformf& reference = base->at(bone);
            local[bone] = Transformf{
                { reference.translation.x + delta.translation.x,
                  reference.translation.y + delta.translation.y,
                  reference.translation.z + delta.translation.z },
                multiplied(reference.rotation, delta.rotation),
                { reference.scale.x * delta.scale.x,
                  reference.scale.y * delta.scale.y,
                  reference.scale.z * delta.scale.z }
            };
        }
        return true;
    }

    bool buildPoseGlobals(const HkxSkeleton& skeleton,
                          const QVector<Transformf>& local,
                          QVector<Transformf>& global, QString& error)
    {
        error.clear();
        if (!skeleton.valid() || local.size() != skeleton.bones.size() || &local == &global) {
            error = QStringLiteral("Pose hierarchy size or output alias is invalid");
            return false;
        }
        global.resize(local.size());
        for (int bone = 0; bone < local.size(); ++bone) {
            const int parent = skeleton.bones.at(bone).parentIndex;
            Transformf inverse;
            Transformf check = local.at(bone);
            // A uniformly zero-scaled child is a valid authored visibility key.
            // Only frame inversion requires nonzero scale.
            if (check.scale.x == 0.0f && check.scale.y == 0.0f && check.scale.z == 0.0f) {
                check.scale = {1.0f, 1.0f, 1.0f};
            }
            if (parent >= bone || parent < -1 || !invertUniformTransform(check, inverse)) {
                const auto& value = local.at(bone);
                error = QStringLiteral("Invalid parent or non-finite/non-uniform transform at bone %1 (%2), parent=%3 scale=(%4,%5,%6) quaternion=(%7,%8,%9,%10)")
                            .arg(bone).arg(skeleton.bones.at(bone).name).arg(parent)
                            .arg(value.scale.x).arg(value.scale.y).arg(value.scale.z)
                            .arg(value.rotation.x).arg(value.rotation.y).arg(value.rotation.z).arg(value.rotation.w);
                return false;
            }
            global[bone] = parent >= 0
                ? composed(global.at(parent), local.at(bone)) : local.at(bone);
        }
        return true;
    }

    bool samplePose(const HkxSkeleton& skeleton, const HkxClip& clip,
                    const float timeSeconds, const QVector<Transformf>* base,
                    QVector<Transformf>& tracks, QVector<Transformf>& local,
                    QVector<Transformf>& global, QString& error)
    {
        error.clear();
        if (!clip.valid() || !std::isfinite(timeSeconds) ||
            (!clip.skeletonName.isEmpty() &&
             clip.skeletonName.compare(skeleton.name, Qt::CaseInsensitive) != 0)) {
            error = QStringLiteral("Invalid clip/time or skeleton name mismatch: '%1' vs '%2'")
                        .arg(clip.skeletonName, skeleton.name);
            return false;
        }
        clip.sample(timeSeconds, tracks);
        return composeLocalPose(skeleton, tracks, clip.trackToBone, clip.additive,
                                base, local, error) &&
               buildPoseGlobals(skeleton, local, global, error);
    }

    bool reframePose(const HkxSkeleton& skeleton,
                     const Transformf& sourceFrame, const Transformf& destinationFrame,
                     QVector<Transformf>& local, QVector<Transformf>& global,
                     QString& error)
    {
        error.clear();
        Transformf inverse;
        Transformf destinationInverse;
        if (local.size() != skeleton.bones.size() || global.size() != local.size() ||
            !invertUniformTransform(sourceFrame, inverse) ||
            !invertUniformTransform(destinationFrame, destinationInverse)) {
            error = QStringLiteral("Cannot change pose frame: invalid pose or anchor transform");
            return false;
        }
        const Transformf correction = composed(destinationFrame, inverse);
        for (int bone = 0; bone < local.size(); ++bone) {
            global[bone] = composed(correction, global.at(bone));
            if (skeleton.bones.at(bone).parentIndex < 0) {
                local[bone] = global.at(bone);
            }
        }
        return true;
    }
}
