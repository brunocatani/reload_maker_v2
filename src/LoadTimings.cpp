#include "LoadTimings.h"

#include <QJsonArray>

namespace reloadmaker
{
    void LoadTimings::add(const QString& stage, qint64 milliseconds)
    {
        _entries.append({ _next++, stage, milliseconds });
        if (_entries.size() > kMaximumEntries) {
            _entries.remove(0, _entries.size() - kMaximumEntries);
        }
    }

    void LoadTimings::append(const LoadTimings& other)
    {
        for (const auto& entry : other._entries) {
            add(entry.stage, entry.milliseconds);
        }
    }

    QJsonObject LoadTimings::json(qint64 since) const
    {
        QJsonArray entries;
        for (const auto& entry : _entries) {
            if (entry.sequence > since) {
                entries.append(QJsonObject{ { "sequence", entry.sequence }, { "stage", entry.stage }, { "milliseconds", entry.milliseconds } });
            }
        }
        return { { "entries", entries }, { "next", _next } };
    }

    StageClock::StageClock(LoadTimings& timings, QString stage)
        : _timings(timings), _stage(std::move(stage))
    {
        _clock.start();
    }

    StageClock::~StageClock()
    {
        _timings.add(_stage, _clock.elapsed());
    }
}
