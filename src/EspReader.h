#pragma once

#include "SoundProfile.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>

namespace reloadmaker
{
    // A form ID as eight upper-case hex digits.
    [[nodiscard]] inline QString formIdText(std::uint32_t formId)
    {
        return QStringLiteral("%1").arg(formId, 8, 16, QLatin1Char('0')).toUpper();
    }

    struct SoundRecord {
        std::uint32_t formId{0};
        QString editorId;
        QStringList files;
        float attenuationDb{0};
        SoundDescriptorType type{SoundDescriptorType::Standard};
        QVector<std::uint32_t> children;       // Compound DNAM, file-local
        QVector<RateOfFireSample> ratesOfFire; // AutoWeapon INTV/FNAM pairs
        QVector<SoundCondition> conditions;    // CTDA
    };
    // KSSM: the descriptor the engine plays for a shot whose Sound keyword
    // context holds every listed keyword (xEdit wbDefinitionsFO4 KSSM).
    struct SoundKeywordMapping {
        std::uint32_t formId{0};
        QString editorId;
        std::uint32_t descriptor{0};
        QVector<std::uint32_t> keywords;
    };
    struct AmmoProperty {
        std::uint16_t property{0};
        std::uint8_t valueType{0}, function{0};
        std::uint32_t value1{0}, value2{0}, stepBits{0};
    };
    // One "Includes" entry: a mod collection's levelled member.
    struct ObjectModInclude
    {
        std::uint32_t formId{ 0 };
        std::uint8_t minLevel{ 0 };
        bool optional{ false };
        bool dontUseAll{ false };
    };

    struct ObjectModRecord
    {
        std::uint32_t formId{ 0 };
        QString editorId;
        QString name;
        QString modelPath;
        QVector<std::uint32_t> addedKeywords;
        std::uint32_t attachPoint{ 0 };
        QVector<std::uint32_t> attachParentSlots;
        QVector<ObjectModInclude> includes;
        QVector<std::uint32_t> targetKeywords;
        QString metadataWarning;
        bool weaponModification{ false };
        QString targetType;
        QVector<AmmoProperty> ammoProperties;
        QVector<std::uint32_t> removedKeywords;
        QVector<AmmoProperty> firingProperties;
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
        std::uint32_t ammo{0};
        std::uint32_t attackFailSound{0};
        int ammoCapacity{0};
        bool ammoDataKnown{false};
        bool automatic{false};
        bool repeatableSingleFire{false};
        bool boltAction{false}; // cycled by hand after each shot
        std::uint32_t recordFlags{0}, dataFlags{0};
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
        struct NodeRemap { QString node, point; };
        QString filePath;
        QString fileName;
        QStringList masters;
        QVector<WeaponRecord> weapons;
        QVector<SoundRecord> sounds;
        QVector<SoundKeywordMapping> soundMappings;
        QSet<std::uint32_t> soundKeywords; // KYWD whose TNAM type is 6, Sound
        QVector<ObjectModRecord> objectMods;
        QVector<AnimationSubgraphRecord> animationSubgraphs;
        QHash<std::uint32_t, QString> keywordNames;
        QHash<std::uint32_t, NodeRemap> nodeRemaps;
        QHash<std::uint32_t, QString> ammoNames;
        QHash<std::uint32_t, std::uint32_t> localizedNameIds; // FULL IDs, before MAST remapping.
        QStringList diagnostics;
        // File-local IDs until LoadOrderLibrary maps them through MAST. Includes
        // deleted and empty overrides so an earlier definition cannot reappear.
        QVector<std::uint32_t> deletedRecords;
        struct Origin { QString plugin; QString winner; std::uint32_t objectId{0}; QStringList contributors; };
        QHash<std::uint32_t, Origin> origins;
        QStringList sourcePlugins; // Actual input files, in dependency/load order.
        QHash<QString,QStringList> sourceMasters;
        bool parsed{false};

        [[nodiscard]] const ObjectModRecord* findObjectMod(std::uint32_t formId) const noexcept;
        [[nodiscard]] const AnimationSubgraphEntry* resolveFirstPersonSubgraph(
            const QVector<std::uint32_t>& activeKeywords) const noexcept;
    };

    class EspReader final
    {
    public:
        [[nodiscard]] static PluginContent read(const QString& path,
                                                const std::function<bool()>& cancelled = {}, bool referenceCatalogOnly = false);
        static void resolveNames(PluginContent& plugin, const QString& language,
            const std::function<QByteArray(const QString&, QString&)>& readResource);
    };
    // A sound record in record identities, its children lower case.
    [[nodiscard]] SoundDescriptor soundDescriptor(const PluginContent& plugin, const SoundRecord& sound);
    [[nodiscard]] QString recordIdentity(const PluginContent& plugin,std::uint32_t formId);
    [[nodiscard]] QString weaponAuthoringExclusion(const WeaponRecord& weapon);
}
