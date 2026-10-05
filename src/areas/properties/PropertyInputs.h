#pragma once

// Converts loaded sources into the plain inputs the domain detectors take.
#include "HkxReader.h"
#include "domain/detectors/ClipEvents.h"

namespace reloadmaker
{
    [[nodiscard]] inline QVector<ClipEvent> clipEvents(const HkxClip& clip)
    {
        QVector<ClipEvent> events;
        events.reserve(clip.annotations.size());
        for (const auto& annotation : clip.annotations) {
            events.append({ annotation.timeSeconds, annotation.text });
        }
        return events;
    }
}
