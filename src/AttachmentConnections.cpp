#include "AttachmentConnections.h"

namespace reloadmaker
{
    AttachmentPointMatch AttachmentConnections::find(const QString& name,
                                                      const QVector<int>& preferredRoots, QString& error) const
    {
        error.clear();
        const auto inScope = [this, &name](int scope, AttachmentPointMatch& match) {
            int count = 0;
            for (int p = 0; p < _scopes.at(scope).points.size(); ++p) {
                if (_scopes.at(scope).points.at(p).name.compare(name, Qt::CaseInsensitive) == 0) {
                    match = {scope, p};
                    ++count;
                }
            }
            return count;
        };
        for (int root : preferredRoots) {
            AttachmentPointMatch match;
            int count = 0;
            for (int scope = 0; scope < _scopes.size(); ++scope) {
                if (_scopes.at(scope).root == root) { count += inScope(scope, match); }
            }
            if (count == 1) { return match; }
            if (count > 1) {
                error = QStringLiteral("Ambiguous connection %1 in model root %2").arg(name).arg(root);
                return {};
            }
        }
        // Some plugins do not declare slot ancestry, or expose their geometric
        // connection from a different installed part. Accept only a unique
        // explicit C-/P- match in that case, never the first model in load order.
        AttachmentPointMatch match;
        int count = 0;
        for (int scope = 0; scope < _scopes.size(); ++scope) { count += inScope(scope, match); }
        if (count == 1) { return match; }
        error = count ? QStringLiteral("Ambiguous connection %1 across installed models").arg(name)
                      : QStringLiteral("No installed model provides %1").arg(name);
        return {};
    }

    const AttachmentPoint* AttachmentConnections::point(AttachmentPointMatch match) const
    {
        if (!match.valid() || match.scope >= _scopes.size() || match.point >= _scopes.at(match.scope).points.size()) { return nullptr; }
        return &_scopes.at(match.scope).points.at(match.point);
    }

    int AttachmentConnections::ownerRoot(AttachmentPointMatch match) const
    {
        return point(match) ? _scopes.at(match.scope).root : -1;
    }

    int AttachmentConnections::anchor(AttachmentPointMatch match, int semanticFallback, QString& error) const
    {
        error.clear();
        const auto* connection = point(match);
        if (!connection) { error = QStringLiteral("Invalid attachment connection"); return -1; }
        const auto& model = _scopes.at(match.scope);
        if (connection->parentName.isEmpty()) { return model.root; }
        int node = -1, count = 0;
        for (const auto& candidate : model.nodes) {
            if (candidate.name.compare(connection->parentName, Qt::CaseInsensitive) == 0) { node = candidate.node; ++count; }
        }
        if (count == 1 && node >= 0) { return node; }
        if (!count && semanticFallback >= 0) { return semanticFallback; }
        error = QStringLiteral("%1 parent '%2' for %3 in model root %4")
            .arg(count ? QStringLiteral("Ambiguous") : QStringLiteral("Missing"), connection->parentName, connection->name).arg(model.root);
        return -1;
    }
}
