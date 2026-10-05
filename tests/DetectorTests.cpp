// The domain detectors against event lists copied from the testbeds' native
// clips (rework 14). Each case pins the value today's weapon behavior uses.
#include "domain/detectors/ClipEvents.h"
#include "domain/detectors/MagazinePieces.h"
#include "domain/detectors/PresetProposal.h"
#include "domain/detectors/DefaultSoundCues.h"

#include <QCoreApplication>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void check(bool value, const QString& message)
    {
        if (!value) {
            throw std::runtime_error(message.toStdString());
        }
    }

    QVector<ClipEvent> sounds(std::initializer_list<std::pair<float, const char*>> list)
    {
        QVector<ClipEvent> events;
        for (const auto& [seconds, name] : list) {
            events.append({ seconds, QStringLiteral("SoundPlay.") + QLatin1String(name) });
        }
        return events;
    }

    void expectMagazine(const char* weapon, const QVector<ClipEvent>& events, float entry, float removal, int ambiguities = 0)
    {
        const auto found = detectMagazineEvents(events);
        const QString name = QLatin1String(weapon);
        check(std::abs(found.entrySeconds - entry) < 1e-4f, name + ": entry at " + QString::number(found.entrySeconds) + " (" + found.entry + ")");
        check(std::abs(found.removalSeconds - removal) < 1e-4f, name + ": removal at " + QString::number(found.removalSeconds) + " (" + found.removal + ")");
        check(found.ambiguities.size() == ambiguities, name + ": ambiguities " + found.ambiguities.join("; "));
    }

    void magazineEvents()
    {
        // m1aanims/quick/wpnreload.hkx
        expectMagazine("M1A", sounds({ { 0.f, "WPNM1AStartReload" }, { .37f, "WPNM1AMagOut" }, { .87f, "WPNM1AGrab" }, { 1.40f, "WPNM1AMagIn" },
                                       { 2.37f, "WPNM1ABoltBack" }, { 2.53f, "WPNM1ABoltForward" }, { 2.80f, "WPNM1AEndReload" } }),
                       1.40f, .37f);
        // c14timb/large/wpnreload.hkx: the insertion stroke wins over the reload layer and the seat.
        expectMagazine("C14", sounds({ { .67f, "C14_MagSlap1" }, { 1.30f, "C14_ReloadMagOut" }, { 1.33f, "C14_MagOut" }, { 1.33f, "C14_MagRelease" },
                                       { 1.83f, "C14_ReloadMagPouch" }, { 2.47f, "C14_ReloadMagIn" }, { 2.93f, "C14_MagInserting" }, { 3.07f, "C14_MagIn" },
                                       { 3.40f, "C14_ReloadMagSlap" }, { 3.57f, "C14_MagSlap2" } }),
                       2.93f, 1.33f);
        // l118/large/wpnreload.hkx: the reload layer's MagIn before the seat; the plain MagOut.
        expectMagazine("L118A2", sounds({ { .87f, "L115A3_ReloadGrab" }, { 1.25f, "L115A3_MagOut" }, { 1.27f, "L115A3_ReloadMagOut" },
                                          { 1.72f, "L115A3_ReloadMagSwap" }, { 2.60f, "L115A3_ReloadMagIn" }, { 2.82f, "L115A3_MagIn" } }),
                       2.60f, 1.25f);
        // ru556anims/verticalgrip/_mag20/wpnreload.hkx: cloth layers are not magazine events.
        expectMagazine("RU556", sounds({ { .47f, "WPNRU556ClothMagout" }, { .77f, "WPNRU556Magout" }, { 1.60f, "WPNRU556ClothMagIn" },
                                         { 2.27f, "WPNRU556MagIn" }, { 3.30f, "WPNRU556Boltopen" } }),
                       2.27f, .77f);
        // glock19xanims/wpnreload.hkx: two insertion strokes; the first is used and reported.
        expectMagazine("Glock", sounds({ { .23f, "WPNGlock19xClothMagOut" }, { .33f, "WPNGlock19xMagOut" }, { .97f, "WPNGlock19xMagIns1" },
                                         { 1.17f, "WPNGlock19xMagIns2" }, { 1.43f, "WPNGlock19xMagIn" } }),
                       .97f, .33f, 1);
        // Other events and names without a magazine ending find nothing.
        QVector<ClipEvent> unrelated{ { .5f, QStringLiteral("reloadComplete") }, { .6f, QStringLiteral("CullBone.WeaponMagazine") } };
        unrelated += sounds({ { .7f, "WPNRU556MagSlap" }, { .8f, "WPNRU556MagCatch" } });
        expectMagazine("unrelated", unrelated, -1.f, -1.f);
        check(playedSound({ 0.f, QStringLiteral("soundplay.X") }) == QLatin1String("X"), "SoundPlay prefix is case-sensitive");
    }

    // The bolt-action testbeds carry the record flag, the Glock a pistol
    // keyword; the rifles' records do not name their action.
    void presetProposals(const QString& directory)
    {
        QVector<WeaponPreset> presets;
        QString error;
        check(loadWeaponPresets(directory, presets, error), error);
        const auto id = [&](ActionEvidence evidence) {
            const auto proposal = proposePreset(evidence, presets);
            check(proposal.preset ? proposal.problem.isEmpty() && !proposal.evidence.isEmpty() : !proposal.problem.isEmpty(), "Proposal without a reason");
            return proposal.preset ? proposal.preset->id : QString();
        };
        check(id({ true, false }) == QLatin1String("sniper.manual-bolt-detachable.v1"), "Bolt action: " + id({ true, false }));
        check(id({ false, true }) == QLatin1String("pistol.slide-detachable.v1"), "Pistol: " + id({ false, true }));
        check(id({ true, true }) == QLatin1String("sniper.manual-bolt-detachable.v1"), "The record's bolt action decides first");
        check(id({}).isEmpty(), "A rifle got a guessed preset");
        const auto pistol = std::find_if(presets.cbegin(), presets.cend(), [](const WeaponPreset& p) { return p.id == QLatin1String("pistol.slide-detachable.v1"); });
        check(pistol != presets.cend(), "No pistol preset");
        auto twice = presets;
        twice.append(*pistol);
        twice.last().id = QStringLiteral("copy");
        check(!proposePreset({ false, true }, twice).preset, "Two matching presets produced a proposal");
    }

    // Shape and material names from the testbeds' magazines. Each case is
    // the role today's per-weapon rules gave the piece.
    void magazinePieceRoles()
    {
        check(nameWords(QStringLiteral("338BrassProjectileINC")) == QStringList{ "338", "brass", "projectile", "inc" }, "Digit and camel words");
        check(nameWords(QStringLiteral("HandmadeARReceiver:77")) == QStringList{ "handmade", "ar", "receiver", "77" }, "Acronym words");
        struct Piece
        {
            const char* name;
            const char* material;
            bool cartridge;
        };
        const Piece pieces[] = {
            { "Mag:0", "Glock19x\\Glock19x\\Magazine.BGSM", false },
            { "Bullet:0", "Glock19x\\Glock19x\\Magazine.BGSM", true }, // the round shares the magazine's material
            { "MagSecond:0", "Glock19x\\Glock19x\\Magazine.BGSM", false },
            { "MagBullet:1", "RU556\\Attachments\\Bullet.BGSM", true },
            { "Mag:0", "RU556\\Attachments\\Mag_P-Mag20.BGSM", false },
            { "MagFollower:0", "M1A_404\\M1A\\M1A.BGSM", false },
            { "Bullet:1", "M1A_404\\M1A\\M1A_Bullet.BGSM", true },
            { "L118A2_magazine_10rds:0", "Weapons\\L118\\Mag10.bgsm", false }, // "rds" is not read
            { "ammo_338_LM_v009:0", "Weapons\\L118\\338Brass.BGSM", true },
            { "Magazine_Ext001:0", "Weapons\\Timberwolf\\MagExt.bgsm", false },
            { "CasingA:0", "Weapons\\HuntingRifle\\308Casings.BGSM", true },
            { "Mag_30", "serviceRifle\\HeffyM16\\AR15_Mag30rd.bgsm", false },
            { "smgMag", "Weapons\\ServiceRifle\\serviceriflemagsmg.bgsm", false },
            { "Shape:3", "Weapons\\MK18\\Ammo\\556.bgsm", true },      // an ammunition folder
            { "Shape:4", "Weapons\\Ammo\\Rifles\\Mag.bgsm", false },   // only the folder holding the file counts
        };
        for (const auto& piece : pieces) {
            const bool cartridge = proposeMagazinePieceRole(QLatin1String(piece.name), QLatin1String(piece.material)).cartridge;
            check(cartridge == piece.cartridge, QStringLiteral("Piece role: %1 / %2").arg(QLatin1String(piece.name), QLatin1String(piece.material)));
        }
        const auto evidence = proposeMagazinePieceRole(QStringLiteral("CasingA:0"), {}).evidence;
        check(evidence.contains(QLatin1String("CasingA:0")) && evidence.contains(QLatin1String("casing")), "Evidence: " + evidence);
    }

    // The testbeds' reload and bolt cycle sounds, and the cues the per-weapon
    // tables gave them. RU556 also gets its cloth magazine sound as the grab.
    void expectCues(const char* weapon, const QVector<ClipEvent>& reload, const QVector<ClipEvent>& cycle, CueAction action,
                    const QStringList& catalog, const QHash<QString, QString>& expected)
    {
        const auto has = [&](const QString& sound) { return catalog.contains(sound, Qt::CaseInsensitive); };
        const auto cues = detectSoundCues(reload, cycle, action, has);
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            check(cues.sounds.value(it.key()).compare(it.value(), Qt::CaseInsensitive) == 0,
                  QStringLiteral("%1 %2: %3 instead of %4").arg(QLatin1String(weapon), it.key(), cues.sounds.value(it.key()), it.value()));
        }
        for (auto it = cues.sounds.cbegin(); it != cues.sounds.cend(); ++it) {
            check(expected.contains(it.key()) || it.value().isEmpty(), QStringLiteral("%1 %2: unexpected %3").arg(QLatin1String(weapon), it.key(), it.value()));
        }
    }

    void soundCues()
    {
        expectCues("Glock",
                   sounds({ { .03f, "WPNGlock19xClothStart" }, { .23f, "WPNGlock19xClothMagOut" }, { .33f, "WPNGlock19xMagOut" }, { .97f, "WPNGlock19xMagIns1" },
                            { 1.17f, "WPNGlock19xMagIns2" }, { 1.43f, "WPNGlock19xMagIn" }, { 1.67f, "WPNGlock19xClothGunRotate" }, { 2.1f, "WPNGlock19xSlideRel" },
                            { 2.5f, "WPNGlock19xClothEnd" } }),
                   {}, CueAction::Slide, { "WPNGlock19xSlideBack", "WPNGlock19xSlideForward" },
                   { { "pull", "WPNGlock19xSlideBack" }, { "forward", "WPNGlock19xSlideForward" }, { "mag-out", "WPNGlock19xMagOut" },
                     { "mag-entry", "WPNGlock19xMagIns1" }, { "mag-insert", "WPNGlock19xMagIns2" }, { "mag-seat", "WPNGlock19xMagIn" } });
        expectCues("RU556",
                   sounds({ { .47f, "WPNRU556ClothMagout" }, { .77f, "WPNRU556Magout" }, { 1.6f, "WPNRU556ClothMagIn" }, { 2.27f, "WPNRU556MagIn" },
                            { 3.3f, "WPNRU556Boltopen" }, { 3.73f, "WPNRU556Boltclose" } }),
                   {}, CueAction::ChargingHandle, { "WPNRU556BoltRelease" },
                   { { "pull", "WPNRU556Boltopen" }, { "forward", "WPNRU556Boltclose" }, { "mag-out", "WPNRU556Magout" }, { "mag-grab", "WPNRU556ClothMagIn" },
                     { "mag-seat", "WPNRU556MagIn" }, { "bolt-release", "WPNRU556BoltRelease" } });
        expectCues("M1A",
                   sounds({ { 0.f, "WPNM1AStartReload" }, { .37f, "WPNM1AMagOut" }, { .87f, "WPNM1AGrab" }, { 1.4f, "WPNM1AMagIn" }, { 2.37f, "WPNM1ABoltBack" },
                            { 2.53f, "WPNM1ABoltForward" }, { 2.8f, "WPNM1AEndReload" } }),
                   {}, CueAction::OperatingHandle, {},
                   { { "pull", "WPNM1ABoltBack" }, { "forward", "WPNM1ABoltForward" }, { "mag-out", "WPNM1AMagOut" }, { "mag-grab", "WPNM1AGrab" },
                     { "mag-seat", "WPNM1AMagIn" } });
        // The SREP's charging handle and bolt make no sound in its reload; the
        // catalog supplies the RU556 sounds it shares.
        expectCues("SREP",
                   sounds({ { .17f, "WPNRU556ClothMagout" }, { .4f, "WPNRU556Magout" }, { .97f, "WPNRU556ClothMagIn" }, { 1.53f, "WPNRU556Magin" },
                            { 2.17f, "WPNRU556MagSlap" }, { 2.2f, "WPNRU556Clothforward" }, { 3.1f, "WPNRU556MagCatch" } }),
                   {}, CueAction::ReciprocatingBolt, { "WPNRU556Boltopen", "WPNRU556BoltBack", "WPNRU556BoltClose", "WPNRU556BoltRelease" },
                   { { "pull", "WPNRU556Boltopen" }, { "forward", "WPNRU556BoltClose" }, { "mag-out", "WPNRU556Magout" }, { "mag-grab", "WPNRU556ClothMagIn" },
                     { "mag-entry", "WPNRU556Magin" }, { "mag-seat", "WPNRU556MagSlap" }, { "bolt-release", "WPNRU556MagCatch" } });
        expectCues("Timberwolf",
                   sounds({ { 0.f, "C14_ReloadStart" }, { .67f, "C14_MagSlap1" }, { 1.3f, "C14_ReloadMagOut" }, { 1.33f, "C14_MagOut" }, { 1.33f, "C14_MagRelease" },
                            { 1.83f, "C14_ReloadMagPouch" }, { 2.47f, "C14_ReloadMagIn" }, { 2.93f, "C14_MagInserting" }, { 3.07f, "C14_MagIn" },
                            { 3.4f, "C14_ReloadMagSlap" }, { 3.57f, "C14_MagSlap2" }, { 4.17f, "C14_ReloadBolt" }, { 4.67f, "C14_BoltUnlatch" },
                            { 4.83f, "C14_BoltBack" }, { 5.07f, "C14_BoltForward" }, { 5.37f, "C14_BoltLatch" } }),
                   sounds({ { .3f, "C14_ReloadMagSlap" }, { .45f, "C14_BoltUnlatch" }, { .5f, "C14_BoltBack" }, { .53f, "C14_ReloadBolt" },
                            { .72f, "C14_BoltForward" }, { .9f, "C14_BoltLatch" } }),
                   CueAction::BoltAction, {},
                   { { "unlock", "C14_BoltUnlatch" }, { "pull", "C14_BoltBack" }, { "forward", "C14_BoltForward" }, { "lock", "C14_BoltLatch" },
                     { "mag-release", "C14_MagRelease" }, { "mag-out", "C14_MagOut" }, { "mag-entry", "C14_MagInserting" }, { "mag-seat", "C14_MagIn" } });
        expectCues("L118A2",
                   sounds({ { .35f, "L115A3_BoltUnlatch" }, { .45f, "L115A3_BoltBackEmpty" }, { .48f, "L115A3_ReloadBoltBack" }, { .87f, "L115A3_ReloadGrab" },
                            { 1.25f, "L115A3_MagOut" }, { 1.27f, "L115A3_ReloadMagOut" }, { 1.72f, "L115A3_ReloadMagSwap" }, { 2.6f, "L115A3_ReloadMagIn" },
                            { 2.82f, "L115A3_MagIn" }, { 3.37f, "L115A3_ReloadGrab" }, { 3.58f, "L115A3_BoltForward" }, { 3.6f, "L115A3_ReloadBoltForward" },
                            { 3.77f, "L115A3_BoltLatch" } }),
                   sounds({ { .17f, "L115A3_ReloadGrab" }, { .53f, "L115A3_BoltUnlatch" }, { .58f, "L115A3_BoltBack" }, { .67f, "L115A3_ReloadBoltBack" },
                            { .78f, "L115A3_ReloadBoltForward" }, { .82f, "L115A3_BoltForward" }, { 1.02f, "L115A3_BoltLatch" } }),
                   CueAction::BoltAction, { "L115A3_MagRelease" },
                   { { "unlock", "L115A3_BoltUnlatch" }, { "pull", "L115A3_BoltBack" }, { "forward", "L115A3_BoltForward" }, { "lock", "L115A3_BoltLatch" },
                     { "mag-release", "L115A3_MagRelease" }, { "mag-out", "L115A3_MagOut" }, { "mag-entry", "L115A3_ReloadMagIn" }, { "mag-seat", "L115A3_MagIn" } });
        // A numbered variant still names its cue; a reload-layer bolt sound
        // stands in when no plain one plays.
        const auto variant = detectSoundCues(sounds({ { .2f, "WPNXMagOut141" }, { .9f, "WPNXMagInserting" }, { 1.1f, "WPNXMagIn141" },
                                                      { 2.f, "WPNRifleReloadBoltOpen" }, { 2.4f, "WPNRifleReloadBoltClose" } }),
                                             {}, CueAction::ChargingHandle, [](const QString&) { return false; });
        check(variant.sounds.value("mag-out") == QLatin1String("WPNXMagOut141") && variant.prefix == QLatin1String("WPNX") &&
                  variant.sounds.value("mag-seat") == QLatin1String("WPNXMagIn141") && variant.sounds.value("pull") == QLatin1String("WPNRifleReloadBoltOpen") &&
                  variant.sounds.value("forward") == QLatin1String("WPNRifleReloadBoltClose"),
              "Numbered or reload-layer sounds were not read");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        if (argc < 2) {
            throw std::runtime_error("usage: DetectorTests <assets/presets>");
        }
        magazineEvents();
        presetProposals(QString::fromLocal8Bit(argv[1]));
        magazinePieceRoles();
        soundCues();
        QTextStream(stdout) << "PASS magazine events from clip sound names, preset proposals from records, magazine piece roles from names, "
                               "default sound cues from clip sounds\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << e.what() << '\n';
        return 1;
    }
}
