#include "domain/detectors/ClipEvents.h"

#include <QRegularExpression>

#include <array>
#include <optional>

namespace reloadmaker
{
    QString playedSound(const ClipEvent& event)
    {
        static const QString prefix = QStringLiteral("SoundPlay.");
        return event.text.startsWith(prefix, Qt::CaseInsensitive) ? event.text.mid(prefix.size()) : QString();
    }

    QString withoutSoundVariant(const QString& sound)
    {
        static const QRegularExpression variant(QStringLiteral("_alt\\d*$"), QRegularExpression::CaseInsensitiveOption);
        QString name = sound;
        name.remove(variant);
        return name;
    }

    namespace
    {
        // Rank of a sound name for one kind of magazine event: lower ranks
        // win; nothing when the name is not that kind of event.
        std::optional<int> entryRank(const QString& sound)
        {
            static const QRegularExpression inserting(QStringLiteral("magins(ert(ing)?|\\d+)$"), QRegularExpression::CaseInsensitiveOption);
            if (inserting.match(sound).hasMatch()) {
                return 0;
            }
            if (sound.endsWith(QStringLiteral("reloadmagin"), Qt::CaseInsensitive)) {
                return 1;
            }
            if (sound.endsWith(QStringLiteral("magin"), Qt::CaseInsensitive) && !sound.endsWith(QStringLiteral("clothmagin"), Qt::CaseInsensitive)) {
                return 2;
            }
            if (sound.endsWith(QStringLiteral("drumin"), Qt::CaseInsensitive) || sound.endsWith(QStringLiteral("magindrum"), Qt::CaseInsensitive)) {
                return 2;
            }
            return std::nullopt;
        }

        std::optional<int> removalRank(const QString& sound)
        {
            if (sound.endsWith(QStringLiteral("reloadmagout"), Qt::CaseInsensitive)) {
                return 1;
            }
            if (sound.endsWith(QStringLiteral("magout"), Qt::CaseInsensitive) && !sound.endsWith(QStringLiteral("clothmagout"), Qt::CaseInsensitive)) {
                return 0;
            }
            // A drum's own removal sound: WPNG3MagOutDrum.
            if (sound.endsWith(QStringLiteral("drumout"), Qt::CaseInsensitive) || sound.endsWith(QStringLiteral("magoutdrum"), Qt::CaseInsensitive)) {
                return 0;
            }
            return std::nullopt;
        }

        template <typename Rank>
        void pick(const QVector<ClipEvent>& events, Rank rank, const char* kind, float& seconds, QString& text, QStringList& ambiguities)
        {
            std::optional<int> best;
            int count = 0;
            for (const auto& event : events) {
                const auto r = rank(withoutSoundVariant(playedSound(event)));
                if (!r) {
                    continue;
                }
                if (!best || *r < *best) {
                    best = r;
                    count = 1;
                    seconds = event.seconds;
                    text = event.text;
                } else if (*r == *best) {
                    ++count;
                    if (event.seconds < seconds) {
                        seconds = event.seconds;
                        text = event.text;
                    }
                }
            }
            if (count > 1) {
                ambiguities << QStringLiteral("%1 magazine %2 events match equally; the earliest, %3, is used.").arg(count).arg(QLatin1String(kind), text);
            }
        }
    }

    MagazineEvents detectMagazineEvents(const QVector<ClipEvent>& events)
    {
        MagazineEvents result;
        pick(events, entryRank, "entry", result.entrySeconds, result.entry, result.ambiguities);
        pick(events, removalRank, "removal", result.removalSeconds, result.removal, result.ambiguities);
        return result;
    }
}
