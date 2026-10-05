// The domain detectors against event lists copied from the testbeds' native
// clips (rework 14). Each case pins the value today's weapon behavior uses.
#include "domain/detectors/ClipEvents.h"
#include "domain/detectors/PresetProposal.h"

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
        QTextStream(stdout) << "PASS magazine events from clip sound names, preset proposals from records\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << e.what() << '\n';
        return 1;
    }
}
