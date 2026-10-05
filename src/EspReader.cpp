#include "EspReader.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <QScopeGuard>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        constexpr qsizetype kRecordHeaderSize = 24;
        constexpr qsizetype kGroupHeaderSize = 24;
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000U;
        constexpr std::uint32_t kLocalizedPluginFlag = 0x00000080U;
        constexpr qsizetype kMaximumRecords = 2'000'000;
        constexpr qsizetype kMaximumSubrecords = 1'000'000;
        constexpr int kMaximumGroupDepth = 64;

        template <class T>
        [[nodiscard]] T readLittle(const QByteArray& data, const qsizetype offset)
        {
            if (offset < 0 || offset > data.size() - static_cast<qsizetype>(sizeof(T))) {
                throw std::runtime_error("plugin field is outside the file");
            }
            return qFromLittleEndian<T>(reinterpret_cast<const uchar*>(data.constData() + offset));
        }

        [[nodiscard]] QByteArray signatureAt(const QByteArray& data, const qsizetype offset)
        {
            if (offset < 0 || offset > data.size() - 4) {
                throw std::runtime_error("plugin signature is outside the file");
            }
            return data.mid(offset, 4);
        }

        [[nodiscard]] QString decodeText(const QByteArray& data)
        {
            const QByteArray trimmed = data.endsWith('\0') ? data.chopped(1) : data;
            const QString utf8 = QString::fromUtf8(trimmed);
            if (!utf8.contains(QChar::ReplacementCharacter)) {
                return utf8;
            }
            return QString::fromLocal8Bit(trimmed);
        }

        struct Subrecord
        {
            QByteArray type;
            QByteArray data;
        };

        struct RawRecord
        {
            QByteArray type;
            std::uint32_t flags{ 0 };
            std::uint32_t formId{ 0 };
            QVector<Subrecord> subrecords;
        };

        struct ParseState
        {
            qsizetype recordCount{ 0 };
            qsizetype subrecordCount{ 0 };
            QVector<RawRecord> records;
            std::function<bool()> cancelled;
            bool referenceCatalogOnly{false};
        };

        [[nodiscard]] QByteArray decompressRecord(const QByteArray& recordData)
        {
            if (recordData.size() < 4) {
                throw std::runtime_error("compressed plugin record is truncated");
            }
            const std::uint32_t expectedSize = readLittle<std::uint32_t>(recordData, 0);
            if (expectedSize > 256U * 1024U * 1024U) {
                throw std::runtime_error("compressed plugin record is too large");
            }

            QByteArray qtCompressed;
            qtCompressed.resize(4);
            qToBigEndian(expectedSize, reinterpret_cast<uchar*>(qtCompressed.data()));
            qtCompressed.append(recordData.constData() + 4, recordData.size() - 4);
            const QByteArray result = qUncompress(qtCompressed);
            if (result.size() != static_cast<qsizetype>(expectedSize)) {
                throw std::runtime_error("compressed plugin record did not expand to its declared size");
            }
            return result;
        }

        [[nodiscard]] QVector<Subrecord> parseSubrecords(const QByteArray& data, ParseState& state)
        {
            QVector<Subrecord> result;
            qsizetype position = 0;
            std::optional<std::uint32_t> extendedSize;
            while (position < data.size()) {
                if (++state.subrecordCount > kMaximumSubrecords) {
                    throw std::runtime_error("plugin contains too many subrecords");
                }
                if (position > data.size() - 6) {
                    throw std::runtime_error("plugin subrecord header is truncated");
                }
                const QByteArray type = data.mid(position, 4);
                const std::uint16_t shortSize = readLittle<std::uint16_t>(data, position + 4);
                position += 6;

                if (type == QByteArrayLiteral("XXXX")) {
                    if (shortSize != 4 || position > data.size() - 4) {
                        throw std::runtime_error("plugin XXXX subrecord is invalid");
                    }
                    extendedSize = readLittle<std::uint32_t>(data, position);
                    position += 4;
                    continue;
                }

                const std::uint32_t size = extendedSize.value_or(shortSize);
                extendedSize.reset();
                if (size > static_cast<std::uint32_t>(data.size() - position)) {
                    throw std::runtime_error("plugin subrecord data is truncated");
                }
                result.append(Subrecord{ type, data.mid(position, static_cast<qsizetype>(size)) });
                position += static_cast<qsizetype>(size);
            }
            if (extendedSize.has_value()) {
                throw std::runtime_error("plugin ends after an XXXX subrecord");
            }
            return result;
        }

        [[nodiscard]] qsizetype parseRecord(const QByteArray& file, const qsizetype position,
                                            const qsizetype limit, ParseState& state)
        {
            if (state.cancelled && state.cancelled()) { throw std::runtime_error("Cancelled"); }
            if (++state.recordCount > kMaximumRecords) {
                throw std::runtime_error("plugin contains too many records");
            }
            if (position < 0 || position > limit - kRecordHeaderSize) {
                throw std::runtime_error("plugin record header is truncated");
            }
            const QByteArray type = signatureAt(file, position);
            const std::uint32_t dataSize = readLittle<std::uint32_t>(file, position + 4);
            const std::uint32_t flags = readLittle<std::uint32_t>(file, position + 8);
            const std::uint32_t formId = readLittle<std::uint32_t>(file, position + 12);
            const qsizetype dataStart = position + kRecordHeaderSize;
            if (dataSize > static_cast<std::uint32_t>(limit - dataStart)) {
                throw std::runtime_error("plugin record data is truncated");
            }
            // A full Data library includes the base ESM. Skip irrelevant payloads
            // before decompression/copying, while still checking record bounds.
            if ((state.referenceCatalogOnly && type!="TES4" && type!="AMMO" && type!="KYWD" && type!="SNDR" && type!="KSSM") ||
                (type != "TES4" && type != "WEAP" && type != "OMOD" && type != "KYWD" && type != "RACE" && type!="AMMO" && type!="SNDR" && type!="KSSM")) {
                return dataStart + static_cast<qsizetype>(dataSize);
            }
            QByteArray recordData = file.mid(dataStart, static_cast<qsizetype>(dataSize));
            if ((flags & kCompressedRecordFlag) != 0) {
                recordData = decompressRecord(recordData);
            }
            state.records.append(RawRecord{
                .type = type,
                .flags = flags,
                .formId = formId,
                .subrecords = parseSubrecords(recordData, state),
            });
            return dataStart + static_cast<qsizetype>(dataSize);
        }

        void parseRegion(const QByteArray& file, qsizetype position, const qsizetype limit,
                         const int depth, ParseState& state)
        {
            if (depth > kMaximumGroupDepth) {
                throw std::runtime_error("plugin group nesting is too deep");
            }
            while (position < limit) {
                const QByteArray type = signatureAt(file, position);
                if (type == QByteArrayLiteral("GRUP")) {
                    if (position > limit - kGroupHeaderSize) {
                        throw std::runtime_error("plugin group header is truncated");
                    }
                    const std::uint32_t groupSize = readLittle<std::uint32_t>(file, position + 4);
                    if (groupSize < kGroupHeaderSize ||
                        groupSize > static_cast<std::uint32_t>(limit - position)) {
                        throw std::runtime_error("plugin group size is invalid");
                    }
                    const qsizetype groupEnd = position + static_cast<qsizetype>(groupSize);
                    parseRegion(file, position + kGroupHeaderSize, groupEnd, depth + 1, state);
                    position = groupEnd;
                } else {
                    position = parseRecord(file, position, limit, state);
                }
            }
            if (position != limit) {
                throw std::runtime_error("plugin region did not end on a record boundary");
            }
        }

        [[nodiscard]] const Subrecord* firstSubrecord(const RawRecord& record, const QByteArray& type)
        {
            const auto iterator = std::find_if(record.subrecords.cbegin(), record.subrecords.cend(),
                [&type](const Subrecord& subrecord) { return subrecord.type == type; });
            return iterator == record.subrecords.cend() ? nullptr : &*iterator;
        }

        [[nodiscard]] QString textSubrecord(const RawRecord& record, const QByteArray& type,
                                            const bool localized)
        {
            const Subrecord* subrecord = firstSubrecord(record, type);
            if (!subrecord) {
                return {};
            }
            if (localized && type == QByteArrayLiteral("FULL")) return {}; // Resolved from the effective STRINGS table.
            return decodeText(subrecord->data);
        }

        void appendUnique(QVector<std::uint32_t>& values, const std::uint32_t value)
        {
            if (!values.contains(value)) {
                values.append(value);
            }
        }

        [[nodiscard]] QVector<std::uint32_t> formIdArray(const QByteArray& data)
        {
            QVector<std::uint32_t> result;
            if ((data.size() % 4) != 0) {
                return result;
            }
            result.reserve(data.size() / 4);
            for (qsizetype offset = 0; offset < data.size(); offset += 4) {
                appendUnique(result, readLittle<std::uint32_t>(data, offset));
            }
            return result;
        }

        void decodeObjectModData(const QByteArray& data, ObjectModRecord& mod)
        {
            // Verified against FO4 Mods MCP raw DATA and the local Mutagen YAML
            // projection: parent-slot FormIDs are variable-length BEFORE properties.
            if (data.size() < 28) {
                mod.metadataWarning = QStringLiteral("Missing or truncated OMOD DATA");
                return;
            }
            mod.targetType = QString::fromLatin1(data.constData() + 10, 4);
            mod.weaponModification = mod.targetType == QStringLiteral("WEAP");
            mod.attachPoint = readLittle<std::uint32_t>(data, 16);
            const auto parents = readLittle<std::uint32_t>(data, 20);
            const auto properties = readLittle<std::uint32_t>(data, 4);
            if (parents > static_cast<std::uint32_t>((data.size() - 28) / 4)) {
                mod.metadataWarning = QStringLiteral("Invalid attach-parent slot array");
                return;
            }
            for (std::uint32_t i = 0; i < parents; ++i) {
                appendUnique(mod.attachParentSlots, readLittle<std::uint32_t>(data, 24 + 4 * i));
            }
            const qsizetype propertyStart = 28 + 4 * static_cast<qsizetype>(parents);
            if (readLittle<std::uint32_t>(data, 0) != 0 ||
                propertyStart + static_cast<qsizetype>(properties) * 24 != data.size()) {
                mod.metadataWarning = QStringLiteral("Slot decoded; additional OMOD data/includes need explicit review");
                return;
            }
            for (std::uint32_t i = 0; i < properties; ++i) {
                const qsizetype offset = propertyStart + static_cast<qsizetype>(i) * 24;
                // xEdit wbObjectModProperties: byte type/function, uint16 ID,
                // two typed 32-bit values, then a float step. Keep raw values
                // until the owning configuration resolves the supported types.
                const auto id=readLittle<std::uint16_t>(data,offset+8);
                if(id==12||id==61)mod.ammoProperties.append({id,static_cast<std::uint8_t>(data[offset]),static_cast<std::uint8_t>(data[offset+4]),
                    readLittle<std::uint32_t>(data,offset+12),readLittle<std::uint32_t>(data,offset+16),readLittle<std::uint32_t>(data,offset+20)});
                if(id==25)mod.firingProperties.append({id,static_cast<std::uint8_t>(data[offset]),static_cast<std::uint8_t>(data[offset+4]),
                    readLittle<std::uint32_t>(data,offset+12),readLittle<std::uint32_t>(data,offset+16),readLittle<std::uint32_t>(data,offset+20)});
                if(id==31&&static_cast<uchar>(data[offset])==4&&static_cast<uchar>(data[offset+4])==1)
                    appendUnique(mod.removedKeywords,readLittle<std::uint32_t>(data,offset+12));
                if (readLittle<std::uint32_t>(data, offset) == 4 &&
                    readLittle<std::uint32_t>(data, offset + 4) == 2 &&
                    readLittle<std::uint32_t>(data, offset + 8) == 31) {
                    appendUnique(mod.addedKeywords, readLittle<std::uint32_t>(data, offset + 12));
                }
            }
        }

        [[nodiscard]] AnimationSubgraphRecord animationSubgraphRecord(const RawRecord& record)
        {
            AnimationSubgraphRecord result;
            result.formId = record.formId;
            result.editorId = textSubrecord(record, QByteArrayLiteral("EDID"), false);

            AnimationSubgraphEntry pending;
            for (const Subrecord& subrecord : record.subrecords) {
                // SADD is an optional additive-race FormID, not a table marker.
                // Native HumanRaceSubGraphData has SGNM/SAPT/STKD/SRAF entries
                // without SADD (xEdit wbDefinitionsFO4 RACE Subgraph Data).
                if (subrecord.type == QByteArrayLiteral("STKD") && subrecord.data.size() == 4) {
                    appendUnique(pending.requiredKeywords,
                                 readLittle<std::uint32_t>(subrecord.data, 0));
                } else if (subrecord.type == QByteArrayLiteral("SGNM")) {
                    pending.behaviorPath = decodeText(subrecord.data);
                } else if (subrecord.type == QByteArrayLiteral("SAPT")) {
                    QString path = decodeText(subrecord.data);
                    path.remove(QChar::CarriageReturn);
                    if (!path.isEmpty()) {
                        pending.animationSearchPaths.append(path);
                    }
                } else if (subrecord.type == QByteArrayLiteral("SRAF") &&
                           subrecord.data.size() == 4) {
                    pending.flags = readLittle<std::uint32_t>(subrecord.data, 0);
                    if (!pending.requiredKeywords.isEmpty() &&
                        !pending.behaviorPath.isEmpty() &&
                        !pending.animationSearchPaths.isEmpty()) {
                        result.entries.append(std::move(pending));
                    }
                    pending = AnimationSubgraphEntry{};
                }
            }
            return result;
        }
    }

    const ObjectModRecord* PluginContent::findObjectMod(const std::uint32_t formId) const noexcept
    {
        const auto iterator = std::find_if(objectMods.cbegin(), objectMods.cend(),
            [formId](const ObjectModRecord& record) { return record.formId == formId; });
        return iterator == objectMods.cend() ? nullptr : &*iterator;
    }

    const AnimationSubgraphEntry* PluginContent::resolveFirstPersonSubgraph(
        const QVector<std::uint32_t>& activeKeywords) const noexcept
    {
        const AnimationSubgraphEntry* best = nullptr;
        qsizetype bestSpecificity = -1;
        for (const AnimationSubgraphRecord& record : animationSubgraphs) {
            for (const AnimationSubgraphEntry& entry : record.entries) {
                if (!entry.firstPerson()) {
                    continue;
                }
                const bool matches = std::all_of(
                    entry.requiredKeywords.cbegin(), entry.requiredKeywords.cend(),
                    [&activeKeywords](const std::uint32_t keyword) {
                        return activeKeywords.contains(keyword);
                    });
                if (matches && entry.requiredKeywords.size() > bestSpecificity) {
                    best = &entry;
                    bestSpecificity = entry.requiredKeywords.size();
                }
            }
        }
        return best;
    }

    SoundDescriptor soundDescriptor(const PluginContent& plugin,const SoundRecord& sound) {
        SoundDescriptor result;result.record=recordIdentity(plugin,sound.formId);result.editorId=sound.editorId;result.type=sound.type;
        result.files=sound.files;result.attenuationDb=sound.attenuationDb;result.ratesOfFire=sound.ratesOfFire;result.conditions=sound.conditions;
        for(const auto child:sound.children)result.children.append(recordIdentity(plugin,child).toLower());
        return result;
    }
    QString recordIdentity(const PluginContent& plugin,std::uint32_t formId) {
        if(!formId)return {};
        const auto found=plugin.origins.constFind(formId);
        if(found!=plugin.origins.cend())return QStringLiteral("%1:%2").arg(found->plugin).arg(found->objectId,6,16,QLatin1Char('0'));
        const auto index=formId>>24;
        const auto origin=index<static_cast<std::uint32_t>(plugin.masters.size())?plugin.masters[index]:plugin.fileName;
        return QStringLiteral("%1:%2").arg(origin).arg(formId&0xffffffU,6,16,QLatin1Char('0'));
    }
    PluginContent EspReader::read(const QString& path, const std::function<bool()>& cancelled,bool referenceCatalogOnly)
    {
        PluginContent content;
        content.filePath = QFileInfo(path).absoluteFilePath();
        content.fileName = QFileInfo(path).fileName();

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            content.diagnostics.append(QStringLiteral("Cannot open plugin: %1").arg(file.errorString()));
            return content;
        }
        if (file.size() <= 0 || file.size() > 1024LL * 1024LL * 1024LL) {
            content.diagnostics.append(QStringLiteral("Plugin size is invalid"));
            return content;
        }
        uchar* mapped = file.map(0, file.size());
        if (!mapped) {
            content.diagnostics.append(QStringLiteral("Cannot map plugin: %1").arg(file.errorString()));
            return content;
        }
        const auto unmap = qScopeGuard([&]() { file.unmap(mapped); });
        const QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), file.size());

        try {
            if (signatureAt(data, 0) != QByteArrayLiteral("TES4")) {
                throw std::runtime_error("plugin does not start with a TES4 record");
            }
            ParseState state;
            state.cancelled = cancelled;
            state.referenceCatalogOnly = referenceCatalogOnly;
            const qsizetype afterHeader = parseRecord(data, 0, data.size(), state);
            if (state.records.isEmpty()) {
                throw std::runtime_error("plugin header is missing");
            }
            const RawRecord header = state.records.takeFirst();
            const bool localized = (header.flags & kLocalizedPluginFlag) != 0;
            for (const Subrecord& subrecord : header.subrecords) {
                if (subrecord.type == QByteArrayLiteral("MAST")) {
                    content.masters.append(decodeText(subrecord.data));
                }
            }
            parseRegion(data, afterHeader, data.size(), 0, state);

            struct PendingWeapon
            {
                WeaponRecord weapon;
                QByteArray objectTemplate;
            };
            QVector<PendingWeapon> pendingWeapons;

            for (const RawRecord& record : state.records) {
                if (cancelled && cancelled()) { throw std::runtime_error("Cancelled"); }
                if (record.flags & 0x20U) {
                    content.deletedRecords.append(record.formId);
                    continue;
                }
                if(localized&&(record.type=="WEAP"||record.type=="OMOD"||record.type=="AMMO"||record.type=="KYWD"))
                    if(const auto* full=firstSubrecord(record,"FULL");full&&full->data.size()==4)
                        content.localizedNameIds.insert(record.formId,readLittle<std::uint32_t>(full->data,0));
                if (record.type == QByteArrayLiteral("OMOD")) {
                    const Subrecord* dataSubrecord = firstSubrecord(
                        record, QByteArrayLiteral("DATA"));
                    ObjectModRecord mod;
                    mod.formId = record.formId;
                    mod.editorId = textSubrecord(record, QByteArrayLiteral("EDID"), false);
                    mod.name = textSubrecord(record, QByteArrayLiteral("FULL"), localized);
                    mod.modelPath = textSubrecord(record, QByteArrayLiteral("MODL"), false);
                    if (dataSubrecord) { decodeObjectModData(dataSubrecord->data, mod); }
                    else { mod.metadataWarning = QStringLiteral("Missing OMOD DATA"); }
                    for (const auto& field : record.subrecords) {
                        if (field.type == QByteArrayLiteral("MNAM")) {
                            for (auto id : formIdArray(field.data)) { appendUnique(mod.targetKeywords, id); }
                        }
                    }
                    content.objectMods.append(std::move(mod));
                } else if (record.type == QByteArrayLiteral("WEAP")) {
                    PendingWeapon pending;
                    pending.weapon.formId = record.formId;
                    pending.weapon.recordFlags = record.flags;
                    pending.weapon.editorId = textSubrecord(record, QByteArrayLiteral("EDID"), false);
                    pending.weapon.name = textSubrecord(record, QByteArrayLiteral("FULL"), localized);
                    pending.weapon.baseModelPath = textSubrecord(record, QByteArrayLiteral("MODL"), false);
                    // FO4 WEAP DNAM: Ammo FormID at 0, Flags at 48, Capacity
                    // uint16 at 52. Verified with local xEdit and Spriggit.
                    if(const auto* d=firstSubrecord(record,"DNAM");d&&d->data.size()>=55) {
                        pending.weapon.ammo=readLittle<std::uint32_t>(d->data,0);
                        pending.weapon.ammoCapacity=readLittle<std::uint16_t>(d->data,52);
                        pending.weapon.dataFlags=readLittle<std::uint32_t>(d->data,48);
                        pending.weapon.automatic=(pending.weapon.dataFlags&0x8000u)!=0;
                        // FO4 DNAM is packed: animation type and damage leave
                        // the attack-fail SNDR unaligned at byte 85 (xEdit).
                        if(d->data.size()>=89)pending.weapon.attackFailSound=readLittle<std::uint32_t>(d->data,85);
                        pending.weapon.ammoDataKnown=true;
                    }
                    if (const Subrecord* keywords = firstSubrecord(
                            record, QByteArrayLiteral("KWDA"))) {
                        pending.weapon.keywords = formIdArray(keywords->data);
                    }
                    if (const Subrecord* objectTemplate = firstSubrecord(record, QByteArrayLiteral("OBTS"))) {
                        pending.objectTemplate = objectTemplate->data;
                    }
                    if (const auto* parentSlots = firstSubrecord(record, QByteArrayLiteral("APPR"))) {
                        pending.weapon.attachParentSlots = formIdArray(parentSlots->data);
                    }
                    pendingWeapons.append(std::move(pending));
                } else if(record.type=="SNDR") {
                    SoundRecord sound; sound.formId=record.formId;sound.editorId=textSubrecord(record,"EDID",false);
                    // CNAM descriptor type: Compound 0x54651A43, AutoWeapon 0xED157AE3 (xEdit).
                    if(const auto* c=firstSubrecord(record,"CNAM");c&&c->data.size()==4){const auto type=readLittle<std::uint32_t>(c->data,0);
                        sound.type=type==0x54651A43u?SoundDescriptorType::Compound:type==0xED157AE3u?SoundDescriptorType::AutoWeapon:SoundDescriptorType::Standard;}
                    int rpm=0;
                    for(const auto& sub:record.subrecords) {
                        if(sub.type=="ANAM")sound.files.append(decodeText(sub.data));
                        else if(sub.type=="DNAM"&&sub.data.size()==4)sound.children.append(readLittle<std::uint32_t>(sub.data,0));
                        else if(sub.type=="INTV"&&sub.data.size()==4)rpm=int(readLittle<std::uint32_t>(sub.data,0));
                        else if(sub.type=="FNAM"&&rpm>0){sound.ratesOfFire.append({rpm,decodeText(sub.data)});rpm=0;}
                        // CTDA: type byte, three unused, comparison float, function index.
                        else if(sub.type=="CTDA"&&sub.data.size()>=10)
                            sound.conditions.append({static_cast<std::uint8_t>(sub.data[0]),readLittle<std::uint16_t>(sub.data,8),readLittle<float>(sub.data,4)});
                    }
                    // Standard SNDR BNAM: frequency bytes, priority, attenuation (hundredths dB).
                    if(const auto* b=firstSubrecord(record,"BNAM");b&&b->data.size()>=6)sound.attenuationDb=readLittle<std::uint16_t>(b->data,4)/100.f;
                    content.sounds.append(std::move(sound));
                } else if(record.type=="KSSM") {
                    SoundKeywordMapping mapping;mapping.formId=record.formId;mapping.editorId=textSubrecord(record,"EDID",false);
                    for(const auto& sub:record.subrecords) {
                        if(sub.type=="DNAM"&&sub.data.size()==4)mapping.descriptor=readLittle<std::uint32_t>(sub.data,0);
                        else if(sub.type=="KNAM"&&sub.data.size()==4)mapping.keywords.append(readLittle<std::uint32_t>(sub.data,0));
                    }
                    content.soundMappings.append(std::move(mapping));
                } else if(record.type=="AMMO") {
                    QString label=textSubrecord(record,"FULL",localized);
                    if(label.isEmpty()||label.startsWith("String "))label=textSubrecord(record,"EDID",false);
                    content.ammoNames.insert(record.formId,label);
                } else if (record.type == QByteArrayLiteral("KYWD")) {
                    const QString editorId=textSubrecord(record,"EDID",false);
                    const auto* kind=firstSubrecord(record,"TNAM");
                    // FO4 FunctionCall (TNAM=8): the Glock declares a named
                    // sub-branch and an explicit parent connection, not a bone alias.
                    const int split=editorId.lastIndexOf("_P-",-1,Qt::CaseInsensitive);
                    if(kind&&kind->data.size()==4&&readLittle<std::uint32_t>(kind->data,0)==8&&
                        editorId.startsWith("remapNode_",Qt::CaseInsensitive)&&split>10&&split+3<editorId.size()&&
                        !editorId.mid(10,split-10).startsWith("P-",Qt::CaseInsensitive))
                        content.nodeRemaps.insert(record.formId,{editorId.mid(10,split-10),editorId.mid(split+1)});
                    if(kind&&kind->data.size()==4&&readLittle<std::uint32_t>(kind->data,0)==6)content.soundKeywords.insert(record.formId);
                    // P-* sources remap connection metadata (e.g. the Glock's
                    // laser mount), not a node inside the incoming model.
                    QString label = textSubrecord(record, QByteArrayLiteral("FULL"), localized);
                    if (label.isEmpty() || label.startsWith("String ")) { label = editorId; }
                    content.keywordNames.insert(record.formId, label);
                } else if (record.type == QByteArrayLiteral("RACE")) {
                    AnimationSubgraphRecord subgraph = animationSubgraphRecord(record);
                    content.animationSubgraphs.append(std::move(subgraph));
                }
            }

            for (PendingWeapon& pending : pendingWeapons) {
                // OBTS has a byte-counted keyword array at offset 15 BEFORE
                // its level bytes and 7-byte includes. The fixed 18-byte offset
                // applies only to zero keywords (xEdit wbOBTSReq; verified on
                // Fallout4.esm CompPrestonLaserMusket's one-keyword template).
                if (pending.objectTemplate.size() >= 18) {
                    const auto count = readLittle<std::uint32_t>(pending.objectTemplate, 0);
                    const qsizetype includeStart = 18 + 4 * static_cast<uchar>(pending.objectTemplate.at(15));
                    if (includeStart <= pending.objectTemplate.size() &&
                        count <= static_cast<std::uint32_t>((pending.objectTemplate.size() - includeStart) / 7)) {
                        for (std::uint32_t i = 0; i < count; ++i) {
                            const auto id = readLittle<std::uint32_t>(pending.objectTemplate, includeStart + i * 7);
                            appendUnique(pending.weapon.defaultObjectMods, id);
                        }
                    } else {
                        content.diagnostics.append(QStringLiteral("Invalid default OMOD array for %1").arg(pending.weapon.editorId));
                    }
                }
                if (pending.weapon.name.isEmpty()) {
                    pending.weapon.name = pending.weapon.editorId;
                }
                content.weapons.append(std::move(pending.weapon));
            }

            content.parsed = true;
        } catch (const std::exception& exception) {
            content.diagnostics.append(QStringLiteral("Plugin parse failed: %1")
                                           .arg(QString::fromUtf8(exception.what())));
            content.weapons.clear();
            content.sounds.clear();
            content.objectMods.clear();
            content.animationSubgraphs.clear();
            content.keywordNames.clear();
            content.nodeRemaps.clear();
            content.ammoNames.clear();
            content.deletedRecords.clear();
            content.localizedNameIds.clear();
        }
        return content;
    }

    QString weaponAuthoringExclusion(const WeaponRecord& weapon) {
        // Local xEdit wbDefinitionsFO4 WEAP header bit 2 and DNAM flags bit 20.
        // Keep the raw winning record in the catalog; filter only selection.
        if((weapon.recordFlags&0x4u)||(weapon.dataFlags&0x100000u))return "Non-playable weapon";
        if(!weapon.ammoDataKnown)return "Missing weapon data";
        if(weapon.baseModelPath.trimmed().isEmpty())return "No weapon model";
        return {}; // No ammo/capacity/category requirement: melee remains editable.
    }

    void EspReader::resolveNames(PluginContent& plugin,const QString& language,
        const std::function<QByteArray(const QString&,QString&)>& readResource) {
        if(!plugin.parsed||plugin.localizedNameIds.isEmpty())return;
        const auto path="strings/"+QFileInfo(plugin.fileName).completeBaseName().toLower()+"_"+language+".strings";
        QString error;const auto bytes=readResource(path,error);QHash<std::uint32_t,QString> names;
        try {
            if(!error.isEmpty())throw std::runtime_error(error.toStdString());
            if(bytes.size()<8||bytes.size()>64*1024*1024)throw std::runtime_error("Missing or oversized STRINGS table");
            // xEdit wbLocalization.ReadDirectory: ID/offset pairs, followed by
            // zero-terminated strings; offsets are relative to the data area.
            const auto count=readLittle<std::uint32_t>(bytes,0),size=readLittle<std::uint32_t>(bytes,4);
            const quint64 start=8ull+8ull*count;
            if(count>1'000'000||start+size!=quint64(bytes.size()))throw std::runtime_error("Invalid STRINGS directory/data size");
            for(quint32 i=0;i<count;++i) {
                const auto id=readLittle<std::uint32_t>(bytes,8+8*qsizetype(i)),offset=readLittle<std::uint32_t>(bytes,12+8*qsizetype(i));
                if(offset>=size||names.contains(id))throw std::runtime_error("Invalid or duplicate STRINGS entry");
                const auto begin=qsizetype(start+offset),end=bytes.indexOf('\0',begin);
                if(end<begin||end-begin>65536)throw std::runtime_error("Unterminated or oversized localized name");
                names.insert(id,decodeText(bytes.mid(begin,end-begin)));
            }
        }catch(const std::exception& e){
            plugin.diagnostics.append("LOCALIZATION "+path+": "+QString::fromUtf8(e.what())+"; displaying editor IDs where needed");return;
        }
        int resolved=0,missing=0;QStringList missingIds;
        const auto resolve=[&](quint32 id,QString& label,const QString& editorId){
            const auto found=plugin.localizedNameIds.constFind(id);if(found==plugin.localizedNameIds.cend()||found.value()==0)return;
            const auto text=names.constFind(found.value());
            if(text!=names.cend()&&!text->isEmpty()){label=*text;++resolved;}
            else{label=editorId;++missing;if(missingIds.size()<8)missingIds.append(QString::number(found.value(),16));}
        };
        for(auto& weapon:plugin.weapons)resolve(weapon.formId,weapon.name,weapon.editorId);
        for(auto& mod:plugin.objectMods)resolve(mod.formId,mod.name,mod.editorId);
        for(auto it=plugin.ammoNames.begin();it!=plugin.ammoNames.end();++it)resolve(it.key(),it.value(),it.value());
        // KYWD editor identifiers remain stable because code uses them as role
        // labels. Only user-facing weapon, attachment and ammo names are resolved.
        plugin.diagnostics.append(QString("LOCALIZATION %1: %2 names resolved, %3 unresolved%4").arg(path).arg(resolved).arg(missing)
            .arg(missing?" (IDs "+missingIds.join(", ")+"); displaying editor IDs":QString()));
    }
}
