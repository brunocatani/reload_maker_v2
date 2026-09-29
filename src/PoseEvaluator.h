#pragma once

#include "HkxReader.h"

namespace reloadmaker
{
    [[nodiscard]] bool invertUniformTransform(const Transformf& value,
                                              Transformf& result) noexcept;
    [[nodiscard]] Transformf interpolatedTransform(const Transformf& from,
                                                   const Transformf& to, float weight);
    // Missing normal tracks use the explicit base, or skeleton reference locals.
    // Additive clips require an explicit evaluated base. No controller state is read.
    [[nodiscard]] bool composeLocalPose(const HkxSkeleton& skeleton,
                                        const QVector<Transformf>& tracks,
                                        const QVector<std::int16_t>& mapping,
                                        bool additive,
                                        const QVector<Transformf>* base,
                                        QVector<Transformf>& local, QString& error);
    [[nodiscard]] bool buildPoseGlobals(const HkxSkeleton& skeleton,
                                        const QVector<Transformf>& local,
                                        QVector<Transformf>& global, QString& error);
    [[nodiscard]] bool samplePose(const HkxSkeleton& skeleton, const HkxClip& clip,
                                  float timeSeconds, const QVector<Transformf>* base,
                                  QVector<Transformf>& tracks,
                                  QVector<Transformf>& local,
                                  QVector<Transformf>& global, QString& error);
    // One change of world frame for the entire evaluated pose. Local articulation
    // below each hierarchy root is unchanged, including Weapon and finger tracks.
    [[nodiscard]] bool reframePose(const HkxSkeleton& skeleton,
                                   const Transformf& sourceFrame,
                                   const Transformf& destinationFrame,
                                   QVector<Transformf>& local,
                                   QVector<Transformf>& global, QString& error);
}
