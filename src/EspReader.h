#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>

namespace reloadmaker
{
    struct ObjectModRecord
    {
        std::uint32_t formId{ 0 };
        QString editorId;
        QString name;
        QString modelPath;
        QVector<std::uint32_t> addedKeywords;
        std::uint32_t attachPoint{ 0 };
        QVector<std::uint32_t> attachParentSlots;
        QVector<std::uint32_t> targetKeywords;
        QString metadataWarning;
        bool weaponModification{ false };
        QString targetType;
    };

    struct WeaponRecord
    {
        std::uint32_t formId{ 0 };
        QString editorId;
        QString name;
        QString baseModelPath;
        QVector<std::uint32_t> keywords;
        QVector<std::uint32_t> defaultObjectMods;
        QVector<std::uint32_t> attachParentSlots;
    };

    struct AnimationSubgraphEntry
    {
        QVector<std::uint32_t> requiredKeywords;
        QString behaviorPath;
        QStringList animationSearchPaths;
        std::uint32_t flags{ 0 };

        [[nodiscard]] bool firstPerson() const noexcept
        {
            return (flags & 0x00010000U) != 0;
        }
    };

    struct AnimationSubgraphRecord
    {
        std::uint32_t formId{ 0 };
        QString editorId;
        QVector<AnimationSubgraphEntry> entries;
    };

    struct PluginContent
    {
        QString filePath;
        QString fileName;
        QStringList masters;
        QVector<WeaponRecord> weapons;
        QVector<ObjectModRecord> objectMods;
        QVector<AnimationSubgraphRecord> animationSubgraphs;
        QHash<std::uint32_t, QString> keywordNames;
        QStringList diagnostics;
        // File-local IDs until LoadOrderLibrary maps them through MAST. Includes
        // deleted and empty overrides so an earlier definition cannot reappear.
        QVector<std::uint32_t> deletedRecords;
        struct Origin { QString plugin; QString winner; std::uint32_t objectId{0}; };
        QHash<std::uint32_t, Origin> origins;
        bool parsed{false};

        [[nodiscard]] const ObjectModRecord* findObjectMod(std::uint32_t formId) const noexcept;
        [[nodiscard]] const AnimationSubgraphEntry* resolveFirstPersonSubgraph(
            const QVector<std::uint32_t>& activeKeywords) const noexcept;
    };

    class EspReader final
    {
    public:
        [[nodiscard]] static PluginContent read(const QString& path,
                                                const std::function<bool()>& cancelled = {});
    };
}
