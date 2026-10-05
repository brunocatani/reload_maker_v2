#pragma once

#include "HkxReader.h"
#include <utility>

namespace reloadmaker
{
    struct AttachmentNodeName
    {
        int node{-1};
        QString name;
    };

    struct AttachmentPoint
    {
        QString name;
        QString parentName;
        Transformf local;
    };

    struct AttachmentModelScope
    {
        int root{-1};
        QVector<AttachmentNodeName> nodes;
        QVector<AttachmentPoint> points;
    };

    struct AttachmentPointMatch
    {
        int scope{-1};
        int point{-1};
        [[nodiscard]] bool valid() const { return scope >= 0 && point >= 0; }
    };

    // Assembly-local block identities, rebuilt with the scene. No source NIF or
    // renderer pointers are retained, and this registry is not used per frame.
    class AttachmentConnections final
    {
    public:
        void clear() { _scopes.clear(); }
        void add(AttachmentModelScope scope) { _scopes.append(std::move(scope)); }
        [[nodiscard]] AttachmentPointMatch find(const QString& name,
                                                const QVector<int>& preferredRoots, QString& error) const;
        [[nodiscard]] const AttachmentPoint* point(AttachmentPointMatch match) const;
        [[nodiscard]] int ownerRoot(AttachmentPointMatch match) const;
        // semanticFallback is the adapter's frame for a parent the owner model
        // does not emit: an explicitly identified rig bone, or the owner root
        // for a label that names no node or bone; never an arbitrary global
        // name match. An ambiguous local name always fails.
        [[nodiscard]] int anchor(AttachmentPointMatch match, int semanticFallback, QString& error) const;
    private:
        QVector<AttachmentModelScope> _scopes;
    };
}
