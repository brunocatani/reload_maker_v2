#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace reloadmaker
{
    // Where loading time goes (performance work, 2026-10-04): named stages
    // with their wall time, numbered so a reader can ask for what happened
    // after a point (MCP load_timings). A worker thread fills its own log and
    // the GUI thread appends it when the work is installed; one log is never
    // shared between threads.
    class LoadTimings
    {
    public:
        struct Entry
        {
            qint64 sequence{ 0 };
            QString stage;
            qint64 milliseconds{ 0 };
        };

        void add(const QString& stage, qint64 milliseconds);
        void append(const LoadTimings& other);
        // Entries after `since` (a sequence number), with the next number.
        [[nodiscard]] QJsonObject json(qint64 since = 0) const;

    private:
        static constexpr int kMaximumEntries = 2000;
        QVector<Entry> _entries;
        qint64 _next{ 1 };
    };

    // Adds the time from construction to destruction as one stage.
    class StageClock
    {
    public:
        StageClock(LoadTimings& timings, QString stage);
        ~StageClock();
        StageClock(const StageClock&) = delete;
        StageClock& operator=(const StageClock&) = delete;

    private:
        LoadTimings& _timings;
        QString _stage;
        QElapsedTimer _clock;
    };
}
