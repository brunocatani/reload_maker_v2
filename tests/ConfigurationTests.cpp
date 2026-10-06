#include "WeaponConfiguration.h"
#include "SlotOverlay.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <stdexcept>
#include <limits>

using namespace reloadmaker;

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) { throw std::runtime_error(message); }
    }

    ObjectModRecord part(std::uint32_t id, std::uint32_t slot, QVector<std::uint32_t> children = {})
    {
        ObjectModRecord mod;
        mod.formId = id;
        mod.attachPoint = slot;
        mod.attachParentSlots = children;
        mod.targetKeywords = {99};
        mod.weaponModification = true;
        mod.name = QStringLiteral("[Part] %1").arg(id);
        mod.modelPath = QStringLiteral("part%1.nif").arg(id);
        return mod;
    }

    void configurationTests()
    {
        WeaponRecord weapon;
        weapon.keywords = {99};
        weapon.attachParentSlots = {10, 40};
        weapon.defaultObjectMods = {1, 3, 5, 8};
        PluginContent plugin;
        plugin.objectMods = {part(1, 10, {20}), part(2, 10, {30}), part(3, 20, {50}),
                             part(4, 30), part(5, 50), part(8, 40)};
        plugin.objectMods.last().modelPath.clear();
        auto foreign = part(6, 10);
        foreign.targetKeywords = {100};
        plugin.objectMods.append(foreign);
        auto armor = part(88, 10);
        armor.targetType = QStringLiteral("ARMO");
        armor.metadataWarning = QStringLiteral("Additional data");
        plugin.objectMods.append(armor);
        WeaponConfiguration config;
        config.initialize(plugin, weapon);
        require(config.entries().size() == 5, "slots not grouped by identity");
        require(!config.mod(6), "another weapon's choices leaked into this weapon");
        require(!config.mod(88), "non-weapon OMOD leaked into weapon slots because it had a warning");
        require(config.activeMods().contains(5), "nested default was not exposed");
        require(config.availableSlots().value(50) == 20, "nested slot parent incorrect");
        require(!config.slot(40)->hasModels && config.activeMods().contains(8), "non-mesh setting was lost");
        require(!config.select(10, 3), "cross-slot choice accepted");
        require(!config.select(10, 1), "unchanged choice scheduled work");
        require(config.select(10, 2), "receiver selection failed");
        require(config.selected(20) == 3 && config.selected(50) == 5, "unavailable choices forgotten");
        require(!config.activeMods().contains(3) && !config.activeMods().contains(5), "unavailable choices still active");
        require(config.availableSlots().contains(30), "new parent did not expose slot");
        require(config.select(10, 1) && config.activeMods().contains(5), "return to parent lost nested choice");
        require(config.select(10, 0) && !config.activeMods().contains(3), "empty parent kept its children active");
        plugin.fileName="Parts.esp";
        const auto saved=config.savedChoices(plugin);
        auto shifted=plugin;
        for(auto& mod:shifted.objectMods){
            shifted.origins.insert(mod.formId+1000,{"Parts.esp","Parts.esp",mod.formId});
            shifted.origins.insert(mod.attachPoint+1000,{"Parts.esp","Parts.esp",mod.attachPoint});
            mod.formId+=1000;mod.attachPoint+=1000;
        }
        WeaponConfiguration reopened;reopened.initialize(shifted,weapon);
        require(reopened.restoreChoices(shifted,saved).isEmpty(),"canonical choices did not survive reassigned catalog ids");
        require(reopened.selected(1010)==0&&reopened.selected(1020)==1003,"empty/nested canonical choices changed on reopen");
        auto absent=saved;absent["slot:parts.esp:00000a"]="Missing.esp:000001";
        require(reopened.restoreChoices(shifted,absent).size()==1,"missing canonical record silently used numeric choice");
        // Cyclic metadata cannot create an unbounded traversal or a cyclic UI tree.
        plugin.objectMods[2].attachParentSlots.append(10);
        config.initialize(plugin, weapon);
        require(config.availableSlots().value(10) == 0, "cycle reparented a root slot");
        // Unknown attach points remain independently selectable, never guessed exclusive.
        plugin.objectMods.append(part(9, 0));
        plugin.objectMods.append(part(11, 0));
        weapon.attachParentSlots.clear();
        weapon.defaultObjectMods.append(2);
        config.initialize(plugin, weapon);
        require(config.entries().size() == 7, "unclassified choices incorrectly merged");
        require(config.selected(10) == 1 && !config.warnings().isEmpty(), "duplicate defaults not reported");
        for (const auto& slot : config.entries()) { require(slot.unverified, "absent root metadata claimed verified"); }
        // A template's default may be a mod collection (PPSh-41, SA58, MG42):
        // the lowest-level member is the default, through nested collections.
        ObjectModRecord barrels = part(60, 10);
        barrels.targetType = QStringLiteral("NONE"); barrels.weaponModification = false; barrels.modelPath.clear();
        barrels.includes = { { 2, 24, false, true }, { 61, 0, false, true } };
        ObjectModRecord shortBarrels = barrels;
        shortBarrels.formId = 61; shortBarrels.includes = { { 4, 12, false, true }, { 1, 0, false, true } };
        plugin.objectMods.append(barrels); plugin.objectMods.append(shortBarrels);
        weapon.defaultObjectMods = {60, 3};
        config.initialize(plugin, weapon);
        require(!config.mod(60) && !config.mod(61), "a mod collection became a selectable choice");
        require(config.selected(10) == 1, "collection default did not resolve to its lowest-level member");
        require(config.activeMods().contains(3), "a direct default beside a collection was lost");
    }

    void u32(QByteArray& data, int offset, std::uint32_t value)
    {
        qToLittleEndian(value, reinterpret_cast<uchar*>(data.data() + offset));
    }

    QByteArray sub(const char* name, const QByteArray& data)
    {
        QByteArray result(name, 4);
        const auto size = qToLittleEndian(std::uint16_t(data.size()));
        result.append(reinterpret_cast<const char*>(&size), 2);
        return result + data;
    }

    QByteArray record(const char* type, std::uint32_t id, const QByteArray& fields)
    {
        QByteArray result(24, '\0');
        result.replace(qsizetype(0), 4, type, 4);
        u32(result, 4, fields.size()); u32(result, 12, id);
        return result + fields;
    }

    void readerTests()
    {
        QTemporaryDir dir;
        require(dir.isValid(), "temporary fixture unavailable");
        QByteArray data(60, '\0'); // 28 + 2 parents * 4 + one 24-byte property
        u32(data, 4, 1); data.replace(10, 4, "WEAP", 4); u32(data, 16, 10);
        u32(data, 20, 2); u32(data, 24, 20); u32(data, 28, 30);
        u32(data, 36, 4); u32(data, 40, 2); u32(data, 44, 31); u32(data, 48, 77);
        QByteArray association(4, '\0'); u32(association, 0, 99);
        QByteArray malformed = data; u32(malformed, 20, 0xffffffffU);
        QByteArray obts(25, '\0'); u32(obts, 0, 1); u32(obts, 18, 1);
        QByteArray roots(4, '\0'); u32(roots, 0, 10);
        QByteArray functionType(4,'\0');u32(functionType,0,8);
        const QByteArray bytes = record("TES4", 0, {}) +
            record("KYWD",77,sub("EDID","remapNode_Sights_Test_P-SlideSights")+sub("TNAM",functionType))+
            record("KYWD",78,sub("EDID","remapNode_Unrelated_P-SlideSights"))+
            record("KYWD",79,sub("EDID","remapNode_P-Laser_P-LaserMount")+sub("TNAM",functionType))+
            record("OMOD", 1, sub("EDID", "TestPart") + sub("DATA", data) + sub("MNAM", association)) +
            record("OMOD", 2, sub("EDID", "Truncated") + sub("DATA", malformed)) +
            record("WEAP", 3, sub("EDID", "Weapon") + sub("APPR", roots) + sub("OBTS", obts));
        const QString path = dir.filePath(QStringLiteral("fixture.esp"));
        QFile file(path); require(file.open(QIODevice::WriteOnly), "cannot create fixture");
        require(file.write(bytes) == bytes.size(), "fixture short write"); file.close();
        const auto plugin = EspReader::read(path);
        require(plugin.objectMods.size() == 2 && plugin.weapons.size() == 1, "reader dropped records");
        const auto* mod = plugin.findObjectMod(1);
        require(mod && mod->attachPoint == 10 && mod->attachParentSlots == QVector<std::uint32_t>({20,30}), "variable parent slots decoded incorrectly");
        require(mod->addedKeywords == QVector<std::uint32_t>({77}), "keyword offset ignored variable parents");
        require(mod->targetKeywords == QVector<std::uint32_t>({99}), "target association missing");
        require(!plugin.findObjectMod(2)->metadataWarning.isEmpty(), "malformed array was not surfaced");
        require(plugin.weapons.first().defaultObjectMods == QVector<std::uint32_t>({1}), "declared default include not decoded");
        require(plugin.weapons.first().attachParentSlots == QVector<std::uint32_t>({10}), "weapon root slot missing");
        require(plugin.nodeRemaps.value(77).node=="Sights_Test"&&plugin.nodeRemaps.value(77).point=="P-SlideSights", "FunctionCall remap did not preserve its node/connection");
        require(!plugin.nodeRemaps.contains(78),"Untyped keyword became an executable remap");
        require(!plugin.nodeRemaps.contains(79),"Connection metadata remap was treated as an incoming model node");
    }

    void layoutTests()
    {
        for (QSizeF size : {QSizeF(320, 240), QSizeF(900, 700), QSizeF(1800, 1000)}) {
            QVector<ProjectedSlot> points;
            for (int i = 0; i < 48; ++i) {
                // Several empty slots share a parent; other anchors approach edges.
                points.append({SlotKey(i + 1), QPointF(i < 20 ? 200 : i * 20, 150 + (i % 7) * 45)});
            }
            const auto result = layoutSlotLabels(points, size, 48);
            require(!result.isEmpty(), "layout dropped every label");
            bool focused = false;
            for (int i = 0; i < result.size(); ++i) {
                const auto& item = result.at(i);
                focused |= item.slot == 48;
                require(QRectF(QPointF(), size).contains(item.card), "label outside viewport");
                for (int j = 0; j < i; ++j) {
                    require(!result.at(j).card.intersects(item.card), "label cards overlap");
                    require(result.at(j).slot != item.slot, "duplicate visible label");
                }
            }
            require(focused, "selected slot hidden by density limit");
            if (size.width() >= 900) { require(result.size() == points.size(), "ordinary viewport dropped nested slots"); }
            const auto repeat = layoutSlotLabels(points, size, 48);
            for (int i = 0; i < result.size(); ++i) { require(result[i].card == repeat[i].card, "layout is nondeterministic"); }
        }
        require(layoutSlotLabels({}, QSizeF(900,700), 0).isEmpty(), "empty layout not empty");
        require(layoutSlotLabels({{1, QPointF(0,0)}}, QSizeF(10,10), 1).isEmpty(), "tiny viewport created invalid cards");
        require(layoutSlotLabels({{1, QPointF(std::numeric_limits<double>::quiet_NaN(),0)}}, QSizeF(900,700), 1).isEmpty(),
                "non-finite anchor accepted");
    }

    void pointerTests()
    {
        SlotPointerGesture gesture;
        require(gesture.consume(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, true), "label press leaked");
        require(gesture.consume(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton), "label drag reached viewport orbit");
        require(gesture.consume(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton), "label release leaked");
        require(!gesture.captured(), "label gesture remained latched");
        require(!gesture.consume(QEvent::MouseMove, Qt::NoButton, Qt::NoButton), "ordinary hover swallowed");
        require(!gesture.consume(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, false), "viewport press swallowed");
        require(!gesture.consume(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, true), "viewport drag crossing label swallowed");
        require(!gesture.consume(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton), "viewport release swallowed");
        require(gesture.consume(QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton, true), "double-click leaked");
        require(gesture.consume(QEvent::MouseButtonPress, Qt::RightButton, Qt::LeftButton | Qt::RightButton), "second button leaked");
        require(gesture.consume(QEvent::MouseButtonRelease, Qt::RightButton, Qt::LeftButton) && gesture.captured(), "other release ended label capture");
        require(gesture.consume(QEvent::MouseMove, Qt::NoButton, Qt::NoButton) && !gesture.captured(), "lost release did not recover");
        require(gesture.consume(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, true), "subsequent label click failed");
        require(!gesture.consume(QEvent::FocusOut, Qt::NoButton, Qt::NoButton) && !gesture.captured(), "focus loss did not cancel");
        require(!gesture.consume(QEvent::MouseButtonPress, Qt::MiddleButton, Qt::MiddleButton, true), "deliberate pan swallowed");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    try {
        configurationTests();
        readerTests();
        layoutTests();
        pointerTests();
        for (const auto& path : app.arguments().mid(1)) {
            const auto plugin = EspReader::read(path);
            require(!plugin.weapons.isEmpty() && !plugin.objectMods.isEmpty(), "real plugin has no weapon metadata");
            for (const auto& weapon : plugin.weapons) {
                WeaponConfiguration config;
                config.initialize(plugin, weapon);
                require(!config.entries().isEmpty(), "real weapon has no configuration slots");
                out << plugin.fileName << " / " << weapon.editorId << " slots=" << config.entries().size()
                    << " available=" << config.availableSlots().size() << " active=" << config.activeMods().size()
                    << " defaults=" << weapon.defaultObjectMods.size() << '\n';
                for (const auto& warning : config.warnings()) { out << "  WARNING " << warning << '\n'; }
                for (const auto& slot : config.entries()) {
                    out << "  " << slot.label << " [" << QString::number(slot.key, 16) << "] choices=" << slot.choices.size()
                        << " selected=" << QString::number(config.selected(slot.key), 16)
                        << " available=" << config.availableSlots().contains(slot.key) << '\n';
                }
            }
            if(plugin.fileName=="Glock19x.esp") {
                const auto remap=plugin.nodeRemaps.value(0x01003616);
                require(remap.node=="SightsGlock19x"&&remap.point=="P-Glock19xSights","Actual Glock remap keyword differs from inspected source");
                for(auto id:{0x01000fb1u,0x0100177du,0x010026b5u,0x01001f46u,0x0100363du})
                    require(plugin.findObjectMod(id)&&plugin.findObjectMod(id)->addedKeywords.contains(0x01003616),"Glock sight variant lost its selected remap");
            }
            if (plugin.fileName == QStringLiteral("[DeltaForce]M4A1.esp")) {
                const auto* magazine = plugin.findObjectMod(0x01001ed3);
                require(magazine && magazine->attachPoint == 0x01011a92 && magazine->attachParentSlots.size() == 2,
                        "M4 raw-record slot reference disagrees");
                require(magazine->addedKeywords.contains(0x00163048) && magazine->addedKeywords.contains(0x01001739),
                        "M4 raw-record keyword reference disagrees");
                WeaponConfiguration config;
                config.initialize(plugin, plugin.weapons.first());
                for (const std::uint32_t accessory : {0x01009146U, 0x0100392fU}) {
                    require(config.availableSlots().value(accessory) == 0x01011a90U,
                            "barrel accessory lost its owning slot");
                    require(config.slot(accessory) && config.slot(accessory)->hasModels,
                            "empty accessory omitted from physical slot catalog");
                    const auto* none = config.mod(config.selected(accessory));
                    require(none && none->modelPath.isEmpty(), "reference default is not the expected empty mount");
                }
            }
        }
        out << "Configuration and ESP metadata tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        out << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
