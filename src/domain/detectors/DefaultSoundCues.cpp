#include "domain/detectors/DefaultSoundCues.h"

#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <optional>

namespace reloadmaker
{
    namespace
    {
        struct Played
        {
            float seconds{ 0.0f };
            QString sound;
        };

        QVector<Played> played(const QVector<ClipEvent>& events)
        {
            QVector<Played> result;
            for (const auto& event : events) {
                if (const auto sound = playedSound(event); !sound.isEmpty()) {
                    result.append({ event.seconds, sound });
                }
            }
            std::stable_sort(result.begin(), result.end(), [](const Played& a, const Played& b) { return a.seconds < b.seconds; });
            return result;
        }

        // The lower-case name without a trailing number ("MagIn141" -> "magin").
        QString base(const QString& sound)
        {
            QString name = sound.toLower();
            while (!name.isEmpty() && name.back().isDigit()) {
                name.chop(1);
            }
            return name;
        }

        bool endsWithAny(const QString& name, const QStringList& endings)
        {
            for (const auto& ending : endings) {
                if (name.endsWith(ending)) {
                    return true;
                }
            }
            return false;
        }

        // The earliest sound of the lowest rank, at or after `after`.
        template <typename Rank>
        std::optional<Played> pick(const QVector<Played>& sounds, Rank rank, float after = -std::numeric_limits<float>::infinity())
        {
            std::optional<Played> best;
            std::optional<int> bestRank;
            for (const auto& sound : sounds) {
                if (sound.seconds < after) {
                    continue;
                }
                const auto r = rank(base(sound.sound));
                if (r && (!bestRank || *r < *bestRank)) {
                    best = sound;
                    bestRank = r;
                }
            }
            return best;
        }

        // Operating sounds: plain names first, then "Reload…" / "…Empty" ones.
        auto operating(const QStringList& endings, const QStringList& lowEndings = {})
        {
            return [endings, lowEndings](const QString& name) -> std::optional<int> {
                if (name.contains(QLatin1String("cloth"))) {
                    return std::nullopt;
                }
                const int reloadRank = name.contains(QLatin1String("reload")) || name.contains(QLatin1String("empty")) ? 1 : 0;
                if (endsWithAny(name, endings)) {
                    return reloadRank;
                }
                if (endsWithAny(name, lowEndings)) {
                    return 2 + reloadRank;
                }
                return std::nullopt;
            };
        }
    }

    DefaultSoundCues detectSoundCues(const QVector<ClipEvent>& reloadEvents, const QVector<ClipEvent>& cycleEvents, CueAction action,
                                     const std::function<bool(const QString&)>& catalogHas)
    {
        DefaultSoundCues result;
        const auto reload = played(reloadEvents);
        const auto cycle = played(cycleEvents);
        const bool boltAction = action == CueAction::BoltAction;
        const bool handle = action == CueAction::ChargingHandle || action == CueAction::ReciprocatingBolt;

        const auto out = pick(reload, [](const QString& name) -> std::optional<int> {
            if (!name.endsWith(QLatin1String("magout")) || name.endsWith(QLatin1String("clothmagout"))) {
                return std::nullopt;
            }
            return name.endsWith(QLatin1String("reloadmagout")) ? 1 : 0;
        });
        if (out) {
            result.sounds["mag-out"] = out->sound;
            result.prefix = out->sound.left(base(out->sound).size() - int(qstrlen("magout")));
        } else if (!reload.isEmpty()) {
            QString common = reload.front().sound;
            for (const auto& sound : reload) {
                while (!common.isEmpty() && !sound.sound.startsWith(common, Qt::CaseInsensitive)) {
                    common.chop(1);
                }
            }
            result.prefix = common.size() >= 3 ? common : QString();
        }
        const auto fromCatalog = [&](const QStringList& names, const QString& except = {}) {
            if (result.prefix.isEmpty()) {
                return QString();
            }
            for (const auto& name : names) {
                const QString sound = result.prefix + name;
                if (sound.compare(except, Qt::CaseInsensitive) != 0 && catalogHas(sound)) {
                    return sound;
                }
            }
            return QString();
        };
        // Bolt actions are worked in the bolt cycle clip; the reload may repeat it.
        const auto operatingSound = [&](auto rank, const QStringList& conventional) {
            if (boltAction) {
                if (const auto found = pick(cycle, rank)) {
                    return found->sound;
                }
            }
            if (const auto found = pick(reload, rank)) {
                return found->sound;
            }
            return fromCatalog(conventional);
        };

        if (boltAction) {
            result.sounds["unlock"] = operatingSound(operating({ "boltunlatch", "boltunlock" }), { "BoltUnlatch" });
            result.sounds["lock"] = operatingSound(operating({ "boltlatch", "boltlock" }), { "BoltLatch" });
        }
        const QStringList pullNames = action == CueAction::Slide ? QStringList{ "SlideBack" }
                                      : handle                   ? QStringList{ "Boltopen", "BoltBack" }
                                                                 : QStringList{ "BoltBack" };
        result.sounds["pull"] = operatingSound(operating({ "boltback", "boltopen", "slideback" }), pullNames);
        const QStringList forwardNames = action == CueAction::Slide ? QStringList{ "SlideForward" }
                                         : handle                   ? QStringList{ "Boltclose", "BoltForward" }
                                                                    : QStringList{ "BoltForward" };
        const auto forwardRank = operating({ "boltforward", "boltclose", "slideforward" },
                                           action == CueAction::ReciprocatingBolt ? QStringList{ "boltrelease" } : QStringList{});
        result.sounds["forward"] = operatingSound(forwardRank, forwardNames);

        if (const auto release = pick(reload, operating({ "magrelease" }))) {
            result.sounds["mag-release"] = release->sound;
        } else {
            result.sounds["mag-release"] = fromCatalog({ "MagRelease" });
        }

        const auto grab = pick(reload, [](const QString& name) -> std::optional<int> {
            if (name.contains(QLatin1String("grab")) && name.contains(QLatin1String("mag"))) {
                return 0;
            }
            if (name.endsWith(QLatin1String("grab")) && !name.contains(QLatin1String("reload")) && !name.contains(QLatin1String("cloth"))) {
                return 1;
            }
            if (name.contains(QLatin1String("cloth")) && name.endsWith(QLatin1String("magin"))) {
                return 2;
            }
            return std::nullopt;
        });
        result.sounds["mag-grab"] = grab ? grab->sound : QString();

        const auto magazine = detectMagazineEvents(reloadEvents);
        QString entry = playedSound({ magazine.entrySeconds, magazine.entry });
        QString seat;
        if (!entry.isEmpty()) {
            const auto later = [&](auto rank) { return pick(reload, rank, magazine.entrySeconds + 1e-4f); };
            const auto plainIn = later([&](const QString& name) -> std::optional<int> {
                const bool magIn = name.endsWith(QLatin1String("magin")) && !name.endsWith(QLatin1String("clothmagin")) &&
                                   !name.endsWith(QLatin1String("reloadmagin"));
                return magIn ? std::optional<int>(0) : std::nullopt;
            });
            const auto slap = later([](const QString& name) {
                return endsWithAny(name, { "magslap", "magslam" }) ? std::optional<int>(0) : std::nullopt;
            });
            if (plainIn) {
                seat = plainIn->sound;
            } else if (slap) {
                seat = slap->sound;
            } else if (base(entry).endsWith(QLatin1String("magin")) && !base(entry).endsWith(QLatin1String("reloadmagin"))) {
                seat = entry; // one insertion sound: it seats the magazine
                entry.clear();
            }
            static const QRegularExpression stroke(QStringLiteral("magins(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
            if (const auto match = stroke.match(entry); match.hasMatch()) {
                const QString next = entry.left(match.capturedStart(1)) + QString::number(match.captured(1).toInt() + 1);
                for (const auto& sound : reload) {
                    if (sound.seconds > magazine.entrySeconds && sound.sound.compare(next, Qt::CaseInsensitive) == 0) {
                        result.sounds["mag-insert"] = sound.sound;
                        break;
                    }
                }
            }
        }
        result.sounds["mag-entry"] = entry;
        result.sounds["mag-seat"] = seat;

        if (handle) {
            const QString forward = result.sounds.value("forward");
            const auto release = pick(reload, [&](const QString& name) -> std::optional<int> {
                if (name.compare(base(forward)) == 0) {
                    return std::nullopt;
                }
                if (name.endsWith(QLatin1String("magcatch")) || name.contains(QLatin1String("boltcatch"))) {
                    return 0;
                }
                return name.endsWith(QLatin1String("boltrelease")) ? std::optional<int>(1) : std::nullopt;
            });
            result.sounds["bolt-release"] = release ? release->sound : fromCatalog({ "BoltCatchPress", "BoltRelease" }, forward);
        }
        return result;
    }
}
