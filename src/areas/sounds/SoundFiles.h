#pragma once

#include "PackageSource.h"

namespace reloadmaker
{
    // A sound file's path inside the package or game archives: forward
    // slashes and no leading "data/".
    [[nodiscard]] inline QString soundResourcePath(QString path)
    {
        path.replace('\\', '/');
        if (path.startsWith("data/", Qt::CaseInsensitive)) {
            path.remove(0, 5);
        }
        return ResourceArchive::normalizeResourcePath(path);
    }
}
