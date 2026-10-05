// The weapon's native clip catalog: the first-person behavior graph chosen by
// the active keywords, the clips it lists resolved through its animation
// search stack, and reading those clip resources.
#include "ReloadMakerController.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace reloadmaker
{
    namespace
    {
        // Graph and search paths may omit the meshes/ root.
        QString meshesPath(QString path)
        {
            if (path.startsWith(QStringLiteral("actors/"), Qt::CaseInsensitive)) {
                path.prepend(QStringLiteral("meshes/"));
            }
            return path;
        }

        QString clipBaseName(QString animationPath)
        {
            animationPath.replace('\\', '/');
            return QFileInfo(animationPath).completeBaseName();
        }

        // Where a behavior graph reference points, relative to the referencing graph.
        QString resolveGraphReference(const QString& graphPath, const QString& reference)
        {
            const QString normalized = ResourceArchive::normalizeResourcePath(reference);
            if (normalized.startsWith(QStringLiteral("meshes/"), Qt::CaseInsensitive)) {
                return normalized;
            }
            if (normalized.startsWith(QStringLiteral("actors/"), Qt::CaseInsensitive)) {
                return QStringLiteral("meshes/%1").arg(normalized);
            }
            const QString directory = QFileInfo(graphPath).path();
            if (normalized.startsWith(QStringLiteral("behaviors/"), Qt::CaseInsensitive)) {
                return QStringLiteral("%1/%2").arg(QFileInfo(directory).path(), normalized);
            }
            return QStringLiteral("%1/%2").arg(directory, normalized);
        }
    }

    // Keywords that select the animation subgraph: the weapon's, those added
    // by its configured object mods, and the resolved receiver's.
    QVector<std::uint32_t> ReloadMakerController::activeAnimationKeywords() const
    {
        QVector<std::uint32_t> result;
        const auto append = [&result](const std::uint32_t keyword) {
            if (!result.contains(keyword)) {
                result.append(keyword);
            }
        };
        if (const WeaponRecord* weapon = currentWeapon()) {
            for (const std::uint32_t keyword : weapon->keywords) {
                append(keyword);
            }
        }
        for (const ObjectModRecord* objectMod : configuredObjectMods()) {
            if (!objectMod) {
                continue;
            }
            for (const std::uint32_t keyword : objectMod->addedKeywords) {
                append(keyword);
            }
        }
        if (const ObjectModRecord* receiver = resolvedReceiverObjectMod()) {
            for (const std::uint32_t keyword : receiver->addedKeywords) {
                append(keyword);
            }
        }
        return result;
    }

    // Reads an animation resource from the package or the game. The first
    // read of each resource records its SHA-256 for source identity.
    QByteArray ReloadMakerController::readAnimationResource(const QString& resourcePath, QString& error) const
    {
        error.clear();
        const auto read = [&](const ResourceArchive& archive) {
            auto bytes = archive.read(resourcePath, error);
            const auto key = ResourceArchive::normalizeResourcePath(resourcePath);
            if (error.isEmpty() && !bytes.isEmpty() && !_animationReadHashes.contains(key)) {
                _animationReadHashes.insert(key, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            }
            return bytes;
        };
        if (_libraryMode || !_package.resourceData.isEmpty()) {
            return read(_packageResources);
        }
        if (_packageResources.contains(resourcePath)) {
            return read(_packageResources);
        }
        if (_gameAnimations.contains(resourcePath)) {
            return read(_gameAnimations);
        }
        error = QStringLiteral("Animation resource is missing: %1").arg(resourcePath);
        return {};
    }

    // Resolves a graph's clip path by file name through the subgraph's
    // animation search stack, first match wins. Empty when no folder has it.
    QString ReloadMakerController::resolveAnimationPath(const QString& graphAnimationPath) const
    {
        QString normalized = ResourceArchive::normalizeResourcePath(graphAnimationPath);
        if (normalized.endsWith(QStringLiteral(".hkt"), Qt::CaseInsensitive)) {
            normalized.chop(4);
            normalized.append(QStringLiteral(".hkx"));
        }
        const QString fileName = QFileInfo(normalized).fileName();
        if (fileName.isEmpty()) {
            return {};
        }
        for (const QString& searchPath : _animationSearchPaths) {
            const QString directory = meshesPath(ResourceArchive::normalizeResourcePath(searchPath));
            const QString candidate = ResourceArchive::normalizeResourcePath(QStringLiteral("%1/%2").arg(directory, fileName));
            const bool gameFallback = !_libraryMode && _package.resourceData.isEmpty() && _gameAnimations.contains(candidate);
            if (_packageResources.contains(candidate) || gameFallback) {
                return candidate;
            }
        }
        return {};
    }

    // The path below the animation set's own folder (the shortest search folder
    // holding it), for display; the whole path when no search folder holds it.
    QString ReloadMakerController::animationSetRelativePath(const QString& path) const
    {
        int base = -1;
        for (const QString& searchPath : _animationSearchPaths) {
            const QString directory = meshesPath(ResourceArchive::normalizeResourcePath(searchPath)) + '/';
            if (path.startsWith(directory, Qt::CaseInsensitive) && (base < 0 || directory.size() < base)) {
                base = directory.size();
            }
        }
        return base < 0 ? path : path.mid(base);
    }

    // Rebuilds _animationActions for the selected configuration: the graph's
    // Ready pose first, then every authored clip the graph lists, ordered by
    // action family.
    bool ReloadMakerController::rebuildAnimationActions(QString& error)
    {
        error.clear();
        _animationActions.clear();
        _animationSearchPaths.clear();
        const PluginContent* plugin = currentPlugin();
        const WeaponRecord* weapon = currentWeapon();
        if (!plugin || !weapon) {
            error = QStringLiteral("No weapon configuration is selected");
            return false;
        }
        const QVector<std::uint32_t> keywords = activeAnimationKeywords();
        const auto* subgraph = plugin->resolveFirstPersonSubgraph(keywords);
        const QString subgraphAuthority = QStringLiteral("automatic-keywords");
        if (!subgraph) {
            error = QStringLiteral("The equipped weapon and OMOD keywords match no first-person animation subgraph.");
            return false;
        }
        _animationSearchPaths = subgraph->animationSearchPaths;
        const QString rootBehavior = meshesPath(ResourceArchive::normalizeResourcePath(subgraph->behaviorPath));
        HkxBehaviorGraph combined;
        QSet<QString> visited;
        if (!loadBehaviorGraph(rootBehavior, 0, combined, visited, error) || combined.clips.isEmpty()) {
            if (error.isEmpty()) {
                error = QStringLiteral("The resolved behavior graph contains no animation clips");
            }
            return false;
        }
        const QString readyResource = resolveAnimationPath(combined.defaultReadyAnimationPath);
        if (readyResource.isEmpty()) {
            error = QStringLiteral("The graph-selected Ready pose cannot be resolved through the RACE search stack");
            return false;
        }
        const QString readyName = QFileInfo(ResourceArchive::normalizeResourcePath(combined.defaultReadyAnimationPath)).completeBaseName();
        const auto readyClip = std::find_if(combined.clips.cbegin(), combined.clips.cend(), [&readyName](const HkxBehaviorClip& clip) {
            return clipBaseName(clip.animationPath).compare(readyName, Qt::CaseInsensitive) == 0;
        });
        _animationActions.append(AnimationAction{
            QStringLiteral("Idle / Ready"),
            readyClip != combined.clips.cend() ? readyClip->nodeName : QStringLiteral("Graph default Ready"),
            readyResource,
            readyResource,
            0.0f,
            0.0f,
            ActionFamily::Idle,
            false,
            false,
        });
        _animationActions.append(authoredGraphActions(combined, readyResource));
        QStringList keywordText;
        for (const std::uint32_t keyword : keywords) {
            keywordText.append(formIdText(keyword));
        }
        appendDiagnostic(QStringLiteral("SUBGRAPH  %1  authority=%2  keywords=[%3]")
                             .arg(rootBehavior, subgraphAuthority, keywordText.join(QStringLiteral(", "))));
        for (qsizetype index = 0; index < _animationSearchPaths.size(); ++index) {
            appendDiagnostic(QStringLiteral("  SEARCH %1  %2").arg(index + 1).arg(_animationSearchPaths.at(index)));
        }
        appendDiagnostic(QStringLiteral("GRAPH  clips=%1 actions=%2 ready=%3")
                             .arg(combined.clips.size())
                             .arg(_animationActions.size())
                             .arg(readyResource));
        return true;
    }

    // Merges a behavior graph and the graphs it references, depth first, into
    // `combined`. A clip listed twice with the same node and path is kept once;
    // the last graph with a default Ready pose sets it.
    bool ReloadMakerController::loadBehaviorGraph(const QString& requestedPath, int depth, HkxBehaviorGraph& combined,
                                                  QSet<QString>& visited, QString& error)
    {
        if (depth > 8) {
            error = QStringLiteral("Behavior graph references are nested too deeply");
            return false;
        }
        const QString path = ResourceArchive::normalizeResourcePath(requestedPath);
        if (visited.contains(path)) {
            return true;
        }
        visited.insert(path);
        QString readError;
        HkxBehaviorGraph graph;
        if (const auto* cached = _graphCache.object(path)) {
            graph = *cached;
        } else {
            const QByteArray bytes = readAnimationResource(path, readError);
            if (bytes.isEmpty()) {
                error = readError;
                return false;
            }
            graph = HkxReader::readBehaviorGraph(bytes, readError);
            if (graph.valid()) {
                ++_sourceMisses;
                _graphCache.insert(path, new HkxBehaviorGraph(graph));
            }
        }
        if (!graph.valid()) {
            error = QStringLiteral("Cannot parse behavior graph '%1': %2").arg(path, readError);
            return false;
        }
        for (const HkxBehaviorClip& clip : graph.clips) {
            const bool duplicate = std::any_of(combined.clips.cbegin(), combined.clips.cend(), [&clip](const HkxBehaviorClip& current) {
                return current.nodeName == clip.nodeName && current.animationPath == clip.animationPath;
            });
            if (!duplicate) {
                combined.clips.append(clip);
            }
        }
        if (!graph.defaultReadyAnimationPath.isEmpty()) {
            combined.defaultReadyAnimationPath = graph.defaultReadyAnimationPath;
        }
        for (const QString& reference : graph.behaviorReferences) {
            if (!loadBehaviorGraph(resolveGraphReference(path, reference), depth + 1, combined, visited, error)) {
                return false;
            }
        }
        return true;
    }

    // The graph, not a fixed alias list, decides which authored actions exist.
    // Each clip is classified by base name and resolved through the RACE
    // search stack; clips absent for this weapon are skipped. The first clip
    // of each label wins, and actions are ordered by family, then by the
    // classifier's order.
    QVector<ReloadMakerController::AnimationAction> ReloadMakerController::authoredGraphActions(const HkxBehaviorGraph& graph,
                                                                                              const QString& readyResource)
    {
        struct Candidate
        {
            AnimationAction action;
            int order;
        };
        QVector<Candidate> candidates;
        QSet<QString> labels;
        for (const HkxBehaviorClip& clip : graph.clips) {
            const auto kind = classifyWeaponClip(clipBaseName(clip.animationPath));
            if (!kind || labels.contains(kind->label)) {
                continue;
            }
            QString resource = resolveAnimationPath(clip.animationPath);
            if (kind->baseReload && _glockNoExtender) {
                const auto alternative = noExtenderReloadPath();
                if (!alternative.isEmpty()) {
                    resource = alternative;
                } else {
                    _glockNoExtender = false;
                }
            }
            if (resource.isEmpty()) {
                continue;
            }
            labels.insert(kind->label);
            candidates.append({ AnimationAction{ kind->label, clip.nodeName, resource, readyResource, clip.blendInSeconds,
                                                 clip.blendOutSeconds, kind->family, kind->automatic, kind->baseReload },
                                kind->order });
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.action.family != b.action.family) {
                return int(a.action.family) < int(b.action.family);
            }
            return a.order < b.order;
        });
        QVector<AnimationAction> actions;
        for (const auto& candidate : candidates) {
            actions.append(candidate.action);
        }
        return actions;
    }
}
