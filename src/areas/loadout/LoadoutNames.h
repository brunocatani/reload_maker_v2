#pragma once

#include "EspReader.h"

#include <QString>

namespace reloadmaker
{
    // Node and bone names compare trimmed and case-insensitively.
    [[nodiscard]] inline QString normalizedKey(const QString& value)
    {
        return value.trimmed().toLower();
    }

    // A receiver OMOD is recognized by its name; P2 replaces this with a detector.
    [[nodiscard]] inline bool isReceiverObjectMod(const ObjectModRecord& record)
    {
        return !record.modelPath.isEmpty() &&
               (record.editorId.contains(QStringLiteral("receiver"), Qt::CaseInsensitive) ||
                record.editorId.contains(QStringLiteral("reciever"), Qt::CaseInsensitive) ||
                record.name.contains(QStringLiteral("receiver"), Qt::CaseInsensitive));
    }
}
