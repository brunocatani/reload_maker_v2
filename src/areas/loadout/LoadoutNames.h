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

    // Receiver records may carry only stats; a separate installed part owns the moving mesh.
    [[nodiscard]] inline bool isReceiverObjectMod(const ObjectModRecord& record)
    {
        return (record.editorId.contains(QStringLiteral("receiver"), Qt::CaseInsensitive) ||
                record.editorId.contains(QStringLiteral("reciever"), Qt::CaseInsensitive) ||
                record.name.contains(QStringLiteral("receiver"), Qt::CaseInsensitive));
    }
}
