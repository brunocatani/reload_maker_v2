// True Scopes profiles: per optic model, the rear lens's frame on a uniquely
// named rigid shape of the installed optic, with its radius, aspect, eye
// relief and camera distance in that shape's model units.
#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "commands/CommandSupport.h"
#include "gl/glshape.h"
#include "glview.h"
#include "model/nifmodel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

namespace reloadmaker
{
    using commands::require;

    // The installed parts with their own model, except the weapon root; the
    // last one named like a sight or scope is preferred. The optic shown
    // before stays selected when it is still offered.
    void ReloadMakerController::refreshScopeTargets()
    {
        if (!_scopePart) {
            return;
        }
        const auto previous = _selectedScopeTarget >= 0 && _selectedScopeTarget < _scopeTargets.size() ? _scopeTargets[_selectedScopeTarget].label : QString();
        _scopeTargets.clear();
        int preferred = -1;
        for (int i = 1; i < _handPoseAnchors.size(); ++i) {
            const auto& anchor = _handPoseAnchors[i];
            if (anchor.description.anchorKind != "installed-part" || anchor.node == _weaponRoot || anchor.description.ownerModel.isEmpty()) {
                continue;
            }
            const auto name = _handPoseAnchors[i].label;
            _scopeTargets.append({ i, name });
            if (name.contains("sight", Qt::CaseInsensitive) || name.contains("scope", Qt::CaseInsensitive)) {
                preferred = int(_scopeTargets.size()) - 1;
            }
        }
        int old = -1;
        for (int i = 0; i < _scopeTargets.size(); ++i) {
            if (_scopeTargets[i].label == previous) {
                old = i;
                break;
            }
        }
        _selectedScopeTarget = old >= 0 ? old : preferred;
        showScopeTargets();
        refreshScopeControls();
        updateScopeViewport();
    }

    // Selecting an optic ends a lens pick and shows its profile.
    void ReloadMakerController::selectScopeTarget(int index)
    {
        if (index == _selectedScopeTarget) {
            return;
        }
        _selectedScopeTarget = index;
        showSelectedScopeTarget();
        _scopeViewport->setPicking(false);
        refreshScopeControls();
        updateScopeViewport();
        noteDocumentChange("selected optic");
    }

    // The selected optic's part anchor; 0 (the whole weapon, never an optic)
    // when none is selected.
    int ReloadMakerController::scopeTargetAnchor() const
    {
        return _selectedScopeTarget >= 0 && _selectedScopeTarget < _scopeTargets.size() ? _scopeTargets[_selectedScopeTarget].anchor : 0;
    }

    // The selected optic's name, as the optic list shows it.
    QString ReloadMakerController::scopeTargetLabel() const
    {
        return _selectedScopeTarget >= 0 && _selectedScopeTarget < _scopeTargets.size() ? _scopeTargets[_selectedScopeTarget].label : QString();
    }

    QString ReloadMakerController::currentScopeKey() const
    {
        const int index = scopeTargetAnchor();
        return index > 0 && index < _handPoseAnchors.size() ? scopeModelKey(_handPoseAnchors[index].description.ownerModel) : QString();
    }

    // The visible rigid shapes of the selected optic, while the scene is
    // assembled.
    QVector<int> ReloadMakerController::scopeShapeBlocks() const
    {
        QVector<int> blocks;
        const int index = scopeTargetAnchor();
        const auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        if (!model || !scene || index <= 0 || index >= _handPoseAnchors.size() || _loadThread || _rebuildTimer->isActive()) {
            return blocks;
        }
        const auto root = _handPoseAnchors[index].node;
        for (auto* shape : scene->shapes) {
            if (!shape || shape->isHidden() || !shape->reloadMakerSkinBones().isEmpty()) {
                continue;
            }
            auto node = model->getBlockIndex(shape->id());
            for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                if (node == root) {
                    blocks.append(shape->id());
                    break;
                }
                node = model->getBlockIndex(model->getParent(node));
            }
        }
        return blocks;
    }

    // The world frame of the profile's shape, which must be the only shape of
    // that name in the scene and belong to the selected optic.
    bool ReloadMakerController::scopeFrame(Transformf& world) const
    {
        const auto found = _scopeProfiles.constFind(currentScopeKey());
        if (found == _scopeProfiles.cend()) {
            return false;
        }
        const auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        if (!model || !scene) {
            return false;
        }
        const auto allowed = scopeShapeBlocks();
        QModelIndex selected;
        int count = 0;
        for (auto* shape : scene->shapes) {
            if (!shape) {
                continue;
            }
            const auto node = model->getBlockIndex(shape->id());
            if (model->get<QString>(node, "Name") == found->shape) {
                ++count;
                if (allowed.contains(shape->id())) {
                    selected = node;
                }
            }
        }
        QString error;
        return count == 1 && selected.isValid() && sceneNodeWorld(selected, world, error);
    }

    // Places the lens at the picked point on the optic. A new profile faces
    // along the optic part; an existing one keeps its rotation and rescales
    // its lengths to the picked shape's model units.
    bool ReloadMakerController::pickScopeLens(QPointF point)
    {
        const auto fail = [this](const QString& message) {
            setStatus(message, true);
            return false;
        };
        auto* view = _window->getGLView();
        auto* model = activeModel();
        const auto blocks = scopeShapeBlocks();
        int block = -1;
        Vector3 hit;
        if (!view->reloadMakerSurfaceAt(point, blocks, block, hit)) {
            return fail("Pick the rear lens on the selected optic's visible rigid mesh.");
        }
        const auto node = model->getBlockIndex(block);
        const auto shape = model->get<QString>(node, "Name");
        int duplicates = 0;
        for (auto* candidate : view->getScene()->shapes) {
            if (candidate && model->get<QString>(model->getBlockIndex(candidate->id()), "Name") == shape) {
                ++duplicates;
            }
        }
        if (duplicates != 1) {
            return fail("This shape name is ambiguous in the assembled weapon. Choose a uniquely named lens or housing shape.");
        }
        Transformf world;
        Transformf inverse;
        Transformf part;
        QString error;
        const int index = scopeTargetAnchor();
        if (!sceneNodeWorld(node, world, error) || !invertUniformTransform(world, inverse) || !sceneNodeWorld(_handPoseAnchors[index].node, part, error)) {
            return fail("The optic's frame is invalid.");
        }
        const auto& source = _handPoseAnchors[index].description;
        auto p = _scopeProfiles.value(currentScopeKey());
        const bool existing = !p.model.isEmpty();
        Transformf oldWorld;
        const bool haveOld = existing && scopeFrame(oldWorld);
        if (haveOld) {
            const float ratio = oldWorld.scale.x / world.scale.x;
            p.radius *= ratio;
            p.eyeRelief *= ratio;
            p.cameraForward *= ratio;
        }
        Transformf target;
        target.translation = { hit[0], hit[1], hit[2] };
        target.rotation = haveOld ? composed(oldWorld, p.lens).rotation : part.rotation;
        p.lens = composed(inverse, target);
        p.lens.scale = { 1, 1, 1 };
        p.model = currentScopeKey();
        p.shape = shape;
        if (!existing) {
            p.name = scopeTargetLabel();
        }
        p.weapon = poseRecordKey(currentWeapon()->formId);
        p.attachment = source.ownerRecord;
        ScopeProfile checked;
        if (!readScopeProfile(scopeProfileJson(p), checked, error)) {
            return fail(error);
        }
        if (!_scopeProfiles.contains(p.model) && _scopeProfiles.size() >= 128) {
            return fail("A project supports at most 128 optic profiles.");
        }
        _scopeProfiles[p.model] = p;
        _scopeViewport->setPicking(false);
        ++_revision;
        refreshScopeControls();
        updateScopeViewport();
        return true;
    }

    // The viewport gizmo moved the lens.
    void ReloadMakerController::editScopeWorld(const Transformf& target)
    {
        Transformf frame;
        Transformf inverse;
        if (!scopeFrame(frame) || !invertUniformTransform(frame, inverse)) {
            return;
        }
        auto p = _scopeProfiles.value(currentScopeKey());
        p.lens = composed(inverse, target);
        p.lens.scale = { 1, 1, 1 };
        ScopeProfile checked;
        QString error;
        if (!readScopeProfile(scopeProfileJson(p), checked, error)) {
            setStatus(error, true);
            return;
        }
        _scopeProfiles[p.model] = p;
        ++_revision;
        refreshScopeControls();
        updateScopeViewport();
    }

    QJsonObject ReloadMakerController::scopeState() const
    {
        QJsonArray targets;
        for (int i = 0; i < _scopeTargets.size(); ++i) {
            const int anchor = _scopeTargets[i].anchor;
            if (anchor <= 0 || anchor >= _handPoseAnchors.size()) {
                continue;
            }
            targets.append(QJsonObject{ { "index", i }, { "name", _scopeTargets[i].label }, { "model", scopeModelKey(_handPoseAnchors[anchor].description.ownerModel) } });
        }
        QJsonArray shapes;
        const auto* model = activeModel();
        for (int b : scopeShapeBlocks()) {
            shapes.append(model->get<QString>(model->getBlockIndex(b), "Name"));
        }
        Transformf frame;
        const auto key = currentScopeKey();
        const bool configured = _scopeProfiles.contains(key);
        return { { "targets", targets },
                 { "selected", _selectedScopeTarget },
                 { "shapes", shapes },
                 { "configured", configured },
                 { "resolved", scopeFrame(frame) },
                 { "profile", configured ? scopeProfileJson(_scopeProfiles.value(key)) : QJsonObject() } };
    }

    // Writes the profile into an MO2-ready TrueScopesVR folder under
    // exports; nothing is deployed.
    QJsonObject ReloadMakerController::exportScopeProfile()
    {
        Transformf frame;
        require(scopeFrame(frame), "The optic's unique rigid shape must be present before exporting.");
        const auto p = _scopeProfiles.value(currentScopeKey());
        ScopeProfile checked;
        QString error;
        require(readScopeProfile(scopeProfileJson(p), checked, error), error);
        auto name = QFileInfo(p.model).completeBaseName();
        name.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
        const auto hash = QString::fromLatin1(QCryptographicHash::hash(p.model.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
        const auto root = studioDataPath("exports/TrueScopesProfiles");
        const auto path = root + "/F4SE/Plugins/TrueScopesVR/Scopes/" + name + "-" + hash + ".json";
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create scope export folder.");
        QSaveFile file(path);
        const auto bytes = QJsonDocument(scopeProfileJson(p)).toJson();
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot save scope export: " + file.errorString());
        setStatus("Exported · " + path);
        return { { "path", path }, { "modFolder", root }, { "kind", "scope" }, { "deployed", false } };
    }

    // open, export, import, select, pick, edit, save, remove.
    QJsonObject ReloadMakerController::configureScope(const QJsonObject& params)
    {
        require(!_loadThread && !_rebuildTimer->isActive(), "Wait for weapon assembly.");
        _lastError.clear();
        const auto op = params["operation"].toString();
        if (op == "open") {
            setWorkspace(ScopesWorkspace);
            return scopeState();
        }
        if (op == "export") {
            return exportScopeProfile();
        }
        if (op == "import") {
            importScopeProfile(params["path"].toString());
        } else if (op == "select") {
            const auto i = params["index"].toInt(-1);
            require(i >= 0 && i < _scopeTargets.size(), "Select an installed optic.");
            selectScopeTarget(i);
        } else if (op == "pick") {
            require(_workspace == ScopesWorkspace, "Open Scopes before picking.");
            require(pickScopeLens({ params["x"].toDouble(-1), params["y"].toDouble(-1) }), _lastError);
        } else if (op == "edit") {
            ScopeProfile p;
            QString error;
            require(readScopeProfile(params["profile"].toObject(), p, error), error);
            const auto key = currentScopeKey();
            require(_scopeProfiles.contains(key) && p.model == key && p.shape == _scopeProfiles.value(key).shape,
                    "Pick a lens before editing; model and shape cannot be changed by an edit.");
            _scopeProfiles[key] = p;
        } else if (op == "save") {
            Transformf frame;
            require(scopeFrame(frame), "Pick a valid lens first.");
            require(saveSession(), _lastError);
        } else if (op == "remove") {
            _scopeProfiles.remove(currentScopeKey());
        } else {
            require(false, "Unknown scope-profile operation.");
        }
        ++_revision;
        refreshScopeControls();
        updateScopeViewport();
        return scopeState();
    }

    // An imported profile may only repeat the one already owning its optic.
    void ReloadMakerController::importScopeProfile(const QString& path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) && file.size() <= 32768, "Cannot read scope profile (maximum 32 KiB).");
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        ScopeProfile p;
        QString error;
        require(parse.error == QJsonParseError::NoError && doc.isObject() && readScopeProfile(doc.object(), p, error), "Invalid scope profile: " + error);
        require(!_scopeProfiles.contains(p.model) || scopeProfileJson(_scopeProfiles.value(p.model)) == scopeProfileJson(p),
                "A different profile already owns this optic. Remove it explicitly before importing a replacement.");
        require(_scopeProfiles.contains(p.model) || _scopeProfiles.size() < 128, "A project supports at most 128 optic profiles.");
        _scopeProfiles[p.model] = p;
    }
}
