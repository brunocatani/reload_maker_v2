#pragma once

#include <QByteArray>
#include <QSharedPointer>
#include <QString>
#include <QVector>

#include <cstdint>

namespace reloadmaker
{
    struct Vec3f
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    struct Quaternionf
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
        float w{ 1.0f };
    };

    struct Transformf
    {
        Vec3f translation{};
        Quaternionf rotation{};
        Vec3f scale{ 1.0f, 1.0f, 1.0f };
    };

    struct HkxBone
    {
        QString name;
        std::int16_t parentIndex{ -1 };
        bool lockTranslation{ false };
        Transformf referenceLocal{};
    };

    struct HkxSkeleton
    {
        QString name;
        QVector<HkxBone> bones;

        [[nodiscard]] int boneIndex(const QString& name) const noexcept;
        [[nodiscard]] bool valid() const noexcept { return !bones.isEmpty(); }
    };

    struct HkxAnnotation
    {
        float timeSeconds{ 0.0f };
        QString trackName;
        QString text;
    };

    struct HkxBehaviorClip
    {
        QString nodeName;
        QString animationPath;
        float blendInSeconds{ 0.0f };
        float blendOutSeconds{ 0.0f };
    };

    struct HkxBehaviorGraph
    {
        QVector<HkxBehaviorClip> clips;
        QStringList behaviorReferences;
        QString defaultReadyAnimationPath;

        [[nodiscard]] bool valid() const noexcept
        {
            return !clips.isEmpty() || !behaviorReferences.isEmpty();
        }
    };

    class HkxClip final
    {
    public:
        HkxClip() = default;

        float durationSeconds{ 0.0f };
        std::uint32_t frameCount{ 0 };
        std::uint32_t transformTrackCount{ 0 };
        bool additive{ false };
        QString skeletonName;
        QVector<std::int16_t> trackToBone;
        QVector<HkxAnnotation> annotations;

        [[nodiscard]] bool valid() const noexcept;
        void sample(float timeSeconds, QVector<Transformf>& result) const;

    private:
        struct Storage;
        QSharedPointer<Storage> _storage;
        friend class HkxReader;
    };

    class HkxReader final
    {
    public:
        [[nodiscard]] static HkxSkeleton readSkeleton(const QByteArray& data, QString& error);
        [[nodiscard]] static HkxClip readClip(const QByteArray& data, QString& error);
        [[nodiscard]] static HkxBehaviorGraph readBehaviorGraph(
            const QByteArray& data, QString& error);
    };

    [[nodiscard]] Quaternionf normalized(Quaternionf value) noexcept;
    [[nodiscard]] Quaternionf multiplied(const Quaternionf& left, const Quaternionf& right) noexcept;
    [[nodiscard]] Vec3f rotated(const Quaternionf& rotation, const Vec3f& value) noexcept;
    [[nodiscard]] Transformf composed(const Transformf& parent, const Transformf& local) noexcept;
}
