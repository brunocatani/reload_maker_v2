#include "LibraryResources.h"
#include "PackageSource.h"
#include <QDebug>
#include <QSet>

namespace reloadmaker {
    namespace {
        const ResourceArchive* activeResources = nullptr;
        QSet<QString> reportedFailures;
    }
    void setLibraryResources(const ResourceArchive* resources) {
        activeResources = resources;
        reportedFailures.clear();
    }
    bool libraryResourcesActive() { return activeResources != nullptr; }
    QString findLibraryResource(const std::string_view& path) {
        const auto key = QString::fromUtf8(path.data(), path.size());
        return activeResources && activeResources->contains(key) ? key : QString();
    }
    bool readLibraryResource(QByteArray& bytes, const std::string_view& path) {
        bytes.clear();
        if (!activeResources) { return false; }
        const auto key = QString::fromUtf8(path.data(), path.size());
        if (!activeResources->contains(key)) { return false; }
        QString error;
        bytes = activeResources->read(key, error);
        if (!error.isEmpty() && !reportedFailures.contains(key)) {
            reportedFailures.insert(key);
            qWarning().noquote() << error;
        }
        return error.isEmpty();
    }
}
