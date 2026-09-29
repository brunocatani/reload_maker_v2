#pragma once

#include <QByteArray>
#include <QString>
#include <string_view>

namespace reloadmaker {
    class ResourceArchive;
    // GUI-thread service. The controller installs only a completed index and
    // clears this non-owning reference before replacing/destroying its owner.
    void setLibraryResources(const ResourceArchive* resources);
    bool libraryResourcesActive();
    QString findLibraryResource(const std::string_view& path);
    bool readLibraryResource(QByteArray& bytes, const std::string_view& path);
}
